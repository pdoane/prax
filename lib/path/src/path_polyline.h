// Building and reading polylines.

#pragma once

#include "pr/float.h"
#include "pr/path_polyline.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if two points of a path are one point: as close as rounding leaves points that arithmetic meant to be
// equal, at their magnitude, such as the end of a contour that returns to its start by relative steps. The bound grows
// with the coordinates, because a float's precision falls as they grow.
inline bool pathSamePoint(PrVec2 a, PrVec2 b) {
    float magnitude = prF32Max(prF32Max(prF32Abs(a.x), prF32Abs(a.y)), prF32Max(prF32Abs(b.x), prF32Abs(b.y)));
    float bound = 1e-5f * prF32Max(magnitude, 0.1f);
    PrVec2 d{a.x - b.x, a.y - b.y};
    return d.x * d.x + d.y * d.y < bound * bound;
}

// Calls `write` with the points of `polyline` as a RawArray. `write` may append points to the last contour.
template <typename Write>
void pathPolylineWrite(PrPathPolyline& polyline, Write write) {
    RawArray<PrVec2> points;
    points.data = polyline.points;
    points.count = polyline.pointCount;
    points.capacity = polyline.pointCapacity;
    write(points);
    polyline.points = points.data;
    polyline.pointCount = points.count;
    polyline.pointCapacity = points.capacity;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
