// A shape layer's modifiers. Each rewrites the contours of the shapes it applies to.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#include "lottie/src/lottie_modifier.h"

#include "pr/float.h"
#include "pr/path_polyline.h"
#include "pr/vec.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_render.h"
#include "lottie/src/lottie_shape.h"

namespace pr_impl {

namespace {

// The number of points at which a segment's length is measured.
constexpr uint32_t kLengthSamples = 150;

// The tolerance of a modifier's region operations against curves, in the shape's units.
constexpr float kModifierTolerance = 0.05f;

// The maximum number of ridges that a zig-zag puts on one segment.
constexpr float kZigZagMaxRidges = 1000;

float distance(PrVec2 a, PrVec2 b) {
    return sqrtF32((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

struct Cubic {
    PrVec2 p[4];
};

PrVec2 cubicAt(const Cubic& c, float t) {
    float u = 1 - t;
    float a = u * u * u;
    float b = 3 * u * u * t;
    float d = 3 * u * t * t;
    float e = t * t * t;
    return PrVec2{a * c.p[0].x + b * c.p[1].x + d * c.p[2].x + e * c.p[3].x,
                   a * c.p[0].y + b * c.p[1].y + d * c.p[2].y + e * c.p[3].y};
}

PrVec2 cubicDerivative(const Cubic& c, float t) {
    float u = 1 - t;
    auto axis = [&](float p0, float p1, float p2, float p3) {
        return 3 * u * u * (p1 - p0) + 6 * u * t * (p2 - p1) + 3 * t * t * (p3 - p2);
    };
    return PrVec2{axis(c.p[0].x, c.p[1].x, c.p[2].x, c.p[3].x), axis(c.p[0].y, c.p[1].y, c.p[2].y, c.p[3].y)};
}

// Returns the part of the cubic from `t0` to `t1`.
Cubic cubicPart(const Cubic& c, float t0, float t1) {
    auto split = [](const Cubic& in, float t, Cubic* before, Cubic* after) {
        PrVec2 a = prVec2Lerp(in.p[0], in.p[1], t);
        PrVec2 b = prVec2Lerp(in.p[1], in.p[2], t);
        PrVec2 d = prVec2Lerp(in.p[2], in.p[3], t);
        PrVec2 ab = prVec2Lerp(a, b, t);
        PrVec2 bd = prVec2Lerp(b, d, t);
        PrVec2 m = prVec2Lerp(ab, bd, t);
        if (before != nullptr)
            *before = Cubic{{in.p[0], a, ab, m}};
        if (after != nullptr)
            *after = Cubic{{m, bd, d, in.p[3]}};
    };
    Cubic after{};
    split(c, t0, nullptr, &after);
    if (t0 >= 1)
        return Cubic{{c.p[3], c.p[3], c.p[3], c.p[3]}};
    Cubic part{};
    split(after, (t1 - t0) / (1 - t0), &part, nullptr);
    return part;
}

// Writes the cubic's cumulative lengths at kLengthSamples points into `lengths`, and returns its total length.
float cubicLengths(const Cubic& c, float* lengths) {
    float total = 0;
    PrVec2 last = c.p[0];
    lengths[0] = 0;
    for (uint32_t k = 1; k < kLengthSamples; ++k) {
        PrVec2 p = cubicAt(c, float(k) / (kLengthSamples - 1));
        total += distance(last, p);
        lengths[k] = total;
        last = p;
    }
    return total;
}

// Returns the parameter at `fraction` of the cubic's length, using its cumulative lengths.
float cubicParameter(const float* lengths, float fraction) {
    float total = lengths[kLengthSamples - 1];
    if (!(total > 0) || fraction <= 0)
        return fraction <= 0 ? 0 : fraction >= 1 ? 1 : fraction;
    if (fraction >= 1)
        return 1;
    float target = total * fraction;
    for (uint32_t k = 1; k < kLengthSamples; ++k) {
        if (target <= lengths[k]) {
            float span = lengths[k] - lengths[k - 1];
            float f = span > 0 ? (target - lengths[k - 1]) / span : 0;
            return (float(k - 1) + f) / (kLengthSamples - 1);
        }
    }
    return 1;
}

uint32_t segmentCount(const LottieContour& c) {
    return c.count == 0 ? 0 : c.closed ? c.count : c.count - 1;
}

Cubic segmentOf(const LottieGeometry& g, const LottieContour& c, uint32_t i) {
    const LottieVertex& a = g.vertices.data[c.first + i];
    const LottieVertex& b = g.vertices.data[c.first + (i + 1) % c.count];
    return Cubic{{a.point, a.out, b.in, b.point}};
}

// Copies the shape's current contours as indices into the geometry, so that they stay valid when the render's refs are
// appended to.
void shapeContours(const LottieRender& r, const LottieShapeEntry& e, Array<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < e.contourCount; ++i)
        out.push(r.refs.data[e.contours + i]);
}

// Replaces the shape's contours with the contours that the modifier appended, from `first` on.
void replaceContours(LottieRender& r, LottieShapeEntry& e, uint32_t first) {
    e.contours = r.refs.count;
    e.contourCount = r.geometry.contours.count - first;
    for (uint32_t c = first; c < r.geometry.contours.count; ++c)
        r.refs.push(c);
}

// A contour being built. Vertices are appended, and its closedness is decided at the end.
struct Builder {
    LottieGeometry* g;
    uint32_t contour;
    bool open = false;

    void start() {
        contour = g->contours.count;
        g->contours.push(LottieContour{g->vertices.count, 0, false});
        open = true;
    }
    // Adds a cubic continuing the contour, or starting one if none is open.
    void add(const Cubic& c) {
        if (!open) {
            start();
            g->vertices.push(LottieVertex{c.p[0], c.p[0], c.p[1]});
        } else {
            g->vertices.last().out = c.p[1];
        }
        g->vertices.push(LottieVertex{c.p[3], c.p[2], c.p[3]});
    }
    void finish(bool closed) {
        if (!open)
            return;
        LottieContour& c = g->contours.data[contour];
        c.count = g->vertices.count - c.first;
        c.closed = closed;
        open = false;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Trim paths

struct Trimmed {
    Array<float> lengths;           // the cumulative samples of each segment, in order
    Array<float> segmentLengths;
    float total;
};

void measureShape(const LottieRender& r, const Array<uint32_t>& contours, Trimmed& t) {
    t.lengths.clear();
    t.segmentLengths.clear();
    t.total = 0;
    for (uint32_t i = 0; i < contours.count; ++i) {
        const LottieContour& c = r.geometry.contours.data[contours.data[i]];
        for (uint32_t s = 0; s < segmentCount(c); ++s) {
            uint32_t at = t.lengths.count;
            for (uint32_t k = 0; k < kLengthSamples; ++k)
                t.lengths.push(0);
            float length = cubicLengths(segmentOf(r.geometry, c, s), t.lengths.data + at);
            t.segmentLengths.push(length);
            t.total += length;
        }
    }
}

// Appends the part of the shape's contours from `start` to `end`, measured along them in the shape's units. Joins onto
// the builder's open contour if there is one. A contour taken whole stays closed. `keepOpen` leaves the last contour
// open so that more can join it.
void trimShape(LottieRender& r, const Array<uint32_t>& contours, const Trimmed& t, float start, float end,
               bool keepOpen, Builder& b) {
    float added = 0;
    uint32_t segment = 0;
    for (uint32_t i = 0; i < contours.count; ++i) {
        const LottieContour c = r.geometry.contours.data[contours.data[i]];
        uint32_t count = segmentCount(c);
        bool whole = c.closed;
        uint32_t taken = 0;
        for (uint32_t s = 0; s < count; ++s, ++segment) {
            float length = t.segmentLengths.data[segment];
            if (added + length < start || added > end) {
                whole = false;
                added += length;
                continue;
            }
            Cubic cubic = segmentOf(r.geometry, c, s);
            if (start <= added && end >= added + length) {
                b.add(cubic);
            } else {
                const float* lengths = t.lengths.data + size_t(segment) * kLengthSamples;
                float t0 = length > 0 ? cubicParameter(lengths, (start - added) / length) : 0;
                float t1 = length > 0 ? cubicParameter(lengths, (end - added) / length) : 1;
                b.add(cubicPart(cubic, t0, t1));
                whole = false;
            }
            ++taken;
            added += length;
        }
        if (added > end || i + 1 == contours.count) {
            if (taken > 0 && !keepOpen)
                b.finish(whole && taken == count);
            break;
        }
        if (taken > 0)
            b.finish(whole && taken == count);
    }
}

void trim(LottieRender& r, const LottieItem& item, double frame, Array<uint32_t>& shapes) {
    const LottieAnimation& a = *r.a;
    float s = lottieScalar(a, item.props[LottieProp_Start], frame, 0) / 100;
    float e = lottieScalar(a, item.props[LottieProp_End], frame, 100) / 100;
    float o = fmodF32(lottieScalar(a, item.props[LottieProp_Offset], frame, 0), 360) / 360;
    if (o < 0)
        o += 1;
    s = (s > 1 ? 1 : s < 0 ? 0 : s) + o;
    e = (e > 1 ? 1 : e < 0 ? 0 : e) + o;
    if (s > e) {
        float swap = s;
        s = e;
        e = swap;
    }
    s = roundF32(s * 10000) * 0.0001f;
    e = roundF32(e * 10000) * 0.0001f;
    if ((s == 0 && e == 1) || (s == 1 && e == 0))
        return;
    Array<uint32_t> contours;
    Trimmed t{};
    // With Individually, the shapes form one length in the order they are listed, which is the reverse of the order in
    // which they were found.
    float totalAll = 0;
    if (item.mode == 2 && shapes.count > 1) {
        for (uint32_t i = 0; i < shapes.count; ++i) {
            shapeContours(r, r.shapes.data[shapes.data[i]], contours);
            measureShape(r, contours, t);
            totalAll += t.total;
        }
    }
    float addedAll = 0;
    for (uint32_t i = shapes.count; i-- > 0;) {
        LottieShapeEntry& shape = r.shapes.data[shapes.data[i]];
        shapeContours(r, shape, contours);
        measureShape(r, contours, t);
        uint32_t first = r.geometry.contours.count;
        // The trim's edges, as fractions of this shape: one range, or two if the range wraps past the end.
        float ranges[4];
        uint32_t rangeCount = 0;
        auto addRange = [&](float rs, float re) {
            if (item.mode == 2 && shapes.count > 1) {
                float scaledS = rs * totalAll;
                float scaledE = re * totalAll;
                if (scaledE < addedAll || scaledS > addedAll + t.total)
                    return;
                float shapeS = scaledS <= addedAll ? 0 : (scaledS - addedAll) / t.total;
                float shapeE = scaledE >= addedAll + t.total ? 1 : (scaledE - addedAll) / t.total;
                ranges[rangeCount++] = shapeS;
                ranges[rangeCount++] = shapeE;
            } else {
                ranges[rangeCount++] = rs;
                ranges[rangeCount++] = re;
            }
        };
        if (s != e) {
            if (item.mode == 2 && shapes.count > 1) {
                if (e <= 1) {
                    addRange(s, e);
                } else if (s >= 1) {
                    addRange(s - 1, e - 1);
                } else {
                    addRange(s, 1);
                    addRange(0, e - 1);
                }
            } else {
                addRange(s, e);
            }
        }
        addedAll += t.total;
        Builder b{&r.geometry, 0};
        for (uint32_t k = 0; k + 1 < rangeCount; k += 2) {
            float rs = ranges[k];
            float re = ranges[k + 1];
            if (re <= 1) {
                trimShape(r, contours, t, t.total * rs, t.total * re, false, b);
            } else if (rs >= 1) {
                trimShape(r, contours, t, t.total * (rs - 1), t.total * (re - 1), false, b);
            } else {
                // The trim runs past the end and continues from the start. A single closed contour continues across its
                // start. With several contours, the pieces stay separate, because the last contour's end is not where
                // the first begins.
                bool closes = contours.count == 1 && r.geometry.contours.data[contours.data[0]].closed;
                trimShape(r, contours, t, t.total * rs, t.total, closes, b);
                trimShape(r, contours, t, 0, t.total * (re - 1), false, b);
            }
            b.finish(false);
        }
        replaceContours(r, shape, first);
    }
    contours.reset();
    t.lengths.reset();
    t.segmentLengths.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Rewriting contour by contour

void roundCorners(LottieGeometry& g, const LottieContour& c, float round) {
    uint32_t contour = g.contours.count;
    g.contours.push(LottieContour{g.vertices.count, 0, c.closed});
    for (uint32_t i = 0; i < c.count; ++i) {
        LottieVertex v = g.vertices.data[c.first + i];
        bool corner = v.point.x == v.out.x && v.point.y == v.out.y && v.point.x == v.in.x && v.point.y == v.in.y;
        if (!corner || ((i == 0 || i == c.count - 1) && !c.closed)) {
            g.vertices.push(v);
            continue;
        }
        PrVec2 before = g.vertices.data[c.first + (i == 0 ? c.count - 1 : i - 1)].point;
        float d = distance(v.point, before);
        float perc = d > 0 ? minF32(d / 2, round) / d : 0;
        PrVec2 p = prVec2Lerp(v.point, before, perc);
        g.vertices.push(LottieVertex{p, p, prVec2Lerp(p, v.point, kLottieQuarterArc)});
        PrVec2 after = g.vertices.data[c.first + (i == c.count - 1 ? 0 : i + 1)].point;
        d = distance(v.point, after);
        perc = d > 0 ? minF32(d / 2, round) / d : 0;
        p = prVec2Lerp(v.point, after, perc);
        g.vertices.push(LottieVertex{p, prVec2Lerp(p, v.point, kLottieQuarterArc), p});
    }
    g.contours.data[contour].count = g.vertices.count - g.contours.data[contour].first;
}

void puckerBloat(LottieGeometry& g, const LottieContour& c, float amount) {
    float percent = amount / 100;
    PrVec2 center{0, 0};
    for (uint32_t i = 0; i < c.count; ++i) {
        center.x += g.vertices.data[c.first + i].point.x;
        center.y += g.vertices.data[c.first + i].point.y;
    }
    if (c.count > 0) {
        center.x /= float(c.count);
        center.y /= float(c.count);
    }
    g.contours.push(LottieContour{g.vertices.count, c.count, c.closed});
    for (uint32_t i = 0; i < c.count; ++i) {
        LottieVertex v = g.vertices.data[c.first + i];
        g.vertices.push(LottieVertex{prVec2Lerp(v.point, center, percent), prVec2Lerp(v.in, center, -percent),
                                     prVec2Lerp(v.out, center, -percent)});
    }
}

// Appends a zig-zag vertex `amplitude` away from `point` across `angle`.
void zigZagPoint(LottieGeometry& g, PrVec2 point, float angle, float direction, float amplitude, float outAmplitude,
                 float inAmplitude) {
    float angleOut = angle - kPiF32 / 2;
    float angleIn = angle + kPiF32 / 2;
    float px = point.x + cosF32(angle) * direction * amplitude;
    float py = point.y - sinF32(angle) * direction * amplitude;
    g.vertices.push(LottieVertex{PrVec2{px, py},
                                 PrVec2{px + cosF32(angleIn) * inAmplitude, py - sinF32(angleIn) * inAmplitude},
                                 PrVec2{px + cosF32(angleOut) * outAmplitude, py - sinF32(angleOut) * outAmplitude}});
}

void zigZag(LottieGeometry& g, const LottieContour& c, float amplitude, float frequency, float pointType) {
    uint32_t count = c.closed ? c.count : c.count - 1;
    uint32_t contour = g.contours.count;
    g.contours.push(LottieContour{g.vertices.count, 0, c.closed});
    if (c.count == 0 || count == 0) {
        g.contours.data[contour].count = 0;
        return;
    }
    auto vertex = [&](uint32_t i) { return g.vertices.data[c.first + i % c.count].point; };
    bool smooth = pointType == 2;
    auto corner = [&](uint32_t cur, float direction) {
        PrVec2 prev = vertex(cur == 0 ? c.count - 1 : cur - 1);
        PrVec2 next = vertex(cur + 1);
        PrVec2 point = vertex(cur);
        // The perpendicular of the line from the previous vertex to the next.
        PrVec2 v{next.x - prev.x, next.y - prev.y};
        PrVec2 rotated{v.y, -v.x};
        float angle = -atan2F32(rotated.y, rotated.x);
        float prevDist = smooth ? distance(point, prev) : 0;
        float nextDist = smooth ? distance(point, next) : 0;
        zigZagPoint(g, point, angle, direction, amplitude, nextDist / ((frequency + 1) * 2),
                    prevDist / ((frequency + 1) * 2));
    };
    float direction = -1;
    corner(0, direction);
    for (uint32_t i = 0; i < count; ++i) {
        Cubic seg = segmentOf(g, c, i);
        // A segment whose tangents sit on its ends is a line. Its tangents are moved to its thirds before it is
        // measured.
        if (seg.p[1].x == seg.p[0].x && seg.p[1].y == seg.p[0].y)
            seg.p[1] = prVec2Lerp(seg.p[0], seg.p[3], 1.0f / 3);
        if (seg.p[2].x == seg.p[3].x && seg.p[2].y == seg.p[3].y)
            seg.p[2] = prVec2Lerp(seg.p[0], seg.p[3], 2.0f / 3);
        direction = -direction;
        for (uint32_t k = 0; k < uint32_t(frequency); ++k) {
            float t = float(k + 1) / (frequency + 1);
            float dist = smooth ? distance(seg.p[3], seg.p[0]) : 0;
            PrVec2 d = cubicDerivative(seg, t);
            float angle = atan2F32(d.x, d.y);
            zigZagPoint(g, cubicAt(seg, t), angle, direction, amplitude, dist / ((frequency + 1) * 2),
                        dist / ((frequency + 1) * 2));
            direction = -direction;
        }
        corner(i + 1, direction);
    }
    // A closed contour ends on a corner at its first vertex again.
    g.contours.data[contour].count = g.vertices.count - g.contours.data[contour].first;
}

void twist(LottieGeometry& g, const LottieContour& c, float angle, PrVec2 center) {
    // Each point is rotated about the center by the angle, scaled by the point's distance relative to the farthest
    // vertex. Points farther out rotate more, matching After Effects' Twist.
    float farthest = 0;
    for (uint32_t i = 0; i < c.count; ++i)
        farthest = maxF32(farthest, distance(g.vertices.data[c.first + i].point, center));
    auto turn = [&](PrVec2 p) {
        float d = farthest > 0 ? distance(p, center) / farthest : 0;
        float radians = angle * kPiF32 / 180 * d;
        float cs = cosF32(radians);
        float sn = sinF32(radians);
        PrVec2 q{p.x - center.x, p.y - center.y};
        return PrVec2{center.x + q.x * cs - q.y * sn, center.y + q.x * sn + q.y * cs};
    };
    g.contours.push(LottieContour{g.vertices.count, c.count, c.closed});
    for (uint32_t i = 0; i < c.count; ++i) {
        LottieVertex v = g.vertices.data[c.first + i];
        g.vertices.push(LottieVertex{turn(v.point), turn(v.in), turn(v.out)});
    }
}

// Appends a polyline's contours as geometry.
void appendPolyline(LottieGeometry& g, const PrPathPolyline& polyline) {
    Builder b{&g, 0};
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        for (uint32_t i = start + 1; i < end; ++i) {
            PrVec2 from = polyline.points[i - 1];
            PrVec2 to = polyline.points[i];
            b.add(Cubic{{from, from, to, to}});
        }
        b.finish(prPathPolylineContourIsClosed(&polyline, c));
    }
}

// Appends a path's contours as geometry.
void appendPath(LottieGeometry& g, const PrPath* path) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t p = 0;
    Builder b{&g, 0};
    PrVec2 last{0, 0};
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                b.finish(false);
                last = points[p++];
                break;
            case PrPathVerb_Line:
                b.add(Cubic{{last, last, points[p], points[p]}});
                last = points[p++];
                break;
            case PrPathVerb_Quad:
                b.add(Cubic{
                    {last, prVec2Lerp(last, points[p],
                                      2.0f / 3), prVec2Lerp(points[p + 1], points[p], 2.0f / 3), points[p + 1]}});
                last = points[p + 1];
                p += 2;
                break;
            case PrPathVerb_Cubic:
                b.add(Cubic{{last, points[p], points[p + 1], points[p + 2]}});
                last = points[p + 2];
                p += 3;
                break;
            case PrPathVerb_Close:
                b.finish(true);
                break;
            default:
                break;
        }
    }
    b.finish(false);
}

void offsetShape(LottieRender& r, LottieShapeEntry& shape, const LottieItem& item, double frame) {
    const LottieAnimation& a = *r.a;
    float amount = lottieScalar(a, item.props[LottieProp_Amount], frame, 0);
    if (amount == 0)
        return;
    float miter = lottieScalar(a, item.props[LottieProp_MiterLimit], frame, 4);
    prPathClear(r.path);
    for (uint32_t i = 0; i < shape.contourCount; ++i)
        lottieAddContour(r.geometry, r.geometry.contours.data[r.refs.data[shape.contours + i]], prAffine2Identity(),
                         r.path);
    PrPathPolyline flat{};
    PrPathPolyline out{};
    prPathFlatten(r.path, prAffine2Identity(), kModifierTolerance, PrPathFlattening_Inside, &flat);
    prPathPolylineOffset(&flat, PrPathFillRule_NonZero, amount, item.join, miter, kModifierTolerance, &out);
    uint32_t first = r.geometry.contours.count;
    appendPolyline(r.geometry, out);
    prPathPolylineReset(&flat);
    prPathPolylineReset(&out);
    replaceContours(r, shape, first);
}

// Merges paths. Every shape that the merge applies to, and that no earlier merge took, is moved into the merge's group
// and combined into the first shape, in the order the shapes are listed. For the merge mode, shapes are appended. For
// other modes, each shape is combined with the result so far by the mode's operation. The first shape then lies in the
// merge's group, and the others are left empty.
void merge(LottieRender& r, const LottieItem& item, uint32_t group, Array<uint32_t>& shapes) {
    PrPathPolylineOp op = item.mode == 2   ? PrPathPolylineOp_Union
                  : item.mode == 3 ? PrPathPolylineOp_Difference
                  : item.mode == 4 ? PrPathPolylineOp_Intersect
                                   : PrPathPolylineOp_Xor;
    // The merge mode appends paths, including their curves. The other modes combine regions, which are polylines, once
    // there is a second shape to combine.
    bool append = item.mode <= 1;
    bool combining = false;
    PrPath* result = prPathCreate();
    PrPath* next = prPathCreate();
    PrPathPolyline region{};
    PrPathPolyline flat{};
    PrPathPolyline combined{};
    LottieShapeEntry* first = nullptr;
    // The listed order is the reverse of the order in which the shapes were found.
    for (uint32_t k = shapes.count; k-- > 0;) {
        LottieShapeEntry& shape = r.shapes.data[shapes.data[k]];
        if (shape.merged)
            continue;
        prPathClear(next);
        PrAffine2 toMerge = lottieGroupToGroup(r, shape.group, group);
        for (uint32_t i = 0; i < shape.contourCount; ++i)
            lottieAddContour(r.geometry, r.geometry.contours.data[r.refs.data[shape.contours + i]], toMerge, next);
        if (append || first == nullptr) {
            prPathAddPath(result, next, prAffine2Identity());
        } else {
            if (!combining)
                prPathFlatten(result, prAffine2Identity(), kModifierTolerance, PrPathFlattening_Inside, &region);
            combining = true;
            prPathPolylineClear(&flat);
            prPathFlatten(next, prAffine2Identity(), kModifierTolerance, PrPathFlattening_Inside, &flat);
            prPathPolylineClear(&combined);
            prPathPolylineCombine(&region, PrPathFillRule_NonZero, &flat, PrPathFillRule_NonZero, op, &combined);
            PrPathPolyline swap = region;
            region = combined;
            combined = swap;
        }
        if (first == nullptr) {
            first = &shape;
        } else {
            shape.contourCount = 0;
            shape.merged = true;
        }
    }
    if (first != nullptr) {
        uint32_t start = r.geometry.contours.count;
        if (combining)
            appendPolyline(r.geometry, region);
        else
            appendPath(r.geometry, result);
        replaceContours(r, *first, start);
        first->group = group;
    }
    prPathDestroy(result);
    prPathDestroy(next);
    prPathPolylineReset(&region);
    prPathPolylineReset(&flat);
    prPathPolylineReset(&combined);
}

} // namespace

void lottieApplyModifiers(LottieRender& r, double frame) {
    const LottieAnimation& a = *r.a;
    Array<uint32_t> shapes;
    // Modifiers apply nearest first. The modifier found last is the nearest to the shapes it applies to.
    for (uint32_t m = r.modifiers.count; m-- > 0;) {
        const LottieItem& item = a.items.data[r.modifiers.data[m].item];
        shapes.clear();
        for (uint32_t s = 0; s < r.shapes.count; ++s) {
            const LottieShapeEntry& e = r.shapes.data[s];
            for (uint32_t k = 0; k < e.modifierCount; ++k) {
                if (r.refs.data[e.modifiers + k] == m) {
                    shapes.push(s);
                    break;
                }
            }
        }
        if (shapes.count == 0)
            continue;
        switch (item.kind) {
            case LottieItemKind_Trim:
                trim(r, item, frame, shapes);
                break;
            case LottieItemKind_Merge:
                merge(r, item, r.modifiers.data[m].group, shapes);
                break;
            case LottieItemKind_OffsetPath:
                for (uint32_t s = 0; s < shapes.count; ++s)
                    offsetShape(r, r.shapes.data[shapes.data[s]], item, frame);
                break;
            case LottieItemKind_RoundCorners:
            case LottieItemKind_PuckerBloat:
            case LottieItemKind_ZigZag:
            case LottieItemKind_Twist: {
                float amount = 0;
                float frequency = 0;
                float pointType = 1;
                PrVec2 center{0, 0};
                if (item.kind == LottieItemKind_RoundCorners) {
                    amount = lottieScalar(a, item.props[LottieProp_Roundness], frame, 0);
                } else if (item.kind == LottieItemKind_PuckerBloat) {
                    amount = lottieScalar(a, item.props[LottieProp_Amount], frame, 0);
                } else if (item.kind == LottieItemKind_ZigZag) {
                    amount = lottieScalar(a, item.props[LottieProp_Size], frame, 0);
                    float ridges = roundF32(lottieScalar(a, item.props[LottieProp_Points], frame, 0));
                    frequency = ridges > 0 ? minF32(ridges, kZigZagMaxRidges) : 0;
                    pointType = lottieScalar(a, item.props[LottieProp_Roundness], frame, 1);
                } else {
                    amount = lottieScalar(a, item.props[LottieProp_Amount], frame, 0);
                    center = lottiePoint(a, item.props[LottieProp_Center], frame, PrVec2{0, 0});
                }
                if (amount == 0)
                    break;
                for (uint32_t s = 0; s < shapes.count; ++s) {
                    LottieShapeEntry& shape = r.shapes.data[shapes.data[s]];
                    uint32_t first = r.geometry.contours.count;
                    for (uint32_t i = 0; i < shape.contourCount; ++i) {
                        LottieContour c = r.geometry.contours.data[r.refs.data[shape.contours + i]];
                        if (item.kind == LottieItemKind_RoundCorners)
                            roundCorners(r.geometry, c, amount);
                        else if (item.kind == LottieItemKind_PuckerBloat)
                            puckerBloat(r.geometry, c, amount);
                        else if (item.kind == LottieItemKind_ZigZag)
                            zigZag(r.geometry, c, amount, frequency, pointType);
                        else
                            twist(r.geometry, c, amount, center);
                    }
                    replaceContours(r, shape, first);
                }
                break;
            }
            default:
                break;
        }
    }
    shapes.reset();
}

} // namespace pr_impl
