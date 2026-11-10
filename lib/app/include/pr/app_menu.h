// Menus that the system draws: the menu bar, context menus and status items.
//
// A program describes a menu by value, and the system copies it. When the user chooses an item, a MenuItem event with
// the item's tag goes to the window that shows the menu, or to the application for its menu bar. Before a menu opens, a
// MenuOpening event with the menu's tag arrives, so that the program can set the menu again with its current state.

#pragma once

#include "pr/app_application.h"
#include "pr/app_input.h"
#include "pr/base.h"
#include "pr/pixels.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// A menu, defined below. An item refers to its submenu by pointer.
typedef struct PrAppMenu PrAppMenu;

// A standard item that the system implements. If the program leaves an item's title or key empty, the system supplies
// its own. Quit sends QuitRequested. The window roles act on the active window.
//
// The editing roles act on what has the keyboard. A system text field, such as one in a file dialog, handles them
// itself. A window of the program receives MenuItem with the item's tag, except for Paste, which sends a Paste event.
enum {
    PrAppMenuRole_None,
    PrAppMenuRole_Separator,
    PrAppMenuRole_About,
    PrAppMenuRole_Services,         // the Services submenu on macOS
    PrAppMenuRole_Hide,
    PrAppMenuRole_HideOthers,
    PrAppMenuRole_ShowAll,
    PrAppMenuRole_Quit,
    PrAppMenuRole_Undo,
    PrAppMenuRole_Redo,
    PrAppMenuRole_Cut,
    PrAppMenuRole_Copy,
    PrAppMenuRole_Paste,
    PrAppMenuRole_SelectAll,
    PrAppMenuRole_Minimize,
    PrAppMenuRole_Zoom,
    PrAppMenuRole_Fullscreen,
    PrAppMenuRole_BringAllToFront,
    PrAppMenuRole_WindowList,       // a submenu that the system fills with the application's windows
    PrAppMenuRole_Help,             // a submenu that holds the system's search field for menus
};
typedef uint8_t PrAppMenuRole;

// The state of a menu item.
enum {
    PrAppMenuItemFlag_Disabled = 1 << 0,
    PrAppMenuItemFlag_Checked = 1 << 1,
};
typedef uint8_t PrAppMenuItemFlags;

// An item in a menu. It is a command if `submenu` is null, and a submenu otherwise. `key` and `modifiers` are its
// keyboard shortcut, which the menu shows beside the title. PrAppKey_Unknown means that it has none. The system handles
// the shortcut before the window receives the key.
typedef struct PrAppMenuItem {
    uint64_t tag;
    PrStringView title;
    PrAppMenuRole role;
    PrAppMenuItemFlags flags;
    PrAppKey key;
    PrAppModifierFlags modifiers;
    const PrAppMenu* PR_NULLABLE submenu;
} PrAppMenuItem;

// A menu and its items, in order. The title labels a submenu in its parent menu or in the menu bar. MenuOpening events
// carry the tag.
struct PrAppMenu {
    uint64_t tag;
    PrStringView title;
    const PrAppMenuItem* PR_NULLABLE items;
    uint32_t count;
};

// Sets the application's menu bar, or clears it if `menu` is null. Each item of `menu` is a top-level menu. On macOS,
// the bar appears at the top of the screen, after the application menu. On other platforms, each Normal window shows it
// unless the window has a menu bar of its own.
void prAppSetMenuBar(PrApp* app, const PrAppMenu* PR_NULLABLE menu);

// Sets the window's own menu bar, which replaces the application's while the window is active. A null `menu` shows the
// application's menu bar again.
void prAppWindowSetMenuBar(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu);

// Shows `menu` as a context menu at `position`, in logical pixels from the window's top left. If the user chooses an
// item, the window receives MenuItem. If the user dismisses the menu, no event follows.
void prAppWindowShowMenu(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position);

// ---------------------------------------------------------------------------------------------------------------------
// Status items
//
// A status item is an icon that the application keeps in the menu bar extras on macOS, the notification area on
// Windows, or the system tray on Linux. An Accessory application often has no other presence.

// A status item, created by prAppStatusItemCreate.
typedef struct PrAppStatusItem PrAppStatusItem;

// The appearance and behavior of a status item. `icon` is straight-alpha sRGB Rgba8. If `templateIcon` is true, the
// system uses only the icon's alpha and colors it to match the menu bar. `title` appears beside the icon on platforms
// that support it. A click opens `menu`. If `menu` is null, a click sends a StatusItem event with `tag` instead.
typedef struct PrAppStatusItemDesc {
    uint64_t tag;
    PrPixelView icon;
    bool templateIcon;
    PrStringView title;
    PrStringView tooltip;
    const PrAppMenu* PR_NULLABLE menu;
} PrAppStatusItemDesc;

// Creates a status item from `desc`, which the item copies. Returns NotSupported if the platform has no status items,
// and InvalidState if the application is not running.
PrStatus prAppStatusItemCreate(PrApp* app, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out);

// Removes the status item and frees it.
void prAppStatusItemDestroy(PrAppStatusItem* PR_NULLABLE item);

// Replaces the appearance and behavior of the status item with `desc`, which the item copies.
void prAppStatusItemSet(PrAppStatusItem* item, const PrAppStatusItemDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
