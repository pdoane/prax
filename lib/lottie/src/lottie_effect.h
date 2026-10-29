// Layer effects, as draw filters.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/draw_filter.h"

#include "lottie/src/lottie_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the filter graph of the layer's effects at `frame`. The caller releases the graph. Returns null if the layer
// has no effects that the engine draws.
const PrDrawFilter* PR_NULLABLE lottieLayerFilter(const LottieRender& r, const LottieLayer& layer, double frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
