#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <cmath>
#include <string>

extern "C" int prTestPathCApi(void);

namespace pr_impl {

namespace {

// Returns the verbs of the path as letters, each followed by its points.
std::string describe(const PrPath* path) {
    std::string text;
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t next = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        static constexpr const char kLetter[] = "MLQCZ";
        static constexpr uint32_t kPoints[] = {1, 1, 2, 3, 0};
        if (!text.empty())
            text += ' ';
        text += kLetter[verbs[i]];
        for (uint32_t j = 0; j < kPoints[verbs[i]]; ++j, ++next)
            text += " " + std::to_string(int(points[next].x)) + "," + std::to_string(int(points[next].y));
    }
    return text;
}

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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestPathCApi() == 0);
}

TEST_CASE("segments record verbs and points") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{1, 2});
    prPathLineTo(path, PrVec2{3, 4});
    prPathQuadTo(path, PrVec2{5, 6}, PrVec2{7, 8});
    prPathCubicTo(path, PrVec2{9, 10}, PrVec2{11, 12}, PrVec2{13, 14});
    prPathClose(path);
    CHECK(describe(path) == "M 1,2 L 3,4 Q 5,6 7,8 C 9,10 11,12 13,14 Z");
    CHECK(prPathPointCount(path) == 7);
    prPathDestroy(path);
}

TEST_CASE("a segment with no open contour starts one where the last one started") {
    PrPath* path = prPathCreate();
    prPathLineTo(path, PrVec2{5, 5});
    prPathMoveTo(path, PrVec2{10, 0});
    prPathLineTo(path, PrVec2{20, 0});
    prPathClose(path);
    prPathClose(path);
    prPathLineTo(path, PrVec2{20, 20});
    CHECK(describe(path) == "M 0,0 L 5,5 M 10,0 L 20,0 Z M 10,0 L 20,20");
    prPathDestroy(path);
}

TEST_CASE("shapes add closed clockwise contours") {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{1, 2, 3, 4});
    prPathAddRect(path, PrRect{5, 5, 5, 9});
    CHECK(describe(path) == "M 1,2 L 3,2 L 3,4 L 1,4 Z");

    prPathClear(path);
    PrCornerRect shape{PrRect{0, 0, 10, 10}, {prCornerRound(PrVec2{2, 2}), {}, {}, {}}};
    prPathAddCornerRect(path, shape);
    CHECK(describe(path) == "M 2,0 L 10,0 L 10,10 L 0,10 L 0,2 C 0,0 0,0 2,0 Z");

    prPathClear(path);
    prPathAddEllipse(path, PrVec2{10, 5}, PrVec2{10, 5}, 0);
    CHECK(prPathVerbCount(path) == 6);
    PrRect bounds = prPathBounds(path);
    CHECK(bounds.left == 0.0f);
    CHECK(bounds.top == 0.0f);
    CHECK(bounds.right == 20.0f);
    CHECK(bounds.bottom == 10.0f);
    prPathDestroy(path);
}

TEST_CASE("a corner's curves stay within its rectangle, however sharply they turn") {
    const float shapes[] = {0.5f, 2, 5, 24, -0.5f, -24};
    for (float k : shapes) {
        PrPath* path = prPathCreate();
        PrCorner corner{PrVec2{25, 25}, k};
        prPathAddCornerRect(path, PrCornerRect{PrRect{0, 0, 100, 100}, {corner, corner, corner, corner}});
        PrRect bounds = prPathBounds(path);
        CHECK(bounds.left >= -1e-3f);
        CHECK(bounds.top >= -1e-3f);
        CHECK(bounds.right <= 100 + 1e-3f);
        CHECK(bounds.bottom <= 100 + 1e-3f);
        prPathDestroy(path);
    }
}

TEST_CASE("a clone is independent and a transform maps every point") {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 1, 1});
    PrPath* copy = prPathClone(path);
    prPathTransform(path, prAffine2Scale(PrVec2{10, 20}));
    CHECK(describe(path) == "M 0,0 L 10,0 L 10,20 L 0,20 Z");
    CHECK(describe(copy) == "M 0,0 L 1,0 L 1,1 L 0,1 Z");
    prPathLineTo(copy, PrVec2{4, 4});
    CHECK(describe(copy) == "M 0,0 L 1,0 L 1,1 L 0,1 Z M 0,0 L 4,4");
    prPathDestroy(copy);
    prPathDestroy(path);
    prPathDestroy(nullptr);
}

TEST_CASE("clones share their path's id until one changes, and every change gives a new id") {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 1, 1});
    uint64_t id = prPathId(path);
    CHECK(id != 0);
    CHECK(prPathId(path) == id);
    PrPath* copy = prPathClone(path);
    CHECK(prPathId(copy) == id);
    prPathLineTo(copy, PrVec2{4, 4});
    uint64_t changed = prPathId(copy);
    CHECK(changed != id);
    CHECK(prPathId(path) == id);
    // A path that no clone shares changes in place, and gets a new id.
    prPathLineTo(copy, PrVec2{5, 4});
    CHECK(prPathId(copy) != changed);
    prPathClear(path);
    CHECK(prPathId(path) != id);
    prPathDestroy(copy);
    prPathDestroy(path);
}

TEST_CASE("a path flattens into contours of its points, closed where it closes") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{10, 0});
    prPathLineTo(path, PrVec2{10, 10});
    prPathClose(path);
    prPathMoveTo(path, PrVec2{20, 0});
    prPathLineTo(path, PrVec2{30, 0});
    PrPathPolyline polyline{};
    prPathFlatten(path, prAffine2Translate(PrVec2{1, 2}), 0.1f, PrPathFlattening_Inside, &polyline);
    REQUIRE(polyline.contourCount == 2);
    CHECK(polyline.pointCount == 5);
    CHECK(polyline.contours[0] == 0);
    CHECK(polyline.contours[1] == 3);
    CHECK(prPathPolylineContourIsClosed(&polyline, 0));
    CHECK_FALSE(prPathPolylineContourIsClosed(&polyline, 1));
    CHECK(polyline.points[1].x == 11.0f);
    CHECK(polyline.points[1].y == 2.0f);

    // Clearing keeps the storage, and the polyline then starts afresh.
    PrVec2* storage = polyline.points;
    prPathPolylineClear(&polyline);
    CHECK(polyline.pointCount == 0);
    CHECK(polyline.contourCount == 0);
    prPathFlatten(path, prAffine2Identity(), 0.1f, PrPathFlattening_Inside, &polyline);
    CHECK(polyline.points == storage);
    CHECK(polyline.contourCount == 2);
    prPathPolylineReset(&polyline);
    CHECK(polyline.points == nullptr);
    CHECK(polyline.pointCapacity == 0);
    prPathDestroy(path);
}

TEST_CASE("curves flatten within the tolerance, inside them or balanced to their area") {
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{0, 0}, PrVec2{10, 10}, 0);
    constexpr double kArea = 3.14159265358979 * 100;
    for (float tolerance : {0.5f, 0.05f}) {
        PrPathPolyline inside{};
        prPathFlatten(circle, prAffine2Identity(), tolerance, PrPathFlattening_Inside, &inside);
        REQUIRE(inside.contourCount == 1);
        for (uint32_t i = 0; i < inside.pointCount; ++i) {
            PrVec2 p = inside.points[i];
            double radius = std::sqrt(double(p.x) * double(p.x) + double(p.y) * double(p.y));
            // The cubics of the ellipse lie up to 0.027% of the radius outside the circle.
            CHECK(radius <= 10.003);
            CHECK(radius >= 10 - double(tolerance) - 0.001);
        }
        CHECK(std::fabs(contourArea(inside, 0)) < kArea);
        PrPathPolyline balanced{};
        prPathFlatten(circle, prAffine2Identity(), tolerance, PrPathFlattening_Balanced, &balanced);
        CHECK(std::fabs(contourArea(balanced, 0)) == doctest::Approx(kArea).epsilon(0.001));
        prPathPolylineReset(&inside);
        prPathPolylineReset(&balanced);
    }
    prPathDestroy(circle);
}

TEST_CASE("a point is in a path by its fill rule, and curves count as they bend") {
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 100, 100});
    prPathAddRect(path, PrRect{25, 25, 75, 75});
    // The inner square winds as the outer one does: nonzero counts it in, even-odd leaves a hole.
    CHECK(prPathContains(path, PrVec2{50, 50}, PrPathFillRule_NonZero));
    CHECK_FALSE(prPathContains(path, PrVec2{50, 50}, PrPathFillRule_EvenOdd));
    CHECK(prPathContains(path, PrVec2{10, 50}, PrPathFillRule_EvenOdd));
    CHECK_FALSE(prPathContains(path, PrVec2{150, 50}, PrPathFillRule_NonZero));
    prPathDestroy(path);
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{50, 50}, PrVec2{50, 50}, 0);
    CHECK(prPathContains(circle, PrVec2{50, 2}, PrPathFillRule_NonZero));
    CHECK_FALSE(prPathContains(circle, PrVec2{8, 8}, PrPathFillRule_NonZero));
    CHECK(prPathContains(circle, PrVec2{50, 50}, PrPathFillRule_EvenOdd));
    prPathDestroy(circle);
}
