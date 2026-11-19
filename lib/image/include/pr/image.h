// Image codecs and the sets that a program builds of them: decoding images, encoding them, and how their pixels are
// represented.
//
// A codec reads the formats of its backend, and each backend has its own constructor (pr/image_png.h, pr/image_jpeg.h,
// pr/image_gif.h, pr/image_ico.h, pr/image_tiff.h, pr/image_emjc.h). A program puts the codecs that it wants in a set,
// and a library that decodes or encodes images takes the set from its caller.
//
// A codec and a set may be used from any number of threads at once. A decoder is used by one thread at a time.

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

// A source of image readers and encodings, for the formats that its backend reads.
typedef struct PrImageCodec PrImageCodec;

// Codecs in priority order (prImageCodecSetCreate).
typedef struct PrImageCodecSet PrImageCodecSet;

// The state of a backend for one image that its codec opened. Only a decoder reads it.
typedef struct PrImageReader PrImageReader;

// Opens images with a codec, one at a time, and reads the open image. The decoder holds the reader that the codec
// created for the open image, and checks what the reader reports against the limits.
typedef struct PrImageDecoder PrImageDecoder;

// How a channel is stored, in the machine's byte order. Unsigned integers map to [0, 1]. Floating point keeps its
// range.
enum {
    PrImageSample_U8,
    PrImageSample_U16,
    PrImageSample_I16,   // -32767 through 32767 map to [-1, 1], and -32768 to -1
    PrImageSample_F16,
    PrImageSample_F32,
};
typedef uint8_t PrImageSample;

// How alpha relates to the color channels.
enum {
    PrImageAlpha_None,            // no alpha channel
    PrImageAlpha_Straight,        // opacity, not applied to the color channels
    PrImageAlpha_Premultiplied,   // opacity, already multiplied into the color channels
    PrImageAlpha_Independent,     // separate data, neither color nor opacity
};
typedef uint8_t PrImageAlpha;

// The source of the meaning of the color channels.
enum {
    PrImageColorEncoding_Unknown,        // none: the color is uncharacterized
    PrImageColorEncoding_Named,          // a PrColorSpace
    PrImageColorEncoding_Icc,            // an ICC profile
    PrImageColorEncoding_Chromaticities, // linear light in the RGB space of a PrColorChromaticities
};
typedef uint8_t PrImageColorEncoding;

// What the color channels mean. `encoding` decides which other field applies.
typedef struct PrImageColor {
    PrImageColorEncoding encoding;
    PrColorSpace space;                     // Named
    PrByteView profile;                     // Icc: the bytes of an ICC profile
    PrColorChromaticities chromaticities;   // Chromaticities: the primaries and white
} PrImageColor;

// Interleaved channels: the channels of `model` in order, then alpha if there is one. The space of a Named color has
// `model` as its model, except that an RGB space also accepts Gray. A color of chromaticities is RGB or Gray, whose
// gray is the luminance of the space.
typedef struct PrImageFormat {
    PrColorModel model;
    PrImageSample sample;
    PrImageAlpha alpha;
    PrImageColor color;
} PrImageFormat;

// How the stored pixels map to the displayed ones, as the EXIF orientation tag gives it.
enum {
    PrImageOrientation_Identity,
    PrImageOrientation_FlipHorizontal,
    PrImageOrientation_Rotate180,
    PrImageOrientation_FlipVertical,
    PrImageOrientation_Transpose,
    PrImageOrientation_Rotate90,
    PrImageOrientation_Transverse,
    PrImageOrientation_Rotate270,
};
typedef uint8_t PrImageOrientation;

// How the mip levels of an image relate to its extent. A mip chain's levels each halve the one before, rounded down or
// up as the image says. A rip map has a level of every combination of a reduction across and a reduction down: its
// levels are in rows of `levelColumns`, one row for each height and one column for each width, from the full extent.
enum {
    PrImageLevels_Mip,
    PrImageLevels_Rip,
};
typedef uint8_t PrImageLevels;

// The frame index that reads the still image that an animation may carry apart from its frames.
enum { kPrImageFrameCover = 0xffffffffu };

// An image as a reader reports it, in its stored orientation. Each frame has `levelCount` mip levels, the first of the
// image's extent, laid out as `levels` says, and each of the extent that prImageDecoderLevelExtent gives. A mip chain's
// levels are each half the one before, rounded down, unless the reader reports their extents itself.
typedef struct PrImageInfo {
    PrUVec2 extent;
    PrImageFormat format;
    PrImageOrientation orientation;
    PrVec2 pixelsPerMeter;  // the physical resolution, or zero if the image has none
    uint32_t frameCount;
    uint32_t levelCount;    // the mip levels of each frame, at least one
    uint32_t playCount;     // the number of times the frames play, or zero for forever
    uint64_t workingBytes;  // the most bytes that a reader allocates, including the input that it keeps
    bool hasCover;          // true if kPrImageFrameCover reads a still image apart from the frames
    PrImageLevels levels;
    uint32_t levelColumns;  // a rip map's levels across, and one for a mip chain
} PrImageInfo;

// One frame of an image.
typedef struct PrImageFrameInfo {
    uint32_t durationMs;    // how long the frame shows, as encoded
} PrImageFrameInfo;

// Called to read `size` bytes at `offset` of a stream into `out`. Returns Ok only if it filled all of them.
typedef PrStatus (*PrImageReadAtFn)(void* PR_NULLABLE context, uint64_t offset, void* out, size_t size);

// The bytes of an image, which do not change. They are in `memory` if `readAt` is null. Otherwise `readAt` reads them
// from `stream`, which holds `size` bytes.
typedef struct PrImageSource {
    PrImageReadAtFn PR_NULLABLE readAt;
    union {
        PrByteView memory;
        struct {
            void* PR_NULLABLE context;
            uint64_t size;
        } stream;
    };
} PrImageSource;

// The largest amounts of resources that an image may use. Each is a hard limit, which the functions here enforce for
// every codec.
typedef struct PrImageLimits {
    uint64_t inputBytes;
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a reader allocates
    uint64_t outputBytes;
    uint64_t work;          // the bounded steps of decoding, as a codec counts them
    uint32_t frames;
    uint32_t metadataBytes;
    uint32_t headerBytes;
} PrImageLimits;

// The assumptions a conversion may make when color is uncharacterized.
enum {
    PrImageConversionFlag_AssumeSrgb = 1u << 0,        // RGB or Gray taken as sRGB
    PrImageConversionFlag_ApproximateCmyk = 1u << 1,   // CMYK converted by multiplying out the ink
};
typedef uint8_t PrImageConversionFlags;

// Frees a codec that no set owns. Its readers must be closed first.
void prImageCodecDestroy(PrImageCodec* PR_NULLABLE codec);

// Creates a set of `count` codecs, in priority order, and takes ownership of them. The set does not change once it is
// created, so any thread may use it. An image is opened by the first codec that recognizes its bytes, and encoded by
// the first codec that encodes its type. A container opens the images that it embeds through the set too, so an icon's
// PNG entries need a PNG codec. Only the codecs that a program creates are linked into it.
PrImageCodecSet* prImageCodecSetCreate(PrImageCodec* PR_NONNULL const* PR_NULLABLE codecs, uint32_t count);

// Frees the set and its codecs. Its decoders must be destroyed first.
void prImageCodecSetDestroy(PrImageCodecSet* PR_NULLABLE set);

// Returns true if a codec of the set decodes the MIME type `type`. `type` is the essence, in lowercase and without
// parameters, such as image/png.
bool prImageCodecSetSupportsType(const PrImageCodecSet* set, PrStringView type);

// Encodes `pixels` as an image of the MIME type `type`, in the form that prImageCodecSetSupportsType takes, with the
// first codec of the set that encodes it. `pixels` are straight-alpha sRGB Rgba8 of nonzero size. Writes up to
// `capacity` bytes to `out`, and sets `*size` to the size of the whole encoding. `out` may be null if `capacity` is
// zero. Returns NotSupported if no codec of the set encodes `type`.
PrStatus prImageCodecSetEncode(const PrImageCodecSet* set, PrStringView type, PrPixelView pixels,
    uint8_t* PR_NULLABLE out, size_t capacity, size_t* size);

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrImageLimits prImageLimitsDefault(void);

// Returns the size of `source` in bytes.
uint64_t prImageSourceSize(const PrImageSource* source);

// Reads exactly `size` bytes at `offset` of `source` into `out`. Returns Io for a range past the end, or if `readAt`
// fails.
PrStatus prImageSourceRead(const PrImageSource* source, uint64_t offset, void* PR_NULLABLE out, size_t size);

// Creates a decoder that opens images with the codecs of `set`, which must outlive it. The decoder keeps its storage
// from one image to the next.
PrImageDecoder* prImageDecoderCreate(const PrImageCodecSet* set);

// Closes the open image, if any, and frees the decoder.
void prImageDecoderDestroy(PrImageDecoder* PR_NULLABLE decoder);

// Closes the open image, if any, and opens the image in `source`. Null `limits` means the defaults. The storage of
// `source` and its context must stay valid until the image is closed.
//
// Returns NotSupported if no codec of the set recognizes the image, Limit if the image exceeds `limits`, and Truncated
// if the image opens from incomplete input. Any failure other than Truncated leaves no image open.
PrStatus prImageDecoderOpen(PrImageDecoder* decoder, PrImageSource source, const PrImageLimits* PR_NULLABLE limits);

// Closes the open image, if any.
void prImageDecoderClose(PrImageDecoder* decoder);

// The functions below read the open image.

// Returns information about the open image. The view of its ICC profile stays valid until the image is closed.
const PrImageInfo* prImageDecoderInfo(const PrImageDecoder* decoder);

// Writes information about `frame` to `*out`. Returns OutOfRange for a frame the image does not have.
PrStatus prImageDecoderFrameInfo(PrImageDecoder* decoder, uint32_t frame, PrImageFrameInfo* out);

// Decodes all of `frame`, in stored orientation, into `output`, which must have the format and size of the image.
// Returns OutOfRange for a frame that the image does not have. After a failure, the pixels are unspecified.
PrStatus prImageDecoderRead(PrImageDecoder* decoder, uint32_t frame, PrPixels output);

// Returns the extent of mip level `level` of an image of `extent` whose levels each halve the one before, rounded down.
PrUVec2 prImageLevelExtent(PrUVec2 extent, uint32_t level);

// Returns the extent of mip level `level` of the open image, which it must have.
PrUVec2 prImageDecoderLevelExtent(const PrImageDecoder* decoder, uint32_t level);

// Decodes mip level `level` of `frame`, as prImageDecoderRead decodes the first, into `output`, which must have the
// format of the image and the extent of the level. Returns OutOfRange for a frame or a level that the image does not
// have.
PrStatus prImageDecoderReadLevel(PrImageDecoder* decoder, uint32_t frame, uint32_t level, PrPixels output);

// Decodes `frame` as sRGB Rgba8 with `alpha`, straight or premultiplied, and applies `orientation` to its stored
// pixels. Passing the orientation of the image gives the displayed image, and Identity gives the stored one. The width
// and height of `output` are those of the frame, swapped if the orientation transposes. Uncharacterized color is taken
// as sRGB, and CMYK is approximated. Returns NotSupported for color or alpha that it cannot convert.
PrStatus prImageDecoderReadRgba8(PrImageDecoder* decoder, uint32_t frame, PrImageAlpha alpha,
    PrImageOrientation orientation, PrPixels output);

// Returns the number of bytes that one pixel of `format` occupies. Returns zero for a format whose fields are not valid
// together, such as one whose color names a space of another model.
uint32_t prImageFormatSize(const PrImageFormat* format);

// Converts `source` in `sourceFormat` into `destination` in `destinationFormat`. The two are the same size and do not
// overlap. Independent alpha is unchanged. `flags` allow assumptions for converting between spaces when color is
// uncharacterized. Returns NotSupported for formats it cannot convert between.
PrStatus prImageConvert(PrPixelView source, const PrImageFormat* sourceFormat, PrPixels destination,
    const PrImageFormat* destinationFormat, PrImageConversionFlags flags);

// Returns true if `orientation` exchanges the width and height of an image, as a transpose or a quarter turn does.
bool prImageOrientationSwapsAxes(PrImageOrientation orientation);

// Returns the orientation that undoes `orientation`: it maps the displayed pixels back to the stored ones.
PrImageOrientation prImageOrientationInverse(PrImageOrientation orientation);

// Copies packed pixels of `pixelSize` bytes each from `source`, in stored orientation, into `output`, arranged as
// `orientation` maps stored pixels to displayed ones. The width and height of `output` are those of `source`, swapped
// if the orientation does. The two must not overlap.
void prImageOrient(PrPixelView source, uint32_t pixelSize, PrImageOrientation orientation, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
