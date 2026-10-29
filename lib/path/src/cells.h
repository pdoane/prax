// Area coverage accumulated in cells along rows of pixels.

#pragma once

#include "pr/path.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An edge stored from top to bottom. `direction` is +1 if the edge runs down and -1 if it runs up.
struct PathCellEdge {
    float x0;
    float y0;
    float x1;
    float y1;
    float direction;
};

// Returns the coverage, from 0 to 1, that the winding number `winding` gives under `rule`.
float pathCoverageFromWinding(float winding, PrPathFillRule rule);

// Adds to `cells` the signed area that `e` leaves in each cell of the rows that it crosses, so that a running sum along
// a row is the winding that covers each pixel. `e` lies within [0, width] horizontally, and its crossing of each row is
// clamped to that range. Row `y` of `cells` starts at `cells + y * stride` and holds `width + 2` cells.
void pathAccumulate(float* cells, size_t stride, uint32_t width, uint32_t height, PathCellEdge e);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
