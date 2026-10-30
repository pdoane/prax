// Sparse coverage of filled polylines: spans of pixels that the edges of a polyline cross, and spans of pixels that it
// covers wholly.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The data that an edge span carries for its pixels.
enum {
    // A byte for each pixel of the span, by rows, as prPathRasterizerFill computes it.
    PrPathSparseKind_Coverage,

    // A record of floats for each tile of the span. The record holds the number of segments, then the winding at the
    // left edge of each row of the tile, then the segments as x0, y0, x1, y1 relative to the top left of the tile,
    // clipped to the tile.
    //
    // A segment going down adds to the winding to its right, and a segment going up subtracts from it. The winding of a
    // pixel is the winding at the left edge of its row plus a term for each segment. The term is the signed height of
    // the segment within the row, times the average of clamp(px + 1 - x, 0, 1) over the length of the segment in the
    // row, where px is the left edge of the pixel.
    PrPathSparseKind_Segments,
};
typedef uint8_t PrPathSparseKind;

// The `data` of a solid span.
static const uint32_t kPrPathSparseSolid = 0xffffffffu;

// A rectangle of pixels in the result of a fill. `data` is kPrPathSparseSolid for a solid span. For an edge span, it is
// the index of the first coverage byte of the span, or of the first float of its first tile.
typedef struct PrPathSparseSpan {
    PrIVec2 origin;
    PrUVec2 extent;
    uint32_t data;
} PrPathSparseSpan;

// The spans of a fill, and the data of its edge spans.
//
// A fill divides the pixels within its box into tiles, in rows of tiles from the top left of the box. Tiles that an
// edge touches form edge spans of adjacent tiles in a row. The pixels between two edge spans in a row have one winding.
// Those that the polyline covers wholly form solid spans, which merge down the rows while they line up.
//
// The tiles of a Segments span divide its width from its left. The last tile is narrower if the box ends within it. The
// records of the tiles follow each other in `tileData`.
typedef struct PrPathSparse {
    const PrPathSparseSpan* PR_NULLABLE spans;
    uint32_t spanCount;
    const uint8_t* PR_NULLABLE coverage;
    uint32_t coverageCount;
    const float* PR_NULLABLE tileData;
    uint32_t tileDataCount;
} PrPathSparse;

// The largest side of a tile, in pixels.
enum { kPrPathSparseMaxTileSize = 256 };

// Options for a rasterizer: the data that its edge spans carry, and the size of its tiles.
typedef struct PrPathSparseRasterizerOptions {
    PrPathSparseKind kind;
    PrUVec2 tileExtent;         // in pixels, each from 1 to kPrPathSparseMaxTileSize
} PrPathSparseRasterizerOptions;

// Returns the default options: Coverage, in tiles of 4 by 4 pixels.
PrPathSparseRasterizerOptions prPathSparseRasterizerOptionsDefault(void);

// Scratch memory for filling, and the storage for the spans of the last fill.
typedef struct PrPathSparseRasterizer PrPathSparseRasterizer;

// Creates a rasterizer. Null `options` means the defaults.
PrPathSparseRasterizer* prPathSparseRasterizerCreate(const PrPathSparseRasterizerOptions* PR_NULLABLE options);

// Frees the rasterizer.
void prPathSparseRasterizerDestroy(PrPathSparseRasterizer* PR_NULLABLE rasterizer);

// The largest distance from the origin at which an edge of a filled box may lie, in pixels.
enum { kPrPathSparseMaxCoordinate = 1 << 30 };

// Fills the region that `polyline` encloses by `rule` within `box`. The points of `polyline` are in pixels, and every
// contour counts as closed. A `box` with an edge farther than kPrPathSparseMaxCoordinate from the origin fills nothing.
// The result lives in the storage of the rasterizer, and stays valid until the next fill.
PrPathSparse prPathSparseRasterizerFill(PrPathSparseRasterizer* rasterizer, const PrPathPolyline* polyline,
    PrPathFillRule rule, PrIRect box);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
