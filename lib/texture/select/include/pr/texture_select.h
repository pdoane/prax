// The selector for lib/texture's formats. It creates a set of every texture format that Prax has. Programs and
// composition libraries link it. A program that wants only some formats creates them itself (prKtxTextureFormatCreate,
// prImageTextureFormatCreate), builds its own set, and links only those formats.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"
#include "pr/image_texture_source.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a set of two formats: KTX 2.0, and then every image that `images` decodes. KTX 2.0 comes first because an
// image codec set may also decode a KTX 2.0 file's pixels, which loses its blocks. KTX 2.0 sources inflate with
// `compress`. Image sources treat color as `color` says (prImageTextureFormatCreate). A null `images` leaves the image
// format out. Both codec sets must outlive the set's sources.
PrTextureFormatSet* prTextureFormatSetCreateDefault(const PrCompressCodecSet* PR_NULLABLE compress,
    const PrImageCodecSet* PR_NULLABLE images, PrImageTextureColor color);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
