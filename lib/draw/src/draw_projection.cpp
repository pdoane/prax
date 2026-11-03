// Projected groups: where a group's layer lands on the target.

#include "pr/draw_projection.h"

#include "pr/float.h"

namespace pr_impl {

namespace {

// The minimum ratio of the smallest to the largest factor by which the projection scales a length. Below it, the plane
// is edge-on and draws nothing.
constexpr float kCollapse = 1e-6f;

// The most layer pixels per target pixel that a plane's visible part covers, if its projection is not affine. A steep
// plane's far part is foreshortened, so a layer sampled at the scale of its nearest part would spend most of its pixels
// on a sliver of the target.
constexpr float kMaxLayerAreaRatio = 2;

// The most corners of the bounds once they are cut behind the eye and then to the visible box.
constexpr uint32_t kMaxCorners = 16;

// A convex polygon in homogeneous target coordinates.
struct Corners {
    float x[kMaxCorners];
    float y[kMaxCorners];
    float w[kMaxCorners];
    uint32_t count;
};

bool isFiniteMatrix(const PrMatrix4& matrix) {
    for (uint32_t i = 0; i < 16; ++i) {
        if (!prF32IsFinite(matrix.m[i / 4][i % 4]))
            return false;
    }
    return true;
}

// Returns the 4x4 matrix that applies `plane` to the z = 0 plane, for prPathPolylineProject.
PrMatrix4 liftPlane(const PrMatrix3& plane) {
    PrMatrix4 m{};
    const uint32_t index[3] = {0, 1, 3};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column)
            m.m[index[row]][index[column]] = plane.m[row][column];
    }
    m.m[2][2] = 1;
    return m;
}

// Keeps the part of the polygon where a x + b y + c w + d is at least zero.
void cutBy(Corners& p, float a, float b, float c, float d) {
    Corners in = p;
    p.count = 0;
    for (uint32_t i = 0; i < in.count; ++i) {
        uint32_t j = (i + 1) % in.count;
        float di = a * in.x[i] + b * in.y[i] + c * in.w[i] + d;
        float dj = a * in.x[j] + b * in.y[j] + c * in.w[j] + d;
        if (di >= 0 && p.count < kMaxCorners) {
            p.x[p.count] = in.x[i];
            p.y[p.count] = in.y[i];
            p.w[p.count] = in.w[i];
            ++p.count;
        }
        if ((di >= 0) != (dj >= 0) && p.count < kMaxCorners) {
            float t = di / (di - dj);
            p.x[p.count] = in.x[i] + (in.x[j] - in.x[i]) * t;
            p.y[p.count] = in.y[i] + (in.y[j] - in.y[i]) * t;
            p.w[p.count] = in.w[i] + (in.w[j] - in.w[i]) * t;
            ++p.count;
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prDrawProjectionPlace(const PrMatrix4* projection, PrAffine2 toTarget, PrRect bounds, PrRect visible,
        float maxLayerSide, PrDrawProjection* out) {
    if (!isFiniteMatrix(*projection) || prRectIsEmpty(bounds) || prRectIsEmpty(visible) || !(maxLayerSide >= 1))
        return false;
    const PrMatrix3 plane = prMatrix3FromMatrix4(projection);
    const PrMatrix3 target = prMatrix3FromAffine2(toTarget);
    const PrMatrix3 h = prMatrix3Concat(&plane, &target);
    PrMatrix3 back{};
    if (!prMatrix3Invert(&h, &back))
        return false;

    // The bounds in homogeneous target coordinates, cut behind the eye and then to the visible box. Homogeneous
    // coordinates keep each cut linear.
    Corners p{};
    const PrVec2 corners[4] = {
        {bounds.left, bounds.top},
        {bounds.right, bounds.top},
        {bounds.right, bounds.bottom},
        {bounds.left, bounds.bottom},
    };
    float largest = 0;
    for (PrVec2 c : corners) {
        p.x[p.count] = h.m[0][0] * c.x + h.m[0][1] * c.y + h.m[0][2];
        p.y[p.count] = h.m[1][0] * c.x + h.m[1][1] * c.y + h.m[1][2];
        p.w[p.count] = h.m[2][0] * c.x + h.m[2][1] * c.y + h.m[2][2];
        largest = prF32Max(largest, p.w[p.count]);
        ++p.count;
    }
    if (!(largest > 0))
        return false;
    cutBy(p, 0, 0, 1, -largest * kPrPathPolylineNearPlane);
    cutBy(p, 1, 0, -visible.left, 0);
    cutBy(p, -1, 0, visible.right, 0);
    cutBy(p, 0, 1, -visible.top, 0);
    cutBy(p, 0, -1, visible.bottom, 0);
    if (p.count < 3)
        return false;

    // The visible corners in target pixels, the area they cover, and the part of the plane they come from.
    PrRect landed{kPrF32Max, kPrF32Max, -kPrF32Max, -kPrF32Max};
    PrRect from{kPrF32Max, kPrF32Max, -kPrF32Max, -kPrF32Max};
    PrVec2 center{0, 0};
    float nearest = kPrF32Max;
    PrVec2 q[kMaxCorners];
    for (uint32_t i = 0; i < p.count; ++i) {
        q[i] = PrVec2{p.x[i] / p.w[i], p.y[i] / p.w[i]};
        landed = PrRect{
            prF32Min(landed.left, q[i].x), prF32Min(landed.top, q[i].y), prF32Max(landed.right, q[i].x),
            prF32Max(landed.bottom, q[i].y),
        };
        PrVec2 u{};
        if (!prMatrix3MapPoint(&back, q[i], &u))
            return false;
        from.left = prF32Min(from.left, u.x);
        from.top = prF32Min(from.top, u.y);
        from.right = prF32Max(from.right, u.x);
        from.bottom = prF32Max(from.bottom, u.y);
        center = center + u / float(p.count);
        nearest = prF32Min(nearest, p.w[i]);
    }
    float targetArea = 0;
    for (uint32_t i = 0; i < p.count; ++i) {
        const PrVec2& a = q[i];
        const PrVec2& b = q[(i + 1) % p.count];
        targetArea += a.x * b.y - b.x * a.y;
    }
    targetArea = prF32Abs(targetArea) / 2;
    PrRect area = prRectIntersect(bounds, from);
    if (prRectIsEmpty(area))
        return false;

    // An edge-on plane scales one direction by a vanishing fraction of the other.
    PrAffine2 tangent = prMatrix3AffineNear(&h, center);
    float squares = tangent.a * tangent.a + tangent.b * tangent.b + tangent.c * tangent.c + tangent.d * tangent.d;
    float determinant = prF32Abs(tangent.a * tangent.d - tangent.b * tangent.c);
    if (!(squares > 0) || determinant <= kCollapse * squares)
        return false;

    // An affine plane is drawn at its exact scale, as any group is. Any other plane is drawn at its largest scale over
    // the visible part, but no finer than the enclosing transform's own scale, and its layer is bounded by the target
    // area that it covers.
    float scale = 0;
    PrAffine2 affine{};
    if (prAffine2FromMatrix3(&h, &affine)) {
        scale = prAffine2MaxScale(affine);
    } else {
        scale = prF32Min(prMatrix3MaxScale(&h, area, nearest), prAffine2MaxScale(toTarget));
        float planeArea = prRectWidth(area) * prRectHeight(area);
        scale = prF32Min(scale, prF32Sqrt(kMaxLayerAreaRatio * targetArea / planeArea));
    }
    float side = prF32Max(prRectWidth(area), prRectHeight(area));
    scale = prF32Min(scale, maxLayerSide / side);
    if (!(scale > 0) || !prF32IsFinite(scale))
        return false;

    const PrMatrix3 unscale{{{1 / scale, 0, 0}, {0, 1 / scale, 0}, {0, 0, 1}}};
    const PrMatrix3 rescale{{{scale, 0, 0}, {0, scale, 0}, {0, 0, 1}}};
    *out = PrDrawProjection{
        .layerToTarget = prMatrix3Concat(&unscale, &h),
        .targetToLayer = prMatrix3Concat(&back, &rescale),
        .area = area,
        .scale = scale,
        .targetBounds = landed,
    };
    return true;
}

void prDrawProjectionQuad(const PrDrawProjection* projection, PrRect rect, PrPathPolyline* out) {
    PrPathPolyline quad{};
    prPathPolylineMoveTo(&quad, PrVec2{rect.left, rect.top});
    prPathPolylineLineTo(&quad, PrVec2{rect.right, rect.top});
    prPathPolylineLineTo(&quad, PrVec2{rect.right, rect.bottom});
    prPathPolylineLineTo(&quad, PrVec2{rect.left, rect.bottom});
    prPathPolylineClose(&quad);
    PrMatrix4 lifted = liftPlane(projection->layerToTarget);
    prPathPolylineProject(&quad, &lifted, out);
    prPathPolylineReset(&quad);
}

PrAffine2 prDrawProjectionTangent(const PrDrawProjection* projection) {
    const float scale = projection->scale;
    const PrMatrix3 rescale{{{scale, 0, 0}, {0, scale, 0}, {0, 0, 1}}};
    const PrMatrix3 planeToTarget = prMatrix3Concat(&rescale, &projection->layerToTarget);
    const PrRect& area = projection->area;
    return prMatrix3AffineNear(&planeToTarget, PrVec2{(area.left + area.right) / 2, (area.top + area.bottom) / 2});
}

PrRect prDrawProjectionBounds(const PrMatrix4* projection, PrRect bounds) {
    if (!isFiniteMatrix(*projection) || prRectIsEmpty(bounds))
        return PrRect{0, 0, 0, 0};
    PrPathPolyline quad{};
    PrPathPolyline landed{};
    prPathPolylineMoveTo(&quad, PrVec2{bounds.left, bounds.top});
    prPathPolylineLineTo(&quad, PrVec2{bounds.right, bounds.top});
    prPathPolylineLineTo(&quad, PrVec2{bounds.right, bounds.bottom});
    prPathPolylineLineTo(&quad, PrVec2{bounds.left, bounds.bottom});
    prPathPolylineClose(&quad);
    prPathPolylineProject(&quad, projection, &landed);
    PrRect result = prPathPolylineBounds(&landed);
    prPathPolylineReset(&quad);
    prPathPolylineReset(&landed);
    return result;
}
