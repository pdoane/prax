// Tessellating polylines into triangles.

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

// Triangles as indices into their vertices. Each vertex has a position and a coverage. Coverage runs from 1 inside the
// region to 0 on the outer edge of the fringe. The winding of the triangles is unspecified.
typedef struct PrPathMesh {
    const PrVec2* PR_NULLABLE points;
    const float* PR_NULLABLE coverage;
    uint32_t vertexCount;
    const uint32_t* PR_NULLABLE indices;    // three per triangle
    uint32_t indexCount;
} PrPathMesh;

// Scratch memory that tessellating reuses between paths, and the mesh that it built last.
typedef struct PrPathTessellator PrPathTessellator;

// Creates and frees a tessellator.
PrPathTessellator* prPathTessellatorCreate(void);
void prPathTessellatorDestroy(PrPathTessellator* PR_NULLABLE tessellator);

// Tessellates the region that `polyline` fills by `rule`, and returns the mesh. Every contour counts as closed. Curves
// flattened with PrPathFlattening_Balanced keep their area. Inputs that are not finite tessellate nothing.
//
// A `fringe` above zero antialiases the boundary of the region. The inside moves in by half the fringe at every point
// of the boundary, with miters at the corners. Where the moved boundary crosses itself, the crossed part drops out. A
// band `fringe` wide straddles the boundary, and its coverage falls from 1 to 0 across it. Where the region is narrower
// than the fringe, the bands overlap.
//
// The mesh lives in the storage of the tessellator, and stays valid until the tessellator is next used.
PrPathMesh prPathTessellatorFill(PrPathTessellator* tessellator, const PrPathPolyline* polyline, PrPathFillRule rule,
    float fringe);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
