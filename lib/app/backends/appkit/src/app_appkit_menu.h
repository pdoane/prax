// The AppKit backend's menus: menu bars, context menus and status items.

#pragma once

#include "app/backends/appkit/src/app_appkit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Makes `tree` the menu bar that `description` describes, after the application's own menu, updating its menus in
// place. A null description leaves the application's menu alone.
void appKitMenuBarOf(const PrAppMenu* PR_NULLABLE description, PrAppKitMenuTree* tree, PrAppKitHost* host,
    PrAppKitWindow* PR_NULLABLE owner);

// Implement the application's and the window's functions of their names (PrApp::setMenuBarFn and the others).
void appKitSetMenuBar(PrApp* app, const PrAppMenu* PR_NULLABLE menu);
void appKitWindowSetMenuBar(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu);
void appKitShowMenu(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position);
PrStatus appKitStatusItemCreate(PrApp* app, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
