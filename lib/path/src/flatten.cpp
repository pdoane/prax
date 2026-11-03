// Curves flattened to lines.
//
// The distance of a curve from its chord is at most an eighth of the largest magnitude of its second derivative,
// divided by the square of the number of lines. That bound determines how many lines a curve needs.
//
// Over a parameter step of `dt`, a chord falls inside the curve by an eighth of the component of the second derivative
// across the curve, times dt squared. The area between the chord and the curve is two thirds of that distance times the
// length of the chord. Moving both ends of a chord outward by a twelfth of that component times dt squared moves the
// chord out by exactly that area. The lines of pathFlattenQuadArea and pathFlattenCubicArea therefore cover the same
// area as the curve, to the accuracy of treating the curve as a parabola over one step.

#include "path/src/flatten.h"

#include "pr/float.h"

namespace pr_impl {
namespace {

// The maximum number of lines that one curve flattens to.
constexpr uint32_t kCurveMaxLines = 1024;

float length(PrVec2 v) {
    return prF32Sqrt(v.x * v.x + v.y * v.y);
}

// Returns the point moved off the curve for a step of `dt`. It moves by the component of the second derivative across
// the tangent, times dt squared over twelve, in the direction opposite to that component.
PrVec2 balanced(PrVec2 point, PrVec2 tangent, PrVec2 second, float dt) {
    float t2 = tangent.x * tangent.x + tangent.y * tangent.y;
    if (!(t2 > 0))
        return point;
    float along = (second.x * tangent.x + second.y * tangent.y) / t2;
    PrVec2 across{second.x - tangent.x * along, second.y - tangent.y * along};
    float k = dt * dt / 12;
    return point - across * k;
}

// Returns the number of lines for a curve whose largest second difference is `difference`. `scale` relates that
// difference to the greatest distance of the curve from its chords.
uint32_t lineCount(float difference, float scale, float tolerance) {
    float count = prF32Ceil(prF32Sqrt(difference * scale / tolerance));
    if (!(count >= 1))
        return 1;
    return count > float(kCurveMaxLines) ? kCurveMaxLines : uint32_t(count);
}

// Appends the balanced points between the ends of a curve for `count` lines, starting from `at`. `at` returns the point
// at a parameter and its first and second derivatives. The ends stay on the curve. The first and last points are placed
// 1/sqrt(2) of a step in from the ends. A chord of that fraction of a step falls inside the curve by that fraction
// cubed of the amount for a full step, and moving the one end of the chord that moves balances it. A single line keeps
// its chord.
template <typename At>
void flattenArea(uint32_t count, RawArray<PrVec2>& out, At at) {
    if (count < 2)
        return;
    constexpr float kEnd = 0.70710678f;
    float dt = 1 / (float(count - 1) + 2 * kEnd);
    for (uint32_t i = 0; i < count; ++i) {
        float t = (kEnd + float(i)) * dt;
        PrVec2 tangent{};
        PrVec2 second{};
        PrVec2 point = at(t, &tangent, &second);
        out.push(balanced(point, tangent, second, dt));
    }
}

} // namespace

uint32_t pathQuadLineCount(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance) {
    PrVec2 second{p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y};
    return lineCount(length(second), 0.25f, tolerance);
}

uint32_t pathCubicLineCount(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance) {
    PrVec2 second1{p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y};
    PrVec2 second2{p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y};
    return lineCount(prF32Max(length(second1), length(second2)), 0.75f, tolerance);
}

void pathFlattenQuadLines(PrVec2 p0, PrVec2 p1, PrVec2 p2, uint32_t count, RawArray<PrVec2>& out) {
    for (uint32_t i = 1; i < count; ++i) {
        float t = float(i) / float(count);
        float u = 1 - t;
        out.push(PrVec2{
            u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x,
            u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y,
        });
    }
    out.push(p2);
}

void pathFlattenCubicLines(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, uint32_t count, RawArray<PrVec2>& out) {
    for (uint32_t i = 1; i < count; ++i) {
        float t = float(i) / float(count);
        float u = 1 - t;
        float a = u * u * u;
        float b = 3 * u * u * t;
        float c = 3 * u * t * t;
        float d = t * t * t;
        out.push(PrVec2{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y});
    }
    out.push(p3);
}

void pathFlattenQuad(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance, RawArray<PrVec2>& out) {
    pathFlattenQuadLines(p0, p1, p2, pathQuadLineCount(p0, p1, p2, tolerance), out);
}

void pathFlattenCubic(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance, RawArray<PrVec2>& out) {
    pathFlattenCubicLines(p0, p1, p2, p3, pathCubicLineCount(p0, p1, p2, p3, tolerance), out);
}

void pathFlattenQuadArea(PrVec2 p0, PrVec2 p1, PrVec2 p2, float tolerance, RawArray<PrVec2>& out) {
    PrVec2 second{2 * (p0.x - 2 * p1.x + p2.x), 2 * (p0.y - 2 * p1.y + p2.y)};
    flattenArea(pathQuadLineCount(p0, p1, p2, tolerance), out, [&](float t, PrVec2* tangent, PrVec2* curvature) {
        float u = 1 - t;
        *tangent = PrVec2{2 * (u * (p1.x - p0.x) + t * (p2.x - p1.x)), 2 * (u * (p1.y - p0.y) + t * (p2.y - p1.y))};
        *curvature = second;
        return PrVec2{u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y};
    });
    out.push(p2);
}

void pathFlattenCubicArea(PrVec2 p0, PrVec2 p1, PrVec2 p2, PrVec2 p3, float tolerance, RawArray<PrVec2>& out) {
    flattenArea(pathCubicLineCount(p0, p1, p2, p3, tolerance), out, [&](float t, PrVec2* tangent, PrVec2* curvature) {
        float u = 1 - t;
        *tangent = PrVec2{
            3 * (u * u * (p1.x - p0.x) + 2 * u * t * (p2.x - p1.x) + t * t * (p3.x - p2.x)),
            3 * (u * u * (p1.y - p0.y) + 2 * u * t * (p2.y - p1.y) + t * t * (p3.y - p2.y)),
        };
        *curvature = PrVec2{
            6 * (u * (p2.x - 2 * p1.x + p0.x) + t * (p3.x - 2 * p2.x + p1.x)),
            6 * (u * (p2.y - 2 * p1.y + p0.y) + t * (p3.y - 2 * p2.y + p1.y)),
        };
        float a = u * u * u;
        float b = 3 * u * u * t;
        float c = 3 * u * t * t;
        float d = t * t * t;
        return PrVec2{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
    });
    out.push(p3);
}

} // namespace pr_impl
