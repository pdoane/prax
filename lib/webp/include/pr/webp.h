// WebP (rfc 9649): the RIFF container and its extended format, lossy and lossless bitstreams, alpha, and animation.

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

// The size of the RIFF header that begins a WebP file.
enum { kPrWebpSignatureBytes = 12 };

// Returns true if `bytes` begin with the RIFF header of a WebP file.
bool prWebpHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrWebpLimits {
    uint64_t inputBytes;
    uint64_t pixels;        // of the canvas, and of each frame
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of one frame's pixels
    uint64_t work;          // the bytes of the file and the pixels of its frames, and of the canvas for each frame
    uint32_t frames;
    uint32_t metadataBytes; // of the profile, the Exif and the XMP chunks together
} PrWebpLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrWebpLimits prWebpLimitsDefault(void);

// A WebP image, as its chunks describe it. The views are of the file's bytes.
typedef struct PrWebpInfo {
    PrUVec2 extent;         // the canvas
    uint32_t frameCount;    // the frames whose chunks the file holds whole
    uint32_t playCount;     // the plays of an animation, or zero for forever, and one for a still image
    bool animated;
    bool alpha;             // the header or the bitstream says that the image has alpha
    PrByteView profile;     // the ICC profile, or empty: the color is sRGB's without one (rfc 9649 2.7.1.4)
    PrByteView exif;        // the Exif metadata, a TIFF structure, or empty
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrWebpInfo;

// A WebP image open for reading: its chunks, the storage that decoding a still frame works through, and the composition
// of an animation. One thread uses a decoder at a time.
typedef struct PrWebpDecoder PrWebpDecoder;

// Opens the WebP image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own. Returns NotSupported without the RIFF header, InvalidData for
// chunks that break the format, Limit past the limits, and Truncated for a file that ends before its RIFF size, with
// the frames whose chunks it holds whole. `*out` is the decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prWebpDecoderCreate(PrByteView bytes, const PrWebpLimits* PR_NULLABLE limits, uint64_t held,
    PrWebpDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prWebpDecoderDestroy(PrWebpDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrWebpInfo* prWebpDecoderInfo(const PrWebpDecoder* decoder);

// Returns how long `frame` shows, in milliseconds as encoded, or zero for a still image. The image has the frame.
uint32_t prWebpDecoderFrameDuration(const PrWebpDecoder* decoder, uint32_t frame);

// Decodes `frame`, which the image has, into `output`, of the canvas's extent: 8-bit RGB, or RGBA with straight alpha
// if the image has alpha. A lossy picture's chroma is upsampled as libwebp upsamples it by default: each pixel takes
// its chroma from the four nearest samples, weighted 9, 3, 3 and 1 by nearness. An animation's canvas starts
// transparent, as a frame disposed to the background leaves its rectangle, and its frames blend in 8-bit sRGB: the
// background color that the file gives is a hint, which the composition does not take. Composing the frames in order
// continues the composition, and any other order restarts it. Returns InvalidData for a frame that does not decode, and
// Limit for one that would hold more than the limits allow.
PrStatus prWebpDecoderRead(PrWebpDecoder* decoder, uint32_t frame, PrPixels output);

// The planes of a lossy WebP picture as its bitstream codes them, before its chroma is upsampled: 8-bit Y'CbCr of
// BT.601's limited range, the chroma at half the luma's width and height, rounded up.
typedef struct PrWebpPlanes {
    PrPixels luma;
    PrPixels blueDifference;
    PrPixels redDifference;
} PrWebpPlanes;

// Decodes the lossy still image in `file` within `limits`, or the defaults if they are null, into `planes`, whose
// extents are the picture's. Returns NotSupported for a lossless or an animated image, and InvalidData if the extents
// are not the picture's. Its alpha, if any, is not decoded.
PrStatus prWebpDecodePlanes(PrByteView file, const PrWebpLimits* PR_NULLABLE limits, const PrWebpPlanes* planes);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
