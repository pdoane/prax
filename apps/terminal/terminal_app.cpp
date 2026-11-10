// The terminal app's interface: its tabs, its windows and their commands, and the work on its terminals.

#include "apps/terminal/terminal_app.h"

#include "pr/command_kit.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/kit_button.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/print.h"
#include "pr/studio_window.h"

namespace terminal {

using namespace pr;

namespace {

TerminalWindow& windowOf(void* context) {
    return *static_cast<TerminalWindow*>(context);
}

// The chords of the app's commands. On a Mac they use Command, which a terminal leaves to the app. Elsewhere they use
// Control with Shift, because the program receives Control chords.
#if defined(__APPLE__)
constexpr const char* kNewTab = "Cmd T";
constexpr const char* kCloseTab = "Cmd W";
constexpr const char* kNewWindow = "Cmd N";
constexpr const char* kQuit = "Cmd Q";
#else
constexpr const char* kNewTab = "Ctrl Shift T";
constexpr const char* kCloseTab = "Ctrl Shift W";
constexpr const char* kNewWindow = "Ctrl Shift N";
constexpr const char* kQuit = "";
#endif

// The font zoom levels that zooming in and out steps through, as in a browser.
constexpr float kZooms[] = {0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4};

// The size of the terminals' font at a zoom of 1, in px.
constexpr float kFontSize = 13;

// The monospaced family that the terminals are set in.
const PrFontFamily kMono{.generic = PrFontGeneric_Monospace};

// The extent that a new window opens at, in px.
constexpr PrUVec2 kWindowExtent{820, 560};

// Binds `chord` to the command `id` of the window, if `chord` is not empty. The binding is an override, because the
// terminal binds every chord that it can send to its program.
void bindChord(TerminalWindow& w, const char* id, const char* chord) {
    PrCommandChord parsed{};
    if (chord[0] != 0 && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(w.keymap, parsed, id, "", PrCommandBindingKind_Override);
}

// Adds a command of the window, bound to `chord` if it is not empty (bindChord).
void addCommand(TerminalWindow& w, const char* id, const char* title, const char* chord, PrCommandRunFn run) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Terminal", .run = run, .context = &w};
    prCommandRegistryAdd(w.commands, &desc);
    bindChord(w, id, chord);
}

// The terminals' font, at the app's zoom.
PrTextFontDesc fontOf(const TerminalApp& app) {
    PrFontStyle style = prFontStyleDefault();
    style.size = kFontSize * app.zoom;
    return PrTextFontDesc{.set = app.fonts, .families = &kMono, .familyCount = 1, .style = style};
}

// Returns the zoom after `zoom` in the direction of `step`, or `zoom` if it is at the end of the range.
float zoomStep(float zoom, int32_t step) {
    if (step > 0) {
        for (float z : kZooms) {
            if (z > zoom + 0.001f)
                return z;
        }
    } else {
        for (size_t i = countOf(kZooms); i-- > 0;) {
            if (kZooms[i] < zoom - 0.001f)
                return kZooms[i];
        }
    }
    return zoom;
}

// Sets the zoom of every terminal's font.
void setZoom(TerminalApp& app, float zoom) {
    if (zoom == app.zoom)
        return;
    app.zoom = zoom;
    PrTextFontDesc font = fontOf(app);
    for (const TerminalTab& tab : app.tabs)
        prTerminalSetFont(tab.terminal, &font);
}

// The id of a terminal's tab: its notification tag, which no other terminal of the app has.
uint64_t idOf(const PrTerminal* terminal) {
    return prTerminalNotificationTag(terminal);
}

// Returns the tab whose id is `id`, or null if no tab has it.
TerminalTab* PR_NULLABLE tabWithId(TerminalApp& app, uint64_t id) {
    for (TerminalTab& tab : app.tabs) {
        if (idOf(tab.terminal) == id)
            return &tab;
    }
    return nullptr;
}

// Returns the tab of `terminal`.
TerminalTab& tabOf(TerminalApp& app, const PrTerminal* terminal) {
    TerminalTab* tab = tabWithId(app, idOf(terminal));
    PR_ASSERT(tab != nullptr);
    return *tab;
}

// Returns the terminal of the window's selected tab, or null if the window has no tabs.
PrTerminal* PR_NULLABLE selectedOf(TerminalWindow& w) {
    TerminalTab* tab = tabWithId(*w.app, prKitTabsSelected(w.tabs));
    return tab != nullptr ? tab->terminal : nullptr;
}

// Follows a tab out of the window's strip: a window left without tabs closes, and otherwise, if the tab was `selected`,
// the tab selected in its place takes focus.
void vacated(TerminalWindow& w, bool selected) {
    if (prKitTabsCount(w.tabs) == 0) {
        if (w.window != nullptr)
            prStudioWindowDestroy(w.window);
        return;
    }
    if (selected)
        prTerminalFocus(selectedOf(w));
}

// Shows the window's tab `step` after the one that shows, wrapping around the ends of its strip.
void cycle(TerminalWindow& w, int32_t step) {
    if (TerminalTab* tab = tabWithId(*w.app, prKitTabsStep(w.tabs, step)))
        prTerminalFocus(tab->terminal);
}

// Applies a move of a tab between windows: a tab dropped on the window's strip from another window, or torn off from it
// into a window of its own. A window keeps its last tab.
void applyMove(TerminalWindow& w, const PrKitTabMove& move) {
    TerminalApp& app = *w.app;
    TerminalTab* tab = tabWithId(app, move.id);
    if (tab == nullptr)
        return;
    PrTerminal* terminal = tab->terminal;
    if (move.kind == PrKitTabMoveKind_Received) {
        moveTab(app, terminal, w, move.index);
        return;
    }
    if (app.studio == nullptr || prKitTabsCount(w.tabs) < 2)
        return;
    bool placed = move.position.display != kPrAppDisplayNone;
    if (TerminalWindow* torn = newWindow(app, placed ? &move.position : nullptr))
        moveTab(app, terminal, *torn, 0);
}

} // namespace

void openApp(TerminalApp& app) {
    app.launcher = prProcessLauncherCreate(PrProcessBackend_Default);
}

void closeApp(TerminalApp& app) {
    PR_ASSERT(app.windows.isEmpty() && app.tabs.isEmpty());
    prProcessLauncherDestroy(app.launcher);
    app.launcher = nullptr;
}

TerminalWindow* PR_NULLABLE newWindow(TerminalApp& app, const PrAppDisplayPosition* PR_NULLABLE position) {
    PR_ASSERT(app.studio != nullptr);
    PrAppWindowPlacement placement = PrAppWindowPlacement_Default;
    if (position != nullptr)
        placement = PrAppWindowPlacement_Position;
    TerminalWindow* w = new TerminalWindow{};
    PrStudioWindowDesc desc{
        .title = "Terminal",
        .extent = kWindowExtent,
        .placement = placement,
        .position = position != nullptr ? *position : PrAppDisplayPosition{},
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2 extent) {
            buildWindow(windowOf(context), root, extent);
        },
        .close = [](void* context, PrStudioWindow*) {
            TerminalWindow* closing = static_cast<TerminalWindow*>(context);
            closeWindow(*closing);
            delete closing;
        },
        .context = w,
    };
    // The Studio window makes its kit as it is created, so the window takes tabs before it opens.
    PrStudioWindow* window = nullptr;
    if (prStudioWindowCreate(app.studio, &desc, &window) != PrStatus_Ok) {
        delete w;
        return nullptr;
    }
    w->window = window;
    w->kit = prStudioWindowKit(window);
    w->ui = prStudioWindowUi(window);
    w->appWindow = prStudioWindowAppWindow(window);
    openWindow(app, *w);
    return w;
}

bool startWindow(TerminalWindow& w) {
    PrStatus status = openTab(w);
    if (status == PrStatus_Ok)
        return true;
    const TerminalApp& app = *w.app;
    PrStringView program = app.program.isEmpty() ? PrStringView("the shell") : app.program.view();
    if (status == PrStatus_NotFound)
        printError("terminal: cannot start {}\n", program);
    else
        printError("terminal: cannot make a terminal\n");
    if (w.window != nullptr)
        prStudioWindowDestroy(w.window);
    return false;
}

void openWindow(TerminalApp& app, TerminalWindow& w) {
    w.app = &app;
    w.grid = app.grid;
    app.grid = PrUVec2{};
    app.windows.push(&w);
    w.tabs = prKitTabsCreate();
    w.commands = prCommandRegistryCreate();
    w.keymap = prCommandKeymapCreate();
    addCommand(w, "tab.new", "New Tab", kNewTab, [](void* c, const PrCommandArgs*) { openTab(windowOf(c)); });
    addCommand(w, "tab.close", "Close Tab", kCloseTab, [](void* c, const PrCommandArgs*) {
        TerminalWindow& w = windowOf(c);
        if (PrTerminal* selected = selectedOf(w))
            closeTab(*w.app, selected);
    });
    addCommand(w, "tab.next", "Next Tab", "Ctrl Tab", [](void* c, const PrCommandArgs*) { cycle(windowOf(c), 1); });
    addCommand(w, "tab.previous", "Previous Tab", "Ctrl Shift Tab", [](void* c, const PrCommandArgs*) {
        cycle(windowOf(c), -1);
    });
    addCommand(w, "window.new", "New Window", kNewWindow, [](void* c, const PrCommandArgs*) {
        TerminalApp& app = *windowOf(c).app;
        if (app.studio == nullptr)
            return;
        if (TerminalWindow* opened = newWindow(app, nullptr))
            startWindow(*opened);
    });
    addCommand(w, "app.quit", "Quit", kQuit, [](void* c, const PrCommandArgs*) {
        TerminalApp& app = *windowOf(c).app;
        if (app.studio != nullptr)
            prStudioAppStop(app.studio);
    });
    // The browser's zoom chords, with the primary modifier: Command on a Mac and Control elsewhere. Elsewhere this
    // takes Control with minus away from the program, as the platforms' own terminals do.
    addCommand(w, "view.zoomIn", "Zoom In", "Cmd =", [](void* c, const PrCommandArgs*) {
        TerminalApp& app = *windowOf(c).app;
        setZoom(app, zoomStep(app.zoom, 1));
    });
    bindChord(w, "view.zoomIn", "Cmd Shift =");
    bindChord(w, "view.zoomIn", "Cmd Shift +");
    addCommand(w, "view.zoomOut", "Zoom Out", "Cmd -", [](void* c, const PrCommandArgs*) {
        TerminalApp& app = *windowOf(c).app;
        setZoom(app, zoomStep(app.zoom, -1));
    });
    addCommand(w, "view.zoomReset", "Actual Size", "Cmd 0", [](void* c, const PrCommandArgs*) {
        setZoom(*windowOf(c).app, 1);
    });
}

void closeWindow(TerminalWindow& w) {
    TerminalApp& app = *w.app;
    for (uint32_t i = app.tabs.count(); i-- > 0;) {
        if (app.tabs[i].window == &w) {
            prTerminalDestroy(app.tabs[i].terminal);
            app.tabs.erase(i);
        }
    }
    prKitTabsDestroy(w.tabs);
    prCommandKeymapDestroy(w.keymap);
    prCommandRegistryDestroy(w.commands);
    w.tabs = nullptr;
    w.keymap = nullptr;
    w.commands = nullptr;
    app.windows.erase(app.windows.find(&w));
}

PrStatus openTab(TerminalWindow& w) {
    PR_ASSERT(w.kit != nullptr && w.app != nullptr);
    TerminalApp& app = *w.app;
    Array<PrStringView> arguments;
    for (const String& argument : app.arguments)
        arguments.push(argument);
    // A new tab starts in the working directory of the tab that shows.
    PrRawString directory = prRawStringNull();
    if (PrTerminal* selected = selectedOf(w))
        directory = prTerminalWorkingDirectory(selected);
    PrTerminalDesc desc{
        .app = app.studio != nullptr ? prStudioAppApp(app.studio) : nullptr,
        .font = fontOf(app),
        .launcher = app.launcher,
        .program = app.program,
        .arguments = arguments.isEmpty() ? nullptr : &arguments[0],
        .argumentCount = arguments.count(),
        .directory = prRawStringView(&directory),
        .scrollback = app.scrollback,
        .notificationTag = app.nextTag,
    };
    PrTerminal* terminal = nullptr;
    PrStatus status = prTerminalCreate(&desc, &terminal);
    prRawStringReset(&directory);
    if (status != PrStatus_Ok)
        return status;
    ++app.nextTag;
    app.tabs.push(TerminalTab{.terminal = terminal, .window = &w});
    prKitTabsInsert(w.tabs, idOf(terminal), prKitTabsCount(w.tabs));
    prKitTabsSelect(w.tabs, idOf(terminal));
    prTerminalFocus(terminal);
    return PrStatus_Ok;
}

Array<PrTerminal*> tabsOf(TerminalWindow& w) {
    Array<PrTerminal*> terminals;
    for (uint32_t i = 0; i < prKitTabsCount(w.tabs); ++i)
        terminals.push(tabWithId(*w.app, prKitTabsAt(w.tabs, i))->terminal);
    return terminals;
}

void closeTab(TerminalApp& app, PrTerminal* terminal) {
    TerminalTab& tab = tabOf(app, terminal);
    TerminalWindow& w = *tab.window;
    uint64_t id = idOf(terminal);
    bool selected = prKitTabsSelected(w.tabs) == id;
    app.tabs.erase(uint32_t(&tab - app.tabs.data()));
    prKitTabsRemove(w.tabs, id);
    vacated(w, selected);
    prTerminalDestroy(terminal);
}

void moveTab(TerminalApp& app, PrTerminal* terminal, TerminalWindow& to, uint32_t index) {
    TerminalTab& tab = tabOf(app, terminal);
    TerminalWindow& from = *tab.window;
    uint64_t id = idOf(terminal);
    bool selected = prKitTabsSelected(from.tabs) == id;
    tab.window = &to;
    prKitTabsMove(from.tabs, to.tabs, id, index);
    prTerminalFocus(terminal);
    if (&from == &to)
        return;
    // The window that the tab left closes if it has no tabs left, so it is asked for a frame first.
    if (from.appWindow != nullptr)
        prAppWindowRequestFrame(from.appWindow);
    vacated(from, selected);
}

void serviceTabs(TerminalApp& app) {
    for (const TerminalTab& tab : app.tabs)
        prTerminalService(tab.terminal, tab.window->appWindow);
    for (uint32_t i = app.tabs.count(); i-- > 0;) {
        if (i >= app.tabs.count())
            continue;
        PrProcessInfo info = prTerminalProcessInfo(app.tabs[i].terminal);
        if (info.exited && info.outputEnded)
            closeTab(app, app.tabs[i].terminal);
    }
}

void buildWindow(TerminalWindow& w, PrLayoutNode root, PrVec2 extent) {
    TerminalApp& app = *w.app;
    prCommandScope(w.ui, root, w.commands, w.keymap, "");
    // The strip reports a tab moved between windows before its tabs, so that the round shows the tab where it went.
    PrKitTabStrip content{.tabs = w.tabs, .group = kTabGroup, .reorderable = true, .collapsesSingle = true};
    w.strip = prKitTabStrip(w.kit, root, content, {});
    PrKitTabMove move{};
    if (prKitTabMoved(w.kit, w.strip, &move))
        applyMove(w, move);
    PrAppColorScheme scheme = prUiUserPreferences(w.ui).colorScheme;
    PrTerminal* closed = nullptr;
    for (const TerminalTab& tab : app.tabs) {
        if (tab.window != &w)
            continue;
        PrTerminal* terminal = tab.terminal;
        // The terminals' programs see the color scheme that the window's theme follows.
        prTerminalSetColorScheme(terminal, scheme);
        PrLayoutNode node = prKitTab(w.kit, w.strip, {.id = idOf(terminal)});
        prKitTabLabel(w.kit, node, {.text = prTerminalTitle(terminal)});
        // A badge marks a tab whose program rang the bell or posted a notification that the user has not seen. A
        // notification takes precedence over a bell. The badge has its own key, so the close control keeps its place
        // among the tab's parts.
        PrTerminalAlertFlags alerts = prTerminalAlerts(terminal);
        if (alerts != 0) {
            bool notified = (alerts & PrTerminalAlertFlag_Notification) != 0;
            PrKitBadge badge{.key = prUiKey(w.ui, node, "alert"), .text = notified ? "new" : "bell"};
            PrKitBadgeVariant variant = notified ? PrKitBadgeVariant_Accent : PrKitBadgeVariant_Warning;
            prKitBadge(w.kit, node, badge, {.variant = variant});
        }
        // A click on a tab gives its terminal focus. The arrow keys leave focus on the strip.
        if (prKitActivated(w.kit, node))
            prTerminalFocus(terminal);
        if (prKitActivated(w.kit, prKitTabClose(w.kit, node)))
            closed = terminal;
    }
    // The closed tab disappears in the next round, which lays out the strip without it.
    if (closed != nullptr) {
        closeTab(app, closed);
        prUiRequestFrame(w.ui);
        return;
    }
    PrTerminal* shown = selectedOf(w);
    if (shown == nullptr)
        return;
    // The terminal fills what the tab strip leaves of the window.
    prTerminalFrame(shown, w.kit, root, PrTerminalBackground_Parent);
    PrVec2 cell = prTerminalCellExtent(shown);
    PrVec2 view{};
    bool measured = prUiExtent(w.ui, prTerminalNode(shown), &view);
    if (w.grid.x > 0 && w.grid.y > 0 && cell.x > 0 && cell.y > 0 && measured && w.appWindow != nullptr) {
        // The window is sized to fit the grid in whole cells from the screen's origin, and keeps the room that the
        // terminal's view leaves to the rest of the window, such as the tab strip.
        PrVec2 origin = prTerminalScreenOrigin(shown);
        float windowWidth = extent.x - view.x + origin.x + float(w.grid.x) * cell.x;
        float windowHeight = extent.y - view.y + origin.y + float(w.grid.y) * cell.y;
        prAppWindowSetExtent(w.appWindow, PrUVec2{uint32_t(prF32Ceil(windowWidth)), uint32_t(prF32Ceil(windowHeight))});
        w.grid = PrUVec2{};
    }

    // The window's title is the title of the terminal that shows.
    PrStringView title = prTerminalTitle(shown);
    if (w.appWindow != nullptr && title != PrStringView(w.title)) {
        w.title = title;
        prAppWindowSetTitle(w.appWindow, title);
    }
}

bool handleEvent(TerminalApp& app, const PrAppEvent& event) {
    if (event.kind == PrAppEventKind_Wake) {
        serviceTabs(app);
        return true;
    }
    if (event.kind != PrAppEventKind_Notification)
        return false;
    // A click on a notification brings its tab forward.
    if (TerminalTab* tab = tabWithId(app, event.tag)) {
        prKitTabsSelect(tab->window->tabs, event.tag);
        prTerminalFocus(tab->terminal);
        if (tab->window->appWindow != nullptr)
            prAppWindowFocus(tab->window->appWindow);
    }
    return true;
}

} // namespace terminal
