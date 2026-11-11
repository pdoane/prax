// The UIKit application backend: UIKit's scenes and windows, and the main run loop.
//
// A window's surface is a PrAppSurfaceKind_UiView, whose layer is a CAMetalLayer. Each window is the root of a scene of
// its own, which the system sizes and places. A window shows once its scene connects: the first window takes the scene
// of the launch, and each further window asks the system for a new scene. The window's frames report the scene's safe
// area and the part that the on-screen keyboard covers.
//
// An application that asks for Multiple or Desktop windows has Multiple ones where the device shows several scenes, as
// an iPad does, and where its Info.plist allows them (UIApplicationSupportsMultipleScenes). Elsewhere, as on an iPhone,
// it has a Single window. A scene that the system restores or that the user opens, which no window asked for, is
// closed.
//
// prAppRun returns only for a headless application, whose windows have no scene and whose run is the main run loop.
// Otherwise UIKit owns the process: the run never returns, and once the program stops it and its shutdown returns, the
// process exits.
//
// A scene that disconnects sends its window CloseRequested. Destroying a window of a Multiple application closes its
// scene.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application whose window is a UIKit window of the application's scene.
PrApp* prAppUiKitCreate(const PrAppOptions* options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
