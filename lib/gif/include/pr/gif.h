// GIF (GIF89a and GIF87a): an image's logical screen, its frames, their disposal and their delays, and their pixels.

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

// The size of the signature and version that begin a GIF file.
enum { kPrGifSignatureBytes = 6 };

// Returns true if `bytes` begin with the GIF signature and a version that the decoder reads.
bool prGifHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrGifLimits {
    uint64_t pixels;        // of the screen, and of each frame
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of one frame's pixels
    uint64_t work;          // the blocks walked, and the bytes of the file and the pixels of its frames
    uint32_t frames;
    uint32_t metadataBytes; // of the extension blocks
} PrGifLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrGifLimits prGifLimitsDefault(void);

// A GIF image, as its blocks describe it. Its frames decode to 8-bit RGBA, straight alpha, with sRGB colors.
typedef struct PrGifInfo {
    PrUVec2 extent;         // the logical screen
    uint32_t frameCount;    // the frames whose blocks the file holds whole, which may be none
    uint32_t playCount;     // the plays of a NETSCAPE2.0 loop, or zero for forever, and one without the extension
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrGifInfo;

// A GIF image open for reading: its frames, and the composition of the screen. One thread uses a decoder at a time.
typedef struct PrGifDecoder PrGifDecoder;

// Opens the GIF image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own. Returns NotSupported without the signature, InvalidData for blocks
// that break the format, Limit past the limits, and Truncated for a file that ends before its trailer, with the frames
// whose blocks it holds whole. `*out` is the decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prGifDecoderCreate(PrByteView bytes, const PrGifLimits* PR_NULLABLE limits, uint64_t held,
    PrGifDecoder* PR_NULLABLE* PR_NONNULL out);

// Creates a decoder of a GIF whose bytes arrive in pieces, which the caller appends (prGifDecoderAppend), within
// `limits`, or the defaults if they are null. `held` is as for prGifDecoderCreate. The decoder keeps the bytes that are
// appended. Its info is zero until the screen's descriptor and global table arrive.
PrStatus prGifDecoderCreateStream(const PrGifLimits* PR_NULLABLE limits, uint64_t held,
    PrGifDecoder* PR_NULLABLE* PR_NONNULL out);

// Appends the next bytes of the file to a decoder that prGifDecoderCreateStream created, and decodes what they
// complete: each byte is walked once, and the codes of the frame that is arriving expand and draw onto the screen as
// they arrive, so the work of all the appends is proportional to the file and the pixels, however the bytes are
// divided. Returns Ok once the trailer has arrived, Truncated before, and InvalidData or Limit, which the decoder
// keeps.
//
// The info counts the frames that have begun. prGifDecoderRead reads the frame that is arriving as far as it has
// arrived, and a frame before it whole.
PrStatus prGifDecoderAppend(PrGifDecoder* decoder, PrByteView bytes);

// Returns the work that a decoder of appended bytes has done: the bytes that its appends read, the codes that they
// expanded, and the pixels of the frames that its reads composed. A read of a frame before the arriving one continues
// from the frame that the last read composed, so reading the frames in order composes each once. A caller measures that
// the work is proportional to the file and to the frames read with it.
uint64_t prGifDecoderWork(const PrGifDecoder* decoder);

// Frees the decoder and what it holds.
void prGifDecoderDestroy(PrGifDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrGifInfo* prGifDecoderInfo(const PrGifDecoder* decoder);

// Returns how long `frame` shows, in milliseconds as encoded. The image has the frame.
uint32_t prGifDecoderFrameDuration(const PrGifDecoder* decoder, uint32_t frame);

// Decodes `frame`, which the image has, into `output`, of the screen's extent, composed over the frames before it from
// a transparent screen. Composing the frames in order continues the composition, and any other order restarts it.
// Returns InvalidData for a frame that does not decode, and Truncated for a frame whose data the bytes cut short, whose
// pixels past the data are left as the frames before it left them.
PrStatus prGifDecoderRead(PrGifDecoder* decoder, uint32_t frame, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
