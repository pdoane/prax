// JPEG (ITU T.81, with JFIF and Exif): Huffman-coded, 8-bit, sequential or progressive images, their application
// segments' metadata, and their pixels.

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

// The size of the start-of-image marker that begins a JPEG file.
enum { kPrJpegSignatureBytes = 2 };

// Returns true if `bytes` begin with a start-of-image marker.
bool prJpegHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrJpegLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the image's pixels
    uint64_t work;          // the segments walked, and the samples of each scan
    uint32_t metadataBytes; // of the application and comment segments
} PrJpegLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrJpegLimits prJpegLimitsDefault(void);

// A JPEG image, as its frame header and its segments describe it. The decoded pixels are interleaved 8-bit samples of
// `components`: gray, RGB, or CMYK, which an Adobe segment's transform turns from YCbCr or YCCK. The views are of the
// file's bytes, but for a profile in more than one segment, which the decoder joins and holds.
typedef struct PrJpegInfo {
    PrUVec2 extent;
    uint32_t components;    // 1, 3 or 4
    PrByteView exif;        // the Exif data of an APP1 segment, a TIFF structure, or empty
    PrByteView profile;     // the ICC profile of the APP2 segments that carry it, joined in their order, or empty
    PrVec2 pixelsPerMeter;  // the JFIF segment's density, or zero if it gives an aspect ratio alone
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrJpegInfo;

// A JPEG image open for reading. One thread uses a decoder at a time.
typedef struct PrJpegDecoder PrJpegDecoder;

// Opens the JPEG image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own. Returns NotSupported without the start-of-image marker or for a
// coding that the decoder does not decode, such as arithmetic coding or 12-bit samples, InvalidData for segments that
// break the format, Limit past the limits, and Truncated for a file that ends before its scans do. `*out` is the
// decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prJpegDecoderCreate(PrByteView bytes, const PrJpegLimits* PR_NULLABLE limits, uint64_t held,
    PrJpegDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prJpegDecoderDestroy(PrJpegDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrJpegInfo* prJpegDecoderInfo(const PrJpegDecoder* decoder);

// The number of reductions that a decoder offers. Reduction `r` divides the image's extent by 2 to the power `r`,
// rounded up. Reduction 0 is the whole image, and reduction 3 is an eighth of it.
enum { kPrJpegReductionCount = 4 };

// Returns the extent of an image of `extent` decoded at `reduction`.
PrUVec2 prJpegReducedExtent(PrUVec2 extent, uint32_t reduction);

// Decodes the image at `reduction` into `output`, which has the extent that prJpegReducedExtent returns.
//
// A reduced decode transforms only the lowest frequencies of each block, at the block's reduced size (T.81 A.3.3). At
// an eighth, it takes only the block's average, so the decode costs little more than the entropy decoding.
//
// Returns InvalidData if the image does not decode.
PrStatus prJpegDecoderRead(PrJpegDecoder* decoder, uint32_t reduction, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
