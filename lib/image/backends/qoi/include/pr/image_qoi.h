// The QOI codec: lib/qoi's decoder (pr/qoi.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes QOI images (.qoi), of the type image/qoi, and encodes nothing. An image decodes to 8-bit
// RGB, or RGBA with straight alpha if its header gives four channels. The header's colorspace makes the color sRGB or
// linear sRGB. The codec has no state of its own, so one codec serves every thread.
PrImageCodec* prImageQoiCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
