#include "pr/texture.h"

int prTestTextureCApi(void);

// Compiles the public headers as C. The extent of a level halves down to one texel. A conversion keeps the channels by
// default. A stored pixel of four 16-bit channels is 8 bytes.
int prTestTextureCApi(void) {
    PrUVec2 extent = prTextureLevelExtent((PrUVec2){12, 8}, 3);
    int result = extent.x != 1 || extent.y != 1;
    PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba8Srgb);
    result |= convert.channels[3] != 3 || convert.format != PrGpuFormat_Rgba8Srgb;
    result |= prTexturePixelSize(PrTextureChannels_Rgba, PrTextureSample_U16) != 8;
    PrTextureLimits limits = prTextureLimitsDefault();
    PrUVec2 past = {(uint32_t)limits.extent + 1u, 1};
    PrTextureDesc desc = prTextureDesc2d(PrGpuFormat_Rgba8, past, PrTextureAlpha_None);
    result |= prTextureLimitsAdmit(&limits, &desc) || limits.extent != 16384;
    return result;
}
