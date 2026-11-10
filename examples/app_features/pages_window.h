// The windows' pages: a window's state, place and look, the Dock or the taskbar, popups and dialogs, backdrops and the
// overlay, and tabs that move between windows.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Opens the second window, under the pointer at `at` if it is set, which tabs move to.
void openSecond(Features& f, const PrAppDisplayPosition* PR_NULLABLE at);

// Shows the overlay and gives it the keyboard, or hides it.
void toggleOverlay(Features& f);

// Build the pages under `parent`, a column of the control window.
void pageWindows(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageTaskbar(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pagePopups(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageBackdrops(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageTabs(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Takes the timer of an attention request and the overlay's hotkey. Returns true for an event that it took.
bool windowsAppEvent(Features& f, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
