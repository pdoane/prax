// Lottie expressions in a build without expression support: every property keeps its keyframe value.

#include "lottie/src/lottie_expression.h"

namespace pr_impl {

bool lottieExpressionEvaluate(const LottieAnimation&, uint32_t, double, float*) {
    return false;
}

void lottieExpressionsDestroy(LottieExpressions* PR_NULLABLE) {}

} // namespace pr_impl
