// The OpenEXR codec: lib/exr's reader (pr/exr.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes OpenEXR images (.exr), of the types image/x-exr and image/aces, and encodes nothing. An
// image is its first part that lib/exr reads, with its mip or rip levels, in the scene-linear values that lib/exr
// decodes it to, with premultiplied alpha. The file's chromaticities, or Rec. ITU-R BT.709's if it gives none, are the
// color: the named linear space whose primaries and white they are, or else PrImageColorEncoding_Chromaticities. A
// file's other parts, its channels by name and its attributes are lib/exr's to read. A chunk that the file does not
// hold decodes to zero, and a read of it returns Truncated. Chunks of zlib decompress through `compress`, which must
// outlive the codec.
PrImageCodec* prImageExrCreate(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
