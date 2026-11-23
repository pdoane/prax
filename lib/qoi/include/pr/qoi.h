// QOI, the Quite OK Image format: 8-bit RGB or RGBA pixels, each coded as a run, an index into the pixels seen before,
// a small difference from the pixel before, or the pixel itself.
//
// The Quite OK Image Format Specification, version 1.0 - https://qoiformat.org/qoi-specification.pdf

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The size of a file's header, which begins with the magic "qoif".
enum { kPrQoiHeaderBytes = 14 };

// Returns true if `bytes` begin with the magic of a QOI file.
bool prQoiHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrQoiLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the pixels
    uint64_t work;          // the pixels decoded
} PrQoiLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrQoiLimits prQoiLimitsDefault(void);

// The meaning of the pixels, as the header's colorspace field gives it. The field is informative: it does not change
// how the pixels are coded.
enum {
    PrQoiColorspace_Srgb,       // sRGB color with linear alpha
    PrQoiColorspace_Linear,     // every channel linear
};
typedef uint8_t PrQoiColorspace;

// A QOI image, as its header describes it. `channels` is 3 for RGB and 4 for RGBA, and is the channels that a read
// writes.
typedef struct PrQoiInfo {
    PrUVec2 extent;
    uint8_t channels;
    PrQoiColorspace colorspace;
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrQoiInfo;

// A QOI image open for reading. One thread uses a decoder at a time.
typedef struct PrQoiDecoder PrQoiDecoder;

// Opens the image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the defaults if
// they are null. `held` is the bytes that the caller holds for it already, its input among them, which the info's
// workingBytes counts with the decoder's own. Returns NotSupported without the magic, InvalidData for a header without
// pixels or with channels or a colorspace that the specification does not define, and Limit past the limits. `*out` is
// the decoder if the status is Ok, and null otherwise.
PrStatus prQoiDecoderCreate(PrByteView bytes, const PrQoiLimits* PR_NULLABLE limits, uint64_t held,
    PrQoiDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prQoiDecoderDestroy(PrQoiDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrQoiInfo* prQoiDecoderInfo(const PrQoiDecoder* decoder);

// Decodes the image into `output`, of its extent, as rows of `channels` 8-bit channels from the top. Returns Truncated
// if the bytes end before the pixels do, after which the pixels that were coded are decoded and the rest are
// transparent black, and InvalidData if a run passes the last pixel.
PrStatus prQoiDecoderRead(PrQoiDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
