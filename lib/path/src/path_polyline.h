// Building and reading polylines.

#pragma once

#include "pr/path_polyline.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

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
