// Stroking paths: the outline covered by a line of some width along a path.
//
// A contour flattens to vertices. Each vertex records its distance along the contour, its half widths, and whether it
// joins two segments of the path or lies inside a flattened curve. A dash pattern cuts the vertices into pieces. An
// open piece becomes one loop around it, and a closed contour becomes two loops. Each side is offset from the vertices,
// with join geometry on the outer side of each turn. The inner side folds over itself.

#include "pr/path_stroke.h"

#include "pr/array.h"
#include "pr/float.h"
#include "pr/vec.h"

#include "path/src/flatten.h"
#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

// Points closer than this are treated as one point.
constexpr float kCoincident = 1e-6f;

// Directions whose cross product is below this do not turn.
constexpr float kParallel = 1e-6f;

// Dash patterns longer than this are ignored instead of doubled.
constexpr uint32_t kMaxDashes = 1024;

// The maximum number of dashes per contour. A pattern that would cut more dashes strokes solid.
constexpr float kMaxDashCount = 100000;

// The maximum number of points that one arc of a join or cap uses.
constexpr uint32_t kArcMaxSteps = 256;

struct Vertex {
    PrVec2 point;
    float along;        // the arc length from the start of the contour

    // The signed curvature of the segment that ends here, positive if it turns toward the right side.
    float curveIn;

    float curveOut;     // the signed curvature of the segment that starts here
    float left;         // the half widths at the vertex
    float right;
    bool corner;        // true if two segments of the path meet here
};

struct Stroker {
    const PrPathStroke* style;
    float tolerance;
    float maxTurn;              // the largest angle that one flattened step of a curve may turn through
    PrPathPolyline* out;
    Array<float> pattern;
    float patternLength;
    Array<Vertex> contour;
    Array<Vertex> piece;
    Array<Vertex> first;        // the first dash of a closed contour, kept to join its last dash
    Array<PrVec2> curve;
    Array<PrVec2> loop;
};

float dot(PrVec2 a, PrVec2 b) {
    return a.x * b.x + a.y * b.y;
}

float cross(PrVec2 a, PrVec2 b) {
    return a.x * b.y - a.y * b.x;
}

float length(PrVec2 a) {
    return sqrtF32(dot(a, a));
}

PrVec2 unit(PrVec2 a) {
    float l = length(a);
    return l > 0 ? a * (1 / l) : PrVec2{1, 0};
}

// Returns the normal on the left of a direction, which is toward -y if the direction is toward +x.
PrVec2 leftNormal(PrVec2 direction) {
    return PrVec2{direction.y, -direction.x};
}

// Returns the signed curvature at a point at which the derivative is `d` and the second derivative is `dd`.
float curvature(PrVec2 d, PrVec2 dd) {
    float l = length(d);
    return l > kCoincident ? cross(d, dd) / (l * l * l) : 0;
}

// Returns the angle between two directions, from 0 to π.
float angleBetween(PrVec2 a, PrVec2 b) {
    if (length(a) < kCoincident || length(b) < kCoincident)
        return 0;
    return absF32(atan2F32(cross(a, b), dot(a, b)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Widths

void widthAt(const Stroker& s, float fraction, float* left, float* right) {
    PrPathStrokeWidth width = prPathStrokeWidthAt(s.style, fraction);
    *left = width.left;
    *right = width.right;
}

// Returns the largest half width anywhere along a contour.
float widestHalf(const PrPathStroke* style) {
    float half = style->width / 2;
    if (style->profile == nullptr || style->profileCount == 0)
        return half;
    float widest = 0;
    for (uint32_t i = 0; i < style->profileCount; ++i)
        widest = maxF32(widest, maxF32(style->profile[i].left, style->profile[i].right));
    return widest * style->width;
}

// ---------------------------------------------------------------------------------------------------------------------
// Outlines

// Returns the number of steps that an arc of `radius` through `angle` needs to stay within the tolerance.
uint32_t arcSteps(float radius, float angle, float tolerance) {
    float ratio = 1 - tolerance / radius;
    float step = ratio > -1 && ratio < 1 ? 2 * acosF32(ratio) : kPiF32;
    float steps = ceilF32(absF32(angle) / step);
    if (!(steps >= 1))
        return 1;
    return steps > float(kArcMaxSteps) ? kArcMaxSteps : uint32_t(steps);
}

void push(Stroker& s, PrVec2 point) {
    if (s.loop.count != 0 && length(point - s.loop.last()) < kCoincident)
        return;
    s.loop.push(point);
}

// Adds the points strictly inside the arc around `center` that starts at `from` and turns through `angle`.
void pushArc(Stroker& s, PrVec2 center, PrVec2 from, float angle) {
    PrVec2 radius = from - center;
    uint32_t steps = arcSteps(length(radius), angle, s.tolerance);
    for (uint32_t i = 1; i < steps; ++i) {
        float a = angle * float(i) / float(steps);
        float c = cosF32(a);
        float n = sinF32(a);
        push(s, center + PrVec2{radius.x * c - radius.y * n, radius.x * n + radius.y * c});
    }
}

// Emits the loop as a closed contour, if it encloses any area.
void emit(Stroker& s) {
    Array<PrVec2>& p = s.loop;
    if (p.count > 1 && length(p.last() - p.data[0]) < kCoincident)
        p.pop();
    float area = 0;
    for (uint32_t i = 0; i < p.count; ++i)
        area += cross(p.data[i], p.data[(i + 1) % p.count]);
    if (p.count >= 3 && area != 0) {
        prPathPolylineMoveTo(s.out, p.data[0]);
        for (uint32_t i = 1; i < p.count; ++i)
            prPathPolylineLineTo(s.out, p.data[i]);
        prPathPolylineClose(s.out);
    }
    p.clear();
}

// Adds the points of a cap, which lie after the end on the left, `p + n l`, and before the end on the right, `p - n r`.
// `d` is the direction that the cap faces, and `n` is its left normal.
void pushCap(Stroker& s, PrVec2 p, PrVec2 d, float l, float r, PrPathCap kind) {
    PrVec2 n = leftNormal(d);
    float half = (l + r) / 2;
    PrVec2 middle = p + n * ((l - r) / 2);
    PrVec2 ahead = d * half;
    switch (kind) {
        case PrPathCap_Square:
            push(s, p + n * l + ahead);
            push(s, p - n * r + ahead);
            break;
        case PrPathCap_Triangle:
            push(s, middle + ahead);
            break;
        case PrPathCap_Round:
            pushArc(s, middle, p + n * l, kPiF32);
            break;
        default:
            break;
    }
}

// Returns the center of the circle of curvature `curve` at `vertex`, for a direction whose left normal is `normal`.
// Every offset of the curve at that vertex shares this center.
PrVec2 centerOf(PrVec2 vertex, PrVec2 normal, float curve) {
    return vertex - normal * (1 / curve);
}

// Finds the point at which two extensions meet. One continues the side that arrives at `from` in `direction`. The other
// continues the side that leaves `to` in `leaving`. Each continues as the circle of its curvature about the center of
// curvature of the vertex, or as a line if the curvature is zero. Returns false if they do not meet on the outer side.
bool arcsMeet(PrVec2 vertex, PrVec2 bisector, PrVec2 from, PrVec2 direction, float curveIn,
              PrVec2 to, PrVec2 leaving, float curveOut, PrVec2* meet) {
    bool lineIn = absF32(curveIn) < kCoincident;
    bool lineOut = absF32(curveOut) < kCoincident;
    PrVec2 centerIn = lineIn ? vertex : centerOf(vertex, leftNormal(direction), curveIn);
    PrVec2 centerOut = lineOut ? vertex : centerOf(vertex, leftNormal(leaving), curveOut);
    float radiusIn = length(from - centerIn);
    float radiusOut = length(to - centerOut);
    PrVec2 candidates[2];
    uint32_t count = 0;
    if (lineIn && lineOut) {
        float denominator = cross(direction, leaving);
        if (absF32(denominator) < kParallel)
            return false;
        float t = cross(to - from, leaving) / denominator;
        candidates[count++] = from + direction * t;
    } else if (lineIn || lineOut) {
        PrVec2 point = lineIn ? from : to;
        PrVec2 along = lineIn ? direction : leaving;
        PrVec2 center = lineIn ? centerOut : centerIn;
        float radius = lineIn ? radiusOut : radiusIn;
        PrVec2 offset = point - center;
        float b = dot(offset, along);
        float c = dot(offset, offset) - radius * radius;
        float discriminant = b * b - c;
        if (discriminant < 0)
            return false;
        float root = sqrtF32(discriminant);
        candidates[count++] = point + along * (-b + root);
        candidates[count++] = point + along * (-b - root);
    } else {
        PrVec2 separation = centerOut - centerIn;
        float d = length(separation);
        if (d < kCoincident || d > radiusIn + radiusOut || d < absF32(radiusIn - radiusOut))
            return false;
        float a = (radiusIn * radiusIn - radiusOut * radiusOut + d * d) / (2 * d);
        float h = sqrtF32(maxF32(radiusIn * radiusIn - a * a, 0));
        PrVec2 base = centerIn + separation * (a / d);
        PrVec2 across = leftNormal(separation) * (h / d);
        candidates[count++] = base + across;
        candidates[count++] = base - across;
    }
    float best = kF32Max;
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 offset = candidates[i] - vertex;
        float reach = length(offset);
        if (dot(offset, bisector) > 0 && reach < best) {
            best = reach;
            *meet = candidates[i];
        }
    }
    return best < kF32Max;
}

// Adds the points strictly inside the arc about `center` from `from`, starting in `direction`, to `to`. Adds nothing if
// the curvature of the side is zero and the extension is a line.
void pushExtension(Stroker& s, PrVec2 center, bool line, PrVec2 from, PrVec2 direction,
                   PrVec2 to) {
    if (line)
        return;
    PrVec2 a = from - center;
    PrVec2 b = to - center;
    float angle = atan2F32(cross(a, b), dot(a, b));
    float turning = cross(a, direction);
    if (turning > 0 && angle < 0)
        angle += 2 * kPiF32;
    else if (turning < 0 && angle > 0)
        angle -= 2 * kPiF32;
    pushArc(s, center, from, angle);
}

// Adds the points that one side of an outline adds at a vertex, arriving in `in` and leaving in `out`, at half width
// `w` on the left of the direction of travel.
void pushJoin(Stroker& s, PrVec2 p, PrVec2 in, PrVec2 out, float w, bool corner, float curveIn,
              float curveOut, float lengthIn, float lengthOut) {
    PrVec2 nIn = leftNormal(in);
    PrVec2 nOut = leftNormal(out);
    PrVec2 a = p + nIn * w;
    PrVec2 b = p + nOut * w;
    float turn = cross(in, out);
    float along = dot(in, out);
    if (absF32(turn) <= kParallel && along > 0) {
        push(s, a);
        push(s, b);
        return;
    }
    if (dot(out, nIn) > kParallel) {
        // The inner side is the point at which the two offsets cross, if that point lies on both segments. Otherwise,
        // it is a fold through the vertex, which the nonzero fill covers. The crossing is checked by its position along
        // each segment, because near a reversal the terms used to find it are tiny and it can land anywhere.
        PrVec2 crossing = p + (nIn + nOut) * (w / (1 + along));
        float back = -dot(crossing - a, in);
        float ahead = dot(crossing - b, out);
        if (1 + along > 0 && back >= 0 && back <= lengthIn && ahead >= 0 && ahead <= lengthOut) {
            push(s, crossing);
        } else {
            push(s, a);
            push(s, p);
            push(s, b);
        }
        return;
    }
    PrPathJoin kind = corner ? s.style->join : PrPathJoin_Round;
    PrVec2 sum = nIn + nOut;
    PrVec2 bisector = length(sum) > kParallel ? unit(sum) : in;
    float cosHalf = dot(bisector, nIn);
    float limit = s.style->miterLimit * w;
    // The tip of the miter lies along the bisector, at the length that the limit measures. Near a reversal, the sum of
    // the offsets and the turn are both tiny, and a tip computed from their ratio can land far beyond the point that
    // the limit allows.
    bool withinLimit = along > -1 + kParallel && cosHalf > 0 && w <= limit * cosHalf;
    PrVec2 tip = p + bisector * (w / (cosHalf > 0 ? cosHalf : 1));
    push(s, a);
    switch (kind) {
        case PrPathJoin_Round:
            pushArc(s, p, a, atan2F32(absF32(turn), along));
            break;
        case PrPathJoin_Miter:
            if (withinLimit)
                push(s, tip);
            break;
        case PrPathJoin_Arcs: {
            PrVec2 meet{};
            if (arcsMeet(p, bisector, a, in, curveIn, b, out, curveOut, &meet)
                && length(meet - p) <= limit) {
                bool lineIn = absF32(curveIn) < kCoincident;
                bool lineOut = absF32(curveOut) < kCoincident;
                pushExtension(s, lineIn ? p : centerOf(p, nIn, curveIn), lineIn, a, in, meet);
                push(s, meet);
                // The extension of the outgoing side, walked back from its start and then reversed.
                uint32_t mark = s.loop.count;
                pushExtension(s, lineOut ? p : centerOf(p, nOut, curveOut), lineOut, b,
                              out * -1, meet);
                for (uint32_t i = mark, j = s.loop.count - 1; i < j; ++i, --j) {
                    PrVec2 swap = s.loop.data[i];
                    s.loop.data[i] = s.loop.data[j];
                    s.loop.data[j] = swap;
                }
                break;
            }
            [[fallthrough]];
        }
        case PrPathJoin_MiterClip:
            if (withinLimit) {
                push(s, tip);
            } else if (limit > w * cosHalf) {
                float step = limit - w * cosHalf;
                float inReach = dot(in, bisector);
                float outReach = -dot(out, bisector);
                if (inReach > kParallel && outReach > kParallel) {
                    push(s, a + in * (step / inReach));
                    push(s, b - out * (step / outReach));
                }
            }
            break;
        default:
            break;
    }
    push(s, b);
}

PrVec2 segmentDirection(const Vertex& a, const Vertex& b) {
    return unit(b.point - a.point);
}

float segmentLength(const Vertex& a, const Vertex& b) {
    return length(b.point - a.point);
}

// Strokes one loop around an open run of vertices, with its caps. `direction` is used for a lone vertex.
void strokeOpen(Stroker& s, const Vertex* v, uint32_t n, PrVec2 direction, PrPathCap startCap,
                PrPathCap endCap) {
    if (n == 1) {
        PrVec2 d = direction;
        PrVec2 normal = leftNormal(d);
        push(s, v[0].point + normal * v[0].left);
        pushCap(s, v[0].point, d, v[0].left, v[0].right, endCap);
        push(s, v[0].point - normal * v[0].right);
        pushCap(s, v[0].point, d * -1, v[0].right, v[0].left, startCap);
        emit(s);
        return;
    }
    PrVec2 first = segmentDirection(v[0], v[1]);
    PrVec2 last = segmentDirection(v[n - 2], v[n - 1]);
    push(s, v[0].point + leftNormal(first) * v[0].left);
    for (uint32_t i = 1; i + 1 < n; ++i) {
        pushJoin(s, v[i].point, segmentDirection(v[i - 1], v[i]),
                 segmentDirection(v[i], v[i + 1]), v[i].left, v[i].corner, v[i].curveIn,
                 v[i].curveOut, segmentLength(v[i - 1], v[i]), segmentLength(v[i], v[i + 1]));
    }
    push(s, v[n - 1].point + leftNormal(last) * v[n - 1].left);
    pushCap(s, v[n - 1].point, last, v[n - 1].left, v[n - 1].right, endCap);
    push(s, v[n - 1].point - leftNormal(last) * v[n - 1].right);
    for (uint32_t i = n - 2; i >= 1; --i) {
        pushJoin(s, v[i].point, segmentDirection(v[i + 1], v[i]),
                 segmentDirection(v[i], v[i - 1]), v[i].right, v[i].corner, -v[i].curveOut,
                 -v[i].curveIn, segmentLength(v[i], v[i + 1]), segmentLength(v[i - 1], v[i]));
    }
    push(s, v[0].point - leftNormal(first) * v[0].right);
    pushCap(s, v[0].point, first * -1, v[0].right, v[0].left, startCap);
    emit(s);
}

// Strokes two loops around a closed run of at least two vertices, one per side.
void strokeClosed(Stroker& s, const Vertex* v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const Vertex& before = v[(i + n - 1) % n];
        const Vertex& after = v[(i + 1) % n];
        pushJoin(s, v[i].point, segmentDirection(before, v[i]), segmentDirection(v[i], after),
                 v[i].left, v[i].corner, v[i].curveIn, v[i].curveOut,
                 segmentLength(before, v[i]), segmentLength(v[i], after));
    }
    emit(s);
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = n - 1 - k;
        const Vertex& before = v[(i + n - 1) % n];
        const Vertex& after = v[(i + 1) % n];
        pushJoin(s, v[i].point, segmentDirection(after, v[i]), segmentDirection(v[i], before),
                 v[i].right, v[i].corner, -v[i].curveOut, -v[i].curveIn,
                 segmentLength(v[i], after), segmentLength(before, v[i]));
    }
    emit(s);
}

// ---------------------------------------------------------------------------------------------------------------------
// Contours

void appendVertex(Array<Vertex>& vertices, const Vertex& vertex) {
    if (vertices.count != 0) {
        Vertex& last = vertices.last();
        if (length(vertex.point - last.point) < kCoincident) {
            last.corner = last.corner || vertex.corner;
            last.curveIn = vertex.curveIn;
            return;
        }
    }
    vertices.push(vertex);
}

// Returns the vertex at fraction `t` of the way from `a` to `b`, inside the segment between them.
Vertex between(const Stroker& s, const Vertex& a, const Vertex& b, float t, float total) {
    Vertex v{};
    v.point = a.point + (b.point - a.point) * t;
    v.along = a.along + (b.along - a.along) * t;
    v.curveIn = a.curveOut;
    v.curveOut = a.curveOut;
    widthAt(s, total > 0 ? v.along / total : 0, &v.left, &v.right);
    return v;
}

// Returns the number of steps for a curve whose control polygon turns through `turn`, so that the offset of its widest
// side stays within the tolerance.
uint32_t turnCount(const Stroker& s, float turn) {
    float steps = ceilF32(turn / s.maxTurn);
    return steps >= 1 ? (steps > 1024 ? 1024 : uint32_t(steps)) : 1;
}

// Adds the flattened points of a curve to the contour. Each point inside the curve is smooth.
void addCurve(Stroker& s, float curveOut, float curveIn) {
    s.contour.last().curveOut = curveOut;
    for (uint32_t i = 0; i < s.curve.count; ++i) {
        bool end = i + 1 == s.curve.count;
        appendVertex(s.contour, Vertex{s.curve.data[i], 0, end ? curveIn : 0, 0, 0, 0, end});
    }
    s.curve.clear();
}

// Sets the arc length and half widths of each vertex, and adds a vertex wherever a profile point falls between two
// vertices. Returns the length of the contour.
float measure(Stroker& s, bool closed) {
    Array<Vertex>& v = s.contour;
    float along = 0;
    for (uint32_t i = 0; i < v.count; ++i) {
        if (i != 0)
            along += segmentLength(v.data[i - 1], v.data[i]);
        v.data[i].along = along;
    }
    float total = along + (closed ? segmentLength(v.last(), v.data[0]) : 0);
    const PrPathStroke* style = s.style;
    if (style->profile != nullptr && total > 0) {
        Array<Vertex> split;
        uint32_t next = 0;
        for (uint32_t i = 0; i < v.count; ++i) {
            const Vertex& a = v.data[i];
            split.push(a);
            if (i + 1 == v.count && !closed)
                break;
            Vertex b = v.data[(i + 1) % v.count];
            b.along = i + 1 == v.count ? total : b.along;
            while (next < style->profileCount
                   && style->profile[next].position * total <= a.along + kCoincident)
                ++next;
            while (next < style->profileCount
                   && style->profile[next].position * total < b.along - kCoincident) {
                float at = style->profile[next].position * total;
                split.push(between(s, a, b, (at - a.along) / (b.along - a.along), total));
                ++next;
            }
        }
        v.clear();
        v.append(split.data, split.count);
        split.reset();
    }
    for (uint32_t i = 0; i < v.count; ++i)
        widthAt(s, total > 0 ? v.data[i].along / total : 0, &v.data[i].left, &v.data[i].right);
    return total;
}

// Strokes the dash in the piece. If it is the first dash of a closed contour, keeps it instead, because the last dash
// may run into it.
void finishPiece(Stroker& s, PrVec2 direction, bool atStart, bool atEnd, bool closed) {
    const PrPathStroke* style = s.style;
    if (closed && atStart) {
        s.first.clear();
        s.first.append(s.piece.data, s.piece.count);
    } else {
        strokeOpen(s, s.piece.data, s.piece.count, direction,
                   atStart && !closed ? style->startCap : style->dashCap,
                   atEnd && !closed ? style->endCap : style->dashCap);
    }
    s.piece.clear();
}

// Cuts the contour into dashes and strokes each.
void dashContour(Stroker& s, bool closed, float total) {
    const PrPathStroke* style = s.style;
    Array<float>& pattern = s.pattern;
    Array<Vertex>& v = s.contour;
    if (closed) {
        Vertex end = v.data[0];
        end.along = total;
        widthAt(s, 1, &end.left, &end.right);
        v.push(end);
    }
    float phase =
        style->dashOffset - s.patternLength * floorF32(style->dashOffset / s.patternLength);
    // A dash that ends at the offset is behind it, and one of no length at the offset is ahead of it.
    uint32_t k = 0;
    while (phase > pattern.data[k] || (phase == pattern.data[k] && pattern.data[k] > 0)) {
        phase -= pattern.data[k];
        k = (k + 1) % pattern.count;
    }
    float remaining = pattern.data[k] - phase;
    bool on = k % 2 == 0;
    bool atStart = on;
    bool whole = on;
    s.first.clear();
    s.piece.clear();
    if (on)
        s.piece.push(v.data[0]);
    PrVec2 direction = segmentDirection(v.data[0], v.data[1]);
    for (uint32_t i = 0; i + 1 < v.count; ++i) {
        const Vertex& a = v.data[i];
        const Vertex& b = v.data[i + 1];
        float span = b.along - a.along;
        direction = segmentDirection(a, b);
        float position = 0;
        while (span - position > remaining) {
            position += remaining;
            Vertex cut = between(s, a, b, position / span, total);
            if (on) {
                appendVertex(s.piece, cut);
                finishPiece(s, direction, atStart, false, closed);
                whole = false;
            } else {
                s.piece.clear();
                s.piece.push(cut);
            }
            atStart = false;
            k = (k + 1) % pattern.count;
            remaining = pattern.data[k];
            on = k % 2 == 0;
        }
        remaining -= span - position;
        if (on)
            appendVertex(s.piece, b);
    }
    // Dashes of no length at the end of an open contour are dots there.
    if (!closed && !on && remaining == 0) {
        for (uint32_t step = 0; step < pattern.count; ++step) {
            k = (k + 1) % pattern.count;
            if (pattern.data[k] > 0)
                break;
            if (k % 2 == 0)
                strokeOpen(s, &v.last(), 1, direction, style->dashCap, style->endCap);
        }
    }
    if (closed && whole) {
        s.piece.clear();
        v.pop();
        strokeClosed(s, v.data, v.count);
        return;
    }
    if (on && s.piece.count != 0) {
        if (closed && s.first.count != 0) {
            // The last dash runs through the start of the contour into the first dash.
            s.piece.pop();
            s.piece.push(v.data[0]);
            s.piece.last().along = 0;
            s.piece.append(s.first.data + 1, s.first.count - 1);
            s.first.clear();
            strokeOpen(s, s.piece.data, s.piece.count, direction, style->dashCap, style->dashCap);
            s.piece.clear();
        } else {
            strokeOpen(s, s.piece.data, s.piece.count, direction,
                       atStart && !closed ? style->startCap : style->dashCap,
                       closed ? style->dashCap : style->endCap);
            s.piece.clear();
        }
    }
    if (s.first.count != 0) {
        PrVec2 d = s.first.count > 1 ? segmentDirection(s.first.data[0], s.first.data[1])
                                      : direction;
        strokeOpen(s, s.first.data, s.first.count, d, style->dashCap, style->dashCap);
        s.first.clear();
    }
}

void strokeContour(Stroker& s, bool closed) {
    Array<Vertex>& v = s.contour;
    if (closed && v.count > 1
        && length(v.last().point - v.data[0].point) < kCoincident) {
        v.data[0].curveIn = v.last().curveIn;
        v.data[0].corner = true;
        v.pop();
    }
    closed = closed && v.count > 1;
    float total = measure(s, closed);
    bool dashed = s.pattern.count != 0 && v.count > 1 && total / s.patternLength < kMaxDashCount;
    if (dashed) {
        dashContour(s, closed, total);
    } else if (closed) {
        strokeClosed(s, v.data, v.count);
    } else {
        PrVec2 direction = v.count > 1 ? segmentDirection(v.data[0], v.data[1])
                                       : unit(s.style->zeroLengthDirection);
        strokeOpen(s, v.data, v.count, direction, s.style->startCap, s.style->endCap);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrPathStroke prPathStrokeDefault(void) {
    PrPathStroke stroke{};
    stroke.width = 1;
    stroke.miterLimit = 4;
    stroke.zeroLengthDirection = PrVec2{1, 0};
    return stroke;
}

void prPathStrokeOutline(const PrPath* path, const PrPathStroke* stroke, float tolerance, PrPathPolyline* out) {
    if (!(stroke->width > 0) || !isFiniteF32(stroke->width) || !(tolerance > 0))
        return;
    Stroker s{};
    s.style = stroke;
    s.tolerance = tolerance;
    s.out = out;
    float widest = widestHalf(stroke);
    float ratio = 1 - tolerance / maxF32(widest, tolerance);
    s.maxTurn = ratio > 0 ? maxF32(2 * acosF32(ratio), 0.01f) : kPiF32 / 2;

    if (stroke->dashes != nullptr && stroke->dashCount != 0 && stroke->dashCount <= kMaxDashes) {
        bool valid = true;
        for (uint32_t repeat = 0; repeat < (stroke->dashCount % 2 == 0 ? 1u : 2u); ++repeat) {
            for (uint32_t i = 0; i < stroke->dashCount; ++i) {
                float value = stroke->dashes[i];
                valid = valid && value >= 0 && isFiniteF32(value);
                s.pattern.push(value);
                s.patternLength += value;
            }
        }
        if (!valid || !(s.patternLength > 0) || !isFiniteF32(s.patternLength)) {
            s.pattern.clear();
            s.patternLength = 0;
        }
    }

    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* source = prPathPoints(path);
    uint32_t next = 0;
    bool segmented = false;
    PrVec2 current{0, 0};
    auto flush = [&](bool closed) {
        if (segmented && s.contour.count != 0)
            strokeContour(s, closed);
        s.contour.clear();
        segmented = false;
    };
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                flush(false);
                current = source[next++];
                s.contour.push(Vertex{current, 0, 0, 0, 0, 0, true});
                break;
            case PrPathVerb_Line:
                s.contour.last().curveOut = 0;
                current = source[next++];
                appendVertex(s.contour, Vertex{current, 0, 0, 0, 0, 0, true});
                segmented = true;
                break;
            case PrPathVerb_Quad: {
                PrVec2 p1 = source[next];
                PrVec2 p2 = source[next + 1];
                PrVec2 second = current - p1 * 2 + p2;
                uint32_t count = pathQuadLineCount(current, p1, p2, tolerance);
                uint32_t turned = turnCount(s, angleBetween(p1 - current, p2 - p1));
                pathFlattenQuadLines(current, p1, p2, count > turned ? count : turned, s.curve);
                addCurve(s, curvature(p1 - current, second),
                         curvature(p2 - p1, second));
                current = p2;
                next += 2;
                segmented = true;
                break;
            }
            case PrPathVerb_Cubic: {
                PrVec2 p1 = source[next];
                PrVec2 p2 = source[next + 1];
                PrVec2 p3 = source[next + 2];
                PrVec2 leg0 = p1 - current;
                PrVec2 leg1 = p2 - p1;
                PrVec2 leg2 = p3 - p2;
                uint32_t count = pathCubicLineCount(current, p1, p2, p3, tolerance);
                uint32_t turned =
                    turnCount(s, angleBetween(leg0, leg1) + angleBetween(leg1, leg2)
                                     + (length(leg1) < kCoincident ? angleBetween(leg0, leg2) : 0));
                pathFlattenCubicLines(current, p1, p2, p3, count > turned ? count : turned, s.curve);
                // The first and second derivatives at each end, in thirds and sixths.
                PrVec2 startSecond = current - p1 * 2 + p2;
                PrVec2 endSecond = p1 - p2 * 2 + p3;
                PrVec2 startTangent = length(leg0) > kCoincident ? leg0 : p2 - current;
                PrVec2 endTangent = length(leg2) > kCoincident ? leg2 : p3 - p1;
                addCurve(s, curvature(startTangent * 3, startSecond * 6),
                         curvature(endTangent * 3, endSecond * 6));
                current = p3;
                next += 3;
                segmented = true;
                break;
            }
            case PrPathVerb_Close:
                // A close is a segment back to the start of the contour, of zero length if the contour has no other.
                segmented = true;
                flush(true);
                break;
        }
    }
    flush(false);
    s.pattern.reset();
    s.contour.reset();
    s.piece.reset();
    s.first.reset();
    s.curve.reset();
    s.loop.reset();
}

PrPathStrokeWidth prPathStrokeWidthAt(const PrPathStroke* stroke, float fraction) {
    float half = stroke->width / 2;
    const PrPathWidthPoint* profile = stroke->profile;
    uint32_t count = stroke->profileCount;
    if (profile == nullptr || count == 0)
        return PrPathStrokeWidth{half, half};
    float width = stroke->width;
    if (fraction <= profile[0].position)
        return PrPathStrokeWidth{profile[0].left * width, profile[0].right * width};
    for (uint32_t i = 1; i < count; ++i) {
        const PrPathWidthPoint& a = profile[i - 1];
        const PrPathWidthPoint& b = profile[i];
        if (fraction <= b.position) {
            float span = b.position - a.position;
            float t = span > 0 ? (fraction - a.position) / span : 1;
            return PrPathStrokeWidth{(a.left + (b.left - a.left) * t) * width,
                                     (a.right + (b.right - a.right) * t) * width};
        }
    }
    return PrPathStrokeWidth{profile[count - 1].left * width, profile[count - 1].right * width};
}

float prPathStrokeReach(const PrPathStroke* stroke) {
    if (!(stroke->width > 0))
        return 0;
    // The tip of a miter lies at most half the limit times the width from its point. The corner of a square cap lies at
    // the diagonal of the half width.
    float factor = 1;
    if (stroke->join == PrPathJoin_Miter || stroke->join == PrPathJoin_MiterClip || stroke->join == PrPathJoin_Arcs)
        factor = maxF32(stroke->miterLimit, 1);
    if (stroke->startCap == PrPathCap_Square || stroke->endCap == PrPathCap_Square
        || stroke->dashCap == PrPathCap_Square)
        factor = maxF32(factor, kSqrt2F32);
    return widestHalf(stroke) * factor;
}
