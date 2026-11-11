// Immersive sessions: the checks of pr/surface.h, and the backends that the build has.

#include "pr/surface.h"

#include "pr/assert.h"
#include "pr/surface_backend.h"

#if defined(PR_SURFACE_HAS_COMPOSITOR_SERVICES)
#include "pr/surface_compositor_services.h"
#endif

bool prSurfaceSessionBackendIsBuilt(PrSurfaceSessionBackend backend) {
    switch (backend) {
        case PrSurfaceSessionBackend_Default:
        case PrSurfaceSessionBackend_CompositorServices:
#if defined(PR_SURFACE_HAS_COMPOSITOR_SERVICES)
            return true;
#else
            return false;
#endif
        default:
            return false;
    }
}

PrStatus prSurfaceSessionCreate(PrSurfaceSessionBackend backend, const PrSurfaceSessionDesc* desc,
        PrSurfaceSession* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(desc->near > 0 && desc->far > desc->near);
    switch (backend) {
        case PrSurfaceSessionBackend_Default:
        case PrSurfaceSessionBackend_CompositorServices:
#if defined(PR_SURFACE_HAS_COMPOSITOR_SERVICES)
            return prSurfaceCompositorServicesCreate(desc, out);
#else
            return PrStatus_NotSupported;
#endif
        default:
            return PrStatus_NotSupported;
    }
}

void prSurfaceSessionDestroy(PrSurfaceSession* PR_NULLABLE session) {
    if (session != nullptr)
        session->destroyFn(session);
}

PrSurface prSurfaceSessionSurface(const PrSurfaceSession* session) {
    return session->surfaceFn(session);
}

PrSurfaceSessionState prSurfaceSessionState(const PrSurfaceSession* session) {
    return session->stateFn(session);
}

PrStatus prSurfaceSessionFrameBegin(PrSurfaceSession* session, PrSurfaceFrame* out) {
    PR_ASSERT(!session->inFrame);
    *out = PrSurfaceFrame{};
    PrStatus status = session->frameBeginFn(session, out);
    session->inFrame = status == PrStatus_Ok;
    if (status != PrStatus_Ok)
        *out = PrSurfaceFrame{};
    return status;
}

void prSurfaceSessionFrameEnd(PrSurfaceSession* session) {
    PR_ASSERT(session->inFrame);
    session->inFrame = false;
    session->frameEndFn(session);
}
