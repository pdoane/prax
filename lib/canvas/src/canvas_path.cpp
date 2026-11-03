// Canvas paths: the path of CanvasPath, which the context's current default path and Path2D share.
//
// html - https://html.spec.whatwg.org/

#include "canvas/src/canvas_path.h"

#include "pr/float.h"
#include "pr/path_data.h"
#include "pr/path_polyline.h"
#include "pr/path_stroke.h"

namespace pr_impl {

namespace {

// The flattening tolerance of hit testing, in the units of the path, and how near an edge a point counts as on it.
constexpr float kHitTolerance = 0.005f;
constexpr double kEdgeDistance = 1e-4;

// Returns a point as the floats that a PrPath holds.
PrVec2 pointOf(PrDVec2 point) {
    return PrVec2{float(point.x), float(point.y)};
}

PrAffine2 affineOf(PrDAffine2 transform) {
    return prAffine2FromDAffine2(transform);
}

// Starts a subpath at `point`, which is in the path's space.
void startAt(CanvasPath* path, PrDVec2 point) {
    prPathMoveTo(path->path, pointOf(point));
    path->start = point;
    path->last = point;
    path->hasSubpath = true;
}

// Ensures there is a subpath for `point` (html/#ensure-there-is-a-subpath), which is in the path's space. A path needs
// a new subpath while it has none.
void ensureSubpath(CanvasPath* path, PrDVec2 point) {
    if (!path->hasSubpath)
        startAt(path, point);
}

void lineAt(CanvasPath* path, PrDVec2 point) {
    prPathLineTo(path->path, pointOf(point));
    path->last = point;
}

void cubicAt(CanvasPath* path, PrDVec2 control1, PrDVec2 control2, PrDVec2 end) {
    prPathCubicTo(path->path, pointOf(control1), pointOf(control2), pointOf(end));
    path->last = end;
}

// An ellipse, whose semi-major axis is inclined by the rotation whose cosine and sine it holds.
struct Ellipse {
    PrDVec2 center;
    PrDVec2 radii;
    double cosRotation;
    double sinRotation;
};

// Returns the point of `ellipse` at `angle` (html/#determine-the-point-on-an-ellipse-steps), and the derivative of that
// point by the angle.
PrDVec2 pointAt(const Ellipse& ellipse, double angle) {
    double x = ellipse.radii.x * prF64Cos(angle);
    double y = ellipse.radii.y * prF64Sin(angle);
    return PrDVec2{
        ellipse.center.x + x * ellipse.cosRotation - y * ellipse.sinRotation,
        ellipse.center.y + x * ellipse.sinRotation + y * ellipse.cosRotation,
    };
}
PrDVec2 tangentAt(const Ellipse& ellipse, double angle) {
    double x = -ellipse.radii.x * prF64Sin(angle);
    double y = ellipse.radii.y * prF64Cos(angle);
    return PrDVec2{
        x * ellipse.cosRotation - y * ellipse.sinRotation,
        x * ellipse.sinRotation + y * ellipse.cosRotation,
    };
}

// Appends the arc of `ellipse` from `startAngle` through `sweep`, mapped through `transform`, as cubic curves of at
// most a quarter turn each. The arc starts a subpath, or is joined by a line to the last point if there is a subpath.
void appendArc(CanvasPath* path, PrDAffine2 transform, const Ellipse& ellipse, double startAngle, double sweep) {
    PrDVec2 first = pointAt(ellipse, startAngle);
    PrDVec2 start = canvasMapPoint(transform, first.x, first.y);
    if (!path->hasSubpath)
        startAt(path, start);
    else if (start.x != path->last.x || start.y != path->last.y)
        lineAt(path, start);
    uint32_t segments = uint32_t(prF64Ceil(prF64Abs(sweep) / (kPrF64Pi / 2) - 1e-9));
    if (segments == 0)
        return;
    double step = sweep / segments;
    double k = 4.0 / 3.0 * prF64Tan(step / 4);
    for (uint32_t i = 0; i < segments; ++i) {
        double from = startAngle + step * i;
        double to = i + 1 == segments ? startAngle + sweep : from + step;
        PrDVec2 p0 = pointAt(ellipse, from);
        PrDVec2 t0 = tangentAt(ellipse, from);
        PrDVec2 p3 = pointAt(ellipse, to);
        PrDVec2 t3 = tangentAt(ellipse, to);
        cubicAt(path, canvasMapPoint(transform, p0.x + k * t0.x, p0.y + k * t0.y),
            canvasMapPoint(transform, p3.x - k * t3.x, p3.y - k * t3.y), canvasMapPoint(transform, p3.x, p3.y));
    }
}

// Returns the sweep of an arc from `*startAngle` to `endAngle` (html/#dom-context-2d-ellipse). The start angle is first
// moved into [0, 2π). A sweep of a whole turn or more in the arc's direction is a whole turn. Otherwise the arc goes
// from the start point to the end point in its direction, so it covers less than a whole turn, and an arc
// counterclockwise from 0 to 2π, which content draws circles with, is a whole turn.
double sweepOf(double* startAngle, double endAngle, bool counterclockwise) {
    double start = prF64Fmod(*startAngle, 2 * kPrF64Pi);
    if (start < 0) {
        start += 2 * kPrF64Pi;
        if (start >= 2 * kPrF64Pi)
            start -= 2 * kPrF64Pi;
    }
    double end = endAngle + (start - *startAngle);
    *startAngle = start;
    if (!counterclockwise && end - start >= 2 * kPrF64Pi)
        return 2 * kPrF64Pi;
    if (counterclockwise && start - end >= 2 * kPrF64Pi)
        return -2 * kPrF64Pi;
    if (!counterclockwise && start > end)
        return 2 * kPrF64Pi - prF64Fmod(start - end, 2 * kPrF64Pi);
    if (counterclockwise && start < end)
        return -(2 * kPrF64Pi - prF64Fmod(end - start, 2 * kPrF64Pi));
    return end - start;
}

// Returns the length of `v`.
double lengthOf(PrDVec2 v) {
    return prF64Sqrt(v.x * v.x + v.y * v.y);
}

// Returns the distance from `point` to the segment from `a` to `b`.
double segmentDistance(PrDVec2 point, PrDVec2 a, PrDVec2 b) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double length = dx * dx + dy * dy;
    double t = length > 0 ? ((point.x - a.x) * dx + (point.y - a.y) * dy) / length : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return lengthOf(PrDVec2{point.x - (a.x + t * dx), point.y - (a.y + t * dy)});
}

// Returns true if `point` is inside the area that `polyline` fills by `rule`, or within kEdgeDistance of an edge. Every
// contour counts as closed.
bool polylineContains(const PrPathPolyline* polyline, PrDVec2 point, PrPathFillRule rule) {
    int32_t winding = 0;
    for (uint32_t c = 0; c < polyline->contourCount; ++c) {
        uint32_t first = polyline->contours[c];
        uint32_t end = prPathPolylineContourEnd(polyline, c);
        for (uint32_t i = first; i < end; ++i) {
            PrVec2 from = polyline->points[i];
            PrVec2 to = polyline->points[i + 1 < end ? i + 1 : first];
            PrDVec2 a{double(from.x), double(from.y)};
            PrDVec2 b{double(to.x), double(to.y)};
            if (segmentDistance(point, a, b) <= kEdgeDistance)
                return true;
            double cross = (b.x - a.x) * (point.y - a.y) - (point.x - a.x) * (b.y - a.y);
            if (a.y <= point.y && b.y > point.y && cross > 0)
                ++winding;
            else if (b.y <= point.y && a.y > point.y && cross < 0)
                --winding;
        }
    }
    return rule == PrPathFillRule_EvenOdd ? (winding & 1) != 0 : winding != 0;
}

} // namespace

void canvasPathInit(CanvasPath* path) {
    *path = CanvasPath{.path = prPathCreate()};
}

void canvasPathReset(CanvasPath* path) {
    prPathDestroy(path->path);
    *path = CanvasPath{};
}

void canvasPathClear(CanvasPath* path) {
    prPathClear(path->path);
    path->start = PrDVec2{};
    path->last = PrDVec2{};
    path->hasSubpath = false;
}

void canvasPathCopy(CanvasPath* out, const CanvasPath& from) {
    prPathDestroy(out->path);
    *out = from;
    out->path = prPathClone(from.path);
}

void canvasPathClose(CanvasPath* path) {
    if (!path->hasSubpath)
        return;
    prPathClose(path->path);
    path->last = path->start;
}

void canvasPathMoveTo(CanvasPath* path, PrDAffine2 transform, double x, double y) {
    if (canvasIsFinite(x, y))
        startAt(path, canvasMapPoint(transform, x, y));
}

void canvasPathLineTo(CanvasPath* path, PrDAffine2 transform, double x, double y) {
    if (!canvasIsFinite(x, y))
        return;
    PrDVec2 point = canvasMapPoint(transform, x, y);
    if (!path->hasSubpath)
        startAt(path, point);
    else
        lineAt(path, point);
}

void canvasPathQuadraticCurveTo(CanvasPath* path, PrDAffine2 transform, double cpx, double cpy, double x, double y) {
    if (!canvasIsFinite(cpx, cpy, x, y))
        return;
    PrDVec2 control = canvasMapPoint(transform, cpx, cpy);
    PrDVec2 end = canvasMapPoint(transform, x, y);
    ensureSubpath(path, control);
    prPathQuadTo(path->path, pointOf(control), pointOf(end));
    path->last = end;
}

void canvasPathBezierCurveTo(CanvasPath* path, PrDAffine2 transform, double cp1x, double cp1y, double cp2x,
        double cp2y, double x, double y) {
    if (!canvasIsFinite(cp1x, cp1y, cp2x, cp2y, x, y))
        return;
    PrDVec2 control1 = canvasMapPoint(transform, cp1x, cp1y);
    ensureSubpath(path, control1);
    cubicAt(path, control1, canvasMapPoint(transform, cp2x, cp2y), canvasMapPoint(transform, x, y));
}

PrStatus canvasPathArcTo(CanvasPath* path, PrDAffine2 transform, double x1, double y1, double x2, double y2, double radius) {
    if (!canvasIsFinite(x1, y1, x2, y2, radius))
        return PrStatus_Ok;
    PrDVec2 corner = canvasMapPoint(transform, x1, y1);
    ensureSubpath(path, corner);
    if (radius < 0)
        return PrStatus_OutOfRange;
    // The last point is mapped back into the space of the arguments. A transform that has no inverse leaves the corner
    // alone, which is where every point of that space lands.
    PrDAffine2 inverse{};
    if (!prDAffine2Invert(transform, &inverse)) {
        lineAt(path, corner);
        return PrStatus_Ok;
    }
    PrDVec2 p0 = canvasMapPoint(inverse, path->last.x, path->last.y);
    PrDVec2 v0{p0.x - x1, p0.y - y1};
    PrDVec2 v2{x2 - x1, y2 - y1};
    double length0 = lengthOf(v0);
    double length2 = lengthOf(v2);
    double cross = v0.x * v2.y - v0.y * v2.x;
    if (length0 == 0 || length2 == 0 || radius == 0 || prF64Abs(cross) <= 1e-12 * length0 * length2) {
        lineAt(path, corner);
        return PrStatus_Ok;
    }
    v0 = PrDVec2{v0.x / length0, v0.y / length0};
    v2 = PrDVec2{v2.x / length2, v2.y / length2};
    // The circle touches both lines at the distance from the corner where its radius meets them at right angles, and
    // its center lies on the bisector of the corner.
    double cosine = v0.x * v2.x + v0.y * v2.y;
    double half = prF64Acos(cosine < -1 ? -1 : cosine > 1 ? 1 : cosine) / 2;
    double tangent = radius / prF64Tan(half);
    PrDVec2 bisector{v0.x + v2.x, v0.y + v2.y};
    double bisectorLength = lengthOf(bisector);
    double centerDistance = radius / prF64Sin(half);
    double reach = centerDistance / bisectorLength;
    PrDVec2 center{x1 + bisector.x * reach, y1 + bisector.y * reach};
    PrDVec2 start{x1 + v0.x * tangent, y1 + v0.y * tangent};
    PrDVec2 end{x1 + v2.x * tangent, y1 + v2.y * tangent};
    double startAngle = prF64Atan2(start.y - center.y, start.x - center.x);
    double sweep = prF64Atan2(end.y - center.y, end.x - center.x) - startAngle;
    if (sweep > kPrF64Pi)
        sweep -= 2 * kPrF64Pi;
    else if (sweep < -kPrF64Pi)
        sweep += 2 * kPrF64Pi;
    appendArc(path, transform, Ellipse{center, PrDVec2{radius, radius}, 1, 0}, startAngle, sweep);
    return PrStatus_Ok;
}

void canvasPathRect(CanvasPath* path, PrDAffine2 transform, double x, double y, double w, double h) {
    if (!canvasIsFinite(x, y, w, h))
        return;
    startAt(path, canvasMapPoint(transform, x, y));
    lineAt(path, canvasMapPoint(transform, x + w, y));
    lineAt(path, canvasMapPoint(transform, x + w, y + h));
    lineAt(path, canvasMapPoint(transform, x, y + h));
    canvasPathClose(path);
    startAt(path, canvasMapPoint(transform, x, y));
}

PrStatus canvasPathRoundRect(CanvasPath* path, PrDAffine2 transform, double x, double y, double w, double h,
        const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount) {
    if (!canvasIsFinite(x, y, w, h))
        return PrStatus_Ok;
    if (radiusCount < 1 || radiusCount > 4)
        return PrStatus_OutOfRange;
    for (uint32_t i = 0; i < radiusCount; ++i) {
        if (!canvasIsFinite(radii[i].x, radii[i].y))
            return PrStatus_Ok;
        if (radii[i].x < 0 || radii[i].y < 0)
            return PrStatus_OutOfRange;
    }
    // The corners as border-radius lists them: upper left, upper right, lower right and lower left.
    constexpr uint8_t kCorners[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    PrDVec2 corners[4];
    for (uint32_t i = 0; i < 4; ++i)
        corners[i] = radii[kCorners[radiusCount - 1][i]];
    // The rectangle is built from its origin with a positive width and height, and mapped back over the rectangle that
    // the arguments give. A negative width mirrors it, which carries the left corners' radii to the right and reverses
    // the direction of the subpath, and a negative height does the same vertically.
    double width = prF64Abs(w);
    double height = prF64Abs(h);
    double sums[4] = {
        corners[0].x + corners[1].x, corners[1].y + corners[2].y, corners[2].x + corners[3].x,
        corners[0].y + corners[3].y,
    };
    double sides[4] = {width, height, width, height};
    double scale = 1;
    for (uint32_t i = 0; i < 4; ++i) {
        if (sums[i] > 0 && sides[i] / sums[i] < scale)
            scale = sides[i] / sums[i];
    }
    for (PrDVec2& corner : corners)
        corner = PrDVec2{corner.x * scale, corner.y * scale};
    PrDAffine2 local{.a = w < 0 ? -1.0 : 1.0, .d = h < 0 ? -1.0 : 1.0, .e = x, .f = y};
    PrDAffine2 mapped = prDAffine2Concat(local, transform);
    // A corner is a quarter of an ellipse around its center, clockwise from `angle`. A corner with a radius of zero is
    // square, and the lines on either side meet at it.
    auto corner = [&](uint32_t index, PrDVec2 center, double angle) {
        if (corners[index].x > 0 && corners[index].y > 0)
            appendArc(path, mapped, Ellipse{center, corners[index], 1, 0}, angle, kPrF64Pi / 2);
    };
    startAt(path, canvasMapPoint(mapped, corners[0].x, 0));
    lineAt(path, canvasMapPoint(mapped, width - corners[1].x, 0));
    corner(1, PrDVec2{width - corners[1].x, corners[1].y}, -kPrF64Pi / 2);
    lineAt(path, canvasMapPoint(mapped, width, height - corners[2].y));
    corner(2, PrDVec2{width - corners[2].x, height - corners[2].y}, 0);
    lineAt(path, canvasMapPoint(mapped, corners[3].x, height));
    corner(3, PrDVec2{corners[3].x, height - corners[3].y}, kPrF64Pi / 2);
    lineAt(path, canvasMapPoint(mapped, 0, corners[0].y));
    corner(0, PrDVec2{corners[0].x, corners[0].y}, kPrF64Pi);
    canvasPathClose(path);
    startAt(path, canvasMapPoint(transform, x, y));
    return PrStatus_Ok;
}

PrStatus canvasPathEllipse(CanvasPath* path, PrDAffine2 transform, double x, double y, double radiusX,
        double radiusY, double rotation, double startAngle, double endAngle, bool counterclockwise) {
    if (!canvasIsFinite(x, y, radiusX, radiusY, rotation, startAngle, endAngle))
        return PrStatus_Ok;
    if (radiusX < 0 || radiusY < 0)
        return PrStatus_OutOfRange;
    double sweep = sweepOf(&startAngle, endAngle, counterclockwise);
    Ellipse ellipse{PrDVec2{x, y}, PrDVec2{radiusX, radiusY}, prF64Cos(rotation), prF64Sin(rotation)};
    appendArc(path, transform, ellipse, startAngle, sweep);
    return PrStatus_Ok;
}

void canvasPathAddPath(CanvasPath* path, const CanvasPath& other, PrDAffine2 transform) {
    if (!other.hasSubpath ||
            !canvasIsFinite(transform.a, transform.b, transform.c, transform.d, transform.e, transform.f))
        return;
    // A path added to itself is read from a copy, since appending grows what it reads.
    PrPath* copy = other.path == path->path ? prPathClone(other.path) : nullptr;
    prPathAddPath(path->path, copy != nullptr ? copy : other.path, affineOf(transform));
    prPathDestroy(copy);
    startAt(path, canvasMapPoint(transform, other.last.x, other.last.y));
}

void canvasPathAppendData(CanvasPath* path, PrStringView data) {
    PrPath* parsed = prPathCreate();
    prPathAppendData(parsed, data);
    const PrPathVerb* verbs = prPathVerbs(parsed);
    const PrVec2* points = prPathPoints(parsed);
    uint32_t verbCount = prPathVerbCount(parsed);
    if (verbCount != 0) {
        // The last point of the data: the end of its last segment, or the start of its last subpath after a close.
        PrVec2 start{};
        PrVec2 last{};
        uint32_t point = 0;
        for (uint32_t i = 0; i < verbCount; ++i) {
            constexpr uint32_t kPoints[] = {1, 1, 2, 3, 0};
            point += kPoints[verbs[i]];
            if (verbs[i] == PrPathVerb_Move)
                start = points[point - 1];
            last = verbs[i] == PrPathVerb_Close ? start : points[point - 1];
        }
        prPathAddPath(path->path, parsed, prAffine2Identity());
        startAt(path, PrDVec2{double(last.x), double(last.y)});
    }
    prPathDestroy(parsed);
}

void canvasPathPrune(const PrPath* path, PrPath* out) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t verbCount = prPathVerbCount(path);
    PrVec2 current{};
    PrVec2 start{};
    bool started = false;       // the subpath has a segment in `out`
    uint32_t point = 0;
    auto same = [](PrVec2 a, PrVec2 b) { return a.x == b.x && a.y == b.y; };
    auto begin = [&]() {
        if (!started)
            prPathMoveTo(out, current);
        started = true;
    };
    for (uint32_t i = 0; i < verbCount; ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                current = start = points[point++];
                started = false;
                break;
            case PrPathVerb_Line: {
                PrVec2 end = points[point++];
                if (!same(end, current)) {
                    begin();
                    prPathLineTo(out, end);
                    current = end;
                }
                break;
            }
            case PrPathVerb_Quad: {
                PrVec2 control = points[point];
                PrVec2 end = points[point + 1];
                point += 2;
                if (!same(control, current) || !same(end, current)) {
                    begin();
                    prPathQuadTo(out, control, end);
                    current = end;
                }
                break;
            }
            case PrPathVerb_Cubic: {
                PrVec2 control1 = points[point];
                PrVec2 control2 = points[point + 1];
                PrVec2 end = points[point + 2];
                point += 3;
                if (!same(control1, current) || !same(control2, current) || !same(end, current)) {
                    begin();
                    prPathCubicTo(out, control1, control2, end);
                    current = end;
                }
                break;
            }
            case PrPathVerb_Close:
                if (started)
                    prPathClose(out);
                started = false;
                current = start;
                break;
            default:
                break;
        }
    }
}

bool canvasPathContains(const PrPath* path, PrVec2 point, PrPathFillRule rule) {
    PrPathPolyline polyline{};
    prPathFlatten(path, prAffine2Identity(), kHitTolerance, PrPathFlattening_Inside, &polyline);
    bool inside = polylineContains(&polyline, PrDVec2{double(point.x), double(point.y)}, rule);
    prPathPolylineReset(&polyline);
    return inside;
}

bool canvasPathStrokeContains(const PrPath* path, const PrPathStroke* stroke, PrVec2 point) {
    PrPath* traced = prPathCreate();
    canvasPathPrune(path, traced);
    PrPathPolyline outline{};
    prPathStrokeOutline(traced, stroke, kHitTolerance, &outline);
    bool inside = polylineContains(&outline, PrDVec2{double(point.x), double(point.y)}, PrPathFillRule_NonZero);
    prPathPolylineReset(&outline);
    prPathDestroy(traced);
    return inside;
}

PrDVec2 canvasMapPoint(PrDAffine2 transform, double x, double y) {
    return PrDVec2{transform.a * x + transform.c * y + transform.e, transform.b * x + transform.d * y + transform.f};
}

} // namespace pr_impl
