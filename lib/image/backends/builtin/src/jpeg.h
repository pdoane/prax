// JPEG decoding for the built-in codec.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

#include "image/backends/builtin/src/metadata.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads the orientation and resolution from the application segments of the JPEG in `bytes`. A field that the JPEG does
// not give leaves its member of `*out` unchanged.
void jpegMetadata(PrByteView bytes, ImageMetadata* out);

// Decodes a Huffman-coded, 8-bit, sequential or progressive JPEG into `pixels`. The pixels are tightly packed Gray, RGB
// or CMYK, in the canonical form of the JPEG.
bool jpegDecode(PrByteView bytes, uint8_t* pixels, size_t pixelsSize);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
