// Scribe's interface: Studio's text editor in a window, with a menu bar, a command palette and a status line.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/kit.h"
#include "pr/process.h"
#include "pr/string.h"
#include "pr/syntax.h"
#include "pr/text_editor.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

using namespace pr;

// A menu of the menu bar: the ids of its commands in order, with an empty id for a separator.
struct Menu {
    PrStringView title;
    Array<PrStringView> items;
};

// The editor and what Scribe puts around it.
struct Scribe {
    PrKit* PR_NULLABLE kit = nullptr;
    PrUi* PR_NULLABLE ui = nullptr;
    PrApp* PR_NULLABLE app = nullptr;
    PrTextFont* PR_NULLABLE font = nullptr;    // what the text is set in

    PrSyntaxRegistry* PR_NULLABLE languages = nullptr;
    PrCommandRegistry* PR_NULLABLE commands = nullptr;
    PrCommandKeymap* PR_NULLABLE keymap = nullptr;
    PrProcessLauncher* PR_NULLABLE launcher = nullptr;     // of language servers and debug adapters
    PrTextEditor* PR_NULLABLE editor = nullptr;

    // The menus of the menu bar, which openEditor sets to Scribe's, and an application may change.
    Array<Menu> menus;
    float menuHeight = 0;           // of the menu bar, at the last layout
    PrLayoutNode menuBar = kPrLayoutNodeNone;
    bool paletteRequested = false;    // a command asked to open the palette in the next round
    bool paletteOpen = false;         // the palette was open in the last round, and has focus
    bool menuOpen = false;            // a menu was open in the last round
    String paletteQuery;
};

// Makes the editor with Scribe's languages, its language servers and its commands, over the kit, the ui and the font,
// which the caller sets first.
void openEditor(Scribe& s);

// Opens the file at `path` as a document, which streams in over the frames that follow, or an empty document of no file
// if `path` is empty. Returns false if the file cannot be read, or is past what a buffer holds.
bool openDocument(Scribe& s, PrStringView path);

// Makes lldb-dap the editor's debugger, launching the program at `program`.
void setDebuggee(Scribe& s, PrStringView program);

// Opens the files at `oldPath` and `newPath`, and shows their comparison. Returns false if either cannot be read.
bool openDiff(Scribe& s, PrStringView oldPath, PrStringView newPath);

// Opens a document of `text` that belongs to no file and streams in as a file does. `name` is a file name that chooses
// its language, or empty for none.
void openText(Scribe& s, PrStringView text, PrStringView name);

// Frees the editor, its documents and the commands.
void closeEditor(Scribe& s);

// Declares a round of Scribe under `root`.
void buildScribe(Scribe& s, PrLayoutNode root, PrVec2 extent);

// Gives the editor the layout of the frame. Returns true if another frame would show something different, as while a
// document loads.
bool afterLayout(Scribe& s);

} // namespace scribe

PR_ASSUME_NONNULL_END
