// The UIKit backend's device: power, keeping awake, haptics, the motion sensors, notifications and the badge.

#pragma once

#include "app/backends/uikit/src/app_uikit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The application's functions of the device: its power, keeping awake, haptics, sensors, notifications and badge.
PrAppPower uiKitPower(const PrApp* app);
void uiKitSetAwake(PrApp* app, PrAppAwakeFlags flags);
void uiKitHapticPlay(PrApp* app, PrAppHaptic haptic);
PrStatus uiKitSensorEnable(PrApp* app, PrAppSensor sensor, bool enable, double interval);
PrStatus uiKitNotificationPost(PrApp* app, const PrAppNotificationDesc* desc);
void uiKitSetBadge(PrApp* app, PrStringView badge);

// Starts the device's services with the run: the delegate of the notification center, which a notification that
// launched the application reaches. Stops them with the run: the sensors. Frees them with the application.
void uiKitDeviceStart(PrApp* app);
void uiKitDeviceStop(PrApp* app);
void uiKitDeviceDestroy(PrApp* app);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
