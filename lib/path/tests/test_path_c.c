#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/path_rasterizer.h"
#include "pr/path_sparse.h"
#include "pr/path_tessellator.h"
#include "pr/vec.h"

int prTestPathCApi(void);

// The public headers compiled as C. A path is built, filled into a caller's mask, and tessellated.
int prTestPathCApi(void) {
    PrPath* path = prPathCreate();
    PrRect rect = {0, 0, 2, 2};
    prPathAddRect(path, rect);
    uint8_t coverage[4] = {0};
    PrPixels mask = {coverage, 2, {2, 2}};
    PrPathRasterizer* rasterizer = prPathRasterizerCreate();
    PrPathPolyline outline = {0};
    prPathFlatten(path, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &outline);
    prPathRasterizerFill(rasterizer, &outline, PrPathFillRule_NonZero, PrPathCoverage_Area, (PrIVec2){0, 0}, mask);
    prPathPolylineReset(&outline);
    int result = coverage[0] != 255 || coverage[3] != 255 || prPathVerbCount(path) != 5;
    PrPathTessellator* tessellator = prPathTessellatorCreate();
    PrPathPolyline tessellated = {0};
    prPathFlatten(path, prAffine2Identity(), 0.25f, PrPathFlattening_Balanced, &tessellated);
    PrPathMesh mesh = prPathTessellatorFill(tessellator, &tessellated, PrPathFillRule_NonZero, 0);
    prPathPolylineReset(&tessellated);
    result |= mesh.indexCount != 6;
    prPathTessellatorDestroy(tessellator);
    PrPathSparseRasterizerOptions sparseOptions = {PrPathSparseKind_Coverage, {4, 4}};
    PrPathSparseRasterizer* sparseRasterizer = prPathSparseRasterizerCreate(&sparseOptions);
    PrIRect box = {0, 0, 4, 4};
    PrPathPolyline filled = {0};
    prPathFlatten(path, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &filled);
    PrPathSparse sparse = prPathSparseRasterizerFill(sparseRasterizer, &filled, PrPathFillRule_NonZero, box);
    prPathPolylineReset(&filled);
    result |= sparse.spanCount != 1 || sparse.coverageCount != 16;
    prPathSparseRasterizerDestroy(sparseRasterizer);
    PrPathPolyline polyline = {0};
    prPathFlatten(path, prAffine2Identity(), 0.25f, PrPathFlattening_Inside, &polyline);
    result |= polyline.contourCount != 1 || polyline.pointCount != 4 || polyline.closed[0] != 1;
    prPathPolylineReset(&polyline);
    prPathRasterizerDestroy(rasterizer);
    prPathDestroy(path);
    prPathDestroy(NULL);
    return result;
}
