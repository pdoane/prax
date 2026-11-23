// The DDS image codec. It makes the textures of lib/dds (pr/dds.h) a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes DDS textures (.dds) of the type image/vnd.ms-dds, and encodes nothing. A texture has mip
// levels, and its array elements and faces are its frames. It decodes to the pixels that lib/dds produces: one channel
// is gray, two are gray and alpha. A DXGI format of the sRGB transfer function is sRGB, and any other color is
// uncharacterized. The codec returns NotSupported for a texture whose texels do not decode to pixels and for a volume.
// The codec does not return a texture's blocks. Use lib/dds for those. The codec has no state of its own, so one codec
// serves every thread.
PrImageCodec* prImageDdsCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
