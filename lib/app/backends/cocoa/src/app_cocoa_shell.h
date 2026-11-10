// The Cocoa backend's shell and device: notifications, URLs, the trash, recent documents, the Dock, sharing, power,
// keeping awake and haptics.

#pragma once

#include "app/backends/cocoa/src/app_cocoa.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Implement the application's functions of their names (PrApp::notificationPostFn and the others).
PrStatus cocoaNotificationPost(PrApp* app, const PrAppNotificationDesc* desc);
PrStatus cocoaOpenUrl(PrApp*, PrStringView url);
PrStatus cocoaRevealPath(PrApp* app, PrStringView path);
PrStatus cocoaTrashPath(PrApp*, PrStringView path);
void cocoaRecentDocumentAdd(PrApp*, PrStringView path);
void cocoaRecentDocumentsClear(PrApp*);
void cocoaSetBadge(PrApp* app, PrStringView badge);
PrAppPower cocoaPower(const PrApp*);
void cocoaSetAwake(PrApp* app, PrAppAwakeFlags flags);

// Implements PrApp::hapticPlayFn. A Mac plays haptics on its Force Touch trackpad, which has three patterns. The
// impacts map to them by strength, and the outcomes to the generic pattern.
void cocoaHapticPlay(PrApp*, PrAppHaptic haptic);

// Implements PrAppWindow::shareFn.
void cocoaShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
    void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
