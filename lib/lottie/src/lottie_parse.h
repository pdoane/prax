// Reading a Lottie animation's JSON into its model.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/lottie.h"
#include "pr/status.h"
#include "pr/string_view.h"

#include "lottie/src/lottie_model.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads the animation from its JSON into `a`, which starts empty.
PrStatus lottieParse(PrStringView json, const PrLottieOptions& options, LottieAnimation& a);

// Frees the model's contents and its expression evaluator, and releases its images and fonts.
void lottieReset(LottieAnimation& a);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
