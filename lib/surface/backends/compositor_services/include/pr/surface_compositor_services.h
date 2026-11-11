// The CompositorServices backend of immersive sessions, on visionOS: a session over the layer renderer of a SwiftUI
// CompositorLayer, which the host's immersive space makes, and which the host gives as the session's `native`. The host
// is lib/app (prAppImmersiveOpen), or an application of SwiftUI's own. The session runs its frames on any one thread,
// and finds the device's pose with ARKit's world tracking.
//
// The host configures the layer as a device draws the session's frames: without foveation, since the frames are drawn
// without rasterization rate maps, and in a layered layout where the capabilities have one, so that a pass of several
// views draws every view, or else a shared one. A dedicated layout's texture for each view is not a frame that a device
// draws. The color and depth formats are the host's to choose.
//
// The session's surface is of kind PrSurfaceKind_CompositorServices. A device presents the begun frame's drawable by
// encoding its present in the command buffer that draws it.

#pragma once

#include "pr/surface.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a session over the cp_layer_renderer_t that `desc->native` is.
PrStatus prSurfaceCompositorServicesCreate(const PrSurfaceSessionDesc* desc, PrSurfaceSession* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
