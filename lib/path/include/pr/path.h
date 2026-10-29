// Paths: contours of lines and Bézier curves.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A sequence of contours, each a run of lines and curves.
typedef struct PrPath PrPath;

// Contours of lines (path_polyline.h).
typedef struct PrPathPolyline PrPathPolyline;

// The commands that a path records. Each verb takes its points, in order, from the points of the path.
enum {
    PrPathVerb_Move,    // one point, which starts a contour
    PrPathVerb_Line,    // the end point
    PrPathVerb_Quad,    // a control point and the end point
    PrPathVerb_Cubic,   // two control points and the end point
    PrPathVerb_Close,   // no points, and ends the contour with a line back to its start
};
typedef uint8_t PrPathVerb;

// The rule that decides which points the contours of a path enclose.
enum {
    PrPathFillRule_NonZero,     // the points that the contours wind around a nonzero number of times
    PrPathFillRule_EvenOdd,     // the points that the contours wind around an odd number of times
};
typedef uint8_t PrPathFillRule;

// Creates an empty path.
PrPath* prPathCreate(void);

// Creates a path equal to `path`. The two share their verbs and points until either changes. A path and its clones may
// be used from different threads.
PrPath* prPathClone(const PrPath* path);

// Frees the path.
void prPathDestroy(PrPath* PR_NULLABLE path);

// Empties the path, keeping its storage.
void prPathClear(PrPath* path);

// Each function appends the verb that it is named for. A segment added when no contour is open first starts a contour
// at the start of the last contour, or at the origin if there is none. Close does nothing when no contour is open.
void prPathMoveTo(PrPath* path, PrVec2 point);
void prPathLineTo(PrPath* path, PrVec2 end);
void prPathQuadTo(PrPath* path, PrVec2 control, PrVec2 end);
void prPathCubicTo(PrPath* path, PrVec2 control1, PrVec2 control2, PrVec2 end);
void prPathClose(PrPath* path);

// Appends the arc of the ellipse around `center` with `radii`, rotated by `rotation`, from `startAngle` through
// `sweep`. Angles are radians clockwise from +x when y points down, measured before the rotation. If a contour is open,
// a line joins its current point to the start of the arc. Otherwise a contour starts there.
void prPathEllipticArcTo(PrPath* path, PrVec2 center, PrVec2 radii, float rotation, float startAngle, float sweep);

// Appends a closed contour of `rect` or `shape`, clockwise when y points down. An empty rectangle adds nothing. The
// corners of `shape` are fitted first (prCornerRectFit).
void prPathAddRect(PrPath* path, PrRect rect);
void prPathAddCornerRect(PrPath* path, PrCornerRect shape);

// Appends a closed contour of the ellipse around `center` with `radii`, rotated by `rotation` (prPathEllipticArcTo).
// The contour starts at angle zero and runs clockwise when y points down. A radius of zero gives a flat contour. A
// radius below zero or not finite adds nothing.
void prPathAddEllipse(PrPath* path, PrVec2 center, PrVec2 radii, float rotation);

// Appends the annular sector around `center` between `innerRadius` and `outerRadius`, from `startAngle` through
// `sweep`, in radians clockwise from +x. With an `innerRadius` of zero the sector is a pie slice. With a `sweep` of a
// full turn or more, it is a ring of two contours wound in opposite directions.
void prPathAddSector(PrPath* path, PrVec2 center, float innerRadius, float outerRadius,
                     float startAngle, float sweep);

// Appends a closed contour around every point within `radius` of the segment from `start` to `end`. A `radius` of zero
// or less adds nothing.
void prPathAddCapsule(PrPath* path, PrVec2 start, PrVec2 end, float radius);

// Appends every contour of `other`, with each point mapped through `transform`.
void prPathAddPath(PrPath* path, const PrPath* other, PrAffine2 transform);

// Appends the contours of `polyline` to the path as lines, with each point mapped through `transform`.
void prPathAddPolyline(PrPath* path, const PrPathPolyline* polyline, PrAffine2 transform);

// Maps every point of the path through `transform`.
void prPathTransform(PrPath* path, PrAffine2 transform);

// Returns the verbs and points of the path. They stay valid until the path changes.
uint32_t prPathVerbCount(const PrPath* path);
const PrPathVerb* PR_NULLABLE prPathVerbs(const PrPath* path);
uint32_t prPathPointCount(const PrPath* path);
const PrVec2* PR_NULLABLE prPathPoints(const PrPath* path);

// Returns a number that identifies the verbs and points of the path for the life of the process. It serves as a key for
// data derived from them. Clones share it until one of them changes, and a change gives the path a new id. Thread-safe
// on a path that does not change.
uint64_t prPathId(const PrPath* path);

// Returns the bounds of every point, including control points. The bounds contain the curves. Returns an empty
// rectangle if the path has no points.
PrRect prPathBounds(const PrPath* path);

// Returns the bounds of the curves themselves, at their extremes instead of at their control points.
PrRect prPathTightBounds(const PrPath* path);

// Returns true if `point` is inside the area that the path fills by `rule`, with its curves flattened to within a
// hundredth of a unit. Each contour counts as closed.
bool prPathContains(const PrPath* path, PrVec2 point, PrPathFillRule rule);

// The placement of the lines that flatten a curve.
enum {
    PrPathFlattening_Inside,        // inside the curve, through points on it
    PrPathFlattening_Balanced,      // moved out from the curve to cover the same area as the curve
};
typedef uint8_t PrPathFlattening;

// Appends the path's contours to `out`, with each point mapped through `transform`. Curves become lines within
// `tolerance` of the mapped curves, placed as `flattening` says. A contour that ends with a close is closed.
void prPathFlatten(const PrPath* path, PrAffine2 transform, float tolerance, PrPathFlattening flattening,
                   PrPathPolyline* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
