// Chooses what a Metal swap chain presents for its window's display. It is plain C++, so that the backends' tests can
// check it with displays that the test machine does not have.

#pragma once

#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the format and the color space that a Metal layer presents for `intent` on a display whose brightest color
// can reach `potentialHeadroom` times SDR white. HDR presents extended sRGB, encoded, in half floats, which the layer
// shows past sRGB's gamut and past SDR white. On a display that never shows past SDR white, HDR presents as SDR does,
// even if the display has a wide gamut. Wide color without high dynamic range is not what the HDR intent asks for.
inline PrGpuSwapChainInfo gpuMetalSwapChainInfoFromDisplay(PrGpuSwapChainIntent intent, float potentialHeadroom) {
    if (intent == PrGpuSwapChainIntent_Hdr && potentialHeadroom > 1)
        return PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_Srgb};
    return PrGpuSwapChainInfo{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
