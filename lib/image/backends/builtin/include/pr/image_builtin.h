// The built-in image codec.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes PNG (including animated PNG), JPEG, GIF, ICO, TIFF and emjc, and encodes PNG. emjc, of
// the type image/x-emjc, is Apple's format of the images of the Apple Color Emoji font on iOS. The codec inflates and
// deflates PNG data and decompresses emjc's LZFSE through `compressCodec`. If `compressCodec` encodes nothing, the
// codec stores PNG data uncompressed.
PrImageCodec* prImageBuiltinCreate(PrCompressCodec* compressCodec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
