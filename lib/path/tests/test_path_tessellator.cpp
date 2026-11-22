#include "pr/array.h"
#include "pr/float.h"
#include "pr/path_tessellator.h"
#include "pr/random.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

constexpr double kPi = 3.14159265358979;

PrPath* polygon(const Array<PrVec2>& points) {
    PrPath* path = prPathCreate();
    for (uint32_t i = 0; i < points.count(); ++i) {
        if (i == 0)
            prPathMoveTo(path, points[i]);
        else
            prPathLineTo(path, points[i]);
    }
    prPathClose(path);
    return path;
}

double twiceArea(PrVec2 a, PrVec2 b, PrVec2 c) {
    return (double(b.x) - double(a.x)) * (double(c.y) - double(a.y)) -
        (double(b.y) - double(a.y)) * (double(c.x) - double(a.x));
}

// Returns the integral of the coverage of the mesh: the area of each triangle times the mean coverage of its vertices.
double covered(const PrPathMesh& mesh) {
    double sum = 0;
    for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
        uint32_t a = mesh.indices[i];
        uint32_t b = mesh.indices[i + 1];
        uint32_t c = mesh.indices[i + 2];
        double area = prF64Abs(twiceArea(mesh.points[a], mesh.points[b], mesh.points[c])) / 2;
        sum += area * double(mesh.coverage[a] + mesh.coverage[b] + mesh.coverage[c]) / 3;
    }
    return sum;
}

// Tessellates the path under the transform, flattened within `tolerance`.
void tessellate(PrPathTessellator* t, const PrPath* path, PrAffine2 transform, PrPathFillRule rule, float tolerance,
        float fringe, PrPathMesh* out) {
    PrPathPolyline polyline{};
    prPathFlatten(path, transform, tolerance, PrPathFlattening_Balanced, &polyline);
    *out = prPathTessellatorFill(t, &polyline, rule, fringe);
    prPathPolylineReset(&polyline);
}

double fill(PrPathTessellator* t, const PrPath* path, PrPathFillRule rule, float fringe, PrPathMesh* out = nullptr) {
    PrPathMesh mesh{};
    tessellate(t, path, prAffine2Identity(), rule, 0.05f, fringe, &mesh);
    if (out != nullptr)
        *out = mesh;
    return covered(mesh);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a rectangle fans, and its fringe keeps its area") {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{10, 10, 50, 30});
    PrPathMesh mesh{};
    CHECK(fill(t, path, PrPathFillRule_NonZero, 0, &mesh) == doctest::Approx(800));
    CHECK(mesh.vertexCount == 4);
    CHECK(mesh.indexCount == 6);
    // The corner of a miter covers a little more than the area of the corner.
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1, &mesh) == doctest::Approx(800).epsilon(0.001));
    CHECK(mesh.vertexCount == 8);
    CHECK(mesh.indexCount == 6 + 24);
    // The inner corners moved in by half the fringe, the outer ones out.
    float left = 1e9f;
    float right = -1e9f;
    for (uint32_t i = 0; i < mesh.vertexCount; ++i) {
        left = mesh.points[i].x < left ? mesh.points[i].x : left;
        right = mesh.points[i].x > right ? mesh.points[i].x : right;
    }
    CHECK(left == doctest::Approx(9.5));
    CHECK(right == doctest::Approx(50.5));
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

TEST_CASE("a circle covers its area with and without a fringe") {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, PrVec2{20, 20}, PrVec2{20, 20}, 0);
    CHECK(fill(t, path, PrPathFillRule_NonZero, 0) == doctest::Approx(kPi * 400).epsilon(0.002));
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1) == doctest::Approx(kPi * 400).epsilon(0.002));
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

TEST_CASE("the fill rules decide a star's center and a ring's hole") {
    PrPathTessellator* t = prPathTessellatorCreate();
    Array<PrVec2> star;
    for (int i = 0; i < 5; ++i) {
        double angle = -kPi / 2 + i * 4 * kPi / 5;
        star.push(PrVec2{float(50 + 40 * prF64Cos(angle)), float(50 + 40 * prF64Sin(angle))});
    }
    PrPath* path = polygon(star);
    double nonzero = fill(t, path, PrPathFillRule_NonZero, 0);
    double evenOdd = fill(t, path, PrPathFillRule_EvenOdd, 0);
    // The center pentagon has the circumradius R cos(72°) / cos(36°), and area (5/2) r² sin(72°).
    double inner = 40 * prF64Cos(2 * kPi / 5) / prF64Cos(kPi / 5);
    double pentagon = 2.5 * inner * inner * prF64Sin(2 * kPi / 5);
    CHECK(nonzero - evenOdd == doctest::Approx(pentagon).epsilon(0.001));
    prPathDestroy(path);

    // Two rings wound the same way: the nonzero rule fills the hole, and the even-odd rule does not.
    path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 30, 30});
    prPathAddRect(path, PrRect{10, 10, 20, 20});
    CHECK(fill(t, path, PrPathFillRule_NonZero, 0) == doctest::Approx(900));
    CHECK(fill(t, path, PrPathFillRule_EvenOdd, 0) == doctest::Approx(800));
    CHECK(fill(t, path, PrPathFillRule_EvenOdd, 1) == doctest::Approx(800));
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

TEST_CASE("overlaps, shared edges and touching corners fill once") {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    prPathAddRect(path, PrRect{5, 5, 15, 15});
    CHECK(fill(t, path, PrPathFillRule_NonZero, 0) == doctest::Approx(175));
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1) == doctest::Approx(175).epsilon(0.005));
    CHECK(fill(t, path, PrPathFillRule_EvenOdd, 0) == doctest::Approx(150));
    prPathClear(path);
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    prPathAddRect(path, PrRect{10, 0, 20, 10});
    PrPathMesh mesh{};
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1, &mesh) == doctest::Approx(200).epsilon(0.005));
    // The shared edge is not on the boundary: one fringe loop of the six corners goes around both.
    CHECK(mesh.indexCount - 3 * 4 == 6 * 6);
    prPathClear(path);
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    prPathAddRect(path, PrRect{10, 10, 20, 20});
    CHECK(fill(t, path, PrPathFillRule_NonZero, 0) == doctest::Approx(200));
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1) == doctest::Approx(200).epsilon(0.005));
    // A contour and its reverse cancel.
    prPathClear(path);
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{0, 10});
    prPathLineTo(path, PrVec2{10, 10});
    prPathLineTo(path, PrVec2{10, 0});
    prPathClose(path);
    CHECK(fill(t, path, PrPathFillRule_NonZero, 1) == doctest::Approx(0));
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

TEST_CASE("the transform maps the path before it flattens") {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, PrVec2{0, 0}, PrVec2{1, 1}, 0);
    PrPathMesh mesh{};
    tessellate(t, path, prAffine2Scale(PrVec2{100, 50}), PrPathFillRule_NonZero, 0.1f, 0, &mesh);
    CHECK(covered(mesh) == doctest::Approx(kPi * 5000).epsilon(0.002));
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

TEST_CASE("inputs that are not finite, and paths with no area, tessellate nothing") {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    PrPathMesh mesh{};
    tessellate(t, path, prAffine2Scale(PrVec2{kPrF32Nan, 1}), PrPathFillRule_NonZero, 0.1f, 1, &mesh);
    CHECK(mesh.indexCount == 0);
    prPathClear(path);
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{10, 10});
    prPathLineTo(path, PrVec2{20, 20});
    prPathClose(path);
    tessellate(t, path, prAffine2Identity(), PrPathFillRule_NonZero, 0.1f, 1, &mesh);
    CHECK(mesh.indexCount == 0);
    prPathDestroy(path);
    prPathTessellatorDestroy(t);
}

