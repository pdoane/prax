// Chooses what a Metal swap chain presents for its window's display. It is plain C++, so that the backends' tests can
// check it with displays that the test machine does not have.

#pragma once

#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a display can show, as the swap chain reads it from AppKit or UIKit: how many times SDR white its brightest
// color can reach, and the widest gamut that it shows.
struct GpuMetalDisplay {
    float potentialHeadroom;
    PrColorSpace gamut;
};

// Returns the format and the color space that a Metal layer presents for `intent` on `display`. HDR and wide gamut
// present scRGB in half floats, which the layer shows past sRGB's gamut, and for HDR past SDR white. HDR does so on a
// display that can show past SDR white, whatever its gamut, and wide gamut on a display of a gamut wider than sRGB's,
// whatever its headroom. Elsewhere they present as SDR does.
inline PrGpuSwapChainInfo gpuMetalSwapChainInfoFromDisplay(PrGpuSwapChainIntent intent, const GpuMetalDisplay& display) {
    bool extended = false;
    if (intent == PrGpuSwapChainIntent_Hdr)
        extended = display.potentialHeadroom > 1;
    else if (intent == PrGpuSwapChainIntent_WideGamut)
        extended = display.gamut != PrColorSpace_Srgb;
    if (extended)
        return PrGpuSwapChainInfo{PrGpuFormat_Rgba16F, PrColorSpace_Srgb};
    return PrGpuSwapChainInfo{PrGpuFormat_Bgra8, PrColorSpace_Srgb};
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
