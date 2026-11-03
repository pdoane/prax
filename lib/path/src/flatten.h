// Curves flattened to lines.

#pragma once

#include "pr/geom.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the point of the quadratic curve `p` at parameter `t`.
inline PrVec2 pathQuadPoint(const PrVec2 p[PR_NONNULL 3], float t) {
    float u = 1 - t;
    return PrVec2{
        u * u * p[0].x + 2 * u * t * p[1].x + t * t * p[2].x,
        u * u * p[0].y + 2 * u * t * p[1].y + t * t * p[2].y,
    };
}

// Returns the point of the cubic curve `p` at parameter `t`.
inline PrVec2 pathCubicPoint(const PrVec2 p[PR_NONNULL 4], float t) {
    float u = 1 - t;
    float a = u * u * u;
    float b = 3 * u * u * t;
    float c = 3 * u * t * t;
    float d = t * t * t;
    return PrVec2{a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x, a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y};
}

// Returns the number of lines that flatten the curve to within `tolerance`.
uint32_t pathQuadLineCount(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance);
uint32_t pathCubicLineCount(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance);

// Appends to `out` the points of `count` lines at even steps of the parameter of the curve. The points leave out `p0`
// and end at the curve's end point.
void pathFlattenQuadLines(PrVec2 p0, PrVec2 p1, PrVec2 p2, uint32_t count, RawArray<PrVec2>& out);
void pathFlattenCubicLines(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, uint32_t count, RawArray<PrVec2>& out);

// Appends to `out` the points of lines within `tolerance` of the curve. The points leave out `p0` and end at the
// curve's end point.
void pathFlattenQuad(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance, RawArray<PrVec2>& out);
void pathFlattenCubic(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance, RawArray<PrVec2>& out);

// Appends to `out` the points of lines within `tolerance` of the curve that cover the same area as the curve. The
// points leave out `p0` and end at the curve's end point.
//
// A chord of a curve falls inside it. Each point between the ends therefore moves off the curve, away from its center
// of curvature. The move balances the area that one chord loses against the area that the next chord gains.
void pathFlattenQuadArea(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance, RawArray<PrVec2>& out);
void pathFlattenCubicArea(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance, RawArray<PrVec2>& out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
