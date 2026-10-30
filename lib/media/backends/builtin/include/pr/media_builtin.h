// The built-in media codec: the metadata of a resource in the ISO base media file format (ISO/IEC 14496-12), as MP4
// stores it.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that reads the duration of an MP4 movie and the extent of its first video track, and decodes no
// frames. It reads video/mp4 and audio/mp4, whatever their codecs parameter names.
PrMediaCodec* prMediaBuiltinCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
