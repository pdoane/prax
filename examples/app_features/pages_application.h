// The application's pages: its lifecycle, presence, hiding and quitting, its timers, and the requests to open files and
// URLs.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Hides the application or the control window, and sets the timer that shows it again (kTimerUnhide). Shows again what
// hideFor hid, as its timer fires.
void hideFor(Features& f, Hiding hiding);
void unhide(Features& f);

// Shows the Lifecycle page's question about a request to quit, in front.
void askToQuit(Features& f, bool sessionEnding);

// Build the pages under `parent`, a column of the control window.
void pageLifecycle(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageTimers(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageOpen(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Takes the application's wakes, timers, quit requests and open requests. Returns true for an event that it took.
bool applicationEvent(Features& f, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
