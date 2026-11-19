// PIZ, OpenEXR's lossless wavelet compression, and the Huffman coding of 16-bit values that it shares with DWA.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

#include "exr/src/exr_compress.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Decodes the Huffman coding of 16-bit values that PIZ and DWA share, from `packed` into exactly `count` values at
// `out`. Returns InvalidData for a coding that does not decode to that many.
PrStatus exrHuffmanDecode(ExrCodec& codec, PrByteView packed, uint16_t* out, size_t count);

// Decompresses a PIZ chunk of `block` into its `size` bytes of pixels at `out`, as exrDecompress describes.
PrStatus exrPizDecompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
