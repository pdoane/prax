// The AppKit application backend: macOS windows and the AppKit event loop.
//
// A window's surface is a PrAppSurfaceKind_NsView. Each new window cascades from the last. The application stays open
// while it has no windows, until the program stops it, and Quit in its menu asks the program to stop.
//
// Notifications use the system's UserNotifications framework. Only a process that runs from an application bundle with
// a bundle identifier can post them. In any other process, and for a headless application, prAppNotificationPost
// returns NotSupported. The first post asks for the user's permission. Notifications posted before the user answers
// wait for the answer. A notification also shows while the application is active.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application whose windows are macOS windows on the AppKit event loop.
PrApp* prAppAppKitCreate(const PrAppOptions* options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
