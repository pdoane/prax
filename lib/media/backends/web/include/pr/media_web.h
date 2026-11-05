// The web media codec: a resource played in a web page's video element, whose frames are the page's images.
//
// A decoder plays its resource muted and looping, and follows the times that it is asked for. It seeks if the time
// asked for is far from where the video plays. A frame is an image of the page (PrGpuImportKind_WebImage), which a
// Canvas 2D device imports, and has no pixels. The resource's metadata comes from the built-in codec, because the page
// gives the video's metadata only once the video has loaded.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that reads what the page's video element plays. Returns null if the program runs without a page.
PrMediaCodec* PR_NULLABLE prMediaWebCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
