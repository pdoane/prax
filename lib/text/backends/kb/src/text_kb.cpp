// The kb shaper: kb_text_shape, reading the font's bytes itself.
//
// A face is a face of a font file as kb parses it, which PrTextShaper keeps. Each font keeps its design point, and a
// shape configuration for each script and language that it shaped, which holds the address of its face.

#include "pr/text_kb.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/raw_array.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"
#include "pr/vec.h"

#include "text/backends/shared/src/text_shared_font_cache.h"

#define KB_TEXT_SHAPE_IMPLEMENTATION
#include "kb_text_shape.h"

namespace pr_impl {

namespace {

struct KbConfig {
    kbts_script script;
    kbts_language language;
    kbts_shape_config* config;
    kbts_shape_scratchpad* scratchpad;
};

void kbAllocate(void*, kbts_allocator_op* op) {
    if (op->Kind == KBTS_ALLOCATOR_OP_KIND_ALLOCATE)
        op->Allocate.Pointer = prMemoryAlloc(op->Allocate.Size != 0 ? op->Allocate.Size : 1);
    else if (op->Kind == KBTS_ALLOCATOR_OP_KIND_FREE)
        prMemoryFree(op->Free.Pointer);
}

// Returns an OpenType tag as kb spells it, with its first character in the low byte.
uint32_t kbTag(uint32_t tag) {
    return (tag >> 24) | ((tag >> 8) & 0xff00u) | ((tag << 8) & 0xff0000u) | (tag << 24);
}

struct KbFace {
    kbts_font kb;
};

struct KbFont {
    float unitsPerEm;
    RawArray<float> variation;             // the design point, by axis in the order of the font; empty unless variable

    // The design point, normalized; empty at the default, at which no variation applies.
    RawArray<kbts_s16> normalized;

    RawArray<KbConfig> configs;
};

bool createFace(PrTextShaper*, const PrFont* font, void* state) {
    KbFace* face = static_cast<KbFace*>(state);
    PrByteView bytes = prFontData(font);
    face->kb = kbts_FontFromMemory(const_cast<uint8_t*>(bytes.data), int(bytes.size), int(prFontIndex(font)),
        kbAllocate, nullptr);
    if (kbts_FontIsValid(&face->kb))
        return true;
    kbts_FreeFont(&face->kb);
    return false;
}

void destroyFace(PrTextShaper*, void* state) {
    kbts_FreeFont(&static_cast<KbFace*>(state)->kb);
}

void createFont(PrTextShaper*, void* faceState, const PrFont* font, void* state) {
    KbFace* face = static_cast<KbFace*>(faceState);
    KbFont* out = static_cast<KbFont*>(state);
    out->unitsPerEm = float(prFontMetrics(font).unitsPerEm);
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i)
        out->variation.push(prFontAxisValue(font, i));
    if (out->variation.count == 0)
        return;
    out->normalized.resize(out->variation.count);
    kbts_NormalizeVariationVector(&face->kb, out->variation.data, out->normalized.data);
    bool atDefault = true;
    for (uint32_t i = 0; i < out->normalized.count; ++i)
        atDefault = atDefault && out->normalized.data[i] == 0;
    if (atDefault)
        out->normalized.reset();
}

void destroyFont(PrTextShaper*, void* state) {
    KbFont* font = static_cast<KbFont*>(state);
    for (uint32_t i = 0; i < font->configs.count; ++i) {
        kbts_DestroyShapeScratchpad(font->configs.data[i].scratchpad);
        kbts_DestroyShapeConfig(font->configs.data[i].config);
    }
    font->configs.reset();
    font->variation.reset();
    font->normalized.reset();
}

// A set of features shared by characters of the run, and its glyph configuration.
struct FeatureSet {
    uint64_t mask;                      // the features of the run in the set, as bits by index
    kbts_glyph_config* config;
};

struct KbBackend : PrTextShaper {
    TextSharedFontCache fonts;
    kbts_glyph_storage storage;
    RawArray<FeatureSet> sets;             // the feature sets of the run, emptied after the run
    RawArray<kbts_feature_override> overrides;
    RawArray<PrTextFeature> unitFeatures;  // the features of a vertical run, for each character shaped across
};

KbConfig* PR_NULLABLE configFor(KbFace* face, KbFont* font, kbts_script script, kbts_language language) {
    for (uint32_t i = 0; i < font->configs.count; ++i) {
        KbConfig& c = font->configs.data[i];
        if (c.script == script && c.language == language)
            return &c;
    }
    kbts_shape_config* config = kbts_CreateShapeConfig2(&face->kb, script, language, font->variation.data,
        kbAllocate, nullptr, nullptr);
    if (config == nullptr)
        return nullptr;
    KbConfig c{script, language, config, kbts_CreateShapeScratchpad(config, kbAllocate, nullptr)};
    if (c.scratchpad == nullptr) {
        kbts_DestroyShapeConfig(config);
        return nullptr;
    }
    font->configs.push(c);
    return &font->configs.last();
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripts and languages

// Returns the kb script for a script: the first of its OpenType tags that kb knows, or the default script if kb knows
// none. That is the case for Common, Inherited and Unknown, which have no tag.
kbts_script scriptOf(PrScript script) {
    uint32_t tags[kPrScriptMaxOpenTypeTags];
    uint32_t count = prScriptOpenTypeTags(script, tags);
    for (uint32_t i = 0; i < count; ++i) {
        kbts_script found = kbts_ScriptTagToScript(kbTag(tags[i]));
        if (found != KBTS_SCRIPT_DONT_KNOW)
            return found;
    }
    return kbts_ScriptTagToScript(kbTag(PR_FONT_TAG('D', 'F', 'L', 'T')));
}

// Returns the most specific OpenType language system of a locale, or DONT_KNOW, the default of the font, if the locale
// has none.
kbts_language languageOf(PrLocale locale) {
    uint32_t tag = 0;
    return prLocaleOpenTypeLanguages(locale, &tag, 1) != 0 ? kbts_language(kbTag(tag)) : KBTS_LANGUAGE_DONT_KNOW;
}

// ---------------------------------------------------------------------------------------------------------------------
// Features

// Returns the glyph configuration of the features over byte `at`, made once for each feature set that the run uses.
kbts_glyph_config* PR_NULLABLE configAt(KbBackend* kb, const PrTextRun* run, KbConfig* config, uint32_t at) {
    uint64_t mask = 0;
    uint32_t count = run->featureCount < 64 ? run->featureCount : 64;
    for (uint32_t i = 0; i < count; ++i) {
        if (run->features[i].start <= at && at < run->features[i].end)
            mask |= uint64_t(1) << i;
    }
    if (mask == 0)
        return nullptr;
    for (uint32_t i = 0; i < kb->sets.count; ++i) {
        if (kb->sets.data[i].mask == mask)
            return kb->sets.data[i].config;
    }
    kb->overrides.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if ((mask & (uint64_t(1) << i)) != 0)
            kb->overrides.push(kbts_feature_override{
                kbts_feature_tag(kbTag(run->features[i].tag)),
                int(run->features[i].value),
            });
    }
    kbts_glyph_config* glyphConfig = kbts_CreateGlyphConfig(
        config->config, kb->overrides.data, int(kb->overrides.count), kbAllocate, nullptr);
    kb->sets.push(FeatureSet{mask, glyphConfig});
    return glyphConfig;
}

// ---------------------------------------------------------------------------------------------------------------------
// The backend

void destroyBackend(PrTextShaper* shaper) {
    KbBackend* kb = static_cast<KbBackend*>(shaper);
    textSharedFontCacheReset(shaper, &kb->fonts);
    kb->sets.reset();
    kb->overrides.reset();
    kb->unitFeatures.reset();
    kbts_FreeAllGlyphs(&kb->storage);
    prMemoryFree(kb);
}

// Appends the glyphs of a run across, left to right or right to left.
PrStatus shapeAcross(KbBackend* kb, void* faceState, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    KbFace* face = static_cast<KbFace*>(faceState);
    KbFont* font = static_cast<KbFont*>(fontState);
    KbConfig* config = configFor(face, font, scriptOf(run->script), languageOf(run->locale));
    if (config == nullptr)
        return PrStatus_NotSupported;

    kbts_ClearActiveGlyphs(&kb->storage);
    size_t at = 0;
    while (at < run->text.size) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run->text, at, &codePoint);
        kbts_PushGlyph(&kb->storage, &face->kb, int(codePoint), configAt(kb, run, config, uint32_t(at)), int(at));
        at += length;
    }
    kbts_glyph_iterator it;
    kbts_direction direction = run->direction == PrTextDirection_Rtl ? KBTS_DIRECTION_RTL : KBTS_DIRECTION_LTR;
    kbts_shape_error error = kbts_ShapeDirect2(config->scratchpad, &kb->storage, direction, font->normalized.data, &it);
    PrStatus status = PrStatus_Ok;
    if (error == KBTS_SHAPE_ERROR_NONE) {
        float scale = 1 / font->unitsPerEm;
        kbts_glyph* glyph = nullptr;
        while (kbts_GlyphIteratorNext(&it, &glyph)) {
            PrTextGlyph* g = prTextGlyphBufferAppend(out, 1);
            g->id = glyph->Id;
            g->cluster = uint32_t(glyph->UserIdOrCodepointIndex);
            g->offset = PrVec2{float(glyph->OffsetX) * scale, -float(glyph->OffsetY) * scale};
            g->advance = PrVec2{float(glyph->AdvanceX) * scale, -float(glyph->AdvanceY) * scale};
        }
    } else {
        status = PrStatus_NotSupported;
    }
    for (uint32_t i = 0; i < kb->sets.count; ++i)
        kbts_DestroyGlyphConfig(kb->sets.data[i].config);
    kb->sets.clear();
    return status;
}

// Returns true if the code point at byte `at` of the text is default ignorable.
bool ignorableAt(PrStringView text, uint32_t at) {
    uint32_t codePoint = 0;
    if (at < text.size)
        prUtf8Decode(text, at, &codePoint);
    return (prUnicodeFlags(codePoint) & PrUnicodeFlag_DefaultIgnorable) != 0;
}

// Appends the glyphs of a run down, which kb cannot shape. Each grapheme cluster is shaped across on its own, with the
// vertical forms of the font (vert) and no context, so that nothing joins or ligates across clusters and the marks of a
// cluster stay with its base. Each cluster is placed by the vertical origin of its base glyph, as a vertical shaping
// places it, and advances down by the vertical advance of that glyph after its last glyph. Vertical positioning places
// glyphs down a line, which a shaping across does not have, so it is left out instead of being half applied.
PrStatus shapeDown(KbBackend* kb, void* faceState, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    constexpr uint32_t kWholeRun = 0xffffffffu;
    RawArray<PrTextFeature>& features = kb->unitFeatures;
    features.clear();
    for (uint32_t i = 0; i < run->featureCount; ++i) {
        uint32_t tag = run->features[i].tag;
        if (tag != PR_FONT_TAG('v', 'k', 'r', 'n') && tag != PR_FONT_TAG('v', 'p', 'a', 'l') &&
                tag != PR_FONT_TAG('v', 'h', 'a', 'l') && tag != PR_FONT_TAG('v', 'a', 'l', 't'))
            features.push(run->features[i]);
    }
    features.push(PrTextFeature{PR_FONT_TAG('v', 'e', 'r', 't'), 1, 0, kWholeRun});
    // The vertical metrics of the font include its size-adjust, which the glyphs, in the ems of the design, do not.
    float sizeAdjust = prFontSizeAdjust(run->font);
    for (size_t at = 0; at < run->text.size;) {
        size_t next = prUnicodeGraphemeNext(run->text, at);
        PrTextRun across = *run;
        across.text = PrStringView{run->text.data + at, next - at};
        across.before = PrStringView{};
        across.after = PrStringView{};
        across.direction = PrTextDirection_Ltr;
        across.features = features.data;
        across.featureCount = features.count;
        uint32_t first = prTextGlyphBufferGlyphCount(out);
        PrStatus status = shapeAcross(kb, faceState, fontState, &across, out);
        if (status != PrStatus_Ok)
            return status;
        uint32_t count = prTextGlyphBufferGlyphCount(out) - first;
        if (count != 0) {
            // The glyphs of the cluster, which were appended last. Appending none gives the end of the buffer.
            PrTextGlyph* glyphs = prTextGlyphBufferAppend(out, 0) - count;
            uint16_t base = glyphs[0].id;
            float advance = prFontGlyphVerticalAdvance(run->font, base) / sizeAdjust;
            PrVec2 origin = prFontGlyphVerticalOrigin(run->font, base);
            PrVec2 pen{-origin.x / sizeAdjust, -origin.y / sizeAdjust};
            // The glyphs of the cluster share its place on the line, and the cluster's advance follows its last drawn
            // glyph. A default ignorable code point that kb sets as nothing, such as a variation selector, has no
            // advance, and shaping drops its glyph (prTextShaperShape).
            uint32_t last = count - 1;
            while (last > 0 && glyphs[last].advance.x == 0 && ignorableAt(across.text, glyphs[last].cluster))
                --last;
            for (uint32_t i = 0; i < count; ++i) {
                PrTextGlyph& glyph = glyphs[i];
                glyph.cluster += uint32_t(at);
                glyph.offset = pen + glyph.offset;
                pen.x += glyph.advance.x;
                glyph.advance = PrVec2{0, i == last ? advance : 0};
            }
        }
        at = next;
    }
    return PrStatus_Ok;
}

PrStatus shape(PrTextShaper* shaper, void* faceState, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    KbBackend* kb = static_cast<KbBackend*>(shaper);
    if (run->axis == PrTextAxis_Vertical)
        return shapeDown(kb, faceState, fontState, run, out);
    return shapeAcross(kb, faceState, fontState, run, out);
}

constexpr TextSharedFontStates kStates = {
    .faceSize = sizeof(KbFace),
    .fontSize = sizeof(KbFont),
    .createFace = createFace,
    .destroyFace = destroyFace,
    .createFont = createFont,
    .destroyFont = destroyFont,
    .shape = shape,
};

PrStatus shapeRun(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out) {
    return textSharedFontCacheShape(shaper, &static_cast<KbBackend*>(shaper)->fonts, run, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextShaper* prTextKbCreate(void) {
    KbBackend* kb = allocZeroed<KbBackend>();
    kb->destroyFn = destroyBackend;
    kb->shapeFn = shapeRun;
    kb->fonts.states = &kStates;
    kbts_InitializeGlyphStorage(&kb->storage, kbAllocate, nullptr);
    return kb;
}
