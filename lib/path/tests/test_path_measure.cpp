#include "pr/path_measure.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <cmath>
#include <initializer_list>

namespace pr_impl {

namespace {

PrPath* square() {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    return path;
}

// Returns a measure of the path with its curves flattened.
PrPathMeasure* measureOf(const PrPath* path) {
    PrPathMeasure* measure = prPathMeasureCreate();
    prPathMeasureSetPath(measure, path, prAffine2Identity(), 0.01f);
    return measure;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a measure runs through its contours end to end, each closed or open") {
    PrPath* path = square();
    prPathMoveTo(path, PrVec2{20, 0});
    prPathLineTo(path, PrVec2{23, 4});
    prPathMoveTo(path, PrVec2{50, 50});
    PrPathMeasure* measure = measureOf(path);
    CHECK(prPathMeasureLength(measure) == doctest::Approx(45));
    REQUIRE(prPathMeasureContourCount(measure) == 2);
    PrPathMeasureContour first = prPathMeasureContour(measure, 0);
    CHECK(first.start == 0.0f);
    CHECK(first.length == doctest::Approx(40));
    CHECK(first.closed);
    PrPathMeasureContour second = prPathMeasureContour(measure, 1);
    CHECK(second.start == doctest::Approx(40));
    CHECK(second.length == doctest::Approx(5));
    CHECK(!second.closed);
    // Measuring again replaces what the measure held.
    prPathMeasureSetPath(measure, path, prAffine2Scale(PrVec2{2, 2}), 0.01f);
    CHECK(prPathMeasureLength(measure) == doctest::Approx(90));
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("a position along the path is held to its ends, and crosses into the next contour") {
    PrPath* path = square();
    prPathMoveTo(path, PrVec2{20, 0});
    prPathLineTo(path, PrVec2{20, 10});
    PrPathMeasure* measure = measureOf(path);
    PrPathMeasurePosition at = prPathMeasurePosition(measure, 15);
    CHECK(at.point.x == doctest::Approx(10));
    CHECK(at.point.y == doctest::Approx(5));
    CHECK(at.tangent.y == doctest::Approx(1));
    at = prPathMeasurePosition(measure, 40);
    CHECK(at.point.x == doctest::Approx(0));
    CHECK(at.point.y == doctest::Approx(0));
    at = prPathMeasurePosition(measure, 44);
    CHECK(at.point.x == doctest::Approx(20));
    CHECK(at.point.y == doctest::Approx(4));
    at = prPathMeasurePosition(measure, 99);
    CHECK(at.point.y == doctest::Approx(10));
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("a line laid on the path keeps its distances along the path and turns with it") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{10, 0});
    prPathLineTo(path, PrVec2{10, 10});
    PrPathMeasure* measure = measureOf(path);
    PrAffine2 onPath{};
    // Along the second side, the x axis of the line runs down, and its y axis runs to the right of the path, which is
    // -x.
    REQUIRE(prPathMeasureLineTransform(measure, 15, &onPath));
    PrVec2 at = prAffine2MapPoint(onPath, PrVec2{15, 0});
    CHECK(at.x == doctest::Approx(10));
    CHECK(at.y == doctest::Approx(5));
    PrVec2 before = prAffine2MapPoint(onPath, PrVec2{13, 2});
    CHECK(before.x == doctest::Approx(8));
    CHECK(before.y == doctest::Approx(3));
    CHECK(!prPathMeasureLineTransform(measure, 21, &onPath));
    CHECK(!prPathMeasureLineTransform(measure, -1, &onPath));
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("a segment is the part of each contour its range crosses") {
    PrPath* path = square();
    prPathMoveTo(path, PrVec2{20, 0});
    prPathLineTo(path, PrVec2{20, 10});
    PrPathMeasure* measure = measureOf(path);
    PrPathPolyline out{};
    prPathMeasureSegment(measure, 35, 45, &out);
    CHECK(out.contourCount == 2);
    PrPathMeasure* part = prPathMeasureCreate();
    prPathMeasureSetPolyline(part, &out);
    CHECK(prPathMeasureLength(part) == doctest::Approx(10));
    CHECK(!prPathMeasureContour(part, 0).closed);
    prPathPolylineClear(&out);
    prPathMeasureSegment(measure, 45, 35, &out);
    CHECK(out.contourCount == 0);
    prPathMeasureDestroy(part);
    prPathPolylineReset(&out);
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("the nearest point knows how far along and to which side it is") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{100, 0});
    prPathLineTo(path, PrVec2{100, 100});
    PrPathMeasure* measure = measureOf(path);
    PrPathMeasureNearest nearest{};
    REQUIRE(prPathMeasureNearest(measure, PrVec2{30, 4}, 10, &nearest));
    CHECK(nearest.distance == doctest::Approx(30));
    CHECK(nearest.offset == doctest::Approx(4));
    REQUIRE(prPathMeasureNearest(measure, PrVec2{103, 60}, 10, &nearest));
    CHECK(nearest.distance == doctest::Approx(160));
    CHECK(nearest.offset == doctest::Approx(-3));
    CHECK(!prPathMeasureNearest(measure, PrVec2{50, 50}, 10, &nearest));
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("a contour that returns to its start before closing leaves later contours' distances in place") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{10, 0});
    prPathLineTo(path, PrVec2{10, 10});
    prPathLineTo(path, PrVec2{0, 0});
    prPathClose(path);
    prPathMoveTo(path, PrVec2{20, 0});
    prPathLineTo(path, PrVec2{20, 4});
    prPathLineTo(path, PrVec2{30, 4});
    prPathClose(path);
    PrPathMeasure* measure = measureOf(path);
    REQUIRE(prPathMeasureContourCount(measure) == 2);
    float start = prPathMeasureContour(measure, 1).start;
    CHECK(start == doctest::Approx(20 + std::sqrt(200.0f)));
    PrPathMeasurePosition at = prPathMeasurePosition(measure, start + 2);
    CHECK(at.point.x == doctest::Approx(20));
    CHECK(at.point.y == doctest::Approx(2));
    PrPathMeasureNearest nearest{};
    REQUIRE(prPathMeasureNearest(measure, PrVec2{25, 5}, 3, &nearest));
    CHECK(nearest.contour == 1);
    CHECK(nearest.distance == doctest::Approx(start + 9));
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
}

TEST_CASE("a path or distances that are not finite measure without failing") {
    for (float bad : {INFINITY, -INFINITY, NAN}) {
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{0, 0});
        prPathLineTo(path, PrVec2{bad, 5});
        prPathCubicTo(path, PrVec2{bad, bad}, PrVec2{1, 2}, PrVec2{3, 4});
        prPathClose(path);
        PrPathMeasure* empty = prPathMeasureCreate();
        for (PrPath* measured : {path, square()}) {
            PrPathMeasure* measure = measureOf(measured);
            PrPathPolyline out{};
            for (PrPathMeasure* m : {measure, empty}) {
                PrAffine2 onPath{};
                prPathMeasurePosition(m, bad);
                prPathMeasurePosition(m, 1);
                prPathMeasureLineTransform(m, bad, &onPath);
                prPathMeasureSegment(m, 0, bad, &out);
                prPathMeasureSegment(m, bad, 2, &out);
                PrPathMeasureNearest nearest{};
                prPathMeasureNearest(m, PrVec2{bad, 1}, 5, &nearest);
                prPathMeasureNearest(m, PrVec2{1, 1}, bad, &nearest);
            }
            prPathPolylineReset(&out);
            prPathMeasureDestroy(measure);
            if (measured != path)
                prPathDestroy(measured);
        }
        prPathMeasureDestroy(empty);
        prPathDestroy(path);
    }
}
