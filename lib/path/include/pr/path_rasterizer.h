// Rasterizing polylines to coverage.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/pixels.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the coverage of a pixel is computed.
enum {
    PrPathCoverage_Area,        // the fraction of the pixel's area inside the polyline
    PrPathCoverage_Center,      // all or none, depending on whether the pixel's center is inside
};
typedef uint8_t PrPathCoverage;

// Scratch memory that rasterizing reuses between fills.
typedef struct PrPathRasterizer PrPathRasterizer;

// Creates and frees a rasterizer.
PrPathRasterizer* prPathRasterizerCreate(void);
void prPathRasterizerDestroy(PrPathRasterizer* PR_NULLABLE rasterizer);

// Writes to every byte of `mask` the fraction of its pixel that `polyline` covers, from 0 for none to 255 for all.
// Pixel (0, 0) of `mask` is the pixel at `origin`. The points of `polyline` are in pixels, and every contour counts as
// closed. Curves flattened with PrPathFlattening_Balanced keep their area.
void prPathRasterizerFill(PrPathRasterizer* rasterizer, const PrPathPolyline* polyline, PrPathFillRule rule,
    PrPathCoverage coverage, PrIVec2 origin, PrPixels mask);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
