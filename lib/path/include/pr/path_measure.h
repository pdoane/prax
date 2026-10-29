// Measuring paths: the arc length of their contours taken end to end, and the nearest point on them.

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

// The contours of a path, measured end to end. A distance along the path runs through the first contour, then the next.
// Contours with no length do not count. A measure copies what it measures, and keeps its storage from one path to the
// next. Any thread may query it.
typedef struct PrPathMeasure PrPathMeasure;

// The range of distances along the path that a contour covers.
typedef struct PrPathMeasureContour {
    float start;                // the distance at which the contour starts
    float length;
    bool closed;
} PrPathMeasureContour;

// A point along a path, and the unit direction of the path there.
typedef struct PrPathMeasurePosition {
    PrVec2 point;
    PrVec2 tangent;
} PrPathMeasurePosition;

// The point of a path nearest a given point. `offset` is how far the given point lies to the right of the path, facing
// along the path with y down. It is negative to the left.
typedef struct PrPathMeasureNearest {
    float distance;     // along the path
    float offset;
    uint32_t contour;   // the contour that the nearest point lies on
} PrPathMeasureNearest;

// Creates an empty measure.
PrPathMeasure* prPathMeasureCreate(void);

// Frees the measure.
void prPathMeasureDestroy(PrPathMeasure* PR_NULLABLE measure);

// Measures `path`, with each point mapped through `transform`, and replaces what the measure held. Curves become lines
// within `tolerance` of the mapped curves.
void prPathMeasureSetPath(PrPathMeasure* measure, const PrPath* path, PrAffine2 transform, float tolerance);

// Measures `polyline`, and replaces what the measure held.
void prPathMeasureSetPolyline(PrPathMeasure* measure, const PrPathPolyline* polyline);

// Returns the length of the path, which is the sum of the lengths of its contours.
float prPathMeasureLength(const PrPathMeasure* measure);

// Returns the number of contours that count, and contour `index` among them.
uint32_t prPathMeasureContourCount(const PrPathMeasure* measure);
PrPathMeasureContour prPathMeasureContour(const PrPathMeasure* measure, uint32_t index);

// Returns the position at `distance` along the path, clamped to the ends of the path. At the distance at which one
// contour ends and the next starts, the point is the end of the first contour.
PrPathMeasurePosition prPathMeasurePosition(const PrPathMeasure* measure, float distance);

// Writes to `out` the transform that lays a straight line onto the path at `distance`. The point (`distance`, 0) of the
// line lands at that distance along the path. The x axis of the line turns to the direction of the path there, and its
// y axis turns to the right of the path. Returns false for a `distance` off either end.
bool prPathMeasureLineTransform(const PrPathMeasure* measure, float distance, PrAffine2* out);

// Appends to `out` the path from `start` to `end` as open contours of lines, one for each contour that the range
// crosses. A range that is empty after clamping to the path adds nothing.
void prPathMeasureSegment(const PrPathMeasure* measure, float start, float end, PrPathPolyline* out);

// How far apart two squared distances to the nearest point may be and still count as equal.
static const float kPrPathMeasureTieTolerance = 1e-3f;

// Writes to `out` the point of the path nearest `point`, within `reach` of it. Among points whose squared distances
// differ by no more than kPrPathMeasureTieTolerance, it is the one earliest along the path. Returns false if no point
// of the path is within `reach`.
bool prPathMeasureNearest(const PrPathMeasure* measure, PrVec2 point, float reach, PrPathMeasureNearest* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
