// The terminal app's interface: terminals in tabs in one window. Each terminal runs the user's shell, or the program
// given on the command line.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/color.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/process.h"
#include "pr/string.h"
#include "pr/terminal.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace terminal {

using namespace pr;

// The terminals, and the window, commands and tab strip around them.
struct TerminalApp {
    PrKit* PR_NULLABLE kit = nullptr;
    PrUi* PR_NULLABLE ui = nullptr;
    PrApp* PR_NULLABLE app = nullptr;
    PrAppWindow* PR_NULLABLE window = nullptr;
    PrFontSet* PR_NULLABLE fonts = nullptr;    // provides the terminals' monospaced faces

    PrProcessLauncher* PR_NULLABLE launcher = nullptr;
    PrCommandRegistry* PR_NULLABLE commands = nullptr;
    PrCommandKeymap* PR_NULLABLE keymap = nullptr;

    // The program that a new terminal runs, and its arguments. If `program` is empty, a terminal runs the user's shell.
    String program;
    Array<String> arguments;

    float zoom = 1;                                     // scale factor of the terminals' font size
    uint32_t scrollback = kPrTerminalScrollbackDefault; // lines of scrollback that each terminal keeps

    // The columns and rows to size the window for, once a frame knows the cell size. Zero keeps the window's extent. It
    // is reset to zero once the window is resized.
    PrUVec2 grid{};

    // The terminals in tab order, and the index of the one that shows.
    Array<PrTerminal*> terminals;
    uint32_t shown = 0;
    uint64_t nextTag = 1;                      // unique notification tag for the next terminal

    PrLayoutNode strip = kPrLayoutNodeNone;    // the tab strip, present only if there is more than one terminal
    float stripHeight = 0;                     // height of the tab strip at the last layout, in px
    String title;                              // the window's title, as last set
};

// Creates the app's commands, keymap and process launcher. The caller sets the kit, the ui and the fonts first.
void openApp(TerminalApp& t);

// Starts a terminal in a new last tab, shows it and gives it focus. It starts in the working directory of the terminal
// that showed before. Returns the status of prTerminalCreate.
PrStatus openTab(TerminalApp& t);

// Closes the terminal at `index`. If it was showing, the next terminal shows, or the previous one if it was last.
void closeTab(TerminalApp& t, uint32_t index);

// Frees the terminals, the launcher and the commands.
void closeApp(TerminalApp& t);

// Declares a round of the app under `root`, which is `extent` px. It declares the tab strip and the terminal that
// shows. The root takes that terminal's background, so the window clears to it, and the tab strip paints its own
// surface on top. The other terminals are updated with prTerminalUpdate.
//
// If `grid` is set, the window is resized for it once the terminal knows its cell size. A terminal closes once its
// program has exited and all of its output has been read. The app stops when no terminals remain.
void buildApp(TerminalApp& t, PrLayoutNode root, PrVec2 extent);

// Measures the tab strip after layout. Returns true if its height changed, so another frame would show something
// different.
bool afterLayout(TerminalApp& t);

// Handles the app's own window events. A click on a terminal's notification shows that terminal's tab and gives it
// focus. Returns true if the app handled the event.
bool handleEvent(TerminalApp& t, const PrAppEvent& event);

} // namespace terminal

PR_ASSUME_NONNULL_END
