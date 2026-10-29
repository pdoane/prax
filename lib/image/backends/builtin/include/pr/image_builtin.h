// The built-in image codec.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes PNG (including animated PNG), JPEG, GIF, ICO and TIFF, and encodes PNG. It inflates and
// deflates PNG data through `compressCodec`. If `compressCodec` encodes nothing, the codec stores PNG data
// uncompressed.
PrImageCodec* prImageBuiltinCreate(PrCompressCodec* compressCodec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
