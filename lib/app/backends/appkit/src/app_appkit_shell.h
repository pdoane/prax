// The AppKit backend's shell and device: notifications, URLs, the trash, recent documents, the Dock, sharing, power,
// keeping awake and haptics.

#pragma once

#include "app/backends/appkit/src/app_appkit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Implement the application's functions of their names (PrApp::notificationPostFn and the others).
PrStatus appKitNotificationPost(PrApp* app, const PrAppNotificationDesc* desc);
PrStatus appKitOpenUrl(PrApp*, PrStringView url);
PrStatus appKitRevealPath(PrApp* app, PrStringView path);
PrStatus appKitTrashPath(PrApp*, PrStringView path);
void appKitRecentDocumentAdd(PrApp*, PrStringView path);
void appKitRecentDocumentsClear(PrApp*);
void appKitSetBadge(PrApp* app, PrStringView badge);
PrAppPower appKitPower(const PrApp*);
void appKitSetAwake(PrApp* app, PrAppAwakeFlags flags);

// Implements PrApp::hapticPlayFn. A Mac plays haptics on its Force Touch trackpad, which has three patterns. The
// impacts map to them by strength, and the outcomes to the generic pattern.
void appKitHapticPlay(PrApp*, PrAppHaptic haptic);

// Implements PrAppWindow::shareFn.
void appKitShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
    void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
