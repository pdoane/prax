// OpenEXR: scan line and tiled images, of one part or several, with their mip and rip levels, in any of the file
// format's compressions but HTJ2K. A chunk compressed with zlib inflates through the compression codec that an image is
// given.
//
// OpenEXR File Layout - https://openexr.com/en/latest/OpenEXRFileLayout.html Technical Introduction to OpenEXR -
// https://openexr.com/en/latest/TechnicalIntroduction.html

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/compress.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bytes that begin an OpenEXR file: its magic number, 20000630, little-endian.
enum { kPrExrSignatureBytes = 4 };

// Returns true if `bytes` begin with the magic number of an OpenEXR file.
bool prExrHasSignature(PrByteView bytes);

// The largest amounts of resources that an image may use. Each is a hard limit.
typedef struct PrExrLimits {
    uint64_t pixels;        // of a part's display window, and of its data window
    uint64_t workingBytes;  // the total bytes that an image holds, including those its caller holds for it
    uint64_t work;          // the channels, the chunks, and the samples of a part's levels
    uint32_t parts;
    uint32_t metadataBytes; // of the headers' attributes
} PrExrLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrExrLimits prExrLimitsDefault(void);

// How a channel stores a sample, by the value of its type in the channel list.
enum {
    PrExrSample_Uint,       // a 32-bit unsigned integer
    PrExrSample_Half,       // an IEEE binary16 float
    PrExrSample_Float,      // an IEEE binary32 float
};
typedef uint8_t PrExrSample;

// The compressions of OpenEXR, by the value of the header's compression attribute.
enum {
    PrExrCompression_None,
    PrExrCompression_Rle,
    PrExrCompression_Zips,
    PrExrCompression_Zip,
    PrExrCompression_Piz,
    PrExrCompression_Pxr24,
    PrExrCompression_B44,
    PrExrCompression_B44a,
    PrExrCompression_Dwaa,
    PrExrCompression_Dwab,
    PrExrCompression_Htj2k256,
    PrExrCompression_Htj2k32,
};
typedef uint8_t PrExrCompression;

// The order in which an image's lines were written, which does not change how they decode.
enum {
    PrExrLineOrder_IncreasingY,
    PrExrLineOrder_DecreasingY,
    PrExrLineOrder_RandomY,
};
typedef uint8_t PrExrLineOrder;

// How a part's levels relate to its extent: one level, a mip chain, whose levels each halve the one before, or a rip
// map, which has a level of every combination of a reduction across and a reduction down, in rows of `levelColumns`,
// one row for each height and one column for each width, from the full extent.
enum {
    PrExrLevels_One,
    PrExrLevels_Mip,
    PrExrLevels_Rip,
};
typedef uint8_t PrExrLevels;

// What the selected part of an image is, and the pixels that prExrImageRead decodes it to: rows of `colors` color
// channels, red, green and blue or gray, and premultiplied alpha if `alpha`, each of `sample`, a Half or a Float.
//
// The color is the channels R, G and B, any that are missing being zero, or Y with RY and BY, which convert to RGB by
// the luminance of the part's primaries, or Y alone as gray, or else the part's first channel as gray. A channel A is
// alpha. The color channels are those without a layer's prefix, or else those of the first layer, in the order of the
// channel list, that has any, as `diffuse.R`. The samples are halves if every channel of the color and alpha is a half,
// and floats otherwise, an integer as its value. A subsampled channel's sample covers each of its pixels. The values
// are scene-linear in the RGB space of `chromaticities`: the file's, or Rec. ITU-R BT.709's if it gives none.
//
// The extent is the display window's, and the first level holds the pixels of the data window where they lie in it,
// with the rest zero. A tiled part has the levels of its mip chain or rip map, each further level of its own extent, of
// the data window alone.
typedef struct PrExrPartInfo {
    PrUVec2 extent;
    PrExrLevels levels;
    uint32_t levelCount;        // at least one
    uint32_t levelColumns;      // a rip map's levels across, and one otherwise
    uint32_t channelCount;
    uint32_t attributeCount;
    uint8_t colors;
    bool alpha;
    PrExrSample sample;
    PrColorChromaticities chromaticities;
    uint64_t workingBytes;      // the most bytes that the image holds, including those its caller holds for it
} PrExrPartInfo;

// One channel of the selected part, as the file names and stores it. A subsampled channel has a sample for each
// `sampling` pixels across and down. A perceptual channel holds values that are perceptually linear rather than linear
// light, as OpenEXR's pLinear says. The name is a view of the file's bytes.
typedef struct PrExrChannel {
    PrStringView name;
    PrExrSample sample;
    PrUVec2 sampling;
    bool perceptual;
} PrExrChannel;

// One attribute of the selected part's header: its name, the name of its type, and its value as the file stores it. The
// views are of the file's bytes.
typedef struct PrExrAttribute {
    PrStringView name;
    PrStringView type;
    PrByteView value;
} PrExrAttribute;

// A rectangle of pixels, from its first pixel to its last, inclusive.
typedef struct PrExrBox {
    int32_t xMin;
    int32_t yMin;
    int32_t xMax;
    int32_t yMax;
} PrExrBox;

// What the header of the selected part gives beyond its info: its data window, the pixels that the file holds, its
// display window, the extent of the image, the width over the height of a pixel, the screen window of its perspective,
// its line order and compression, the extent of the tiles that its tile description gives, which is zero if it gives
// none, and its part's name, type and view, which are empty if the header gives none. The views are of the file's
// bytes.
typedef struct PrExrHeader {
    PrExrBox dataWindow;
    PrExrBox displayWindow;
    float pixelAspectRatio;
    PrVec2 screenWindowCenter;
    float screenWindowWidth;
    PrExrLineOrder lineOrder;
    PrExrCompression compression;
    PrUVec2 tileExtent;
    PrStringView name;
    PrStringView type;
    PrStringView view;
} PrExrHeader;

// An OpenEXR file open for reading: the headers of its parts, the offset table of the selected part, and what decoding
// its chunks works through. One thread uses an image at a time.
typedef struct PrExrImage PrExrImage;

// Opens the file in `bytes`, which must stay valid until the image is destroyed, within `limits`, or the defaults if
// they are null, and selects its first part that the decoder reads. `held` is the bytes that the caller holds for it
// already, its input among them, which the part info's workingBytes counts with the image's own. Chunks decompress
// through the set `compress`, which outlives the image, and a chunk of zlib is NotSupported without one. Returns
// InvalidData for headers that break the file format, NotSupported for a version or a part that the decoder does not
// read, such as deep data or HTJ2K, Limit past the limits, and Truncated for a file that ends before the chunks of the
// part do, or whose offsets of chunks are missing, of which the chunks that it holds are read. `*out` is the image if
// the status is Ok or Truncated, and null otherwise.
PrStatus prExrImageCreate(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE compress,
    const PrExrLimits* PR_NULLABLE limits, uint64_t held, PrExrImage* PR_NULLABLE* PR_NONNULL out);

// Frees the image.
void prExrImageDestroy(PrExrImage* PR_NULLABLE image);

// Returns the number of parts of the file, at least one.
uint32_t prExrImagePartCount(const PrExrImage* image);

// Selects `part`, which the file has, with the statuses of prExrImageCreate. After a failure other than Truncated, the
// part that was selected before stays selected.
PrStatus prExrImageSelectPart(PrExrImage* image, uint32_t part);

// Returns the part that is selected, and what it is.
uint32_t prExrImageSelectedPart(const PrExrImage* image);
const PrExrPartInfo* prExrImagePartInfo(const PrExrImage* image);

// Returns the extent of `level` of the selected part, which it has.
PrUVec2 prExrImageLevelExtent(const PrExrImage* image, uint32_t level);

// Return channel `index` of the selected part, and attribute `index` of its header, which it has.
PrExrChannel prExrImageChannel(const PrExrImage* image, uint32_t index);
PrExrAttribute prExrImageAttribute(const PrExrImage* image, uint32_t index);

// Reads the header of the selected part from its attributes. A pixel aspect ratio, screen window or line order that the
// header leaves out has its default: 1, a window of width 1 at the origin, and increasing y. Returns InvalidData for a
// header without its windows.
PrStatus prExrImageHeaderRead(const PrExrImage* image, PrExrHeader* out);

// Decodes `level` of the selected part, which it has, into `output`, of the level's extent, in the format of the part's
// info. Returns InvalidData for a chunk that is corrupt, and Truncated if chunks are missing, whose pixels are zero.
PrStatus prExrImageRead(PrExrImage* image, uint32_t level, PrPixels output);

// Decodes the channels of `channels`, `count` of them, of `level` of the selected part, which it has, into `output`, of
// the level's extent: their samples interleaved in that order, each of `sample`. A subsampled channel's sample covers
// each of its pixels. Integers convert to floats by value, and floats to integers not at all. Returns NotSupported for
// a sample that a channel does not convert to, and the failures of prExrImageRead otherwise.
PrStatus prExrImageReadChannels(PrExrImage* image, uint32_t level, const uint32_t* channels, uint32_t count,
    PrExrSample sample, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
