#include "pr/path_tessellator.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace pr_impl {

namespace {

constexpr double kPi = 3.14159265358979;

PrPath* polygon(const std::vector<PrVec2>& points) {
    PrPath* path = prPathCreate();
    for (size_t i = 0; i < points.size(); ++i) {
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
        double area = std::fabs(twiceArea(mesh.points[a], mesh.points[b], mesh.points[c])) / 2;
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

// Returns the number of triangles of the mesh that contain the point.
uint32_t containing(const PrPathMesh& mesh, PrVec2 p) {
    uint32_t count = 0;
    for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
        PrVec2 a = mesh.points[mesh.indices[i]];
        PrVec2 b = mesh.points[mesh.indices[i + 1]];
        PrVec2 c = mesh.points[mesh.indices[i + 2]];
        double d1 = twiceArea(a, b, p);
        double d2 = twiceArea(b, c, p);
        double d3 = twiceArea(c, a, p);
        if ((d1 > 0 && d2 > 0 && d3 > 0) || (d1 < 0 && d2 < 0 && d3 < 0))
            ++count;
    }
    return count;
}

// Returns the winding of the polygons around the point.
int winding(const std::vector<std::vector<PrVec2>>& rings, PrVec2 p) {
    int w = 0;
    for (const std::vector<PrVec2>& ring : rings) {
        for (size_t i = 0; i < ring.size(); ++i) {
            PrVec2 a = ring[i];
            PrVec2 b = ring[(i + 1) % ring.size()];
            if (a.y <= p.y) {
                if (b.y > p.y && twiceArea(a, b, p) > 0)
                    ++w;
            } else if (b.y <= p.y && twiceArea(a, b, p) < 0) {
                --w;
            }
        }
    }
    return w;
}

// A small deterministic generator.
struct Random {
    uint64_t state;

    float next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return float((state >> 40) & 0xffffff) / float(0x1000000);
    }
};

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
    std::vector<PrVec2> star;
    for (int i = 0; i < 5; ++i) {
        double angle = -kPi / 2 + i * 4 * kPi / 5;
        star.push_back(PrVec2{float(50 + 40 * std::cos(angle)), float(50 + 40 * std::sin(angle))});
    }
    PrPath* path = polygon(star);
    double nonzero = fill(t, path, PrPathFillRule_NonZero, 0);
    double evenOdd = fill(t, path, PrPathFillRule_EvenOdd, 0);
    // The center pentagon has the circumradius R cos(72°) / cos(36°), and area (5/2) r² sin(72°).
    double inner = 40 * std::cos(2 * kPi / 5) / std::cos(kPi / 5);
    double pentagon = 2.5 * inner * inner * std::sin(2 * kPi / 5);
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
    tessellate(t, path, prAffine2Scale(PrVec2{NAN, 1}), PrPathFillRule_NonZero, 0.1f, 1, &mesh);
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

TEST_CASE("random polygons' triangles cover each point as the fill rule does") {
    PrPathTessellator* t = prPathTessellatorCreate();
    Random random{12345};
    for (int round = 0; round < 60; ++round) {
        std::vector<std::vector<PrVec2>> rings;
        PrPath* path = prPathCreate();
        int ringCount = 1 + round % 3;
        for (int r = 0; r < ringCount; ++r) {
            std::vector<PrVec2> ring;
            int n = 3 + int(random.next() * 12);
            for (int i = 0; i < n; ++i) {
                // Some rounds snap to a coarse grid, for shared vertices and collinear edges.
                float x = random.next() * 100;
                float y = random.next() * 100;
                if (round % 4 == 0) {
                    x = std::floor(x / 20) * 20;
                    y = std::floor(y / 20) * 20;
                }
                ring.push_back(PrVec2{x, y});
            }
            rings.push_back(ring);
            for (size_t i = 0; i < ring.size(); ++i) {
                if (i == 0)
                    prPathMoveTo(path, ring[i]);
                else
                    prPathLineTo(path, ring[i]);
            }
            prPathClose(path);
        }
        for (PrPathFillRule rule : {PrPathFillRule_NonZero, PrPathFillRule_EvenOdd}) {
            PrPathMesh mesh{};
            tessellate(t, path, prAffine2Identity(), rule, 0.1f, 0, &mesh);
            uint32_t wrong = 0;
            for (int sy = 0; sy < 40; ++sy) {
                for (int sx = 0; sx < 40; ++sx) {
                    // Off the coarse grid and its diagonals.
                    PrVec2 p{float(sx) * 2.5f + 0.3173f, float(sy) * 2.5f + 0.7331f};
                    int w = winding(rings, p);
                    bool in = rule == PrPathFillRule_EvenOdd ? (w & 1) != 0 : w != 0;
                    if (containing(mesh, p) != (in ? 1u : 0u))
                        ++wrong;
                }
            }
            CHECK_MESSAGE(wrong == 0, "round ", round, " rule ", int(rule));
        }
        prPathDestroy(path);
    }
    prPathTessellatorDestroy(t);
}

TEST_CASE("with a fringe, points away from the boundary are still covered once or not at all") {
    PrPathTessellator* t = prPathTessellatorCreate();
    Random random{777};
    auto distance = [](PrVec2 a, PrVec2 b, PrVec2 p) {
        double dx = double(b.x) - double(a.x);
        double dy = double(b.y) - double(a.y);
        double length2 = dx * dx + dy * dy;
        double u = length2 > 0 ? ((double(p.x) - double(a.x)) * dx + (double(p.y) - double(a.y)) * dy) / length2 : 0;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        double ex = double(a.x) + u * dx - double(p.x);
        double ey = double(a.y) + u * dy - double(p.y);
        return std::sqrt(ex * ex + ey * ey);
    };
    for (int round = 0; round < 60; ++round) {
        std::vector<std::vector<PrVec2>> rings;
        PrPath* path = prPathCreate();
        int ringCount = 1 + round % 3;
        for (int r = 0; r < ringCount; ++r) {
            std::vector<PrVec2> ring;
            int n = 3 + int(random.next() * 10);
            for (int i = 0; i < n; ++i)
                ring.push_back(PrVec2{random.next() * 100, random.next() * 100});
            rings.push_back(ring);
            for (size_t i = 0; i < ring.size(); ++i) {
                if (i == 0)
                    prPathMoveTo(path, ring[i]);
                else
                    prPathLineTo(path, ring[i]);
            }
            prPathClose(path);
        }
        PrPathMesh mesh{};
        tessellate(t, path, prAffine2Identity(), PrPathFillRule_NonZero, 0.1f, 1, &mesh);
        uint32_t wrong = 0;
        for (int sy = 0; sy < 50; ++sy) {
            for (int sx = 0; sx < 50; ++sx) {
                PrVec2 p{float(sx) * 2 + 0.3173f, float(sy) * 2 + 0.7331f};
                double nearest = 1e9;
                for (const std::vector<PrVec2>& ring : rings) {
                    for (size_t i = 0; i < ring.size(); ++i)
                        nearest = std::fmin(nearest, distance(ring[i], ring[(i + 1) % ring.size()], p));
                }
                if (nearest < 1.5)
                    continue;
                bool in = winding(rings, p) != 0;
                if (containing(mesh, p) != (in ? 1u : 0u))
                    ++wrong;
            }
        }
        CHECK_MESSAGE(wrong == 0, "round ", round, " wrong ", wrong);
        prPathDestroy(path);
    }
    prPathTessellatorDestroy(t);
}
