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

// Fills the path under the transform, flattened into `polyline`.
void fillPath(PrPathSparseRasterizer* rasterizer, PrPathPolyline& polyline, const PrPath* path, PrAffine2 transform,
        PrPathFillRule rule, PrIRect box, PrPathSparse* out) {
    prPathPolylineClear(&polyline);
    prPathFlatten(path, transform, 0.05f, PrPathFlattening_Balanced, &polyline);
    *out = prPathSparseRasterizerFill(rasterizer, &polyline, rule, box);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

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
