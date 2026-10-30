// Projected groups (PrDrawGroup::projection): where a group's layer, drawn flat in its plane, lands on the target.
// Backends share these to composite the layer through the projection.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/path_polyline.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where a projected group's layer lands. The layer holds the plane scaled by `scale` about the plane's origin: a point
// p of the plane is at p * scale in the layer's pixels.
typedef struct PrDrawProjection {
    PrMatrix3 layerToTarget;    // the layer's pixels to target pixels
    PrMatrix3 targetToLayer;    // target pixels to the layer's pixels, which is how a composite samples the layer
    PrRect area;                // the part of the group's bounds that lands in the visible box, in the plane
    float scale;                // the layer's pixels per unit of the plane
    PrRect targetBounds;        // the bounds of where `area` lands, within the visible box, in target pixels
} PrDrawProjection;

// Places a projected group's layer. `projection` and `bounds` are the group's, and `toTarget` maps the space that is
// current at the group's push to target pixels. `visible` is the part of the target that the composite may change, in
// target pixels.
//
// If the projection is affine on the plane, the scale is its exact scale. Otherwise the scale is the largest factor by
// which the projection scales a length over the plane's visible part, but no more than the largest scale of `toTarget`,
// so the layer is no finer than the device. It is then reduced until `area` holds at most twice as many layer pixels as
// the target pixels that the visible part covers. In either case, the scale is reduced until `area` is no wider or
// taller than `maxLayerSide` layer pixels. The near part of a steep plane may therefore be sampled at less than one
// texel per pixel.
//
// Returns false if the group draws nothing: its matrix is not finite, its plane is edge-on or behind the eye, or no
// part of it lands in `visible`.
bool prDrawProjectionPlace(const PrMatrix4* projection, PrAffine2 toTarget, PrRect bounds, PrRect visible,
    float maxLayerSide, PrDrawProjection* out);

// Appends `rect`, a rectangle of the layer's pixels, to `out` in target pixels, cut where it passes behind the eye.
void prDrawProjectionQuad(const PrDrawProjection* projection, PrRect rect, PrPathPolyline* out);

// Returns the affine map from the plane to target pixels that agrees with the projection to first order at the center
// of `area`. A backend that cannot sample a layer through the projection draws the plane through this map.
PrAffine2 prDrawProjectionTangent(const PrDrawProjection* projection);

// Returns the bounds of `bounds`, a rectangle of a projected group's plane, projected into the space that is current at
// the group's push and cut behind the eye. Returns an empty rectangle if no part of it lies in front of the eye or if
// the matrix is not finite.
PrRect prDrawProjectionBounds(const PrMatrix4* projection, PrRect bounds);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
