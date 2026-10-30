// Arithmetic in layout's unit (PrLayoutUnit), saturating at the ends of its range when a sum or product passes them.
// The operators are declared in pr/layout_unit.h.

#pragma once

#include "pr/float.h"
#include "pr/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The largest and smallest raw values of a unit.
constexpr int32_t kLayoutUnitMax = INT32_MAX;
constexpr int32_t kLayoutUnitMin = INT32_MIN;

// Returns the unit whose raw value is `raw`.
constexpr PrLayoutUnit layoutUnit(int32_t raw) {
    return PrLayoutUnit{raw};
}

// kLayoutUnitZero is zero units, and layoutUnitPx returns `px` whole pixels in units.
constexpr PrLayoutUnit kLayoutUnitZero = PrLayoutUnit{0};
constexpr PrLayoutUnit layoutUnitPx(int32_t px) {
    return prLayoutUnitClamped(int64_t(px) * kPrLayoutUnitsPerPx);
}

// Returns `unit` scaled by `factor`, truncated toward zero, which is how a percentage of it resolves.
inline PrLayoutUnit layoutUnitScaled(PrLayoutUnit unit, float factor) {
    double scaled = double(unit.raw) * double(factor);
    return scaled != scaled ? kLayoutUnitZero : prLayoutUnitClamped(int64_t(maxF64(minF64(scaled, 4e18), -4e18)));
}

// Returns the greater of two units, or the lesser.
inline PrLayoutUnit layoutUnitMax(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw > b.raw ? a : b;
}
inline PrLayoutUnit layoutUnitMin(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw < b.raw ? a : b;
}

// layoutVec returns a vector of two units, and kLayoutVecZero is the zero vector.
constexpr PrLayoutVec layoutVec(PrLayoutUnit x, PrLayoutUnit y) {
    return PrLayoutVec{x, y};
}
constexpr PrLayoutVec kLayoutVecZero = PrLayoutVec{{0}, {0}};

// Returns the rectangle at `offset` with extent `extent`.
inline PrLayoutRect layoutRectAt(PrLayoutVec offset, PrLayoutVec extent) {
    return PrLayoutRect{offset.x, offset.y, offset.x + extent.x, offset.y + extent.y};
}

// Returns a rectangle's width or height, never negative.
PrLayoutUnit layoutRectWidth(PrLayoutRect rect);
PrLayoutUnit layoutRectHeight(PrLayoutRect rect);

// Returns true if `rect` holds `point`, including its left and top edges but not its right and bottom edges.
bool layoutRectContains(PrLayoutRect rect, PrLayoutVec point);

// Returns the smallest rectangle that holds both rectangles.
inline PrLayoutRect layoutRectUnion(PrLayoutRect a, PrLayoutRect b) {
    return PrLayoutRect{
        layoutUnitMin(a.left, b.left), layoutUnitMin(a.top, b.top), layoutUnitMax(a.right, b.right),
        layoutUnitMax(a.bottom, b.bottom),
    };
}

// Returns the part of the rectangles that both cover, which is empty if they do not meet.
inline PrLayoutRect layoutRectIntersect(PrLayoutRect a, PrLayoutRect b) {
    return PrLayoutRect{
        layoutUnitMax(a.left, b.left), layoutUnitMax(a.top, b.top), layoutUnitMin(a.right, b.right),
        layoutUnitMin(a.bottom, b.bottom),
    };
}

// Returns `rect` moved by `offset`.
inline PrLayoutRect layoutRectMoved(PrLayoutRect rect, PrLayoutVec offset) {
    return PrLayoutRect{rect.left + offset.x, rect.top + offset.y, rect.right + offset.x, rect.bottom + offset.y};
}

// The coordinate of a clip's sides in an axis that it does not clip: a distance that no layout comes near, and that
// moving a clip leaves unchanged.
constexpr PrLayoutUnit kLayoutClipUnbounded = PrLayoutUnit{kLayoutUnitMax / 4};

// Returns the clip `clip` moved by `offset`, except for its unbounded sides (kLayoutClipUnbounded).
inline PrLayoutRect layoutClipMoved(PrLayoutRect clip, PrLayoutVec offset) {
    PrLayoutRect moved = layoutRectMoved(clip, offset);
    if (clip.left.raw == -kLayoutClipUnbounded.raw) {
        moved.left = clip.left;
        moved.right = clip.right;
    }
    if (clip.top.raw == -kLayoutClipUnbounded.raw) {
        moved.top = clip.top;
        moved.bottom = clip.bottom;
    }
    return moved;
}

// Returns the smallest rectangle of units that holds a rectangle of pixels.
PrLayoutRect layoutRectEnclosing(PrRect px);

// Returns the translation by `offset`, or `offset` itself, in pixels.
PrDAffine2 layoutTranslation(PrLayoutVec offset);
PrDVec2 layoutOffset(PrLayoutVec offset);

// Returns `point` mapped through `transform`, which maps pixels, rounded to the nearest unit, or the smallest rectangle
// of units that holds `rect` mapped through `transform`. Both map in doubles, so that they keep the precision of units
// far from the origin. Rounding keeps a point that lands on an edge on that edge, where truncation in either direction
// would move it across the edge on one side.
PrLayoutVec layoutVecMapped(PrDAffine2 transform, PrLayoutVec point);
PrLayoutRect layoutRectMapped(PrDAffine2 transform, PrLayoutRect rect);

// Writes `point` mapped through `transform`, which maps pixels, rounded to the nearest unit, to `*out`. Returns false,
// and leaves `*out` unchanged, if the point lies behind the eye (prDMatrix3MapPoint).
bool layoutVecProjected(const PrDMatrix3& transform, PrLayoutVec point, PrLayoutVec* out);

// Returns the smallest rectangle of units that holds `rect` mapped through `transform`, with the part nearer the eye
// than kPrPathPolylineNearPlane cut away, as drawing cuts a projected plane. Returns an empty rectangle at the origin
// if the whole rectangle lies behind the eye.
PrLayoutRect layoutRectProjected(const PrDMatrix3& transform, PrLayoutRect rect);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
