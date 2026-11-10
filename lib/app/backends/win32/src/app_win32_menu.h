// The Win32 backend's menus: menu bars, context menus and status items.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Frees a tree's menus. A menu bar that a window shows must leave the window first.
void win32MenuTreeReset(Win32MenuTree& tree);

// Frees a copy of a menu's description, and its submenus.
void win32MenuCopyReset(Win32MenuCopy* PR_NULLABLE copy);

// Shows the application's menu bar on a Normal window that has none of its own. A custom title bar has no room for a
// menu bar, so its window shows none.
void win32ShowMenuBar(Win32Window* window);

// Shows the context menu that waits, which runs the menu's modal loop until the user chooses or dismisses it.
void win32ShowContextMenu(Win32Window* window);

// Delivers MenuOpening for a menu that is about to open, to the window whose menu it is, or to the application for its
// menu bar and its status items.
void win32MenuOpening(Win32App* app, Win32Window* PR_NULLABLE window, HMENU menu);

// Acts on the command `id` of a menu: a role's action, or a MenuItem with the item's tag, to the window whose menu it
// is, or to the application for its menu bar and its status items.
void win32MenuCommand(Win32App* app, Win32Window* PR_NULLABLE window, UINT id);

// Translates a key of a menu's shortcut into its command, for the window that the key goes to. Returns true if the key
// was a shortcut.
bool win32TranslateAccelerator(Win32App* app, MSG& message);

// Removes the status item from the notification area, and frees it.
void win32StatusItemDestroy(PrAppStatusItem* base);

// Acts on a click on a status item: opens its menu at the pointer, or delivers StatusItem.
void win32StatusItemAction(Win32App* app, UINT id, UINT action);

// Implements prAppSetMenuBar.
void win32SetMenuBar(PrApp* base, const PrAppMenu* PR_NULLABLE menu);

// Implements prAppStatusItemCreate.
PrStatus win32StatusItemCreate(PrApp* base, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out);

// Implements prAppWindowSetMenuBar.
void win32WindowSetMenuBar(PrAppWindow* base, const PrAppMenu* PR_NULLABLE menu);

// Implements prAppWindowShowMenu.
void win32ShowMenu(PrAppWindow* base, const PrAppMenu* menu, PrVec2 position);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
