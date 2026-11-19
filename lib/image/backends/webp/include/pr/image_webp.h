// The WebP codec: lib/webp's decoder (pr/webp.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes WebP (rfc 9649), lossy, lossless or animated, of the type image/webp, and encodes
// nothing. An image decodes to 8-bit RGB, with straight alpha if its header or its bitstream says that it has alpha,
// and in sRGB unless an ICC profile describes it. Its orientation and resolution are its Exif metadata's. A lossy
// picture's chroma is upsampled as libwebp upsamples it by default, so its pixels are libwebp's. An animation's canvas
// starts transparent, and a frame disposed to the background leaves its rectangle transparent: the background color
// that the file gives is a hint, which the composition does not take, and its frames blend in 8-bit sRGB, so that they
// compose to the canvases of libwebp's animation decoder. The codec has no state of its own, so one codec serves every
// thread.
PrImageCodec* prImageWebpCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
