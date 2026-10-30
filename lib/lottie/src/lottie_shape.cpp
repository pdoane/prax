// A shape layer's contents: its shapes gathered into the styles that paint them, and drawn.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// Items are taken last to first. A style collects every shape after it in that order, in its group and in nested
// groups. Each shape is first changed by the modifiers found before it, nearest first, and then brought into the space
// of the style's group. The styles draw in the order they were found, each in its group, which draws under its
// transform and opacity. A style above merge paths, in its group or a nested group, draws nothing. The merge's shapes
// are drawn by the styles below it.

#include "lottie/src/lottie_shape.h"

#include "pr/float.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_modifier.h"
#include "lottie/src/lottie_render.h"

namespace pr_impl {

namespace {

void addVertex(LottieGeometry& g, PrVec2 point, PrVec2 in, PrVec2 out) {
    g.vertices.push(LottieVertex{point, in, out});
}

uint32_t beginContour(LottieGeometry& g, bool closed) {
    g.contours.push(LottieContour{g.vertices.count, 0, closed});
    return g.contours.count - 1;
}

void endContour(LottieGeometry& g, uint32_t contour) {
    LottieContour& c = g.contours.data[contour];
    c.count = g.vertices.count - c.first;
}

// Appends a vertex. The arguments are its point, its out tangent, then its in tangent.
void triple(LottieGeometry& g, float x, float y, float ox, float oy, float ix, float iy) {
    addVertex(g, PrVec2{x, y}, PrVec2{ix, iy}, PrVec2{ox, oy});
}

void rectShape(PrVec2 p, PrVec2 size, float roundness, bool reversed, LottieGeometry& g) {
    float v0 = size.x / 2;
    float v1 = size.y / 2;
    float round = minF32(minF32(v0, v1), roundness);
    float c = round * (1 - kLottieQuarterArc);
    float x = p.x;
    float y = p.y;
    uint32_t contour = beginContour(g, true);
    if (!reversed) {
        triple(g, x + v0, y - v1 + round, x + v0, y - v1 + round, x + v0, y - v1 + c);
        triple(g, x + v0, y + v1 - round, x + v0, y + v1 - c, x + v0, y + v1 - round);
        if (round != 0) {
            triple(g, x + v0 - round, y + v1, x + v0 - round, y + v1, x + v0 - c, y + v1);
            triple(g, x - v0 + round, y + v1, x - v0 + c, y + v1, x - v0 + round, y + v1);
            triple(g, x - v0, y + v1 - round, x - v0, y + v1 - round, x - v0, y + v1 - c);
            triple(g, x - v0, y - v1 + round, x - v0, y - v1 + c, x - v0, y - v1 + round);
            triple(g, x - v0 + round, y - v1, x - v0 + round, y - v1, x - v0 + c, y - v1);
            triple(g, x + v0 - round, y - v1, x + v0 - c, y - v1, x + v0 - round, y - v1);
        } else {
            triple(g, x - v0, y + v1, x - v0 + c, y + v1, x - v0, y + v1);
            triple(g, x - v0, y - v1, x - v0, y - v1 + c, x - v0, y - v1);
        }
    } else {
        triple(g, x + v0, y - v1 + round, x + v0, y - v1 + c, x + v0, y - v1 + round);
        if (round != 0) {
            triple(g, x + v0 - round, y - v1, x + v0 - round, y - v1, x + v0 - c, y - v1);
            triple(g, x - v0 + round, y - v1, x - v0 + c, y - v1, x - v0 + round, y - v1);
            triple(g, x - v0, y - v1 + round, x - v0, y - v1 + round, x - v0, y - v1 + c);
            triple(g, x - v0, y + v1 - round, x - v0, y + v1 - c, x - v0, y + v1 - round);
            triple(g, x - v0 + round, y + v1, x - v0 + round, y + v1, x - v0 + c, y + v1);
            triple(g, x + v0 - round, y + v1, x + v0 - c, y + v1, x + v0 - round, y + v1);
            triple(g, x + v0, y + v1 - round, x + v0, y + v1 - round, x + v0, y + v1 - c);
        } else {
            triple(g, x - v0, y - v1, x - v0 + c, y - v1, x - v0, y - v1);
            triple(g, x - v0, y + v1, x - v0, y + v1 - c, x - v0, y + v1);
            triple(g, x + v0, y + v1, x + v0 - c, y + v1, x + v0, y + v1);
        }
    }
    endContour(g, contour);
}

void ellipseShape(PrVec2 p, PrVec2 size, bool reversed, LottieGeometry& g) {
    float s0 = size.x / 2;
    float s1 = size.y / 2;
    float k = kLottieQuarterArc;
    bool cw = !reversed;
    float x = p.x;
    float y = p.y;
    uint32_t contour = beginContour(g, true);
    addVertex(g, PrVec2{x, y - s1}, PrVec2{cw ? x - s0 * k : x + s0 * k, y - s1}, PrVec2{cw ? x + s0 * k : x - s0 * k, y - s1});
    addVertex(g, PrVec2{cw ? x + s0 : x - s0, y}, PrVec2{cw ? x + s0 : x - s0, y - s1 * k},
        PrVec2{cw ? x + s0 : x - s0, y + s1 * k});
    addVertex(g, PrVec2{x, y + s1}, PrVec2{cw ? x + s0 * k : x - s0 * k, y + s1}, PrVec2{cw ? x - s0 * k : x + s0 * k, y + s1});
    addVertex(g, PrVec2{cw ? x - s0 : x + s0, y}, PrVec2{cw ? x - s0 : x + s0, y + s1 * k},
        PrVec2{cw ? x - s0 : x + s0, y - s1 * k});
    endContour(g, contour);
}

void starShape(const LottieAnimation& a, const LottieItem& item, double frame, LottieGeometry& g) {
    PrVec2 p = lottiePoint(a, item.props[LottieProp_Position], frame, PrVec2{0, 0});
    double points = floorF64(double(lottieScalar(a, item.props[LottieProp_Points], frame, 5)));
    double rotation = double(lottieScalar(a, item.props[LottieProp_Rotation], frame, 0)) * kPiF64 / 180;
    double outer = double(lottieScalar(a, item.props[LottieProp_OuterRadius], frame, 0));
    double outerRound = double(lottieScalar(a, item.props[LottieProp_OuterRoundness], frame, 0)) / 100;
    bool star = item.mode != 2;
    double inner = star ? double(lottieScalar(a, item.props[LottieProp_InnerRadius], frame, 0)) : outer;
    double innerRound = star ? double(lottieScalar(a, item.props[LottieProp_InnerRoundness], frame, 0)) / 100 : 0;
    if (!(points >= 1) || points > 10000)
        return;
    uint32_t count = star ? uint32_t(points) * 2 : uint32_t(points);
    double angle = 2 * kPiF64 / count;
    // The length by which a corner's roundness scales its tangents: the circumference at the corner's radius, divided
    // by four times the number of points.
    double outerSegment = 2 * kPiF64 * outer / (star ? count * 2 : count * 4);
    double innerSegment = 2 * kPiF64 * inner / (count * 2);
    double current = -kPiF64 / 2 + rotation;
    double dir = item.reversed ? -1 : 1;
    uint32_t contour = beginContour(g, true);
    for (uint32_t i = 0; i < count; ++i) {
        bool longPoint = !star || i % 2 == 0;
        double radius = longPoint ? outer : inner;
        double roundness = longPoint ? outerRound : innerRound;
        double segment = longPoint ? outerSegment : innerSegment;
        double x = radius * cosF64(current);
        double y = radius * sinF64(current);
        double length = sqrtF64(x * x + y * y);
        double ox = x == 0 && y == 0 ? 0 : y / length;
        double oy = x == 0 && y == 0 ? 0 : -x / length;
        x += double(p.x);
        y += double(p.y);
        double t = segment * roundness * dir;
        triple(g, float(x), float(y), float(x - ox * t), float(y - oy * t), float(x + ox * t), float(y + oy * t));
        current += angle * dir;
    }
    endContour(g, contour);
}

// ---------------------------------------------------------------------------------------------------------------------
// Gathering

// Reads a group's transform and opacity from its Transform item, or from the last one if there are several.
void groupTransform(const LottieAnimation& a, const LottieItem& group, double frame, LottieShapeGroup& out) {
    out.transform = prAffine2Identity();
    out.opacity = 1;
    for (uint32_t i = group.count; i-- > 0;) {
        const LottieItem& item = a.items.data[group.first + i];
        if (item.kind != LottieItemKind_Transform || item.transform == kLottieNone)
            continue;
        const LottieTransform& t = a.transforms.data[item.transform];
        out.transform = lottieTransformAffine(a, t, frame);
        out.opacity = lottieTransformOpacity(a, t, frame);
        return;
    }
}

PrPath* stylePath(LottieRender& r, uint32_t style) {
    while (r.paths.count <= style)
        r.paths.push(prPathCreate());
    PrPath* path = r.paths.data[style];
    prPathClear(path);
    return path;
}

// Appends the shape item's geometry at `frame` as contours.
void shapeGeometry(LottieRender& r, const LottieItem& item, double frame) {
    const LottieAnimation& a = *r.a;
    switch (item.kind) {
        case LottieItemKind_Path: {
            uint32_t dims = lottieDims(a, item.props[LottieProp_Shape]);
            if (dims < 2)
                return;
            r.values.clear();
            r.values.reserve(dims);
            lottieEvaluate(a, item.props[LottieProp_Shape], frame, r.values.data);
            lottieShapeFromValue(r.values.data, item.reversed, r.geometry);
            return;
        }
        case LottieItemKind_Rect:
            rectShape(lottiePoint(a, item.props[LottieProp_Position], frame, PrVec2{0, 0}),
                lottiePoint(a, item.props[LottieProp_Size], frame, PrVec2{0, 0}),
                lottieScalar(a, item.props[LottieProp_Roundness], frame, 0), item.reversed, r.geometry);
            return;
        case LottieItemKind_Ellipse:
            ellipseShape(lottiePoint(a, item.props[LottieProp_Position], frame, PrVec2{0, 0}),
                lottiePoint(a, item.props[LottieProp_Size], frame, PrVec2{0, 0}), item.reversed, r.geometry);
            return;
        case LottieItemKind_Star:
            starShape(a, item, frame, r.geometry);
            return;
        default:
            return;
    }
}

bool isStyle(LottieItemKind kind) {
    return kind == LottieItemKind_Fill || kind == LottieItemKind_Stroke || kind == LottieItemKind_GradientFill ||
        kind == LottieItemKind_GradientStroke;
}

bool isShape(LottieItemKind kind) {
    return kind == LottieItemKind_Path || kind == LottieItemKind_Rect || kind == LottieItemKind_Ellipse ||
        kind == LottieItemKind_Star;
}

bool isModifier(LottieItemKind kind) {
    return kind == LottieItemKind_Trim || kind == LottieItemKind_RoundCorners || kind == LottieItemKind_Repeater ||
        kind == LottieItemKind_PuckerBloat || kind == LottieItemKind_ZigZag || kind == LottieItemKind_Twist ||
        kind == LottieItemKind_OffsetPath || kind == LottieItemKind_Merge;
}

// The styles and modifiers in effect as items are found, innermost last.
struct Active {
    Array<uint32_t> styles;
    Array<uint32_t> modifiers;
    bool merged;                // true if a merge was found, so that the styles above it draw nothing
};

void gather(LottieRender& r, Active& active, uint32_t first, uint32_t count, uint32_t group, double frame);

// Returns the transform of a repeater's copy that is `times` steps of the repeater's transform in. The transform
// rotates and scales about the anchor, then moves.
PrAffine2 repeaterStep(const LottieAnimation& a, const LottieTransform& t, double frame, double times) {
    PrVec2 anchor = lottiePoint(a, t.anchor, frame, PrVec2{0, 0});
    PrVec2 scale = lottiePoint(a, t.scale, frame, PrVec2{100, 100});
    double rotation = double(lottieScalar(a, t.rotation, frame, 0)) * kPiF64 / 180;
    PrVec2 position = lottieTransformPosition(a, t, frame);
    // A whole step scales by the full scale. A partial step scales only part of the way.
    double whole = times >= 0 ? floorF64(times) : ceilF64(times);
    double part = times - whole;
    auto stepScale = [&](float s, double perc) { return double(s) / 100 + (1 - double(s) / 100) * (1 - perc); };
    double sx = 1;
    double sy = 1;
    double steps = whole >= 0 ? whole : -whole;
    for (double k = 0; k < steps; ++k) {
        double fx = stepScale(scale.x, 1);
        double fy = stepScale(scale.y, 1);
        sx *= whole >= 0 ? fx : 1 / fx;
        sy *= whole >= 0 ? fy : 1 / fy;
    }
    if (part != 0) {
        double perc = part >= 0 ? part : -part;
        double fx = stepScale(scale.x, perc);
        double fy = stepScale(scale.y, perc);
        sx *= part >= 0 ? fx : 1 / fx;
        sy *= part >= 0 ? fy : 1 / fy;
    }
    PrAffine2 about = prAffine2Translate(-anchor);
    PrAffine2 back = prAffine2Translate(anchor);
    PrAffine2 m = prAffine2Concat(about, prAffine2Rotate(float(rotation * times)));
    m = prAffine2Concat(m, back);
    m = prAffine2Concat(m, about);
    m = prAffine2Concat(m, prAffine2Scale(PrVec2{float(sx), float(sy)}));
    m = prAffine2Concat(m, back);
    PrVec2 moved{float(double(position.x) * times), float(double(position.y) * times)};
    return prAffine2Concat(m, prAffine2Translate(moved));
}

// Applies a repeater. The items before it, `first` to `before`, are drawn once per copy. Each copy is a group with its
// own steps of the repeater's transform, and its share of the opacity from start to end. The first copy is on top when
// copies stack above, and at the bottom when they stack below.
void repeat(LottieRender& r, Active& active, const LottieItem& item, uint32_t first, uint32_t before, uint32_t group,
        double frame) {
    const LottieAnimation& a = *r.a;
    if (item.transform == kLottieNone || before == 0)
        return;
    const LottieTransform& t = a.transforms.data[item.transform];
    double copies = ceilF64(double(lottieScalar(a, item.props[LottieProp_Copies], frame, 1)));
    if (!(copies >= 1) || copies > 10000)
        return;
    uint32_t n = uint32_t(copies);
    double offset = double(lottieScalar(a, item.props[LottieProp_Offset], frame, 0));
    float startOpacity = lottieScalar(a, t.startOpacity, frame, 100) / 100;
    float endOpacity = lottieScalar(a, t.endOpacity, frame, 100) / 100;
    // Copies stack by their position in the list. The copy listed first is on top, and is found last.
    for (uint32_t place = n; place-- > 0;) {
        uint32_t steps = item.mode == 2 ? n - 1 - place : place;
        LottieShapeGroup g{};
        g.parent = group;
        g.blend = PrDrawBlendMode_Normal;
        g.transform = repeaterStep(a, t, frame, offset + double(steps));
        g.opacity = n == 1 ? startOpacity : startOpacity + (endOpacity - startOpacity) * float(place) / float(n - 1);
        uint32_t inner = r.groups.count;
        r.groups.push(g);
        r.steps.push(LottieShapeStep{LottieShapeOp_Open, inner});
        gather(r, active, first, before, inner, frame);
        r.steps.push(LottieShapeStep{LottieShapeOp_Close, inner});
    }
}

void gather(LottieRender& r, Active& active, uint32_t first, uint32_t count, uint32_t group, double frame) {
    const LottieAnimation& a = *r.a;
    uint32_t styleMark = active.styles.count;
    uint32_t modifierMark = active.modifiers.count;
    bool merged = active.merged;
    for (uint32_t i = count; i-- > 0;) {
        uint32_t index = first + i;
        const LottieItem& item = a.items.data[index];
        if (item.hidden)
            continue;
        if (isStyle(item.kind)) {
            if (active.merged)
                continue;
            LottieShapeStyle added{index, group, stylePath(r, r.styles.count)};
            uint32_t style = r.styles.count;
            r.styles.push(added);
            r.steps.push(LottieShapeStep{LottieShapeOp_Style, style});
            active.styles.push(style);
        } else if (item.kind == LottieItemKind_Group) {
            LottieShapeGroup g{};
            g.parent = group;
            g.blend = item.blend;
            groupTransform(a, item, frame, g);
            uint32_t inner = r.groups.count;
            r.groups.push(g);
            r.steps.push(LottieShapeStep{LottieShapeOp_Open, inner});
            gather(r, active, item.first, item.count, inner, frame);
            r.steps.push(LottieShapeStep{LottieShapeOp_Close, inner});
        } else if (isShape(item.kind)) {
            LottieShapeEntry e{};
            e.item = index;
            e.group = group;
            uint32_t firstContour = r.geometry.contours.count;
            shapeGeometry(r, item, frame);
            e.contours = r.refs.count;
            e.contourCount = r.geometry.contours.count - firstContour;
            for (uint32_t c = firstContour; c < r.geometry.contours.count; ++c)
                r.refs.push(c);
            e.styles = r.refs.count;
            e.styleCount = active.styles.count;
            for (uint32_t s = 0; s < active.styles.count; ++s)
                r.refs.push(active.styles.data[s]);
            e.modifiers = r.refs.count;
            e.modifierCount = active.modifiers.count;
            for (uint32_t m = active.modifiers.count; m-- > 0;)
                r.refs.push(active.modifiers.data[m]);
            r.shapes.push(e);
        } else if (item.kind == LottieItemKind_Repeater) {
            repeat(r, active, item, first, i, group, frame);
            break;
        } else if (isModifier(item.kind)) {
            uint32_t modifier = r.modifiers.count;
            r.modifiers.push(LottieShapeModifier{index, group});
            active.modifiers.push(modifier);
            active.merged = active.merged || item.kind == LottieItemKind_Merge;
        }
    }
    active.styles.count = styleMark;
    active.modifiers.count = modifierMark;
    active.merged = merged;
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

float channel(float value) {
    return clampF32(value, 0, 1);
}

// Gathers the gradient's stops at `frame`: its color stops and opacity stops, merged at every offset that either has.
void gradientStops(LottieRender& r, const LottieItem& item, double frame, float opacity) {
    const LottieAnimation& a = *r.a;
    r.stops.clear();
    uint32_t dims = lottieDims(a, item.props[LottieProp_Stops]);
    uint32_t colors = item.stops;
    if (colors == 0 || colors > dims / 4)
        return;
    r.values.clear();
    r.values.reserve(dims);
    lottieEvaluate(a, item.props[LottieProp_Stops], frame, r.values.data);
    const float* v = r.values.data;
    uint32_t alphas = (dims - colors * 4) / 2;
    const float* o = v + colors * 4;
    auto colorAt = [&](float t) {
        if (t <= v[0])
            return PrColor{v[1], v[2], v[3], 1};
        for (uint32_t i = 1; i < colors; ++i) {
            const float* s = v + (i - 1) * 4;
            const float* e = v + i * 4;
            if (t <= e[0]) {
                float f = e[0] > s[0] ? (t - s[0]) / (e[0] - s[0]) : 1;
                return PrColor{s[1] + (e[1] - s[1]) * f, s[2] + (e[2] - s[2]) * f, s[3] + (e[3] - s[3]) * f, 1};
            }
        }
        const float* last = v + (colors - 1) * 4;
        return PrColor{last[1], last[2], last[3], 1};
    };
    auto alphaAt = [&](float t) {
        if (alphas == 0)
            return 1.0f;
        if (t <= o[0])
            return o[1];
        for (uint32_t i = 1; i < alphas; ++i) {
            const float* s = o + (i - 1) * 2;
            const float* e = o + i * 2;
            if (t <= e[0]) {
                float f = e[0] > s[0] ? (t - s[0]) / (e[0] - s[0]) : 1;
                return s[1] + (e[1] - s[1]) * f;
            }
        }
        return o[(alphas - 1) * 2 + 1];
    };
    // Every offset that either list has, in order. Each list is sorted, so this is a merge.
    uint32_t ci = 0;
    uint32_t oi = 0;
    while (ci < colors || oi < alphas) {
        float t;
        if (oi >= alphas || (ci < colors && v[ci * 4] <= o[oi * 2]))
            t = v[ci++ * 4];
        else
            t = o[oi++ * 2];
        if (r.stops.count > 0 && r.stops.last().offset == t && alphas > 0)
            continue;
        PrColor c = colorAt(t);
        c.a = alphaAt(t) * opacity;
        r.stops.push(PrDrawColorStop{.offset = t, .color = c});
    }
}

bool gradientBrush(LottieRender& r, const LottieItem& item, double frame, float opacity, PrDrawBrush* brush) {
    const LottieAnimation& a = *r.a;
    gradientStops(r, item, frame, opacity);
    if (r.stops.count == 0)
        return false;
    PrVec2 s = lottiePoint(a, item.props[LottieProp_Start], frame, PrVec2{0, 0});
    PrVec2 e = lottiePoint(a, item.props[LottieProp_End], frame, PrVec2{0, 0});
    if (item.mode != 2) {
        *brush = prDrawBrushLinear(s, e, r.stops.data, r.stops.count);
        return true;
    }
    float radius = sqrtF32((e.x - s.x) * (e.x - s.x) + (e.y - s.y) * (e.y - s.y));
    float angle = atan2F32(e.y - s.y, e.x - s.x);
    float highlight = lottieScalar(a, item.props[LottieProp_HighlightLength], frame, 0) / 100;
    highlight = clampF32(highlight, -0.99f, 0.99f);
    float highlightAngle = lottieScalar(a, item.props[LottieProp_HighlightAngle], frame, 0) * kPiF32 / 180;
    float dist = radius * highlight;
    PrVec2 focal{s.x + cosF32(angle + highlightAngle) * dist, s.y + sinF32(angle + highlightAngle) * dist};
    *brush = prDrawBrushRadial(focal, 0, s, radius, r.stops.data, r.stops.count);
    return true;
}

bool strokeOf(LottieRender& r, const LottieItem& item, double frame, PrDrawStroke* stroke) {
    const LottieAnimation& a = *r.a;
    float width = lottieScalar(a, item.props[LottieProp_Width], frame, 0);
    if (!(width > 0))
        return false;
    *stroke = prDrawStrokeDefault();
    stroke->shape.width = width;
    stroke->shape.miterLimit = item.miterLimit;
    stroke->shape.join = item.join;
    stroke->shape.startCap = item.cap;
    stroke->shape.endCap = item.cap;
    stroke->shape.dashCap = item.cap;
    r.dashes.clear();
    for (uint32_t i = 0; i < item.count; ++i) {
        const LottieDash& dash = a.dashes.data[item.first + i];
        float value = lottieScalar(a, dash.value, frame, 0);
        if (dash.kind == LottieDashKind_Offset)
            stroke->shape.dashOffset = value;
        else
            r.dashes.push(value);
    }
    if (r.dashes.count > 0) {
        stroke->shape.dashes = r.dashes.data;
        stroke->shape.dashCount = r.dashes.count;
    }
    return true;
}

void paintStyle(LottieRender& r, const LottieShapeStyle& style, double frame) {
    const LottieAnimation& a = *r.a;
    const LottieItem& item = a.items.data[style.item];
    if (prPathVerbCount(style.path) == 0)
        return;
    float opacity = lottieScalar(a, item.props[LottieProp_Opacity], frame, 100) / 100;
    if (!(opacity > 0))
        return;
    PrDrawStroke stroke{};
    bool stroked = item.kind == LottieItemKind_Stroke || item.kind == LottieItemKind_GradientStroke;
    if (stroked && !strokeOf(r, item, frame, &stroke))
        return;
    PrPathFillRule rule = stroked ? PrPathFillRule_NonZero : item.rule;
    PrDrawPaint paint{.stroke = stroked ? &stroke : nullptr, .blendMode = item.blend};
    PrDrawBrush brush{};
    if (item.kind == LottieItemKind_Fill || item.kind == LottieItemKind_Stroke) {
        float c[4] = {0, 0, 0, 1};
        uint32_t dims = lottieDims(a, item.props[LottieProp_Color]);
        if (dims >= 3 && dims <= 4)
            lottieEvaluate(a, item.props[LottieProp_Color], frame, c);
        paint.color = PrColor{channel(c[0]), channel(c[1]), channel(c[2]), opacity};
    } else {
        if (!gradientBrush(r, item, frame, opacity, &brush))
            return;
        paint.brush = &brush;
    }
    prDrawListPath(r.list, style.path, rule, paint);
}

} // namespace

void lottieShapeFromValue(const float* value, bool reversed, LottieGeometry& g) {
    uint32_t count = lottieShapeCount(value);
    uint32_t contour = beginContour(g, lottieShapeClosed(value));
    const float* v = value + 2;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t j = reversed ? (count - k) % count : k;
        const float* at = v + j * 6;
        PrVec2 point{at[0], at[1]};
        PrVec2 in{at[2], at[3]};
        PrVec2 out{at[4], at[5]};
        if (reversed)
            addVertex(g, point, out, in);
        else
            addVertex(g, point, in, out);
    }
    endContour(g, contour);
}

PrAffine2 lottieGroupToGroup(const LottieRender& r, uint32_t group, uint32_t ancestor) {
    PrAffine2 m = prAffine2Identity();
    for (uint32_t g = group; g != ancestor && g != kLottieNone; g = r.groups.data[g].parent)
        m = prAffine2Concat(m, r.groups.data[g].transform);
    return m;
}

void lottieAddContour(const LottieGeometry& g, const LottieContour& c, PrAffine2 transform, PrPath* path) {
    if (c.count == 0)
        return;
    const LottieVertex* v = g.vertices.data + c.first;
    auto map = [&](PrVec2 p) { return prAffine2MapPoint(transform, p); };
    prPathMoveTo(path, map(v[0].point));
    for (uint32_t i = 1; i < c.count; ++i)
        prPathCubicTo(path, map(v[i - 1].out), map(v[i].in), map(v[i].point));
    if (c.closed) {
        prPathCubicTo(path, map(v[c.count - 1].out), map(v[0].in), map(v[0].point));
        prPathClose(path);
    }
}

void lottieItemPaths(LottieRender& r, uint32_t first, uint32_t count, double frame, PrAffine2 transform, PrPath* path) {
    const LottieAnimation& a = *r.a;
    for (uint32_t i = count; i-- > 0;) {
        const LottieItem& item = a.items.data[first + i];
        if (item.hidden)
            continue;
        if (item.kind == LottieItemKind_Group) {
            LottieShapeGroup g{};
            groupTransform(a, item, frame, g);
            lottieItemPaths(r, item.first, item.count, frame, prAffine2Concat(g.transform, transform), path);
        } else if (isShape(item.kind)) {
            LottieGeometry geometry{};
            LottieGeometry saved = r.geometry;
            r.geometry = geometry;
            shapeGeometry(r, item, frame);
            for (uint32_t c = 0; c < r.geometry.contours.count; ++c)
                lottieAddContour(r.geometry, r.geometry.contours.data[c], transform, path);
            r.geometry.vertices.reset();
            r.geometry.contours.reset();
            r.geometry = saved;
        }
    }
}

void lottieDrawShapes(LottieRender& r, const LottieLayer& layer, double frame) {
    r.geometry.vertices.clear();
    r.geometry.contours.clear();
    r.groups.clear();
    r.shapes.clear();
    r.styles.clear();
    r.modifiers.clear();
    r.steps.clear();
    r.refs.clear();
    Active active{};
    gather(r, active, layer.first, layer.count, kLottieNone, frame);
    active.styles.reset();
    active.modifiers.reset();
    lottieApplyModifiers(r, frame);

    // Adds each shape to each of its styles, mapped from the space of the shape's group into the space of the style's
    // group.
    for (uint32_t s = 0; s < r.shapes.count; ++s) {
        const LottieShapeEntry& e = r.shapes.data[s];
        for (uint32_t k = 0; k < e.styleCount; ++k) {
            const LottieShapeStyle& style = r.styles.data[r.refs.data[e.styles + k]];
            PrAffine2 m = lottieGroupToGroup(r, e.group, style.group);
            for (uint32_t c = 0; c < e.contourCount; ++c)
                lottieAddContour(r.geometry, r.geometry.contours.data[r.refs.data[e.contours + c]], m, style.path);
        }
    }

    for (uint32_t i = 0; i < r.steps.count; ++i) {
        const LottieShapeStep& step = r.steps.data[i];
        if (step.op == LottieShapeOp_Style) {
            paintStyle(r, r.styles.data[step.index], frame);
            continue;
        }
        const LottieShapeGroup& g = r.groups.data[step.index];
        bool grouped = g.opacity < 1 || g.blend != PrDrawBlendMode_Normal;
        if (step.op == LottieShapeOp_Open) {
            prDrawListPushTransformAffine2(r.list, g.transform);
            if (grouped) {
                PrDrawGroup group = prDrawGroupDefault();
                group.opacity = clampF32(g.opacity, 0, 1);
                group.blendMode = g.blend;
                prDrawListPushGroup(r.list, &group);
            }
        } else {
            if (grouped)
                prDrawListPopGroup(r.list);
            prDrawListPopTransform(r.list);
        }
    }
}

} // namespace pr_impl
