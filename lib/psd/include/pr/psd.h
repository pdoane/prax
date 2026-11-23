// PSD and PSB: the documents of Adobe Photoshop, read for their flattened composite, the merged image that the file
// stores after its layers. A file is a header, the color mode data, the image resources, the layer and mask
// information, and the merged image's channels, stored as they are or compressed with PackBits. The image resources
// give the ICC profile, the Exif data, the resolution and a JPEG thumbnail. The layers are not read.
//
// Adobe Photoshop File Formats Specification (2019) -
// https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/

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

// The size of a file's header, which begins with "8BPS" and its version: 1 for PSD, and 2 for PSB.
enum { kPrPsdHeaderBytes = 26 };

// Returns true if `bytes` begin with the signature and version of a PSD or PSB file.
bool prPsdHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrPsdLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the pixels
    uint64_t work;          // the samples decoded
} PrPsdLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrPsdLimits prPsdLimitsDefault(void);

// The color modes of the header.
enum {
    PrPsdMode_Bitmap = 0,
    PrPsdMode_Grayscale = 1,
    PrPsdMode_Indexed = 2,
    PrPsdMode_Rgb = 3,
    PrPsdMode_Cmyk = 4,
    PrPsdMode_Multichannel = 7,
    PrPsdMode_Duotone = 8,
    PrPsdMode_Lab = 9,
};
typedef uint8_t PrPsdMode;

// The channels of the decoded pixels, before alpha.
enum {
    PrPsdColors_Gray,       // Bitmap, Grayscale and Duotone: black at zero
    PrPsdColors_Rgb,        // RGB, and Indexed through its color table
    PrPsdColors_Cmyk,       // ink, full at one
    PrPsdColors_Lab,        // L in [0, 100], and a and b about zero, as floats
};
typedef uint8_t PrPsdColors;

// The storage type of one channel of the decoded pixels, in the machine's byte order. Unsigned integers map to [0, 1].
enum {
    PrPsdSample_U8,         // of depths 1 and 8
    PrPsdSample_U16,
    PrPsdSample_F32,        // of depth 32, which holds linear values, and of Lab
};
typedef uint8_t PrPsdSample;

// A document, as its header and its image resources describe it. The decoded pixels are the merged image's color
// channels interleaved, then its transparency if `alpha` is set, straight. Views are of the file's bytes.
typedef struct PrPsdInfo {
    PrUVec2 extent;
    PrPsdMode mode;
    uint16_t depth;             // bits of a channel: 1, 8, 16 or 32
    PrPsdColors colors;
    PrPsdSample sample;
    uint8_t channels;           // of a decoded pixel
    bool alpha;
    PrByteView profile;         // the ICC profile (resource 0x040F), or empty
    PrByteView exif;            // the Exif data (resource 0x0422), a TIFF structure, or empty
    PrByteView thumbnail;       // the JFIF data of the JPEG thumbnail (resource 0x040C), or empty
    PrVec2 pixelsPerMeter;      // the resolution (resource 0x03ED), or zero
    uint64_t workingBytes;      // the most bytes that the decoder holds, including those its caller holds for it
} PrPsdInfo;

// A document open for reading. One thread uses a decoder at a time.
typedef struct PrPsdDecoder PrPsdDecoder;

// Opens the document in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the defaults
// if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the info's
// workingBytes counts with the decoder's own.
//
// The first extra channel is alpha if the layer information says that it holds the merged image's transparency, and the
// color is then unmatted from white, as Photoshop stores it. Returns InvalidData without the signature, for fields or
// sections that break the format, and for a depth that the mode does not have. Returns NotSupported for the
// Multichannel mode, for a merged image compressed with ZIP, and for a document whose version information says that it
// has no real merged image, as one saved without maximized compatibility does: its composite is blank. Returns Limit
// past the limits. Returns Truncated, with a decoder, for bytes that end after the image resources and the start of the
// layer information but before the merged image's data, as a document whose bytes are still arriving does: the info
// describes the document, its thumbnail among it, and a read clears the pixels and returns Truncated. `*out` is the
// decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prPsdDecoderCreate(PrByteView bytes, const PrPsdLimits* PR_NULLABLE limits, uint64_t held,
    PrPsdDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prPsdDecoderDestroy(PrPsdDecoder* PR_NULLABLE decoder);

// Returns what the decoder's document is.
const PrPsdInfo* prPsdDecoderInfo(const PrPsdDecoder* decoder);

// Decodes the merged image into `output`, of its extent, from the top row. An index past the color table is black.
// Returns Truncated if the bytes end before the image does, after which the channels that they hold are decoded and the
// rest are zero, and InvalidData for PackBits runs that break the format.
PrStatus prPsdDecoderRead(PrPsdDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
