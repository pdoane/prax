// Targa (TGA): Truevision's image files of color-mapped, true-color and black and white pixels, stored as they are or
// run-length encoded, and the TGA 2.0 footer and extension area that say what their alpha means.
//
// Truevision TGA File Format Specification, version 2.0 (1991).

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

// The size of a file's header, and of the TGA 2.0 footer that ends a file.
enum {
    kPrTgaHeaderBytes = 18,
    kPrTgaFooterBytes = 26,
};

// Returns true if `bytes` begin with a header that a TGA file may have: an image type of the specification, a color map
// that the type allows, a pixel depth that the type stores, and pixels to decode. A TGA file has no signature, so a
// file of another format may pass. A caller that has the whole file also checks for the footer.
bool prTgaHasHeader(PrByteView bytes);

// Returns true if `bytes`, a whole file, end with the TGA 2.0 footer.
bool prTgaHasFooter(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrTgaLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the pixels
    uint64_t work;          // the pixels decoded
} PrTgaLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrTgaLimits prTgaLimitsDefault(void);

// What the alpha of the decoded pixels means. A file says it with the attribute bits of its image descriptor, and the
// extension area's attributes type replaces what those bits say.
enum {
    PrTgaAlpha_None,
    PrTgaAlpha_Straight,
    PrTgaAlpha_Premultiplied,
};
typedef uint8_t PrTgaAlpha;

// A TGA image, as its header and extension area describe it. The decoded pixels are interleaved 8-bit channels: gray,
// gray and alpha, RGB, or RGB and alpha, in the rows and columns of the stored order.
typedef struct PrTgaInfo {
    PrUVec2 extent;
    uint8_t channels;       // 1 to 4
    bool gray;
    PrTgaAlpha alpha;

    // The image descriptor's origin. `fromRight` is true if the columns run from the right. `fromBottom` is true if the
    // rows run from the bottom, as they do in most files.
    bool fromRight;
    bool fromBottom;

    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrTgaInfo;

// A TGA image open for reading. One thread uses a decoder at a time.
typedef struct PrTgaDecoder PrTgaDecoder;

// Opens the image in `bytes`, the whole file, which must stay valid until the decoder is destroyed, within `limits`, or
// the defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which
// the info's workingBytes counts with the decoder's own. Returns InvalidData for a header that prTgaHasHeader refuses
// or a color map past the end of the file, NotSupported for indices of more than 8 bits, and Limit past the limits.
// `*out` is the decoder if the status is Ok, and null otherwise.
PrStatus prTgaDecoderCreate(PrByteView bytes, const PrTgaLimits* PR_NULLABLE limits, uint64_t held,
    PrTgaDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prTgaDecoderDestroy(PrTgaDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrTgaInfo* prTgaDecoderInfo(const PrTgaDecoder* decoder);

// Decodes the image into `output`, of its extent. A 15-bit or 16-bit color widens each 5-bit channel to 8 bits. Returns
// Truncated if the bytes end before the pixels do, after which the pixels that were stored are decoded and the rest are
// zero, and InvalidData for an index past the color map.
PrStatus prTgaDecoderRead(PrTgaDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
