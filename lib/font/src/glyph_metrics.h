// The advances and side bearings of glyphs, from hmtx and vmtx and their variations (HVAR and VVAR), in font units.

#pragma once

#include "pr/base.h"

#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The metric tables of a face.
struct FontGlyphMetricTables {
    SfntTable hmtx;
    SfntTable vmtx;
    SfntTable hvar;
    SfntTable vvar;
    uint32_t horizontalCount;       // the long metrics of hmtx; the advance of the last repeats past them
    uint32_t verticalCount;         // the long metrics of vmtx, or 0 if the face has no vertical metrics

    // The vertical extent that a face without vertical metrics gives every glyph: the typographic ascender and
    // descender of OS/2, or those of hhea without OS/2.
    int16_t ascender;
    int16_t descender;
};

// Opens the metric tables of the face.
void fontGlyphMetricsOpen(const Sfnt& sfnt, FontGlyphMetricTables* out);

// Returns true if the face varies its advances by HVAR or VVAR, rather than by the phantom points of its outlines.
bool fontGlyphMetricsVariesHorizontal(const FontGlyphMetricTables& tables);
bool fontGlyphMetricsVariesVertical(const FontGlyphMetricTables& tables);

// Gets the horizontal advance and left side bearing of the glyph. HVAR's delta moves the advance by whole units, as
// FreeType rounds it.
void fontGlyphMetricsHorizontal(const FontGlyphMetricTables& tables, SfntCoords coords, uint32_t glyph,
                                int32_t* advance, int32_t* lsb);

// Gets the vertical advance and top side bearing of the glyph, whose top is at `yMax`. A face without vertical metrics
// gives every glyph its vertical extent as its advance, with the glyph's top at the ascender. VVAR's delta moves the
// advance by whole units.
void fontGlyphMetricsVertical(const FontGlyphMetricTables& tables, SfntCoords coords, uint32_t glyph, int32_t yMax,
                              int32_t* advance, int32_t* tsb);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
