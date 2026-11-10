// The terminal app's interface: terminals in tabs, in as many windows as the user opens. Each terminal runs the user's
// shell, or the program given on the command line.
//
// The app keeps its tabs in no order, and each tab names the window that shows it. Every window is a Studio window of
// its own (pr/studio_window.h), with its own kit and commands, and only draws its tabs. Its tab strip holds the tabs of
// the window, in their order, and the one that shows (PrKitTabs). The app does its work on the terminals as it wakes,
// whether or not a window draws them: it services every terminal, closes the tabs whose programs ended, and closes the
// windows left without tabs. No window is the main one: the app quits when its last window closes, as its windows'
// Studio application does.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/color.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_choice.h"
#include "pr/process.h"
#include "pr/string.h"
#include "pr/studio_app.h"
#include "pr/terminal.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace terminal {

using namespace pr;

struct TerminalWindow;

// The group of the windows' tab strips, which exchange tabs, and the kind of a dragged tab's payload.
constexpr uint64_t kTabGroup = 0x7465726d696e616cull;

// A tab: its terminal, and the window that shows the tab.
struct TerminalTab {
    PrTerminal* terminal;
    TerminalWindow* window;
};

// The app: the program that a new terminal runs, the terminals' fonts and scrollback, the tabs and the windows.
struct TerminalApp {
    // The Studio application that makes the windows, or null for an app whose windows are made by hand, as in tests.
    PrStudioApp* PR_NULLABLE studio = nullptr;
    PrFontSet* PR_NULLABLE fonts = nullptr;    // provides the terminals' monospaced faces
    PrProcessLauncher* PR_NULLABLE launcher = nullptr;

    // The program that a new terminal runs, and its arguments. If `program` is empty, a terminal runs the user's shell.
    String program;
    Array<String> arguments;

    float zoom = 1;                                     // scale factor of the terminals' font size
    uint32_t scrollback = kPrTerminalScrollbackDefault; // lines of scrollback that each terminal keeps
    uint64_t nextTag = 1;                               // unique notification tag for the next terminal

    // The columns and rows to size the next window that opens for, once a frame knows the cell size, or zero to keep
    // its extent. The window takes it, and it is reset to zero.
    PrUVec2 grid{};

    // The tabs of every window, in no order, and the windows in the order they opened.
    Array<TerminalTab> tabs;
    Array<TerminalWindow*> windows;
};

// A window: the commands and the tab strip around the tab that shows.
struct TerminalWindow {
    TerminalApp* PR_NULLABLE app = nullptr;

    // The window's Studio window, or null for one made by hand, and what it draws with.
    PrStudioWindow* PR_NULLABLE window = nullptr;
    PrKit* PR_NULLABLE kit = nullptr;
    PrUi* PR_NULLABLE ui = nullptr;
    PrAppWindow* PR_NULLABLE appWindow = nullptr;

    PrCommandRegistry* PR_NULLABLE commands = nullptr;
    PrCommandKeymap* PR_NULLABLE keymap = nullptr;

    PrKitTabs* PR_NULLABLE tabs = nullptr;    // the window's tabs, by their terminals' notification tags

    // The columns and rows to size the window for, once a frame knows the cell size. Zero keeps the window's extent. It
    // is reset to zero once the window is resized.
    PrUVec2 grid{};

    PrLayoutNode strip = kPrLayoutNodeNone;    // the tab strip of the last round

    String title;                              // the window's title, as last set
};

// Creates the app's process launcher. The caller sets the fonts first.
void openApp(TerminalApp& app);

// Frees the launcher. The windows are closed first.
void closeApp(TerminalApp& app);

// Opens a Studio window of the app at `position`, or where the platform places a window for null, and returns it, or
// returns null if the platform opens none. The window has no tabs until the caller gives it one (openTab, moveTab). It
// frees itself as it closes.
TerminalWindow* PR_NULLABLE newWindow(TerminalApp& app, const PrAppDisplayPosition* PR_NULLABLE position);

// Starts the first tab of a window that newWindow opened. If it cannot start, tells the user why and closes the window.
// Returns true if it started.
bool startWindow(TerminalWindow& w);

// Adds a window to the app and creates its commands and its tabs' strip state. The caller sets the window's kit, ui and
// platform window first.
void openWindow(TerminalApp& app, TerminalWindow& w);

// Closes the window's tabs, frees its commands and its tabs' strip state, and removes it from its app.
void closeWindow(TerminalWindow& w);

// Starts a terminal in a new last tab of the window, shows it and gives it focus. It starts in the working directory of
// the terminal that showed before. Returns the status of prTerminalCreate.
PrStatus openTab(TerminalWindow& w);

// Returns the terminals of the window's tabs, in the strip's order.
Array<PrTerminal*> tabsOf(TerminalWindow& w);

// Closes the tab of `terminal`. If it showed, the tab after it shows and takes focus, or the last tab. A window left
// without tabs closes.
void closeTab(TerminalApp& app, PrTerminal* terminal);

// Moves the tab of `terminal` to `index` in the strip of the window `to`, or last if `index` is past the last tab. It
// shows there and takes focus. The window that it leaves shows another tab as closeTab does, or closes.
void moveTab(TerminalApp& app, PrTerminal* terminal, TerminalWindow& to, uint32_t index);

// Does the app's work on its terminals, as it wakes: services every terminal, closes the tabs whose programs ended and
// all of whose output was read, and closes the windows left without tabs.
void serviceTabs(TerminalApp& app);

// Declares a round of the window under `root`, which is `extent` px: the tab strip, and the terminal of the tab that
// shows, which fills what the strip leaves. The root takes that terminal's background, so the window clears to it, and
// the tab strip paints its own surface on top. The strip takes no room while the window has one tab. The round applies
// the user's moves of tabs between windows and the tabs closed in the strip. If `grid` is set, the window is resized
// for it once the terminal knows its cell size.
void buildWindow(TerminalWindow& w, PrLayoutNode root, PrVec2 extent);

// Handles the app's events. A wake services the tabs (serviceTabs). A click on a terminal's notification shows that
// terminal's tab, gives it focus, and brings its window to the front. Returns true if the app handled the event.
bool handleEvent(TerminalApp& app, const PrAppEvent& event);

} // namespace terminal

PR_ASSUME_NONNULL_END
