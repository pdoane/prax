// The immersive space of the UIKit backend, on visionOS: a scene that SwiftUI hosts (uikit_immersive.swift), whose
// layer renderer the application receives with ImmersiveOpened.

#pragma once

#include "app/backends/uikit/src/app_uikit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Implement prAppImmersiveOpen and prAppImmersiveClose.
PrStatus uiKitImmersiveOpen(PrApp* app);
void uiKitImmersiveClose(PrApp* app);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
