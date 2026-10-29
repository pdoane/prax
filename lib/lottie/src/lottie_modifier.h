// Shape modifiers, which rewrite the contours of the shapes they follow.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "lottie/src/lottie_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Rewrites the contours of the gathered shapes with their modifiers, nearest modifier first.
void lottieApplyModifiers(LottieRender& r, double frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
