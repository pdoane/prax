// Polygons on an integer grid, and the arrangement that their edges form.
//
// Flattened contours snap to a grid that is fine relative to their extent. A point is then a pair of integers, and an
// orientation test is exact. Every edge is split where another edge crosses or touches it, until no split creates
// another. Coincident pieces then merge, and keep the winding of each region across them. The result is a planar
// arrangement: pieces that meet only at their ends.

#pragma once

#include "pr/geom.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A point on the integer grid that flattened contours snap to.
struct PathGridPoint {
    int64_t x;
    int64_t y;
};

// Returns true if `a` and `b` are the same point.
inline bool operator==(PathGridPoint a, PathGridPoint b) {
    return a.x == b.x && a.y == b.y;
}

// Returns true if `a` precedes `b` ordered by x, then by y.
inline bool pathGridBeforeX(PathGridPoint a, PathGridPoint b) {
    return a.x != b.x ? a.x < b.x : a.y < b.y;
}

// Returns true if `a` precedes `b` ordered by y, then by x.
inline bool pathGridBeforeY(PathGridPoint a, PathGridPoint b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
}

// Returns twice the signed area of the triangle `a`, `b`, `c`. The result is positive if `c` lies left of the line from
// `a` to `b`, with y up.
inline int64_t pathGridOrient(PathGridPoint a, PathGridPoint b, PathGridPoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// The mapping between the plane and the grid.
struct PathGrid {
    double originX;
    double originY;
    double scale;
};

// Returns a grid over the extent of every point in the `count` arrays of `arrays`.
PathGrid pathGridOver(const RawArray<PrVec2>* PR_NONNULL const* PR_NONNULL arrays, uint32_t count);

// Converts `p` from the plane to `grid`, and from `grid` back to the plane.
PathGridPoint pathGridSnap(const PathGrid& grid, PrVec2 p);
PrVec2 pathGridUnsnap(const PathGrid& grid, PathGridPoint p);

// An edge of one region, directed as its contour runs.
struct PathGridEdge {
    PathGridPoint a;
    PathGridPoint b;
    uint8_t owner;              // the region that the edge belongs to: 0 or 1
};

// Appends to `edges` the edges of the rings in `points`, snapped to `grid` and marked with `owner`. Each entry of
// `ends` is the end of a ring in `points`. An edge that snaps to a point is left out.
void pathGridAddEdges(const PathGrid& grid, const RawArray<PrVec2>& points, const RawArray<uint32_t>& ends,
    uint8_t owner, RawArray<PathGridEdge>& edges);

// Splits each edge where another edge meets it, other than at a shared end. A split rounds its point to the grid, which
// can move an edge onto another, so splitting repeats a bounded number of times.
void pathGridSplitEdges(RawArray<PathGridEdge>& edges);

// A piece of the arrangement, from its lesser end to its greater end by pathGridBeforeX. `winding` holds how much each
// region winds across the piece, counting edges that run from `lo` to `hi` as +1.
struct PathGridPiece {
    PathGridPoint lo;
    PathGridPoint hi;
    int32_t winding[2];
};

// Appends to `out` the pieces of the split `edges`, with coincident edges merged, and sorts `out` by the ends of its
// pieces.
void pathGridMergePieces(const RawArray<PathGridEdge>& edges, RawArray<PathGridPiece>& out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
