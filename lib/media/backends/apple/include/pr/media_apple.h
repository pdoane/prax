// The AVFoundation media codec.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that reads the resources that AVFoundation plays: MPEG-4 and QuickTime movies, and MP3, WAVE and WebM
// where the system reads them. A decoder decodes the frames of the first video track on a thread of its own, a few
// frames ahead of the time that was last asked for, and the first audio track on another, once its audio starts.
PrMediaCodec* prMediaAppleCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
