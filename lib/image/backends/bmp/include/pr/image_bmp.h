// The BMP codec: lib/bmp's decoder (pr/bmp.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes BMP files (.bmp, .dib), of the type image/bmp, and encodes nothing. A file decodes to
// 8-bit RGB, or RGBA with straight alpha for bit fields with an alpha mask and for run-length encoding, from the top
// row. The color is sRGB or the embedded ICC profile that a version 4 or 5 header names, and uncharacterized otherwise.
// The header's resolution is the image's. A file that embeds a JPEG or PNG image is NotSupported. The codec has no
// state of its own, so one codec serves every thread.
PrImageCodec* prImageBmpCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
