// ICO decoding for the built-in codec: choosing an image from the directory, and decoding a bitmap image.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An image chosen from an ICO directory, still encoded.
struct IcoImage {
    const uint8_t* bytes;           // the encoded image
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t headerSize;
    uint32_t paletteCount;
    uint16_t bitCount;
    bool png;                       // true if the image is a PNG instead of a bitmap
};

// Chooses the largest complete image in `bytes` that the codec reads. Among images of equal area, it chooses the one
// with the greatest bit depth. Returns false for a malformed directory or one without such an image. `bytes` must stay
// valid until the chosen image is decoded.
bool icoSelectImage(PrByteView bytes, IcoImage* out);

// Decodes the bitmap `image` into `pixels` as tightly packed, straight-alpha sRGB Rgba8. Returns false if `image` is a
// PNG or `pixelsSize` is not the size of the image.
bool icoDecodeBitmap(const IcoImage& image, uint8_t* pixels, size_t pixelsSize);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
