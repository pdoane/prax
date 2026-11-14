// The shell's and the device's pages: notifications, URLs and sharing, power and staying awake, sensors, haptics and
// orientations, and the displays and the user's preferences.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Build the pages under `parent`, a column of the control window.
void pageShell(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pagePower(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageSensors(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageDisplays(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Takes the sensors' readings, the device's notices, and the clicks on notifications. Returns true for an event that it
// took.
bool deviceAppEvent(Features& f, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
