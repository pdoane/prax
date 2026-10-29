// Evaluating a Lottie animation's properties and transforms at a frame.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#include "lottie/src/lottie_eval.h"

#include "pr/float.h"

#include "lottie/src/lottie_expression.h"

namespace pr_impl {

namespace {

// The number of points at which a spatial keyframe's path is sampled to measure its length.
constexpr uint32_t kSpatialSamples = 150;

// The frames over which auto-orient measures the position's motion.
constexpr double kOrientSpan = 0.01;

// The frames over which auto-orient measures a combined position's motion past its last keyframe.
constexpr double kOrientSpanAtEnd = 0.05;

constexpr double kDegrees = kPiF64 / 180;

double bezierAt(double t, double p1, double p2) {
    double u = 1 - t;
    return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t;
}

double bezierSlope(double t, double p1, double p2) {
    return 3 * (1 - t) * (1 - t) * p1 + 6 * (1 - t) * t * (p2 - p1) + 3 * t * t * (1 - p2);
}

// Writes the point on a spatial keyframe's path at `fraction` of the path's length.
void spatialAt(const float* s, const float* e, const float* to, const float* ti, uint32_t dims,
               double fraction, float* out) {
    auto point = [&](double t, uint32_t i) {
        double u = 1 - t;
        return u * u * u * double(s[i]) + 3 * u * u * t * double(s[i] + to[i])
               + 3 * u * t * t * double(e[i] + ti[i]) + t * t * t * double(e[i]);
    };
    double lengths[kSpatialSamples];
    double total = 0;
    lengths[0] = 0;
    for (uint32_t k = 1; k < kSpatialSamples; ++k) {
        double t0 = double(k - 1) / (kSpatialSamples - 1);
        double t1 = double(k) / (kSpatialSamples - 1);
        double squared = 0;
        for (uint32_t i = 0; i < dims; ++i) {
            double d = point(t1, i) - point(t0, i);
            squared += d * d;
        }
        total += sqrtF64(squared);
        lengths[k] = total;
    }
    double target = total * fraction;
    if (fraction <= 0 || target <= 0) {
        for (uint32_t i = 0; i < dims; ++i)
            out[i] = float(point(0, i));
        return;
    }
    for (uint32_t k = 1; k < kSpatialSamples; ++k) {
        if (target < lengths[k]) {
            double segment = lengths[k] - lengths[k - 1];
            double t = segment > 0 ? (target - lengths[k - 1]) / segment : 0;
            double t0 = double(k - 1) / (kSpatialSamples - 1);
            double t1 = double(k) / (kSpatialSamples - 1);
            for (uint32_t i = 0; i < dims; ++i)
                out[i] = float(point(t0, i) + (point(t1, i) - point(t0, i)) * t);
            return;
        }
    }
    for (uint32_t i = 0; i < dims; ++i)
        out[i] = float(point(1, i));
}

// Returns the angle of the position's motion, which auto-orient rotates to. The angle is measured over the kOrientSpan
// frames before `frame`. If the frame is outside the keyframes, it is measured after the first keyframe or before the
// last.
float orientation(const LottieAnimation& a, const LottieTransform& t, double frame) {
    uint32_t keyed = t.position != kLottieNone ? t.position : t.positionX;
    if (keyed == kLottieNone || a.properties.data[keyed].keyframeCount == 0)
        return 0;
    const LottieProperty& p = a.properties.data[keyed];
    double first = a.keyframes.data[p.keyframe].time;
    double last = a.keyframes.data[p.keyframe + p.keyframeCount - 1].time;
    double to = frame;
    double from = frame - kOrientSpan;
    if (frame <= first) {
        to = first + kOrientSpan;
        from = first;
    } else if (frame >= last) {
        to = last;
        from = last - (t.position != kLottieNone ? kOrientSpanAtEnd : kOrientSpan);
    }
    PrVec2 v1 = lottieTransformPosition(a, t, to);
    PrVec2 v2 = lottieTransformPosition(a, t, from);
    return atan2F32(v1.y - v2.y, v1.x - v2.x);
}

// Matrix operations, each applied after the matrix's existing operations.
void then(PrMatrix4& m, const PrMatrix4& operation) {
    m = prMatrix4Concat(&m, &operation);
}

void translate(PrMatrix4& m, double x, double y, double z) {
    then(m, prMatrix4Translate(PrVec3{float(x), float(y), float(z)}));
}

void scale(PrMatrix4& m, double x, double y, double z) {
    then(m, prMatrix4Scale(PrVec3{float(x), float(y), float(z)}));
}

// Applies a rotation about the axis (x, y, z), in the direction that prMatrix4Rotate uses.
void rotate(PrMatrix4& m, float x, float y, float z, double radians) {
    then(m, prMatrix4Rotate(PrVec3{x, y, z}, float(radians)));
}

void skewFromAxis(PrMatrix4& m, double skew, double axis) {
    rotate(m, 0, 0, 1, axis);
    then(m, PrMatrix4{{{1, float(tanF64(skew)), 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    rotate(m, 0, 0, 1, -axis);
}

// Writes up to three floats of a property's value, and `fallback` for each float that the property does not have.
void values3(const LottieAnimation& a, uint32_t property, double frame, float fallback, float* out) {
    out[0] = out[1] = out[2] = fallback;
    uint32_t dims = lottieDims(a, property);
    if (dims == 0)
        return;
    float v[16];
    if (dims > 16)
        return;
    lottieEvaluate(a, property, frame, v);
    for (uint32_t i = 0; i < 3 && i < dims; ++i)
        out[i] = v[i];
}

} // namespace

double lottieEase(double x1, double y1, double x2, double y2, double x) {
    if (x1 == y1 && x2 == y2)
        return x;
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    x1 = minF64(maxF64(x1, 0), 1);
    x2 = minF64(maxF64(x2, 0), 1);
    // Solves for the t at `x` by Newton's method, starting from a guess from a table. Falls back to bisection if the
    // slope is too flat.
    constexpr uint32_t kTable = 11;
    constexpr double kStep = 1.0 / (kTable - 1);
    double samples[kTable];
    for (uint32_t i = 0; i < kTable; ++i)
        samples[i] = bezierAt(double(i) * kStep, x1, x2);
    double start = 0;
    uint32_t sample = 1;
    for (; sample != kTable - 1 && samples[sample] <= x; ++sample)
        start += kStep;
    --sample;
    double dist = (x - samples[sample]) / (samples[sample + 1] - samples[sample]);
    double t = start + dist * kStep;
    double slope = bezierSlope(t, x1, x2);
    if (slope >= 0.001) {
        for (uint32_t i = 0; i < 4; ++i) {
            double s = bezierSlope(t, x1, x2);
            if (s == 0)
                break;
            t -= (bezierAt(t, x1, x2) - x) / s;
        }
    } else if (slope != 0) {
        double a = start;
        double b = start + kStep;
        for (uint32_t i = 0; i < 10; ++i) {
            t = a + (b - a) / 2;
            double current = bezierAt(t, x1, x2) - x;
            if (absF64(current) <= 0.0000001)
                break;
            if (current > 0)
                b = t;
            else
                a = t;
        }
    }
    return bezierAt(t, y1, y2);
}

uint32_t lottieDims(const LottieAnimation& a, uint32_t property) {
    return property == kLottieNone ? 0 : a.properties.data[property].dims;
}

void lottieEvaluate(const LottieAnimation& a, uint32_t property, double frame, float* out) {
    lottieEvaluateKeyframes(a, property, frame, out);
    const LottieProperty& p = a.properties.data[property];
    if (p.expression == kLottieNone || a.expressionDepth >= kLottieMaxExpressionDepth)
        return;
    ++a.expressionDepth;
    lottieExpressionEvaluate(a, property, frame, out);
    --a.expressionDepth;
}

void lottieEvaluateKeyframes(const LottieAnimation& a, uint32_t property, double frame, float* out) {
    const LottieProperty& p = a.properties.data[property];
    const float* numbers = a.numbers.data;
    if (p.keyframeCount == 0) {
        for (uint32_t i = 0; i < p.dims; ++i)
            out[i] = numbers[p.value + i];
        return;
    }
    const LottieKeyframe* keys = a.keyframes.data + p.keyframe;
    uint32_t count = p.keyframeCount;
    // Before the first keyframe, the value is the first keyframe's start. At or after the last keyframe, the value
    // holds.
    if (count == 1 || frame < keys[0].time) {
        for (uint32_t i = 0; i < p.dims; ++i)
            out[i] = numbers[keys[0].start + i];
        return;
    }
    if (frame >= keys[count - 1].time) {
        const LottieKeyframe& last = keys[count - 1];
        const LottieKeyframe& before = keys[count - 2];
        // The last keyframe carries its own value only if it specifies one. Otherwise the value is where the previous
        // keyframe ends.
        uint32_t value = before.hold ? last.start : before.end;
        for (uint32_t i = 0; i < p.dims; ++i)
            out[i] = numbers[value + i];
        return;
    }
    uint32_t k = 0;
    while (k + 2 < count && frame >= keys[k + 1].time)
        ++k;
    const LottieKeyframe& key = keys[k];
    const float* s = numbers + key.start;
    const float* e = numbers + key.end;
    if (key.hold) {
        for (uint32_t i = 0; i < p.dims; ++i)
            out[i] = s[i];
        return;
    }
    double span = keys[k + 1].time - key.time;
    double x = span > 0 ? (frame - key.time) / span : 1;
    auto eased = [&](uint32_t dim) {
        if (key.easing == kLottieNone)
            return x;
        const float* ease = numbers + key.easing + (dim < key.easingDims ? dim : 0) * 4;
        return lottieEase(double(ease[0]), double(ease[1]), double(ease[2]), double(ease[3]), x);
    };
    if (key.spatial != kLottieNone) {
        spatialAt(s, e, numbers + key.spatial, numbers + key.spatial + p.dims, p.dims, eased(0), out);
        return;
    }
    for (uint32_t i = 0; i < p.dims; ++i)
        out[i] = float(double(s[i]) + (double(e[i]) - double(s[i])) * eased(i));
}

float lottieScalar(const LottieAnimation& a, uint32_t property, double frame, float fallback) {
    if (property == kLottieNone)
        return fallback;
    const LottieProperty& p = a.properties.data[property];
    if (p.dims == 1) {
        float value = 0;
        lottieEvaluate(a, property, frame, &value);
        return value;
    }
    float values[16];
    if (p.dims > 16)
        return fallback;
    lottieEvaluate(a, property, frame, values);
    return values[0];
}

PrVec2 lottiePoint(const LottieAnimation& a, uint32_t property, double frame, PrVec2 fallback) {
    if (property == kLottieNone)
        return fallback;
    const LottieProperty& p = a.properties.data[property];
    float values[16];
    if (p.dims > 16 || p.dims == 0)
        return fallback;
    lottieEvaluate(a, property, frame, values);
    return PrVec2{values[0], p.dims > 1 ? values[1] : values[0]};
}

PrVec2 lottieTransformPosition(const LottieAnimation& a, const LottieTransform& t, double frame) {
    if (t.position != kLottieNone)
        return lottiePoint(a, t.position, frame, PrVec2{0, 0});
    return PrVec2{lottieScalar(a, t.positionX, frame, 0), lottieScalar(a, t.positionY, frame, 0)};
}

PrMatrix4 lottieTransformMatrix4(const LottieAnimation& a, const LottieTransform& t, double frame, bool autoOrient) {
    float anchor[3];
    float scaled[3];
    float position[3];
    float orient[3];
    values3(a, t.anchor, frame, 0, anchor);
    values3(a, t.scale, frame, 100, scaled);
    values3(a, t.orientation, frame, 0, orient);
    if (lottieDims(a, t.scale) == 2)
        scaled[2] = 100;
    if (t.position != kLottieNone) {
        values3(a, t.position, frame, 0, position);
        if (lottieDims(a, t.position) == 2)
            position[2] = 0;
    } else {
        position[0] = lottieScalar(a, t.positionX, frame, 0);
        position[1] = lottieScalar(a, t.positionY, frame, 0);
        position[2] = lottieScalar(a, t.positionZ, frame, 0);
    }
    if (lottieDims(a, t.anchor) == 2)
        anchor[2] = 0;
    PrMatrix4 m = prMatrix4Identity();
    translate(m, -double(anchor[0]), -double(anchor[1]), double(anchor[2]));
    scale(m, double(scaled[0]) / 100, double(scaled[1]) / 100, double(scaled[2]) / 100);
    if (t.skew != kLottieNone)
        skewFromAxis(m, -double(lottieScalar(a, t.skew, frame, 0)) * kDegrees,
                     double(lottieScalar(a, t.skewAxis, frame, 0)) * kDegrees);
    rotate(m, 0, 0, 1, double(lottieScalar(a, t.rotation, frame, 0)) * kDegrees);
    rotate(m, 0, 1, 0, -double(lottieScalar(a, t.rotationY, frame, 0)) * kDegrees);
    rotate(m, 1, 0, 0, -double(lottieScalar(a, t.rotationX, frame, 0)) * kDegrees);
    rotate(m, 0, 0, 1, double(orient[2]) * kDegrees);
    rotate(m, 0, 1, 0, -double(orient[1]) * kDegrees);
    rotate(m, 1, 0, 0, -double(orient[0]) * kDegrees);
    if (autoOrient)
        rotate(m, 0, 0, 1, double(orientation(a, t, frame)));
    translate(m, double(position[0]), double(position[1]), -double(position[2]));
    return m;
}

PrMatrix4 lottieCameraView(const LottieAnimation& a, const LottieTransform& t, double frame, PrVec2 extent,
                           float zoom) {
    float position[3];
    float interest[3];
    float orient[3];
    if (t.position != kLottieNone) {
        values3(a, t.position, frame, 0, position);
    } else {
        position[0] = lottieScalar(a, t.positionX, frame, 0);
        position[1] = lottieScalar(a, t.positionY, frame, 0);
        position[2] = lottieScalar(a, t.positionZ, frame, 0);
    }
    values3(a, t.orientation, frame, 0, orient);
    PrMatrix4 m = prMatrix4Identity();
    translate(m, -double(position[0]), -double(position[1]), double(position[2]));
    // A two-node camera faces its point of interest: turned about y, then tilted about x.
    if (t.anchor != kLottieNone) {
        values3(a, t.anchor, frame, 0, interest);
        double toward[3] = {double(position[0] - interest[0]), double(position[1] - interest[1]),
                            double(position[2] - interest[2])};
        double length = sqrtF64(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
        if (length > 0) {
            for (double& v : toward)
                v /= length;
            double across = sqrtF64(toward[0] * toward[0] + toward[2] * toward[2]);
            rotate(m, 0, 1, 0, -atan2F64(toward[0], -toward[2]));
            rotate(m, 1, 0, 0, atan2F64(toward[1], across));
        }
    }
    rotate(m, 1, 0, 0, double(lottieScalar(a, t.rotationX, frame, 0)) * kDegrees);
    rotate(m, 0, 1, 0, double(lottieScalar(a, t.rotationY, frame, 0)) * kDegrees);
    rotate(m, 0, 0, 1, -double(lottieScalar(a, t.rotation, frame, 0)) * kDegrees);
    rotate(m, 1, 0, 0, double(orient[0]) * kDegrees);
    rotate(m, 0, 1, 0, double(orient[1]) * kDegrees);
    rotate(m, 0, 0, 1, -double(orient[2]) * kDegrees);
    translate(m, double(extent.x) / 2, double(extent.y) / 2, double(zoom));
    return m;
}

PrAffine2 lottieTransformAffine(const LottieAnimation& a, const LottieTransform& t, double frame, bool autoOrient) {
    PrVec2 anchor = lottiePoint(a, t.anchor, frame, PrVec2{0, 0});
    PrVec2 scale = lottiePoint(a, t.scale, frame, PrVec2{100, 100});
    float rotation = lottieScalar(a, t.rotation, frame, 0);
    float skew = lottieScalar(a, t.skew, frame, 0);
    float skewAxis = lottieScalar(a, t.skewAxis, frame, 0);
    PrVec2 position = lottieTransformPosition(a, t, frame);
    PrAffine2 m = prAffine2Translate(-anchor);
    m = prAffine2Concat(m, prAffine2Scale(scale / 100));
    if (skew != 0) {
        // Skews along the skew axis: rotates into the axis, shears along it by the negated angle, and rotates back.
        float axis = float(double(skewAxis) * kDegrees);
        PrAffine2 shear{1, 0, -float(tanF64(double(skew) * kDegrees)), 1, 0, 0};
        m = prAffine2Concat(m, prAffine2Rotate(axis));
        m = prAffine2Concat(m, shear);
        m = prAffine2Concat(m, prAffine2Rotate(-axis));
    }
    if (rotation != 0)
        m = prAffine2Concat(m, prAffine2Rotate(float(double(rotation) * kDegrees)));
    if (autoOrient)
        m = prAffine2Concat(m, prAffine2Rotate(orientation(a, t, frame)));
    return prAffine2Concat(m, prAffine2Translate(position));
}

float lottieTransformOpacity(const LottieAnimation& a, const LottieTransform& t, double frame) {
    return lottieScalar(a, t.opacity, frame, 100) / 100;
}

bool lottieTransformFixed(const LottieAnimation& a, const LottieTransform& t) {
    const uint32_t parts[] = {t.anchor,    t.position,  t.positionX,   t.positionY, t.positionZ, t.scale,
                              t.rotation,  t.rotationX, t.rotationY, t.orientation, t.skew,      t.skewAxis};
    for (uint32_t p : parts) {
        if (p != kLottieNone && a.properties.data[p].keyframeCount > 0)
            return false;
    }
    return true;
}

} // namespace pr_impl
