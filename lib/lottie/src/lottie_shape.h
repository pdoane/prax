// Drawing shape layers: their items, groups and contours.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/geom.h"
#include "pr/path.h"

#include "lottie/src/lottie_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Appends the Bézier shape `value` to the geometry as one contour, in reverse order if `reversed`.
void lottieShapeFromValue(const float* value, bool reversed, LottieGeometry& g);

// Returns the transform from the space of shape group `group` to the space of `ancestor`. An `ancestor` of kLottieNone
// is the layer's space.
PrAffine2 lottieGroupToGroup(const LottieRender& r, uint32_t group, uint32_t ancestor);

// Appends the contour to `path`, mapped through `transform`, as a move and cubics. A closed contour ends with a close.
void lottieAddContour(const LottieGeometry& g, const LottieContour& c, PrAffine2 transform, PrPath* path);

// Appends the paths of `count` items from `first` to `path`, mapped through `transform`. Groups among the items add
// their paths under their own transforms. Styles and modifiers are ignored.
void lottieItemPaths(LottieRender& r, uint32_t first, uint32_t count, double frame, PrAffine2 transform,
                     PrPath* path);

// Draws a shape layer's contents at `frame`, in its composition's time, in the layer's space.
void lottieDrawShapes(LottieRender& r, const LottieLayer& layer, double frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
