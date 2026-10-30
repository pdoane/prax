#include "pr/float.h"
#include "pr/path_rasterizer.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pr_impl {

namespace {

struct Fill {
    std::vector<uint8_t> coverage;
    PrPixels mask;
};

// Fills `path` into a mask of the given size at (left, top), under the transform.
Fill fill(const PrPath* path, uint32_t width, uint32_t height, PrPathFillRule rule = PrPathFillRule_NonZero,
        bool antialias = true, PrAffine2 transform = prAffine2Identity(), int32_t left = 0, int32_t top = 0) {
    Fill result;
    result.coverage.assign(size_t(width) * height, 0xaa);
    result.mask = PrPixels{result.coverage.data(), width, {width, height}};
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPathPolyline polyline{};
    prPathFlatten(path, transform, 0.05f, PrPathFlattening_Balanced, &polyline);
    prPathRasterizerFill(rasterizer, &polyline, rule, antialias ? PrPathCoverage_Area : PrPathCoverage_Center,
        PrIVec2{left, top}, result.mask);
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
    return result;
}

// Returns the coverage as rows of values, one row per line.
std::string rows(const Fill& f) {
    std::string text;
    for (uint32_t y = 0; y < f.mask.extent.y; ++y) {
        for (uint32_t x = 0; x < f.mask.extent.x; ++x) {
            if (x != 0)
                text += ' ';
            text += std::to_string(f.coverage[y * f.mask.extent.x + x]);
        }
        text += '\n';
    }
    return text;
}

PrPath* rectPath(PrRect rect) {
    PrPath* path = prPathCreate();
    prPathAddRect(path, rect);
    return path;
}

// Returns the area that a path covers within 64 by 64, computed from its coverage at eight times that resolution.
double filledArea(const PrPath* path, PrPathFillRule rule = PrPathFillRule_NonZero) {
    constexpr uint32_t kScale = 8;
    constexpr uint32_t kSize = 64 * kScale;
    std::vector<uint8_t> coverage(kSize * kSize);
    PrPixels mask{coverage.data(), kSize, {kSize, kSize}};
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPathPolyline polyline{};
    PrAffine2 scale = prAffine2Scale(PrVec2{float(kScale), float(kScale)});
    prPathFlatten(path, scale, 0.05f, PrPathFlattening_Balanced, &polyline);
    prPathRasterizerFill(rasterizer, &polyline, rule, PrPathCoverage_Area, PrIVec2{0, 0}, mask);
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
    double sum = 0;
    for (uint8_t c : coverage)
        sum += c / 255.0;
    return sum / (kScale * kScale);
}

constexpr double kPiD = 3.14159265358979;

// Returns the area of the convex polygon `points` inside the pixel at `x`, `y`: the polygon clipped to each of the
// pixel's sides in turn, then measured by the shoelace formula.
double areaInPixel(std::vector<std::pair<double, double>> points, uint32_t x, uint32_t y) {
    // Each side keeps the points whose coordinate `axis` is on its side of `edge`: above it if `above`, else below.
    struct Side {
        int axis;
        double edge;
        bool above;
    };
    const Side sides[] = {
        {0, double(x), true}, {0, double(x + 1), false}, {1, double(y), true},
        {1, double(y + 1), false},
    };
    for (const Side& side : sides) {
        auto coordinate = [&](const std::pair<double, double>& p) { return side.axis == 0 ? p.first : p.second; };
        auto keeps = [&](const std::pair<double, double>& p) {
            return side.above ? coordinate(p) >= side.edge : coordinate(p) <= side.edge;
        };
        std::vector<std::pair<double, double>> kept;
        for (size_t i = 0; i < points.size(); ++i) {
            const std::pair<double, double>& a = points[i];
            const std::pair<double, double>& b = points[(i + 1) % points.size()];
            if (keeps(a))
                kept.push_back(a);
            if (keeps(a) != keeps(b)) {
                double t = (side.edge - coordinate(a)) / (coordinate(b) - coordinate(a));
                kept.push_back({a.first + t * (b.first - a.first), a.second + t * (b.second - a.second)});
            }
        }
        points = kept;
    }
    double twice = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const std::pair<double, double>& a = points[i];
        const std::pair<double, double>& b = points[(i + 1) % points.size()];
        twice += a.first * b.second - b.first * a.second;
    }
    return std::abs(twice) / 2;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a pixel-aligned rectangle covers its pixels whole") {
    PrPath* path = rectPath(PrRect{1, 1, 3, 2});
    CHECK(rows(fill(path, 4, 3)) == "0 0 0 0\n0 255 255 0\n0 0 0 0\n");
    CHECK(rows(fill(path, 4, 3, PrPathFillRule_NonZero, false)) == "0 0 0 0\n0 255 255 0\n0 0 0 0\n");
    prPathDestroy(path);
}

TEST_CASE("antialiased coverage is area, and aliased coverage is the pixel center") {
    PrPath* path = rectPath(PrRect{0.5f, 0, 1.5f, 1});
    CHECK(rows(fill(path, 3, 1)) == "128 128 0\n");
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_NonZero, false)) == "255 0 0\n");
    prPathDestroy(path);

    PrPath* quarter = rectPath(PrRect{0.5f, 0.5f, 1, 1});
    CHECK(rows(fill(quarter, 1, 1)) == "64\n");
    prPathDestroy(quarter);
}

TEST_CASE("a diagonal edge covers each pixel by the area under it") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{0, 0});
    prPathLineTo(path, PrVec2{2, 2});
    prPathLineTo(path, PrVec2{0, 2});
    prPathClose(path);
    CHECK(rows(fill(path, 2, 2)) == "128 0\n255 128\n");
    prPathDestroy(path);
}

TEST_CASE("the fill rule decides a region wound twice") {
    PrPath* path = rectPath(PrRect{0, 0, 3, 1});
    prPathAddRect(path, PrRect{1, 0, 2, 1});
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_NonZero)) == "255 255 255\n");
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_EvenOdd)) == "255 0 255\n");
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_EvenOdd, false)) == "255 0 255\n");
    prPathDestroy(path);
}

TEST_CASE("a region wound in opposite directions cancels under either rule") {
    PrPath* path = rectPath(PrRect{0, 0, 3, 1});
    prPathMoveTo(path, PrVec2{1, 0});
    prPathLineTo(path, PrVec2{1, 1});
    prPathLineTo(path, PrVec2{2, 1});
    prPathLineTo(path, PrVec2{2, 0});
    prPathClose(path);
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_NonZero)) == "255 0 255\n");
    prPathDestroy(path);
}

TEST_CASE("a path beyond the mask's sides covers what the mask holds") {
    PrPath* path = rectPath(PrRect{-5, 0, 10, 1});
    CHECK(rows(fill(path, 3, 1)) == "255 255 255\n");
    CHECK(rows(fill(path, 3, 1, PrPathFillRule_NonZero, false)) == "255 255 255\n");
    prPathDestroy(path);

    PrPath* slanted = prPathCreate();
    prPathMoveTo(slanted, PrVec2{-2, 0});
    prPathLineTo(slanted, PrVec2{2, 0});
    prPathLineTo(slanted, PrVec2{0, 2});
    prPathLineTo(slanted, PrVec2{-2, 2});
    prPathClose(slanted);
    // Right of x = 0, the edge from (2, 0) to (0, 2) leaves triangles of half a pixel.
    CHECK(rows(fill(slanted, 2, 2)) == "255 128\n128 0\n");
    prPathDestroy(slanted);
}

TEST_CASE("an edge that nearly lies along a row stays within the mask") {
    // The miter that the stroker makes for a square corner. Its tip lands within a rounding error of the origin.
    PrPath* corner = prPathCreate();
    prPathMoveTo(corner, PrVec2{1, 1});
    prPathLineTo(corner, PrVec2{0, 1});
    prPathLineTo(corner, PrVec2{5.96046e-08f, 5.96046e-08f});
    prPathLineTo(corner, PrVec2{1, 0});
    prPathClose(corner);
    CHECK(rows(fill(corner, 2, 2)) == "255 0\n0 0\n");
    prPathDestroy(corner);
}

TEST_CASE("the mask's origin and the transform place the path") {
    PrPath* path = rectPath(PrRect{0, 0, 1, 1});
    Fill f = fill(path, 2, 2, PrPathFillRule_NonZero, true, prAffine2Translate(PrVec2{11, 21}), 10, 20);
    CHECK(rows(f) == "0 0\n0 255\n");
    Fill scaled = fill(path, 2, 2, PrPathFillRule_NonZero, true, prAffine2Scale(PrVec2{2, 2}));
    CHECK(rows(scaled) == "255 255\n255 255\n");
    prPathDestroy(path);
}

TEST_CASE("a circle's coverage sums to its area") {
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, PrVec2{22, 22}, PrVec2{20, 20}, 0);
    Fill f = fill(path, 44, 44);
    double sum = 0;
    for (uint8_t c : f.coverage)
        sum += c / 255.0;
    double area = 3.14159265358979 * 400;
    CHECK(sum > area * 0.995);
    CHECK(sum < area * 1.005);
    CHECK(f.coverage[22 * 44 + 22] == 255);
    CHECK(f.coverage[0] == 0);
    prPathDestroy(path);
}

TEST_CASE("an empty path and an empty mask write nothing they should not") {
    PrPath* path = prPathCreate();
    CHECK(rows(fill(path, 2, 1)) == "0 0\n");
    prPathDestroy(path);
}

TEST_CASE("shaped corners cut the rectangle by their curves") {
    PrRect rect{4, 4, 60, 44};
    double whole = 56 * 40;
    double r = 12;
    auto area = [&](float shape) {
        PrCorner c{{12, 12}, shape};
        PrCornerRect cornered{rect, {c, c, c, c}};
        PrPath* path = prPathCreate();
        prPathAddCornerRect(path, cornered);
        double result = filledArea(path);
        prPathDestroy(path);
        return result;
    };
    CHECK(area(1) == doctest::Approx(whole - 4 * (r * r - kPiD * r * r / 4)).epsilon(0.001));
    CHECK(area(0) == doctest::Approx(whole - 4 * (r * r / 2)).epsilon(0.001));
    CHECK(area(kInfinityF32) == doctest::Approx(whole).epsilon(0.001));
    CHECK(area(-1) == doctest::Approx(whole - 4 * (kPiD * r * r / 4)).epsilon(0.001));
    CHECK(area(-kInfinityF32) == doctest::Approx(whole - 4 * r * r).epsilon(0.001));
    // A quarter of a squircle covers 0.92704 of its box.
    CHECK(area(2) == doctest::Approx(whole - 4 * r * r * (1 - 0.92704)).epsilon(0.001));
}

TEST_CASE("sectors are slices, rings and annular pieces") {
    PrPath* path = prPathCreate();
    prPathAddSector(path, PrVec2{32, 32}, 0, 20, 0, kPiF32 / 2);
    CHECK(filledArea(path) == doctest::Approx(kPiD * 400 / 4).epsilon(0.002));
    prPathClear(path);
    prPathAddSector(path, PrVec2{32, 32}, 10, 20, 1, -kPiF32);
    CHECK(filledArea(path) == doctest::Approx(kPiD * 300 / 2).epsilon(0.002));
    prPathClear(path);
    prPathAddSector(path, PrVec2{32, 32}, 10, 20, 0, 7);
    CHECK(filledArea(path) == doctest::Approx(kPiD * 300).epsilon(0.002));
    CHECK(filledArea(path, PrPathFillRule_EvenOdd) == doctest::Approx(kPiD * 300).epsilon(0.002));
    prPathDestroy(path);
}

TEST_CASE("a capsule is every point within its radius of the segment") {
    PrPath* path = prPathCreate();
    prPathAddCapsule(path, PrVec2{15, 20}, PrVec2{45, 40}, 8);
    double length = std::sqrt(30.0 * 30 + 20 * 20);
    CHECK(filledArea(path) == doctest::Approx(16 * length + kPiD * 64).epsilon(0.002));
    prPathClear(path);
    prPathAddCapsule(path, PrVec2{30, 30}, PrVec2{30, 30}, 8);
    CHECK(filledArea(path) == doctest::Approx(kPiD * 64).epsilon(0.002));
    prPathDestroy(path);
}

TEST_CASE("an arc lines from an open contour to its start") {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{10, 32});
    prPathEllipticArcTo(path, PrVec2{32, 32}, PrVec2{20, 10}, 0, kPiF32, kPiF32);
    prPathClose(path);
    CHECK(filledArea(path) == doctest::Approx(kPiD * 200 / 2).epsilon(0.002));
    prPathDestroy(path);
}

TEST_CASE("coverage along a curve is the area inside the pixel") {
    // Circles, and the exact area of each pixel inside them, sampled finely.
    constexpr float kCenter = 12;
    constexpr int kSamples = 128;
    for (float radius : {8.0f, 2.5f}) {
        PrPath* path = prPathCreate();
        prPathAddEllipse(path, PrVec2{kCenter, kCenter}, PrVec2{radius, radius}, 0);
        Fill f = fill(path, 24, 24);
        int worst = 0;
        for (uint32_t y = 0; y < 24; ++y) {
            for (uint32_t x = 0; x < 24; ++x) {
                int inside = 0;
                for (int j = 0; j < kSamples; ++j) {
                    for (int i = 0; i < kSamples; ++i) {
                        double px = x + (i + 0.5) / kSamples - double(kCenter);
                        double py = y + (j + 0.5) / kSamples - double(kCenter);
                        inside += px * px + py * py <= double(radius * radius);
                    }
                }
                int exact = int(std::lround(255.0 * inside / (kSamples * kSamples)));
                int error = std::abs(int(f.coverage[y * 24 + x]) - exact);
                worst = error > worst ? error : worst;
            }
        }
        CHECK(worst <= 2);
        prPathDestroy(path);
    }
}

TEST_CASE("coverage along straight edges is the area inside the pixel") {
    // A square rotated by a third of a right angle, and a polygon of many sides, against the exact area of each pixel
    // inside them.
    for (uint32_t sides : {4u, 64u}) {
        PrPath* path = prPathCreate();
        std::vector<std::pair<double, double>> points;
        for (uint32_t i = 0; i < sides; ++i) {
            double angle = 0.5 + 2 * kPiD * i / sides;
            PrVec2 p{float(12 + 8 * std::cos(angle)), float(12 + 8 * std::sin(angle))};
            points.push_back({double(p.x), double(p.y)});
            if (i == 0)
                prPathMoveTo(path, p);
            else
                prPathLineTo(path, p);
        }
        prPathClose(path);
        Fill f = fill(path, 24, 24);
        int worst = 0;
        for (uint32_t y = 0; y < 24; ++y) {
            for (uint32_t x = 0; x < 24; ++x) {
                int exact = int(std::lround(255.0 * areaInPixel(points, x, y)));
                int error = std::abs(int(f.coverage[y * 24 + x]) - exact);
                worst = error > worst ? error : worst;
            }
        }
        CHECK(worst <= 1);
        prPathDestroy(path);
    }
}

TEST_CASE("a path with points or a transform that are not finite fills without failing") {
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    std::vector<uint8_t> coverage(16 * 16, 0xab);
    PrPixels mask{coverage.data(), 16, {16, 16}};
    PrPathPolyline polyline{};
    for (float bad : {kInfinityF32, -kInfinityF32, kNanF32, 1e30f}) {
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{0, 0});
        prPathLineTo(path, PrVec2{bad, 5});
        prPathCubicTo(path, PrVec2{bad, bad}, PrVec2{2, 3}, PrVec2{5, 10});
        prPathClose(path);
        prPathPolylineClear(&polyline);
        prPathFlatten(path, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &polyline);
        prPathRasterizerFill(rasterizer, &polyline, PrPathFillRule_NonZero, PrPathCoverage_Area, PrIVec2{0, 0}, mask);
        prPathRasterizerFill(rasterizer, &polyline, PrPathFillRule_EvenOdd, PrPathCoverage_Center, PrIVec2{0, 0}, mask);
        PrPath* circle = prPathCreate();
        prPathAddEllipse(circle, PrVec2{5, 5}, PrVec2{3, 3}, 0);
        prPathPolylineClear(&polyline);
        prPathFlatten(circle, PrAffine2{bad, 0, 0, 1, 0, 0}, 0.05f, PrPathFlattening_Balanced, &polyline);
        prPathRasterizerFill(rasterizer, &polyline, PrPathFillRule_NonZero, PrPathCoverage_Area, PrIVec2{0, 0}, mask);
        prPathDestroy(circle);
        prPathDestroy(path);
    }
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
}
