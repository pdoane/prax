// Draw images: pixels that a list draws. An image is immutable once it is created.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/draw.h"
#include "pr/image.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Pixels that commands draw from. An image is immutable and reference counted, and any thread may draw and retain it.
typedef struct PrDrawImage PrDrawImage;

// Pixels to copy into an image. The format is Rgba8, Bgra8 or RgbaF16.
typedef struct PrDrawImageDesc {
    PrPixelView pixels;
    PrPixelFormat format;
    PrDrawAlpha alpha;          // how the alpha relates to the color channels
    PrColorSpace space;         // what the color channels mean
} PrDrawImageDesc;

// Creates an image that holds a copy of the pixels, which must not be empty. An image in sRGB keeps the pixels as
// given. An image in any other space is converted to extended sRGB, as RgbaF16. Returns Limit for pixels too large to
// address. On failure, `*out` is null.
PrStatus prDrawImageCreate(const PrDrawImageDesc* desc, const PrDrawImage* PR_NULLABLE* PR_NONNULL out);

// Creates an image of the first frame of the encoded image in `bytes`, decoded by `codec` and arranged as its
// orientation displays it. An image that names a space other than sRGB, such as BT.2100 PQ, keeps its range as extended
// sRGB in half floats; any other is sRGB in 8 bits. Writes the image's orientation to `orientation` if it is not null.
// Returns NotSupported if the codec cannot read the image, and InvalidData if the image is corrupt. On failure, `*out`
// is null.
PrStatus prDrawImageCreateFromBytes(PrImageCodec* codec, PrByteView bytes,
                                    const PrDrawImage* PR_NULLABLE* PR_NONNULL out,
                                    PrImageOrientation* PR_NULLABLE orientation);

// Creates a copy of `image` with its pixels arranged as `orientation` maps them.
const PrDrawImage* prDrawImageCreateOriented(const PrDrawImage* image, PrImageOrientation orientation);

// Adds or releases a reference. The last release frees the image.
void prDrawImageRetain(const PrDrawImage* image);
void prDrawImageRelease(const PrDrawImage* PR_NULLABLE image);

// Returns the image's extent in pixels, its format, and how its alpha relates to its color.
PrUVec2 prDrawImageExtent(const PrDrawImage* image);
PrPixelFormat prDrawImageFormat(const PrDrawImage* image);
PrDrawAlpha prDrawImageAlpha(const PrDrawImage* image);

// Returns the pixels, in the image's format. They are valid for the lifetime of the image.
PrPixelView prDrawImagePixels(const PrDrawImage* image);

// Returns a number that is unique to the image for the life of the process. A backend uses it as a key for data that it
// derives from the image.
uint64_t prDrawImageId(const PrDrawImage* image);

// Copies the image's pixels over those of `target`, with the image's top left at `origin`. This is the operation that a
// WritePixels command performs. `target` is premultiplied extended sRGB: Rgba8 or Bgra8, which clamp colors to [0, 1],
// or RgbaF16, which keeps them. Pixels outside the target are dropped.
void prDrawImageWritePixels(const PrDrawImage* image, PrPixels target, PrPixelFormat format, PrIVec2 origin);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
