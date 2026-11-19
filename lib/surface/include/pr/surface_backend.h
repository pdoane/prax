// The interface that session backends implement. Callers use the functions of pr/surface.h, which check their arguments
// and call these. Only backends include this header.

#pragma once

#include "pr/surface.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a session's backend, at the start of its state. Each implements the function of pr/surface.h that it
// is named for. `frameBeginFn` is called only while the session has no frame begun, and `frameEndFn` only while it has
// one.
struct PrSurfaceSession {
    void (*destroyFn)(PrSurfaceSession* session);
    PrSurface (*surfaceFn)(const PrSurfaceSession* session);
    PrSurfaceSessionState (*stateFn)(const PrSurfaceSession* session);
    PrStatus (*frameBeginFn)(PrSurfaceSession* session, PrSurfaceFrame* out);
    void (*frameEndFn)(PrSurfaceSession* session);
    bool inFrame;   // kept by pr/surface.h: a frame is begun
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
