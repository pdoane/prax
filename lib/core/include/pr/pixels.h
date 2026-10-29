// Pixels in memory: rows of a pixel format, and views of them.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The channels of a pixel in memory order. A channel is 8 bits, or an IEEE float of 16 or 32 bits in the machine's byte
// order, except in a packed format. The four-channel formats come first, then the one-channel formats, each group
// ordered by the width of its color channels. A format says nothing about what the values mean, such as their encoding
// or how alpha relates to color. The code that holds the pixels defines that.
enum {
    PrPixelFormat_Rgba8,
    PrPixelFormat_Bgra8,
    PrPixelFormat_Rgb10A2,      // 10-bit R, G and B, then 2-bit alpha, from the low bits of a 32-bit word
    PrPixelFormat_RgbaF16,
    PrPixelFormat_RgbaF32,
    PrPixelFormat_R8,
    PrPixelFormat_RF32,
    PrPixelFormat_Count,
};
typedef uint8_t PrPixelFormat;

// Returns the number of bytes that one pixel of the format occupies.
uint32_t prPixelFormatSize(PrPixelFormat format);

// Rows of pixels in memory that the structure does not own. Row `y` starts at `data + y * stride`. If a call does not
// imply the format, the format is passed beside the pixels.
typedef struct PrPixels {
    uint8_t* data;
    size_t stride;
    PrUVec2 extent;
} PrPixels;

// Rows of pixels to read, laid out as in PrPixels.
typedef struct PrPixelView {
    const uint8_t* data;
    size_t stride;
    PrUVec2 extent;
} PrPixelView;

// Returns a read-only view of `pixels`.
static inline PrPixelView prPixelsView(PrPixels pixels) {
    return PR_LITERAL(PrPixelView){pixels.data, pixels.stride, pixels.extent};
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
