// Evaluating a Lottie animation's properties and transforms at a frame.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// Frames are measured in the time of the composition that holds the property's layer. Keyframe times use the same time.

#pragma once

#include "lottie/src/lottie_model.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the cubic Bézier easing from (0, 0) through (x1, y1) and (x2, y2) to (1, 1), evaluated at `x`. The curve is
// the same as CSS cubic-bezier().
double lottieEase(double x1, double y1, double x2, double y2, double x);

// Returns the number of floats in the property's value, or zero for kLottieNone.
uint32_t lottieDims(const LottieAnimation& a, uint32_t property);

// Writes the property's value at `frame` into `out`, which holds lottieDims floats. If the property has an expression
// and the build evaluates expressions, the value comes from the expression.
void lottieEvaluate(const LottieAnimation& a, uint32_t property, double frame, float* out);

// Writes the property's value at `frame` from its keyframes alone, ignoring any expression.
void lottieEvaluateKeyframes(const LottieAnimation& a, uint32_t property, double frame, float* out);

// The maximum depth of nested expression evaluation. Beyond it, properties with expressions are evaluated from their
// keyframes.
constexpr uint32_t kLottieMaxExpressionDepth = 16;

// Returns the value of a number property at `frame`, or `fallback` for kLottieNone.
float lottieScalar(const LottieAnimation& a, uint32_t property, double frame, float fallback);

// Returns the first two floats of the property's value at `frame`, or `fallback` for kLottieNone.
PrVec2 lottiePoint(const LottieAnimation& a, uint32_t property, double frame, PrVec2 fallback);

// Returns a transform's position at `frame`, whether or not the position is split into separate axes.
PrVec2 lottieTransformPosition(const LottieAnimation& a, const LottieTransform& t, double frame);

// Returns the transform's matrix in three dimensions. The matrix moves the anchor to the origin, scales, skews, rotates
// about z, y and x and by the orientation, and moves to the position. Negative z points toward the viewer, as in After
// Effects. With `autoOrient`, the matrix also rotates to the direction of motion, as in lottieTransformAffine. The
// matrix has no perspective, so it flattens to an affine transform of the plane (prAffine2FromMatrix4).
PrMatrix4 lottieTransformMatrix4(const LottieAnimation& a, const LottieTransform& t, double frame,
                                 bool autoOrient = false);

// Returns the view of a camera layer whose transform is `t`, for a composition of `extent`: the matrix from the
// composition's 3D space to the camera's, in which the eye looks down -z from `zoom` above the composition's center. A
// camera at its default place, centered `zoom` in front of the composition and aimed at its center, gives the identity.
// The camera turns to face its point of interest, which its transform holds as the anchor, then by its rotations and
// orientation.
PrMatrix4 lottieCameraView(const LottieAnimation& a, const LottieTransform& t, double frame, PrVec2 extent,
                           float zoom);

// Returns the transform's matrix in two dimensions. The matrix moves the anchor to the origin, scales, skews, rotates
// and moves to the position. With `autoOrient`, it also rotates to the direction that the position moves in at `frame`.
PrAffine2 lottieTransformAffine(const LottieAnimation& a, const LottieTransform& t, double frame,
                                bool autoOrient = false);

// Returns the transform's opacity, as a fraction.
float lottieTransformOpacity(const LottieAnimation& a, const LottieTransform& t, double frame);

// Returns true if none of the parts that make up the transform's matrix has keyframes.
bool lottieTransformFixed(const LottieAnimation& a, const LottieTransform& t);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
