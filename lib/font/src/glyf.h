// TrueType outlines: glyf and loca, with the variations of gvar, and the phantom points that carry each glyph's metrics
// through them.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

#include "font/src/glyph_metrics.h"
#include "font/src/outline.h"
#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The TrueType outline tables of a face.
struct GlyfFace {
    SfntTable glyf;
    SfntTable loca;
    SfntTable gvar;
    bool longOffsets;               // loca holds 32-bit offsets, not halved 16-bit ones
    uint32_t glyphCount;
    const FontGlyphMetricTables* metrics;
};

// The phantom points of a loaded glyph, in font units with y up: the horizontal origin and advance, and the vertical
// origin and advance, as TrueType places them after the glyph's points.
struct GlyfPhantom {
    PrVec2 horizontalOrigin;
    PrVec2 horizontalAdvance;
    PrVec2 verticalOrigin;
    PrVec2 verticalAdvance;
};

// Opens the outline tables of the face. Returns false if the face has no glyf and loca.
bool glyfOpen(const Sfnt& sfnt, const FontGlyphMetricTables* metrics, uint32_t glyphCount, GlyfFace* out);

// Loads the outline of the glyph at the coordinates into `out`, which it replaces, translated so that the horizontal
// origin is at x = 0, as FreeType places it. Gets the phantom points after the same translation. Returns false for a
// glyph past the end, or a glyph whose data does not parse; an empty glyph loads as no contours.
bool glyfLoad(const GlyfFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out, GlyfPhantom* phantom);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
