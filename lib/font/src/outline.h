// Glyph outlines as fonts store them: contours of points, each on the curve or a control point, in font units with y
// up. The readers of glyf and CFF load them, and a font converts them to paths.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a point of an outline is. A quadratic contour may hold control points in a row, which imply the on-curve points
// halfway between them, and may start with a control point. Cubic control points come in pairs.
enum {
    OutlinePoint_On,
    OutlinePoint_Quadratic,
    OutlinePoint_Cubic,
};
typedef uint8_t OutlinePoint;

// The contours of a glyph. Each contour ends at the point before the start of the next.
struct FontGlyphOutline {
    Array<PrVec2> points;
    Array<OutlinePoint> kinds;      // the kind of each point
    Array<uint32_t> ends;           // for each contour, the index after its last point
};

// Empties the outline, keeping its storage, or frees it.
void fontOutlineClear(FontGlyphOutline& outline);
void fontOutlineReset(FontGlyphOutline& outline);

// Appends a point to the contour being built, or ends the contour. A contour ends with at least one point.
void fontOutlinePush(FontGlyphOutline& outline, PrVec2 point, OutlinePoint kind);
void fontOutlineEndContour(FontGlyphOutline& outline);

// Returns the box of the outline's points, control points included, or an empty rectangle at the origin if it has none.
PrRect fontOutlineControlBox(const FontGlyphOutline& outline);

// Transforms the points from `first` on.
void fontOutlineTransform(FontGlyphOutline& outline, uint32_t first, PrAffine2 transform);

// Grows each contour outward by half of `x` and `y` on each side, as FT_Outline_EmboldenXY does: each point moves along
// the bisector of its neighbors' directions, by no more than the shorter of its two segments allows.
void fontOutlineEmbolden(FontGlyphOutline& outline, float x, float y);

// Appends the outline to `path`, each point scaled by `scale` with y flipped down, as FT_Outline_Decompose walks it:
// every contour closes with a line back to its start if it does not already curve back to it.
void fontOutlineAppendPath(const FontGlyphOutline& outline, float scale, PrPath* path);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
