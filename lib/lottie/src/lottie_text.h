// Text layers: laying out text documents into letters, and drawing them.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/lottie.h"

#include "lottie/src/lottie_model.h"
#include "lottie/src/lottie_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Lays out the letters of the text documents in the fonts that the options match.
void lottieLayoutText(LottieAnimation& a, const PrLottieOptions& options);

// Draws a text layer's letters at `frame`, in its composition's time, in the layer's space.
void lottieDrawText(LottieRender& r, const LottieLayer& layer, double frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
