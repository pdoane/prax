// The terminal app's interface: its commands, its tabs, and the terminal that shows.

#include "apps/terminal/terminal_app.h"

#include "pr/command_kit.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"

namespace terminal {

using namespace pr;

namespace {

TerminalApp& appOf(void* context) {
    return *static_cast<TerminalApp*>(context);
}

// The chords of the app's commands. On a Mac they use Command, which a terminal leaves to the app. Elsewhere they use
// Control with Shift, because the program receives Control chords.
#if defined(__APPLE__)
constexpr const char* kNewTab = "Cmd T";
constexpr const char* kCloseTab = "Cmd W";
constexpr const char* kQuit = "Cmd Q";
#else
constexpr const char* kNewTab = "Ctrl Shift T";
constexpr const char* kCloseTab = "Ctrl Shift W";
constexpr const char* kQuit = "";
#endif

// The font zoom levels that zooming in and out steps through, as in a browser.
constexpr float kZooms[] = {0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4};

// The size of the terminals' font at a zoom of 1, in px.
constexpr float kFontSize = 13;

// The monospaced family that the terminals are set in.
const PrFontFamily kMono{.generic = PrFontGeneric_Monospace};

// Binds `chord` to the command `id`, if `chord` is not empty. The binding is an override, because the terminal binds
// every chord that it can send to its program.
void bindChord(TerminalApp& t, const char* id, const char* chord) {
    PrCommandChord parsed{};
    if (chord[0] != 0 && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(t.keymap, parsed, id, "", PrCommandBindingKind_Override);
}

// Adds a command, bound to `chord` if it is not empty (bindChord).
void addCommand(TerminalApp& t, const char* id, const char* title, const char* chord, PrCommandRunFn run) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Terminal", .run = run, .context = &t};
    prCommandRegistryAdd(t.commands, &desc);
    bindChord(t, id, chord);
}

// The terminals' font, at the app's zoom.
PrTextFontDesc fontOf(const TerminalApp& t) {
    PrFontStyle style = prFontStyleDefault();
    style.size = kFontSize * t.zoom;
    return PrTextFontDesc{.set = t.fonts, .families = &kMono, .familyCount = 1, .style = style};
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
void setZoom(TerminalApp& t, float zoom) {
    if (zoom == t.zoom)
        return;
    t.zoom = zoom;
    PrTextFontDesc font = fontOf(t);
    for (PrTerminal* terminal : t.terminals)
        prTerminalSetFont(terminal, &font);
}

// Shows the terminal `step` after the one that shows, around the tabs.
void cycle(TerminalApp& t, int32_t step) {
    uint32_t count = t.terminals.count();
    if (count == 0)
        return;
    t.shown = uint32_t((int64_t(t.shown) + step + count) % count);
    prTerminalFocus(t.terminals[t.shown]);
}

// Returns true if the terminal's program has exited and all of its output has been read.
bool hasEnded(PrTerminal* terminal) {
    PrProcessInfo info = prTerminalProcessInfo(terminal);
    return info.exited && info.outputEnded;
}

// Declares the tab strip. Returns the index of the tab whose close control was clicked, or the number of terminals if
// none was.
uint32_t buildTabs(TerminalApp& t, PrLayoutNode root) {
    uint32_t closed = t.terminals.count();
    t.strip = prKitTabStrip(t.kit, root, {}, {});
    for (uint32_t i = 0; i < t.terminals.count(); ++i) {
        PrTerminal* terminal = t.terminals[i];
        PrUiKey key = prUiKeyIndex(t.ui, t.strip, uint64_t(uintptr_t(terminal)));
        PrLayoutNode tab = prKitTab(t.kit, t.strip, {.key = key, .selected = i == t.shown});
        prKitTabLabel(t.kit, tab, {.text = prTerminalTitle(terminal)});
        // A badge marks a tab whose program rang the bell or posted a notification that the user has not seen. A
        // notification takes precedence over a bell. The badge has its own key, so the close control keeps its place
        // among the tab's parts.
        PrTerminalAlertFlags alerts = prTerminalAlerts(terminal);
        if (alerts != 0) {
            bool notified = (alerts & PrTerminalAlertFlag_Notification) != 0;
            PrKitBadge badge{.key = prUiKey(t.ui, tab, "alert"), .text = notified ? "new" : "bell"};
            PrKitBadgeVariant variant = notified ? PrKitBadgeVariant_Accent : PrKitBadgeVariant_Warning;
            prKitBadge(t.kit, tab, badge, {.variant = variant});
        }
        if (prKitActivated(t.kit, prKitTabClose(t.kit, tab))) {
            closed = i;
        } else if (prKitActivated(t.kit, tab) && i != t.shown) {
            t.shown = i;
            prTerminalFocus(terminal);
        }
    }
    return closed;
}

} // namespace

void openApp(TerminalApp& t) {
    t.commands = prCommandRegistryCreate();
    t.keymap = prCommandKeymapCreate();
    t.launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    addCommand(t, "tab.new", "New Tab", kNewTab, [](void* c, const PrCommandArgs*) { openTab(appOf(c)); });
    addCommand(t, "tab.close", "Close Tab", kCloseTab, [](void* c, const PrCommandArgs*) {
        TerminalApp& t = appOf(c);
        if (!t.terminals.isEmpty())
            closeTab(t, t.shown);
    });
    addCommand(t, "tab.next", "Next Tab", "Ctrl Tab", [](void* c, const PrCommandArgs*) { cycle(appOf(c), 1); });
    addCommand(t, "tab.previous", "Previous Tab", "Ctrl Shift Tab", [](void* c, const PrCommandArgs*) {
        cycle(appOf(c), -1);
    });
    addCommand(t, "app.quit", "Quit", kQuit, [](void* c, const PrCommandArgs*) {
        if (appOf(c).app != nullptr)
            prAppStop(appOf(c).app);
    });
    // The browser's zoom chords, with the primary modifier: Command on a Mac and Control elsewhere. Elsewhere this
    // takes Control with minus away from the program, as the platforms' own terminals do.
    addCommand(t, "view.zoomIn", "Zoom In", "Cmd =", [](void* c, const PrCommandArgs*) {
        setZoom(appOf(c), zoomStep(appOf(c).zoom, 1));
    });
    bindChord(t, "view.zoomIn", "Cmd Shift =");
    bindChord(t, "view.zoomIn", "Cmd Shift +");
    addCommand(t, "view.zoomOut", "Zoom Out", "Cmd -", [](void* c, const PrCommandArgs*) {
        setZoom(appOf(c), zoomStep(appOf(c).zoom, -1));
    });
    addCommand(t, "view.zoomReset", "Actual Size", "Cmd 0", [](void* c, const PrCommandArgs*) {
        setZoom(appOf(c), 1);
    });
}

PrStatus openTab(TerminalApp& t) {
    PR_ASSERT(t.kit != nullptr && t.fonts != nullptr);
    Array<PrStringView> arguments;
    for (const String& argument : t.arguments)
        arguments.push(argument);
    // A new tab starts in the working directory of the tab that shows.
    PrRawString directory = prRawStringNull();
    if (t.shown < t.terminals.count())
        directory = prTerminalWorkingDirectory(t.terminals[t.shown]);
    PrTerminalDesc desc{
        .kit = t.kit,
        .font = fontOf(t),
        .app = t.app,
        .launcher = t.launcher,
        .program = t.program,
        .arguments = arguments.isEmpty() ? nullptr : &arguments[0],
        .argumentCount = arguments.count(),
        .directory = prRawStringView(&directory),
        .scrollback = t.scrollback,
        .notificationTag = t.nextTag,
    };
    PrTerminal* terminal = nullptr;
    PrStatus status = prTerminalCreate(&desc, &terminal);
    prRawStringReset(&directory);
    if (status != PrStatus_Ok)
        return status;
    ++t.nextTag;
    t.terminals.push(terminal);
    t.shown = t.terminals.count() - 1;
    prTerminalFocus(terminal);
    return PrStatus_Ok;
}

void closeTab(TerminalApp& t, uint32_t index) {
    PR_ASSERT(index < t.terminals.count());
    prTerminalDestroy(t.terminals[index]);
    t.terminals.erase(index);
    if (t.terminals.isEmpty()) {
        t.shown = 0;
        return;
    }
    if (t.shown > index || t.shown >= t.terminals.count())
        t.shown = t.shown > 0 ? t.shown - 1 : 0;
    prTerminalFocus(t.terminals[t.shown]);
}

void closeApp(TerminalApp& t) {
    for (PrTerminal* terminal : t.terminals)
        prTerminalDestroy(terminal);
    t.terminals.clear();
    prProcessLauncherDestroy(t.launcher);
    prCommandKeymapDestroy(t.keymap);
    prCommandRegistryDestroy(t.commands);
}

void buildApp(TerminalApp& t, PrLayoutNode root, PrVec2 extent) {
    prCommandScope(t.ui, root, t.commands, t.keymap, "");
    // The terminals' programs see the color scheme that the window's theme follows.
    PrAppColorScheme scheme = prUiUserPreferences(t.ui).colorScheme;
    for (PrTerminal* terminal : t.terminals)
        prTerminalSetColorScheme(terminal, scheme);
    // Hidden terminals still read their programs' output, so their titles and exits stay current.
    for (uint32_t i = 0; i < t.terminals.count(); ++i) {
        if (i != t.shown)
            prTerminalUpdate(t.terminals[i]);
    }
    for (uint32_t i = t.terminals.count(); i-- > 0;) {
        if (hasEnded(t.terminals[i]))
            closeTab(t, i);
    }
    if (t.terminals.isEmpty()) {
        if (t.app != nullptr)
            prAppStop(t.app);
        return;
    }

    float height = extent.y;
    t.strip = kPrLayoutNodeNone;
    if (t.terminals.count() > 1) {
        uint32_t closed = buildTabs(t, root);
        height -= t.stripHeight;
        if (closed < t.terminals.count()) {
            // The closed tab disappears in the next round, which lays out the strip without it.
            closeTab(t, closed);
            prUiRequestFrame(t.ui);
        }
    }
    PrTerminal* shown = t.terminals[t.shown];
    prTerminalFrame(shown, root, PrVec2{extent.x, height > 0 ? height : 0}, PrTerminalBackground_Parent);
    PrVec2 cell = prTerminalCellExtent(shown);
    if (t.grid.x > 0 && t.grid.y > 0 && cell.x > 0 && cell.y > 0 && t.app != nullptr) {
        // The window is sized to fit the grid in whole cells from the screen's origin. The tab strip keeps its height.
        PrVec2 origin = prTerminalScreenOrigin(shown);
        float windowWidth = origin.x + float(t.grid.x) * cell.x;
        float windowHeight = extent.y - height + origin.y + float(t.grid.y) * cell.y;
        prAppSetExtent(t.app, PrUVec2{uint32_t(prF32Ceil(windowWidth)), uint32_t(prF32Ceil(windowHeight))});
        t.grid = PrUVec2{};
    }

    // The window's title is the title of the terminal that shows.
    PrStringView title = prTerminalTitle(shown);
    if (t.app != nullptr && title != PrStringView(t.title)) {
        t.title = title;
        prAppSetTitle(t.app, title);
    }
}

bool afterLayout(TerminalApp& t) {
    float height = 0;
    if (t.strip != kPrLayoutNodeNone) {
        PrRect bar = prLayoutRectToPx(prLayoutNodeBounds(prUiTree(t.ui), t.strip, PrLayoutCoordinates_Viewport));
        height = bar.bottom - bar.top;
    }
    if (height == t.stripHeight)
        return false;
    t.stripHeight = height;
    return true;
}

bool handleEvent(TerminalApp& t, const PrAppEvent& event) {
    if (event.kind != PrAppEventKind_Notification)
        return false;
    for (uint32_t i = 0; i < t.terminals.count(); ++i) {
        if (prTerminalNotificationTag(t.terminals[i]) != event.tag)
            continue;
        t.shown = i;
        prTerminalFocus(t.terminals[i]);
        break;
    }
    return true;
}

} // namespace terminal
