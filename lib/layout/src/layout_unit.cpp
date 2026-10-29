#include "layout/src/layout_unit.h"

#include "pr/path_polyline.h"

namespace pr_impl {

PrLayoutUnit layoutRectWidth(PrLayoutRect rect) {
    return layoutUnitMax(kLayoutUnitZero, rect.right - rect.left);
}

PrLayoutUnit layoutRectHeight(PrLayoutRect rect) {
    return layoutUnitMax(kLayoutUnitZero, rect.bottom - rect.top);
}

bool layoutRectContains(PrLayoutRect rect, PrLayoutVec point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

PrLayoutRect layoutRectEnclosing(PrRect px) {
    return PrLayoutRect{prLayoutUnitFromPxFloor(px.left), prLayoutUnitFromPxFloor(px.top),
                        prLayoutUnitFromPxCeil(px.right), prLayoutUnitFromPxCeil(px.bottom)};
}

PrDAffine2 layoutTranslation(PrLayoutVec offset) {
    return prDAffine2Translate(prLayoutUnitToPxDouble(offset.x), prLayoutUnitToPxDouble(offset.y));
}

PrDVec2 layoutOffset(PrLayoutVec offset) {
    return PrDVec2{prLayoutUnitToPxDouble(offset.x), prLayoutUnitToPxDouble(offset.y)};
}

PrLayoutVec layoutVecMapped(PrDAffine2 transform, PrLayoutVec point) {
    double x = prLayoutUnitToPxDouble(point.x);
    double y = prLayoutUnitToPxDouble(point.y);
    return PrLayoutVec{prLayoutUnitFromPxRoundDouble(transform.a * x + transform.c * y + transform.e),
                       prLayoutUnitFromPxRoundDouble(transform.b * x + transform.d * y + transform.f)};
}

PrLayoutRect layoutRectMapped(PrDAffine2 transform, PrLayoutRect rect) {
    double xs[2] = {prLayoutUnitToPxDouble(rect.left), prLayoutUnitToPxDouble(rect.right)};
    double ys[2] = {prLayoutUnitToPxDouble(rect.top), prLayoutUnitToPxDouble(rect.bottom)};
    double left = kF64Max;
    double top = kF64Max;
    double right = -kF64Max;
    double bottom = -kF64Max;
    for (double x : xs) {
        for (double y : ys) {
            double mx = transform.a * x + transform.c * y + transform.e;
            double my = transform.b * x + transform.d * y + transform.f;
            left = minF64(left, mx);
            top = minF64(top, my);
            right = maxF64(right, mx);
            bottom = maxF64(bottom, my);
        }
    }
    return PrLayoutRect{prLayoutUnitFromPxFloorDouble(left), prLayoutUnitFromPxFloorDouble(top),
                        prLayoutUnitFromPxCeilDouble(right), prLayoutUnitFromPxCeilDouble(bottom)};
}

bool layoutVecProjected(const PrDMatrix3& transform, PrLayoutVec point, PrLayoutVec* out) {
    PrDVec2 mapped{};
    if (!prDMatrix3MapPoint(&transform, PrDVec2{prLayoutUnitToPxDouble(point.x), prLayoutUnitToPxDouble(point.y)},
                            &mapped))
        return false;
    *out = PrLayoutVec{prLayoutUnitFromPxRoundDouble(mapped.x), prLayoutUnitFromPxRoundDouble(mapped.y)};
    return true;
}

PrLayoutRect layoutRectProjected(const PrDMatrix3& transform, PrLayoutRect rect) {
    // The corners in order around the rectangle, in homogeneous coordinates. w is affine over the plane, so its largest
    // value over the rectangle is at a corner, and the cut is a line that crosses each edge at most once.
    const double (*m)[3] = transform.m;
    double xs[4] = {prLayoutUnitToPxDouble(rect.left), prLayoutUnitToPxDouble(rect.right),
                    prLayoutUnitToPxDouble(rect.right), prLayoutUnitToPxDouble(rect.left)};
    double ys[4] = {prLayoutUnitToPxDouble(rect.top), prLayoutUnitToPxDouble(rect.top),
                    prLayoutUnitToPxDouble(rect.bottom), prLayoutUnitToPxDouble(rect.bottom)};
    double corners[4][3];
    double largest = -kF64Max;
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t row = 0; row < 3; ++row)
            corners[i][row] = m[row][0] * xs[i] + m[row][1] * ys[i] + m[row][2];
        largest = maxF64(largest, corners[i][2]);
    }
    if (!(largest > 0))
        return PrLayoutRect{};
    double near = double(kPrPathPolylineNearPlane) * largest;
    double left = kF64Max;
    double top = kF64Max;
    double right = -kF64Max;
    double bottom = -kF64Max;
    auto add = [&](double x, double y, double w) {
        left = minF64(left, x / w);
        top = minF64(top, y / w);
        right = maxF64(right, x / w);
        bottom = maxF64(bottom, y / w);
    };
    for (uint32_t i = 0; i < 4; ++i) {
        const double* a = corners[i];
        const double* b = corners[(i + 1) % 4];
        if (a[2] >= near)
            add(a[0], a[1], a[2]);
        if ((a[2] >= near) != (b[2] >= near)) {
            double t = (near - a[2]) / (b[2] - a[2]);
            add(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, near);
        }
    }
    return PrLayoutRect{prLayoutUnitFromPxFloorDouble(left), prLayoutUnitFromPxFloorDouble(top),
                        prLayoutUnitFromPxCeilDouble(right), prLayoutUnitFromPxCeilDouble(bottom)};
}

} // namespace pr_impl
