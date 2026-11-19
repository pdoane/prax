// Surfaces: the native objects that a device presents to, such as a window's view, and the immersive sessions that make
// the surfaces of a headset's displays. A surface names its object by kind, so that a library that makes surfaces, such
// as lib/app or a session, and a library that presents to them, such as lib/gpu, need not know each other.
//
// An immersive session runs frames in the order of OpenXR's: the session waits for the compositor to want a frame,
// begins it, and says where the frame's views are at the time that it shows (prSurfaceSessionFrameBegin). The
// application draws the views to the session's surface, through a swap chain of its device, presents, submits its work,
// and ends the frame (prSurfaceSessionFrameEnd).

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of native surface.
enum {
    PrSurfaceKind_NsView,       // `handle` is an NSView
    PrSurfaceKind_UiView,       // `handle` is a UIView whose layer is a CAMetalLayer

    // `handle` points to the cp_drawable_t of the frame that a CompositorServices session has begun, which the session
    // sets for each frame (pr/surface_compositor_services.h).
    PrSurfaceKind_CompositorServices,

    PrSurfaceKind_Hwnd,         // `handle` is a Win32 window's HWND
    PrSurfaceKind_HtmlCanvas,   // `handle` is the CSS selector of a page's canvas, a NUL-terminated string
};
typedef uint8_t PrSurfaceKind;

// A native surface. `kind` says which native object `handle` is.
typedef struct PrSurface {
    PrSurfaceKind kind;
    void* handle;
} PrSurface;

// ---------------------------------------------------------------------------------------------------------------------
// Immersive sessions

// A session of an immersive display: its compositor, the surface that its frames are drawn to, and where the viewer's
// eyes are.
typedef struct PrSurfaceSession PrSurfaceSession;

// The library's session backends, which the build turns on (README.md), and Default, the first backend in the build
// that the system supports.
enum {
    PrSurfaceSessionBackend_Default,
    PrSurfaceSessionBackend_CompositorServices,
    PrSurfaceSessionBackend_Count,
};
typedef uint8_t PrSurfaceSessionBackend;

// What a session runs over, and the depth of its frames.
typedef struct PrSurfaceSessionDesc {
    // The native object that the session runs over, which the host of the immersive space gives, as the backend's
    // header says.
    void* PR_NULLABLE native;

    // The distances in meters from the viewer to the near and the far planes of the views' projections, which the
    // compositor also reprojects the frame's depth with.
    float near;
    float far;
} PrSurfaceSessionDesc;

// Whether a session shows frames.
enum {
    PrSurfaceSessionState_Paused,   // the compositor shows nothing of the session's, and wants no frames until it runs
    PrSurfaceSessionState_Running,
    PrSurfaceSessionState_Ended,    // the session shows nothing again, and the application destroys it
};
typedef uint8_t PrSurfaceSessionState;

// The most views that a frame has.
enum { kPrSurfaceMaxViews = 4 };

// Where one view of a frame looks from. The world's space is the session's: y up, in meters, from an origin that the
// system chose when the session began. A view's space has x right, y up and z toward the viewer.
typedef struct PrSurfaceView {
    PrMatrix4 view;         // from the world's space to the view's
    PrMatrix4 projection;   // from the view's space to clip space, with y up, and depth from 1 at near to 0 at far
} PrSurfaceView;

// A frame that a session began: when the compositor shows it, and its views, as a device's frame of the session's
// surface numbers them.
typedef struct PrSurfaceFrame {
    double time;            // when the frame shows, on lib/core's monotonic clock (prTimeMonotonicMilliseconds)
    uint8_t viewCount;
    PrSurfaceView views[kPrSurfaceMaxViews];
} PrSurfaceFrame;

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prSurfaceSessionBackendIsBuilt(PrSurfaceSessionBackend backend);

// Creates a session by the backend over what `desc` names. Returns NotSupported if the build does not have the backend
// or the system has no immersive display that it drives, and InvalidData if `desc` names no native object that the
// backend runs over.
PrStatus prSurfaceSessionCreate(PrSurfaceSessionBackend backend, const PrSurfaceSessionDesc* desc,
    PrSurfaceSession* PR_NULLABLE* PR_NONNULL out);

// Destroys the session. A device's swap chain over its surface is destroyed first.
void prSurfaceSessionDestroy(PrSurfaceSession* PR_NULLABLE session);

// Returns the surface that the session's frames are drawn to, which lives as long as the session.
PrSurface prSurfaceSessionSurface(const PrSurfaceSession* session);

// Returns whether the session shows frames.
PrSurfaceSessionState prSurfaceSessionState(const PrSurfaceSession* session);

// Waits until the compositor wants the next frame, while the session is paused too, and begins it: the session's
// surface then holds the frame, for a device to acquire. Writes when the frame shows, and where its views are then, as
// they are best predicted when the call returns, so an application reads its input after the call. Returns Lost, and
// begins nothing, once the session has ended.
PrStatus prSurfaceSessionFrameBegin(PrSurfaceSession* session, PrSurfaceFrame* out);

// Ends the frame that the session began, once the work that draws it has been submitted after its present.
void prSurfaceSessionFrameEnd(PrSurfaceSession* session);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
