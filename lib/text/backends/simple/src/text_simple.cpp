// The simple shaper: one glyph per character, kerned by GPOS pair adjustments or the kern table.
//
// A face holds where the font's GPOS, GDEF and kern tables are. Each font keeps its design point, which varies GPOS
// values through GDEF's variation store, and the kerning lookups of each script and language that it shaped.

#include "pr/text_simple.h"

#include "pr/alloc.h"
#include "pr/locale.h"
#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/utf8.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

constexpr uint32_t kGpos = PR_FONT_TAG('G', 'P', 'O', 'S');
constexpr uint32_t kGdef = PR_FONT_TAG('G', 'D', 'E', 'F');
constexpr uint32_t kKern = PR_FONT_TAG('k', 'e', 'r', 'n');
constexpr uint32_t kTtcf = PR_FONT_TAG('t', 't', 'c', 'f');
constexpr uint32_t kDflt = PR_FONT_TAG('D', 'F', 'L', 'T');
constexpr uint32_t kLatn = PR_FONT_TAG('l', 'a', 't', 'n');

// The lookup types of GPOS that kerning reads.
constexpr uint16_t kPairPos = 2;
constexpr uint16_t kExtension = 9;

// The fields of a GPOS value record, by the bits of its format.
enum {
    ValueFlag_XPlacement = 0x01,
    ValueFlag_YPlacement = 0x02,
    ValueFlag_XAdvance = 0x04,
    ValueFlag_YAdvance = 0x08,
    ValueFlag_XPlacementDevice = 0x10,
    ValueFlag_YPlacementDevice = 0x20,
    ValueFlag_XAdvanceDevice = 0x40,
    ValueFlag_YAdvanceDevice = 0x80,
};
typedef uint16_t ValueFlags;

// Bytes of the font file, read big-endian. A read past the end returns zero.
struct Bytes {
    const uint8_t* PR_NULLABLE data;
    uint32_t size;

    uint8_t uint8At(uint32_t at) const {
        return at < size ? data[at] : 0;
    }
    uint16_t uint16At(uint32_t at) const {
        return uint16_t(uint8At(at) << 8 | uint8At(at + 1));
    }
    int16_t int16At(uint32_t at) const {
        return int16_t(uint16At(at));
    }
    uint32_t uint32At(uint32_t at) const {
        return uint32_t(uint16At(at)) << 16 | uint16At(at + 2);
    }
    Bytes from(uint32_t at) const {
        return at < size ? Bytes{data + at, size - at} : Bytes{nullptr, 0};
    }
};

// Returns the table `tag` of the face `index` of the file, or no bytes.
Bytes tableOf(Bytes file, uint32_t index, uint32_t tag) {
    uint32_t directory = 0;
    if (file.uint32At(0) == kTtcf) {
        if (index >= file.uint32At(8))
            return Bytes{nullptr, 0};
        directory = file.uint32At(12 + index * 4);
    }
    uint32_t count = file.uint16At(directory + 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record = directory + 12 + i * 16;
        if (file.uint32At(record) != tag)
            continue;
        uint32_t offset = file.uint32At(record + 8);
        uint32_t length = file.uint32At(record + 12);
        if (offset > file.size || length > file.size - offset)
            return Bytes{nullptr, 0};
        return Bytes{file.data + offset, length};
    }
    return Bytes{nullptr, 0};
}

// Returns the index of the glyph in the coverage table, or -1 if it does not cover the glyph.
int32_t coverageIndex(Bytes coverage, uint16_t glyph) {
    uint16_t format = coverage.uint16At(0);
    uint32_t count = coverage.uint16At(2);
    if (format == 1) {
        uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return coverage.uint16At(4 + k * 2) < glyph; });
        return i < count && coverage.uint16At(4 + i * 2) == glyph ? int32_t(i) : -1;
    }
    if (format != 2)
        return -1;
    // Ranges are sorted by their last glyph.
    uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return coverage.uint16At(4 + k * 6 + 2) < glyph; });
    if (i == count || coverage.uint16At(4 + i * 6) > glyph)
        return -1;
    return int32_t(coverage.uint16At(4 + i * 6 + 4)) + int32_t(glyph - coverage.uint16At(4 + i * 6));
}

// Returns the class that the class definition gives the glyph, which is 0 if it names none.
uint16_t classOf(Bytes classes, uint16_t glyph) {
    uint16_t format = classes.uint16At(0);
    if (format == 1) {
        uint16_t start = classes.uint16At(2);
        uint16_t count = classes.uint16At(4);
        return glyph >= start && glyph - start < count ? classes.uint16At(6 + uint32_t(glyph - start) * 2) : 0;
    }
    if (format != 2)
        return 0;
    uint32_t count = classes.uint16At(2);
    uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return classes.uint16At(4 + k * 6 + 2) < glyph; });
    return i < count && classes.uint16At(4 + i * 6) <= glyph ? classes.uint16At(4 + i * 6 + 4) : 0;
}

// Returns the number of bytes of a value record of the format.
uint32_t valueSize(ValueFlags format) {
    uint32_t bits = 0;
    for (uint32_t b = 0; b < 8; ++b)
        bits += (format >> b) & 1;
    return bits * 2;
}

struct SimpleFace {
    Bytes gpos;
    Bytes gdefStore;                    // GDEF's ItemVariationStore, which varies GPOS values, or no bytes
    Bytes kern;                         // the kern table, read if GPOS has no kerning
};

// The kerning of one script and language: the PairPos subtables of its kern feature, by lookup.
struct KernConfig {
    uint32_t script;
    uint32_t language;
    RawArray<uint32_t> lookups;            // offsets of the lookups within GPOS, in lookup order
    bool fromKernTable;
};

struct SimpleFont {
    float unitsPerEm;
    RawArray<float> coords;                // the design point, normalized; empty at the default, at which nothing
                                           // varies
    RawArray<KernConfig> configs;
};

bool createFace(PrTextShaper*, const PrFont* font, void* state) {
    SimpleFace* face = static_cast<SimpleFace*>(state);
    PrByteView bytes = prFontData(font);
    Bytes file{bytes.data, uint32_t(bytes.size < 0xffffffffu ? bytes.size : 0xffffffffu)};
    uint32_t index = prFontIndex(font);
    face->gpos = tableOf(file, index, kGpos);
    Bytes gdef = tableOf(file, index, kGdef);
    // GDEF 1.3 gives the store of the deltas that GPOS's device tables name.
    if (gdef.uint16At(0) == 1 && gdef.uint16At(2) >= 3 && gdef.uint32At(14) != 0)
        face->gdefStore = gdef.from(gdef.uint32At(14));
    face->kern = tableOf(file, index, kKern);
    return true;
}

void destroyFace(PrTextShaper*, void*) {
}

void createFont(PrTextShaper*, void*, const PrFont* font, void* state) {
    SimpleFont* out = static_cast<SimpleFont*>(state);
    out->unitsPerEm = float(prFontMetrics(font).unitsPerEm);
    bool varied = false;
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i) {
        float coord = prFontAxisCoordinate(font, i);
        out->coords.push(coord);
        varied = varied || coord != 0;
    }
    if (!varied)
        out->coords.reset();
}

void destroyFont(PrTextShaper*, void* state) {
    SimpleFont* font = static_cast<SimpleFont*>(state);
    for (uint32_t i = 0; i < font->configs.count; ++i)
        font->configs.data[i].lookups.reset();
    font->configs.reset();
    font->coords.reset();
}

// Returns the scalar of a region of the store at the coordinates.
float regionScalar(Bytes regions, uint16_t region, const SimpleFont& font) {
    uint32_t axes = regions.uint16At(0);
    if (region >= regions.uint16At(2))
        return 0;
    float scalar = 1;
    for (uint32_t axis = 0; axis < axes && scalar != 0; ++axis) {
        uint32_t at = 4 + (uint32_t(region) * axes + axis) * 6;
        float start = float(regions.int16At(at)) / 16384;
        float peak = float(regions.int16At(at + 2)) / 16384;
        float end = float(regions.int16At(at + 4)) / 16384;
        float coord = axis < font.coords.count ? font.coords.data[axis] : 0;
        if (peak == 0 || start > peak || peak > end || (start < 0 && end > 0) || coord == peak)
            continue;
        if (coord <= start || coord >= end)
            return 0;
        scalar *= coord < peak ? (coord - start) / (peak - start) : (end - coord) / (end - peak);
    }
    return scalar;
}

// Returns the delta of the device table at `at` of `table`, in font units, if it is a VariationIndex table.
float deviceDelta(const SimpleFace& face, const SimpleFont& font, Bytes table, uint16_t at) {
    if (at == 0 || font.coords.count == 0 || face.gdefStore.size == 0)
        return 0;
    Bytes device = table.from(at);
    if (device.uint16At(4) != 0x8000)
        return 0;
    uint16_t outer = device.uint16At(0);
    uint16_t inner = device.uint16At(2);
    const Bytes& store = face.gdefStore;
    if (store.uint16At(0) != 1 || outer >= store.uint16At(6))
        return 0;
    Bytes regions = store.from(store.uint32At(2));
    Bytes data = store.from(store.uint32At(8 + uint32_t(outer) * 4));
    uint32_t items = data.uint16At(0);
    uint16_t words = data.uint16At(2);
    bool longWords = (words & 0x8000) != 0;
    uint32_t wordCount = words & 0x7fff;
    uint32_t regionCount = data.uint16At(4);
    if (inner >= items || wordCount > regionCount)
        return 0;
    uint32_t wide = longWords ? 4 : 2;
    uint32_t narrow = longWords ? 2 : 1;
    uint32_t at2 = 6 + regionCount * 2 + inner * (wordCount * wide + (regionCount - wordCount) * narrow);
    float delta = 0;
    for (uint32_t r = 0; r < regionCount; ++r) {
        int32_t value = 0;
        if (r < wordCount) {
            value = longWords ? int32_t(data.uint32At(at2)) : data.int16At(at2);
            at2 += wide;
        } else {
            value = longWords ? data.int16At(at2) : int8_t(data.uint8At(at2));
            at2 += narrow;
        }
        if (value != 0)
            delta += float(value) * regionScalar(regions, data.uint16At(6 + r * 2), font);
    }
    return delta;
}

// Applies the value record at `record`, in font units, to the glyph. Its device tables are offsets from `subtable`.
void applyValue(const SimpleFace& face, const SimpleFont& font, Bytes record, Bytes subtable, ValueFlags format,
        float unit, PrTextGlyph& glyph) {
    float values[8] = {};
    uint16_t devices[4] = {};
    uint32_t field = 0;
    for (uint32_t b = 0; b < 8; ++b) {
        if ((format & (1u << b)) == 0)
            continue;
        if (b < 4)
            values[b] = float(record.int16At(field));
        else
            devices[b - 4] = record.uint16At(field);
        field += 2;
    }
    for (uint32_t b = 0; b < 4; ++b)
        values[b] += deviceDelta(face, font, subtable, devices[b]);
    glyph.offset.x += values[0] * unit;
    glyph.offset.y -= values[1] * unit;
    glyph.advance.x += values[2] * unit;
    glyph.advance.y -= values[3] * unit;
}

// Applies a PairPos subtable to the pair at `first` and the glyph after it. Returns 0 if the subtable has no adjustment
// for the pair, and otherwise the number of glyphs that the adjustment consumes: two if it adjusts the second glyph,
// which then starts no pair of its own.
uint32_t applyPair(const SimpleFace& face, const SimpleFont& font, Bytes subtable, PrTextGlyph* glyphs, uint32_t first) {
    uint16_t format = subtable.uint16At(0);
    int32_t covered = coverageIndex(subtable.from(subtable.uint16At(2)), glyphs[first].id);
    if (covered < 0)
        return 0;
    ValueFlags format1 = subtable.uint16At(4);
    ValueFlags format2 = subtable.uint16At(6);
    uint32_t size1 = valueSize(format1);
    uint32_t size2 = valueSize(format2);
    float unit = 1 / font.unitsPerEm;
    uint16_t second = glyphs[first + 1].id;
    if (format == 1) {
        if (uint32_t(covered) >= subtable.uint16At(8))
            return 0;
        Bytes set = subtable.from(subtable.uint16At(10 + uint32_t(covered) * 2));
        uint32_t record = 2 + size1 + size2;
        uint32_t count = set.uint16At(0);
        uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return set.uint16At(2 + k * record) < second; });
        if (i == count || set.uint16At(2 + i * record) != second)
            return 0;
        uint32_t at = 2 + i * record;
        applyValue(face, font, set.from(at + 2), subtable, format1, unit, glyphs[first]);
        applyValue(face, font, set.from(at + 2 + size1), subtable, format2, unit, glyphs[first + 1]);
        return format2 != 0 ? 2 : 1;
    }
    if (format != 2)
        return 0;
    uint16_t class1 = classOf(subtable.from(subtable.uint16At(8)), glyphs[first].id);
    uint16_t class2 = classOf(subtable.from(subtable.uint16At(10)), second);
    uint32_t class1Count = subtable.uint16At(12);
    uint32_t class2Count = subtable.uint16At(14);
    if (class1 >= class1Count || class2 >= class2Count)
        return 0;
    uint32_t at = 16 + (uint32_t(class1) * class2Count + class2) * (size1 + size2);
    applyValue(face, font, subtable.from(at), subtable, format1, unit, glyphs[first]);
    applyValue(face, font, subtable.from(at + size1), subtable, format2, unit, glyphs[first + 1]);
    return format2 != 0 ? 2 : 1;
}

// Returns the script record's LangSys for the language, or its default LangSys, or no bytes.
Bytes langSysOf(Bytes scripts, uint32_t scriptTag, const uint32_t* languages, uint32_t languageCount) {
    uint32_t count = scripts.uint16At(0);
    for (uint32_t i = 0; i < count; ++i) {
        if (scripts.uint32At(2 + i * 6) != scriptTag)
            continue;
        Bytes script = scripts.from(scripts.uint16At(2 + i * 6 + 4));
        uint32_t langs = script.uint16At(2);
        for (uint32_t l = 0; l < languageCount; ++l) {
            for (uint32_t k = 0; k < langs; ++k) {
                if (script.uint32At(4 + k * 6) == languages[l])
                    return script.from(script.uint16At(4 + k * 6 + 4));
            }
        }
        return script.uint16At(0) != 0 ? script.from(script.uint16At(0)) : Bytes{nullptr, 0};
    }
    return Bytes{nullptr, 0};
}

// Returns the kerning of the font for a script and language, read on first use.
const KernConfig& configFor(const SimpleFace& face, SimpleFont& font, PrScript script, PrLocale locale) {
    uint32_t scriptTags[kPrScriptMaxOpenTypeTags];
    uint32_t scriptCount = prScriptOpenTypeTags(script, scriptTags);
    uint32_t key = scriptCount != 0 ? scriptTags[0] : kDflt;
    uint32_t languages[4];
    uint32_t languageCount = prLocaleOpenTypeLanguages(locale, languages, 4);
    if (languageCount > 4)
        languageCount = 4;
    uint32_t language = languageCount != 0 ? languages[0] : 0;
    for (uint32_t i = 0; i < font.configs.count; ++i) {
        if (font.configs.data[i].script == key && font.configs.data[i].language == language)
            return font.configs.data[i];
    }
    KernConfig& config = font.configs.emplace();
    config = KernConfig{};
    config.script = key;
    config.language = language;
    const Bytes& gpos = face.gpos;
    if (gpos.uint16At(0) == 1) {
        Bytes scripts = gpos.from(gpos.uint16At(4));
        Bytes features = gpos.from(gpos.uint16At(6));
        Bytes lookups = gpos.from(gpos.uint16At(8));
        Bytes langSys{nullptr, 0};
        for (uint32_t i = 0; i < scriptCount && langSys.size == 0; ++i)
            langSys = langSysOf(scripts, scriptTags[i], languages, languageCount);
        if (langSys.size == 0)
            langSys = langSysOf(scripts, kDflt, languages, languageCount);
        if (langSys.size == 0)
            langSys = langSysOf(scripts, kLatn, languages, languageCount);
        RawArray<uint16_t> indices;
        uint32_t featureCount = langSys.uint16At(4);
        for (uint32_t f = 0; f < featureCount; ++f) {
            uint16_t feature = langSys.uint16At(6 + f * 2);
            if (features.uint32At(2 + uint32_t(feature) * 6) != kKern)
                continue;
            Bytes table = features.from(features.uint16At(2 + uint32_t(feature) * 6 + 4));
            for (uint32_t k = 0; k < table.uint16At(2); ++k)
                indices.push(table.uint16At(4 + k * 2));
        }
        // Lookups apply in the order of the lookup list.
        for (uint32_t i = 1; i < indices.count; ++i) {
            for (uint32_t j = i; j > 0 && indices.data[j - 1] > indices.data[j]; --j) {
                uint16_t t = indices.data[j];
                indices.data[j] = indices.data[j - 1];
                indices.data[j - 1] = t;
            }
        }
        for (uint32_t i = 0; i < indices.count; ++i) {
            if (i != 0 && indices.data[i] == indices.data[i - 1])
                continue;
            if (indices.data[i] < lookups.uint16At(0))
                config.lookups.push(lookups.uint16At(2 + uint32_t(indices.data[i]) * 2) + gpos.uint16At(8));
        }
        indices.reset();
    }
    // A font without GPOS kerning kerns by its kern table, as HarfBuzz falls back to it.
    config.fromKernTable = config.lookups.count == 0 && face.kern.size != 0;
    return config;
}

// Kerns the pair at `first` by the kern table. Returns true if the table has an adjustment for it.
bool kernPair(const SimpleFace& face, const SimpleFont& font, PrTextGlyph* glyphs, uint32_t first) {
    const Bytes& kern = face.kern;
    if (kern.uint16At(0) != 0)
        return false;
    uint32_t key = uint32_t(glyphs[first].id) << 16 | glyphs[first + 1].id;
    uint32_t at = 4;
    for (uint32_t t = 0; t < kern.uint16At(2); ++t) {
        uint32_t length = kern.uint16At(at + 2);
        uint16_t coverage = kern.uint16At(at + 4);
        // Format 0, horizontal, of kerning values rather than minimums or cross-stream adjustments.
        if ((coverage >> 8) == 0 && (coverage & 0x7) == 0x1) {
            uint32_t count = kern.uint16At(at + 6);
            uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return kern.uint32At(at + 14 + k * 6) < key; });
            if (i < count && kern.uint32At(at + 14 + i * 6) == key) {
                glyphs[first].advance.x += float(kern.int16At(at + 14 + i * 6 + 4)) / font.unitsPerEm;
                return true;
            }
        }
        if (length == 0)
            break;
        at += length;
    }
    return false;
}

// Returns true if the run kerns the glyph at byte `at`: unless a feature turns kern off there.
bool kerns(const PrTextRun* run, uint32_t at) {
    bool on = true;
    for (uint32_t i = 0; i < run->featureCount; ++i) {
        const PrTextFeature& f = run->features[i];
        if (f.tag == kKern && f.start <= at && at < f.end)
            on = f.value != 0;
    }
    return on;
}

// Kerns the glyphs of a run, in the order of the text.
void kern(const SimpleFace& face, SimpleFont& font, const PrTextRun* run, PrTextGlyph* glyphs, uint32_t count) {
    if (count < 2)
        return;
    const KernConfig& config = configFor(face, font, run->script, run->locale);
    if (config.fromKernTable) {
        for (uint32_t i = 0; i + 1 < count; ++i) {
            if (kerns(run, glyphs[i].cluster))
                kernPair(face, font, glyphs, i);
        }
        return;
    }
    for (uint32_t l = 0; l < config.lookups.count; ++l) {
        Bytes lookup = face.gpos.from(config.lookups.data[l]);
        uint16_t type = lookup.uint16At(0);
        uint32_t subtables = lookup.uint16At(4);
        for (uint32_t i = 0; i + 1 < count;) {
            uint32_t consumed = 0;
            if (kerns(run, glyphs[i].cluster)) {
                for (uint32_t s = 0; s < subtables && consumed == 0; ++s) {
                    Bytes subtable = lookup.from(lookup.uint16At(6 + s * 2));
                    uint16_t subtype = type;
                    if (type == kExtension) {
                        subtype = subtable.uint16At(2);
                        subtable = subtable.from(subtable.uint32At(4));
                    }
                    if (subtype == kPairPos)
                        consumed = applyPair(face, font, subtable, glyphs, i);
                }
            }
            i += consumed > 1 ? 2 : 1;
        }
    }
}

// Appends a run across, in the order of the text, and then reverses a right-to-left run into visual order.
PrStatus shapeAcross(const SimpleFace& face, SimpleFont& font, const PrTextRun* run, PrTextGlyphBuffer* out) {
    float sizeAdjust = prFontSizeAdjust(run->font);
    uint32_t first = prTextGlyphBufferGlyphCount(out);
    for (size_t at = 0; at < run->text.size;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run->text, at, &codePoint);
        PrTextGlyph* g = prTextGlyphBufferAppend(out, 1);
        g->id = prFontCodePointGlyph(run->font, codePoint);
        g->cluster = uint32_t(at);
        // The font's advances include its size-adjust, which the ems of the design that a backend returns do not.
        g->advance = PrVec2{prFontGlyphAdvance(run->font, g->id, 0) / sizeAdjust, 0};
        at += length;
    }
    uint32_t count = prTextGlyphBufferGlyphCount(out) - first;
    PrTextGlyph* glyphs = prTextGlyphBufferAppend(out, 0) - count;
    kern(face, font, run, glyphs, count);
    if (run->direction == PrTextDirection_Rtl) {
        for (uint32_t i = 0; i < count / 2; ++i) {
            PrTextGlyph t = glyphs[i];
            glyphs[i] = glyphs[count - 1 - i];
            glyphs[count - 1 - i] = t;
        }
    }
    return PrStatus_Ok;
}

// Appends a run down a vertical line. Each glyph stands upright at its vertical origin, and advances down by its
// vertical advance.
PrStatus shapeDown(const PrTextRun* run, PrTextGlyphBuffer* out) {
    float sizeAdjust = prFontSizeAdjust(run->font);
    for (size_t at = 0; at < run->text.size;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run->text, at, &codePoint);
        PrTextGlyph* g = prTextGlyphBufferAppend(out, 1);
        g->id = prFontCodePointGlyph(run->font, codePoint);
        g->cluster = uint32_t(at);
        PrVec2 origin = prFontGlyphVerticalOrigin(run->font, g->id);
        g->offset = PrVec2{-origin.x / sizeAdjust, -origin.y / sizeAdjust};
        g->advance = PrVec2{0, prFontGlyphVerticalAdvance(run->font, g->id) / sizeAdjust};
        at += length;
    }
    return PrStatus_Ok;
}

PrStatus shape(PrTextShaper*, void* faceState, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    if (run->axis == PrTextAxis_Vertical)
        return shapeDown(run, out);
    return shapeAcross(*static_cast<SimpleFace*>(faceState), *static_cast<SimpleFont*>(fontState), run, out);
}

void destroyBackend(PrTextShaper* shaper) {
    prMemoryFree(shaper);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextShaper* prTextSimpleCreate(void) {
    PrTextShaper* shaper = allocZeroed<PrTextShaper>();
    shaper->destroy = destroyBackend;
    shaper->shape = shape;
    shaper->faceSize = sizeof(SimpleFace);
    shaper->fontSize = sizeof(SimpleFont);
    shaper->createFace = createFace;
    shaper->destroyFace = destroyFace;
    shaper->createFont = createFont;
    shaper->destroyFont = destroyFont;
    return shaper;
}
