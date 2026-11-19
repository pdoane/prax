// Radiance HDR: the pictures of the Radiance lighting system (.hdr, .pic), whose pixels are three 8-bit mantissas and
// one shared 8-bit exponent, and hold radiance in linear light.
//
// Ward describes the pixels in "Real Pixels" (Graphics Gems II, 1991), and Radiance's file formats document describes
// the header, the resolution line, and the run-length encodings of a scanline.

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

// The bytes of the start of a Radiance picture that prHdrHasSignature reads.
enum { kPrHdrSignatureBytes = 8 };

// Returns true if `bytes` begin with the first line of a Radiance picture's header.
bool prHdrHasSignature(PrByteView bytes);

// One axis of a picture's resolution line: Radiance's x axis, which runs to the right, or its y axis, which runs up,
// whether the stored order runs against it, and the pixels along it.
typedef struct PrHdrAxis {
    bool y;
    bool decreasing;
    uint32_t length;
} PrHdrAxis;

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrHdrLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the pixels
    uint64_t work;          // the pixels decoded
} PrHdrLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrHdrLimits prHdrLimitsDefault(void);

// A Radiance picture, as its header and its resolution line describe it.
typedef struct PrHdrInfo {
    PrUVec2 extent;         // the pixels of a scanline, by the scanlines

    // The axis that the scanlines follow one another along, the resolution line's first, and the axis along a scanline,
    // its second.
    PrHdrAxis scanlineAxis;
    PrHdrAxis pixelAxis;

    // The factor that the picture's pixels were multiplied by, which decoding divides out: the product of the header's
    // EXPOSURE values, or 1.
    double exposure;

    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrHdrInfo;

// A Radiance picture open for reading. One thread uses a decoder at a time.
typedef struct PrHdrDecoder PrHdrDecoder;

// Opens the picture in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the defaults
// if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the info's
// workingBytes counts with the decoder's own. Returns InvalidData for bytes without a whole header, a malformed
// resolution line or a picture of no pixels, NotSupported for a picture whose pixels are not red, green and blue, such
// as one in CIE XYZ, and Limit past the limits. `*out` is the decoder if the status is Ok, and null otherwise.
PrStatus prHdrDecoderCreate(PrByteView bytes, const PrHdrLimits* PR_NULLABLE limits, uint64_t held,
    PrHdrDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prHdrDecoderDestroy(PrHdrDecoder* PR_NULLABLE decoder);

// Returns what the decoder's picture is.
const PrHdrInfo* prHdrDecoderInfo(const PrHdrDecoder* decoder);

// Decodes the picture into `output`, of its extent, as rows of red, green and blue 32-bit floats, in the stored order
// of the scanlines. A pixel is its mantissas, each plus one half, times two to the power of its exponent less 136, over
// the picture's exposure, and black for an exponent of zero. Returns InvalidData if the scanlines end early or are
// malformed, after which the pixels are unspecified.
PrStatus prHdrDecoderRead(PrHdrDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
