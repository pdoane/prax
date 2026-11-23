// The selector for lib/image's codecs: the choice of the backend of each format, and a set of every format for a
// program that wants them all. Programs and composition libraries link it. A program that wants only some formats
// builds its set from their codecs' own constructors instead, and carries only those.

#pragma once

#include "pr/backend.h"
#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The backends of a format.
enum {
    PrImageCodecBackend_Default,
    PrImageCodecBackend_Builtin,    // Prax's own, such as pr/image_png.h's
    PrImageCodecBackend_ImageIO,    // Apple's ImageIO (pr/image_imageio.h)
    PrImageCodecBackend_Count,
};
typedef uint8_t PrImageCodecBackend;

// Returns the descriptor of the codecs of the format of the media type `type`, or null for a format that Prax does not
// have: "image/png", "image/jpeg", "image/gif", "image/webp", "image/x-icon", "image/tiff", "image/vnd.radiance",
// "image/x-exr", "image/ktx2", "image/x-emjc", "image/qoi", "image/bmp", "image/vnd.ms-dds",
// "image/vnd.adobe.photoshop" or "image/x-tga". The environment chooses one with PR_BACKEND_IMAGE_<FORMAT>, such as
// PR_BACKEND_IMAGE_PNG.
const PrBackendInterface* PR_NULLABLE prImageCodecSelectInterface(PrStringView type);

// Creates a codec of the format of `type` with `backend`. Default is the backend that the format's variable chooses, or
// else Prax's own. The codec decompresses through `compress`, which must outlive it. Returns null for a format that
// Prax does not have, or if the build does not include the backend or it does not decode the format. For Default, it
// also returns null if the variable is not valid (prBackendFromEnvironment).
PrImageCodec* PR_NULLABLE prImageCodecCreate(PrImageCodecBackend backend, PrStringView type, const PrCompressCodecSet* compress);

// Creates a set of a codec of each format that Prax has, with its Default backend, leaving out a format that no backend
// of the build decodes. The codecs decompress through `compress`, which must outlive the set. Returns null if a
// variable is not valid.
PrImageCodecSet* PR_NULLABLE prImageCodecSetCreateDefault(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
