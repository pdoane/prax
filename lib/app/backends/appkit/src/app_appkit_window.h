// The AppKit backend's windows: their windows, views and panels, their frames, and their placement.

#pragma once

#include "app/backends/appkit/src/app_appkit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the state of the window's frame now, for an event that the window delivers.
PrAppFrame appKitFrameState(PrAppWindow* window);

// Implement PrAppWindow::destroyFn and PrAppWindow::requestFrameFn.
void appKitWindowDestroy(PrAppWindow* window);
void appKitRequestFrame(PrAppWindow* window);

// Requests a frame at `time`, in the seconds of appKitUptime, or none if `time` is zero, as a window's output asks.
void appKitRequestFrameAt(PrAppWindow* window, double time);

// Shows `cursor` over the window's content, as a window's output asks for.
void appKitSetCursor(PrAppWindow* window, PrAppCursor cursor);

// Applies the text being edited in the window: where the input method opens its windows, and whether the window holds
// secure input for a password.
void appKitApplyTextInput(PrAppWindow* window);

// Implements PrApp::windowCreateFn.
PrStatus appKitWindowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
