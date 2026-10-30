// Canvas paths: the path of CanvasPath, which the context's current default path and Path2D share.
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/float.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_stroke.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if every value is finite.
template <typename... Values>
bool canvasIsFinite(Values... values) {
    return (isFiniteF64(values) && ...);
}

// The path of an object that implements CanvasPath (html/#canvaspath): subpaths, whose points each method maps through
// a transform as it adds them. The current default path is in the bitmap's space, and a Path2D's is in its own.
//
// The subpaths are a PrPath, whose closed contours continue at their starts as a closed canvas subpath starts its next
// subpath. A canvas subpath of one point is a contour of a move alone.
struct CanvasPath {
    PrPath* path;
    PrDVec2 start;          // the first point of the last subpath, in the path's space
    PrDVec2 last;           // the last point of the last subpath
    bool hasSubpath;        // the path has a subpath
};

// Makes an empty path, or frees one.
void canvasPathInit(CanvasPath* path);
void canvasPathReset(CanvasPath* path);

// Empties the path, keeping its storage.
void canvasPathClear(CanvasPath* path);

// Replaces `out`'s subpaths with a copy of `from`'s.
void canvasPathCopy(CanvasPath* out, const CanvasPath& from);

// The methods of CanvasPath, with each point mapped through `transform`. Each returns or ignores its arguments as the
// method of its name does (html/#dom-context-2d-moveto and the methods after it). A radius of canvasPathRoundRect is a
// corner's horizontal and vertical radius.
void canvasPathClose(CanvasPath* path);
void canvasPathMoveTo(CanvasPath* path, PrDAffine2 transform, double x, double y);
void canvasPathLineTo(CanvasPath* path, PrDAffine2 transform, double x, double y);
void canvasPathQuadraticCurveTo(CanvasPath* path, PrDAffine2 transform, double cpx, double cpy, double x, double y);
void canvasPathBezierCurveTo(CanvasPath* path, PrDAffine2 transform, double cp1x, double cp1y, double cp2x,
    double cp2y, double x, double y);
PrStatus canvasPathArcTo(CanvasPath* path, PrDAffine2 transform, double x1, double y1, double x2, double y2, double radius);
void canvasPathRect(CanvasPath* path, PrDAffine2 transform, double x, double y, double w, double h);
PrStatus canvasPathRoundRect(CanvasPath* path, PrDAffine2 transform, double x, double y, double w, double h,
    const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount);
PrStatus canvasPathEllipse(CanvasPath* path, PrDAffine2 transform, double x, double y, double radiusX,
    double radiusY, double rotation, double startAngle, double endAngle, bool counterclockwise);

// Appends the subpaths of `other` mapped through `transform`, then starts a subpath at its last point mapped
// (html/#dom-path2d-addpath). Adds nothing if `other` has no subpath.
void canvasPathAddPath(CanvasPath* path, const CanvasPath& other, PrDAffine2 transform);

// Appends the path that SVG path data describes, up to its first error, then starts a subpath at its last point
// (html/#dom-path2d).
void canvasPathAppendData(CanvasPath* path, PrStringView data);

// Appends to `out` the subpaths of `path` that a stroke traces (html/#trace-a-path): without its segments of zero
// length, and without the subpaths that are left with no segment.
void canvasPathPrune(const PrPath* path, PrPath* out);

// Returns true if `point` is inside the area that `path` fills by `rule`, or on its edge, which hit testing counts as
// inside (html/#dom-context-2d-ispointinpath). Every subpath counts as closed.
bool canvasPathContains(const PrPath* path, PrVec2 point, PrPathFillRule rule);

// Returns true if `point` is inside the outline of `path` stroked as `stroke`, or on its edge.
bool canvasPathStrokeContains(const PrPath* path, const PrPathStroke* stroke, PrVec2 point);

// Returns `point` mapped through `transform`.
PrDVec2 canvasMapPoint(PrDAffine2 transform, double x, double y);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
