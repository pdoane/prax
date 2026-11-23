// Images as texture sources (pr/texture.h). A source decodes any image that an image codec reads: its mip levels, a
// cube's faces and layers, and its previews.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/image.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How an image's source treats the image's color.
enum {
    // The values are kept as the image stores them, in a plain format. An image does not say whether its values are
    // color. A library that decides by its own rules, as a material does, uses this.
    PrImageTextureColor_Stored,

    // The image is converted to sRGB for display.
    //
    // - 8-bit values in sRGB use a format's Srgb form.
    // - An image in another named RGB space is read as linear extended sRGB in Rgba16F.
    // - An image in any other color, including one with an ICC profile, is read as Rgba8Srgb. The codec converts it.
    // - An image of floats keeps its linear values.
    PrImageTextureColor_Srgb,
};
typedef uint8_t PrImageTextureColor;

// Creates a source that decodes the image in `bytes` with `codec`. `codec` must outlive the source. The source keeps a
// copy of `bytes`, and each read decodes it with a decoder of its own. A format set opens an image of a blob source the
// same way, with the format of prImageTextureFormatCreate (prTextureFormatSetOpen): that source retains the blob source
// and reads its bytes again for each read.
//
// The texture is the image's first frame. For a cube, it is the cube's faces and layers. The description states the
// image's alpha and its stored orientation. The texture has the image's mip levels if each is half the size of the one
// before, as a texture's levels are. Otherwise it has the first level only, as for a rip map.
//
// With Stored color, the format is Rgba8 for 8-bit and 16-bit samples, and Rgba32F for floats and 32-bit integers. The
// source can also decode to the others of Rgba8, Rgba16F and Rgba32F, and to R8 with the gray or red channel
// (prTextureSourceReformat). The values are not converted. An image whose channels are not gray or RGB, such as CMYK,
// decodes only to Rgba8Srgb, which the codec converts to. The source answers a need (prTextureSourceAnswer) with its
// own pixels, or with R8 for a need of the red channel of data alone.
//
// A 2D image with one level has the previews that its codec reads (prTextureSourcePreview, prImageDecoderReadPreview).
// For a JPEG, these are the thumbnail that the file embeds and the decodes at a quarter and an eighth of its extent. A
// consumer that shows a thumbnail of such an image reads a preview and never decodes the image. An image without
// previews is read whole.
//
// Returns the failure of reading the bytes, NotSupported if the codec does not recognize them, Limit if a side of the
// image or its layers are past `limits`, or InvalidData if the image does not open. An image within `limits` opens
// whatever its size in bytes: the limits that the codec checks follow from the extent. A null `limits` means the
// defaults. On failure, `*out` is null.
PrStatus prImageTextureSourceOpenBytes(PrByteView bytes, const PrImageCodecSet* codec,
    const PrTextureLimits* PR_NULLABLE limits, PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Creates a format for a texture format set (prTextureFormatSetCreate). It reads the media types that `codec` decodes,
// and its sources treat color as `color` says. A program that shows images passes Srgb. `codec` must outlive the
// format's sources.
PrTextureFormat* prImageTextureFormatCreate(const PrImageCodecSet* codec, PrImageTextureColor color);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
