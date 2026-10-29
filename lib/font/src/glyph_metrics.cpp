// The advances and side bearings of glyphs.

#include "font/src/glyph_metrics.h"

namespace pr_impl {

namespace {

constexpr uint32_t kHhea = 0x68686561;           // 'hhea'
constexpr uint32_t kHmtx = 0x686d7478;           // 'hmtx'
constexpr uint32_t kVhea = 0x76686561;           // 'vhea'
constexpr uint32_t kVmtx = 0x766d7478;           // 'vmtx'
constexpr uint32_t kHvar = 0x48564152;           // 'HVAR'
constexpr uint32_t kVvar = 0x56564152;           // 'VVAR'
constexpr uint32_t kOs2 = 0x4f532f32;            // 'OS/2'

// Returns the advance delta that HVAR or VVAR gives the glyph, rounded to whole units, or 0 at the default instance.
int32_t advanceDelta(SfntTable table, SfntCoords coords, uint32_t glyph) {
    if (table.size == 0 || coords.count == 0)
        return 0;
    SfntTable store = table.from(table.uint32At(4));
    uint32_t mapping = table.uint32At(8);
    uint16_t outer = 0;
    uint16_t inner = 0;
    sfntDeltaSetIndex(mapping != 0 ? table.from(mapping) : SfntTable{nullptr, 0}, glyph, &outer, &inner);
    float delta = sfntVariationDelta(store, outer, inner, coords);
    return int32_t(delta < 0 ? delta - 0.5f : delta + 0.5f);
}

} // namespace

void fontGlyphMetricsOpen(const Sfnt& sfnt, FontGlyphMetricTables* out) {
    *out = FontGlyphMetricTables{};
    SfntTable hhea = sfntTable(sfnt, kHhea);
    out->hmtx = sfntTable(sfnt, kHmtx);
    uint32_t horizontal = hhea.uint16At(34);
    out->horizontalCount = out->hmtx.size >= horizontal * 4 ? horizontal : out->hmtx.size / 4;
    SfntTable vhea = sfntTable(sfnt, kVhea);
    out->vmtx = sfntTable(sfnt, kVmtx);
    uint32_t vertical = vhea.size != 0 ? vhea.uint16At(34) : 0;
    out->verticalCount = out->vmtx.size >= vertical * 4 ? vertical : out->vmtx.size / 4;
    out->hvar = sfntTable(sfnt, kHvar);
    out->vvar = sfntTable(sfnt, kVvar);
    SfntTable os2 = sfntTable(sfnt, kOs2);
    if (os2.size >= 74) {
        out->ascender = os2.int16At(68);
        out->descender = os2.int16At(70);
    } else {
        out->ascender = hhea.int16At(4);
        out->descender = hhea.int16At(6);
    }
}

bool fontGlyphMetricsVariesHorizontal(const FontGlyphMetricTables& tables) {
    return tables.hvar.size != 0;
}

bool fontGlyphMetricsVariesVertical(const FontGlyphMetricTables& tables) {
    return tables.vvar.size != 0;
}

void fontGlyphMetricsHorizontal(const FontGlyphMetricTables& tables, SfntCoords coords, uint32_t glyph,
                                int32_t* advance, int32_t* lsb) {
    uint32_t count = tables.horizontalCount;
    if (count == 0) {
        *advance = 0;
        *lsb = 0;
        return;
    }
    // Past the long metrics, the advance of the last repeats, and a short array of left side bearings follows.
    if (glyph < count) {
        *advance = tables.hmtx.uint16At(glyph * 4);
        *lsb = tables.hmtx.int16At(glyph * 4 + 2);
    } else {
        *advance = tables.hmtx.uint16At((count - 1) * 4);
        *lsb = tables.hmtx.int16At(count * 4 + (glyph - count) * 2);
    }
    *advance += advanceDelta(tables.hvar, coords, glyph);
}

void fontGlyphMetricsVertical(const FontGlyphMetricTables& tables, SfntCoords coords, uint32_t glyph, int32_t yMax,
                              int32_t* advance, int32_t* tsb) {
    uint32_t count = tables.verticalCount;
    if (count == 0) {
        *tsb = tables.ascender - yMax;
        int32_t extent = int32_t(tables.ascender) - tables.descender;
        *advance = extent < 0 ? -extent : extent;
        return;
    }
    if (glyph < count) {
        *advance = tables.vmtx.uint16At(glyph * 4);
        *tsb = tables.vmtx.int16At(glyph * 4 + 2);
    } else {
        *advance = tables.vmtx.uint16At((count - 1) * 4);
        *tsb = tables.vmtx.int16At(count * 4 + (glyph - count) * 2);
    }
    *advance += advanceDelta(tables.vvar, coords, glyph);
}

} // namespace pr_impl
