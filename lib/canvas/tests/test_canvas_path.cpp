// The paths of CanvasPath: their subpaths, arcs and rounded rectangles, as Path2D builds them.

#include "pr/canvas.h"

#include "doctest/doctest.h"

#include <cmath>

namespace pr_impl {

namespace {

// Returns true if `point` is inside `path` filled with the nonzero rule, tested by a context with the identity
// transform.
bool inside(const PrCanvasPath2D* path, double x, double y) {
    PrCanvasDesc desc{.extent = PrUVec2{1, 1}, .settings = prCanvasSettingsDefault()};
    PrCanvas* canvas = prCanvasCreate(&desc);
    bool result = prCanvasIsPointInPath(canvas, path, x, y, PrPathFillRule_NonZero);
    prCanvasDestroy(canvas);
    return result;
}

constexpr double kPi = 3.14159265358979323846;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an arc of a whole turn or more is a circle, and less than a turn goes from the start to the end point") {
    PrCanvasPath2D* circle = prCanvasPath2DCreate();
    CHECK(prCanvasPath2DArc(circle, 50, 50, 10, 0, 2 * kPi, false) == PrStatus_Ok);
    CHECK(inside(circle, 50, 41));
    CHECK(inside(circle, 41, 50));
    CHECK(!inside(circle, 50, 39));
    prCanvasPath2DDestroy(circle);

    // A clockwise arc from 0 to π covers the lower half, where y points down.
    PrCanvasPath2D* half = prCanvasPath2DCreate();
    prCanvasPath2DArc(half, 50, 50, 10, 0, kPi, false);
    CHECK(inside(half, 50, 55));
    CHECK(!inside(half, 50, 45));
    prCanvasPath2DDestroy(half);

    // Counterclockwise from 0 to 2π draws the whole circle.
    PrCanvasPath2D* back = prCanvasPath2DCreate();
    prCanvasPath2DArc(back, 50, 50, 10, 0, 2 * kPi, true);
    CHECK(inside(back, 50, 45));
    CHECK(inside(back, 50, 55));
    prCanvasPath2DDestroy(back);
}

TEST_CASE("a negative radius is out of range and adds nothing, and a non-finite argument adds nothing") {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    CHECK(prCanvasPath2DArc(path, 50, 50, -1, 0, kPi, false) == PrStatus_OutOfRange);
    CHECK(prCanvasPath2DArc(path, std::nan(""), 50, -1, 0, kPi, false) == PrStatus_Ok);
    CHECK(prCanvasPath2DArcTo(path, 0, 0, 10, 10, -1) == PrStatus_OutOfRange);
    prCanvasPath2DDestroy(path);
}

TEST_CASE("roundRect takes one to four radii, as border-radius does, and flips with a negative extent") {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    PrDVec2 radii[5] = {{10, 10}, {0, 0}, {10, 10}, {0, 0}, {0, 0}};
    CHECK(prCanvasPath2DRoundRect(path, 0, 0, 40, 40, radii, 0) == PrStatus_OutOfRange);
    CHECK(prCanvasPath2DRoundRect(path, 0, 0, 40, 40, radii, 5) == PrStatus_OutOfRange);
    PrDVec2 negative{-1, 0};
    CHECK(prCanvasPath2DRoundRect(path, 0, 0, 40, 40, &negative, 1) == PrStatus_OutOfRange);
    CHECK(prCanvasPath2DRoundRect(path, 0, 0, 40, 40, radii, 2) == PrStatus_Ok);
    // The upper left corner is rounded and the upper right is square.
    CHECK(!inside(path, 1, 1));
    CHECK(inside(path, 39, 1));
    prCanvasPath2DDestroy(path);

    // A negative width mirrors the rectangle, so the upper left radius applies to the corner at x.
    PrCanvasPath2D* flipped = prCanvasPath2DCreate();
    CHECK(prCanvasPath2DRoundRect(flipped, 40, 0, -40, 40, radii, 2) == PrStatus_Ok);
    CHECK(!inside(flipped, 39, 1));
    CHECK(inside(flipped, 1, 1));
    prCanvasPath2DDestroy(flipped);
}

TEST_CASE("arcTo joins the last point to a tangent arc in the corner of the two lines") {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    prCanvasPath2DMoveTo(path, 0, 0);
    CHECK(prCanvasPath2DArcTo(path, 100, 0, 100, 100, 50) == PrStatus_Ok);
    prCanvasPath2DLineTo(path, 100, 100);
    prCanvasPath2DLineTo(path, 0, 100);
    // The corner at (100, 0) is cut by the arc around (50, 50).
    CHECK(!inside(path, 99, 1));
    CHECK(inside(path, 50, 1));
    CHECK(inside(path, 99, 60));
    prCanvasPath2DDestroy(path);
}

TEST_CASE("a Path2D from path data holds its subpaths, and addPath maps them") {
    PrCanvasPath2D* data = prCanvasPath2DCreateFromPathData("M 0 0 L 10 0 L 10 10 L 0 10 Z");
    CHECK(inside(data, 5, 5));
    PrCanvasPath2D* moved = prCanvasPath2DCreate();
    prCanvasPath2DAddPath(moved, data, PrDAffine2{.a = 1, .d = 1, .e = 20, .f = 0});
    CHECK(inside(moved, 25, 5));
    CHECK(!inside(moved, 5, 5));
    // A path added to itself doubles.
    prCanvasPath2DAddPath(moved, moved, PrDAffine2{.a = 1, .d = 1, .e = 0, .f = 20});
    CHECK(inside(moved, 25, 25));
    PrCanvasPath2D* copy = prCanvasPath2DCreateCopy(moved);
    CHECK(inside(copy, 25, 5));
    prCanvasPath2DDestroy(copy);
    prCanvasPath2DDestroy(moved);
    prCanvasPath2DDestroy(data);
}

TEST_CASE("lineTo with no subpath starts one, and closePath starts the next at the first point") {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    prCanvasPath2DLineTo(path, 0, 0);
    prCanvasPath2DLineTo(path, 10, 0);
    prCanvasPath2DLineTo(path, 10, 10);
    prCanvasPath2DClosePath(path);
    prCanvasPath2DLineTo(path, 0, 10);
    prCanvasPath2DLineTo(path, -10, 10);
    CHECK(inside(path, 7, 3));
    CHECK(inside(path, -2, 7));
    prCanvasPath2DDestroy(path);
}
