// Evaluating a Lottie animation's expressions: JavaScript that After Effects evaluates for a property's value.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// A build with PR_JS evaluates expressions with QuickJS (lottie_expression.cpp). Any other build leaves every property
// at its keyframe value (lottie_expression_none.cpp).

#pragma once

#include "lottie/src/lottie_model.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Replaces `value` with the value of the property's expression at `frame`. On entry, `value` holds the keyframe value.
// Returns false if the expression does not evaluate, and then leaves `value` unchanged. The animation's evaluator is
// created when an expression is first evaluated.
bool lottieExpressionEvaluate(const LottieAnimation& a, uint32_t property, double frame, float* value);

// Frees an animation's expression evaluator.
void lottieExpressionsDestroy(LottieExpressions* PR_NULLABLE expressions);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
