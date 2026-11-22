// The KTX 2.0 image codec. It makes the textures of lib/ktx (pr/ktx.h) a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes KTX 2.0 textures (.ktx2) of the type image/ktx2. The codec encodes nothing. A texture
// has mip levels, and its layers and faces are its frames. It decodes to the pixels that lib/ktx produces: one channel
// is gray, and two channels are red and green with no blue. The texture's descriptor gives the color of its pixels and
// whether alpha is premultiplied. Its KTXorientation gives the orientation. Levels that are Zstandard or zlib streams
// decompress through `compress`, which must outlive the codec. The codec returns NotSupported for a block texture whose
// blocks do not decode to pixels, a volume, an ETC1S video and any other format. The codec does not return a texture's
// blocks or transcode it to GPU blocks. Use lib/ktx for those.
PrImageCodec* prImageKtxCreate(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
