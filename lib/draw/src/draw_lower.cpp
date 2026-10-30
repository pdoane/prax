// Lowering: draw list primitives broken into simpler parts.
//
// A border's contours are the outer shape inset by fractions of each side's width. If sides differ, each side's bands
// are polygons that pair points of an outer contour with points of an inner one. A band starts at the point where the
// line between the corners of the outer and inner rectangles crosses one corner's curve. It ends at the point where the
// next such line crosses the next corner's curve. Neighboring sides therefore share their dividing edges.

#include "pr/draw_lower.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// Thresholds in each space's units. A polar color whose chroma or saturation is below the threshold has a powerless
// hue.
constexpr double kPowerlessLch = 1e-2;
constexpr double kPowerlessOklch = 4e-5;
constexpr double kPowerlessJzczhz = 0.0000026;
constexpr double kPowerlessHsl = 1e-3;

// ---------------------------------------------------------------------------------------------------------------------
// Shapes and strokes

void addPolyline(PrPath* out, const PrVec2* points, uint32_t count, bool closed) {
    prPathMoveTo(out, points[0]);
    for (uint32_t i = 1; i < count; ++i)
        prPathLineTo(out, points[i]);
    if (count == 1)
        prPathLineTo(out, points[0]);
    if (closed)
        prPathClose(out);
}

// Appends each contour of `path` to `closed` or `open`, depending on whether the contour is closed.
void splitContours(const PrPath* path, PrPath* closed, PrPath* open) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t count = prPathVerbCount(path);
    uint32_t verb = 0;
    uint32_t point = 0;
    while (verb < count) {
        uint32_t end = verb + 1;
        while (end < count && verbs[end] != PrPathVerb_Move)
            ++end;
        PrPath* into = verbs[end - 1] == PrPathVerb_Close ? closed : open;
        for (uint32_t i = verb; i < end; ++i) {
            switch (verbs[i]) {
                case PrPathVerb_Move:
                    prPathMoveTo(into, points[point++]);
                    break;
                case PrPathVerb_Line:
                    prPathLineTo(into, points[point++]);
                    break;
                case PrPathVerb_Quad:
                    prPathQuadTo(into, points[point], points[point + 1]);
                    point += 2;
                    break;
                case PrPathVerb_Cubic:
                    prPathCubicTo(into, points[point], points[point + 1], points[point + 2]);
                    point += 3;
                    break;
                case PrPathVerb_Close:
                    prPathClose(into);
                    break;
            }
        }
        verb = end;
    }
}

// Strokes `geometry` at `widthFactor` times the stroke's width into target space, and appends the outline to `out`.
void strokeInto(const PrDrawStroke* stroke, const PrPath* geometry, PrAffine2 transform,
        float tolerance, float widthFactor, PrPathPolyline* out) {
    if (prPathVerbCount(geometry) == 0)
        return;
    PrPathStroke shape = stroke->shape;
    bool device = stroke->nonScaling;
    shape.width *= widthFactor;
    if (device || prAffine2IsSimilarity(transform)) {
        // Strokes in device space, with lengths scaled by the transform's scale, and the direction of a contour of zero
        // length mapped with the geometry.
        PrPath* mapped = prPathCreate();
        prPathAddPath(mapped, geometry, transform);
        PrVec2 direction = shape.zeroLengthDirection;
        shape.zeroLengthDirection = PrVec2{
            transform.a * direction.x + transform.c * direction.y,
            transform.b * direction.x + transform.d * direction.y,
        };
        Array<float> dashes;
        if (!device) {
            float scale = prAffine2MaxScale(transform);
            shape.width *= scale;
            shape.dashOffset *= scale;
            for (uint32_t i = 0; i < shape.dashCount; ++i)
                dashes.push(shape.dashes[i] * scale);
            shape.dashes = dashes.data;
        }
        prPathStrokeOutline(mapped, &shape, tolerance, out);
        dashes.reset();
        prPathDestroy(mapped);
        return;
    }
    float scale = prAffine2MaxScale(transform);
    if (!(scale > 0))
        return;
    uint32_t first = out->pointCount;
    prPathStrokeOutline(geometry, &shape, tolerance / scale, out);
    for (uint32_t i = first; i < out->pointCount; ++i)
        out->points[i] = prAffine2MapPoint(transform, out->points[i]);
}

// Implements prDrawLowerStroke for a stroke of positive width.
void lowerStroke(const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform, float tolerance,
        PrPathPolyline* fill, PrPathPolyline* aligned, PrPathPolyline* region) {
    if (stroke->align == PrDrawStrokeAlign_Center) {
        strokeInto(stroke, path, transform, tolerance, 1, fill);
        return;
    }
    PrPath* closed = prPathCreate();
    PrPath* open = prPathCreate();
    splitContours(path, closed, open);
    strokeInto(stroke, closed, transform, tolerance, 2, aligned);
    strokeInto(stroke, open, transform, tolerance, 1, fill);
    prPathFlatten(closed, transform, tolerance, PrPathFlattening_Inside, region);
    prPathDestroy(closed);
    prPathDestroy(open);
}

// ---------------------------------------------------------------------------------------------------------------------
// Gradients

float dot(PrVec2 a, PrVec2 b) {
    return a.x * b.x + a.y * b.y;
}

// Returns true if a color's hue is powerless, such as for a gray.
bool huePowerless(PrColorSpace space, const double c[3]) {
    switch (space) {
        case PrColorSpace_Lch:
            return c[1] < kPowerlessLch;
        case PrColorSpace_Oklch:
            return c[1] < kPowerlessOklch;
        case PrColorSpace_Jzczhz:
            return c[1] <= kPowerlessJzczhz;
        case PrColorSpace_Hsl:
            return c[1] < kPowerlessHsl || c[2] <= 0 || c[2] >= 100;
        case PrColorSpace_Hwb:
            return c[1] + c[2] >= 100 - kPowerlessHsl;
        default:
            return false;
    }
}

// A stop in the interpolation space. Its channels other than hue are premultiplied, unless the ramp mixes straight.
struct SpaceStop {
    float offset;
    float hint;
    double channels[3];
    double alpha;
    bool powerless;
};

struct Ramp {
    const PrDrawBrush* brush;
    PrColorSpace space;
    uint32_t hue;
    bool straight;              // stops interpolate color and alpha separately
    Array<SpaceStop> stops;
};

void prepareStops(Ramp& ramp) {
    const PrDrawBrush* brush = ramp.brush;
    float previous = -kInfinityF32;
    for (uint32_t i = 0; i < brush->stopCount; ++i) {
        const PrDrawColorStop& stop = brush->stops[i];
        SpaceStop s{};
        s.offset = maxF32(stop.offset, previous);
        previous = s.offset;
        s.hint = stop.hint;
        PrColor color = stop.color;
        double rgb[3] = {double(color.r), double(color.g), double(color.b)};
        prColorConvert(PrColorSpace_Srgb, ramp.space, rgb, s.channels);
        s.alpha = double(clampF32(color.a, 0, 1));
        s.powerless = huePowerless(ramp.space, s.channels);
        if (stop.hasHue && ramp.hue < 3) {
            s.channels[ramp.hue] = double(stop.hue);
            s.powerless = false;
        }
        for (uint32_t c = 0; c < 3 && !ramp.straight; ++c) {
            if (c != ramp.hue)
                s.channels[c] *= s.alpha;
        }
        ramp.stops.push(s);
    }
}

// Adjusts the two hues so that interpolating between them goes in the requested direction.
void fixHues(PrColorHueMethod mode, double* h1, double* h2) {
    double a = *h1 - 360 * floorF64(*h1 / 360);
    double b = *h2 - 360 * floorF64(*h2 / 360);
    double d = b - a;
    switch (mode) {
        case PrColorHueMethod_Longer:
            if (d > 0 && d < 180)
                a += 360;
            else if (d > -180 && d <= 0)
                b += 360;
            break;
        case PrColorHueMethod_Increasing:
            if (b < a)
                b += 360;
            break;
        case PrColorHueMethod_Decreasing:
            if (a < b)
                a += 360;
            break;
        default:
            if (d > 180)
                a += 360;
            else if (d < -180)
                b += 360;
            break;
    }
    *h1 = a;
    *h2 = b;
}

// Computes the color a fraction `f` of the way from stop `a` to stop `b`, in the interpolation space, premultiplied as
// the stops are, and its alpha. A hint moves the point at which the two colors mix evenly.
void mixStops(const Ramp& ramp, const SpaceStop& a, const SpaceStop& b, float f, double out[3], double* alpha) {
    if (a.hint > 0 && a.hint != 0.5f && f > 0)
        f = powF32(f, logF32(0.5f) / logF32(a.hint));
    double h1 = 0;
    double h2 = 0;
    if (ramp.hue < 3) {
        // A powerless hue is missing, and takes the hue of the stop that it mixes with.
        h1 = a.channels[ramp.hue];
        h2 = b.channels[ramp.hue];
        if (a.powerless && !b.powerless)
            h1 = h2;
        else if (b.powerless && !a.powerless)
            h2 = h1;
        fixHues(ramp.brush->hueMethod, &h1, &h2);
    }
    for (uint32_t c = 0; c < 3; ++c) {
        double from = c == ramp.hue ? h1 : a.channels[c];
        double to = c == ramp.hue ? h2 : b.channels[c];
        out[c] = from + (to - from) * double(f);
    }
    *alpha = a.alpha + (b.alpha - a.alpha) * double(f);
}

// A piece of a ramp, and the stops that its colors mix: one stop for a piece of one color.
struct RampSpan {
    float start;
    float end;
    uint32_t a;
    uint32_t b;
    bool flat;                  // the piece has one color
    bool linear;                // the piece's colors mix linearly in the ramp's output space
};

// Returns true if two stops have the same color in the interpolation space.
bool sameStop(const SpaceStop& a, const SpaceStop& b) {
    return a.alpha == b.alpha && a.powerless == b.powerless && a.channels[0] == b.channels[0] &&
        a.channels[1] == b.channels[1] && a.channels[2] == b.channels[2];
}

// Calls `f` with each piece of the ramp, whose colors are in `space`. The pieces cover the parameter from -1 to 2: the
// first stop's color before 0, then from 0 to 1 one color before the first stop, a piece between each pair of stops
// that are apart, and one color after the last, then the last stop's color past 1.
template <typename F>
void eachSpan(const Ramp& ramp, PrColorSpace space, F f) {
    const Array<SpaceStop>& s = ramp.stops;
    if (s.count == 0) {
        f(RampSpan{.start = -1, .end = 2, .flat = true});
        return;
    }
    f(RampSpan{.start = -1, .end = 0, .flat = true});
    if (s.data[0].offset > 0)
        f(RampSpan{.start = 0, .end = minF32(s.data[0].offset, 1), .flat = true});
    for (uint32_t i = 0; i + 1 < s.count; ++i) {
        const SpaceStop& a = s.data[i];
        const SpaceStop& b = s.data[i + 1];
        float start = maxF32(a.offset, 0);
        float end = minF32(b.offset, 1);
        if (!(end > start))
            continue;
        bool flat = sameStop(a, b);
        // A ramp that mixes straight is linear in the premultiplied output only where its alpha or its color is
        // constant.
        bool sameColor =
            a.channels[0] == b.channels[0] && a.channels[1] == b.channels[1] && a.channels[2] == b.channels[2];
        bool linear = ramp.space == space && ramp.hue >= 3 && (a.hint <= 0 || a.hint == 0.5f) &&
            (!ramp.straight || a.alpha == b.alpha || sameColor);
        f(RampSpan{.start = start, .end = end, .a = i, .b = i + 1, .flat = flat, .linear = linear});
    }
    if (s.last().offset < 1)
        f(RampSpan{.start = maxF32(s.last().offset, 0), .end = 1, .a = s.count - 1, .b = s.count - 1, .flat = true});
    f(RampSpan{.start = 1, .end = 2, .a = s.count - 1, .b = s.count - 1, .flat = true});
}

// Prepares the ramp of the brush's stops in its interpolation space.
void prepareRamp(const PrDrawBrush* brush, Ramp& ramp) {
    ramp.brush = brush;
    bool known = prColorSpaceModel(brush->interpolationSpace) != PrColorModel_Count;
    ramp.space = known ? brush->interpolationSpace : PrColorSpace_Srgb;
    ramp.hue = prColorSpaceHueChannel(ramp.space);
    ramp.straight = brush->interpolationAlpha == PrDrawAlpha_Straight;
    prepareStops(ramp);
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes and images

// The number of bisection steps used to find the point where a line crosses a corner's curve.
constexpr uint32_t kCrossingSteps = 32;

// The maximum number of rectangles along one axis of a nine-slice region. Beyond it, the region stretches instead of
// tiling.
constexpr uint32_t kMaxTiles = 4096;

// The maximum number of lines that a wavy decoration's centerline flattens to.
constexpr uint32_t kWaveMaxLines = 65536;

// A border contour: the outer shape inset by a fraction of each side's width.
using Contour = PrCornerRect;

// A band of a side that a style paints between two contours, and whether the band is shaded.
struct Band {
    float from;
    float to;
    bool shaded;
};

// Points of an outer contour paired with points of an inner one along a side.
struct Track {
    Array<PrVec2> outer;
    Array<PrVec2> inner;
    Array<float> along;         // the arc length of each outer point from the side's start
};

struct Border {
    PrRect rect;
    const PrDrawBorder* border;
    float widths[4];
    PrCorner corners[4];        // fitted
    PrRect innerRect;           // the rectangle of the contour at 1
    float tolerance;
    PrDrawBorderEmitFn emit;
    void* PR_NULLABLE context;
    PrPathPolyline shape;
    PrPathPolyline cut;
    PrPath* curve;              // a dot or contour before it is flattened
    Track track;
};

float cross(PrVec2 a, PrVec2 b) {
    return a.x * b.y - a.y * b.x;
}

float length(PrVec2 a) {
    return sqrtF32(a.x * a.x + a.y * a.y);
}

Contour contourAt(const Border& b, float fraction) {
    float insets[4];
    for (uint32_t i = 0; i < 4; ++i)
        insets[i] = b.widths[i] * fraction;
    Contour outer{b.rect, {b.corners[0], b.corners[1], b.corners[2], b.corners[3]}};
    return prCornerRectInset(outer, insets);
}

// Returns the point of the corner's curve at `t`, or the rectangle's corner for a corner without radii.
PrVec2 curvePoint(const PrCornerPlacement& p, float t) {
    if (!(p.radiusBefore > 0 && p.radiusAfter > 0))
        return p.point;
    return prCornerPlacementPoint(p, prCornerCurvePoint(p.shape, t));
}

// Returns the parameter at which the line from `from` toward `toward` crosses the corner's curve.
float crossing(const PrCornerPlacement& p, PrVec2 from, PrVec2 toward) {
    PrVec2 direction = toward - from;
    if (!(p.radiusBefore > 0 && p.radiusAfter > 0) || length(direction) == 0)
        return 0.5f;
    auto side = [&](float t) { return cross(direction, curvePoint(p, t) - from); };
    float low = 0;
    float high = 1;
    float lowSide = side(low);
    if ((lowSide > 0) == (side(high) > 0))
        return 0.5f;
    for (uint32_t i = 0; i < kCrossingSteps; ++i) {
        float middle = (low + high) / 2;
        if ((side(middle) > 0) == (lowSide > 0))
            low = middle;
        else
            high = middle;
    }
    return (low + high) / 2;
}

// Returns the number of lines that a span `span` of a corner's parameter needs to stay within the tolerance.
uint32_t curveSteps(const PrCornerPlacement& a, const PrCornerPlacement& b, float span, float tolerance) {
    float radius = maxF32(maxF32(a.radiusBefore, a.radiusAfter), maxF32(b.radiusBefore, b.radiusAfter));
    if (!(radius > 0))
        return 1;
    float ratio = 1 - tolerance / radius;
    float step = ratio > 0 ? 2 * acosF32(ratio) : kPiF32 / 2;
    float sharpness = 1 + minF32(absF32(a.shape) + absF32(b.shape), 8);
    float steps = ceilF32(kPiF32 / 2 * span * sharpness / step);
    return steps < 1 ? 1 : steps > 256 ? 256 : uint32_t(steps);
}

// Pairs the two contours along a side, from the dividing line of the start corner to that of the end corner.
void trackSide(Border& b, uint32_t side, const Contour& outer, const Contour& inner) {
    Track& t = b.track;
    t.outer.clear();
    t.inner.clear();
    t.along.clear();
    uint32_t corners[2] = {side, (side + 1) % 4};
    for (uint32_t k = 0; k < 2; ++k) {
        uint32_t corner = corners[k];
        PrCornerPlacement po = prCornerRectPlacement(outer, PrRectCorner(corner));
        PrCornerPlacement pi = prCornerRectPlacement(inner, PrRectCorner(corner));
        PrVec2 from = prRectCornerPoint(b.rect, PrRectCorner(corner));
        PrVec2 toward = prRectCornerPoint(b.innerRect, PrRectCorner(corner));
        float to = crossing(po, from, toward);
        float ti = crossing(pi, from, toward);
        // The start corner's part after its dividing line, and the end corner's part before it.
        float o0 = k == 0 ? to : 0;
        float o1 = k == 0 ? 1 : to;
        float i0 = k == 0 ? ti : 0;
        float i1 = k == 0 ? 1 : ti;
        uint32_t steps = curveSteps(po, pi, maxF32(o1 - o0, i1 - i0), b.tolerance);
        for (uint32_t j = 0; j <= steps; ++j) {
            float u = float(j) / float(steps);
            t.outer.push(curvePoint(po, o0 + (o1 - o0) * u));
            t.inner.push(curvePoint(pi, i0 + (i1 - i0) * u));
        }
    }
    float along = 0;
    for (uint32_t i = 0; i < t.outer.count; ++i) {
        if (i != 0)
            along += length(t.outer.data[i] - t.outer.data[i - 1]);
        t.along.push(along);
    }
}

// Returns the segment of the track that contains arc length `s`, and writes the fraction of the segment's length at
// which `s` lies to `fraction`.
uint32_t segmentAt(const Track& t, float s, float* fraction) {
    uint32_t i = 0;
    while (i + 2 < t.along.count && t.along.data[i + 1] < s)
        ++i;
    float span = t.along.data[i + 1] - t.along.data[i];
    *fraction = span > 0 ? clampF32((s - t.along.data[i]) / span, 0, 1) : 0;
    return i;
}

PrVec2 outerAt(const Track& t, float s) {
    float f = 0;
    uint32_t i = segmentAt(t, s, &f);
    return prVec2Lerp(t.outer.data[i], t.outer.data[i + 1], f);
}

PrVec2 innerAt(const Track& t, float s) {
    float f = 0;
    uint32_t i = segmentAt(t, s, &f);
    return prVec2Lerp(t.inner.data[i], t.inner.data[i + 1], f);
}

// Appends the polygon of the track between arc lengths `from` and `to`: outer points forward, then inner points back.
void addTrackPolygon(PrPathPolyline* polyline, const Track& t, float from, float to) {
    prPathPolylineMoveTo(polyline, outerAt(t, from));
    for (uint32_t i = 0; i < t.along.count; ++i) {
        if (t.along.data[i] > from && t.along.data[i] < to)
            prPathPolylineLineTo(polyline, t.outer.data[i]);
    }
    prPathPolylineLineTo(polyline, outerAt(t, to));
    prPathPolylineLineTo(polyline, innerAt(t, to));
    for (uint32_t k = t.along.count; k > 0; --k) {
        uint32_t i = k - 1;
        if (t.along.data[i] > from && t.along.data[i] < to)
            prPathPolylineLineTo(polyline, t.inner.data[i]);
    }
    prPathPolylineLineTo(polyline, innerAt(t, from));
    prPathPolylineClose(polyline);
}

float trackLength(const Track& t) {
    return t.along.count != 0 ? t.along.last() : 0;
}

uint32_t bandsOf(PrDrawBorderStyle style, uint32_t side, Band out[2]) {
    bool topLeft = side == PrRectSide_Top || side == PrRectSide_Left;
    switch (style) {
        case PrDrawBorderStyle_Double:
            out[0] = Band{0, 1.0f / 3, false};
            out[1] = Band{2.0f / 3, 1, false};
            return 2;
        case PrDrawBorderStyle_Groove:
            out[0] = Band{0, 0.5f, topLeft};
            out[1] = Band{0.5f, 1, !topLeft};
            return 2;
        case PrDrawBorderStyle_Ridge:
            out[0] = Band{0, 0.5f, !topLeft};
            out[1] = Band{0.5f, 1, topLeft};
            return 2;
        case PrDrawBorderStyle_Inset:
            out[0] = Band{0, 1, topLeft};
            return 1;
        case PrDrawBorderStyle_Outset:
            out[0] = Band{0, 1, !topLeft};
            return 1;
        default:
            out[0] = Band{0, 1, false};
            return 1;
    }
}

// Returns the color that a side's brush multiplies: the side's color, or white at its brush's opacity.
PrColor colorOf(const PrDrawBorderSide& side) {
    return side.brush != nullptr ? PrColor{1, 1, 1, side.brush->opacity} : PrColor(side.color);
}

PrColor shaded(PrColor color, bool shade) {
    if (!shade)
        return color;
    return PrColor{color.r * kPrDrawBorderShade, color.g * kPrDrawBorderShade, color.b * kPrDrawBorderShade, color.a};
}

bool brushEquals(const PrDrawBrush* PR_NULLABLE a, const PrDrawBrush* PR_NULLABLE b) {
    if (a == b)
        return true;
    if (a == nullptr || b == nullptr)
        return false;
    // Compares the records without their stops, which are compared by value.
    PrDrawBrush x = *a;
    PrDrawBrush y = *b;
    x.stops = nullptr;
    y.stops = nullptr;
    return memEquals(&x, &y, sizeof(PrDrawBrush)) &&
        (a->stopCount == 0 || memEquals(a->stops, b->stops, sizeof(PrDrawColorStop) * a->stopCount));
}

bool paints(const PrDrawBorderSide& side) {
    return side.width > 0 && prDrawBorderStylePaints(side.style);
}

// Returns the side whose style and paint every side shares, if the border is one area, or null. A border with a side
// that paints nothing is not one area, whatever that side's style: the area's outer and inner contours would meet along
// the side, and a side of no width draws as a side of no style does.
const PrDrawBorderSide* PR_NULLABLE uniformSide(const PrDrawBorder* border) {
    const PrDrawBorderSide* first = nullptr;
    for (const PrDrawBorderSide& side : border->sides) {
        if (!paints(side))
            return nullptr;
        if (first == nullptr) {
            if (side.style != PrDrawBorderStyle_Solid && side.style != PrDrawBorderStyle_Double)
                return nullptr;
            first = &side;
        } else if (side.style != first->style || !brushEquals(side.brush, first->brush) ||
                !memEquals(&side.color, &first->color, sizeof(PrColor16))) {
            return nullptr;
        }
    }
    return first;
}

// Computes the side's region as a convex polygon. The region lies between the lines from its outer corners through its
// inner corners, and extends far past the border on the outside.
void wedgePoints(const Border& b, uint32_t side, Array<PrVec2>& out) {
    out.clear();
    uint32_t c0 = side;
    uint32_t c1 = (side + 1) % 4;
    PrVec2 q0 = prRectCornerPoint(b.rect, PrRectCorner(c0));
    PrVec2 q1 = prRectCornerPoint(b.rect, PrRectCorner(c1));
    PrVec2 d0 = prRectCornerPoint(b.innerRect, PrRectCorner(c0)) - q0;
    PrVec2 d1 = prRectCornerPoint(b.innerRect, PrRectCorner(c1)) - q1;
    float far = prRectWidth(b.rect) + prRectHeight(b.rect);
    PrVec2 u0 = length(d0) > 0 ? d0 * (1 / length(d0)) : PrVec2{0, 0};
    PrVec2 u1 = length(d1) > 0 ? d1 * (1 / length(d1)) : PrVec2{0, 0};
    out.push(q0 - u0 * far);
    out.push(q1 - u1 * far);
    float denominator = cross(u0, u1);
    float s = denominator != 0 ? cross(q1 - q0, u1) / denominator : -1;
    if (s > 0) {
        out.push(q0 + u0 * s);
    } else {
        out.push(q1 + u1 * far);
        out.push(q0 + u0 * far);
    }
}

void addPolygon(PrPathPolyline* polyline, const Array<PrVec2>& points) {
    if (points.count < 3)
        return;
    prPathPolylineMoveTo(polyline, points.data[0]);
    for (uint32_t i = 1; i < points.count; ++i)
        prPathPolylineLineTo(polyline, points.data[i]);
    prPathPolylineClose(polyline);
}

void addWedge(Border& b, uint32_t side, PrPathPolyline* polyline) {
    Array<PrVec2> points;
    wedgePoints(b, side, points);
    addPolygon(polyline, points);
    points.reset();
}

// Keeps the part of the convex polygon where dot(normal, p - at) >= 0, using `scratch`.
void clipPolygon(Array<PrVec2>& polygon, PrVec2 at, PrVec2 normal, Array<PrVec2>& scratch) {
    scratch.clear();
    for (uint32_t i = 0; i < polygon.count; ++i) {
        PrVec2 a = polygon.data[i];
        PrVec2 c = polygon.data[(i + 1) % polygon.count];
        float da = normal.x * (a.x - at.x) + normal.y * (a.y - at.y);
        float dc = normal.x * (c.x - at.x) + normal.y * (c.y - at.y);
        if (da >= 0)
            scratch.push(a);
        if ((da >= 0) != (dc >= 0))
            scratch.push(prVec2Lerp(a, c, da / (da - dc)));
    }
    polygon.clear();
    polygon.append(scratch.data, scratch.count);
}

// Returns the direction of the track's outer edge at arc length `s`.
PrVec2 outerDirection(const Track& t, float s) {
    float f = 0;
    uint32_t i = segmentAt(t, s, &f);
    PrVec2 d = t.outer.data[i + 1] - t.outer.data[i];
    for (uint32_t k = i; length(d) <= 0 && k + 1 < t.outer.count; ++k)
        d = t.outer.data[k + 1] - t.outer.data[k];
    return length(d) > 0 ? d * (1 / length(d)) : PrVec2{0, 0};
}

// Returns true if two sides draw the same dots, so that one side can draw a corner's dot whole.
bool dotsEquals(const PrDrawBorderSide& a, const PrDrawBorderSide& b) {
    return a.style == PrDrawBorderStyle_Dotted && b.style == PrDrawBorderStyle_Dotted &&
        a.width == b.width && memEquals(&a.color, &b.color, sizeof(PrColor16)) && brushEquals(a.brush, b.brush);
}

// Returns the whole number of periods nearest `periods`, from one to kPrDrawPatternMaxPeriods, or one if `periods` is
// not a number.
uint32_t periodCount(float periods) {
    float count = roundF32(periods);
    return !(count >= 1) ? 1 : count > float(kPrDrawPatternMaxPeriods) ? kPrDrawPatternMaxPeriods : uint32_t(count);
}

uint32_t periodsOf(const PrDrawBorderSide& side, float length, float perWidth) {
    if (side.patternPeriods >= 1 && side.patternPeriods <= kPrDrawPatternMaxPeriods)
        return side.patternPeriods;
    return periodCount(length / (perWidth * side.width));
}

void emitPiece(Border& b, uint32_t side, PrColor color, PrPathFillRule rule, bool cut) {
    if (b.shape.pointCount == 0)
        return;
    PrDrawBorderPiece piece{.side = PrRectSide(side), .rule = rule, .color = color};
    b.emit(b.context, &piece, &b.shape, cut ? &b.cut : nullptr);
}

void lowerSide(Border& b, uint32_t side) {
    const PrDrawBorderSide& s = b.border->sides[side];
    if (!paints(s))
        return;
    Contour outer = contourAt(b, 0);
    Contour inner = contourAt(b, 1);
    if (s.style == PrDrawBorderStyle_Dashed) {
        prPathPolylineClear(&b.shape);
        prPathPolylineClear(&b.cut);
        trackSide(b, side, outer, inner);
        float total = trackLength(b.track);
        if (!(total > 0))
            return;
        addTrackPolygon(&b.shape, b.track, 0, total);
        // Each dash is the side's region cut by lines square to the outer edge at its ends, so every gap crosses the
        // whole band.
        uint32_t n = periodsOf(s, total, 4);
        float period = total / float(n);
        Array<PrVec2> wedge;
        Array<PrVec2> dash;
        Array<PrVec2> scratch;
        wedgePoints(b, side, wedge);
        for (uint32_t k = 0; k <= n; ++k) {
            float center = float(k) * period;
            float from = center - period * 3 / 8;
            float to = center + period * 3 / 8;
            dash.clear();
            dash.append(wedge.data, wedge.count);
            if (from > 0)
                clipPolygon(dash, outerAt(b.track, from), outerDirection(b.track, from), scratch);
            if (to < total)
                clipPolygon(dash, outerAt(b.track, to), -outerDirection(b.track, to), scratch);
            addPolygon(&b.cut, dash);
        }
        wedge.reset();
        dash.reset();
        scratch.reset();
        emitPiece(b, side, colorOf(s), PrPathFillRule_NonZero, true);
        return;
    }
    if (s.style == PrDrawBorderStyle_Dotted) {
        // A dot at a corner is split between the sides, unless the side that shares it has the same dots. In that case
        // the side before the corner draws it whole.
        const PrDrawBorderSide& previous = b.border->sides[(side + 3) % 4];
        const PrDrawBorderSide& next = b.border->sides[(side + 1) % 4];
        bool shareStart = dotsEquals(s, previous);
        bool shareEnd = dotsEquals(s, next);
        trackSide(b, side, outer, contourAt(b, 0.5f));
        float total = trackLength(b.track);
        uint32_t n = periodsOf(s, total, 2);
        float period = total / float(n);
        for (uint32_t pass = 0; pass < 2; ++pass) {
            bool cut = pass == 1;
            prPathPolylineClear(&b.shape);
            prPathPolylineClear(&b.cut);
            for (uint32_t k = 0; k <= n; ++k) {
                bool whole = (k != 0 && k != n) || (k == n && shareEnd);
                bool split = (k == 0 && !shareStart) || (k == n && !shareEnd);
                if (cut ? !split : !whole)
                    continue;
                PrVec2 center = innerAt(b.track, float(k) * period);
                prPathClear(b.curve);
                prPathEllipticArcTo(b.curve, center, PrVec2{s.width / 2, s.width / 2}, 0, 0, 2 * kPiF32);
                prPathClose(b.curve);
                prPathFlatten(b.curve, prAffine2Identity(), b.tolerance, PrPathFlattening_Balanced, &b.shape);
            }
            if (cut)
                addWedge(b, side, &b.cut);
            emitPiece(b, side, colorOf(s), PrPathFillRule_NonZero, cut);
        }
        return;
    }
    Band bands[2];
    uint32_t count = bandsOf(s.style, side, bands);
    for (uint32_t i = 0; i < count; ++i) {
        // Bands of one color share a piece.
        if (i != 0 && bands[i].shaded == bands[i - 1].shaded)
            continue;
        prPathPolylineClear(&b.shape);
        for (uint32_t j = i; j < count && bands[j].shaded == bands[i].shaded; ++j) {
            trackSide(b, side, contourAt(b, bands[j].from), contourAt(b, bands[j].to));
            addTrackPolygon(&b.shape, b.track, 0, trackLength(b.track));
        }
        emitPiece(b, side, shaded(colorOf(s), bands[i].shaded), PrPathFillRule_NonZero, false);
    }
}

void addContour(Border& b, const Contour& c) {
    prPathClear(b.curve);
    prPathAddCornerRect(b.curve, c);
    prPathFlatten(b.curve, prAffine2Identity(), b.tolerance, PrPathFlattening_Balanced, &b.shape);
}

// Tiles one axis of a nine-slice region, appending source and destination intervals in pairs.
void tileAxis(PrDrawSliceMode mode, float sourceStart, float sourceSize, float start, float size,
        float tile, Array<float>& out) {
    auto emit = [&](float s0, float s1, float d0, float d1) {
        float values[4] = {s0, s1, d0, d1};
        out.append(values, 4);
    };
    if (!(size > 0) || !(sourceSize > 0))
        return;
    if (mode == PrDrawSliceMode_Stretch || !(tile > 0) || size / tile > float(kMaxTiles)) {
        emit(sourceStart, sourceStart + sourceSize, start, start + size);
        return;
    }
    if (mode == PrDrawSliceMode_Round) {
        float n = maxF32(roundF32(size / tile), 1);
        float step = size / n;
        for (uint32_t i = 0; i < uint32_t(n); ++i)
            emit(sourceStart, sourceStart + sourceSize, start + step * float(i), start + step * float(i + 1));
        return;
    }
    if (mode == PrDrawSliceMode_Space) {
        float n = floorF32(size / tile);
        if (n < 1)
            return;
        float gap = (size - n * tile) / (n + 1);
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            float d0 = start + gap + (tile + gap) * float(i);
            emit(sourceStart, sourceStart + sourceSize, d0, d0 + tile);
        }
        return;
    }
    // Repeat: one tile is centered, and the rest extend outward from it, cut at both ends.
    float end = start + size;
    float first = start + size / 2 - tile / 2;
    first -= ceilF32((first - start) / tile) * tile;
    for (float d0 = first; d0 < end; d0 += tile) {
        float d1 = d0 + tile;
        float c0 = maxF32(d0, start);
        float c1 = minF32(d1, end);
        if (c1 <= c0)
            continue;
        float s0 = sourceStart + (c0 - d0) / tile * sourceSize;
        float s1 = sourceStart + (c1 - d0) / tile * sourceSize;
        emit(s0, s1, c0, c1);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Shapes and strokes

void prDrawShapePath(const PrDrawCommand* command, PrPath* out) {
    const PrDrawCommand& c = *command;
    switch (c.kind) {
        case PrDrawCommandKind_Rect:
        case PrDrawCommandKind_PushClipRect:
            if (prRectIsEmpty(c.rect)) {
                PrVec2 sides[4] = {
                    {c.rect.left, c.rect.top},
                    {c.rect.right, c.rect.top},
                    {c.rect.right, c.rect.bottom},
                    {c.rect.left, c.rect.bottom},
                };
                addPolyline(out, sides, 4, true);
            } else {
                prPathAddRect(out, c.rect);
            }
            break;
        case PrDrawCommandKind_CornerRect:
        case PrDrawCommandKind_PushClipCornerRect:
            prPathAddCornerRect(out, *c.cornerRect);
            break;
        case PrDrawCommandKind_Ellipse:
            prPathAddEllipse(out, c.ellipse.center, c.ellipse.radii, c.ellipse.rotation);
            break;
        case PrDrawCommandKind_Sector:
            prPathAddSector(out, c.sector.center, c.sector.innerRadius, c.sector.outerRadius, c.sector.startAngle,
                c.sector.sweep);
            break;
        case PrDrawCommandKind_Capsule:
            if (c.capsule.radius > 0) {
                prPathAddCapsule(out, c.capsule.start, c.capsule.end, c.capsule.radius);
            } else {
                prPathMoveTo(out, c.capsule.start);
                prPathLineTo(out, c.capsule.end);
            }
            break;
        case PrDrawCommandKind_Line:
            prPathMoveTo(out, c.line.start);
            prPathLineTo(out, c.line.end);
            break;
        case PrDrawCommandKind_Polyline: {
            const PrPathPolyline& polyline = *c.polyline;
            for (uint32_t i = 0; i < polyline.contourCount; ++i) {
                uint32_t start = polyline.contours[i];
                uint32_t end = prPathPolylineContourEnd(&polyline, i);
                if (end > start)
                    addPolyline(out, polyline.points + start, end - start, prPathPolylineContourIsClosed(&polyline, i));
            }
            break;
        }
        case PrDrawCommandKind_Path:
        case PrDrawCommandKind_PushClipPath:
            prPathAddPath(out, c.path, prAffine2Identity());
            break;
        case PrDrawCommandKind_Glyphs: {
            const PrFontGlyphRun* run = c.glyphs;
            for (uint32_t i = 0; i < run->glyphCount; ++i)
                prFontGlyphOutline(run->font, run->glyphs[i].id, 0, prDrawGlyphTransform(run, i), out);
            break;
        }
        default:
            break;
    }
}

PrRect prDrawFillBounds(const PrDrawCommand* command) {
    const PrDrawCommand& c = *command;
    switch (c.kind) {
        case PrDrawCommandKind_Rect:
            return c.rect;
        case PrDrawCommandKind_CornerRect:
            return c.cornerRect->rect;
        case PrDrawCommandKind_Ellipse: {
            float s = sinF32(c.ellipse.rotation);
            float k = cosF32(c.ellipse.rotation);
            PrVec2 r = c.ellipse.radii;
            float x = sqrtF32(r.x * k * r.x * k + r.y * s * r.y * s);
            float y = sqrtF32(r.x * s * r.x * s + r.y * k * r.y * k);
            return PrRect{
                c.ellipse.center.x - x, c.ellipse.center.y - y, c.ellipse.center.x + x,
                c.ellipse.center.y + y,
            };
        }
        case PrDrawCommandKind_Capsule: {
            float r = c.capsule.radius;
            return PrRect{
                minF32(c.capsule.start.x, c.capsule.end.x) - r,
                minF32(c.capsule.start.y, c.capsule.end.y) - r,
                maxF32(c.capsule.start.x, c.capsule.end.x) + r,
                maxF32(c.capsule.start.y, c.capsule.end.y) + r,
            };
        }
        case PrDrawCommandKind_Sector:
        case PrDrawCommandKind_Line:
        case PrDrawCommandKind_Polyline:
        case PrDrawCommandKind_Path: {
            if (c.kind == PrDrawCommandKind_Path)
                return prPathTightBounds(c.path);
            PrPath* shape = prPathCreate();
            prDrawShapePath(command, shape);
            PrRect bounds = prPathTightBounds(shape);
            prPathDestroy(shape);
            return bounds;
        }
        case PrDrawCommandKind_Glyphs: {
            const PrFontGlyphRun* run = c.glyphs;
            PrFontMetrics metrics = prFontMetrics(run->font);
            PrRect bounds{0, 0, 0, 0};
            for (uint32_t i = 0; i < run->glyphCount; ++i) {
                float advance = prFontGlyphAdvance(run->font, run->glyphs[i].id, 0);
                PrRect cell{0, -metrics.ascent, advance, metrics.descent};
                bounds = prRectUnion(bounds, prAffine2MapRect(prDrawGlyphTransform(run, i), cell));
            }
            return bounds;
        }
        case PrDrawCommandKind_Border:
            return c.border->shape.rect;
        case PrDrawCommandKind_DropShadow: {
            const PrDrawShadow* s = c.shadow;
            const PrRect& r = s->shape.rect;
            return PrRect{
                r.left + s->offset.x - s->spread, r.top + s->offset.y - s->spread,
                r.right + s->offset.x + s->spread, r.bottom + s->offset.y + s->spread,
            };
        }
        case PrDrawCommandKind_InnerShadow:
            return c.shadow->shape.rect;
        case PrDrawCommandKind_Decoration: {
            const PrDrawDecoration* d = c.decoration;
            float half = d->thickness / 2;
            if (d->style == PrDrawDecorationStyle_Double)
                half = d->thickness * 1.5f;
            else if (d->style == PrDrawDecorationStyle_Wavy)
                half += absF32(d->amplitude);
            return PrRect{d->origin.x, d->origin.y - half, d->origin.x + d->length, d->origin.y + half};
        }
        case PrDrawCommandKind_NineSlice:
            return c.nineSlice.destination;
        default:
            return PrRect{0, 0, 0, 0};
    }
}

float prDrawStrokeReach(const PrDrawStroke* stroke) {
    PrPathStroke shape = stroke->shape;
    shape.width = 1;
    float reach = prPathStrokeReach(&shape);
    return stroke->align != PrDrawStrokeAlign_Center ? reach * 2 : reach;
}

void prDrawLowerStroke(const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform, float tolerance,
        PrPathPolyline* fill, PrPathPolyline* aligned, PrPathPolyline* region) {
    if (stroke->shape.width > 0)
        lowerStroke(path, stroke, transform, tolerance, fill, aligned, region);
}

// ---------------------------------------------------------------------------------------------------------------------
// Brushes

bool prDrawBrushFromTarget(const PrDrawBrush* brush, PrAffine2 transform, PrRect bounds, PrAffine2* out) {
    PrAffine2 spaceToDevice = transform;
    switch (brush->space) {
        case PrDrawBrushSpace_ObjectBounds: {
            float width = prRectWidth(bounds);
            float height = prRectHeight(bounds);
            if (!(width > 0 && height > 0))
                return false;
            PrAffine2 unit = prAffine2Concat(prAffine2Scale(PrVec2{width, height}),
                prAffine2Translate(PrVec2{bounds.left, bounds.top}));
            spaceToDevice = prAffine2Concat(unit, transform);
            break;
        }
        case PrDrawBrushSpace_Target:
            spaceToDevice = prAffine2Identity();
            break;
        default:
            break;
    }
    return prAffine2Invert(prAffine2Concat(brush->transform, spaceToDevice), out);
}

bool prDrawGradientParameter(const PrDrawBrush* brush, PrVec2 point, float* t) {
    switch (brush->kind) {
        case PrDrawBrushKind_Linear: {
            PrVec2 d = brush->linear.end - brush->linear.start;
            float length = dot(d, d);
            if (!(length > 0))
                return false;
            *t = dot(point - brush->linear.start, d) / length;
            return true;
        }
        case PrDrawBrushKind_Radial: {
            // The largest t whose circle, of nonnegative radius, passes through the point.
            PrVec2 focal = brush->radial.start;
            float r0 = brush->radial.startRadius;
            PrVec2 cd = brush->radial.end - focal;
            float dr = brush->radial.endRadius - r0;
            PrVec2 pd = point - focal;
            float a = dot(cd, cd) - dr * dr;
            float b = dot(pd, cd) + r0 * dr;
            float c = dot(pd, pd) - r0 * r0;
            if (dot(cd, cd) == 0 && dr == 0)
                return false;
            if (absF32(a) < 1e-6f * maxF32(dot(cd, cd), dr * dr)) {
                if (b == 0)
                    return false;
                float root = c / (2 * b);
                if (!(r0 + root * dr >= 0))
                    return false;
                *t = root;
                return true;
            }
            float discriminant = b * b - a * c;
            if (discriminant < 0)
                return false;
            float root = sqrtF32(discriminant);
            float t1 = (b + root) / a;
            float t2 = (b - root) / a;
            float larger = maxF32(t1, t2);
            float smaller = minF32(t1, t2);
            if (r0 + larger * dr >= 0) {
                *t = larger;
                return true;
            }
            if (r0 + smaller * dr >= 0) {
                *t = smaller;
                return true;
            }
            return false;
        }
        case PrDrawBrushKind_Conic: {
            PrVec2 d = point - brush->conic.center;
            float turn = (atan2F32(d.y, d.x) - brush->conic.angle) / (2 * kPiF32);
            *t = turn - floorF32(turn);
            return true;
        }
        case PrDrawBrushKind_Diamond: {
            PrVec2 r = brush->diamond.radii;
            if (!(r.x > 0 && r.y > 0))
                return false;
            PrVec2 d = point - brush->diamond.center;
            *t = absF32(d.x) / r.x + absF32(d.y) / r.y;
            return true;
        }
        default:
            return false;
    }
}

bool prDrawGradientExtend(PrDrawExtend extend, float* t) {
    float v = *t;
    if (extend == PrDrawExtend_None && !(v >= 0 && v <= 1))
        return false;
    // Pad keeps which end a point lies past: below 0 or above 1.
    const bool pad = extend != PrDrawExtend_Repeat && extend != PrDrawExtend_Reflect;
    if (!isFiniteF32(v)) {
        *t = isNanF32(v) ? 0 : v > 0 ? (pad ? 2 : 1) : (pad ? -1 : 0);
        return true;
    }
    switch (extend) {
        case PrDrawExtend_Repeat:
            *t = v - floorF32(v);
            break;
        case PrDrawExtend_Reflect: {
            float m = v - 2 * floorF32(v / 2);
            *t = m > 1 ? 2 - m : m;
            break;
        }
        default:
            *t = clampF32(v, -1, 2);
            break;
    }
    return true;
}

uint64_t prDrawGradientRampHash(const PrDrawBrush* brush, PrColor color) {
    const uint32_t mode = uint32_t(brush->interpolationSpace) | uint32_t(brush->hueMethod) << 8 |
        uint32_t(brush->interpolationAlpha) << 16;
    uint64_t h = hashBytes(&color, sizeof(color), mode);
    return hashBytes(brush->stops, sizeof(PrDrawColorStop) * brush->stopCount, h);
}

bool prDrawGradientRampEquals(const PrDrawBrush* a, PrColor aColor, const PrDrawBrush* b, PrColor bColor) {
    return a->interpolationSpace == b->interpolationSpace && a->hueMethod == b->hueMethod &&
        a->interpolationAlpha == b->interpolationAlpha && a->stopCount == b->stopCount &&
        memEquals(&aColor, &bColor, sizeof(PrColor)) &&
        (a->stopCount == 0 || memEquals(a->stops, b->stops, sizeof(PrDrawColorStop) * a->stopCount));
}

uint32_t prDrawGradientRampMaxPieces(uint32_t stopCount) {
    return stopCount + 3;
}

uint32_t prDrawGradientRampMaxColors(uint32_t stopCount, uint32_t resolution) {
    // The pieces between 0 and 1 span at most 1, and each piece has up to two colors besides its steps.
    return resolution + 2 * prDrawGradientRampMaxPieces(stopCount);
}

uint32_t prDrawGradientRamp(const PrDrawBrush* brush, PrColor color, PrColorSpace space, uint32_t resolution,
        PrDrawRampPiece* pieces, PrColor* colors) {
    Ramp ramp{};
    prepareRamp(brush, ramp);
    const SpaceStop transparent{};
    const float alphaOf = clampF32(color.a, 0, 1);
    uint32_t pieceCount = 0;
    uint32_t colorCount = 0;
    eachSpan(ramp, space, [&](const RampSpan& span) {
        uint32_t count = 1;
        if (!span.flat) {
            float steps = ceilF32(float(resolution) * (span.end - span.start));
            count = span.linear || steps < 1 ? 2 : uint32_t(steps) + 1;
        }
        pieces[pieceCount++] = PrDrawRampPiece{.start = span.start, .end = span.end, .first = colorCount, .count = count};
        const SpaceStop& a = ramp.stops.count != 0 ? ramp.stops.data[span.a] : transparent;
        const SpaceStop& b = ramp.stops.count != 0 ? ramp.stops.data[span.b] : transparent;
        for (uint32_t k = 0; k < count; ++k) {
            // The last color is at the piece's end exactly.
            float t = span.start;
            if (k + 1 == count && k != 0)
                t = span.end;
            else if (k != 0)
                t += (span.end - span.start) * float(k) / float(count - 1);
            float f = span.a == span.b ? 0 : clampF32((t - a.offset) / (b.offset - a.offset), 0, 1);
            double channels[3];
            double alpha = 0;
            mixStops(ramp, a, b, f, channels, &alpha);
            if (alpha > 0 && !ramp.straight) {
                for (uint32_t c = 0; c < 3; ++c) {
                    if (c != ramp.hue)
                        channels[c] /= alpha;
                }
            }
            prColorConvert(ramp.space, space, channels, channels);
            float alphaOut = float(alpha) * alphaOf;
            colors[colorCount++] = PrColor{
                float(channels[0]) * color.r * alphaOut,
                float(channels[1]) * color.g * alphaOut,
                float(channels[2]) * color.b * alphaOut, alphaOut,
            };
        }
    });
    ramp.stops.reset();
    return pieceCount;
}

uint32_t prDrawRampColorCount(const PrDrawRampPiece* PR_NULLABLE pieces, uint32_t count) {
    return count == 0 ? 0 : pieces[count - 1].first + pieces[count - 1].count;
}

uint32_t prDrawRampPieceAt(const PrDrawRampPiece* pieces, float t, uint32_t hint) {
    uint32_t k = pieces[hint].start <= t ? hint : 0;
    while (t > pieces[k].end || (t == pieces[k].end && t < 1))
        ++k;
    return k;
}

PrColor prDrawRampColor(const PrDrawRampPiece* piece, const PrColor* colors, float t) {
    const PrColor* c = colors + piece->first;
    if (piece->count == 1)
        return c[0];
    float x = (t - piece->start) / (piece->end - piece->start) * float(piece->count - 1);
    float i = clampF32(floorF32(x), 0, float(piece->count - 2));
    float f = clampF32(x - i, 0, 1);
    const PrColor& a = c[uint32_t(i)];
    const PrColor& b = c[uint32_t(i) + 1];
    return PrColor{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes and images

void prDrawBorderLower(const PrDrawBorder* border, float tolerance, PrDrawBorderEmitFn emit, void* PR_NULLABLE context) {
    const PrRect rect = border->shape.rect;
    if (prRectIsEmpty(rect))
        return;
    Border b{};
    b.rect = rect;
    b.border = border;
    b.tolerance = tolerance > 0 ? tolerance : 0.01f;
    b.emit = emit;
    b.context = context;
    for (uint32_t i = 0; i < 4; ++i)
        b.widths[i] = paints(border->sides[i]) ? border->sides[i].width : 0;
    PrCornerRect fitted = prCornerRectFit(border->shape);
    memCopy(b.corners, fitted.corners, sizeof(b.corners));
    b.innerRect = PrRect{
        rect.left + b.widths[PrRectSide_Left], rect.top + b.widths[PrRectSide_Top],
        rect.right - b.widths[PrRectSide_Right],
        rect.bottom - b.widths[PrRectSide_Bottom],
    };
    b.curve = prPathCreate();
    if (const PrDrawBorderSide* shared = uniformSide(border)) {
        addContour(b, contourAt(b, 0));
        if (shared->style == PrDrawBorderStyle_Double) {
            addContour(b, contourAt(b, 1.0f / 3));
            addContour(b, contourAt(b, 2.0f / 3));
        }
        addContour(b, contourAt(b, 1));
        emitPiece(b, 0, colorOf(*shared), PrPathFillRule_EvenOdd, false);
    } else {
        for (uint32_t side = 0; side < 4; ++side)
            lowerSide(b, side);
    }
    prPathPolylineReset(&b.shape);
    prPathPolylineReset(&b.cut);
    prPathDestroy(b.curve);
    b.track.outer.reset();
    b.track.inner.reset();
    b.track.along.reset();
}

uint32_t prDrawBorderBands(const PrDrawBorder* border, PrRectSide side,
        PrDrawBorderBand out[PR_NONNULL kPrDrawBorderMaxBands]) {
    const PrDrawBorderSide& s = border->sides[side];
    if (!paints(s) || s.style == PrDrawBorderStyle_Dashed || s.style == PrDrawBorderStyle_Dotted)
        return 0;
    Band bands[2];
    uint32_t count = bandsOf(s.style, side, bands);
    for (uint32_t i = 0; i < count; ++i)
        out[i] = PrDrawBorderBand{bands[i].from, bands[i].to, shaded(colorOf(s), bands[i].shaded)};
    return count;
}

const PrDrawBorderSide* PR_NULLABLE prDrawBorderUniformSide(const PrDrawBorder* border) {
    return uniformSide(border);
}

PrCornerRect prDrawBorderContour(const PrDrawBorder* border, float fraction) {
    Border b{};
    b.rect = border->shape.rect;
    for (uint32_t i = 0; i < 4; ++i)
        b.widths[i] = paints(border->sides[i]) ? border->sides[i].width : 0;
    PrCornerRect fitted = prCornerRectFit(border->shape);
    memCopy(b.corners, fitted.corners, sizeof(b.corners));
    return contourAt(b, fraction);
}

bool prDrawBorderSides(const PrDrawBorder* border, PrDrawBorderSides* out) {
    if (prDrawBorderUniformSide(border) != nullptr)
        return false;
    float widths[4] = {};
    PrColor colors[4] = {};
    bool any = false;
    for (uint32_t side = 0; side < 4; ++side) {
        const PrDrawBorderSide& s = border->sides[side];
        if (!paints(s))
            continue;
        PrDrawBorderBand bands[kPrDrawBorderMaxBands];
        if (s.brush != nullptr || prDrawBorderBands(border, PrRectSide(side), bands) != 1 || bands[0].from != 0 ||
                bands[0].to != 1)
            return false;
        widths[side] = s.width;
        colors[side] = bands[0].color;
        any = true;
    }
    if (!any)
        return false;
    // Each side reaches inward by its width or by its corners' radii, whichever is greater, and opposite sides part
    // halfway between their reaches.
    const PrRect rect = border->shape.rect;
    const PrCornerRect fitted = prCornerRectFit(border->shape);
    const PrCorner* c = fitted.corners;
    float top = maxF32(widths[PrRectSide_Top],
        maxF32(c[PrRectCorner_TopLeft].radii.y, c[PrRectCorner_TopRight].radii.y));
    float bottom = maxF32(widths[PrRectSide_Bottom],
        maxF32(c[PrRectCorner_BottomRight].radii.y, c[PrRectCorner_BottomLeft].radii.y));
    float left = maxF32(widths[PrRectSide_Left],
        maxF32(c[PrRectCorner_TopLeft].radii.x, c[PrRectCorner_BottomLeft].radii.x));
    float right = maxF32(widths[PrRectSide_Right],
        maxF32(c[PrRectCorner_TopRight].radii.x, c[PrRectCorner_BottomRight].radii.x));
    if (rect.top + top > rect.bottom - bottom || rect.left + left > rect.right - right)
        return false;
    PrDrawBorderSides sides{};
    sides.split = PrVec2{(rect.left + left + rect.right - right) / 2, (rect.top + top + rect.bottom - bottom) / 2};
    const PrRect inner{
        rect.left + widths[PrRectSide_Left], rect.top + widths[PrRectSide_Top],
        rect.right - widths[PrRectSide_Right], rect.bottom - widths[PrRectSide_Bottom],
    };
    for (uint32_t k = 0; k < 4; ++k) {
        uint32_t before = (k + 3) % 4;
        sides.colors[k][0] = widths[before] > 0 ? colors[before] : colors[k];
        sides.colors[k][1] = widths[k] > 0 ? colors[k] : colors[before];
        PrVec2 q = prRectCornerPoint(rect, PrRectCorner(k));
        PrVec2 e = prRectCornerPoint(inner, PrRectCorner(k));
        // The line is left unnormalized, so that it is exact at points of whole and half units, such as a pixel's
        // center that it passes through.
        float a = -(e.y - q.y);
        float b = e.x - q.x;
        if (a == 0 && b == 0)
            continue;
        float offset = -(a * q.x + b * q.y);
        // The middle of the outer edge of the side after the corner lies on the positive side.
        PrVec2 next = prRectCornerPoint(rect, PrRectCorner((k + 1) % 4));
        if (a * (q.x + next.x) / 2 + b * (q.y + next.y) / 2 + offset < 0) {
            a = -a;
            b = -b;
            offset = -offset;
        }
        sides.lines[k][0] = a;
        sides.lines[k][1] = b;
        sides.lines[k][2] = offset;
    }
    *out = sides;
    return true;
}

uint32_t prDrawBorderSidesCorner(const PrDrawBorderSides* sides, PrVec2 point) {
    if (point.y < sides->split.y)
        return point.x < sides->split.x ? PrRectCorner_TopLeft : PrRectCorner_TopRight;
    return point.x < sides->split.x ? PrRectCorner_BottomLeft : PrRectCorner_BottomRight;
}

float prDrawHalfPlaneArea(float distance, PrVec2 gradient) {
    float length = sqrtF32(gradient.x * gradient.x + gradient.y * gradient.y);
    if (!(length > 0))
        return distance > 0 ? 1.0f : 0.0f;
    // In pixels, the line's unit normal has components u >= v. The area is linear in the distance while the line
    // crosses both sides of the pixel that run along the major axis, and quadratic where it cuts off a corner.
    float d = distance / length;
    float u = maxF32(absF32(gradient.x), absF32(gradient.y)) / length;
    float v = minF32(absF32(gradient.x), absF32(gradient.y)) / length;
    float reach = (u + v) / 2;
    float m = absF32(d);
    if (m >= reach)
        return d > 0 ? 1.0f : 0.0f;
    if (m <= (u - v) / 2)
        return 0.5f + d / u;
    float t = reach - m;
    float corner = t * t / (2 * u * v);
    return d > 0 ? 1 - corner : corner;
}

PrDrawShadowGeometry prDrawShadowGeometry(const PrDrawShadow* shadow, PrDrawShadowKind kind) {
    const PrDrawShadow& s = *shadow;
    PrDrawShadowGeometry out{};
    PrVec2 offset = s.nonScaling ? PrVec2{0, 0} : s.offset;
    float spread = kind == PrDrawShadowKind_Inner ? -s.spread : s.spread;
    const PrRect& rect = s.shape.rect;
    out.shape.rect = PrRect{
        rect.left + offset.x - spread, rect.top + offset.y - spread,
        rect.right + offset.x + spread, rect.bottom + offset.y + spread,
    };
    PrCornerRect fitted = prCornerRectFit(s.shape);
    for (uint32_t i = 0; i < 4; ++i) {
        PrCorner c = fitted.corners[i];
        float radii[2] = {c.radii.x, c.radii.y};
        for (float& r : radii) {
            if (!(r > 0))
                continue;
            if (spread >= 0) {
                // A radius smaller than the spread grows by less, so a sharp corner stays sharp.
                float growth = r >= spread || spread == 0 ? spread : spread * (1 + powF32(r / spread - 1, 3));
                r += growth;
            } else {
                r = maxF32(r + spread, 0);
            }
        }
        c.radii = PrVec2{radii[0], radii[1]};
        out.shape.corners[i] = c;
    }
    out.sigma = maxF32(s.sigma, 0);
    return out;
}

void prDrawShadowCoverage(PrPathRasterizer* rasterizer, const PrPathPolyline* shape, const PrPathPolyline* caster,
        PrDrawShadowKind kind, float sigma, PrPathCoverage coverage, PrIVec2 origin, PrPixels out) {
    const uint32_t width = out.extent.x;
    const uint32_t height = out.extent.y;
    if (width == 0 || height == 0)
        return;
    const PrIRect box{origin.x, origin.y, origin.x + int32_t(width), origin.y + int32_t(height)};
    // Blurs at the resolution that holds the sigma to the limit, over the box and as far beyond it as the blur reaches.
    sigma = maxF32(sigma, 0);
    const float s = sigma > float(kPrDrawBlurMaxSigma) ? float(kPrDrawBlurMaxSigma) / sigma : 1;
    const int32_t reach = int32_t(ceilF32(sigma * s * 4)) + 1;
    const PrRect scaled{float(box.left) * s, float(box.top) * s, float(box.right) * s, float(box.bottom) * s};
    const PrIRect small = prIRectOutset(prIRectRoundOut(scaled), reach);
    const uint32_t smallWidth = prIRectWidth(small);
    const uint32_t smallHeight = prIRectHeight(small);
    const size_t count = size_t(smallWidth) * smallHeight;
    uint8_t* covered = static_cast<uint8_t*>(prMemoryAlloc(count));
    const PrPixels mask{covered, smallWidth, {smallWidth, smallHeight}};
    if (s < 1) {
        PrPathPolyline scaledShape{};
        prPathPolylineAddPolyline(&scaledShape, shape, prAffine2Scale(PrVec2{s, s}));
        prPathRasterizerFill(rasterizer, &scaledShape, PrPathFillRule_NonZero, coverage, PrIVec2{small.left, small.top}, mask);
        prPathPolylineReset(&scaledShape);
    } else {
        prPathRasterizerFill(rasterizer, shape, PrPathFillRule_NonZero, coverage, PrIVec2{small.left, small.top}, mask);
    }
    float* values = static_cast<float*>(prMemoryAlloc(count * sizeof(float)));
    for (size_t i = 0; i < count; ++i)
        values[i] = kind == PrDrawShadowKind_Inner ? 255 - float(covered[i]) : float(covered[i]);
    prMemoryFree(covered);
    PrPixels blurred{reinterpret_cast<uint8_t*>(values), size_t(smallWidth) * sizeof(float), {smallWidth, smallHeight}};
    prImageBlur(blurred, PrPixelFormat_RF32, PrVec2{sigma * s, sigma * s}, PrImageEdgeMode_None);
    // Rasterizes the caster's coverage at full resolution, then samples the blur at each pixel's center and cuts it by
    // the caster's coverage.
    prPathRasterizerFill(rasterizer, caster, PrPathFillRule_NonZero, coverage, origin, out);
    for (uint32_t y = 0; y < height; ++y) {
        float fy = clampF32((float(box.top + int32_t(y)) + 0.5f) * s - 0.5f - float(small.top), 0, float(smallHeight - 1));
        uint32_t y0 = uint32_t(fy);
        uint32_t y1 = y0 + 1 < smallHeight ? y0 + 1 : y0;
        float ty = fy - float(y0);
        uint8_t* row = out.data + size_t(y) * out.stride;
        for (uint32_t x = 0; x < width; ++x) {
            float fx = clampF32((float(box.left + int32_t(x)) + 0.5f) * s - 0.5f - float(small.left), 0,
                float(smallWidth - 1));
            uint32_t x0 = uint32_t(fx);
            uint32_t x1 = x0 + 1 < smallWidth ? x0 + 1 : x0;
            float tx = fx - float(x0);
            const float* r0 = values + size_t(y0) * smallWidth;
            const float* r1 = values + size_t(y1) * smallWidth;
            float upper = r0[x0] + (r0[x1] - r0[x0]) * tx;
            float lower = r1[x0] + (r1[x1] - r1[x0]) * tx;
            float value = upper + (lower - upper) * ty;
            float keep = kind == PrDrawShadowKind_Inner ? float(row[x]) / 255 : 1 - float(row[x]) / 255;
            row[x] = uint8_t(clampF32(value * keep, 0, 255) + 0.5f);
        }
    }
    prMemoryFree(values);
}

void prDrawDecorationOutline(const PrDrawDecoration* decoration, float tolerance, PrPathPolyline* out) {
    const PrDrawDecoration& d = *decoration;
    if (!(d.length > 0) || !(d.thickness > 0) || !isFiniteF32(d.length) || !isFiniteF32(d.thickness) || !(tolerance > 0))
        return;
    float half = d.thickness / 2;
    float left = d.origin.x;
    float right = d.origin.x + d.length;
    float y = d.origin.y;
    auto addRect = [&](PrRect r) {
        prPathPolylineMoveTo(out, PrVec2{r.left, r.top});
        prPathPolylineLineTo(out, PrVec2{r.right, r.top});
        prPathPolylineLineTo(out, PrVec2{r.right, r.bottom});
        prPathPolylineLineTo(out, PrVec2{r.left, r.bottom});
        prPathPolylineClose(out);
    };
    switch (d.style) {
        case PrDrawDecorationStyle_Double:
            addRect(PrRect{left, y - d.thickness - half, right, y - d.thickness + half});
            addRect(PrRect{left, y + d.thickness - half, right, y + d.thickness + half});
            return;
        case PrDrawDecorationStyle_Dotted: {
            uint32_t n = periodCount(d.length / (2 * d.thickness));
            float period = d.length / float(n);
            PrPath* dots = prPathCreate();
            for (uint32_t i = 0; i < n; ++i) {
                PrVec2 center{left + period * (float(i) + 0.5f), y};
                prPathEllipticArcTo(dots, center, PrVec2{half, half}, 0, 0, 2 * kPiF32);
                prPathClose(dots);
            }
            prPathFlatten(dots, prAffine2Identity(), tolerance, PrPathFlattening_Balanced, out);
            prPathDestroy(dots);
            return;
        }
        case PrDrawDecorationStyle_Dashed: {
            uint32_t n = periodCount(d.length / (5 * d.thickness));
            float period = d.length / float(n);
            for (uint32_t i = 0; i < n; ++i) {
                float x = left + period * float(i);
                addRect(PrRect{x, y - half, x + period * 3 / 5, y + half});
            }
            return;
        }
        case PrDrawDecorationStyle_Wavy: {
            if (!(d.wavelength > 0) || !isFiniteF32(d.wavelength) || !isFiniteF32(d.amplitude)) {
                addRect(PrRect{left, y - half, right, y + half});
                return;
            }
            // The centerline is made of lines whose chords stay within the tolerance of the sine. Their spacing comes
            // from the sine's greatest curvature: a chord over dx lies at most A k^2 dx^2 / 8 from the sine. A wave
            // with more periods than kPrDrawPatternMaxPeriods has its wavelength stretched to fit that many periods,
            // and it has at most kWaveMaxLines lines.
            float wavelength = maxF32(d.wavelength, d.length / float(kPrDrawPatternMaxPeriods));
            float k = 2 * kPiF32 / wavelength;
            float bend = absF32(d.amplitude) * k * k;
            float dx = bend > 0 ? sqrtF32(8 * tolerance / bend) : d.length;
            float count = ceilF32(d.length / dx);
            uint32_t lines = !(count >= 1) ? 1 : count > float(kWaveMaxLines) ? kWaveMaxLines : uint32_t(count);
            PrPath* wave = prPathCreate();
            for (uint32_t i = 0; i <= lines; ++i) {
                float x = d.length * float(i) / float(lines);
                PrVec2 p{left + x, y + d.amplitude * sinF32(k * (x + d.phase))};
                if (i == 0)
                    prPathMoveTo(wave, p);
                else
                    prPathLineTo(wave, p);
            }
            PrPathStroke stroke = prPathStrokeDefault();
            stroke.width = d.thickness;
            stroke.join = PrPathJoin_Round;
            prPathStrokeOutline(wave, &stroke, tolerance, out);
            prPathDestroy(wave);
            return;
        }
        default:
            addRect(PrRect{left, y - half, right, y + half});
            return;
    }
}

uint32_t prDrawNineSliceRects(const PrDrawNineSlice* slices, PrVec2 imageExtent, PrRect destination,
        PrDrawImageRect* PR_NULLABLE out, uint32_t capacity) {
    const PrDrawNineSlice& n = *slices;
    float imageWidth = imageExtent.x;
    float imageHeight = imageExtent.y;
    float st = clampF32(n.slices[PrRectSide_Top], 0, imageHeight);
    float sr = clampF32(n.slices[PrRectSide_Right], 0, imageWidth);
    float sb = clampF32(n.slices[PrRectSide_Bottom], 0, imageHeight);
    float sl = clampF32(n.slices[PrRectSide_Left], 0, imageWidth);
    float wt = maxF32(n.widths[PrRectSide_Top], 0);
    float wr = maxF32(n.widths[PrRectSide_Right], 0);
    float wb = maxF32(n.widths[PrRectSide_Bottom], 0);
    float wl = maxF32(n.widths[PrRectSide_Left], 0);
    float dw = prRectWidth(destination);
    float dh = prRectHeight(destination);
    float factor = 1;
    if (wl + wr > dw)
        factor = minF32(factor, dw / (wl + wr));
    if (wt + wb > dh)
        factor = minF32(factor, dh / (wt + wb));
    wt *= factor;
    wr *= factor;
    wb *= factor;
    wl *= factor;
    float sx[4] = {0, sl, maxF32(imageWidth - sr, sl), imageWidth};
    float sy[4] = {0, st, maxF32(imageHeight - sb, st), imageHeight};
    float dx[4] = {destination.left, destination.left + wl, destination.right - wr, destination.right};
    float dy[4] = {destination.top, destination.top + wt, destination.bottom - wb, destination.bottom};
    uint32_t count = 0;
    auto emit = [&](float s0x, float s1x, float s0y, float s1y, float d0x, float d1x, float d0y, float d1y) {
        if (!(s1x > s0x && s1y > s0y && d1x > d0x && d1y > d0y))
            return;
        if (out != nullptr && count < capacity)
            out[count] = PrDrawImageRect{PrRect{s0x, s0y, s1x, s1y}, PrRect{d0x, d0y, d1x, d1y}};
        ++count;
    };
    // Corners map whole.
    for (uint32_t row = 0; row < 3; row += 2) {
        for (uint32_t column = 0; column < 3; column += 2)
            emit(sx[column], sx[column + 1], sy[row], sy[row + 1], dx[column], dx[column + 1], dy[row], dy[row + 1]);
    }
    // Tiles each region along an axis, with the tile size scaled by the same factor as the edges across it.
    float topScale = st > 0 ? wt / st : sb > 0 ? wb / sb : 1;
    float leftScale = sl > 0 ? wl / sl : sr > 0 ? wr / sr : 1;
    Array<float> xs;
    Array<float> ys;
    auto region = [&](uint32_t column, uint32_t row, float tileScaleX, float tileScaleY) {
        xs.clear();
        ys.clear();
        float sw = sx[column + 1] - sx[column];
        float sh = sy[row + 1] - sy[row];
        tileAxis(column == 1 ? n.horizontal : PrDrawSliceMode_Stretch, sx[column], sw, dx[column],
            dx[column + 1] - dx[column], sw * tileScaleX, xs);
        tileAxis(row == 1 ? n.vertical : PrDrawSliceMode_Stretch, sy[row], sh, dy[row], dy[row + 1] - dy[row],
            sh * tileScaleY, ys);
        for (uint32_t j = 0; j < ys.count; j += 4) {
            for (uint32_t i = 0; i < xs.count; i += 4)
                emit(xs.data[i], xs.data[i + 1], ys.data[j], ys.data[j + 1], xs.data[i + 2],
                    xs.data[i + 3], ys.data[j + 2], ys.data[j + 3]);
        }
    };
    float topRow = st > 0 ? wt / st : 1;
    float bottomRow = sb > 0 ? wb / sb : 1;
    float leftColumn = sl > 0 ? wl / sl : 1;
    float rightColumn = sr > 0 ? wr / sr : 1;
    region(1, 0, topRow, topRow);
    region(1, 2, bottomRow, bottomRow);
    region(0, 1, leftColumn, leftColumn);
    region(2, 1, rightColumn, rightColumn);
    if (n.center)
        region(1, 1, topScale, leftScale);
    xs.reset();
    ys.reset();
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers

float prDrawLayerResolution(float reach) {
    return reach > float(kPrDrawFilterMaxReach) ? float(kPrDrawFilterMaxReach) / reach : 1;
}

PrUVec2 prDrawLayerExtent(PrVec2 extent, PrVec2 scale, uint32_t limit) {
    float w = extent.x * scale.x;
    float h = extent.y * scale.y;
    if (!(w > 0) || !(h > 0) || !isFiniteF32(w) || !isFiniteF32(h) || limit == 0)
        return PrUVec2{0, 0};
    float fit = minF32(1, minF32(float(limit) / w, float(limit) / h));
    return PrUVec2{
        uint32_t(clampF32(ceilF32(w * fit), 1, float(limit))),
        uint32_t(clampF32(ceilF32(h * fit), 1, float(limit))),
    };
}
