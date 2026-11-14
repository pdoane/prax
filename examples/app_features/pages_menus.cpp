// The menus' pages: the application's menu bar, a window's own, a context menu, and a status item with and without a
// menu.

#include "examples/app_features/pages_menus.h"

#include "pr/count_of.h"
#include "pr/kit_box.h"
#include "pr/kit_choice.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/ui.h"

#include "examples/app_features/features.h"
#include "examples/app_features/pages_window.h"

namespace features {

using namespace pr;

namespace {

// The modifier of a menu's shortcuts: Command on a Mac, and Control elsewhere.
#if defined(__APPLE__)
constexpr PrAppModifierFlags kShortcut = PrAppModifierFlag_Super;
#else
constexpr PrAppModifierFlags kShortcut = PrAppModifierFlag_Control;
#endif

// The tags of the menus' items, and of the menus, which MenuItem and MenuOpening carry.
enum {
    Tag_NewWindow = 1,
    Tag_Open,
    Tag_Toolbar,
    Tag_Undo,
    Tag_Redo,
    Tag_Cut,
    Tag_Copy,
    Tag_Paste,
    Tag_SelectAll,
    Tag_Minimize,
    Tag_Zoom,
    Tag_Fullscreen,
    Tag_Front,
    Tag_Help,
    Tag_WindowOnly,
    Tag_ContextCopy,
    Tag_ContextInspect,
    Tag_ShowControl,
    Tag_Quit,

    Tag_MenuBar = 100,
    Tag_FileMenu,
    Tag_EditMenu,
    Tag_ViewMenu,
    Tag_WindowMenu,
    Tag_HelpMenu,
    Tag_WindowBar,
    Tag_WindowBarMenu,
    Tag_ContextMenu,
    Tag_StatusMenu,
};
typedef uint8_t Tag;

// The tag of the status item, which a click on an item without a menu reports.
constexpr uint64_t kStatusItemTag = 7;

// Acts on a menu item: logs it, and does what the program's own items do.
void chosen(Features& f, uint64_t tag) {
    f.lastMenu = format("MenuItem tag {}", tag);
    switch (tag) {
        case Tag_NewWindow:
            if (f.second == nullptr)
                openSecond(f, nullptr);
            break;
        case Tag_Toolbar:
            f.toolbarShown = !f.toolbarShown;
            installMenuBar(f);
            break;
        case Tag_ShowControl:
            if (PrAppWindow* control = controlOf(f))
                prAppWindowFocus(control);
            break;
        case Tag_Paste:
            // The Paste item pastes the clipboard's text into the control window's focused field.
            if (f.control != nullptr) {
                PrUi* ui = prStudioWindowUi(f.control);
                prUiPaste(ui, prUiFocus(ui));
            }
            break;
        default:
            break;
    }
    if (PrAppWindow* control = controlOf(f))
        prAppWindowRequestFrame(control);
}

void opening(Features& f, uint64_t tag) {
    ++f.menuOpenings;
    f.lastMenu = format("MenuOpening tag {}", tag);
    // The View menu shows the toolbar's state as it opens, which setting the menu bar again updates in place.
    if (tag == Tag_ViewMenu)
        installMenuBar(f);
}

} // namespace

void installMenuBar(Features& f) {
    static const PrAppMenuItem kFile[] = {
        {.tag = Tag_NewWindow, .title = "New Window", .key = PrAppKey('N'), .modifiers = kShortcut},
        {.tag = Tag_Open, .title = "Open...", .key = PrAppKey('O'), .modifiers = kShortcut},
        {.role = PrAppMenuRole_Separator},
        {.tag = Tag_Quit, .role = PrAppMenuRole_Quit},
    };
    static const PrAppMenuItem kEdit[] = {
        {.tag = Tag_Undo, .role = PrAppMenuRole_Undo},
        {.tag = Tag_Redo, .role = PrAppMenuRole_Redo},
        {.role = PrAppMenuRole_Separator},
        {.tag = Tag_Cut, .role = PrAppMenuRole_Cut},
        {.tag = Tag_Copy, .role = PrAppMenuRole_Copy},
        {.tag = Tag_Paste, .role = PrAppMenuRole_Paste},
        {.tag = Tag_SelectAll, .role = PrAppMenuRole_SelectAll},
    };
    PrAppMenuItem view[] = {
        {
            .tag = Tag_Toolbar,
            .title = "Show Toolbar",
            .flags = PrAppMenuItemFlags(f.toolbarShown ? PrAppMenuItemFlag_Checked : 0),
            .key = PrAppKey('T'),
            .modifiers = kShortcut | PrAppModifierFlag_Alt,
        },
        {.tag = Tag_Fullscreen, .role = PrAppMenuRole_Fullscreen},
        {.title = "Disabled Item", .flags = PrAppMenuItemFlag_Disabled},
    };
    static const PrAppMenuItem kWindow[] = {
        {.tag = Tag_Minimize, .role = PrAppMenuRole_Minimize},
        {.tag = Tag_Zoom, .role = PrAppMenuRole_Zoom},
        {.role = PrAppMenuRole_Separator},
        {.tag = Tag_Front, .role = PrAppMenuRole_BringAllToFront},
    };
    static const PrAppMenuItem kHelp[] = {{.tag = Tag_Help, .title = "app_features Help"}};
    PrAppMenu file{.tag = Tag_FileMenu, .title = "File", .items = kFile, .count = uint32_t(countOf(kFile))};
    PrAppMenu edit{.tag = Tag_EditMenu, .title = "Edit", .items = kEdit, .count = uint32_t(countOf(kEdit))};
    PrAppMenu viewMenu{.tag = Tag_ViewMenu, .title = "View", .items = view, .count = uint32_t(countOf(view))};
    PrAppMenu window{.tag = Tag_WindowMenu, .title = "Window", .items = kWindow, .count = uint32_t(countOf(kWindow))};
    PrAppMenu help{.tag = Tag_HelpMenu, .title = "Help", .items = kHelp, .count = uint32_t(countOf(kHelp))};
    PrAppMenuItem top[] = {
        {.submenu = &file},
        {.submenu = &edit},
        {.submenu = &viewMenu},
        {.role = PrAppMenuRole_WindowList, .submenu = &window},
        {.role = PrAppMenuRole_Help, .submenu = &help},
    };
    PrAppMenu bar{.tag = Tag_MenuBar, .items = top, .count = uint32_t(countOf(top))};
    prAppSetMenuBar(appOf(f), f.appMenuBar ? &bar : nullptr);

    static const PrAppMenuItem kOwn[] = {{.tag = Tag_WindowOnly, .title = "An item of this window alone"}};
    PrAppMenu own{.tag = Tag_WindowBarMenu, .title = "This Window", .items = kOwn, .count = uint32_t(countOf(kOwn))};
    PrAppMenuItem windowTop[] = {{.submenu = &file}, {.submenu = &own}};
    PrAppMenu windowBar{.tag = Tag_WindowBar, .items = windowTop, .count = uint32_t(countOf(windowTop))};
    if (PrAppWindow* control = controlOf(f))
        prAppWindowSetMenuBar(control, f.windowMenuBar ? &windowBar : nullptr);
}

void showStatusItem(Features& f) {
    Array<uint8_t> pixels;
    static const PrAppMenuItem kItems[] = {
        {.tag = Tag_ShowControl, .title = "Show the control window"},
        {.role = PrAppMenuRole_Separator},
        {.tag = Tag_Quit, .role = PrAppMenuRole_Quit},
    };
    static const PrAppMenu kMenu{.tag = Tag_StatusMenu, .items = kItems, .count = uint32_t(countOf(kItems))};
    String title = format("Prax {}", f.statusClicks);
    PrAppStatusItemDesc desc{
        .tag = kStatusItemTag,
        .icon = iconPixels(pixels, 36, PrColor{1, 1, 1, 1}),
        .templateIcon = true,
        .title = title,
        .tooltip = "app_features",
        .menu = f.statusMenu ? &kMenu : nullptr,
    };
    if (f.statusItem != nullptr) {
        prAppStatusItemSet(f.statusItem, &desc);
        return;
    }
    PrStatus status = prAppStatusItemCreate(appOf(f), &desc, &f.statusItem);
    logLine(f, format("status item: {}", statusName(status)));
}

void pageMenus(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "MENU BARS");
    PrLayoutNode r = row(kit, s);
    PrLayoutNode app = prKitToggle(kit, r, {.value = &f.appMenuBar, .label = "Application's menu bar"}, {});
    PrLayoutNode own = prKitToggle(kit, r, {.value = &f.windowMenuBar, .label = "This window's own menu bar"}, {});
    if (prKitActivated(kit, app) || prKitActivated(kit, own))
        installMenuBar(f);
    PrStringView last = f.lastMenu.isEmpty() ? PrStringView("none") : PrStringView(f.lastMenu);
    line(kit, s, format("last: {}   openings: {}", last, f.menuOpenings));
    note(kit, s, "macOS shows the menu bar at the top of the screen, after the application's own menu. Windows shows it "
        "in each window without a custom title bar. The View menu's checked item updates as the menu opens. The Edit "
        "menu's items reach the window that has the keyboard, and a system text field.");

    PrLayoutNode c = section(kit, parent, "CONTEXT MENU");
    PrLayoutNode area = prKitPanel(kit, c, {}, PrKitPanelStyle{
        .box = {.align = PrKitAlign_Center, .padding = {kPrKitSpace6, kPrKitSpace5, kPrKitSpace6, kPrKitSpace5}},
        .background = PrKitBackground_Raised,
        .rules = PrKitRuleFlag_All,
    });
    prKitLabel(kit, area, {.text = "Right-click here"}, {.tone = PrKitTone_Muted});
    // A secondary click on the area opens the system's context menu at the pointer.
    PrUi* ui = prStudioWindowUi(window);
    prUiSetFlags(ui, area, PrUiFlag_Pressable);
    PrVec2 at{};
    if (prUiClicked(ui, area, 1) && prUiPointer(ui, nullptr, &at)) {
        static const PrAppMenuItem kItems[] = {
            {.tag = Tag_ContextCopy, .title = "Copy"},
            {.tag = Tag_ContextInspect, .title = "Inspect"},
            {.role = PrAppMenuRole_Separator},
            {.title = "Checked", .flags = PrAppMenuItemFlag_Checked},
        };
        PrAppMenu menu{.tag = Tag_ContextMenu, .items = kItems, .count = uint32_t(countOf(kItems))};
        prAppWindowShowMenu(prStudioWindowAppWindow(window), &menu, at);
    }
}

void pageStatusItems(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "STATUS ITEM");
    PrLayoutNode r = row(kit, s);
    if (button(kit, r, f.statusItem == nullptr ? "Add status item" : "Update status item"))
        showStatusItem(f);
    PrLayoutNode menu = prKitToggle(kit, r, {.value = &f.statusMenu, .label = "With a menu"}, {});
    if (prKitActivated(kit, menu) && f.statusItem != nullptr)
        showStatusItem(f);
    if (button(kit, r, "Remove status item", f.statusItem == nullptr)) {
        prAppStatusItemDestroy(f.statusItem);
        f.statusItem = nullptr;
    }
    line(kit, s, format("clicks without a menu: {}", f.statusClicks));
    note(kit, s, "The item shows in the menu bar's extras on macOS and in the notification area on Windows. Without a "
        "menu, a click sends StatusItem, which focuses the control window. Accessory presence on the Lifecycle page "
        "leaves the item as the application's only presence.");
}

bool menusAppEvent(Features& f, const PrAppEvent& e) {
    switch (e.kind) {
        case PrAppEventKind_MenuItem:
            chosen(f, e.tag);
            return true;
        case PrAppEventKind_MenuOpening:
            opening(f, e.tag);
            return true;
        case PrAppEventKind_StatusItem:
            ++f.statusClicks;
            showStatusItem(f);
            if (PrAppWindow* control = controlOf(f))
                prAppWindowFocus(control);
            return true;
        default:
            return false;
    }
}

bool menusWindowEvent(Features& f, const PrAppEvent& e) {
    switch (e.kind) {
        case PrAppEventKind_MenuItem:
            chosen(f, e.tag);
            return true;
        case PrAppEventKind_MenuOpening:
            opening(f, e.tag);
            return true;
        default:
            return false;
    }
}

} // namespace features
