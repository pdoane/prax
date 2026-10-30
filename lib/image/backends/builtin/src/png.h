// PNG decoding and encoding for the built-in codec.

#pragma once

#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/image.h"

#include "image/backends/builtin/src/metadata.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The largest number of bytes of expanded profile text that pngMetadata reads, whatever its `textLimit`.
constexpr size_t kPngProfileTextMaxBytes = 1u << 20;

// Reads the orientation and resolution from the ancillary chunks of the PNG in `bytes`. A field that the PNG does not
// give leaves its member of `*out` unchanged. `textLimit` bounds the expanded profile text. Scratch storage uses at
// most kPngProfileTextMaxBytes plus one byte.
PrStatus pngMetadata(PrCompressCodec* codec, PrByteView bytes, size_t textLimit, ImageMetadata* out);

// Returns the color space that the PNG in `bytes` names by its coding-independent code points: its cICP chunk (png
// 11.3.2.6), or else the cicp tag of its ICC profile (ICC.1 v4.4). Returns PrColorSpace_Count if neither names a space
// of PrColorSpace, as for narrow-range or YCbCr code points. The profile expands into at most kPngProfileTextMaxBytes.
PrColorSpace pngCicpSpace(PrCompressCodec* codec, PrByteView bytes);

// Writes the ICC profile of the PNG in `bytes`, its iCCP chunk expanded, to `*out`, which the caller frees with
// prMemoryFree, and its length to `*size`. Returns false if the PNG has no profile, or one that does not expand into at
// most `limit` bytes.
bool pngIccProfile(PrCompressCodec* codec, PrByteView bytes, size_t limit, uint8_t* PR_NULLABLE* PR_NONNULL out, size_t* size);

// Decodes the PNG in `bytes` into `pixels` as tightly packed samples in the format of the PNG, with straight alpha.
bool pngDecode(PrCompressCodec* codec, PrByteView bytes, uint8_t* pixels, size_t pixelsSize);

// An animated PNG being decoded. It keeps the composed image from one frame to the next.
struct PngAnimation;

// Scans the animation chunks of the PNG in `bytes` within `limits`, updates `info` with the animation that they
// describe, and opens the animation. For a PNG that is not animated, returns Ok and sets `*out` to null. `bytes` must
// stay valid until pngAnimationClose.
PrStatus pngAnimationOpen(PrCompressCodec* codec, PrByteView bytes, const PrImageLimits& limits, PrImageInfo& info,
    PngAnimation* PR_NULLABLE* PR_NONNULL out);

// Frees the animation and its storage.
void pngAnimationClose(PngAnimation* PR_NULLABLE animation);

// Returns the duration of `frame` in milliseconds, as encoded.
uint32_t pngAnimationDelay(const PngAnimation& animation, uint32_t frame);

// Decodes `frame`, composed over the frames before it, into `pixels` as tightly packed, straight-alpha samples in the
// format of the PNG. Decoding forward continues the composition, and going back restarts it.
bool pngAnimationFrame(PngAnimation& animation, uint32_t frame, uint8_t* pixels, size_t pixelsSize);

// Encodes `rgba8` as an 8-bit PNG. Each row is filtered, and the rows are compressed by `codec`, or stored if `codec`
// encodes nothing. The pixels are top-down, straight-alpha sRGB Rgba8. Writes up to `capacity` bytes to `out`, and
// returns the size of the whole encoding. `out` may be null if `capacity` is zero. Each call compresses the image
// again.
size_t pngEncode(PrPixelView rgba8, PrCompressCodec* codec, uint8_t* PR_NULLABLE out, size_t capacity);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
