// KTX 2.0 files as texture sources (pr/texture.h). A source produces the file's blocks as stored, the blocks that Basis
// Universal's formats transcode to, and the pixels that its levels decode to.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/compress.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A format set opens a KTX 2.0 texture with the format of prKtxTextureFormatCreate (prTextureFormatSetOpen). Its source
// retains the bytes and reads them again for each read. Supercompressed levels are inflated with the format's codec.
// Without a codec, such a file is NotSupported.
//
// A file of blocks gives a source of those blocks. A file of pixels, or of one of Basis Universal's formats, gives a
// source of pixels: Rgba8 for 8-bit and 16-bit samples, and Rgba32F for floats. The source can also produce other
// formats (prTextureSourceReformat):
//
// - the blocks that Basis Universal's formats transcode to (PrKtxInfo::transcodes);
// - Rgba8, Rgba16F and Rgba32F, if the levels decode to pixels;
// - R8, with the gray or red channel.
//
// The source answers a need (prTextureSourceAnswer) with the file's blocks if the consumer takes them, or else with the
// first transcode target that it takes, in the order ASTC 4x4, BC7, EAC R11, BC4, EAC RG11, BC5, ETC2 RGBA8, BC3, ETC2
// RGB8 and BC1, with the targets of one channel first for a need of one channel of data. It answers with pixels if no
// blocks serve, and for a need of pixels if the levels decode.
//
// A file that ends before its level data still opens, because the bytes before the levels describe the whole texture
// (prKtxInfoBytes). Reads of the missing levels fail.
//
// The open fails with Limit if the texture is past the open's limits, or with a failure of prKtxTextureCreate other
// than Truncated.
//
// Creates a format for a texture format set (prTextureFormatSetCreate). It reads the media type image/ktx2. Its sources
// inflate supercompressed levels with `codec`, which must outlive them.
PrTextureFormat* prKtxTextureFormatCreate(const PrCompressCodecSet* PR_NULLABLE codec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
