// The HarfBuzz shaper, reading the font's bytes itself.
//
// A face is a HarfBuzz face over a face of a font file, which PrTextShaper keeps. Each font keeps a HarfBuzz font over
// its face, scaled to its units per em, at its design point.

#include "pr/text_harfbuzz.h"

#include "pr/alloc.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "text/backends/shared/src/text_shared_font_cache.h"

#include "hb.h"

namespace pr_impl {

namespace {

// The code points of context on each side of a run that HarfBuzz reads (its HB_BUFFER_CONTEXT_LENGTH).
constexpr uint32_t kContextLength = 5;

struct HbFace {
    hb_face_t* hb;
};

struct HbFont {
    hb_font_t* hb;
    float unitsPerEm;
};

bool createFace(PrTextShaper*, const PrFont* font, void* state) {
    PrByteView bytes = prFontData(font);
    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data), unsigned(bytes.size),
        HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    static_cast<HbFace*>(state)->hb = hb_face_create(blob, prFontIndex(font));
    hb_blob_destroy(blob);
    return true;
}

void destroyFace(PrTextShaper*, void* state) {
    hb_face_destroy(static_cast<HbFace*>(state)->hb);
}

void createFont(PrTextShaper*, void* face, const PrFont* font, void* state) {
    HbFont* out = static_cast<HbFont*>(state);
    out->unitsPerEm = float(prFontMetrics(font).unitsPerEm);
    out->hb = hb_font_create(static_cast<HbFace*>(face)->hb);
    int scale = int(out->unitsPerEm);
    hb_font_set_scale(out->hb, scale, scale);
    RawArray<hb_variation_t> variations;
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i)
        variations.push(hb_variation_t{prFontAxis(font, i).tag, prFontAxisValue(font, i)});
    if (variations.count != 0)
        hb_font_set_variations(out->hb, variations.data, variations.count);
    variations.reset();
}

void destroyFont(PrTextShaper*, void* state) {
    hb_font_destroy(static_cast<HbFont*>(state)->hb);
}

struct HbBackend : PrTextShaper {
    TextSharedFontCache fonts;
    hb_buffer_t* buffer;
    RawArray<hb_feature_t> features;
    RawArray<char> text;                   // the run, with its context on either side
};

bool startsCodePoint(char byte) {
    return (uint8_t(byte) & 0xc0) != 0x80;
}

// Returns the context next to the run that HarfBuzz reads. Copying only that context keeps shaping a text in pieces
// linear.
PrStringView contextBefore(PrStringView before) {
    size_t start = before.size;
    for (uint32_t n = 0; n < kContextLength && start > 0; ++n) {
        --start;
        while (start > 0 && !startsCodePoint(before.data[start]))
            --start;
    }
    return PrStringView{before.data + start, before.size - start};
}

PrStringView contextAfter(PrStringView after) {
    size_t end = 0;
    for (uint32_t n = 0; n < kContextLength && end < after.size; ++n) {
        ++end;
        while (end < after.size && !startsCodePoint(after.data[end]))
            ++end;
    }
    return PrStringView{after.data, end};
}

void destroyBackend(PrTextShaper* shaper) {
    HbBackend* h = static_cast<HbBackend*>(shaper);
    textSharedFontCacheReset(shaper, &h->fonts);
    h->features.reset();
    h->text.reset();
    hb_buffer_destroy(h->buffer);
    prMemoryFree(h);
}

PrStatus shape(PrTextShaper* shaper, void*, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    HbBackend* h = static_cast<HbBackend*>(shaper);
    HbFont* font = static_cast<HbFont*>(fontState);
    hb_buffer_t* buffer = h->buffer;
    hb_buffer_reset(buffer);
    // The run with its surrounding context. HarfBuzz shapes the bytes of the run and reads the rest as context.
    PrStringView before = contextBefore(run->before);
    PrStringView after = contextAfter(run->after);
    h->text.clear();
    if (before.size != 0)
        h->text.append(before.data, uint32_t(before.size));
    h->text.append(run->text.data, uint32_t(run->text.size));
    if (after.size != 0)
        h->text.append(after.data, uint32_t(after.size));
    uint32_t start = uint32_t(before.size);
    hb_buffer_add_utf8(buffer, h->text.data, int(h->text.count), start, int(run->text.size));
    hb_direction_t direction = HB_DIRECTION_LTR;
    if (run->axis == PrTextAxis_Vertical)
        direction = HB_DIRECTION_TTB;
    else if (run->direction == PrTextDirection_Rtl)
        direction = HB_DIRECTION_RTL;
    hb_buffer_set_direction(buffer, direction);
    if (run->script != PrScript_Zzzz) {
        PrStringView code = prScriptCode(run->script);
        hb_buffer_set_script(buffer, hb_script_from_string(code.data, int(code.size)));
    }
    // HarfBuzz maps a BCP 47 tag to OpenType language systems itself.
    if (run->locale != 0) {
        char tag[kPrLocaleMaxTag];
        hb_buffer_set_language(buffer, hb_language_from_string(tag, int(prLocaleTag(run->locale, tag))));
    }
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    h->features.clear();
    for (uint32_t i = 0; i < run->featureCount; ++i) {
        const PrTextFeature& f = run->features[i];
        unsigned end = f.end == HB_FEATURE_GLOBAL_END ? f.end : f.end + start;
        h->features.push(hb_feature_t{f.tag, f.value, f.start + start, end});
    }
    hb_shape(font->hb, buffer, h->features.data, h->features.count);
    unsigned count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    PrTextGlyph* glyphs = prTextGlyphBufferAppend(out, count);
    float scale = 1 / font->unitsPerEm;
    for (unsigned i = 0; i < count; ++i) {
        const hb_glyph_position_t& p = positions[i];
        glyphs[i].id = uint16_t(infos[i].codepoint);
        glyphs[i].cluster = infos[i].cluster - start;
        glyphs[i].offset = PrVec2{float(p.x_offset) * scale, -float(p.y_offset) * scale};
        glyphs[i].advance = PrVec2{float(p.x_advance) * scale, -float(p.y_advance) * scale};
    }
    return PrStatus_Ok;
}

constexpr TextSharedFontStates kStates = {
    .faceSize = sizeof(HbFace),
    .fontSize = sizeof(HbFont),
    .createFace = createFace,
    .destroyFace = destroyFace,
    .createFont = createFont,
    .destroyFont = destroyFont,
    .shape = shape,
};

PrStatus shapeRun(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out) {
    return textSharedFontCacheShape(shaper, &static_cast<HbBackend*>(shaper)->fonts, run, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextShaper* prTextHarfBuzzCreate(void) {
    HbBackend* h = allocZeroed<HbBackend>();
    h->destroyFn = destroyBackend;
    h->shapeFn = shapeRun;
    h->fonts.states = &kStates;
    h->buffer = hb_buffer_create();
    return h;
}
