// Polylines: contours of lines, which is what paths become once their curves are flattened.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Contours of lines. The points of contour i start at `contours[i]`. `closed` holds a bit for each contour, set if the
// contour is closed. The polyline owns its storage.
typedef struct PrPathPolyline {
    PrVec2* PR_NULLABLE points;
    uint32_t* PR_NULLABLE contours;
    uint64_t* PR_NULLABLE closed;
    uint32_t pointCount;
    uint32_t pointCapacity;
    uint32_t contourCount;
    uint32_t contourCapacity;
} PrPathPolyline;

// Returns the index in `points` just past the last point of contour `index`.
static inline uint32_t prPathPolylineContourEnd(const PrPathPolyline* polyline, uint32_t index) {
    return index + 1 < polyline->contourCount ? polyline->contours[index + 1] : polyline->pointCount;
}

// Returns true if contour `index` is closed.
static inline bool prPathPolylineContourIsClosed(const PrPathPolyline* polyline, uint32_t index) {
    return (polyline->closed[index >> 6] >> (index & 63) & 1) != 0;
}

// Empties the polyline, keeping its storage.
void prPathPolylineClear(PrPathPolyline* polyline);

// Frees the polyline's storage and empties it.
void prPathPolylineReset(PrPathPolyline* polyline);

// Starts an open contour at `point`.
void prPathPolylineMoveTo(PrPathPolyline* polyline, PrVec2 point);

// Adds a point to the last contour.
void prPathPolylineLineTo(PrPathPolyline* polyline, PrVec2 point);

// Closes the last contour.
void prPathPolylineClose(PrPathPolyline* polyline);

// Appends every contour of `other`, with each point mapped through `transform`.
void prPathPolylineAddPolyline(PrPathPolyline* polyline, const PrPathPolyline* other, PrAffine2 transform);

// Maps every point of the polyline through `transform`.
void prPathPolylineTransform(PrPathPolyline* polyline, PrAffine2 transform);

// Returns the bounds of every point, or an empty rectangle if the polyline has no points.
PrRect prPathPolylineBounds(const PrPathPolyline* polyline);

// The smallest w that prPathPolylineProject keeps, relative to the largest w of a contour. Points nearer the eye are
// cut away. Code that cuts a projected plane by hand uses the same fraction, so that its cut agrees with
// prPathPolylineProject.
static const float kPrPathPolylineNearPlane = 1e-4f;

// Appends `polyline` to `out` as closed contours, placed on the z = 0 plane, mapped through `matrix` and divided by w.
// Parts nearer the eye than kPrPathPolylineNearPlane are cut away, and the cut closes each contour it crosses.
void prPathPolylineProject(const PrPathPolyline* polyline, const PrMatrix4* matrix, PrPathPolyline* out);

// ---------------------------------------------------------------------------------------------------------------------
// Regions
//
// Combining the regions that two polylines fill, and growing or shrinking a region. The results are closed contours.
// Each contour winds once around what it bounds, so the nonzero and even-odd rules fill them the same. A polyline with
// a point that is not finite gives no result.

// How prPathPolylineCombine combines two regions.
enum {
    PrPathPolylineOp_Union,             // the area covered by either region
    PrPathPolylineOp_Intersect,         // the area covered by both regions
    PrPathPolylineOp_Difference,        // the first region without the second
    PrPathPolylineOp_Xor,               // the area covered by one region but not the other
};
typedef uint8_t PrPathPolylineOp;

// Appends to `out` the result of `op` on the regions that `a` and `b` fill by `aRule` and `bRule`. An open contour
// counts as closed. A union with an empty `b` gives the region of `a` with its overlaps resolved.
void prPathPolylineCombine(const PrPathPolyline* a, PrPathFillRule aRule, const PrPathPolyline* b, PrPathFillRule bRule,
    PrPathPolylineOp op, PrPathPolyline* out);

// Appends to `out` the region that `polyline` fills by `rule`, grown outward by `distance`, or shrunk if `distance` is
// negative. `join` and `miterLimit` shape the corners, and curved corners lie within `tolerance` of their arcs. A
// `tolerance` that is not both finite and above zero leaves the region as it is.
void prPathPolylineOffset(const PrPathPolyline* polyline, PrPathFillRule rule, float distance, PrPathJoin join,
    float miterLimit, float tolerance, PrPathPolyline* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
