// The bridging header of the Swift host's SwiftUI application: what it calls of swift_host.mm, which declares it with C
// linkage too.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

// Runs the immersive space's frames on the calling thread, prints the test's outcome and returns whether it passed.
// `renderer` is the cp_layer_renderer_t. Swift reads the declaration as C.
#if defined(__cplusplus)
extern "C" bool prSwiftHostRun(void* renderer);
#else
bool prSwiftHostRun(void* renderer);
#endif

PR_ASSUME_NONNULL_END
