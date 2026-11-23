// The Targa codec: lib/tga's decoder (pr/tga.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes Targa images (.tga), of the type image/x-tga, and encodes nothing. An image decodes to
// 8-bit gray or RGB, with alpha if the file says it has alpha, and its origin is the image's orientation. Its color is
// uncharacterized. A Targa file has no signature, so the codec recognizes a header that the specification allows, and a
// set tries it after the codecs of formats that have one. The codec has no state of its own, so one codec serves every
// thread.
PrImageCodec* prImageTgaCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
