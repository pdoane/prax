// emjc decoding for the built-in codec: the images that Apple Color Emoji stores in its sbix strikes on iOS.
//
// Apple does not document the format. The layout here follows the decoders below, with the rounding that reproduces the
// PNG strikes of the same glyphs exactly.
//
// emjc-decoder - https://github.com/cc4966/emjc-decoder
// EmojiFonts - https://github.com/PoomSmart/EmojiFonts/blob/main/EMJC.md

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `bytes` begin with the signature of an emjc image.
bool emjcHasSignature(PrByteView bytes);

// An emjc image, as its header describes it. `stream` refers to the bytes that the header was read from.
struct EmjcImage {
    PrByteView stream;              // the LZFSE stream of the image's planes
    uint32_t width;
    uint32_t height;
    uint32_t appendixSize;          // the bytes of the offsets of the residuals that exceed a byte
};

// Reads the header of the image in `bytes`. Returns InvalidData for bytes without a header or an image of no pixels.
PrStatus emjcImage(PrByteView bytes, EmjcImage* out);

// Returns the size of the decompressed planes of `image`, which emjcDecode allocates.
uint64_t emjcPlanesSize(const EmjcImage& image);

// Decodes `image` into `pixels` as tightly packed, straight-alpha Rgba8, decompressing its planes through `codec`.
// Returns false if the planes do not decompress to their size, a row names no filter, or `pixelsSize` is not the size
// of the image.
bool emjcDecode(PrCompressCodec* codec, const EmjcImage& image, uint8_t* pixels, size_t pixelsSize);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
