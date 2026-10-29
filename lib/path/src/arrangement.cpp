// Polygons on an integer grid, and the arrangement that their edges form.

#include "path/src/arrangement.h"

#include "pr/sort.h"

namespace pr_impl {

namespace {

// The number of grid steps across the extent.
constexpr double kGridSteps = double(1 << 26);

// The number of further rounds of splitting allowed after rounding a split moves a point off its line.
constexpr uint32_t kSplitRounds = 8;

int sign(int64_t v) {
    return v > 0 ? 1 : v < 0 ? -1 : 0;
}

// Returns the nearest integer to `v`, rounding halves away from zero.
int64_t nearest(double v) {
    return v >= 0 ? int64_t(v + 0.5) : -int64_t(-v + 0.5);
}

// Returns true if `p`, which is on the line through the segment, lies strictly between the ends of the segment.
bool strictlyWithin(PathGridPoint a, PathGridPoint b, PathGridPoint p) {
    if (p == a || p == b)
        return false;
    int64_t dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);
    int64_t length = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
    return dot > 0 && dot < length;
}

struct Split {
    uint32_t edge;
    PathGridPoint at;
};

// Records the points at which the two segments meet, other than at shared ends, as splits of either segment.
void meet(const Array<PathGridEdge>& edges, uint32_t i, uint32_t j, Array<Split>& splits) {
    const PathGridEdge& p = edges.data[i];
    const PathGridEdge& q = edges.data[j];
    int64_t d1 = pathGridOrient(q.a, q.b, p.a);
    int64_t d2 = pathGridOrient(q.a, q.b, p.b);
    int64_t d3 = pathGridOrient(p.a, p.b, q.a);
    int64_t d4 = pathGridOrient(p.a, p.b, q.b);
    if (sign(d1) * sign(d2) < 0 && sign(d3) * sign(d4) < 0) {
        // A crossing, rounded to the grid.
        double t = double(d1) / double(d1 - d2);
        PathGridPoint at{p.a.x + nearest(t * double(p.b.x - p.a.x)),
                     p.a.y + nearest(t * double(p.b.y - p.a.y))};
        splits.push(Split{i, at});
        splits.push(Split{j, at});
        return;
    }
    // An end of one segment lies on the other. This also covers overlaps of collinear segments.
    if (d3 == 0 && strictlyWithin(p.a, p.b, q.a))
        splits.push(Split{i, q.a});
    if (d4 == 0 && strictlyWithin(p.a, p.b, q.b))
        splits.push(Split{i, q.b});
    if (d1 == 0 && strictlyWithin(q.a, q.b, p.a))
        splits.push(Split{j, p.a});
    if (d2 == 0 && strictlyWithin(q.a, q.b, p.b))
        splits.push(Split{j, p.b});
}

// Splits every edge where another meets it. Returns false if no edge was split.
bool splitOnce(Array<PathGridEdge>& edges) {
    // The pairs whose x extents overlap, found by a sweep over the edges in order of their left ends.
    Array<uint32_t> order;
    for (uint32_t i = 0; i < edges.count; ++i)
        order.push(i);
    auto left = [&](uint32_t i) {
        return edges.data[i].a.x < edges.data[i].b.x ? edges.data[i].a.x : edges.data[i].b.x;
    };
    auto right = [&](uint32_t i) {
        return edges.data[i].a.x > edges.data[i].b.x ? edges.data[i].a.x : edges.data[i].b.x;
    };
    sortItems(order.data, order.count, [&](uint32_t a, uint32_t b) { return left(a) < left(b); });
    Array<Split> splits;
    Array<uint32_t> active;
    for (uint32_t k = 0; k < order.count; ++k) {
        uint32_t i = order.data[k];
        int64_t x = left(i);
        uint32_t kept = 0;
        for (uint32_t a = 0; a < active.count; ++a) {
            uint32_t j = active.data[a];
            if (right(j) < x)
                continue;
            active.data[kept++] = j;
            const PathGridEdge& p = edges.data[i];
            const PathGridEdge& q = edges.data[j];
            int64_t pTop = p.a.y < p.b.y ? p.a.y : p.b.y;
            int64_t pBottom = p.a.y > p.b.y ? p.a.y : p.b.y;
            int64_t qTop = q.a.y < q.b.y ? q.a.y : q.b.y;
            int64_t qBottom = q.a.y > q.b.y ? q.a.y : q.b.y;
            if (pBottom >= qTop && qBottom >= pTop)
                meet(edges, i, j, splits);
        }
        active.count = kept;
        active.push(i);
    }
    order.reset();
    active.reset();
    if (splits.count == 0) {
        splits.reset();
        return false;
    }

    // Sorts the splits of each edge along the edge, and emits the pieces between them.
    sortItems(splits.data, splits.count, [&](const Split& a, const Split& b) {
        if (a.edge != b.edge)
            return a.edge < b.edge;
        const PathGridEdge& e = edges.data[a.edge];
        int64_t ta = (a.at.x - e.a.x) * (e.b.x - e.a.x) + (a.at.y - e.a.y) * (e.b.y - e.a.y);
        int64_t tb = (b.at.x - e.a.x) * (e.b.x - e.a.x) + (b.at.y - e.a.y) * (e.b.y - e.a.y);
        return ta < tb;
    });
    Array<PathGridEdge> pieces;
    uint32_t s = 0;
    for (uint32_t i = 0; i < edges.count; ++i) {
        const PathGridEdge e = edges.data[i];
        PathGridPoint from = e.a;
        for (; s < splits.count && splits.data[s].edge == i; ++s) {
            PathGridPoint at = splits.data[s].at;
            if (!(at == from) && !(at == e.b)) {
                pieces.push(PathGridEdge{from, at, e.owner});
                from = at;
            }
        }
        if (!(from == e.b))
            pieces.push(PathGridEdge{from, e.b, e.owner});
    }
    splits.reset();
    edges.reset();
    edges = pieces;
    return true;
}

} // namespace

PathGrid pathGridOver(const Array<PrVec2>* PR_NONNULL const* PR_NONNULL arrays, uint32_t count) {
    double lowX = 0, lowY = 0, highX = 0, highY = 0;
    bool any = false;
    for (uint32_t a = 0; a < count; ++a) {
        const Array<PrVec2>& points = *arrays[a];
        for (uint32_t i = 0; i < points.count; ++i) {
            double x = double(points.data[i].x);
            double y = double(points.data[i].y);
            lowX = !any || x < lowX ? x : lowX;
            lowY = !any || y < lowY ? y : lowY;
            highX = !any || x > highX ? x : highX;
            highY = !any || y > highY ? y : highY;
            any = true;
        }
    }
    double extent = highX - lowX > highY - lowY ? highX - lowX : highY - lowY;
    return PathGrid{lowX, lowY, extent > 0 ? kGridSteps / extent : 1};
}

PathGridPoint pathGridSnap(const PathGrid& grid, PrVec2 p) {
    return PathGridPoint{nearest((double(p.x) - grid.originX) * grid.scale),
                     nearest((double(p.y) - grid.originY) * grid.scale)};
}

PrVec2 pathGridUnsnap(const PathGrid& grid, PathGridPoint p) {
    return PrVec2{float(double(p.x) / grid.scale + grid.originX),
                   float(double(p.y) / grid.scale + grid.originY)};
}

void pathGridAddEdges(const PathGrid& grid, const Array<PrVec2>& points, const Array<uint32_t>& ends,
                      uint8_t owner, Array<PathGridEdge>& edges) {
    uint32_t start = 0;
    for (uint32_t r = 0; r < ends.count; ++r) {
        uint32_t end = ends.data[r];
        for (uint32_t i = start; i < end; ++i) {
            PathGridPoint a = pathGridSnap(grid, points.data[i]);
            PathGridPoint b = pathGridSnap(grid, points.data[i + 1 < end ? i + 1 : start]);
            if (!(a == b))
                edges.push(PathGridEdge{a, b, owner});
        }
        start = end;
    }
}

void pathGridSplitEdges(Array<PathGridEdge>& edges) {
    for (uint32_t round = 0; round < kSplitRounds && splitOnce(edges); ++round) {
    }
}

void pathGridMergePieces(const Array<PathGridEdge>& edges, Array<PathGridPiece>& out) {
    for (uint32_t i = 0; i < edges.count; ++i) {
        const PathGridEdge& e = edges.data[i];
        bool forward = pathGridBeforeX(e.a, e.b);
        PathGridPiece piece{forward ? e.a : e.b, forward ? e.b : e.a, {0, 0}};
        piece.winding[e.owner] = forward ? 1 : -1;
        out.push(piece);
    }
    sortItems(out.data, out.count, [](const PathGridPiece& a, const PathGridPiece& b) {
        if (!(a.lo == b.lo))
            return pathGridBeforeX(a.lo, b.lo);
        return pathGridBeforeX(a.hi, b.hi);
    });
    uint32_t kept = 0;
    for (uint32_t i = 0; i < out.count; ++i) {
        const PathGridPiece& p = out.data[i];
        if (kept != 0 && out.data[kept - 1].lo == p.lo && out.data[kept - 1].hi == p.hi) {
            out.data[kept - 1].winding[0] += p.winding[0];
            out.data[kept - 1].winding[1] += p.winding[1];
            continue;
        }
        out.data[kept++] = p;
    }
    out.count = kept;
}

} // namespace pr_impl
