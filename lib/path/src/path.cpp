// Paths: contours of lines and Bézier curves.

#include "pr/path.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/vec.h"

#include "path/src/flatten.h"
#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

// The verbs and points. Clones share them until one of the clones changes.
struct PathStore {
    Atomic<uint32_t> references;
    Atomic<uint64_t> id;            // the id of the contents, assigned when first requested; zero until then
    Array<PrPathVerb> verbs;
    Array<PrVec2> points;
};

struct Path {
    PathStore* store;
    uint32_t contourStart;   // the index of the point at which the last contour started
    bool open;               // true if a contour is open for new segments
};

// The distance of the control points of a cubic along the tangents for a quarter ellipse. It puts the midpoint of the
// cubic on the ellipse.
constexpr float kQuarterArc = 0.5522847498f;

// The maximum distance of the cubics of a shaped corner from its curve, relative to its larger radius.
constexpr float kCornerTolerance = 2e-4f;

// The maximum number of times that the cubics of a shaped corner may be halved.
constexpr uint32_t kCornerDepth = 6;

// The number of points at which a fitted cubic is compared to the curve, and the number of lines that the cubic is
// flattened into for the comparison.
constexpr uint32_t kCornerSamples = 3;
constexpr uint32_t kCornerCubicSteps = 16;

PathStore* createStore() {
    PathStore* store = allocZeroed<PathStore>();
    store->references.storeRelaxed(1);
    return store;
}

void releaseStore(PathStore* store) {
    if (store->references.fetchSub(1) != 1)
        return;
    store->verbs.reset();
    store->points.reset();
    prMemoryFree(store);
}

// The last id given to the contents of a path.
Atomic<uint64_t> lastPathId;

// Returns the store that a change may write: the path's own store, copied first if a clone shares it. The change gives
// the contents a new id.
PathStore* own(Path* path) {
    PathStore* store = path->store;
    if (store->references.load() == 1) {
        store->id.storeRelaxed(0);
        return store;
    }
    PathStore* copy = createStore();
    if (store->verbs.count != 0)
        copy->verbs.append(store->verbs.data, store->verbs.count);
    if (store->points.count != 0)
        copy->points.append(store->points.data, store->points.count);
    releaseStore(store);
    path->store = copy;
    return copy;
}

void addVerb(Path* path, PrPathVerb verb, const PrVec2* points, uint32_t count) {
    PathStore* store = own(path);
    store->verbs.push(verb);
    store->points.append(points, count);
}

// Begins a contour at `point`.
void moveTo(Path* path, PrVec2 point) {
    path->contourStart = path->store->points.count;
    path->open = true;
    addVerb(path, PrPathVerb_Move, &point, 1);
}

// Opens a contour if a segment needs one.
void ensureContour(Path* path) {
    if (path->open)
        return;
    const PathStore* store = path->store;
    moveTo(path, store->points.count != 0 ? store->points.data[path->contourStart] : PrVec2{0, 0});
}

// Appends a quarter of an ellipse from `from` to `to` around the corner at `corner`.
void quarter(PrPath* path, PrVec2 from, PrVec2 corner, PrVec2 to) {
    PrVec2 control1{from.x + (corner.x - from.x) * kQuarterArc,
                     from.y + (corner.y - from.y) * kQuarterArc};
    PrVec2 control2{to.x + (corner.x - to.x) * kQuarterArc,
                     to.y + (corner.y - to.y) * kQuarterArc};
    prPathCubicTo(path, control1, control2, to);
}

float distance(PrVec2 a, PrVec2 b) {
    return sqrtF32((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

PrVec2 placedTangent(const PrCornerPlacement& c, float t) {
    PrVec2 d = prCornerCurveTangent(c.shape, t);
    PrVec2 w{c.before.x * c.radiusBefore * d.x + c.after.x * c.radiusAfter * d.y,
              c.before.y * c.radiusBefore * d.x + c.after.y * c.radiusAfter * d.y};
    float length = sqrtF32(w.x * w.x + w.y * w.y);
    return length > 0 ? w / length : c.before;
}

// Writes to `out` the cubic from the curve at t0 to the curve at t1, along the tangents there. Its handles are those of
// a circular arc that turns through the same angle, but reach no further than where the two tangents meet, so that the
// cubic stays within the triangle that holds the curve. A corner of a large K turns nearly all of its angle at one
// point, where an arc's handles would overshoot it.
void fitCubic(const PrCornerPlacement& c, float t0, float t1, PrVec2 out[4]) {
    PrVec2 p0 = prCornerPlacementPoint(c, prCornerCurvePoint(c.shape, t0));
    PrVec2 p3 = prCornerPlacementPoint(c, prCornerCurvePoint(c.shape, t1));
    PrVec2 d0 = placedTangent(c, t0);
    PrVec2 d1 = placedTangent(c, t1);
    float cosine = clampF32(d0.x * d1.x + d0.y * d1.y, -1, 1);
    float quarter = acosF32(cosine) / 4;
    float handle = distance(p0, p3) / (3 * cosF32(quarter) * cosF32(quarter));
    float handle0 = handle;
    float handle1 = handle;
    // The tangents meet at p0 + d0 a = p3 - d1 b.
    PrVec2 chord{p3.x - p0.x, p3.y - p0.y};
    float cross = d0.x * d1.y - d0.y * d1.x;
    if (absF32(cross) > 1e-6f) {
        float a = (chord.x * d1.y - chord.y * d1.x) / cross;
        float b = (d0.x * chord.y - d0.y * chord.x) / cross;
        if (a >= 0 && b >= 0) {
            handle0 = minF32(handle0, a);
            handle1 = minF32(handle1, b);
        }
    }
    out[0] = p0;
    out[1] = p0 + d0 * handle0;
    out[2] = p3 - d1 * handle1;
    out[3] = p3;
}

// Returns the largest distance from the curve between t0 and t1 to the cubic, sampled at a few points.
float fitError(const PrCornerPlacement& c, float t0, float t1, const PrVec2 cubic[4]) {
    PrVec2 polyline[kCornerCubicSteps + 1];
    for (uint32_t i = 0; i <= kCornerCubicSteps; ++i)
        polyline[i] = pathCubicPoint(cubic, float(i) / float(kCornerCubicSteps));
    float worst = 0;
    for (uint32_t s = 1; s <= kCornerSamples; ++s) {
        float t = t0 + (t1 - t0) * float(s) / float(kCornerSamples + 1);
        PrVec2 p = prCornerPlacementPoint(c, prCornerCurvePoint(c.shape, t));
        float nearest = kF32Max;
        for (uint32_t i = 0; i < kCornerCubicSteps; ++i) {
            PrVec2 a = polyline[i];
            PrVec2 b = polyline[i + 1];
            PrVec2 ab{b.x - a.x, b.y - a.y};
            float lengthSquared = ab.x * ab.x + ab.y * ab.y;
            float along = lengthSquared > 0
                              ? clampF32(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lengthSquared,
                                         0, 1)
                              : 0;
            nearest = minF32(nearest, distance(p, prVec2Lerp(a, b, along)));
        }
        worst = maxF32(worst, nearest);
    }
    return worst;
}

void appendCornerCubics(PrPath* path, const PrCornerPlacement& c, float t0, float t1, float tolerance,
                        uint32_t depth) {
    PrVec2 cubic[4];
    fitCubic(c, t0, t1, cubic);
    if (depth < kCornerDepth && fitError(c, t0, t1, cubic) > tolerance) {
        float middle = (t0 + t1) / 2;
        appendCornerCubics(path, c, t0, middle, tolerance, depth + 1);
        appendCornerCubics(path, c, middle, t1, tolerance, depth + 1);
        return;
    }
    prPathCubicTo(path, cubic[1], cubic[2], cubic[3]);
}

// Appends the curve of the corner from its start, which is the current point of the contour, to its end.
void appendCorner(PrPath* path, const PrCornerPlacement& c) {
    PrVec2 start = prCornerPlacementPoint(c, PrVec2{0, 0});
    PrVec2 corner = c.point;
    PrVec2 end = corner + c.after * c.radiusAfter;
    if (!(c.radiusBefore > 0 && c.radiusAfter > 0)) {
        // A fitted square corner has no radii, so its start, corner and end coincide.
        return;
    } else if (c.shape == kInfinityF32) {
        prPathLineTo(path, corner);
        prPathLineTo(path, end);
    } else if (c.shape == -kInfinityF32) {
        prPathLineTo(path, prCornerPlacementPoint(c, PrVec2{0, 1}));
        prPathLineTo(path, end);
    } else if (c.shape == 0) {
        prPathLineTo(path, end);
    } else if (c.shape == 1) {
        quarter(path, start, corner, end);
    } else {
        float tolerance = maxF32(c.radiusBefore, c.radiusAfter) * kCornerTolerance;
        appendCornerCubics(path, c, 0, 0.5f, tolerance, 1);
        appendCornerCubics(path, c, 0.5f, 1, tolerance, 1);
    }
}

// Appends the arc of the unit circle from `a0` through `sweep`, which is at most a quarter turn, as a cubic mapped by
// `transform`.
void arcCubic(PrPath* path, PrAffine2 transform, float a0, float sweep) {
    float a1 = a0 + sweep;
    float k = 4.0f / 3.0f * tanF32(sweep / 4);
    PrVec2 p0{cosF32(a0), sinF32(a0)};
    PrVec2 p3{cosF32(a1), sinF32(a1)};
    PrVec2 p1{p0.x - k * p0.y, p0.y + k * p0.x};
    PrVec2 p2{p3.x + k * p3.y, p3.y - k * p3.x};
    prPathCubicTo(path, prAffine2MapPoint(transform, p1), prAffine2MapPoint(transform, p2),
                  prAffine2MapPoint(transform, p3));
}

void include(PrRect& bounds, PrVec2 p) {
    bounds.left = minF32(bounds.left, p.x);
    bounds.top = minF32(bounds.top, p.y);
    bounds.right = maxF32(bounds.right, p.x);
    bounds.bottom = maxF32(bounds.bottom, p.y);
}

// Includes in `bounds` the extremes of the quadratic on each axis, at the parameters inside (0, 1) at which its
// derivative is zero.
void includeQuadExtremes(PrRect& bounds, PrVec2 p0, PrVec2 p1, PrVec2 p2) {
    float ts[2];
    uint32_t count = 0;
    float dx = p0.x - 2 * p1.x + p2.x;
    float dy = p0.y - 2 * p1.y + p2.y;
    if (dx != 0)
        ts[count++] = (p0.x - p1.x) / dx;
    if (dy != 0)
        ts[count++] = (p0.y - p1.y) / dy;
    for (uint32_t i = 0; i < count; ++i) {
        float t = ts[i];
        if (t > 0 && t < 1) {
            float u = 1 - t;
            include(bounds, PrVec2{u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x,
                                    u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y});
        }
    }
}

// Writes to `out` the parameters at which one coordinate of a cubic with these control values turns, and returns their
// number. The parameters may lie outside (0, 1), and the caller discards those.
uint32_t cubicTurns(float a, float b, float c, float d, float out[2]) {
    // The derivative over three: (b - a)(1 - t)^2 + 2 (c - b)(1 - t) t + (d - c) t^2.
    float qa = (b - a) - 2 * (c - b) + (d - c);
    float qb = 2 * ((c - b) - (b - a));
    float qc = b - a;
    uint32_t count = 0;
    if (absF32(qa) < 1e-12f) {
        if (qb != 0)
            out[count++] = -qc / qb;
    } else {
        float discriminant = qb * qb - 4 * qa * qc;
        if (discriminant >= 0) {
            float root = sqrtF32(discriminant);
            out[count++] = (-qb + root) / (2 * qa);
            out[count++] = (-qb - root) / (2 * qa);
        }
    }
    return count;
}

void includeCubicExtremes(PrRect& bounds, const PrVec2 p[4]) {
    float ts[4];
    uint32_t count = cubicTurns(p[0].x, p[1].x, p[2].x, p[3].x, ts);
    count += cubicTurns(p[0].y, p[1].y, p[2].y, p[3].y, ts + count);
    for (uint32_t i = 0; i < count; ++i) {
        if (ts[i] > 0 && ts[i] < 1)
            include(bounds, pathCubicPoint(p, ts[i]));
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPath : Path {};

PrPath* prPathCreate(void) {
    PrPath* path = allocZeroed<PrPath>();
    path->store = createStore();
    return path;
}

PrPath* prPathClone(const PrPath* path) {
    PrPath* copy = alloc<PrPath>();
    *copy = *path;
    copy->store->references.fetchAdd(1);
    return copy;
}

void prPathDestroy(PrPath* PR_NULLABLE path) {
    if (path == nullptr)
        return;
    releaseStore(path->store);
    prMemoryFree(path);
}

void prPathClear(PrPath* path) {
    if (path->store->references.load() == 1) {
        path->store->id.storeRelaxed(0);
        path->store->verbs.clear();
        path->store->points.clear();
    } else {
        releaseStore(path->store);
        path->store = createStore();
    }
    path->contourStart = 0;
    path->open = false;
}

void prPathMoveTo(PrPath* path, PrVec2 point) {
    moveTo(path, point);
}

void prPathLineTo(PrPath* path, PrVec2 end) {
    ensureContour(path);
    addVerb(path, PrPathVerb_Line, &end, 1);
}

void prPathQuadTo(PrPath* path, PrVec2 control, PrVec2 end) {
    ensureContour(path);
    PrVec2 points[2] = {control, end};
    addVerb(path, PrPathVerb_Quad, points, 2);
}

void prPathCubicTo(PrPath* path, PrVec2 control1, PrVec2 control2, PrVec2 end) {
    ensureContour(path);
    PrVec2 points[3] = {control1, control2, end};
    addVerb(path, PrPathVerb_Cubic, points, 3);
}

void prPathClose(PrPath* path) {
    if (!path->open)
        return;
    own(path)->verbs.push(PrPathVerb_Close);
    path->open = false;
}

void prPathEllipticArcTo(PrPath* path, PrVec2 center, PrVec2 radii, float rotation, float startAngle, float sweep) {
    PrAffine2 transform = prAffine2Concat(
        prAffine2Concat(prAffine2Scale(radii), prAffine2Rotate(rotation)),
        prAffine2Translate(center));
    PrVec2 start = prAffine2MapPoint(transform, PrVec2{cosF32(startAngle), sinF32(startAngle)});
    if (path->open)
        prPathLineTo(path, start);
    else
        prPathMoveTo(path, start);
    if (!isFiniteF32(sweep) || sweep == 0)
        return;
    uint32_t count = uint32_t(ceilF32(absF32(sweep) / (kPiF32 / 2) - 1e-4f));
    count = count < 1 ? 1 : count;
    float step = sweep / float(count);
    for (uint32_t i = 0; i < count; ++i)
        arcCubic(path, transform, startAngle + step * float(i), step);
}

void prPathAddRect(PrPath* path, PrRect rect) {
    if (prRectIsEmpty(rect))
        return;
    prPathMoveTo(path, PrVec2{rect.left, rect.top});
    prPathLineTo(path, PrVec2{rect.right, rect.top});
    prPathLineTo(path, PrVec2{rect.right, rect.bottom});
    prPathLineTo(path, PrVec2{rect.left, rect.bottom});
    prPathClose(path);
}

void prPathAddCornerRect(PrPath* path, PrCornerRect shape) {
    if (prRectIsEmpty(shape.rect))
        return;
    PrCornerRect fittedShape = prCornerRectFit(shape);
    const PrRect& b = shape.rect;
    const PrCorner& tl = fittedShape.corners[PrRectCorner_TopLeft];
    PrCornerPlacement placed[4];
    for (uint32_t i = 0; i < 4; ++i)
        placed[i] = prCornerRectPlacement(fittedShape, PrRectCorner(i));
    prPathMoveTo(path, PrVec2{b.left + tl.radii.x, b.top});
    for (uint32_t i = 1; i <= 4; ++i) {
        const PrCornerPlacement& c = placed[i % 4];
        prPathLineTo(path, prCornerPlacementPoint(c, PrVec2{0, 0}));
        appendCorner(path, c);
    }
    prPathClose(path);
}

void prPathAddEllipse(PrPath* path, PrVec2 center, PrVec2 radii, float rotation) {
    if (!(radii.x >= 0) || !(radii.y >= 0) || !isFiniteF32(radii.x) || !isFiniteF32(radii.y))
        return;
    PrAffine2 transform = prAffine2Concat(
        prAffine2Concat(prAffine2Scale(radii), prAffine2Rotate(rotation)),
        prAffine2Translate(center));
    // The quarters of the unit circle, clockwise from +x, mapped onto the ellipse.
    const PrVec2 unit[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    PrVec2 points[4];
    for (uint32_t i = 0; i < 4; ++i)
        points[i] = prAffine2MapPoint(transform, unit[i]);
    prPathMoveTo(path, points[0]);
    for (uint32_t i = 0; i < 4; ++i) {
        PrVec2 from = unit[i];
        PrVec2 to = unit[(i + 1) % 4];
        PrVec2 corner = prAffine2MapPoint(transform, from + to);
        quarter(path, points[i], corner, points[(i + 1) % 4]);
    }
    prPathClose(path);
}

void prPathAddSector(PrPath* path, PrVec2 center, float innerRadius, float outerRadius,
                     float startAngle, float sweep) {
    if (!(outerRadius > 0) || !(sweep != 0))
        return;
    float inner = innerRadius > 0 ? minF32(innerRadius, outerRadius) : 0;
    PrVec2 outerRadii{outerRadius, outerRadius};
    PrVec2 innerRadii{inner, inner};
    if (absF32(sweep) >= 2 * kPiF32) {
        float turn = sweep > 0 ? 2 * kPiF32 : -2 * kPiF32;
        path->open = false;
        prPathEllipticArcTo(path, center, outerRadii, 0, startAngle, turn);
        prPathClose(path);
        if (inner > 0) {
            prPathEllipticArcTo(path, center, innerRadii, 0, startAngle + turn, -turn);
            prPathClose(path);
        }
        return;
    }
    path->open = false;
    prPathEllipticArcTo(path, center, outerRadii, 0, startAngle, sweep);
    if (inner > 0)
        prPathEllipticArcTo(path, center, innerRadii, 0, startAngle + sweep, -sweep);
    else
        prPathLineTo(path, center);
    prPathClose(path);
}

void prPathAddCapsule(PrPath* path, PrVec2 start, PrVec2 end, float radius) {
    if (!(radius > 0))
        return;
    PrVec2 delta{end.x - start.x, end.y - start.y};
    float angle = delta.x == 0 && delta.y == 0 ? 0 : atan2F32(delta.y, delta.x);
    PrVec2 radii{radius, radius};
    path->open = false;
    prPathEllipticArcTo(path, end, radii, 0, angle - kPiF32 / 2, kPiF32);
    prPathEllipticArcTo(path, start, radii, 0, angle + kPiF32 / 2, kPiF32);
    prPathClose(path);
}

void prPathAddPath(PrPath* path, const PrPath* other, PrAffine2 transform) {
    const PathStore* source = other->store;
    uint32_t verbCount = source->verbs.count;
    if (verbCount == 0)
        return;
    // Copy first, since `other` may be `path` or share its store.
    Array<PrPathVerb> verbs;
    Array<PrVec2> points;
    verbs.append(source->verbs.data, verbCount);
    points.append(source->points.data, source->points.count);
    uint32_t next = 0;
    path->open = false;
    for (uint32_t i = 0; i < verbCount; ++i) {
        switch (verbs.data[i]) {
            case PrPathVerb_Move:
                prPathMoveTo(path, prAffine2MapPoint(transform, points.data[next++]));
                break;
            case PrPathVerb_Line:
                prPathLineTo(path, prAffine2MapPoint(transform, points.data[next++]));
                break;
            case PrPathVerb_Quad:
                prPathQuadTo(path, prAffine2MapPoint(transform, points.data[next]),
                             prAffine2MapPoint(transform, points.data[next + 1]));
                next += 2;
                break;
            case PrPathVerb_Cubic:
                prPathCubicTo(path, prAffine2MapPoint(transform, points.data[next]),
                              prAffine2MapPoint(transform, points.data[next + 1]),
                              prAffine2MapPoint(transform, points.data[next + 2]));
                next += 3;
                break;
            case PrPathVerb_Close:
                prPathClose(path);
                break;
        }
    }
    verbs.reset();
    points.reset();
}

void prPathAddPolyline(PrPath* path, const PrPathPolyline* polyline, PrAffine2 transform) {
    for (uint32_t c = 0; c < polyline->contourCount; ++c) {
        uint32_t start = polyline->contours[c];
        uint32_t end = prPathPolylineContourEnd(polyline, c);
        if (end == start)
            continue;
        prPathMoveTo(path, prAffine2MapPoint(transform, polyline->points[start]));
        for (uint32_t i = start + 1; i < end; ++i)
            prPathLineTo(path, prAffine2MapPoint(transform, polyline->points[i]));
        if (prPathPolylineContourIsClosed(polyline, c))
            prPathClose(path);
    }
}

void prPathTransform(PrPath* path, PrAffine2 transform) {
    PathStore* store = own(path);
    for (uint32_t i = 0; i < store->points.count; ++i)
        store->points.data[i] = prAffine2MapPoint(transform, store->points.data[i]);
}

uint32_t prPathVerbCount(const PrPath* path) {
    return path->store->verbs.count;
}

const PrPathVerb* PR_NULLABLE prPathVerbs(const PrPath* path) {
    return path->store->verbs.data;
}

uint32_t prPathPointCount(const PrPath* path) {
    return path->store->points.count;
}

const PrVec2* PR_NULLABLE prPathPoints(const PrPath* path) {
    return path->store->points.data;
}

uint64_t prPathId(const PrPath* path) {
    PathStore* store = path->store;
    uint64_t id = store->id.load();
    if (id != 0)
        return id;
    // If paths that share the store request the id at the same time, they agree on the first id stored.
    uint64_t made = lastPathId.fetchAdd(1) + 1;
    return store->id.cas(id, made) ? made : id;
}

PrRect prPathBounds(const PrPath* path) {
    const PathStore* store = path->store;
    if (store->points.count == 0)
        return PrRect{0, 0, 0, 0};
    const PrVec2* points = store->points.data;
    PrRect bounds{points[0].x, points[0].y, points[0].x, points[0].y};
    for (uint32_t i = 1; i < store->points.count; ++i) {
        bounds.left = points[i].x < bounds.left ? points[i].x : bounds.left;
        bounds.top = points[i].y < bounds.top ? points[i].y : bounds.top;
        bounds.right = points[i].x > bounds.right ? points[i].x : bounds.right;
        bounds.bottom = points[i].y > bounds.bottom ? points[i].y : bounds.bottom;
    }
    return bounds;
}

PrRect prPathTightBounds(const PrPath* path) {
    const PathStore* store = path->store;
    if (store->points.count == 0)
        return PrRect{0, 0, 0, 0};
    const PrPathVerb* verbs = store->verbs.data;
    const PrVec2* points = store->points.data;
    PrRect bounds{points[0].x, points[0].y, points[0].x, points[0].y};
    PrVec2 current = points[0];
    uint32_t next = 0;
    for (uint32_t i = 0; i < store->verbs.count; ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
            case PrPathVerb_Line:
                current = points[next++];
                include(bounds, current);
                break;
            case PrPathVerb_Quad:
                include(bounds, points[next + 1]);
                includeQuadExtremes(bounds, current, points[next], points[next + 1]);
                current = points[next + 1];
                next += 2;
                break;
            case PrPathVerb_Cubic: {
                PrVec2 cubic[4] = {current, points[next], points[next + 1], points[next + 2]};
                include(bounds, cubic[3]);
                includeCubicExtremes(bounds, cubic);
                current = cubic[3];
                next += 3;
                break;
            }
            default:
                break;
        }
    }
    return bounds;
}

bool prPathContains(const PrPath* path, PrVec2 point, PrPathFillRule rule) {
    PrRect bounds = prPathBounds(path);
    if (!(point.x >= bounds.left && point.x <= bounds.right && point.y >= bounds.top && point.y <= bounds.bottom))
        return false;
    PrPathPolyline lines{};
    prPathFlatten(path, prAffine2Identity(), 0.01f, PrPathFlattening_Inside, &lines);
    // The winding number of the contours around the point: each edge that crosses the horizontal line through the point
    // to its right counts +1 upward and -1 downward.
    int32_t winding = 0;
    for (uint32_t c = 0; c < lines.contourCount; ++c) {
        uint32_t start = lines.contours[c];
        uint32_t end = prPathPolylineContourEnd(&lines, c);
        for (uint32_t i = start; i < end; ++i) {
            PrVec2 a = lines.points[i];
            PrVec2 b = lines.points[i + 1 < end ? i + 1 : start];
            if ((a.y <= point.y) == (b.y <= point.y))
                continue;
            float x = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x > point.x)
                winding += b.y > a.y ? -1 : 1;
        }
    }
    prPathPolylineReset(&lines);
    return rule == PrPathFillRule_EvenOdd ? (winding & 1) != 0 : winding != 0;
}

void prPathFlatten(const PrPath* path, PrAffine2 transform, float tolerance, PrPathFlattening flattening,
                   PrPathPolyline* out) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* p = prPathPoints(path);
    uint32_t at = 0;
    PrVec2 last{0, 0};
    auto map = [&](uint32_t i) { return prAffine2MapPoint(transform, p[i]); };
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                last = map(at++);
                prPathPolylineMoveTo(out, last);
                break;
            case PrPathVerb_Line:
                last = map(at++);
                prPathPolylineLineTo(out, last);
                break;
            case PrPathVerb_Quad: {
                PrVec2 control = map(at);
                PrVec2 end = map(at + 1);
                pathPolylineWrite(*out, [&](Array<PrVec2>& points) {
                    if (flattening == PrPathFlattening_Balanced)
                        pathFlattenQuadArea(last, control, end, tolerance, points);
                    else
                        pathFlattenQuad(last, control, end, tolerance, points);
                });
                last = end;
                at += 2;
                break;
            }
            case PrPathVerb_Cubic: {
                PrVec2 control1 = map(at);
                PrVec2 control2 = map(at + 1);
                PrVec2 end = map(at + 2);
                pathPolylineWrite(*out, [&](Array<PrVec2>& points) {
                    if (flattening == PrPathFlattening_Balanced)
                        pathFlattenCubicArea(last, control1, control2, end, tolerance, points);
                    else
                        pathFlattenCubic(last, control1, control2, end, tolerance, points);
                });
                last = end;
                at += 3;
                break;
            }
            case PrPathVerb_Close:
                prPathPolylineClose(out);
                break;
            default:
                break;
        }
    }
}
