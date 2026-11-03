// Sparse coverage of filled polylines.
//
// Edges are moved into the coordinates of the box and cut into pieces, one for each row of tiles that an edge crosses.
// Each piece refers to the tiles that it touches. Sorted by row and tile, the references give the edge spans of each
// row. Parts of edges left of the box become the winding that each pixel row starts with. Along a row of tiles, a
// running winding for each pixel row carries the winding that the spans so far leave behind them into the pixels
// between spans.

#include "pr/path_sparse.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/vec.h"

#include "path/src/cells.h"
#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

// Returns true if an edge of a box is a pixel coordinate far enough inside the range of int32_t that the width and
// height of the box are too.
bool inRange(int32_t edge) {
    return edge >= -kPrPathSparseMaxCoordinate && edge <= kPrPathSparseMaxCoordinate;
}

uint32_t minU32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

uint32_t maxU32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

// The coverage at or above which a pixel is wholly covered, and at or below which it is empty, after rounding to a
// byte.
constexpr float kFull = 254.5f / 255;
constexpr float kEmpty = 0.5f / 255;

// A part of an edge within one row of tiles, in the coordinates of the box, from top to bottom. A horizontal piece has
// no direction. It only makes its tiles edge tiles.
struct Piece {
    float x0;
    float y0;
    float x1;
    float y1;
    float direction;
    uint32_t stamp;             // the last span that accumulated the piece
};

// A tile that a piece touches.
struct Ref {
    uint32_t row;
    uint32_t tile;
    uint32_t piece;
};

struct PathSparseRasterizer {
    PrPathSparseRasterizerOptions options;

    // The box of the fill underway, and its tiles.
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
    uint32_t tilesX;
    uint32_t tilesY;

    uint32_t stamp;
    RawArray<Piece> pieces;
    RawArray<Ref> refs;
    RawArray<float> leftWinding;   // for each pixel row, the winding of the edges left of the box
    RawArray<float> cells;
    RawArray<float> carry;         // for each pixel row of the row of tiles, the winding at the current column
    RawArray<uint32_t> open;       // for each pixel column, the solid spans ending atop the tiles
    RawArray<uint32_t> nextOpen;
    RawArray<PrPathSparseSpan> spans;
    RawArray<uint8_t> coverage;
    RawArray<float> tiles;
};

uint32_t tileOf(const PathSparseRasterizer* f, float x) {
    float tile = prF32Floor(x / float(f->options.tileExtent.x));
    if (!(tile > 0))
        return 0;
    return tile >= float(f->tilesX) ? f->tilesX - 1 : uint32_t(tile);
}

void addRefs(PathSparseRasterizer* f, uint32_t row, float xa, float xb, uint32_t piece) {
    uint32_t first = tileOf(f, prF32Min(xa, xb));
    uint32_t last = tileOf(f, prF32Max(xa, xb));
    for (uint32_t tile = first; tile <= last; ++tile)
        f->refs.push(Ref{row, tile, piece});
}

// Adds a part of an edge at or left of the left side of the box to the winding that its rows start with.
void addLeft(PathSparseRasterizer* f, float y0, float y1, float direction) {
    for (uint32_t y = uint32_t(y0); float(y) < y1 && y < f->height; ++y) {
        float dy = prF32Min(float(y + 1), y1) - prF32Max(float(y), y0);
        f->leftWinding.data[y] += dy * direction;
    }
}

// Adds a part of an edge that lies horizontally within the box, from top to bottom, as a piece in each row of tiles
// that it crosses.
void addInside(PathSparseRasterizer* f, PathCellEdge e) {
    float th = float(f->options.tileExtent.y);
    float slope = (e.x1 - e.x0) / (e.y1 - e.y0);
    for (uint32_t row = uint32_t(e.y0 / th); row < f->tilesY && float(row) * th < e.y1; ++row) {
        float ya = prF32Max(e.y0, float(row) * th);
        float yb = prF32Min(e.y1, float(row + 1) * th);
        if (!(yb > ya))
            continue;
        float xa = ya == e.y0 ? e.x0 : e.x0 + (ya - e.y0) * slope;
        float xb = yb == e.y1 ? e.x1 : e.x0 + (yb - e.y0) * slope;
        xa = prF32Min(prF32Max(xa, 0), float(f->width));
        xb = prF32Min(prF32Max(xb, 0), float(f->width));
        uint32_t piece = f->pieces.count;
        f->pieces.push(Piece{xa, ya, xb, yb, e.direction, 0});
        addRefs(f, row, xa, xb, piece);
    }
}

// Adds a horizontal edge, which covers no area but divides the pixels above it from those below.
void addHorizontal(PathSparseRasterizer* f, float x0, float x1, float y) {
    float th = float(f->options.tileExtent.y);
    if (!(y > 0 && y < float(f->height)) || y == prF32Floor(y / th) * th)
        return;
    float xa = prF32Max(prF32Min(x0, x1), 0);
    float xb = prF32Min(prF32Max(x0, x1), float(f->width));
    if (!(xb > xa))
        return;
    uint32_t row = uint32_t(y / th);
    uint32_t piece = f->pieces.count;
    f->pieces.push(Piece{xa, y, xb, y, 0, 0});
    addRefs(f, row, xa, xb, piece);
}

void addEdge(PathSparseRasterizer* f, PrVec2 from, PrVec2 to) {
    if (!prF32IsFinite(from.x) || !prF32IsFinite(from.y) || !prF32IsFinite(to.x) || !prF32IsFinite(to.y))
        return;
    if (from.y == to.y) {
        addHorizontal(f, from.x, to.x, from.y);
        return;
    }
    PathCellEdge e =
        from.y < to.y ? PathCellEdge{from.x, from.y, to.x, to.y, 1} : PathCellEdge{to.x, to.y, from.x, from.y, -1};
    float height = float(f->height);
    if (e.y1 <= 0 || e.y0 >= height)
        return;
    // Within the box vertically.
    float slope = (e.x1 - e.x0) / (e.y1 - e.y0);
    if (e.y0 < 0) {
        e.x0 += (0 - e.y0) * slope;
        e.y0 = 0;
    }
    if (e.y1 > height) {
        e.x1 += (height - e.y1) * slope;
        e.y1 = height;
    }
    // Split the edge at the points at which it crosses the left and right sides of the box.
    float right = float(f->width);
    float cuts[4] = {0, 0, 0, 1};
    uint32_t cutCount = 1;
    float sides[2] = {0, right};
    for (float side : sides) {
        if ((e.x0 < side) != (e.x1 < side)) {
            float t = (side - e.x0) / (e.x1 - e.x0);
            if (t > 0 && t < 1)
                cuts[cutCount++] = t;
        }
    }
    if (cutCount == 3 && cuts[2] < cuts[1]) {
        float swap = cuts[1];
        cuts[1] = cuts[2];
        cuts[2] = swap;
    }
    cuts[cutCount] = 1;
    for (uint32_t i = 0; i < cutCount; ++i) {
        float t0 = cuts[i];
        float t1 = cuts[i + 1];
        PathCellEdge piece{
            t0 == 0 ? e.x0 : e.x0 + (e.x1 - e.x0) * t0, t0 == 0 ? e.y0 : e.y0 + (e.y1 - e.y0) * t0,
            t1 == 1 ? e.x1 : e.x0 + (e.x1 - e.x0) * t1, t1 == 1 ? e.y1 : e.y0 + (e.y1 - e.y0) * t1,
            e.direction,
        };
        if (!(piece.y1 > piece.y0))
            continue;
        float middle = (piece.x0 + piece.x1) / 2;
        if (middle <= 0)
            addLeft(f, piece.y0, piece.y1, piece.direction);
        else if (middle < right)
            addInside(f, piece);
    }
}

// Adds the edges of the polyline in the coordinates of the box, with every contour closed.
void addEdges(PathSparseRasterizer* f, const PrPathPolyline& polyline) {
    auto at = [&](uint32_t i) {
        return PrVec2{polyline.points[i].x - float(f->left), polyline.points[i].y - float(f->top)};
    };
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        for (uint32_t i = start; i < end; ++i)
            addEdge(f, at(i), at(i + 1 < end ? i + 1 : start));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spans

// Adds a solid span, extending the one above it if they line up.
void addSolid(PathSparseRasterizer* f, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t& openAt) {
    while (openAt < f->open.count && f->spans.data[f->open.data[openAt]].origin.x < f->left + int32_t(x))
        ++openAt;
    if (openAt < f->open.count) {
        PrPathSparseSpan& above = f->spans.data[f->open.data[openAt]];
        if (above.origin.x == f->left + int32_t(x) && above.extent.x == width) {
            above.extent.y += height;
            f->nextOpen.push(f->open.data[openAt]);
            return;
        }
    }
    f->nextOpen.push(f->spans.count);
    f->spans.push(PrPathSparseSpan{{f->left + int32_t(x), f->top + int32_t(y)}, {width, height}, kPrPathSparseSolid});
}

// Adds the pixels of a row of tiles from `x0` to `x1`, which no edge touches, by the winding they carry.
void addGap(PathSparseRasterizer* f, PrPathFillRule rule, uint32_t x0, uint32_t x1, uint32_t y, uint32_t height,
        uint32_t& openAt) {
    if (x1 <= x0)
        return;
    bool full = true;
    bool empty = true;
    for (uint32_t r = 0; r < height; ++r) {
        float c = pathCoverageFromWinding(f->carry.data[r], rule);
        full = full && c >= kFull;
        empty = empty && c <= kEmpty;
    }
    if (empty)
        return;
    if (full) {
        addSolid(f, x0, y, x1 - x0, height, openAt);
        return;
    }
    // Rows partly covered to their ends.
    uint32_t width = x1 - x0;
    if (f->options.kind == PrPathSparseKind_Coverage) {
        uint32_t data = f->coverage.count;
        f->coverage.reserve(data + width * height);
        for (uint32_t r = 0; r < height; ++r) {
            uint8_t c = uint8_t(pathCoverageFromWinding(f->carry.data[r], rule) * 255 + 0.5f);
            prMemFill(f->coverage.data + data + r * width, c, width);
        }
        f->coverage.count = data + width * height;
        f->spans.push(PrPathSparseSpan{{f->left + int32_t(x0), f->top + int32_t(y)}, {width, height}, data});
        return;
    }
    uint32_t data = f->tiles.count;
    uint32_t tw = f->options.tileExtent.x;
    for (uint32_t x = x0; x < x1; x += tw) {
        f->tiles.push(0);
        f->tiles.append(f->carry.data, height);
    }
    f->spans.push(PrPathSparseSpan{{f->left + int32_t(x0), f->top + int32_t(y)}, {width, height}, data});
}

// Adds a span of edge tiles, with the coverage of each pixel by the pieces of the span.
void addCoverageSpan(PathSparseRasterizer* f, PrPathFillRule rule, uint32_t x0, uint32_t x1, uint32_t y,
        uint32_t height, const Ref* refs, uint32_t refCount) {
    uint32_t width = x1 - x0;
    size_t stride = size_t(width) + 2;
    size_t cellCount = stride * height;
    f->cells.clear();
    f->cells.reserve(uint32_t(cellCount));
    prMemZero(f->cells.data, cellCount * sizeof(float));
    uint32_t stamp = ++f->stamp;
    for (uint32_t i = 0; i < refCount; ++i) {
        Piece& p = f->pieces.data[refs[i].piece];
        if (p.stamp == stamp || p.direction == 0)
            continue;
        p.stamp = stamp;
        PathCellEdge e{p.x0 - float(x0), p.y0 - float(y), p.x1 - float(x0), p.y1 - float(y), p.direction};
        pathAccumulate(f->cells.data, stride, width, height, e);
    }
    uint32_t data = f->coverage.count;
    f->coverage.reserve(data + width * height);
    for (uint32_t r = 0; r < height; ++r) {
        const float* row = f->cells.data + r * stride;
        uint8_t* out = f->coverage.data + data + r * width;
        float winding = f->carry.data[r];
        for (uint32_t x = 0; x < width; ++x) {
            winding += row[x];
            out[x] = uint8_t(pathCoverageFromWinding(winding, rule) * 255 + 0.5f);
        }
        f->carry.data[r] = winding + row[width] + row[width + 1];
    }
    f->coverage.count = data + width * height;
    f->spans.push(PrPathSparseSpan{{f->left + int32_t(x0), f->top + int32_t(y)}, {width, height}, data});
}

// Adds a span of edge tiles as records of the pieces of the span, clipped to each tile.
void addSegmentSpan(PathSparseRasterizer* f, uint32_t firstTile, uint32_t x1, uint32_t y, uint32_t height,
        const Ref* refs, uint32_t refCount) {
    uint32_t tw = f->options.tileExtent.x;
    uint32_t x0 = firstTile * tw;
    uint32_t data = f->tiles.count;
    uint32_t at = 0;
    for (uint32_t tileX = x0; tileX < x1; tileX += tw) {
        uint32_t tile = tileX / tw;
        float left = float(tileX);
        float right = float(minU32(tileX + tw, x1));
        uint32_t record = f->tiles.count;
        f->tiles.push(0);
        f->tiles.append(f->carry.data, height);
        uint32_t count = 0;
        for (; at < refCount && refs[at].tile == tile; ++at) {
            const Piece& p = f->pieces.data[refs[at].piece];
            if (p.direction == 0)
                continue;
            // The part of the piece within the columns of the tile.
            float t0 = 0;
            float t1 = 1;
            if (p.x1 != p.x0) {
                float ta = (left - p.x0) / (p.x1 - p.x0);
                float tb = (right - p.x0) / (p.x1 - p.x0);
                t0 = prF32Max(prF32Min(ta, tb), 0);
                t1 = prF32Min(prF32Max(ta, tb), 1);
            }
            if (!(t1 > t0))
                continue;
            float xa = t0 == 0 ? p.x0 : p.x0 + (p.x1 - p.x0) * t0;
            float ya = t0 == 0 ? p.y0 : p.y0 + (p.y1 - p.y0) * t0;
            float xb = t1 == 1 ? p.x1 : p.x0 + (p.x1 - p.x0) * t1;
            float yb = t1 == 1 ? p.y1 : p.y0 + (p.y1 - p.y0) * t1;
            if (!(yb > ya))
                continue;
            xa = prF32Min(prF32Max(xa, left), right) - left;
            xb = prF32Min(prF32Max(xb, left), right) - left;
            // The segment runs from its start to its end, so the order of its points carries its direction.
            float down[4] = {xa, ya - float(y), xb, yb - float(y)};
            float up[4] = {xb, yb - float(y), xa, ya - float(y)};
            f->tiles.append(p.direction > 0 ? down : up, 4);
            ++count;
            // The winding of the segment reaches every column right of it.
            for (uint32_t r = uint32_t(ya) - y; r < height && float(y + r) < yb; ++r) {
                float dy = prF32Min(float(y + r + 1), yb) - prF32Max(float(y + r), ya);
                f->carry.data[r] += dy * p.direction;
            }
        }
        f->tiles.data[record] = float(count);
    }
    f->spans.push(PrPathSparseSpan{{f->left + int32_t(x0), f->top + int32_t(y)}, {x1 - x0, height}, data});
}

void fill(PathSparseRasterizer* f, PrPathFillRule rule) {
    uint32_t tw = f->options.tileExtent.x;
    uint32_t th = f->options.tileExtent.y;
    sortItems(f->refs.data, f->refs.count, [](const Ref& a, const Ref& b) {
        if (a.row != b.row)
            return a.row < b.row;
        if (a.tile != b.tile)
            return a.tile < b.tile;
        return a.piece < b.piece;
    });
    f->open.clear();
    uint32_t i = 0;
    for (uint32_t row = 0; row < f->tilesY; ++row) {
        uint32_t y = row * th;
        uint32_t height = minU32(th, f->height - y);
        prMemCopy(f->carry.data, f->leftWinding.data + y, height * sizeof(float));
        f->nextOpen.clear();
        uint32_t openAt = 0;
        uint32_t gapTile = 0;
        while (i < f->refs.count && f->refs.data[i].row == row) {
            // Finds the run of adjacent tiles that starts at this one.
            uint32_t first = f->refs.data[i].tile;
            uint32_t end = first + 1;
            uint32_t j = i;
            while (j < f->refs.count && f->refs.data[j].row == row && f->refs.data[j].tile <= end) {
                if (f->refs.data[j].tile == end)
                    ++end;
                ++j;
            }
            uint32_t x1 = minU32(end * tw, f->width);
            addGap(f, rule, gapTile * tw, first * tw, y, height, openAt);
            if (f->options.kind == PrPathSparseKind_Coverage)
                addCoverageSpan(f, rule, first * tw, x1, y, height, f->refs.data + i, j - i);
            else
                addSegmentSpan(f, first, x1, y, height, f->refs.data + i, j - i);
            gapTile = end;
            i = j;
        }
        addGap(f, rule, minU32(gapTile * tw, f->width), f->width, y, height, openAt);
        RawArray<uint32_t> swap = f->open;
        f->open = f->nextOpen;
        f->nextOpen = swap;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPathSparseRasterizer : PathSparseRasterizer {};

PrPathSparseRasterizerOptions prPathSparseRasterizerOptionsDefault(void) {
    return PrPathSparseRasterizerOptions{PrPathSparseKind_Coverage, {4, 4}};
}

PrPathSparseRasterizer* prPathSparseRasterizerCreate(const PrPathSparseRasterizerOptions* PR_NULLABLE options) {
    PrPathSparseRasterizer* f = allocZeroed<PrPathSparseRasterizer>();
    f->options = options != nullptr ? *options : prPathSparseRasterizerOptionsDefault();
    f->options.tileExtent.x = minU32(maxU32(f->options.tileExtent.x, 1), kPrPathSparseMaxTileSize);
    f->options.tileExtent.y = minU32(maxU32(f->options.tileExtent.y, 1), kPrPathSparseMaxTileSize);
    return f;
}

void prPathSparseRasterizerDestroy(PrPathSparseRasterizer* PR_NULLABLE rasterizer) {
    if (rasterizer == nullptr)
        return;
    rasterizer->pieces.reset();
    rasterizer->refs.reset();
    rasterizer->leftWinding.reset();
    rasterizer->cells.reset();
    rasterizer->carry.reset();
    rasterizer->open.reset();
    rasterizer->nextOpen.reset();
    rasterizer->spans.reset();
    rasterizer->coverage.reset();
    rasterizer->tiles.reset();
    prMemoryFree(rasterizer);
}

PrPathSparse prPathSparseRasterizerFill(PrPathSparseRasterizer* rasterizer, const PrPathPolyline* polyline,
        PrPathFillRule rule, PrIRect box) {
    rasterizer->pieces.clear();
    rasterizer->refs.clear();
    rasterizer->spans.clear();
    rasterizer->coverage.clear();
    rasterizer->tiles.clear();
    if (!(box.right > box.left && box.bottom > box.top) || !inRange(box.left) || !inRange(box.top) ||
            !inRange(box.right) || !inRange(box.bottom))
        return PrPathSparse{};
    rasterizer->left = box.left;
    rasterizer->top = box.top;
    rasterizer->width = prIRectWidth(box);
    rasterizer->height = prIRectHeight(box);
    rasterizer->tilesX = (rasterizer->width + rasterizer->options.tileExtent.x - 1) / rasterizer->options.tileExtent.x;
    rasterizer->tilesY = (rasterizer->height + rasterizer->options.tileExtent.y - 1) / rasterizer->options.tileExtent.y;
    rasterizer->leftWinding.clear();
    rasterizer->leftWinding.reserve(rasterizer->height);
    rasterizer->leftWinding.count = rasterizer->height;
    prMemZero(rasterizer->leftWinding.data, rasterizer->height * sizeof(float));
    rasterizer->carry.clear();
    rasterizer->carry.reserve(rasterizer->options.tileExtent.y);
    rasterizer->carry.count = rasterizer->options.tileExtent.y;
    addEdges(rasterizer, *polyline);
    fill(rasterizer, rule);
    return PrPathSparse{
        rasterizer->spans.data, rasterizer->spans.count, rasterizer->coverage.data,
        rasterizer->coverage.count, rasterizer->tiles.data, rasterizer->tiles.count,
    };
}
