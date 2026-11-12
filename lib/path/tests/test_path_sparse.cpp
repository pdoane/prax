#include "pr/array.h"
#include "pr/float.h"
#include "pr/min_max.h"
#include "pr/path_rasterizer.h"
#include "pr/path_sparse.h"
#include "pr/random.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns a path of closed contours through random points within `size`, some of them curves.
PrPath* randomPath(PrRandom& random, float size) {
    PrPath* path = prPathCreate();
    int contours = 1 + int(prRandomF32(&random) * 3);
    for (int c = 0; c < contours; ++c) {
        prPathMoveTo(path, PrVec2{prRandomF32(&random) * size, prRandomF32(&random) * size});
        int n = 2 + int(prRandomF32(&random) * 8);
        for (int i = 0; i < n; ++i) {
            PrVec2 p{prRandomF32(&random) * size, prRandomF32(&random) * size};
            if (prRandomF32(&random) < 0.3f)
                prPathQuadTo(path, PrVec2{prRandomF32(&random) * size, prRandomF32(&random) * size}, p);
            else
                prPathLineTo(path, p);
        }
        prPathClose(path);
    }
    return path;
}

float pathCoverageFromWinding(float winding, PrPathFillRule rule) {
    float magnitude = prF32Abs(winding);
    if (rule == PrPathFillRule_EvenOdd) {
        magnitude -= 2 * prF32Floor(magnitude / 2);
        return magnitude > 1 ? 2 - magnitude : magnitude;
    }
    return prF32Min(magnitude, 1.0f);
}

// Returns the integral of clamp(u, 0, 1) from 0 to u.
float integral(float u) {
    if (u <= 0)
        return 0;
    return u < 1 ? u * u / 2 : u - 0.5f;
}

// Returns the winding of a pixel in a tile record, as the header defines it.
float tileWinding(const float* record, uint32_t height, float px, uint32_t row) {
    uint32_t count = uint32_t(record[0]);
    float winding = record[1 + row];
    const float* segments = record + 1 + height;
    for (uint32_t i = 0; i < count; ++i) {
        const float* s = segments + i * 4;
        float top = prF32Min(s[1], s[3]);
        float bottom = prF32Max(s[1], s[3]);
        float y0 = prF32Max(top, float(row));
        float y1 = prF32Min(bottom, float(row + 1));
        if (!(y1 > y0))
            continue;
        auto xAt = [&](float y) { return s[0] + (s[2] - s[0]) * (y - s[1]) / (s[3] - s[1]); };
        float xa = xAt(y0);
        float xb = xAt(y1);
        float average;
        if (prF32Abs(xb - xa) < 1e-6f)
            average = prF32Min(prF32Max(px + 1 - (xa + xb) / 2, 0.0f), 1.0f);
        else
            average = (integral(px + 1 - xa) - integral(px + 1 - xb)) / (xb - xa);
        winding += (s[3] > s[1] ? 1 : -1) * (y1 - y0) * average;
    }
    return winding;
}

// Returns the coverage that the spans give each pixel of the box, as bytes. Every pixel is covered at most once.
Array<uint8_t> render(const PrPathSparse& sparse, const PrPathSparseRasterizerOptions& options,
        PrPathFillRule rule, int32_t left, int32_t top, uint32_t width, uint32_t height) {
    Array<uint8_t> pixels(size_t(width) * height);
    Array<uint8_t> covered(size_t(width) * height);
    for (uint32_t i = 0; i < sparse.spanCount; ++i) {
        const PrPathSparseSpan& s = sparse.spans[i];
        REQUIRE(s.origin.x >= left);
        REQUIRE(s.origin.y >= top);
        REQUIRE(s.origin.x + int32_t(s.extent.x) <= left + int32_t(width));
        REQUIRE(s.origin.y + int32_t(s.extent.y) <= top + int32_t(height));
        for (uint32_t y = 0; y < s.extent.y; ++y) {
            for (uint32_t x = 0; x < s.extent.x; ++x) {
                uint32_t row = uint32_t(s.origin.y - top + int32_t(y));
                uint32_t at = row * width + uint32_t(s.origin.x - left + int32_t(x));
                CHECK(covered[at] == 0);
                covered[at] = 1;
                uint8_t value = 255;
                if (s.data != kPrPathSparseSolid && options.kind == PrPathSparseKind_Coverage) {
                    value = sparse.coverage[s.data + y * s.extent.x + x];
                } else if (s.data != kPrPathSparseSolid) {
                    uint32_t tile = x / options.tileExtent.x;
                    const float* record = sparse.tileData + s.data;
                    for (uint32_t k = 0; k < tile; ++k)
                        record += 1 + s.extent.y + uint32_t(record[0]) * 4;
                    float px = float(x - tile * options.tileExtent.x);
                    value = uint8_t(pathCoverageFromWinding(tileWinding(record, s.extent.y, px, y), rule) * 255 + 0.5f);
                }
                pixels[at] = value;
            }
        }
    }
    return pixels;
}

// Fills the path under the transform, flattened into `polyline`.
void fillPath(PrPathSparseRasterizer* rasterizer, PrPathPolyline& polyline, const PrPath* path, PrAffine2 transform,
        PrPathFillRule rule, PrIRect box, PrPathSparse* out) {
    prPathPolylineClear(&polyline);
    prPathFlatten(path, transform, 0.05f, PrPathFlattening_Balanced, &polyline);
    *out = prPathSparseRasterizerFill(rasterizer, &polyline, rule, box);
}

Array<uint8_t> rasterize(const PrPath* path, PrAffine2 transform, PrPathFillRule rule, int32_t left,
        int32_t top, uint32_t width, uint32_t height) {
    Array<uint8_t> pixels(size_t(width) * height);
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPixels mask{pixels.data(), width, {width, height}};
    PrPathPolyline polyline{};
    prPathFlatten(path, transform, 0.05f, PrPathFlattening_Balanced, &polyline);
    prPathRasterizerFill(rasterizer, &polyline, rule, PrPathCoverage_Area, PrIVec2{left, top}, mask);
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
    return pixels;
}

int largestDifference(const Array<uint8_t>& a, const Array<uint8_t>& b) {
    int largest = 0;
    for (uint32_t i = 0; i < a.count(); ++i) {
        int difference = int(a[i]) - int(b[i]);
        largest = max(largest, difference < 0 ? -difference : difference);
    }
    return largest;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("sparse spans cover each pixel as the rasterizer does") {
    PrPathSparseRasterizerOptions kinds[] = {
        {PrPathSparseKind_Coverage, {4, 4}},
        {PrPathSparseKind_Segments, {16, 16}},
        {PrPathSparseKind_Segments, {8, 4}},
        {PrPathSparseKind_Coverage, {16, 2}},
    };
    PrRandom random = {.state = 2024};
    for (const PrPathSparseRasterizerOptions& options : kinds) {
        PrPathSparseRasterizer* rasterizer = prPathSparseRasterizerCreate(&options);
        PrPathPolyline polyline{};
        for (int trial = 0; trial < 60; ++trial) {
            PrPath* path = randomPath(random, 120);
            PrPathFillRule rule = trial % 2 == 0 ? PrPathFillRule_NonZero : PrPathFillRule_EvenOdd;
            PrAffine2 transform = prAffine2Translate(PrVec2{prRandomF32(&random) * 3 - 1.5f, prRandomF32(&random) * 3 - 1.5f});
            // A box that cuts the path on some sides.
            int32_t left = int32_t(prRandomF32(&random) * 30) - 10;
            int32_t top = int32_t(prRandomF32(&random) * 30) - 10;
            uint32_t width = 60 + uint32_t(prRandomF32(&random) * 80);
            uint32_t height = 60 + uint32_t(prRandomF32(&random) * 80);
            PrIRect box{left, top, left + int32_t(width), top + int32_t(height)};
            PrPathSparse sparse{};
            fillPath(rasterizer, polyline, path, transform, rule, box, &sparse);
            Array<uint8_t> drawn = render(sparse, options, rule, left, top, width, height);
            Array<uint8_t> expected = rasterize(path, transform, rule, left, top, width, height);
            INFO("kind " << int(options.kind) << " tile " << options.tileExtent.x << "x" << options.tileExtent.y <<
                " trial " << trial);
            CHECK(largestDifference(drawn, expected) <= 2);
            prPathDestroy(path);
        }
        prPathPolylineReset(&polyline);
        prPathSparseRasterizerDestroy(rasterizer);
    }
}

TEST_CASE("sparse spans merge a rectangle's interior into one solid span") {
    PrPathSparseRasterizerOptions options{PrPathSparseKind_Coverage, {4, 4}};
    PrPathSparseRasterizer* rasterizer = prPathSparseRasterizerCreate(&options);
    PrPathPolyline polyline{};
    PrPath* rect = prPathCreate();
    prPathAddRect(rect, PrRect{10.5f, 10.5f, 90.5f, 70.5f});
    PrPathSparse sparse{};
    fillPath(rasterizer, polyline, rect, prAffine2Identity(), PrPathFillRule_NonZero, PrIRect{0, 0, 100, 100}, &sparse);
    uint32_t solid = 0;
    uint32_t solidArea = 0;
    for (uint32_t i = 0; i < sparse.spanCount; ++i) {
        if (sparse.spans[i].data == kPrPathSparseSolid) {
            ++solid;
            solidArea += sparse.spans[i].extent.x * sparse.spans[i].extent.y;
        }
    }
    // The tiles within the edges of the rectangle, from 12 to 88 across and 12 to 68 down.
    CHECK(solid == 1);
    CHECK(solidArea == 76 * 56);
    // The edge spans hold only the tiles along the edges.
    CHECK(sparse.coverageCount < 100 * 100 / 4);
    prPathDestroy(rect);
    prPathPolylineReset(&polyline);
    prPathSparseRasterizerDestroy(rasterizer);
}

TEST_CASE("sparse fills of an empty box or path produce nothing") {
    PrPathSparseRasterizerOptions options{PrPathSparseKind_Segments, {16, 16}};
    PrPathSparseRasterizer* rasterizer = prPathSparseRasterizerCreate(&options);
    PrPathPolyline polyline{};
    PrPath* path = prPathCreate();
    PrPathSparse sparse{};
    fillPath(rasterizer, polyline, path, prAffine2Identity(), PrPathFillRule_NonZero, PrIRect{0, 0, 64, 64}, &sparse);
    CHECK(sparse.spanCount == 0);
    prPathAddRect(path, PrRect{0, 0, 10, 10});
    fillPath(rasterizer, polyline, path, prAffine2Identity(), PrPathFillRule_NonZero, PrIRect{5, 5, 5, 20}, &sparse);
    CHECK(sparse.spanCount == 0);
    prPathDestroy(path);
    prPathPolylineReset(&polyline);
    prPathSparseRasterizerDestroy(rasterizer);
}

TEST_CASE("sparse fills of points or transforms that are not finite, or of boxes out of range, do not fail") {
    const PrPathSparseKind kinds[] = {PrPathSparseKind_Coverage, PrPathSparseKind_Segments};
    for (PrPathSparseKind kind : kinds) {
        PrPathSparseRasterizerOptions options{kind, {16, 4}};
        PrPathSparseRasterizer* rasterizer = prPathSparseRasterizerCreate(&options);
        PrPathPolyline polyline{};
        PrPathSparse sparse{};
        const float bads[] = {kPrF32Infinity, -kPrF32Infinity, kPrF32Nan, 1e30f};
        for (float bad : bads) {
            PrPath* path = prPathCreate();
            prPathMoveTo(path, PrVec2{0, 0});
            prPathLineTo(path, PrVec2{bad, 5});
            prPathLineTo(path, PrVec2{5, 10});
            prPathClose(path);
            fillPath(rasterizer, polyline, path, prAffine2Identity(), PrPathFillRule_NonZero, PrIRect{0, 0, 64, 64}, &sparse);
            PrPath* square = prPathCreate();
            prPathAddRect(square, PrRect{2, 2, 8, 8});
            fillPath(rasterizer, polyline, square, PrAffine2{bad, 0, 0, 1, 0, 0}, PrPathFillRule_NonZero,
                PrIRect{0, 0, 64, 64}, &sparse);
            fillPath(rasterizer, polyline, square, prAffine2Identity(), PrPathFillRule_NonZero,
                PrIRect{0, 0, INT32_MAX, 64}, &sparse);
            prPathDestroy(square);
            prPathDestroy(path);
        }
        prPathPolylineReset(&polyline);
        prPathSparseRasterizerDestroy(rasterizer);
    }
}
