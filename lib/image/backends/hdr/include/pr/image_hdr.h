// The Radiance HDR codec: lib/hdr's decoder (pr/hdr.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes Radiance HDR pictures (.hdr), of the type image/vnd.radiance, and encodes nothing. A
// picture decodes to red, green and blue 32-bit floats in linear sRGB without alpha: scene-referred radiance, which may
// be far above 1. Its exposure is divided out, and the order of its scanlines is the image's orientation. The codec has
// no state of its own, so one codec serves every thread.
PrImageCodec* prImageHdrCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
