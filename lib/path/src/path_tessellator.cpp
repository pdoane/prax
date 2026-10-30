// Tessellating paths into triangles.
//
// A path of one convex contour fans from its first point. For any other path, the edges form an arrangement on an
// integer grid (arrangement.h), in which edges meet only at their ends. A sweep from top to bottom visits the vertices
// of the arrangement in order of y, then x. It keeps the edges that cross the sweep line in order from left to right,
// and the winding of each span between them. Each span inside the region is a polygon that is monotone in the order of
// the sweep, and it is triangulated as its vertices arrive. If two spans merge at a vertex, both polygons stay open
// until the next vertex below, and a diagonal to that vertex separates them. If a vertex splits a span, a diagonal to
// the last vertex of the span separates them.
//
// With a fringe, a first sweep finds only the boundary: every edge that has the inside on one side only. The boundary
// edges chain into loops, each wound with the inside on its left, and the loops place the fringe. The loops moved
// inward are then swept again and triangulated where they wind positively. If a loop crosses itself after moving, the
// crossed part therefore drops out instead of being covered twice.

#include "pr/path_tessellator.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/vec.h"

#include "path/src/arrangement.h"
#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

constexpr uint32_t kNone = 0xffffffffu;

// The largest factor by which a corner of a fringe stretches its half width. It limits the miter at a sharp corner to
// twice the half width.
constexpr float kMiterMaxStretch = 4;

// The rule that decides which windings are inside: the fill rules, and the positive windings of loops wound with the
// inside on their left.
enum {
    Winding_NonZero,
    Winding_EvenOdd,
    Winding_Positive,
};
typedef uint8_t Winding;

// The chain of a monotone polygon that a vertex is on, facing down the sweep.
enum {
    Side_Left,
    Side_Right,
};
typedef uint8_t Side;

// An edge of the arrangement, from its earlier vertex in the order of the sweep to its later one.
struct SweepEdge {
    uint32_t upper;
    uint32_t lower;

    // The windings of the contours along the edge, each counting +1 for a contour that runs from lower to upper. A
    // contour wound with the inside on its left therefore winds +1 around the inside.
    int32_t winding;

    bool boundary;              // true if the inside is on one side only
    bool insideLeft;            // for a boundary edge, true if the inside is on the left
};

// A polygon that is monotone in the order of the sweep, triangulated as its vertices arrive.
struct Monotone {
    Array<uint32_t> stack;      // the vertices not yet triangulated, a reflex chain
    Side stackSide;
    uint32_t last;              // the vertex added last, on `lastSide`
    Side lastSide;
};

// The space between two neighboring active edges. If it is inside the region, it holds the polygon above it. After two
// polygons merge at a vertex, the span holds both until the next vertex in it.
struct Span {
    int32_t winding;
    uint32_t polygon;           // kNone outside the region, or if the sweep does not triangulate
    uint32_t merged;            // the polygon right of the merge, or kNone
};

// An edge of the boundary, with the inside on its left.
struct BoundaryEdge {
    uint32_t from;
    uint32_t to;
    bool used;
};

struct PathTessellator {
    // The mesh.
    Array<PrVec2> points;
    Array<float> coverage;
    Array<uint32_t> indices;

    // The rings that a sweep reads: the flattened path, then the loops moved inward.
    Array<PrVec2> ring;
    Array<uint32_t> ends;

    // The arrangement.
    Array<PathGridEdge> gridEdges;
    Array<PathGridPiece> pieces;
    Array<PathGridPoint> vertices;      // sorted by pathGridBeforeY
    Array<SweepEdge> edges;             // sorted by upper vertex; the edges of each vertex start at `starts`
    Array<uint32_t> starts;             // vertices.count + 1

    // The sweep.
    bool triangulate;
    uint32_t base;                      // the mesh vertex of the first vertex of the sweep
    Array<uint32_t> active;             // edges crossing the sweep line, left to right
    Array<Span> spans;                  // active.count + 1, left to right
    Array<Monotone> polygons;
    Array<uint32_t> freePolygons;
    Array<uint32_t> starting;           // the edges below the current vertex, left to right
    Array<Span> between;                // the spans that replace those around the current vertex

    // The boundary.
    Array<BoundaryEdge> boundary;       // sorted by `from`
    Array<uint32_t> loop;
    Array<PrVec2> loopPoints;
};

bool inside(int32_t winding, Winding rule) {
    switch (rule) {
        case Winding_EvenOdd:
            return (winding & 1) != 0;
        case Winding_Positive:
            return winding > 0;
        default:
            return winding != 0;
    }
}

// Returns twice the signed area of the triangle. The result is positive if `c` is left of the line from `a` to `b`
// under pathGridOrient.
double sideOf(PrVec2 a, PrVec2 b, PrVec2 c) {
    return (double(b.x) - double(a.x)) * (double(c.y) - double(a.y)) -
        (double(b.y) - double(a.y)) * (double(c.x) - double(a.x));
}

// The shape of a ring of points.
enum {
    Shape_Other,
    Shape_Empty,                // no area
    Shape_Convex,               // convex, turning left under pathGridOrient
    Shape_ConvexReversed,       // convex, turning right
};
typedef uint8_t Shape;

// Returns the shape of the ring. The ring is convex and simple if every turn has the same sign, and the x and y of the
// direction each change sign at most twice. Collinear and repeated points are allowed.
Shape shapeOf(const PrVec2* p, uint32_t n) {
    int turn = 0;
    uint32_t flipsX = 0;
    uint32_t flipsY = 0;
    int lastX = 0;
    int lastY = 0;
    double area = 0;
    for (uint32_t i = 0; i < n; ++i) {
        PrVec2 a = p[i];
        PrVec2 b = p[i + 1 < n ? i + 1 : 0];
        PrVec2 c = p[i + 2 < n ? i + 2 : i + 2 - n];
        double cross = sideOf(a, b, c);
        int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        if (s != 0) {
            if (turn != 0 && s != turn)
                return Shape_Other;
            turn = s;
        }
        int dx = b.x > a.x ? 1 : b.x < a.x ? -1 : 0;
        int dy = b.y > a.y ? 1 : b.y < a.y ? -1 : 0;
        if (dx != 0) {
            flipsX += lastX != 0 && dx != lastX ? 1 : 0;
            lastX = dx;
        }
        if (dy != 0) {
            flipsY += lastY != 0 && dy != lastY ? 1 : 0;
            lastY = dy;
        }
        area += double(a.x) * double(b.y) - double(b.x) * double(a.y);
    }
    if (flipsX > 2 || flipsY > 2)
        return Shape_Other;
    if (area == 0 || turn == 0)
        return Shape_Empty;
    return area > 0 ? Shape_Convex : Shape_ConvexReversed;
}

// ---------------------------------------------------------------------------------------------------------------------
// The fringe

// Places the fringe of a loop. Each point moves inward by half the fringe, and a band of quads joins it to the point
// moved outward by the same distance. The loop turns with the inside on its left under pathGridOrient. The inner points
// are appended to `ring` as a ring of their own. Returns the mesh vertex of the first inner point, which the others
// follow.
uint32_t addFringe(PathTessellator& t, const PrVec2* loop, uint32_t count, float half) {
    uint32_t first = t.points.count;
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 p = loop[i];
        PrVec2 prev = loop[i == 0 ? count - 1 : i - 1];
        PrVec2 next = loop[i + 1 == count ? 0 : i + 1];
        PrVec2 d0 = prVec2Normalize(p - prev);
        PrVec2 d1 = prVec2Normalize(next - p);
        // The mean of the outward normals, stretched to reach the point at which the offset sides meet.
        PrVec2 m{(d0.y + d1.y) / 2, (-d0.x - d1.x) / 2};
        float length2 = m.x * m.x + m.y * m.y;
        if (length2 > 1e-6f) {
            float stretch = minF32(1 / length2, kMiterMaxStretch);
            m.x *= stretch;
            m.y *= stretch;
        }
        PrVec2 in{p.x - m.x * half, p.y - m.y * half};
        t.points.push(in);
        t.coverage.push(1);
        t.points.push(p + m * half);
        t.coverage.push(0);
        t.ring.push(in);
    }
    t.ends.push(t.ring.count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t j = i + 1 == count ? 0 : i + 1;
        uint32_t inI = first + i * 2;
        uint32_t inJ = first + j * 2;
        uint32_t quad[6] = {inI, inJ, inJ + 1, inI, inJ + 1, inI + 1};
        t.indices.append(quad, 6);
    }
    return first;
}

// ---------------------------------------------------------------------------------------------------------------------
// Monotone polygons

void triangle(PathTessellator& t, uint32_t a, uint32_t b, uint32_t c) {
    if (pathGridOrient(t.vertices.data[a], t.vertices.data[b], t.vertices.data[c]) == 0)
        return;
    uint32_t indices[3] = {t.base + a, t.base + b, t.base + c};
    t.indices.append(indices, 3);
}

// Starts a polygon with its top at `v`, and returns it, or kNone if the sweep does not triangulate.
uint32_t startPolygon(PathTessellator& t, uint32_t v) {
    if (!t.triangulate)
        return kNone;
    uint32_t index;
    if (t.freePolygons.count != 0) {
        index = t.freePolygons.data[--t.freePolygons.count];
    } else {
        index = t.polygons.count;
        t.polygons.emplace().stack = Array<uint32_t>{};
    }
    Monotone& m = t.polygons.data[index];
    m.stack.clear();
    m.stack.push(v);
    m.stackSide = Side_Right;
    m.last = v;
    m.lastSide = Side_Right;
    return index;
}

// Adds the next vertex on one chain, emitting the triangles it completes.
void addVertex(PathTessellator& t, uint32_t polygon, uint32_t v, Side side) {
    Monotone& m = t.polygons.data[polygon];
    m.last = v;
    m.lastSide = side;
    if (m.stack.count == 1) {
        m.stack.push(v);
        m.stackSide = side;
        return;
    }
    if (side != m.stackSide) {
        // The vertex sees the whole chain on the other side.
        for (uint32_t i = 0; i + 1 < m.stack.count; ++i)
            triangle(t, m.stack.data[i], m.stack.data[i + 1], v);
        uint32_t top = m.stack.last();
        m.stack.clear();
        m.stack.push(top);
        m.stack.push(v);
        m.stackSide = side;
        return;
    }
    // The vertex sees the chain back to its first reflex vertex.
    const PathGridPoint p = t.vertices.data[v];
    while (m.stack.count >= 2) {
        uint32_t a = m.stack.data[m.stack.count - 2];
        uint32_t b = m.stack.last();
        int64_t o = pathGridOrient(t.vertices.data[a], p, t.vertices.data[b]);
        if (side == Side_Left ? o <= 0 : o >= 0)
            break;
        triangle(t, a, b, v);
        m.stack.pop();
    }
    m.stack.push(v);
}

// Ends the polygon at its last vertex.
void endPolygon(PathTessellator& t, uint32_t polygon, uint32_t v) {
    Monotone& m = t.polygons.data[polygon];
    for (uint32_t i = 0; i + 1 < m.stack.count; ++i)
        triangle(t, m.stack.data[i], m.stack.data[i + 1], v);
    m.stack.clear();
    t.freePolygons.push(polygon);
}

// Adds the vertex to the side of the span, and returns the polygon that continues below it. In a merged span, a
// diagonal runs from the merge to this vertex, and ends the polygon on the side that the vertex is on.
uint32_t continueSpan(PathTessellator& t, const Span& s, uint32_t v, Side side) {
    if (s.polygon == kNone)
        return kNone;
    if (s.merged != kNone) {
        if (side == Side_Right) {
            endPolygon(t, s.merged, v);
            addVertex(t, s.polygon, v, Side_Right);
            return s.polygon;
        }
        endPolygon(t, s.polygon, v);
        addVertex(t, s.merged, v, Side_Left);
        return s.merged;
    }
    addVertex(t, s.polygon, v, side);
    return s.polygon;
}

void closeSpan(PathTessellator& t, const Span& s, uint32_t v) {
    if (s.polygon != kNone)
        endPolygon(t, s.polygon, v);
    if (s.merged != kNone)
        endPolygon(t, s.merged, v);
}

// ---------------------------------------------------------------------------------------------------------------------
// The sweep

// Ends an active edge at its lower vertex, recording it if it is on the boundary.
void finishEdge(PathTessellator& t, uint32_t edge) {
    const SweepEdge& e = t.edges.data[edge];
    if (!e.boundary)
        return;
    // Facing from upper to lower, the left is toward smaller x, the side on which pathGridOrient is positive.
    if (e.insideLeft)
        t.boundary.push(BoundaryEdge{e.upper, e.lower, false});
    else
        t.boundary.push(BoundaryEdge{e.lower, e.upper, false});
}

// Divides the active edge at `v`, which it passes through or near. The edge ends there, and a new edge carries on
// below.
void divideEdge(PathTessellator& t, uint32_t edge, uint32_t v) {
    SweepEdge next = t.edges.data[edge];
    next.upper = v;
    uint32_t added = t.edges.count;
    t.edges.push(next);
    t.edges.data[edge].lower = v;
    t.starting.push(added);
}

// Visits a vertex. The active edges that end at it leave the sweep, the edges that start there join it, and the spans
// around it end, continue, merge or split.
void visit(PathTessellator& t, uint32_t v, Winding rule) {
    const PathGridPoint p = t.vertices.data[v];
    t.starting.clear();
    for (uint32_t e = t.starts.data[v]; e < t.starts.data[v + 1]; ++e)
        t.starting.push(e);

    // Finds the active edges that end here. Any edges between them that do not end here are divided. So are edges that
    // pass through the vertex, which the rounding of a split can leave behind.
    uint32_t first = kNone;
    uint32_t last = kNone;
    for (uint32_t i = 0; i < t.active.count; ++i) {
        const SweepEdge& e = t.edges.data[t.active.data[i]];
        bool here = e.lower == v || pathGridOrient(t.vertices.data[e.upper], t.vertices.data[e.lower], p) == 0;
        if (here) {
            first = first == kNone ? i : first;
            last = i;
        }
    }
    uint32_t position = first;
    if (first == kNone) {
        position = t.active.count;
        for (uint32_t i = 0; i < t.active.count; ++i) {
            const SweepEdge& e = t.edges.data[t.active.data[i]];
            if (pathGridOrient(t.vertices.data[e.upper], t.vertices.data[e.lower], p) > 0) {
                position = i;
                break;
            }
        }
    } else {
        for (uint32_t i = first; i <= last; ++i) {
            if (t.edges.data[t.active.data[i]].lower != v)
                divideEdge(t, t.active.data[i], v);
        }
    }
    if (first == kNone && t.starting.count == 0)
        return;

    // The edges below, left to right.
    sortItems(t.starting.data, t.starting.count, [&](uint32_t a, uint32_t b) {
        return pathGridOrient(p, t.vertices.data[t.edges.data[a].lower], t.vertices.data[t.edges.data[b].lower]) < 0;
    });

    t.between.clear();
    uint32_t replaced;
    if (first == kNone) {
        // The vertex is inside one span, whose polygon splits.
        Span s = t.spans.data[position];
        replaced = 1;
        uint32_t left = kNone;
        uint32_t right = kNone;
        if (s.polygon != kNone) {
            if (s.merged != kNone) {
                addVertex(t, s.polygon, v, Side_Right);
                addVertex(t, s.merged, v, Side_Left);
                left = s.polygon;
                right = s.merged;
            } else {
                const Monotone& m = t.polygons.data[s.polygon];
                uint32_t helper = m.last;
                if (m.lastSide == Side_Right) {
                    addVertex(t, s.polygon, v, Side_Right);
                    left = s.polygon;
                    right = startPolygon(t, helper);
                    addVertex(t, right, v, Side_Left);
                } else {
                    addVertex(t, s.polygon, v, Side_Left);
                    right = s.polygon;
                    left = startPolygon(t, helper);
                    addVertex(t, left, v, Side_Right);
                }
            }
        }
        t.between.push(Span{s.winding, left, kNone});
        int32_t winding = s.winding;
        for (uint32_t i = 0; i + 1 < t.starting.count; ++i) {
            winding += t.edges.data[t.starting.data[i]].winding;
            uint32_t polygon = inside(winding, rule) ? startPolygon(t, v) : kNone;
            t.between.push(Span{winding, polygon, kNone});
        }
        t.between.push(Span{s.winding, right, kNone});
    } else {
        Span sl = t.spans.data[first];
        Span sr = t.spans.data[last + 1];
        replaced = last - first + 2;
        for (uint32_t k = first + 1; k <= last; ++k)
            closeSpan(t, t.spans.data[k], v);
        for (uint32_t k = first; k <= last; ++k)
            finishEdge(t, t.active.data[k]);
        uint32_t left = continueSpan(t, sl, v, Side_Right);
        uint32_t right = continueSpan(t, sr, v, Side_Left);
        if (t.starting.count == 0) {
            // The spans on either side merge.
            Span merged{sl.winding, left, right};
            if (left == kNone || right == kNone)
                merged = Span{sl.winding, left != kNone ? left : right, kNone};
            if (merged.polygon != kNone && !inside(sl.winding, rule)) {
                closeSpan(t, merged, v);
                merged = Span{sl.winding, kNone, kNone};
            }
            t.between.push(merged);
        } else {
            t.between.push(Span{sl.winding, left, kNone});
            int32_t winding = sl.winding;
            for (uint32_t i = 0; i + 1 < t.starting.count; ++i) {
                winding += t.edges.data[t.starting.data[i]].winding;
                uint32_t polygon = inside(winding, rule) ? startPolygon(t, v) : kNone;
                t.between.push(Span{winding, polygon, kNone});
            }
            t.between.push(Span{sr.winding, right, kNone});
        }
    }

    // Each new edge is on the boundary if the inside differs across it.
    int32_t winding = t.between.data[0].winding;
    for (uint32_t i = 0; i < t.starting.count; ++i) {
        SweepEdge& e = t.edges.data[t.starting.data[i]];
        bool leftInside = inside(winding, rule);
        winding += e.winding;
        bool rightInside = inside(winding, rule);
        e.boundary = leftInside != rightInside;
        e.insideLeft = leftInside;
    }

    // Replaces the spans and edges around the vertex with the new ones.
    uint32_t removed = first == kNone ? 0 : last - first + 1;
    t.active.erase(position, removed);
    for (uint32_t i = 0; i < t.starting.count; ++i)
        t.active.insert(position + i, t.starting.data[i]);
    t.spans.erase(position, replaced);
    for (uint32_t i = 0; i < t.between.count; ++i)
        t.spans.insert(position + i, t.between.data[i]);
}

// ---------------------------------------------------------------------------------------------------------------------
// The arrangement

// Returns the index of the vertex at `p`.
uint32_t vertexAt(const PathTessellator& t, PathGridPoint p) {
    return partitionPoint(0, t.vertices.count, [&](uint32_t i) { return pathGridBeforeY(t.vertices.data[i], p); });
}

// Builds the vertices and edges of the sweep from the pieces of the arrangement.
void buildSweep(PathTessellator& t) {
    t.vertices.clear();
    for (uint32_t i = 0; i < t.pieces.count; ++i) {
        t.vertices.push(t.pieces.data[i].lo);
        t.vertices.push(t.pieces.data[i].hi);
    }
    sortItems(t.vertices.data, t.vertices.count, pathGridBeforeY);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < t.vertices.count; ++i) {
        if (unique == 0 || !(t.vertices.data[unique - 1] == t.vertices.data[i]))
            t.vertices.data[unique++] = t.vertices.data[i];
    }
    t.vertices.count = unique;

    t.edges.clear();
    for (uint32_t i = 0; i < t.pieces.count; ++i) {
        const PathGridPiece& piece = t.pieces.data[i];
        uint32_t lo = vertexAt(t, piece.lo);
        uint32_t hi = vertexAt(t, piece.hi);
        // The winding of a piece counts contours that run from lo to hi.
        if (lo < hi)
            t.edges.push(SweepEdge{lo, hi, -piece.winding[0], false, false});
        else
            t.edges.push(SweepEdge{hi, lo, piece.winding[0], false, false});
    }
    sortItems(t.edges.data, t.edges.count, [](const SweepEdge& a, const SweepEdge& b) { return a.upper < b.upper; });
    t.starts.clear();
    uint32_t e = 0;
    for (uint32_t v = 0; v <= t.vertices.count; ++v) {
        while (e < t.edges.count && t.edges.data[e].upper < v)
            ++e;
        t.starts.push(e);
    }
}

// Returns the angle from `r` to `d`, turning toward the inside of a boundary loop, as a pseudo-angle in [0, 4).
double turnFrom(PathGridPoint r, PathGridPoint d) {
    double cross = -(double(r.x) * double(d.y) - double(r.y) * double(d.x));
    double dot = double(r.x) * double(d.x) + double(r.y) * double(d.y);
    // A pseudo-angle that has the same order as atan2(cross, dot) taken into [0, 2π).
    double sum = absF64(cross) + absF64(dot);
    if (sum == 0)
        return 4;
    double a = cross / sum;
    double angle = dot >= 0 ? a : (cross >= 0 ? 2 - a : -2 - a);
    return angle < 0 ? angle + 4 : angle;
}

// Sweeps the arrangement of `ring`, finding its boundary and, if `triangulate`, its triangles and their vertices.
// Returns the grid it swept on.
PathGrid sweep(PathTessellator& t, Winding rule, bool triangulate) {
    const Array<PrVec2>* rings[1] = {&t.ring};
    PathGrid grid = pathGridOver(rings, 1);
    t.gridEdges.clear();
    pathGridAddEdges(grid, t.ring, t.ends, 0, t.gridEdges);
    pathGridSplitEdges(t.gridEdges);
    t.pieces.clear();
    pathGridMergePieces(t.gridEdges, t.pieces);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < t.pieces.count; ++i) {
        if (t.pieces.data[i].winding[0] != 0)
            t.pieces.data[kept++] = t.pieces.data[i];
    }
    t.pieces.count = kept;
    buildSweep(t);

    t.triangulate = triangulate;
    t.base = t.points.count;
    t.active.clear();
    t.spans.clear();
    t.spans.push(Span{0, kNone, kNone});
    t.boundary.clear();
    for (uint32_t v = 0; v < t.vertices.count; ++v)
        visit(t, v, rule);
    if (triangulate) {
        for (uint32_t v = 0; v < t.vertices.count; ++v) {
            t.points.push(pathGridUnsnap(grid, t.vertices.data[v]));
            t.coverage.push(1);
        }
    }
    return grid;
}

// Chains the boundary into loops, places their fringe, and replaces `ring` with the loops moved inward.
void addBoundaryFringe(PathTessellator& t, const PathGrid& grid, float fringe) {
    sortItems(t.boundary.data, t.boundary.count,
        [](const BoundaryEdge& a, const BoundaryEdge& b) { return a.from < b.from; });
    auto firstFrom = [&](uint32_t v) {
        return partitionPoint(0, t.boundary.count, [&](uint32_t i) { return t.boundary.data[i].from < v; });
    };
    t.ring.clear();
    t.ends.clear();
    for (uint32_t i = 0; i < t.boundary.count; ++i) {
        if (t.boundary.data[i].used)
            continue;
        t.loop.clear();
        uint32_t edge = i;
        uint32_t start = t.boundary.data[i].from;
        while (edge != kNone) {
            BoundaryEdge& e = t.boundary.data[edge];
            e.used = true;
            t.loop.push(e.from);
            if (e.to == start)
                break;
            // Of the edges that leave `to`, the first one met by turning from the way back toward the inside bounds the
            // same area.
            PathGridPoint at = t.vertices.data[e.to];
            PathGridPoint from = t.vertices.data[e.from];
            PathGridPoint back{from.x - at.x, from.y - at.y};
            uint32_t next = kNone;
            double best = 0;
            for (uint32_t k = firstFrom(e.to); k < t.boundary.count && t.boundary.data[k].from == e.to; ++k) {
                if (t.boundary.data[k].used)
                    continue;
                PathGridPoint to = t.vertices.data[t.boundary.data[k].to];
                double turn = turnFrom(back, PathGridPoint{to.x - at.x, to.y - at.y});
                if (next == kNone || turn < best) {
                    next = k;
                    best = turn;
                }
            }
            edge = next;
        }
        if (t.loop.count < 3)
            continue;
        t.loopPoints.clear();
        for (uint32_t k = 0; k < t.loop.count; ++k)
            t.loopPoints.push(pathGridUnsnap(grid, t.vertices.data[t.loop.data[k]]));
        addFringe(t, t.loopPoints.data, t.loopPoints.count, fringe / 2);
    }
}

// Fans the ring if it is one convex contour, and returns false otherwise.
bool fanConvex(PathTessellator& t, float fringe) {
    if (t.ends.count != 1)
        return false;
    // The distinct points in order.
    uint32_t n = 0;
    for (uint32_t i = 0; i < t.ring.count; ++i) {
        PrVec2 p = t.ring.data[i];
        if (n != 0 && p.x == t.ring.data[n - 1].x && p.y == t.ring.data[n - 1].y)
            continue;
        t.ring.data[n++] = p;
    }
    while (n > 1 && t.ring.data[n - 1].x == t.ring.data[0].x && t.ring.data[n - 1].y == t.ring.data[0].y)
        --n;
    t.ring.count = n;
    t.ends.data[0] = n;
    Shape shape = n < 3 ? Shape_Empty : shapeOf(t.ring.data, n);
    if (shape == Shape_Other)
        return false;
    if (shape == Shape_Empty)
        return true;
    uint32_t first = t.points.count;
    if (fringe > 0) {
        // The loop runs with the inside on its left, and its inner points replace the ring.
        t.loopPoints.clear();
        for (uint32_t i = 0; i < n; ++i)
            t.loopPoints.push(t.ring.data[shape == Shape_Convex ? i : n - 1 - i]);
        t.ring.clear();
        t.ends.clear();
        first = addFringe(t, t.loopPoints.data, n, fringe / 2);
        if (shapeOf(t.ring.data, n) != Shape_Convex) {
            // Moving inward turned the ring inside out somewhere.
            sweep(t, Winding_Positive, true);
            return true;
        }
        for (uint32_t i = 1; i + 1 < n; ++i) {
            uint32_t triangle[3] = {first, first + i * 2, first + i * 2 + 2};
            t.indices.append(triangle, 3);
        }
        return true;
    }
    for (uint32_t i = 0; i < n; ++i) {
        t.points.push(t.ring.data[i]);
        t.coverage.push(1);
    }
    for (uint32_t i = 1; i + 1 < n; ++i) {
        uint32_t triangle[3] = {first, first + i, first + i + 1};
        t.indices.append(triangle, 3);
    }
    return true;
}

void tessellate(PathTessellator& t, PrPathFillRule rule, float fringe) {
    Winding winding = rule == PrPathFillRule_EvenOdd ? Winding_EvenOdd : Winding_NonZero;
    if (!(fringe > 0)) {
        sweep(t, winding, true);
        return;
    }
    PathGrid grid = sweep(t, winding, false);
    addBoundaryFringe(t, grid, fringe);
    sweep(t, Winding_Positive, true);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPathTessellator : PathTessellator {};

PrPathTessellator* prPathTessellatorCreate(void) {
    return allocZeroed<PrPathTessellator>();
}

void prPathTessellatorDestroy(PrPathTessellator* PR_NULLABLE tessellator) {
    if (tessellator == nullptr)
        return;
    tessellator->points.reset();
    tessellator->coverage.reset();
    tessellator->indices.reset();
    tessellator->ring.reset();
    tessellator->ends.reset();
    tessellator->gridEdges.reset();
    tessellator->pieces.reset();
    tessellator->vertices.reset();
    tessellator->edges.reset();
    tessellator->starts.reset();
    tessellator->active.reset();
    tessellator->spans.reset();
    for (uint32_t i = 0; i < tessellator->polygons.count; ++i)
        tessellator->polygons.data[i].stack.reset();
    tessellator->polygons.reset();
    tessellator->freePolygons.reset();
    tessellator->starting.reset();
    tessellator->between.reset();
    tessellator->boundary.reset();
    tessellator->loop.reset();
    tessellator->loopPoints.reset();
    prMemoryFree(tessellator);
}

PrPathMesh prPathTessellatorFill(PrPathTessellator* tessellator, const PrPathPolyline* polyline, PrPathFillRule rule,
        float fringe) {
    tessellator->points.clear();
    tessellator->coverage.clear();
    tessellator->indices.clear();
    tessellator->ring.clear();
    tessellator->ends.clear();
    bool finite = isFiniteF32(fringe);
    if (finite && polyline->pointCount != 0) {
        tessellator->ring.append(polyline->points, polyline->pointCount);
        for (uint32_t c = 1; c < polyline->contourCount; ++c)
            tessellator->ends.push(polyline->contours[c]);
        tessellator->ends.push(polyline->pointCount);
        for (uint32_t i = 0; i < tessellator->ring.count && finite; ++i)
            finite = isFiniteF32(tessellator->ring.data[i].x) && isFiniteF32(tessellator->ring.data[i].y);
    }
    float band = fringe > 0 ? fringe : 0;
    if (finite && tessellator->ring.count >= 3 && !fanConvex(*tessellator, band))
        tessellate(*tessellator, rule, band);
    return PrPathMesh{
        tessellator->points.data, tessellator->coverage.data, tessellator->points.count,
        tessellator->indices.data, tessellator->indices.count,
    };
}
