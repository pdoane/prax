// DWAA and DWAB, OpenEXR's lossy compressions of blocks of the discrete cosine transform.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

#include "exr/src/exr_compress.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Decompresses a DWAA or DWAB chunk of `block` into its `size` bytes of pixels at `out`, as exrDecompress describes.
PrStatus exrDwaDecompress(ExrCodec& codec, const ExrBlock& block, PrByteView packed, uint8_t* out, size_t size);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
