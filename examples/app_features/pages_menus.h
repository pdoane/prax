// The menus' pages: the application's menu bar, a window's own, a context menu, and a status item with and without a
// menu.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Sets the menu bars that the Menus page chose: the application's, and the control window's own.
void installMenuBar(Features& f);

// Adds the status item, or updates its title and menu.
void showStatusItem(Features& f);

// Build the pages under `parent`, a column of the control window.
void pageMenus(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageStatusItems(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Take the choices and the openings of the menus, and the clicks of the status item. Each returns true for an event
// that it took.
bool menusAppEvent(Features& f, const PrAppEvent& e);
bool menusWindowEvent(Features& f, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
