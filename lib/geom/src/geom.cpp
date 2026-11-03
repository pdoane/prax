// Points, rectangles and transforms in two dimensions.

#include "pr/geom.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// The tolerance, relative to the lengths of a transform's axes, within which the axes may differ in length or angle and
// still count as scaling equally.
constexpr float kSimilarityTolerance = 1e-5f;

// The number of halvings the concave pass uses to settle its factor.
constexpr uint32_t kConcaveSearchSteps = 24;

// Returns the factor by which one side needs every radius scaled, or `factor` if that is smaller.
float sideFactor(float factor, float length, float first, float second) {
    float sum = first + second;
    return sum > length ? prF32Min(factor, length / sum) : factor;
}

// Returns sin(πt/2) for t in [0, 1], which is exactly 0 and 1 at the ends. In floats, cos(πt/2) is not exact at the
// ends.
float quarterSine(float t) {
    return prF32Clamp(prF32Sin(kPrF32Pi / 2 * prF32Clamp(t, 0, 1)), 0, 1);
}

// A corner's position on the rectangle: its point, the direction of the side before it, and the direction of the side
// after it.
struct CornerFrame {
    PrVec2 point;
    PrVec2 before;
    PrVec2 after;
};

CornerFrame frameOf(PrRect r, uint32_t corner) {
    switch (corner) {
        case PrRectCorner_TopLeft: return CornerFrame{{r.left, r.top}, {0, -1}, {1, 0}};
        case PrRectCorner_TopRight: return CornerFrame{{r.right, r.top}, {1, 0}, {0, 1}};
        case PrRectCorner_BottomRight: return CornerFrame{{r.right, r.bottom}, {0, 1}, {-1, 0}};
        default: return CornerFrame{{r.left, r.bottom}, {-1, 0}, {0, -1}};
    }
}

// Writes the radius along the side before a corner, and the radius along the side after it.
void cornerRadii(uint32_t corner, PrVec2 radii, float* before, float* after) {
    bool vertical = corner == PrRectCorner_TopLeft || corner == PrRectCorner_BottomRight;
    *before = vertical ? radii.y : radii.x;
    *after = vertical ? radii.x : radii.y;
}

// Writes the triangle that contains a corner's curve: its start, its end, and either the rectangle's corner for a
// convex curve or the box's inner corner for a concave one.
void hullOf(PrRect r, uint32_t corner, PrCorner c, PrVec2 out[3]) {
    CornerFrame f = frameOf(r, corner);
    float before = 0;
    float after = 0;
    cornerRadii(corner, c.radii, &before, &after);
    PrVec2 start{f.point.x - f.before.x * before, f.point.y - f.before.y * before};
    PrVec2 end{f.point.x + f.after.x * after, f.point.y + f.after.y * after};
    out[0] = start;
    out[1] = c.shape < 0 ? start + f.after * after : f.point;
    out[2] = end;
}

// Returns true if the interiors of the two triangles overlap, testing each edge as a separating axis.
bool trianglesOverlap(const PrVec2 a[3], const PrVec2 b[3]) {
    const PrVec2* triangles[2] = {a, b};
    for (const PrVec2* t : triangles) {
        for (uint32_t i = 0; i < 3; ++i) {
            PrVec2 p = t[i];
            PrVec2 q = t[(i + 1) % 3];
            PrVec2 axis{q.y - p.y, p.x - q.x};
            float minA = kPrF32Max;
            float maxA = -kPrF32Max;
            float minB = kPrF32Max;
            float maxB = -kPrF32Max;
            for (uint32_t j = 0; j < 3; ++j) {
                float da = axis.x * a[j].x + axis.y * a[j].y;
                float db = axis.x * b[j].x + axis.y * b[j].y;
                minA = prF32Min(minA, da);
                maxA = prF32Max(maxA, da);
                minB = prF32Min(minB, db);
                maxB = prF32Max(maxB, db);
            }
            float scale = prF32Abs(axis.x) + prF32Abs(axis.y);
            float slack = scale * 1e-4f;
            if (maxA <= minB + slack || maxB <= minA + slack)
                return false;
        }
    }
    return true;
}

// Returns true if any two opposite corners, at least one of them concave, have overlapping hulls after every concave
// corner's radii are scaled by `factor`.
bool concaveOverlap(PrRect r, const PrCorner corners[4], float factor) {
    PrCorner scaled[4];
    for (uint32_t i = 0; i < 4; ++i) {
        scaled[i] = corners[i];
        if (corners[i].shape < 0)
            scaled[i].radii = corners[i].radii * factor;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const PrCorner& first = scaled[i];
        const PrCorner& second = scaled[i + 2];
        if ((first.shape >= 0 && second.shape >= 0) || prCornerIsSquare(first) || prCornerIsSquare(second))
            continue;
        PrVec2 a[3];
        PrVec2 b[3];
        hullOf(r, i, first, a);
        hullOf(r, i + 2, second, b);
        if (trianglesOverlap(a, b))
            return true;
    }
    return false;
}

int32_t pixelEdge(float whole) {
    return int32_t(prF32Clamp(whole, -float(kPrIRectMaxCoordinate), float(kPrIRectMaxCoordinate)));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles

PrRect prRectAt(PrVec2 origin, PrVec2 extent) {
    return PrRect{origin.x, origin.y, origin.x + extent.x, origin.y + extent.y};
}

bool prRectIsEmpty(PrRect rect) {
    return !(rect.right > rect.left && rect.bottom > rect.top);
}

float prRectWidth(PrRect rect) {
    return rect.right > rect.left ? rect.right - rect.left : 0;
}

float prRectHeight(PrRect rect) {
    return rect.bottom > rect.top ? rect.bottom - rect.top : 0;
}

PrVec2 prRectCenter(PrRect rect) {
    return PrVec2{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

PrVec2 prRectCornerPoint(PrRect rect, PrRectCorner corner) {
    switch (corner) {
        case PrRectCorner_TopLeft:
            return PrVec2{rect.left, rect.top};
        case PrRectCorner_TopRight:
            return PrVec2{rect.right, rect.top};
        case PrRectCorner_BottomRight:
            return PrVec2{rect.right, rect.bottom};
        default:
            PR_ASSERT(corner == PrRectCorner_BottomLeft);
            return PrVec2{rect.left, rect.bottom};
    }
}

PrRect prRectUnion(PrRect a, PrRect b) {
    if (prRectIsEmpty(a))
        return b;
    if (prRectIsEmpty(b))
        return a;
    return PrRect{
        .left = prF32Min(a.left, b.left),
        .top = prF32Min(a.top, b.top),
        .right = prF32Max(a.right, b.right),
        .bottom = prF32Max(a.bottom, b.bottom),
    };
}

PrRect prRectIntersect(PrRect a, PrRect b) {
    return PrRect{
        .left = prF32Max(a.left, b.left),
        .top = prF32Max(a.top, b.top),
        .right = prF32Min(a.right, b.right),
        .bottom = prF32Min(a.bottom, b.bottom),
    };
}

bool prRectIsOnPixels(PrRect rect) {
    float edges[4] = {rect.left, rect.top, rect.right, rect.bottom};
    for (float e : edges) {
        if (!(prF32Abs(e - prF32Round(e)) <= kPrRectOnPixelTolerance))
            return false;
    }
    return true;
}

bool prIRectIsEmpty(PrIRect rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

uint32_t prIRectWidth(PrIRect rect) {
    return rect.right > rect.left ? uint32_t(rect.right - rect.left) : 0;
}

uint32_t prIRectHeight(PrIRect rect) {
    return rect.bottom > rect.top ? uint32_t(rect.bottom - rect.top) : 0;
}

PrIRect prIRectRoundOut(PrRect rect) {
    if (prRectIsEmpty(rect))
        return PrIRect{0, 0, 0, 0};
    float t = kPrRectOnPixelTolerance;
    return PrIRect{
        pixelEdge(prF32Floor(rect.left + t)), pixelEdge(prF32Floor(rect.top + t)),
        pixelEdge(prF32Ceil(rect.right - t)), pixelEdge(prF32Ceil(rect.bottom - t)),
    };
}

PrIRect prIRectRound(PrRect rect) {
    if (prRectIsEmpty(rect))
        return PrIRect{0, 0, 0, 0};
    return PrIRect{
        pixelEdge(prF32Ceil(rect.left - 0.5f)), pixelEdge(prF32Ceil(rect.top - 0.5f)),
        pixelEdge(prF32Ceil(rect.right - 0.5f)), pixelEdge(prF32Ceil(rect.bottom - 0.5f)),
    };
}

PrRect prRectFromIRect(PrIRect rect) {
    return PrRect{float(rect.left), float(rect.top), float(rect.right), float(rect.bottom)};
}

PrIRect prIRectAt(PrIVec2 origin, PrUVec2 extent) {
    return PrIRect{origin.x, origin.y, origin.x + int32_t(extent.x), origin.y + int32_t(extent.y)};
}

PrIRect prIRectUnion(PrIRect a, PrIRect b) {
    if (prIRectIsEmpty(a))
        return b;
    if (prIRectIsEmpty(b))
        return a;
    return PrIRect{
        a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top,
        a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom,
    };
}

PrIRect prIRectIntersect(PrIRect a, PrIRect b) {
    return PrIRect{
        a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
        a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom,
    };
}

PrIRect prIRectOutset(PrIRect rect, int32_t by) {
    return PrIRect{rect.left - by, rect.top - by, rect.right + by, rect.bottom + by};
}

bool prIRectContains(PrIRect outer, PrIRect inner) {
    return !prIRectIsEmpty(inner) && inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}

// ---------------------------------------------------------------------------------------------------------------------
// Corners

bool prCornerIsSquare(PrCorner corner) {
    return !(corner.radii.x > 0 && corner.radii.y > 0) || corner.shape == kPrF32Infinity;
}

PrVec2 prCornerCurvePoint(float shape, float t) {
    bool concave = shape < 0;
    float k = prF32Abs(shape);
    if (k == kPrF32Infinity) {
        // The curve is two straight lines through the box's outer corner, or through its inner one.
        if (t <= 0.5f)
            return concave ? PrVec2{0, 2 * t} : PrVec2{2 * t, 0};
        return concave ? PrVec2{2 * t - 1, 1} : PrVec2{1, 2 * t - 1};
    }
    float p = prF32Pow(2, 1 - k);
    float s = prF32Pow(quarterSine(t), p);
    float c = prF32Pow(quarterSine(1 - t), p);
    return concave ? PrVec2{1 - c, s} : PrVec2{s, 1 - c};
}

PrVec2 prCornerCurveTangent(float shape, float t) {
    bool concave = shape < 0;
    float k = prF32Abs(shape);
    if (k == kPrF32Infinity) {
        if (t < 0.5f)
            return concave ? PrVec2{0, 1} : PrVec2{1, 0};
        return concave ? PrVec2{1, 0} : PrVec2{0, 1};
    }
    // The derivative scaled by (sin cos)^(1 - p), which keeps it finite if p < 1.
    float q = 2 - prF32Pow(2, 1 - k);
    float s = prF32Pow(quarterSine(t), q);
    float c = prF32Pow(quarterSine(1 - t), q);
    PrVec2 d = concave ? PrVec2{s, c} : PrVec2{c, s};
    float length = prF32Sqrt(d.x * d.x + d.y * d.y);
    return length > 0 ? d / length : PrVec2{1, 0};
}

PrCorner prCornerRound(PrVec2 radii) {
    return PrCorner{radii, 1};
}

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles with shaped corners

PrCornerRect prCornerRectRound(PrRect rect, float radius) {
    PrCorner round = prCornerRound(PrVec2{radius, radius});
    return PrCornerRect{rect, {round, round, round, round}};
}

PrCornerRect prCornerRectFit(PrCornerRect shape) {
    const PrRect& rect = shape.rect;
    PrCorner* c = shape.corners;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!(c[i].radii.x > 0 && c[i].radii.y > 0))
            c[i].radii = PrVec2{0, 0};
    }
    float width = rect.right - rect.left;
    float height = rect.bottom - rect.top;
    float factor = 1;
    factor = sideFactor(factor, width, c[PrRectCorner_TopLeft].radii.x, c[PrRectCorner_TopRight].radii.x);
    factor = sideFactor(factor, width, c[PrRectCorner_BottomLeft].radii.x, c[PrRectCorner_BottomRight].radii.x);
    factor = sideFactor(factor, height, c[PrRectCorner_TopLeft].radii.y, c[PrRectCorner_BottomLeft].radii.y);
    factor = sideFactor(factor, height, c[PrRectCorner_TopRight].radii.y, c[PrRectCorner_BottomRight].radii.y);
    if (!(factor > 0))
        factor = 0;
    for (PrCorner& corner : shape.corners)
        corner.radii = corner.radii * factor;

    if (concaveOverlap(rect, c, 1)) {
        float low = 0;
        float high = 1;
        for (uint32_t step = 0; step < kConcaveSearchSteps; ++step) {
            float middle = (low + high) / 2;
            if (concaveOverlap(rect, c, middle))
                high = middle;
            else
                low = middle;
        }
        for (PrCorner& corner : shape.corners) {
            if (corner.shape < 0)
                corner.radii = corner.radii * low;
        }
    }
    return shape;
}

PrCornerRect prCornerRectInset(PrCornerRect shape, const float insets[PR_NONNULL PrRectSide_Count]) {
    PrCornerRect fittedShape = prCornerRectFit(shape);
    const PrRect& rect = shape.rect;
    PrCorner* fitted = fittedShape.corners;
    float top = insets[PrRectSide_Top];
    float right = insets[PrRectSide_Right];
    float bottom = insets[PrRectSide_Bottom];
    float left = insets[PrRectSide_Left];
    PrRect inner{rect.left + left, rect.top + top, rect.right - right, rect.bottom - bottom};
    float horizontal[4] = {left, right, right, left};
    float vertical[4] = {top, top, bottom, bottom};
    for (uint32_t i = 0; i < 4; ++i) {
        // A convex corner shrinks so that the midpoint of its curve moves inward by the insets across the corner's
        // diagonal. The midpoint lies (1 - 2^(-1 / 2^K)) of each radius from the corner. A round corner therefore
        // shrinks by the insets about its center, and a bevel stays parallel to itself. A concave corner moves inward
        // whole.
        PrCorner c = fitted[i];
        if (c.shape >= 0) {
            // A round corner's midpoint is 1 - sqrt(1/2) of its radii from the corner, so its factor is 1.
            float midpoint = 1;
            if (c.shape == 1)
                midpoint = 1 - prF32Sqrt(0.5f);
            else if (prF32IsFinite(c.shape))
                midpoint = 1 - prF32Pow(2, -1 / prF32Pow(2, c.shape));
            float factor = (1 - prF32Sqrt(0.5f)) / midpoint;
            c.radii = PrVec2{c.radii.x - horizontal[i] * factor, c.radii.y - vertical[i] * factor};
        }
        if (!(c.radii.x > 0 && c.radii.y > 0))
            c.radii = PrVec2{0, 0};
        fitted[i] = c;
    }
    fittedShape.rect = inner;
    if (prRectIsEmpty(inner)) {
        for (PrCorner& c : fittedShape.corners)
            c.radii = PrVec2{0, 0};
        return fittedShape;
    }
    return prCornerRectFit(fittedShape);
}

PrCornerRect prCornerRectOutset(PrCornerRect shape, const float outsets[PR_NONNULL PrRectSide_Count]) {
    PrCornerRect grown = prCornerRectFit(shape);
    const PrRect& rect = shape.rect;
    grown.rect = PrRect{
        rect.left - outsets[PrRectSide_Left], rect.top - outsets[PrRectSide_Top],
        rect.right + outsets[PrRectSide_Right], rect.bottom + outsets[PrRectSide_Bottom],
    };
    float horizontal[4] = {
        outsets[PrRectSide_Left], outsets[PrRectSide_Right], outsets[PrRectSide_Right],
        outsets[PrRectSide_Left],
    };
    float vertical[4] = {
        outsets[PrRectSide_Top], outsets[PrRectSide_Top], outsets[PrRectSide_Bottom],
        outsets[PrRectSide_Bottom],
    };
    auto grow = [](float radius, float outset) {
        if (!(radius > 0) || !(outset > 0))
            return radius;
        if (radius >= outset)
            return radius + outset;
        float ratio = radius / outset - 1;
        return radius + outset * (1 + ratio * ratio * ratio);
    };
    for (uint32_t i = 0; i < 4; ++i) {
        PrCorner& c = grown.corners[i];
        if (c.radii.x > 0 && c.radii.y > 0)
            c.radii = PrVec2{grow(c.radii.x, horizontal[i]), grow(c.radii.y, vertical[i])};
    }
    return prCornerRectFit(grown);
}

bool prCornerRectContains(PrCornerRect shape, PrVec2 point) {
    const PrRect& r = shape.rect;
    if (!(point.x >= r.left && point.x <= r.right && point.y >= r.top && point.y <= r.bottom))
        return false;
    PrCornerRect fitted = prCornerRectFit(shape);
    for (uint32_t i = 0; i < PrRectCorner_Count; ++i) {
        const PrCorner& c = fitted.corners[i];
        if (!(c.radii.x > 0 && c.radii.y > 0))
            continue;
        // The point's distance from the corner along each side, as a fraction of the corner's radius.
        bool right = i == PrRectCorner_TopRight || i == PrRectCorner_BottomRight;
        bool bottom = i == PrRectCorner_BottomRight || i == PrRectCorner_BottomLeft;
        float a = (right ? r.right - point.x : point.x - r.left) / c.radii.x;
        float b = (bottom ? r.bottom - point.y : point.y - r.top) / c.radii.y;
        if (!(a < 1 && b < 1))
            continue;
        // A convex corner holds the points within the superellipse of exponent 2^K around its center. A concave corner
        // is that curve of -K reflected into the box, centered on the corner, and holds the points beyond it.
        float exponent = prF32Pow(2, c.shape >= 0 ? c.shape : -c.shape);
        if (c.shape >= 0)
            return prF32Pow(1 - a, exponent) + prF32Pow(1 - b, exponent) <= 1;
        return prF32Pow(a, exponent) + prF32Pow(b, exponent) >= 1;
    }
    return true;
}

PrCornerPlacement prCornerRectPlacement(PrCornerRect shape, PrRectCorner corner) {
    const PrCorner& k = shape.corners[corner];
    PrVec2 point = prRectCornerPoint(shape.rect, corner);
    switch (corner) {
        case PrRectCorner_TopLeft:
            return PrCornerPlacement{point, {0, -1}, {1, 0}, k.radii.y, k.radii.x, k.shape};
        case PrRectCorner_TopRight:
            return PrCornerPlacement{point, {1, 0}, {0, 1}, k.radii.x, k.radii.y, k.shape};
        case PrRectCorner_BottomRight:
            return PrCornerPlacement{point, {0, 1}, {-1, 0}, k.radii.y, k.radii.x, k.shape};
        default:
            PR_ASSERT(corner == PrRectCorner_BottomLeft);
            return PrCornerPlacement{point, {-1, 0}, {0, -1}, k.radii.x, k.radii.y, k.shape};
    }
}

PrVec2 prCornerPlacementPoint(PrCornerPlacement placement, PrVec2 uv) {
    PrVec2 start = placement.point - placement.before * placement.radiusBefore;
    return start + placement.before * (placement.radiusBefore * uv.x) + placement.after * (placement.radiusAfter * uv.y);
}

// ---------------------------------------------------------------------------------------------------------------------
// Affine transforms of the plane

PrAffine2 prAffine2Identity(void) {
    return PrAffine2{1, 0, 0, 1, 0, 0};
}

PrAffine2 prAffine2Translate(PrVec2 offset) {
    return PrAffine2{1, 0, 0, 1, offset.x, offset.y};
}

PrAffine2 prAffine2Scale(PrVec2 factors) {
    return PrAffine2{factors.x, 0, 0, factors.y, 0, 0};
}

PrAffine2 prAffine2Rotate(float radians) {
    float s = prF32Sin(radians);
    float c = prF32Cos(radians);
    // At a multiple of a quarter turn, the sine or cosine that float error leaves near zero becomes zero, and the other
    // becomes one, so that a whole turn is the identity.
    constexpr float kNearZero = 1e-6f;
    if (prF32Abs(s) < kNearZero) {
        s = 0;
        c = c < 0 ? -1.0f : 1.0f;
    } else if (prF32Abs(c) < kNearZero) {
        c = 0;
        s = s < 0 ? -1.0f : 1.0f;
    }
    return PrAffine2{c, s, -s, c, 0, 0};
}

PrAffine2 prAffine2Concat(PrAffine2 first, PrAffine2 second) {
    const PrAffine2& m = second;
    const PrAffine2& n = first;
    return PrAffine2{
        m.a * n.a + m.c * n.b,       m.b * n.a + m.d * n.b,
        m.a * n.c + m.c * n.d,       m.b * n.c + m.d * n.d,
        m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f,
    };
}

bool prAffine2Invert(PrAffine2 transform, PrAffine2* out) {
    const PrAffine2& m = transform;
    float determinant = m.a * m.d - m.b * m.c;
    if (determinant == 0 || !prF32IsFinite(1 / determinant))
        return false;
    float inverse = 1 / determinant;
    *out = PrAffine2{
        m.d * inverse,
        -m.b * inverse,
        -m.c * inverse,
        m.a * inverse,
        (m.c * m.f - m.d * m.e) * inverse,
        (m.b * m.e - m.a * m.f) * inverse,
    };
    return true;
}

PrVec2 prAffine2MapPoint(PrAffine2 transform, PrVec2 point) {
    const PrAffine2& m = transform;
    return PrVec2{m.a * point.x + m.c * point.y + m.e, m.b * point.x + m.d * point.y + m.f};
}

PrRect prAffine2MapRect(PrAffine2 transform, PrRect rect) {
    if (prRectIsEmpty(rect))
        return PrRect{0, 0, 0, 0};
    PrVec2 corners[4] = {
        prAffine2MapPoint(transform, PrVec2{rect.left, rect.top}),
        prAffine2MapPoint(transform, PrVec2{rect.right, rect.top}),
        prAffine2MapPoint(transform, PrVec2{rect.right, rect.bottom}),
        prAffine2MapPoint(transform, PrVec2{rect.left, rect.bottom}),
    };
    PrRect bounds{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const PrVec2& corner : corners) {
        bounds.left = prF32Min(bounds.left, corner.x);
        bounds.top = prF32Min(bounds.top, corner.y);
        bounds.right = prF32Max(bounds.right, corner.x);
        bounds.bottom = prF32Max(bounds.bottom, corner.y);
    }
    return bounds;
}

float prAffine2MaxScale(PrAffine2 transform) {
    const PrAffine2& t = transform;
    float x = prF32Sqrt(t.a * t.a + t.b * t.b);
    float y = prF32Sqrt(t.c * t.c + t.d * t.d);
    return prF32Max(x, y);
}

bool prAffine2IsIdentity(PrAffine2 transform) {
    return transform.a == 1 && transform.b == 0 && transform.c == 0 && transform.d == 1 && transform.e == 0 &&
        transform.f == 0;
}

bool prAffine2IsSimilarity(PrAffine2 transform) {
    const PrAffine2& t = transform;
    float x = t.a * t.a + t.b * t.b;
    float y = t.c * t.c + t.d * t.d;
    float tolerance = kSimilarityTolerance * prF32Max(x, y);
    return prF32Abs(x - y) <= tolerance && prF32Abs(t.a * t.c + t.b * t.d) <= tolerance;
}

bool prAffine2IsAxisAligned(PrAffine2 transform) {
    return (transform.b == 0 && transform.c == 0) || (transform.a == 0 && transform.d == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Affine transforms of the plane in doubles

PrDAffine2 prDAffine2Identity(void) {
    return PrDAffine2{1, 0, 0, 1, 0, 0};
}

PrDAffine2 prDAffine2Translate(double x, double y) {
    return PrDAffine2{1, 0, 0, 1, x, y};
}

PrDAffine2 prDAffine2FromAffine2(PrAffine2 transform) {
    return PrDAffine2{
        double(transform.a), double(transform.b), double(transform.c),
        double(transform.d), double(transform.e), double(transform.f),
    };
}

PrAffine2 prAffine2FromDAffine2(PrDAffine2 transform) {
    return PrAffine2{
        float(transform.a), float(transform.b), float(transform.c),
        float(transform.d), float(transform.e), float(transform.f),
    };
}

PrDAffine2 prDAffine2Concat(PrDAffine2 first, PrDAffine2 second) {
    const PrDAffine2& m = second;
    const PrDAffine2& n = first;
    return PrDAffine2{
        m.a * n.a + m.c * n.b,       m.b * n.a + m.d * n.b,
        m.a * n.c + m.c * n.d,       m.b * n.c + m.d * n.d,
        m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f,
    };
}

bool prDAffine2Invert(PrDAffine2 transform, PrDAffine2* out) {
    const PrDAffine2& m = transform;
    double determinant = m.a * m.d - m.b * m.c;
    if (determinant == 0 || !prF64IsFinite(1 / determinant))
        return false;
    double inverse = 1 / determinant;
    *out = PrDAffine2{
        m.d * inverse,
        -m.b * inverse,
        -m.c * inverse,
        m.a * inverse,
        (m.c * m.f - m.d * m.e) * inverse,
        (m.b * m.e - m.a * m.f) * inverse,
    };
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Transforms in three dimensions

PrMatrix4 prMatrix4Identity(void) {
    PrMatrix4 m{};
    for (uint32_t i = 0; i < 4; ++i)
        m.m[i][i] = 1;
    return m;
}

PrMatrix4 prMatrix4FromAffine2(PrAffine2 transform) {
    PrMatrix4 m = prMatrix4Identity();
    m.m[0][0] = transform.a;
    m.m[1][0] = transform.b;
    m.m[0][1] = transform.c;
    m.m[1][1] = transform.d;
    m.m[0][3] = transform.e;
    m.m[1][3] = transform.f;
    return m;
}

PrMatrix4 prMatrix4Translate(PrVec3 offset) {
    PrMatrix4 m = prMatrix4Identity();
    m.m[0][3] = offset.x;
    m.m[1][3] = offset.y;
    m.m[2][3] = offset.z;
    return m;
}

PrMatrix4 prMatrix4Scale(PrVec3 factors) {
    PrMatrix4 m = prMatrix4Identity();
    m.m[0][0] = factors.x;
    m.m[1][1] = factors.y;
    m.m[2][2] = factors.z;
    return m;
}

PrMatrix4 prMatrix4Rotate(PrVec3 axis, float radians) {
    float x = axis.x;
    float y = axis.y;
    float z = axis.z;
    float length = prF32Sqrt(x * x + y * y + z * z);
    if (!(length > 0))
        return prMatrix4Identity();
    x /= length;
    y /= length;
    z /= length;
    float s = prF32Sin(radians);
    float c = prF32Cos(radians);
    float k = 1 - c;
    PrMatrix4 m = prMatrix4Identity();
    m.m[0][0] = c + x * x * k;
    m.m[0][1] = x * y * k - z * s;
    m.m[0][2] = x * z * k + y * s;
    m.m[1][0] = y * x * k + z * s;
    m.m[1][1] = c + y * y * k;
    m.m[1][2] = y * z * k - x * s;
    m.m[2][0] = z * x * k - y * s;
    m.m[2][1] = z * y * k + x * s;
    m.m[2][2] = c + z * z * k;
    return m;
}

PrMatrix4 prMatrix4Perspective(float distance) {
    PrMatrix4 m = prMatrix4Identity();
    if (distance > 0)
        m.m[3][2] = -1 / distance;
    return m;
}

PrMatrix4 prMatrix4Concat(const PrMatrix4* first, const PrMatrix4* second) {
    PrMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            float sum = 0;
            for (uint32_t k = 0; k < 4; ++k)
                sum += second->m[row][k] * first->m[k][column];
            out.m[row][column] = sum;
        }
    }
    return out;
}

bool prMatrix4Invert(const PrMatrix4* matrix, PrMatrix4* out) {
    PrDMatrix4 doubled = prDMatrix4FromMatrix4(matrix);
    PrDMatrix4 inverse{};
    if (!prDMatrix4Invert(&doubled, &inverse))
        return false;
    *out = prMatrix4FromDMatrix4(&inverse);
    return true;
}

bool prMatrix4MapPoint(const PrMatrix4* matrix, PrVec2 point, PrVec2* out) {
    const float (*m)[4] = matrix->m;
    float x = m[0][0] * point.x + m[0][1] * point.y + m[0][3];
    float y = m[1][0] * point.x + m[1][1] * point.y + m[1][3];
    float d = m[3][0] * point.x + m[3][1] * point.y + m[3][3];
    if (!(d > 0))
        return false;
    *out = PrVec2{x / d, y / d};
    return true;
}

bool prAffine2FromMatrix4(const PrMatrix4* matrix, PrAffine2* out) {
    const float (*m)[4] = matrix->m;
    if (m[3][0] != 0 || m[3][1] != 0 || m[3][3] != 1)
        return false;
    *out = PrAffine2{m[0][0], m[1][0], m[0][1], m[1][1], m[0][3], m[1][3]};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Transforms in three dimensions in doubles

PrDMatrix4 prDMatrix4Identity(void) {
    return PrDMatrix4{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

PrDMatrix4 prDMatrix4FromMatrix4(const PrMatrix4* matrix) {
    PrDMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            out.m[row][column] = double(matrix->m[row][column]);
    }
    return out;
}

PrMatrix4 prMatrix4FromDMatrix4(const PrDMatrix4* matrix) {
    PrMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            out.m[row][column] = float(matrix->m[row][column]);
    }
    return out;
}

PrDMatrix4 prDMatrix4Concat(const PrDMatrix4* first, const PrDMatrix4* second) {
    PrDMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            double sum = 0;
            for (uint32_t k = 0; k < 4; ++k)
                sum += second->m[row][k] * first->m[k][column];
            out.m[row][column] = sum;
        }
    }
    return out;
}

bool prDMatrix4Invert(const PrDMatrix4* matrix, PrDMatrix4* out) {
    // Gauss-Jordan elimination with partial pivoting.
    double a[4][8];
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            a[row][column] = matrix->m[row][column];
            a[row][column + 4] = row == column ? 1 : 0;
        }
    }
    for (uint32_t column = 0; column < 4; ++column) {
        uint32_t pivot = column;
        for (uint32_t row = column + 1; row < 4; ++row) {
            if (prF64Abs(a[row][column]) > prF64Abs(a[pivot][column]))
                pivot = row;
        }
        if (!(prF64Abs(a[pivot][column]) > 1e-12))
            return false;
        for (uint32_t k = 0; k < 8; ++k) {
            double swap = a[column][k];
            a[column][k] = a[pivot][k];
            a[pivot][k] = swap;
        }
        double scale = 1 / a[column][column];
        for (uint32_t k = 0; k < 8; ++k)
            a[column][k] *= scale;
        for (uint32_t row = 0; row < 4; ++row) {
            if (row == column)
                continue;
            double factor = a[row][column];
            for (uint32_t k = 0; k < 8; ++k)
                a[row][k] -= factor * a[column][k];
        }
    }
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            out->m[row][column] = a[row][column + 4];
    }
    return true;
}

bool prDMatrix4ShowsBack(const PrDMatrix4* matrix) {
    // The mapped normal's z is the inverse's entry at row 2 and column 2: the cofactor there over the determinant,
    // whose sign is that of their product.
    const double (*m)[4] = matrix->m;
    auto minor = [&](uint32_t skipRow, uint32_t skipColumn) {
        uint32_t r[3];
        uint32_t c[3];
        for (uint32_t i = 0, k = 0; i < 4; ++i) {
            if (i != skipRow)
                r[k++] = i;
        }
        for (uint32_t i = 0, k = 0; i < 4; ++i) {
            if (i != skipColumn)
                c[k++] = i;
        }
        return m[r[0]][c[0]] * (m[r[1]][c[1]] * m[r[2]][c[2]] - m[r[1]][c[2]] * m[r[2]][c[1]]) -
            m[r[0]][c[1]] * (m[r[1]][c[0]] * m[r[2]][c[2]] - m[r[1]][c[2]] * m[r[2]][c[0]]) +
            m[r[0]][c[2]] * (m[r[1]][c[0]] * m[r[2]][c[1]] - m[r[1]][c[1]] * m[r[2]][c[0]]);
    };
    double determinant = 0;
    for (uint32_t column = 0; column < 4; ++column) {
        double term = m[0][column] * minor(0, column);
        determinant += (column & 1) != 0 ? -term : term;
    }
    return determinant * minor(2, 2) < 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Projective transforms of the plane

PrMatrix3 prMatrix3FromAffine2(PrAffine2 transform) {
    return PrMatrix3{{{transform.a, transform.c, transform.e}, {transform.b, transform.d, transform.f}, {0, 0, 1}}};
}

PrMatrix3 prMatrix3FromMatrix4(const PrMatrix4* matrix) {
    const float (*m)[4] = matrix->m;
    return PrMatrix3{{{m[0][0], m[0][1], m[0][3]}, {m[1][0], m[1][1], m[1][3]}, {m[3][0], m[3][1], m[3][3]}}};
}

bool prMatrix3IsAffine(const PrMatrix3* matrix) {
    return matrix->m[2][0] == 0 && matrix->m[2][1] == 0 && matrix->m[2][2] > 0;
}

bool prAffine2FromMatrix3(const PrMatrix3* matrix, PrAffine2* out) {
    if (!prMatrix3IsAffine(matrix))
        return false;
    const float (*m)[3] = matrix->m;
    if (m[2][2] == 1) {
        *out = PrAffine2{m[0][0], m[1][0], m[0][1], m[1][1], m[0][2], m[1][2]};
        return true;
    }
    float w = 1 / m[2][2];
    *out = PrAffine2{m[0][0] * w, m[1][0] * w, m[0][1] * w, m[1][1] * w, m[0][2] * w, m[1][2] * w};
    return true;
}

PrAffine2 prMatrix3AffineNear(const PrMatrix3* matrix, PrVec2 point) {
    const PrDMatrix3 h = prDMatrix3FromMatrix3(matrix);
    const double x = double(point.x);
    const double y = double(point.y);
    const double w = h.m[2][0] * x + h.m[2][1] * y + h.m[2][2];
    if (!(w > 0))
        return PrAffine2{};
    // The derivative of the projection is (A - q g^T) / w, where A is the linear part, g the last row's linear part and
    // q the mapped point.
    const double qx = (h.m[0][0] * x + h.m[0][1] * y + h.m[0][2]) / w;
    const double qy = (h.m[1][0] * x + h.m[1][1] * y + h.m[1][2]) / w;
    const double a = (h.m[0][0] - qx * h.m[2][0]) / w;
    const double c = (h.m[0][1] - qx * h.m[2][1]) / w;
    const double b = (h.m[1][0] - qy * h.m[2][0]) / w;
    const double d = (h.m[1][1] - qy * h.m[2][1]) / w;
    return PrAffine2{float(a), float(b), float(c), float(d), float(qx - a * x - c * y), float(qy - b * x - d * y)};
}

float prMatrix3MaxScale(const PrMatrix3* matrix, PrRect rect, float minimumW) {
    PR_ASSERT(minimumW > 0);
    const PrDMatrix3 h = prDMatrix3FromMatrix3(matrix);
    const double nearW = double(minimumW);

    // The part of `rect` where w is at least `minimumW`: a convex polygon of at most five corners.
    const double corners[4][2] = {
        {double(rect.left), double(rect.top)},
        {double(rect.right), double(rect.top)},
        {double(rect.right), double(rect.bottom)},
        {double(rect.left), double(rect.bottom)},
    };
    double visible[5][2];
    uint32_t count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const double* a = corners[i];
        const double* b = corners[(i + 1) % 4];
        const double wa = h.m[2][0] * a[0] + h.m[2][1] * a[1] + h.m[2][2];
        const double wb = h.m[2][0] * b[0] + h.m[2][1] * b[1] + h.m[2][2];
        const bool aIn = wa >= nearW;
        if (aIn) {
            visible[count][0] = a[0];
            visible[count][1] = a[1];
            ++count;
        }
        if (aIn != (wb >= nearW)) {
            const double t = (nearW - wa) / (wb - wa);
            visible[count][0] = a[0] + (b[0] - a[0]) * t;
            visible[count][1] = a[1] + (b[1] - a[1]) * t;
            ++count;
        }
    }
    if (count == 0)
        return 0;

    // The derivative at a point that maps to q is (A - q g^T) / w, where A is the linear part and g the last row's
    // linear part. Over the polygon's image, 1 / w is affine in q and |q - q0| is convex, so each is largest at a
    // corner. With q0 the corners' centroid, the spectral norm is at most max(1 / w) (|A - q0 g^T| + max |q - q0| |g|).
    double inverseW = 0;
    double qx[5];
    double qy[5];
    double centerX = 0;
    double centerY = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const double x = visible[i][0];
        const double y = visible[i][1];
        const double w = h.m[2][0] * x + h.m[2][1] * y + h.m[2][2];
        inverseW = prF64Max(inverseW, 1 / w);
        qx[i] = (h.m[0][0] * x + h.m[0][1] * y + h.m[0][2]) / w;
        qy[i] = (h.m[1][0] * x + h.m[1][1] * y + h.m[1][2]) / w;
        centerX += qx[i] / count;
        centerY += qy[i] / count;
    }
    double reach = 0;
    for (uint32_t i = 0; i < count; ++i) {
        double dx = qx[i] - centerX;
        double dy = qy[i] - centerY;
        reach = prF64Max(reach, prF64Sqrt(dx * dx + dy * dy));
    }
    const double a = h.m[0][0] - centerX * h.m[2][0];
    const double c = h.m[0][1] - centerX * h.m[2][1];
    const double b = h.m[1][0] - centerY * h.m[2][0];
    const double d = h.m[1][1] - centerY * h.m[2][1];
    const double sum = a * a + b * b + c * c + d * d;
    const double determinant = a * d - b * c;
    const double norm = prF64Sqrt((sum + prF64Sqrt(prF64Max(sum * sum - 4 * determinant * determinant, 0))) / 2);
    const double row = prF64Sqrt(h.m[2][0] * h.m[2][0] + h.m[2][1] * h.m[2][1]);
    return float(inverseW * (norm + reach * row));
}

PrMatrix3 prMatrix3Concat(const PrMatrix3* first, const PrMatrix3* second) {
    PrMatrix3 out{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            float sum = 0;
            for (uint32_t k = 0; k < 3; ++k)
                sum += second->m[row][k] * first->m[k][column];
            out.m[row][column] = sum;
        }
    }
    return out;
}

bool prMatrix3MapPoint(const PrMatrix3* matrix, PrVec2 point, PrVec2* out) {
    const float (*m)[3] = matrix->m;
    float d = m[2][0] * point.x + m[2][1] * point.y + m[2][2];
    if (!(d > 0))
        return false;
    *out = PrVec2{
        (m[0][0] * point.x + m[0][1] * point.y + m[0][2]) / d,
        (m[1][0] * point.x + m[1][1] * point.y + m[1][2]) / d,
    };
    return true;
}

bool prMatrix3Invert(const PrMatrix3* matrix, PrMatrix3* out) {
    PrDMatrix3 doubled = prDMatrix3FromMatrix3(matrix);
    PrDMatrix3 inverse{};
    if (!prDMatrix3Invert(&doubled, &inverse))
        return false;
    *out = prMatrix3FromDMatrix3(&inverse);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Projective transforms of the plane in doubles

PrDMatrix3 prDMatrix3Identity(void) {
    return PrDMatrix3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

PrDMatrix3 prDMatrix3FromDAffine2(PrDAffine2 transform) {
    return PrDMatrix3{{{transform.a, transform.c, transform.e}, {transform.b, transform.d, transform.f}, {0, 0, 1}}};
}

PrDMatrix3 prDMatrix3FromMatrix3(const PrMatrix3* matrix) {
    PrDMatrix3 out{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column)
            out.m[row][column] = double(matrix->m[row][column]);
    }
    return out;
}

PrMatrix3 prMatrix3FromDMatrix3(const PrDMatrix3* matrix) {
    PrMatrix3 out{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column)
            out.m[row][column] = float(matrix->m[row][column]);
    }
    return out;
}

PrDMatrix3 prDMatrix3FromDMatrix4(const PrDMatrix4* matrix) {
    const double (*m)[4] = matrix->m;
    return PrDMatrix3{{{m[0][0], m[0][1], m[0][3]}, {m[1][0], m[1][1], m[1][3]}, {m[3][0], m[3][1], m[3][3]}}};
}

bool prDAffine2FromDMatrix3(const PrDMatrix3* matrix, PrDAffine2* out) {
    const double (*m)[3] = matrix->m;
    const double w = m[2][2];
    if (m[2][0] != 0 || m[2][1] != 0 || !(w > 0))
        return false;
    if (w == 1) {
        *out = PrDAffine2{m[0][0], m[1][0], m[0][1], m[1][1], m[0][2], m[1][2]};
        return true;
    }
    *out = PrDAffine2{m[0][0] / w, m[1][0] / w, m[0][1] / w, m[1][1] / w, m[0][2] / w, m[1][2] / w};
    return true;
}

PrDMatrix3 prDMatrix3Concat(const PrDMatrix3* first, const PrDMatrix3* second) {
    PrDMatrix3 out{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            double sum = 0;
            for (uint32_t k = 0; k < 3; ++k)
                sum += second->m[row][k] * first->m[k][column];
            out.m[row][column] = sum;
        }
    }
    return out;
}

bool prDMatrix3MapPoint(const PrDMatrix3* matrix, PrDVec2 point, PrDVec2* out) {
    const double (*m)[3] = matrix->m;
    const double d = m[2][0] * point.x + m[2][1] * point.y + m[2][2];
    if (!(d > 0))
        return false;
    *out = PrDVec2{
        (m[0][0] * point.x + m[0][1] * point.y + m[0][2]) / d,
        (m[1][0] * point.x + m[1][1] * point.y + m[1][2]) / d,
    };
    return true;
}

bool prDMatrix3Invert(const PrDMatrix3* matrix, PrDMatrix3* out) {
    double m[9];
    for (uint32_t i = 0; i < 9; ++i)
        m[i] = matrix->m[i / 3][i % 3];
    const double c[9] = {
        m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
        m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
        m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3],
    };
    const double determinant = m[0] * c[0] + m[1] * c[3] + m[2] * c[6];
    if (!(determinant != 0) || !prF64IsFinite(determinant))
        return false;
    for (uint32_t i = 0; i < 9; ++i)
        out->m[i / 3][i % 3] = c[i] / determinant;
    return true;
}
