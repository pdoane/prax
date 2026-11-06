// The Cocoa application backend: a macOS window and the AppKit event loop.
//
// Notifications use the system's UserNotifications framework. Only a process that runs from an application bundle with
// a bundle identifier can post them. In any other process, and for a headless window, prAppNotificationPost returns
// NotSupported. The first post asks for the user's permission. Notifications posted before the user answers wait for
// the answer. A notification also shows while the application is active.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an application that is a macOS window on the AppKit event loop, as `desc` describes.
PrApp* prAppCocoaCreate(const PrAppDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
