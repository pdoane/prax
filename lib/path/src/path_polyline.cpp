// Polylines: contours of lines, which is what paths become once their curves are flattened. Also combining, growing and
// shrinking the regions that polylines fill.
//
// The edges of the polygons of both regions form an arrangement on an integer grid (arrangement.h). A piece is on the
// boundary of the result if the result differs on its two sides. The windings on one side are those crossed by a ray
// from the midpoint of the piece. The other side adds the winding of the piece itself. The kept pieces are oriented so
// that the result lies on one side of each, and are chained into contours.
//
// Offsetting unites the region with every point within the distance of its boundary, or removes those points. That area
// is a band along each edge, plus, at each corner, the area that the join fills in the gap between the bands. When a
// region shrinks, its joins appear at its concave corners.

#include "path/src/path_polyline.h"

#include "pr/array.h"
#include "pr/bitset.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/search.h"
#include "pr/sort.h"

#include "path/src/arrangement.h"

namespace pr_impl {

namespace {

bool finite(const PrPathPolyline& polyline) {
    for (uint32_t i = 0; i < polyline.pointCount; ++i) {
        if (!isFiniteF32(polyline.points[i].x) || !isFiniteF32(polyline.points[i].y))
            return false;
    }
    return true;
}

// Appends the points of the polyline, and the end of each of its contours, in the form in which the arrangement reads
// rings.
void ringsOf(const PrPathPolyline& polyline, Array<PrVec2>& points, Array<uint32_t>& ends) {
    if (polyline.pointCount != 0)
        points.append(polyline.points, polyline.pointCount);
    for (uint32_t c = 0; c < polyline.contourCount; ++c)
        ends.push(prPathPolylineContourEnd(&polyline, c));
}

// A piece of the boundary of the result.
struct Boundary {
    PathGridPoint from;
    PathGridPoint to;
    bool used;
};

// Pieces bucketed by the bands of y, or of x, that they span, for finding the pieces that a ray crosses.
struct Bands {
    int64_t origin;
    int64_t size;
    uint32_t count;
    Array<uint32_t> starts;     // count + 1 offsets into `pieces`
    Array<uint32_t> pieces;
};

void buildBands(const Array<PathGridPiece>& pieces, bool vertical, Bands& bands) {
    int64_t low = 0;
    int64_t high = 0;
    for (uint32_t i = 0; i < pieces.count; ++i) {
        const PathGridPiece& p = pieces.data[i];
        int64_t a = vertical ? p.lo.x : p.lo.y;
        int64_t b = vertical ? p.hi.x : p.hi.y;
        int64_t lo = a < b ? a : b;
        int64_t hi = a > b ? a : b;
        low = i == 0 || lo < low ? lo : low;
        high = i == 0 || hi > high ? hi : high;
    }
    uint32_t count = 1;
    while (uint64_t(count) * count < pieces.count && count < 4096)
        ++count;
    bands.origin = low;
    bands.count = count;
    bands.size = (high - low) / int64_t(count) + 1;
    Array<uint32_t> sizes;
    for (uint32_t i = 0; i <= count; ++i)
        sizes.push(0);
    auto span = [&](const PathGridPiece& p, uint32_t* first, uint32_t* last) {
        int64_t a = vertical ? p.lo.x : p.lo.y;
        int64_t b = vertical ? p.hi.x : p.hi.y;
        int64_t lo = a < b ? a : b;
        int64_t hi = a > b ? a : b;
        *first = uint32_t((lo - bands.origin) / bands.size);
        *last = uint32_t((hi - bands.origin) / bands.size);
    };
    for (uint32_t i = 0; i < pieces.count; ++i) {
        uint32_t first = 0;
        uint32_t last = 0;
        span(pieces.data[i], &first, &last);
        for (uint32_t b = first; b <= last; ++b)
            ++sizes.data[b];
    }
    bands.starts.push(0);
    for (uint32_t b = 0; b < count; ++b)
        bands.starts.push(bands.starts.data[b] + sizes.data[b]);
    bands.pieces.reserve(bands.starts.data[count]);
    bands.pieces.count = bands.starts.data[count];
    for (uint32_t b = 0; b < count; ++b)
        sizes.data[b] = bands.starts.data[b];
    for (uint32_t i = 0; i < pieces.count; ++i) {
        uint32_t first = 0;
        uint32_t last = 0;
        span(pieces.data[i], &first, &last);
        for (uint32_t b = first; b <= last; ++b)
            bands.pieces.data[sizes.data[b]++] = i;
    }
    sizes.reset();
}

// Writes how much each region winds on the side of piece `self` from which a ray from its midpoint exits. The ray runs
// toward +x, or toward +y for a horizontal piece. Coordinates are doubled, so that the midpoint is on the grid.
void windingBeside(const Array<PathGridPiece>& pieces, const Bands& bands, uint32_t self, bool vertical, int32_t* out) {
    const PathGridPiece& s = pieces.data[self];
    PathGridPoint m{s.lo.x + s.hi.x, s.lo.y + s.hi.y};
    int64_t along = vertical ? m.x : m.y;
    int64_t band = (along / 2 - bands.origin) / bands.size;
    band = band < 0 ? 0 : band >= int64_t(bands.count) ? int64_t(bands.count) - 1 : band;
    out[0] = out[1] = 0;
    for (uint32_t k = bands.starts.data[band]; k < bands.starts.data[band + 1]; ++k) {
        uint32_t i = bands.pieces.data[k];
        if (i == self)
            continue;
        const PathGridPiece& p = pieces.data[i];
        PathGridPoint a{2 * p.lo.x, 2 * p.lo.y};
        PathGridPoint b{2 * p.hi.x, 2 * p.hi.y};
        int direction = 0;
        if (!vertical) {
            if ((a.y <= m.y) == (b.y <= m.y))
                continue;
            int64_t side = pathGridOrient(a, b, m);
            bool up = b.y > a.y;
            if (side == 0 || (up ? side < 0 : side > 0))
                continue;
            direction = up ? 1 : -1;
        } else {
            if ((a.x <= m.x) == (b.x <= m.x))
                continue;
            int64_t side = pathGridOrient(a, b, m);
            bool rightward = b.x > a.x;
            if (side == 0 || (rightward ? side > 0 : side < 0))
                continue;
            direction = rightward ? -1 : 1;
        }
        out[0] += direction * p.winding[0];
        out[1] += direction * p.winding[1];
    }
}

bool inside(int32_t winding, PrPathFillRule rule) {
    return rule == PrPathFillRule_EvenOdd ? (winding & 1) != 0 : winding != 0;
}

bool result(PrPathPolylineOp op, bool a, bool b) {
    switch (op) {
        case PrPathPolylineOp_Union:
            return a || b;
        case PrPathPolylineOp_Intersect:
            return a && b;
        case PrPathPolylineOp_Difference:
            return a && !b;
        default:
            return a != b;
    }
}

// Chains the boundary into contours, with a point wherever the direction turns.
void chain(const PathGrid& g, Array<Boundary>& boundary, PrPathPolyline& out) {
    sortItems(boundary.data, boundary.count,
        [](const Boundary& a, const Boundary& b) { return pathGridBeforeX(a.from, b.from); });
    auto next = [&](PathGridPoint from) -> Boundary* {
        uint32_t lo = partitionPoint(0, boundary.count, [&](uint32_t i) { return pathGridBeforeX(boundary.data[i].from, from); });
        for (uint32_t i = lo; i < boundary.count && boundary.data[i].from == from; ++i) {
            if (!boundary.data[i].used)
                return &boundary.data[i];
        }
        return nullptr;
    };
    Array<PathGridPoint> contour;
    for (uint32_t i = 0; i < boundary.count; ++i) {
        if (boundary.data[i].used)
            continue;
        contour.clear();
        Boundary* edge = &boundary.data[i];
        PathGridPoint start = edge->from;
        contour.push(start);
        while (edge != nullptr) {
            edge->used = true;
            PathGridPoint to = edge->to;
            // A point on the line through the two before it is not a corner.
            if (contour.count >= 2 && pathGridOrient(contour.data[contour.count - 2], contour.last(), to) == 0)
                contour.pop();
            if (to == start)
                break;
            contour.push(to);
            edge = next(to);
        }
        if (contour.count >= 3 && pathGridOrient(contour.last(), start, contour.data[1]) == 0) {
            // The start may also be on the line through its neighbors.
            contour.data[0] = contour.last();
            contour.pop();
        }
        if (contour.count < 3)
            continue;
        prPathPolylineMoveTo(&out, pathGridUnsnap(g, contour.data[0]));
        for (uint32_t k = 1; k < contour.count; ++k)
            prPathPolylineLineTo(&out, pathGridUnsnap(g, contour.data[k]));
        prPathPolylineClose(&out);
    }
    contour.reset();
}

// Adds the polygon, winding counterclockwise with y up, so that the windings of overlapping polygons add.
void addPolygon(const PrVec2* points, uint32_t count, PrPathPolyline& out) {
    double twiceArea = 0;
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 a = points[i];
        PrVec2 b = points[(i + 1) % count];
        twiceArea += double(a.x) * double(b.y) - double(b.x) * double(a.y);
    }
    if (twiceArea == 0)
        return;
    bool reverse = twiceArea < 0;
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 p = points[reverse ? count - 1 - i : i];
        if (i == 0)
            prPathPolylineMoveTo(&out, p);
        else
            prPathPolylineLineTo(&out, p);
    }
    prPathPolylineClose(&out);
}

// Adds every point within `distance` of the edges of the rings: a band along each edge, and, at each corner, the area
// that the join fills on the side that the two bands leave open.
void boundaryBand(const Array<PrVec2>& points, const Array<uint32_t>& ends, float distance,
        PrPathJoin join, float miterLimit, float tolerance, PrPathPolyline& out) {
    auto normal = [](PrVec2 a, PrVec2 b) {
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float length = sqrtF32(dx * dx + dy * dy);
        return length > 0 ? PrVec2{-dy / length, dx / length} : PrVec2{0, 0};
    };
    auto along = [](PrVec2 p, PrVec2 n, float d) {
        return p + n * d;
    };
    // A circle approximated as a polygon within the tolerance.
    uint32_t steps = 8;
    while (steps < 256 && distance * (1 - cosF32(kPiF32 / float(steps))) > tolerance)
        steps *= 2;
    uint32_t start = 0;
    for (uint32_t r = 0; r < ends.count; ++r) {
        uint32_t end = ends.data[r];
        uint32_t count = end - start;
        for (uint32_t i = 0; i < count; ++i) {
            PrVec2 a = points.data[start + i];
            PrVec2 b = points.data[start + (i + 1) % count];
            PrVec2 c = points.data[start + (i + 2) % count];
            PrVec2 n1 = normal(a, b);
            if (n1.x == 0 && n1.y == 0)
                continue;
            PrVec2 band[4] = {
                along(a, n1, distance), along(b, n1, distance),
                along(b, n1, -distance), along(a, n1, -distance),
            };
            addPolygon(band, 4, out);
            // The corner at b, toward the next edge with nonzero length.
            PrVec2 n2 = normal(b, c);
            if (n2.x == 0 && n2.y == 0)
                continue;
            float turn = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
            if (turn == 0)
                continue;
            if (join == PrPathJoin_Round || join == PrPathJoin_Arcs) {
                PrVec2 circle[256];
                for (uint32_t k = 0; k < steps; ++k) {
                    float angle = 2 * kPiF32 * float(k) / float(steps);
                    circle[k] = PrVec2{b.x + distance * cosF32(angle), b.y + distance * sinF32(angle)};
                }
                addPolygon(circle, steps, out);
                continue;
            }
            // The open side is the one away from the turn.
            float side = turn > 0 ? -1 : 1;
            PrVec2 p1 = along(b, n1, side * distance);
            PrVec2 p2 = along(b, n2, side * distance);
            PrVec2 wedge[5] = {b, p1, p2, p2, p2};
            uint32_t corners = 3;
            if (join == PrPathJoin_Miter || join == PrPathJoin_MiterClip) {
                // The tip of the miter, at which the outer sides of the two bands meet.
                PrVec2 mid{n1.x + n2.x, n1.y + n2.y};
                float length = sqrtF32(mid.x * mid.x + mid.y * mid.y);
                float cosine = length / 2;            // cosine of half the angle between the normals
                if (cosine > 0) {
                    float reach = distance / cosine;   // distance from b to the tip
                    PrVec2 dir{mid.x / length * side, mid.y / length * side};
                    float limit = miterLimit * distance;
                    if (reach <= limit) {
                        wedge[2] = along(b, dir, reach);
                        wedge[3] = p2;
                        corners = 4;
                    } else if (join == PrPathJoin_MiterClip) {
                        // Cut across the miter at the limit.
                        float t = (limit - distance * cosine) / (reach - distance * cosine);
                        PrVec2 tip = along(b, dir, reach);
                        wedge[2] = p1 + (tip - p1) * t;
                        wedge[3] = p2 + (tip - p2) * t;
                        wedge[4] = p2;
                        corners = 5;
                    }
                }
            }
            addPolygon(wedge, corners, out);
        }
        start = end;
    }
}

// A point of the z = 0 plane mapped by a matrix, before the divide by w.
struct HomogeneousPoint {
    float x;
    float y;
    float w;
};

// Returns `p` on the z = 0 plane, mapped through `matrix`, before the divide by w.
HomogeneousPoint homogeneousLift(const PrMatrix4* matrix, PrVec2 p) {
    const float (*m)[4] = matrix->m;
    return HomogeneousPoint{
        m[0][0] * p.x + m[0][1] * p.y + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][3],
        m[3][0] * p.x + m[3][1] * p.y + m[3][3],
    };
}

// Returns the point `h` divided by its w.
PrVec2 homogeneousDivide(HomogeneousPoint h) {
    return PrVec2{h.x / h.w, h.y / h.w};
}

// Appends to `out` the closed contour `polygon` of the plane, projected through `matrix`. The contour is first clipped
// to the part in which w is at least a small fraction of its largest w. `clipped` is scratch storage.
void polylineAddProjected(PrPathPolyline& out, const PrMatrix4* matrix, const Array<PrVec2>& polygon,
        Array<HomogeneousPoint>& clipped) {
    clipped.clear();
    float largest = 0;
    for (uint32_t i = 0; i < polygon.count; ++i)
        largest = maxF32(largest, homogeneousLift(matrix, polygon.data[i]).w);
    if (!(largest > 0))
        return;
    float near = largest * kPrPathPolylineNearPlane;
    for (uint32_t i = 0; i < polygon.count; ++i) {
        HomogeneousPoint a = homogeneousLift(matrix, polygon.data[i]);
        HomogeneousPoint b = homogeneousLift(matrix, polygon.data[(i + 1) % polygon.count]);
        bool aIn = a.w >= near;
        bool bIn = b.w >= near;
        if (aIn)
            clipped.push(a);
        if (aIn != bIn) {
            float t = (near - a.w) / (b.w - a.w);
            clipped.push(HomogeneousPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, near});
        }
    }
    if (clipped.count < 3)
        return;
    prPathPolylineMoveTo(&out, homogeneousDivide(clipped.data[0]));
    for (uint32_t i = 1; i < clipped.count; ++i)
        prPathPolylineLineTo(&out, homogeneousDivide(clipped.data[i]));
    prPathPolylineClose(&out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prPathPolylineClear(PrPathPolyline* polyline) {
    polyline->pointCount = 0;
    polyline->contourCount = 0;
}

void prPathPolylineReset(PrPathPolyline* polyline) {
    prMemoryFree(polyline->points);
    prMemoryFree(polyline->contours);
    prMemoryFree(polyline->closed);
    *polyline = PrPathPolyline{};
}

void prPathPolylineMoveTo(PrPathPolyline* polyline, PrVec2 point) {
    Array<uint32_t> contours;
    contours.data = polyline->contours;
    contours.count = polyline->contourCount;
    contours.capacity = polyline->contourCapacity;
    contours.push(polyline->pointCount);
    size_t words = bitsetWords(polyline->contourCapacity);
    size_t grown = bitsetWords(contours.capacity);
    if (grown != words) {
        polyline->closed = static_cast<uint64_t*>(prMemoryRealloc(polyline->closed, grown * sizeof(uint64_t)));
        memZero(polyline->closed + words, (grown - words) * sizeof(uint64_t));
    }
    polyline->contours = contours.data;
    polyline->contourCount = contours.count;
    polyline->contourCapacity = contours.capacity;
    bitsetClear(polyline->closed, polyline->contourCount - 1);
    prPathPolylineLineTo(polyline, point);
}

void prPathPolylineLineTo(PrPathPolyline* polyline, PrVec2 point) {
    pathPolylineWrite(*polyline, [&](Array<PrVec2>& points) { points.push(point); });
}

void prPathPolylineClose(PrPathPolyline* polyline) {
    bitsetSet(polyline->closed, polyline->contourCount - 1);
}

void prPathPolylineAddPolyline(PrPathPolyline* polyline, const PrPathPolyline* other, PrAffine2 transform) {
    for (uint32_t c = 0; c < other->contourCount; ++c) {
        uint32_t start = other->contours[c];
        uint32_t end = prPathPolylineContourEnd(other, c);
        if (end == start)
            continue;
        prPathPolylineMoveTo(polyline, prAffine2MapPoint(transform, other->points[start]));
        for (uint32_t i = start + 1; i < end; ++i)
            prPathPolylineLineTo(polyline, prAffine2MapPoint(transform, other->points[i]));
        if (prPathPolylineContourIsClosed(other, c))
            prPathPolylineClose(polyline);
    }
}

void prPathPolylineTransform(PrPathPolyline* polyline, PrAffine2 transform) {
    for (uint32_t i = 0; i < polyline->pointCount; ++i)
        polyline->points[i] = prAffine2MapPoint(transform, polyline->points[i]);
}

PrRect prPathPolylineBounds(const PrPathPolyline* polyline) {
    if (polyline->pointCount == 0)
        return PrRect{0, 0, 0, 0};
    const PrVec2* p = polyline->points;
    PrRect bounds{p[0].x, p[0].y, p[0].x, p[0].y};
    for (uint32_t i = 1; i < polyline->pointCount; ++i) {
        bounds.left = minF32(bounds.left, p[i].x);
        bounds.top = minF32(bounds.top, p[i].y);
        bounds.right = maxF32(bounds.right, p[i].x);
        bounds.bottom = maxF32(bounds.bottom, p[i].y);
    }
    return bounds;
}

void prPathPolylineProject(const PrPathPolyline* polyline, const PrMatrix4* matrix, PrPathPolyline* out) {
    Array<PrVec2> polygon;
    Array<HomogeneousPoint> clipped;
    for (uint32_t c = 0; c < polyline->contourCount; ++c) {
        uint32_t start = polyline->contours[c];
        uint32_t end = prPathPolylineContourEnd(polyline, c);
        if (end - start < 3)
            continue;
        polygon.clear();
        polygon.append(polyline->points + start, end - start);
        polylineAddProjected(*out, matrix, polygon, clipped);
    }
    polygon.reset();
    clipped.reset();
}

void prPathPolylineCombine(const PrPathPolyline* a, PrPathFillRule aRule, const PrPathPolyline* b, PrPathFillRule bRule,
        PrPathPolylineOp op, PrPathPolyline* out) {
    if (!finite(*a) || !finite(*b))
        return;
    Array<PrVec2> points[2];
    Array<uint32_t> ends[2];
    ringsOf(*a, points[0], ends[0]);
    ringsOf(*b, points[1], ends[1]);

    // The grid spans the extent of both regions.
    const Array<PrVec2>* both[2] = {&points[0], &points[1]};
    PathGrid g = pathGridOver(both, 2);
    Array<PathGridEdge> edges;
    pathGridAddEdges(g, points[0], ends[0], 0, edges);
    pathGridAddEdges(g, points[1], ends[1], 1, edges);
    for (uint32_t i = 0; i < 2; ++i) {
        points[i].reset();
        ends[i].reset();
    }
    pathGridSplitEdges(edges);

    Array<PathGridPiece> pieces;
    pathGridMergePieces(edges, pieces);
    edges.reset();
    Bands rows{};
    Bands columns{};
    buildBands(pieces, false, rows);
    buildBands(pieces, true, columns);
    PrPathFillRule rules[2] = {aRule, bRule};
    Array<Boundary> boundary;
    for (uint32_t i = 0; i < pieces.count; ++i) {
        const PathGridPiece& p = pieces.data[i];
        if (p.winding[0] == 0 && p.winding[1] == 0)
            continue;
        bool horizontal = p.lo.y == p.hi.y;
        // The side from which a ray exits, and the other side, across which the piece also counts.
        int32_t beside[2];
        windingBeside(pieces, horizontal ? columns : rows, i, horizontal, beside);
        int direction = horizontal ? -1 : (p.hi.y > p.lo.y ? 1 : -1);
        bool near = result(op, inside(beside[0], rules[0]), inside(beside[1], rules[1]));
        bool far = result(op, inside(beside[0] + direction * p.winding[0], rules[0]),
            inside(beside[1] + direction * p.winding[1], rules[1]));
        if (near == far)
            continue;
        // The result is to the left of each boundary piece, with y up. The side of the ray is left of the piece, run
        // from lo to hi, if the piece runs down or is horizontal.
        bool rayLeft = horizontal || p.hi.y < p.lo.y;
        bool forward = near == rayLeft;
        boundary.push(Boundary{forward ? p.lo : p.hi, forward ? p.hi : p.lo, false});
    }
    pieces.reset();
    rows.starts.reset();
    rows.pieces.reset();
    columns.starts.reset();
    columns.pieces.reset();
    chain(g, boundary, *out);
    boundary.reset();
}

void prPathPolylineOffset(const PrPathPolyline* polyline, PrPathFillRule rule, float distance, PrPathJoin join,
        float miterLimit, float tolerance, PrPathPolyline* out) {
    PrPathPolyline band{};
    if (distance != 0 && isFiniteF32(distance) && tolerance > 0 && isFiniteF32(tolerance)) {
        Array<PrVec2> points;
        Array<uint32_t> ends;
        ringsOf(*polyline, points, ends);
        boundaryBand(points, ends, absF32(distance), join, miterLimit, tolerance, band);
        points.reset();
        ends.reset();
    }
    prPathPolylineCombine(polyline, rule, &band, PrPathFillRule_NonZero,
        distance > 0 ? PrPathPolylineOp_Union : PrPathPolylineOp_Difference, out);
    prPathPolylineReset(&band);
}
