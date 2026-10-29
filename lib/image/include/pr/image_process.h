// Image processing: operations on pixels in memory.

#pragma once

#include "pr/base.h"
#include "pr/pixels.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What an area operation reads beyond the edges of its pixels.
enum {
    PrImageEdgeMode_None,       // transparent black
    PrImageEdgeMode_Duplicate,  // the edge pixels
    PrImageEdgeMode_Wrap,       // the opposite edge
    PrImageEdgeMode_Mirror,     // the pixels mirrored across the edge, the edge pixel first
};
typedef uint8_t PrImageEdgeMode;

// Blurs `pixels` in place by a Gaussian of `sigma` along each axis, in pixels. `format` is RgbaF32 or RF32, and the
// stride is a whole number of floats. `edge` decides what the blur reads beyond the edges. A small sigma blurs by the
// exact Gaussian, and a larger one by three box passes that approximate it.
void prImageBlur(PrPixels pixels, PrPixelFormat format, PrVec2 sigma, PrImageEdgeMode edge);

// Writes to `out` the mip level below `above`. Both are in `format`: four 8-bit channels (Rgba8 or Bgra8), or RgbaF16.
// If `above` is w by h pixels, `out` is max(w/2, 1) by max(h/2, 1). Each pixel of `out` is the mean of the 2 by 2 block
// above it, rounded to the format. If `above` has one row or column too few for a block, the block repeats its last row
// or column.
void prImageMipLevel(PrPixelView above, PrPixels out, PrPixelFormat format);

// The number of points in the lattice of Perlin turbulence.
enum { kPrImageNoiseLattice = 256 };

// The tables that Perlin turbulence reads for a seed, as the SVG turbulence filter defines them. `lattice` is a
// permutation of the lattice points, and `gradients` holds a unit gradient for each channel at each point. Both repeat
// past the end of the lattice, so that looking up a point and the next point needs no wrap.
typedef struct PrImageNoise {
    int32_t lattice[kPrImageNoiseLattice * 2 + 2];
    float gradients[4][kPrImageNoiseLattice * 2 + 2][2];
} PrImageNoise;

// Writes the tables for `seed` to `out`.
void prImageNoise(float seed, PrImageNoise* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
