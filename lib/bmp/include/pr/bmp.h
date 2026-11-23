// BMP: Windows bitmaps, the device-independent bitmaps (DIBs) that a BMP file holds after its file header and that an
// icon holds as its bitmap images. A DIB is an information header of one of the versions of Windows and OS/2, a color
// table, and rows of 1 to 32 bits a pixel, stored as they are, as bit fields or run-length encoded.
//
// Bitmap Storage - https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-storage
// BITMAPV5HEADER - https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv5header
// Icons - https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)

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

// The size of the file header that begins a BMP file, and of the start of the information header after it, whose size
// names its version.
enum { kPrBmpSignatureBytes = 18 };

// Returns true if `bytes` begin with a BMP file header: "BM", reserved fields of zero, and the size of an information
// header of a known version.
bool prBmpHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrBmpLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the pixels
    uint64_t work;          // the pixels decoded
} PrBmpLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrBmpLimits prBmpLimitsDefault(void);

// Where a DIB comes from, which decides how its fields are read.
enum {
    // A BMP file: the file header, which locates the pixels, and then the DIB.
    PrBmpSource_File,
    // An icon's bitmap image: a DIB whose height counts its rows twice, for the color bitmap and the 1-bit AND mask
    // that follows it. The mask makes a pixel transparent where its bit is set, unless a 32-bit bitmap has alpha of its
    // own.
    PrBmpSource_Icon,
};
typedef uint8_t PrBmpSource;

// The meaning of the color of the pixels, from the color space type of a version 4 or 5 header.
enum {
    PrBmpColor_Unknown,     // calibrated RGB, a linked profile, or no color space type
    PrBmpColor_Srgb,        // LCS_sRGB, or LCS_WINDOWS_COLOR_SPACE, the system's default space, which is sRGB
    PrBmpColor_Profile,     // PROFILE_EMBEDDED: the ICC profile that the header locates
};
typedef uint8_t PrBmpColor;

// A DIB, as its headers describe it. The decoded pixels are rows of 8-bit RGB, or RGBA with straight alpha, from the
// top row, whatever the order in which the rows are stored.
typedef struct PrBmpInfo {
    PrUVec2 extent;
    uint8_t channels;           // 3, or 4 with alpha
    uint16_t bitCount;          // of a stored pixel
    PrBmpColor color;
    PrByteView profile;         // the embedded ICC profile for Profile, a view of the DIB's bytes
    PrVec2 pixelsPerMeter;      // the header's resolution, or zero if it has none
    uint64_t workingBytes;      // the most bytes that the decoder holds, including those its caller holds for it
} PrBmpInfo;

// A DIB open for reading. One thread uses a decoder at a time.
typedef struct PrBmpDecoder PrBmpDecoder;

// Opens the DIB in `bytes` from `source`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own.
//
// A DIB has alpha if its bit fields have an alpha mask, if it is run-length encoded, since the codes may leave pixels
// unwritten and those are transparent, or if it is an icon's. Returns NotSupported without the file header of a file,
// for a JPEG or PNG image that a DIB embeds, and for the compressions of OS/2's version 2 header, and for 64-bit
// pixels. Returns InvalidData for fields that break the format, and Limit past the limits. Returns Truncated if the
// pixels, or an icon's mask, end before the bytes do, with the decoder open: a read then decodes what the bytes hold.
// `*out` is the decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prBmpDecoderCreate(PrByteView bytes, PrBmpSource source, const PrBmpLimits* PR_NULLABLE limits, uint64_t held,
    PrBmpDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prBmpDecoderDestroy(PrBmpDecoder* PR_NULLABLE decoder);

// Returns what the decoder's DIB is.
const PrBmpInfo* prBmpDecoderInfo(const PrBmpDecoder* decoder);

// Decodes the DIB into `output`, of its extent and channels. An index past the color table is black. A DIB whose alpha
// mask gives zero in every pixel is opaque instead, and an icon's 32-bit bitmap whose alpha is zero in every pixel
// takes its transparency from its mask. Returns Truncated if the bytes end before the pixels do, after which the pixels
// that they hold are decoded and the rest are transparent black, and InvalidData for run-length codes that break the
// format.
PrStatus prBmpDecoderRead(PrBmpDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
