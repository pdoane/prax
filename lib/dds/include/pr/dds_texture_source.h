// DDS files as texture sources (pr/texture.h). A source produces the file's texels as stored, and the pixels that its
// levels decode to.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A format set opens a DDS texture with the format of prDdsTextureFormatCreate (prTextureFormatSetOpen). Its source
// retains the bytes and reads them again for each read.
//
// A file of blocks gives a source of those blocks, and a file of pixels in a format that lib/gpu has, swizzled or not,
// gives a source of those pixels as stored. Any other file gives a source of the pixels that it decodes to: Rgba8 for
// 8-bit samples, and Rgba32F for wider ones. An sRGB format of DXGI gives the Srgb form. The source can also produce
// other formats (prTextureSourceReformat) if its texels decode: Rgba8, Rgba16F and Rgba32F, and R8 with the gray or red
// channel.
//
// The source answers a need (prTextureSourceAnswer) with its blocks if the consumer takes them, and otherwise, or for a
// need of pixels if they decode, with pixels: its stored pixels if lib/texture converts their format, R8 for a need of
// the red channel of data alone, or the pixels that it decodes to.
//
// A file that ends before its texels still opens, because its headers describe the whole texture. Reads of the missing
// levels fail.
//
// The open fails with Limit if a side, the depth or the layers of the texture are past the open's limits, or with a
// failure of prDdsTextureCreate other than Truncated.
//
// Creates a format for a texture format set (prTextureFormatSetCreate). It reads the media type image/vnd.ms-dds.
PrTextureFormat* prDdsTextureFormatCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
