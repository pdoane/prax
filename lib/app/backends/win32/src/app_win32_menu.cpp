// The Win32 backend's menus: menu bars, context menus and status items.

#include "app/backends/win32/src/app_win32_menu.h"

#include "app/backends/win32/src/app_win32_data.h"
#include "app/backends/win32/src/app_win32_input.h"
#include "app/backends/win32/src/app_win32_shell.h"
#include "app/backends/win32/src/app_win32_window.h"

namespace pr_impl {

namespace {

// Returns the virtual key of `key`, or zero for none.
UINT virtualKeyOfKey(PrAppKey key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return UINT(key);
    if (key > ' ' && key < 127) {
        SHORT scan = VkKeyScanW(wchar_t(key));
        return scan != -1 ? UINT(scan & 0xFF) : 0;
    }
    if (key >= PrAppKey_F1 && key <= PrAppKey_F20)
        return VK_F1 + UINT(key - PrAppKey_F1);
    switch (key) {
        case PrAppKey_Enter: return VK_RETURN;
        case PrAppKey_Escape: return VK_ESCAPE;
        case PrAppKey_Backspace: return VK_BACK;
        case PrAppKey_Tab: return VK_TAB;
        case PrAppKey_Delete: return VK_DELETE;
        case PrAppKey_Insert: return VK_INSERT;
        case PrAppKey_Left: return VK_LEFT;
        case PrAppKey_Right: return VK_RIGHT;
        case PrAppKey_Up: return VK_UP;
        case PrAppKey_Down: return VK_DOWN;
        case PrAppKey_Home: return VK_HOME;
        case PrAppKey_End: return VK_END;
        case PrAppKey_PageUp: return VK_PRIOR;
        case PrAppKey_PageDown: return VK_NEXT;
        default: return 0;
    }
}

// What Windows does for an item of a role: the title and the key of an item that the program leaves without them.
struct Win32MenuRole {
    PrAppMenuRole role;
    const char* title;
    PrAppKey key;
    PrAppModifierFlags modifiers;
};

const Win32MenuRole* PR_NULLABLE menuRoleOf(PrAppMenuRole role) {
    static const Win32MenuRole kRoles[] = {
        {PrAppMenuRole_About, "About", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Hide, "Hide", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Quit, "Exit", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Undo, "Undo", PrAppKey('Z'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Redo, "Redo", PrAppKey('Y'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Cut, "Cut", PrAppKey('X'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Copy, "Copy", PrAppKey('C'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Paste, "Paste", PrAppKey('V'), PrAppModifierFlag_Control},
        {PrAppMenuRole_SelectAll, "Select All", PrAppKey('A'), PrAppModifierFlag_Control},
        {PrAppMenuRole_Minimize, "Minimize", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Zoom, "Maximize", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Fullscreen, "Full Screen", PrAppKey_F11, 0},
        {PrAppMenuRole_WindowList, "Window", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Help, "Help", PrAppKey_Unknown, 0},
    };
    for (const Win32MenuRole& entry : kRoles) {
        if (entry.role == role)
            return &entry;
    }
    return nullptr;
}

// Appends the text of a shortcut to `out`, as a menu shows it after a tab: "Ctrl+Shift+O".
void appendShortcut(RawArray<char>& out, PrAppKey key, PrAppModifierFlags modifiers) {
    auto append = [&out](PrStringView text) { out.append(text.data, uint32_t(text.size)); };
    append("\t");
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        append("Ctrl+");
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        append("Alt+");
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        append("Shift+");
    if (key > ' ' && key < 127) {
        char character = char(key);
        out.push(character);
        return;
    }
    if (key >= PrAppKey_F1 && key <= PrAppKey_F20) {
        char name[4] = {'F', 0, 0, 0};
        uint32_t number = uint32_t(key - PrAppKey_F1) + 1;
        name[1] = char('0' + (number >= 10 ? number / 10 : number));
        if (number >= 10)
            name[2] = char('0' + number % 10);
        append(PrStringView{name, number >= 10 ? 3u : 2u});
        return;
    }
    append(prAppKeyName(key));
}

// Returns the id of a new command. WM_COMMAND carries 16 bits, and ids below 0x100 are left to the system.
UINT newCommandId(Win32App* app) {
    app->lastCommandId = app->lastCommandId >= 0xEFFF || app->lastCommandId < 0x100 ? 0x100 : app->lastCommandId + 1;
    return app->lastCommandId;
}

// Removes every item of `menu`, keeping its submenus, which RemoveMenu detaches rather than destroys.
void menuEmpty(HMENU menu) {
    while (GetMenuItemCount(menu) > 0)
        RemoveMenu(menu, 0, MF_BYPOSITION);
}

HMENU menuFill(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
    const PrAppMenu& description);

// Appends the item that `description` describes to `menu`.
void menuItemAppend(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
        HMENU menu, const PrAppMenuItem& description) {
    if (description.role == PrAppMenuRole_Separator) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        return;
    }
    const Win32MenuRole* role = menuRoleOf(description.role);
    RawArray<char> text;
    PrStringView title = description.title;
    if (title.size == 0 && description.submenu != nullptr)
        title = description.submenu->title;
    if (title.size == 0 && role != nullptr)
        title = PrStringView{role->title, strlen(role->title)};
    text.append(title.data, uint32_t(title.size));
    PrAppKey key = description.key;
    PrAppModifierFlags modifiers = description.modifiers;
    if (key == PrAppKey_Unknown && role != nullptr) {
        key = role->key;
        modifiers = role->modifiers;
    }
    UINT flags = MF_STRING;
    if ((description.flags & PrAppMenuItemFlag_Disabled) != 0)
        flags |= MF_GRAYED;
    if ((description.flags & PrAppMenuItemFlag_Checked) != 0)
        flags |= MF_CHECKED;
    if (description.submenu != nullptr) {
        HMENU submenu = menuFill(app, tree, previous, accelerators, *description.submenu);
        windowsWiden(PrStringView{text.data, text.count}, app->utf16);
        AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), app->utf16.data);
        text.reset();
        return;
    }
    UINT id = newCommandId(app);
    tree.commands.push(Win32Command{.id = id, .tag = description.tag, .role = description.role});
    UINT virtualKey = key != PrAppKey_Unknown ? virtualKeyOfKey(key) : 0;
    if (virtualKey != 0) {
        appendShortcut(text, key, modifiers);
        BYTE virt = FVIRTKEY;
        if ((modifiers & PrAppModifierFlag_Control) != 0)
            virt |= FCONTROL;
        if ((modifiers & PrAppModifierFlag_Alt) != 0)
            virt |= FALT;
        if ((modifiers & PrAppModifierFlag_Shift) != 0)
            virt |= FSHIFT;
        accelerators.push(ACCEL{.fVirt = virt, .key = WORD(virtualKey), .cmd = WORD(id)});
    }
    windowsWiden(PrStringView{text.data, text.count}, app->utf16);
    AppendMenuW(menu, flags, id, app->utf16.data);
    text.reset();
}

// Fills the menu of `description`'s tag, from `previous` if one has the tag and new otherwise, and returns it.
HMENU menuFill(Win32App* app, Win32MenuTree& tree, RawArray<Win32Menu>& previous, RawArray<ACCEL>& accelerators,
        const PrAppMenu& description) {
    HMENU handle = nullptr;
    for (uint32_t i = 0; i < previous.count; ++i) {
        if (previous.data[i].tag == description.tag) {
            handle = previous.data[i].handle;
            previous.erase(i);
            break;
        }
    }
    if (handle == nullptr)
        handle = CreatePopupMenu();
    tree.menus.push(Win32Menu{.handle = handle, .tag = description.tag});
    for (uint32_t i = 0; i < description.count; ++i)
        menuItemAppend(app, tree, previous, accelerators, handle, description.items[i]);
    return handle;
}

// Makes `tree` the menus that `description` describes: a menu bar of its items' submenus, if `bar`, or else the menu
// itself. The menus of the same tags are filled in place.
void menuTreeSet(Win32App* app, Win32MenuTree& tree, const PrAppMenu& description, bool bar) {
    RawArray<Win32Menu> previous;
    previous.append(tree.menus.data, tree.menus.count);
    tree.menus.clear();
    tree.commands.clear();
    // The old menus are emptied first, so that none is a submenu of another while the new tree takes them.
    for (uint32_t i = 0; i < previous.count; ++i)
        menuEmpty(previous.data[i].handle);
    RawArray<ACCEL> accelerators;
    tree.bar = bar;
    if (bar) {
        if (tree.root == nullptr)
            tree.root = CreateMenu();
        menuEmpty(tree.root);
        for (uint32_t i = 0; i < description.count; ++i)
            menuItemAppend(app, tree, previous, accelerators, tree.root, description.items[i]);
    } else {
        // A menu's root is one of its menus, which the menus of the same tag reuse or the loop below frees.
        tree.root = menuFill(app, tree, previous, accelerators, description);
    }
    for (uint32_t i = 0; i < previous.count; ++i) {
        if (previous.data[i].handle != tree.root)
            DestroyMenu(previous.data[i].handle);
    }
    previous.reset();
    if (tree.accelerators != nullptr)
        DestroyAcceleratorTable(tree.accelerators);
    tree.accelerators = nullptr;
    if (accelerators.count != 0)
        tree.accelerators = CreateAcceleratorTableW(accelerators.data, int(accelerators.count));
    accelerators.reset();
}

// Returns a copy of `description` that owns its strings and its submenus.
Win32MenuCopy* menuCopyOf(const PrAppMenu& description) {
    Win32MenuCopy* copy = allocZeroed<Win32MenuCopy>();
    appStringsAdd(copy->strings, description.title);
    for (uint32_t i = 0; i < description.count; ++i)
        appStringsAdd(copy->strings, description.items[i].title);
    const PrStringView* titles = appStringsViews(copy->strings);
    copy->menu = PrAppMenu{.tag = description.tag, .title = titles[0]};
    for (uint32_t i = 0; i < description.count; ++i) {
        PrAppMenuItem item = description.items[i];
        item.title = titles[i + 1];
        if (item.submenu != nullptr) {
            Win32MenuCopy* submenu = menuCopyOf(*item.submenu);
            copy->submenus.push(submenu);
            item.submenu = &submenu->menu;
        }
        copy->items.push(item);
    }
    copy->menu.items = copy->items.data;
    copy->menu.count = copy->items.count;
    return copy;
}

// Finds the menu `handle` among the trees that `window` shows, or among the status items' menus for a null window.
// `ofWindow` is true for a menu whose events go to the window: its own menu bar or its context menu.
bool menuTagOf(Win32App* app, Win32Window* PR_NULLABLE window, HMENU handle, uint64_t* out, bool* ofWindow) {
    auto find = [handle, out](const Win32MenuTree& tree) {
        for (uint32_t i = 0; i < tree.menus.count; ++i) {
            if (tree.menus.data[i].handle == handle) {
                *out = tree.menus.data[i].tag;
                return true;
            }
        }
        return false;
    };
    *ofWindow = false;
    if (window != nullptr && find(window->contextMenu)) {
        *ofWindow = true;
        return true;
    }
    if (window != nullptr && find(window->menuBar)) {
        *ofWindow = window->ownMenuBar;
        return true;
    }
    for (uint32_t i = 0; window == nullptr && i < app->statusItems.count; ++i) {
        if (find(app->statusItems.data[i]->menu))
            return true;
    }
    return false;
}

void statusItemApply(Win32StatusItem* item, const PrAppStatusItemDesc& desc, DWORD action) {
    Win32App* app = item->app;
    if (item->icon != nullptr)
        DestroyIcon(item->icon);
    item->icon = win32IconOf(desc.icon);
    item->tag = desc.tag;
    item->hasMenu = desc.menu != nullptr;
    if (item->hasMenu)
        menuTreeSet(app, item->menu, *desc.menu, false);
    else
        win32MenuTreeReset(item->menu);
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = item->id;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kMessageStatusItem;
    icon.hIcon = item->icon;
    if (icon.hIcon == nullptr)
        icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
    PrStringView tip = desc.tooltip.size != 0 ? desc.tooltip : desc.title;
    win32CopyField(app, tip, icon.szTip, countOf(icon.szTip));
    Shell_NotifyIconW(action, &icon);
    if (action == NIM_ADD) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }
}

void statusItemSet(PrAppStatusItem* base, const PrAppStatusItemDesc* desc) {
    statusItemApply(static_cast<Win32StatusItem*>(base), *desc, NIM_MODIFY);
}

} // namespace

void win32MenuTreeReset(Win32MenuTree& tree) {
    for (uint32_t i = 0; i < tree.menus.count; ++i)
        menuEmpty(tree.menus.data[i].handle);
    for (uint32_t i = 0; i < tree.menus.count; ++i)
        DestroyMenu(tree.menus.data[i].handle);
    if (tree.bar && tree.root != nullptr)
        DestroyMenu(tree.root);
    tree.menus.reset();
    tree.commands.reset();
    tree.root = nullptr;
    if (tree.accelerators != nullptr)
        DestroyAcceleratorTable(tree.accelerators);
    tree.accelerators = nullptr;
}

void win32MenuCopyReset(Win32MenuCopy* PR_NULLABLE copy) {
    if (copy == nullptr)
        return;
    for (uint32_t i = 0; i < copy->submenus.count; ++i)
        win32MenuCopyReset(copy->submenus.data[i]);
    copy->submenus.reset();
    copy->items.reset();
    appStringsReset(copy->strings);
    prMemoryFree(copy);
}

void win32ShowMenuBar(Win32Window* window) {
    Win32App* app = appOf(window);
    if (window->ownMenuBar || window->role != PrAppWindowRole_Normal || window->customTitleBar)
        return;
    if (app->menuBar == nullptr) {
        if (GetMenu(window->window) != nullptr)
            SetMenu(window->window, nullptr);
        win32MenuTreeReset(window->menuBar);
        return;
    }
    menuTreeSet(app, window->menuBar, app->menuBar->menu, true);
    if (GetMenu(window->window) != window->menuBar.root)
        SetMenu(window->window, window->menuBar.root);
    DrawMenuBar(window->window);
}

void win32ShowContextMenu(Win32Window* window) {
    if (!window->contextPending || window->contextMenu.root == nullptr)
        return;
    window->contextPending = false;
    POINT at{MulDiv(int(window->contextAt.x), int(window->dpi), 96), MulDiv(int(window->contextAt.y), int(window->dpi), 96)};
    ClientToScreen(window->window, &at);
    TrackPopupMenuEx(window->contextMenu.root, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, at.x, at.y,
        window->window, nullptr);
}

void win32MenuOpening(Win32App* app, Win32Window* PR_NULLABLE window, HMENU menu) {
    uint64_t tag = 0;
    bool ofWindow = false;
    if (!menuTagOf(app, window, menu, &tag, &ofWindow))
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_MenuOpening;
    event.time = win32SecondsNow();
    event.tag = tag;
    win32Deliver(app, ofWindow ? window : nullptr, event);
}

void win32MenuCommand(Win32App* app, Win32Window* PR_NULLABLE window, UINT id) {
    const Win32Command* command = nullptr;
    bool ofWindow = false;
    auto find = [id, &command](const Win32MenuTree& tree) {
        for (uint32_t i = 0; i < tree.commands.count; ++i) {
            if (tree.commands.data[i].id == id) {
                command = &tree.commands.data[i];
                return true;
            }
        }
        return false;
    };
    if (window != nullptr && find(window->contextMenu))
        ofWindow = true;
    else if (window != nullptr && find(window->menuBar))
        ofWindow = window->ownMenuBar;
    for (uint32_t i = 0; command == nullptr && i < app->statusItems.count; ++i)
        find(app->statusItems.data[i]->menu);
    if (command == nullptr)
        return;
    Win32Window* focused = window != nullptr ? window : app->lastActive;
    PrAppEvent event{};
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.tag = command->tag;
    switch (command->role) {
        case PrAppMenuRole_Quit:
            event.kind = PrAppEventKind_QuitRequested;
            win32Deliver(app, nullptr, event);
            return;
        case PrAppMenuRole_Hide:
            app->setHiddenFn(app, true);
            return;
        case PrAppMenuRole_Minimize:
            if (focused != nullptr)
                ShowWindow(focused->window, SW_MINIMIZE);
            return;
        case PrAppMenuRole_Zoom:
            if (focused != nullptr)
                ShowWindow(focused->window, IsZoomed(focused->window) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        case PrAppMenuRole_Fullscreen:
            if (focused != nullptr) {
                bool full = win32State(focused) == PrAppWindowState_Fullscreen;
                win32SetState(focused, full ? PrAppWindowState_Normal : PrAppWindowState_Fullscreen);
            }
            return;
        case PrAppMenuRole_Undo:
        case PrAppMenuRole_Redo:
        case PrAppMenuRole_Cut:
        case PrAppMenuRole_Copy:
        case PrAppMenuRole_Paste:
        case PrAppMenuRole_SelectAll:
            // An editing item acts on the window that has the keyboard.
            event.kind = PrAppEventKind_MenuItem;
            if (focused != nullptr)
                win32Deliver(focused, event);
            return;
        default:
            event.kind = PrAppEventKind_MenuItem;
            win32Deliver(app, ofWindow ? window : nullptr, event);
            return;
    }
}

bool win32TranslateAccelerator(Win32App* app, MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
        return false;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->window != message.hwnd)
            continue;
        HACCEL table = window->menuBar.accelerators;
        return table != nullptr && TranslateAcceleratorW(window->window, table, &message) != 0;
    }
    return false;
}

void win32StatusItemDestroy(PrAppStatusItem* base) {
    Win32StatusItem* item = static_cast<Win32StatusItem*>(base);
    Win32App* app = item->app;
    for (uint32_t i = 0; i < app->statusItems.count; ++i) {
        if (app->statusItems.data[i] == item) {
            app->statusItems.erase(i);
            break;
        }
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app->messageWindow;
    icon.uID = item->id;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    win32MenuTreeReset(item->menu);
    if (item->icon != nullptr)
        DestroyIcon(item->icon);
    prMemoryFree(item);
}

void win32StatusItemAction(Win32App* app, UINT id, UINT action) {
    if (action != NIN_SELECT && action != NIN_KEYSELECT && action != WM_CONTEXTMENU)
        return;
    for (uint32_t i = 0; i < app->statusItems.count; ++i) {
        Win32StatusItem* item = app->statusItems.data[i];
        if (item->id != id)
            continue;
        if (!item->hasMenu) {
            PrAppEvent event{};
            event.kind = PrAppEventKind_StatusItem;
            event.time = win32SecondsNow();
            event.tag = item->tag;
            win32Deliver(app, nullptr, event);
            return;
        }
        // A menu of the notification area closes as the user clicks elsewhere only if its owner is the foreground
        // window, and the message that follows lets the shell see the menu end.
        POINT at{};
        GetCursorPos(&at);
        SetForegroundWindow(app->messageWindow);
        TrackPopupMenuEx(item->menu.root, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, app->messageWindow, nullptr);
        PostMessageW(app->messageWindow, WM_NULL, 0, 0);
        return;
    }
}

void win32SetMenuBar(PrApp* base, const PrAppMenu* PR_NULLABLE menu) {
    Win32App* app = static_cast<Win32App*>(base);
    win32MenuCopyReset(app->menuBar);
    app->menuBar = menu != nullptr ? menuCopyOf(*menu) : nullptr;
    for (uint32_t i = 0; i < app->windows.count; ++i)
        win32ShowMenuBar(app->windows.data[i]);
}

PrStatus win32StatusItemCreate(PrApp* base, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    if (app->headless)
        return PrStatus_NotSupported;
    Win32StatusItem* item = allocZeroed<Win32StatusItem>();
    item->destroyFn = win32StatusItemDestroy;
    item->setFn = statusItemSet;
    item->app = app;
    item->id = kNotifyIconId + 1 + ++app->lastStatusItemId;
    app->statusItems.push(item);
    statusItemApply(item, *desc, NIM_ADD);
    *out = item;
    return PrStatus_Ok;
}

void win32WindowSetMenuBar(PrAppWindow* base, const PrAppMenu* PR_NULLABLE menu) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->ownMenuBar = menu != nullptr;
    if (menu == nullptr) {
        win32ShowMenuBar(window);
        return;
    }
    if (window->customTitleBar)
        return;
    menuTreeSet(appOf(window), window->menuBar, *menu, true);
    if (GetMenu(window->window) != window->menuBar.root)
        SetMenu(window->window, window->menuBar.root);
    DrawMenuBar(window->window);
}

void win32ShowMenu(PrAppWindow* base, const PrAppMenu* menu, PrVec2 position) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless)
        return;
    menuTreeSet(appOf(window), window->contextMenu, *menu, false);
    window->contextPending = true;
    window->contextAt = position;
    PostMessageW(window->window, kMessageShowMenu, 0, 0);
}

} // namespace pr_impl
