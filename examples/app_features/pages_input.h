// The input's pages: the pointer and its modes, the cursors, the keys and their labels, the kinds of text input, and
// the gestures and touches of a trackpad.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Build the pages under `parent`, a column of the control window.
void pagePointer(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageKeys(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageGestures(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Records the control window's pointer, keys, gestures and touches, and takes Escape while the window holds the
// pointer. Returns true for an event that it consumed.
bool inputWindowEvent(Features& f, PrStudioWindow* window, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
