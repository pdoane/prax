// Rasterizing polylines to coverage.
//
// Antialiased coverage accumulates the signed area that each edge leaves in the cells of its rows. A running sum along
// a row turns the areas into the winding that covers each pixel.

#include "pr/path_rasterizer.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/vec.h"

#include "path/src/cells.h"
#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

// The point at which an edge crosses the center of a row, for aliased coverage.
struct Crossing {
    float x;
    float direction;
};

struct PathRasterizer {
    Array<PathCellEdge> edges;
    Array<float> cells;   // a row of width + 2 per mask row, zeroed between fills
    Array<Crossing> crossings;
};

void addEdge(PathRasterizer* r, PrVec2 from, PrVec2 to) {
    if (from.y == to.y || !isFiniteF32(from.x) || !isFiniteF32(from.y) || !isFiniteF32(to.x) || !isFiniteF32(to.y))
        return;
    if (from.y < to.y)
        r->edges.push(PathCellEdge{from.x, from.y, to.x, to.y, 1});
    else
        r->edges.push(PathCellEdge{to.x, to.y, from.x, from.y, -1});
}

// Builds the edges of the polyline in mask coordinates, with every contour closed.
void addEdges(PathRasterizer* r, const PrPathPolyline& polyline, int32_t x, int32_t y) {
    r->edges.clear();
    PrVec2 offset{float(x), float(y)};
    auto at = [&](uint32_t i) { return polyline.points[i] - offset; };
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        for (uint32_t i = start; i < end; ++i)
            addEdge(r, at(i), at(i + 1 < end ? i + 1 : start));
    }
}

// Accumulates an edge, split at the points at which it crosses the left and right sides of the mask. The parts beyond a
// side move onto that side. Area left of the mask covers its first column, and area right of the mask covers nothing in
// it.
void accumulateClipped(float* cells, size_t stride, uint32_t width, uint32_t height, PathCellEdge e) {
    float right = float(width);
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
            e.x0 + (e.x1 - e.x0) * t0, e.y0 + (e.y1 - e.y0) * t0,
            e.x0 + (e.x1 - e.x0) * t1, e.y0 + (e.y1 - e.y0) * t1, e.direction,
        };
        piece.x0 = minF32(maxF32(piece.x0, 0), right);
        piece.x1 = minF32(maxF32(piece.x1, 0), right);
        pathAccumulate(cells, stride, width, height, piece);
    }
}

void fillAntialiased(PathRasterizer* r, PrPathFillRule rule, PrPixels mask) {
    size_t stride = size_t(mask.extent.x) + 2;
    size_t cellCount = stride * mask.extent.y;
    if (r->cells.capacity < cellCount) {
        r->cells.reset();
        r->cells.reserve(uint32_t(cellCount));
        memZero(r->cells.data, size_t(r->cells.capacity) * sizeof(float));
    }
    float* cells = r->cells.data;
    for (uint32_t i = 0; i < r->edges.count; ++i)
        accumulateClipped(cells, stride, mask.extent.x, mask.extent.y, r->edges.data[i]);
    for (uint32_t y = 0; y < mask.extent.y; ++y) {
        float* row = cells + size_t(y) * stride;
        uint8_t* out = mask.data + size_t(y) * mask.stride;
        float winding = 0;
        for (uint32_t x = 0; x < mask.extent.x; ++x) {
            winding += row[x];
            out[x] = uint8_t(pathCoverageFromWinding(winding, rule) * 255 + 0.5f);
        }
        memZero(row, stride * sizeof(float));
    }
}

bool inside(float winding, PrPathFillRule rule) {
    int32_t count = int32_t(winding);
    return rule == PrPathFillRule_EvenOdd ? (count & 1) != 0 : count != 0;
}

// Returns the first column whose center is at or right of `x`, clamped to [0, width].
uint32_t columnAt(float x, uint32_t width) {
    float column = ceilF32(x - 0.5f);
    if (!(column > 0))
        return 0;
    return column >= float(width) ? width : uint32_t(column);
}

void fillAliased(PathRasterizer* r, PrPathFillRule rule, PrPixels mask) {
    for (uint32_t y = 0; y < mask.extent.y; ++y) {
        uint8_t* out = mask.data + size_t(y) * mask.stride;
        memZero(out, mask.extent.x);
        float center = float(y) + 0.5f;
        r->crossings.clear();
        for (uint32_t i = 0; i < r->edges.count; ++i) {
            const PathCellEdge& e = r->edges.data[i];
            if (center < e.y0 || center >= e.y1)
                continue;
            float x = e.x0 + (center - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0);
            Crossing crossing{x, e.direction};
            uint32_t at = r->crossings.count;
            while (at != 0 && r->crossings.data[at - 1].x > x)
                --at;
            r->crossings.insert(at, crossing);
        }
        float winding = 0;
        for (uint32_t i = 0; i + 1 < r->crossings.count; ++i) {
            winding += r->crossings.data[i].direction;
            if (!inside(winding, rule))
                continue;
            uint32_t from = columnAt(r->crossings.data[i].x, mask.extent.x);
            uint32_t to = columnAt(r->crossings.data[i + 1].x, mask.extent.x);
            for (uint32_t x = from; x < to; ++x)
                out[x] = 255;
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPathRasterizer : PathRasterizer {};

PrPathRasterizer* prPathRasterizerCreate(void) {
    return allocZeroed<PrPathRasterizer>();
}

void prPathRasterizerDestroy(PrPathRasterizer* PR_NULLABLE rasterizer) {
    if (rasterizer == nullptr)
        return;
    rasterizer->edges.reset();
    rasterizer->cells.reset();
    rasterizer->crossings.reset();
    prMemoryFree(rasterizer);
}

void prPathRasterizerFill(PrPathRasterizer* rasterizer, const PrPathPolyline* polyline, PrPathFillRule rule,
        PrPathCoverage coverage, PrIVec2 origin, PrPixels mask) {
    if (mask.extent.x == 0 || mask.extent.y == 0)
        return;
    addEdges(rasterizer, *polyline, origin.x, origin.y);
    if (coverage == PrPathCoverage_Area)
        fillAntialiased(rasterizer, rule, mask);
    else
        fillAliased(rasterizer, rule, mask);
}
