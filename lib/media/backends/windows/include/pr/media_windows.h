// The Media Foundation media codec.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that reads the resources that Media Foundation plays: MPEG-4 and QuickTime movies, MP3 and WAVE, and
// WebM where the system reads it. A decoder decodes the frames of the first video track on a thread of its own, a few
// frames ahead of the time that was last asked for, and decodes no audio. Returns null if the system's Media Foundation
// does not start. The codec keeps the process's COM multithreaded apartment alive until it is destroyed.
PrMediaCodec* PR_NULLABLE prMediaWindowsCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
