#include "pr/path_polyline.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cmath>
#include <initializer_list>

namespace pr_impl {

namespace {

// Returns the signed area that the contour at `index` encloses, whether or not it is closed.
double contourArea(const PrPathPolyline& polyline, uint32_t index) {
    uint32_t start = polyline.contours[index];
    uint32_t end = prPathPolylineContourEnd(&polyline, index);
    double sum = 0;
    for (uint32_t k = start; k < end; ++k) {
        PrVec2 a = polyline.points[k];
        PrVec2 b = polyline.points[k + 1 < end ? k + 1 : start];
        sum += double(a.x) * double(b.y) - double(b.x) * double(a.y);
    }
    return sum / 2;
}

bool closed(const PrPathPolyline& polyline, uint32_t index) {
    return (polyline.closed[index / 64] >> (index % 64) & 1) != 0;
}

constexpr double kPi = 3.14159265358979;

PrPath* rect(float left, float top, float right, float bottom) {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{left, top, right, bottom});
    return path;
}

// Returns the area that the contours of the path enclose, each counted by its winding direction. The result of a
// combination winds once around what it covers, so this is its area.
double area(const PrPath* path) {
    double sum = 0;
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t at = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        if (verbs[i] == PrPathVerb_Move) {
            start = at++;
        } else if (verbs[i] == PrPathVerb_Line) {
            ++at;
        } else if (verbs[i] == PrPathVerb_Close) {
            for (uint32_t k = start; k < at; ++k) {
                PrVec2 a = points[k];
                PrVec2 b = points[k + 1 < at ? k + 1 : start];
                sum += double(a.x) * double(b.y) - double(b.x) * double(a.y);
            }
        }
    }
    return std::fabs(sum / 2);
}

uint32_t contours(const PrPath* path) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i)
        count += prPathVerbs(path)[i] == PrPathVerb_Move ? 1 : 0;
    return count;
}

// Appends to `out` the combination of the regions of the paths, flattened within `tolerance`.
void combinePaths(const PrPath* a, PrPathFillRule aRule, const PrPath* b, PrPathFillRule bRule, PrPathPolylineOp op,
        float tolerance, PrPath* out) {
    PrPathPolyline flatA{};
    PrPathPolyline flatB{};
    PrPathPolyline result{};
    prPathFlatten(a, prAffine2Identity(), tolerance, PrPathFlattening_Inside, &flatA);
    prPathFlatten(b, prAffine2Identity(), tolerance, PrPathFlattening_Inside, &flatB);
    prPathPolylineCombine(&flatA, aRule, &flatB, bRule, op, &result);
    prPathAddPolyline(out, &result, prAffine2Identity());
    prPathPolylineReset(&flatA);
    prPathPolylineReset(&flatB);
    prPathPolylineReset(&result);
}

// Appends to `out` the offset region of the path, flattened within `tolerance`.
void offsetPath(const PrPath* path, PrPathFillRule rule, float distance, PrPathJoin join, float miterLimit,
        float tolerance, PrPath* out) {
    PrPathPolyline flat{};
    PrPathPolyline result{};
    prPathFlatten(path, prAffine2Identity(), tolerance, PrPathFlattening_Inside, &flat);
    prPathPolylineOffset(&flat, rule, distance, join, miterLimit, tolerance, &result);
    prPathAddPolyline(out, &result, prAffine2Identity());
    prPathPolylineReset(&flat);
    prPathPolylineReset(&result);
}

double combined(const PrPath* a, const PrPath* b, PrPathPolylineOp op, PrPathFillRule rule = PrPathFillRule_NonZero) {
    PrPath* out = prPathCreate();
    combinePaths(a, rule, b, rule, op, 0.01f, out);
    double result = area(out);
    prPathDestroy(out);
    return result;
}

// Returns the winding number of a polygon path around a point, counted by the crossings of a ray toward +x.
int winding(const PrPath* path, PrVec2 p) {
    int w = 0;
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t at = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        if (verbs[i] == PrPathVerb_Move) {
            start = at++;
        } else if (verbs[i] == PrPathVerb_Line) {
            ++at;
        } else if (verbs[i] == PrPathVerb_Close) {
            for (uint32_t k = start; k < at; ++k) {
                PrVec2 a = points[k];
                PrVec2 b = points[k + 1 < at ? k + 1 : start];
                if ((a.y <= p.y) == (b.y <= p.y))
                    continue;
                float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > p.x)
                    w += b.y > a.y ? 1 : -1;
            }
        }
    }
    return w;
}

PrPath* randomPolygon(uint32_t& state, uint32_t count) {
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24);
    };
    PrPath* path = prPathCreate();
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 p{next() * 100, next() * 100};
        if (i == 0)
            prPathMoveTo(path, p);
        else
            prPathLineTo(path, p);
    }
    prPathClose(path);
    return path;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("closed flags hold for any number of contours") {
    PrPathPolyline polyline{};
    for (int i = 0; i < 200; ++i) {
        prPathPolylineMoveTo(&polyline, PrVec2{float(i), 0});
        prPathPolylineLineTo(&polyline, PrVec2{float(i), 1});
        if (i % 3 == 0)
            prPathPolylineClose(&polyline);
    }
    REQUIRE(polyline.contourCount == 200);
    for (uint32_t i = 0; i < 200; ++i)
        CHECK(closed(polyline, i) == (i % 3 == 0));
    prPathPolylineReset(&polyline);
}

TEST_CASE("a polyline is built a contour at a time, each open until it closes") {
    PrPathPolyline polyline{};
    prPathPolylineMoveTo(&polyline, PrVec2{0, 0});
    prPathPolylineLineTo(&polyline, PrVec2{10, 0});
    prPathPolylineLineTo(&polyline, PrVec2{10, 10});
    prPathPolylineClose(&polyline);
    prPathPolylineMoveTo(&polyline, PrVec2{20, 0});
    prPathPolylineLineTo(&polyline, PrVec2{30, 0});
    REQUIRE(polyline.contourCount == 2);
    CHECK(polyline.pointCount == 5);
    CHECK(polyline.contours[1] == 3);
    CHECK(closed(polyline, 0));
    CHECK_FALSE(closed(polyline, 1));
    CHECK(contourArea(polyline, 0) == doctest::Approx(50));
    PrRect bounds = prPathPolylineBounds(&polyline);
    CHECK(bounds.right == 30.0f);
    CHECK(bounds.bottom == 10.0f);
    prPathPolylineReset(&polyline);
}

TEST_CASE("overlapping squares unite, intersect, subtract and exclude") {
    PrPath* a = rect(0, 0, 10, 10);
    PrPath* b = rect(5, 5, 15, 15);
    CHECK(combined(a, b, PrPathPolylineOp_Union) == doctest::Approx(175));
    CHECK(combined(a, b, PrPathPolylineOp_Intersect) == doctest::Approx(25));
    CHECK(combined(a, b, PrPathPolylineOp_Difference) == doctest::Approx(75));
    CHECK(combined(b, a, PrPathPolylineOp_Difference) == doctest::Approx(75));
    CHECK(combined(a, b, PrPathPolylineOp_Xor) == doctest::Approx(150));
    prPathDestroy(a);
    prPathDestroy(b);
}

TEST_CASE("squares sharing an edge unite into one contour of four corners") {
    PrPath* a = rect(0, 0, 10, 10);
    PrPath* b = rect(10, 0, 20, 10);
    PrPath* out = prPathCreate();
    combinePaths(a, PrPathFillRule_NonZero, b, PrPathFillRule_NonZero, PrPathPolylineOp_Union, 0.01f, out);
    CHECK(area(out) == doctest::Approx(200));
    CHECK(contours(out) == 1);
    CHECK(prPathPointCount(out) == 4);
    // The union of a square with itself is the square. Xor and subtraction give nothing.
    CHECK(combined(a, a, PrPathPolylineOp_Union) == doctest::Approx(100));
    CHECK(combined(a, a, PrPathPolylineOp_Xor) == doctest::Approx(0));
    CHECK(combined(a, a, PrPathPolylineOp_Difference) == doctest::Approx(0));
    prPathDestroy(out);
    prPathDestroy(a);
    prPathDestroy(b);
}

TEST_CASE("a square cut from another leaves a hole") {
    PrPath* a = rect(0, 0, 30, 30);
    PrPath* b = rect(10, 10, 20, 20);
    PrPath* out = prPathCreate();
    combinePaths(a, PrPathFillRule_NonZero, b, PrPathFillRule_NonZero, PrPathPolylineOp_Difference, 0.01f, out);
    CHECK(area(out) == doctest::Approx(800));
    CHECK(contours(out) == 2);
    // Its hole winds the other way, so the even-odd rule gives the same region.
    PrPath* inner = rect(12, 12, 18, 18);
    CHECK(combined(out, inner, PrPathPolylineOp_Union) == doctest::Approx(836));
    prPathDestroy(inner);
    prPathDestroy(out);
    prPathDestroy(a);
    prPathDestroy(b);
}

TEST_CASE("a self-intersecting star fills by its rule") {
    PrPath* star = prPathCreate();
    for (int i = 0; i < 5; ++i) {
        double angle = -kPi / 2 + 2 * kPi * double(i * 2 % 5) / 5;
        PrVec2 p{float(10 * std::cos(angle)), float(10 * std::sin(angle))};
        if (i == 0)
            prPathMoveTo(star, p);
        else
            prPathLineTo(star, p);
    }
    prPathClose(star);
    PrPath* empty = prPathCreate();
    double nonzero = combined(star, empty, PrPathPolylineOp_Union, PrPathFillRule_NonZero);
    double evenodd = combined(star, empty, PrPathPolylineOp_Union, PrPathFillRule_EvenOdd);
    // The pentagon in the middle winds twice, so it is inside by nonzero and outside by even-odd.
    double inner = 10 * std::sin(kPi / 10) / std::sin(7 * kPi / 10);
    double pentagon = 2.5 * inner * inner * std::sin(2 * kPi / 5);
    CHECK(nonzero - evenodd == doctest::Approx(pentagon).epsilon(0.001));
    prPathDestroy(empty);
    prPathDestroy(star);
}

TEST_CASE("curves flatten within the tolerance") {
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{0, 0}, PrVec2{10, 10}, 0);
    PrPath* half = rect(0, -20, 20, 20);
    CHECK(combined(circle, half, PrPathPolylineOp_Intersect) == doctest::Approx(50 * kPi).epsilon(0.01));
    prPathDestroy(half);
    prPathDestroy(circle);
}

TEST_CASE("a region grows and shrinks by its joins") {
    PrPath* square = rect(0, 0, 10, 10);
    PrPath* out = prPathCreate();
    offsetPath(square, PrPathFillRule_NonZero, 2, PrPathJoin_Miter, 4, 0.01f, out);
    CHECK(area(out) == doctest::Approx(196));
    prPathClear(out);
    offsetPath(square, PrPathFillRule_NonZero, 2, PrPathJoin_Round, 4, 0.01f, out);
    CHECK(area(out) == doctest::Approx(100 + 80 + 4 * kPi).epsilon(0.005));
    prPathClear(out);
    offsetPath(square, PrPathFillRule_NonZero, 2, PrPathJoin_Bevel, 4, 0.01f, out);
    CHECK(area(out) == doctest::Approx(100 + 80 + 8));
    prPathClear(out);
    offsetPath(square, PrPathFillRule_NonZero, -2, PrPathJoin_Miter, 4, 0.01f, out);
    CHECK(area(out) == doctest::Approx(36));
    prPathClear(out);
    offsetPath(square, PrPathFillRule_NonZero, -6, PrPathJoin_Miter, 4, 0.01f, out);
    CHECK(prPathVerbCount(out) == 0);
    // A region thinner than twice the distance grows as a whole.
    PrPath* thin = rect(0, 0, 10, 1);
    prPathClear(out);
    offsetPath(thin, PrPathFillRule_NonZero, 3, PrPathJoin_Miter, 4, 0.01f, out);
    CHECK(area(out) == doctest::Approx(16 * 7));
    CHECK(contours(out) == 1);
    prPathDestroy(thin);
    // A concave corner shrinks to a point with a miter join, and to a round corner with a round join.
    PrPath* ell = prPathCreate();
    prPathMoveTo(ell, PrVec2{0, 0});
    prPathLineTo(ell, PrVec2{20, 0});
    prPathLineTo(ell, PrVec2{20, 10});
    prPathLineTo(ell, PrVec2{10, 10});
    prPathLineTo(ell, PrVec2{10, 20});
    prPathLineTo(ell, PrVec2{0, 20});
    prPathClose(ell);
    prPathClear(out);
    offsetPath(ell, PrPathFillRule_NonZero, -2, PrPathJoin_Miter, 4, 0.01f, out);
    double mitered = area(out);
    prPathClear(out);
    offsetPath(ell, PrPathFillRule_NonZero, -2, PrPathJoin_Round, 4, 0.01f, out);
    // A miter fills the whole square at the corner, and a round join the quarter circle within it.
    CHECK(area(out) - mitered == doctest::Approx(4 - kPi).epsilon(0.05));
    prPathDestroy(ell);
    prPathDestroy(out);
    prPathDestroy(square);
}

TEST_CASE("random self-intersecting polygons combine as their windings say") {
    uint32_t state = 7;
    const PrPathPolylineOp ops[] = {
        PrPathPolylineOp_Union, PrPathPolylineOp_Intersect, PrPathPolylineOp_Difference,
        PrPathPolylineOp_Xor,
    };
    for (uint32_t trial = 0; trial < 12; ++trial) {
        PrPath* a = randomPolygon(state, 12 + trial);
        PrPath* b = randomPolygon(state, 9 + trial);
        PrPathFillRule rule = trial % 2 == 0 ? PrPathFillRule_NonZero : PrPathFillRule_EvenOdd;
        for (PrPathPolylineOp op : ops) {
            PrPath* out = prPathCreate();
            combinePaths(a, rule, b, rule, op, 0.01f, out);
            uint32_t wrong = 0;
            for (uint32_t y = 0; y < 50; ++y) {
                for (uint32_t x = 0; x < 50; ++x) {
                    PrVec2 p{float(x) * 2 + 0.37f, float(y) * 2 + 0.61f};
                    auto in = [&](int w) {
                        return rule == PrPathFillRule_NonZero ? w != 0 : (w & 1) != 0;
                    };
                    bool ia = in(winding(a, p));
                    bool ib = in(winding(b, p));
                    bool expected;
                    if (op == PrPathPolylineOp_Union)
                        expected = ia || ib;
                    else if (op == PrPathPolylineOp_Intersect)
                        expected = ia && ib;
                    else if (op == PrPathPolylineOp_Difference)
                        expected = ia && !ib;
                    else
                        expected = ia != ib;
                    int w = winding(out, p);
                    // The result winds once around what it covers, in either direction.
                    CHECK((w == 0 || w == 1 || w == -1));
                    wrong += (w != 0) != expected ? 1 : 0;
                }
            }
            CAPTURE(trial);
            CAPTURE(int(op));
            CHECK(wrong == 0);
            prPathDestroy(out);
        }
        prPathDestroy(a);
        prPathDestroy(b);
    }
}

TEST_CASE("a contour of no area leaves nothing where it is intersected") {
    // A lone point, as a contour of one vertex closed onto itself, beside a square and on its edge. The region
    // intersected with each contains all of it.
    for (PrVec2 point : {PrVec2{50, 50}, PrVec2{15, 10}, PrVec2{20, 13.37f}}) {
        PrPath* shapes = prPathCreate();
        prPathMoveTo(shapes, point);
        prPathCubicTo(shapes, point, point, point);
        prPathClose(shapes);
        // A loop that goes out a little way and comes back, which is how a lone vertex with a tangent is drawn.
        PrVec2 away{point.x - 0.181f, point.y - 0.003f};
        prPathMoveTo(shapes, point);
        prPathCubicTo(shapes, point, away, point);
        prPathCubicTo(shapes, point, point, point);
        prPathClose(shapes);
        prPathAddRect(shapes, PrRect{10, 10, 20, 20});
        PrPath* region = rect(0, 0, 100, 100);
        PrPath* out = prPathCreate();
        combinePaths(shapes, PrPathFillRule_NonZero, region, PrPathFillRule_NonZero, PrPathPolylineOp_Intersect, 0.1f, out);
        PrRect bounds = prPathBounds(out);
        CHECK(bounds.right <= 20.0f);
        CHECK(area(out) == doctest::Approx(100));
        // Every remaining contour encloses area. None is a point or a line folded onto itself.
        const PrPathVerb* verbs = prPathVerbs(out);
        uint32_t contours = 0;
        for (uint32_t i = 0; i < prPathVerbCount(out); ++i)
            contours += verbs[i] == PrPathVerb_Move ? 1 : 0;
        CHECK(contours == 1);
        prPathDestroy(out);
        prPathDestroy(region);
        prPathDestroy(shapes);
    }
}

TEST_CASE("polylines with points that are not finite combine to nothing") {
    PrPath* square = rect(0, 0, 10, 10);
    for (float bad : {INFINITY, -INFINITY, NAN}) {
        PrPath* broken = prPathCreate();
        prPathMoveTo(broken, PrVec2{0, 0});
        prPathLineTo(broken, PrVec2{bad, 5});
        prPathLineTo(broken, PrVec2{5, 10});
        prPathClose(broken);
        for (PrPathPolylineOp op :
                {PrPathPolylineOp_Union, PrPathPolylineOp_Intersect, PrPathPolylineOp_Difference, PrPathPolylineOp_Xor}) {
            PrPath* out = prPathCreate();
            combinePaths(square, PrPathFillRule_NonZero, broken, PrPathFillRule_NonZero, op, 0.1f, out);
            CHECK(prPathVerbCount(out) == 0);
            prPathClear(out);
            offsetPath(broken, PrPathFillRule_NonZero, 2, PrPathJoin_Round, 4, 0.1f, out);
            CHECK(prPathVerbCount(out) == 0);
            prPathDestroy(out);
        }
        prPathDestroy(broken);
    }
    prPathDestroy(square);
}
