// Scribe's document and its interface: a file in a windowed view, its commands, and what a round shows.

#pragma once

#include "pr/app.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/kit.h"
#include "pr/layout_style.h"
#include "pr/text_edit.h"
#include "pr/text_view.h"
#include "pr/ui.h"

#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// How each line of the document takes its paragraph direction.
enum {
    Direction_Ltr,
    Direction_Rtl,
    Direction_Auto,     // from the first strong character of the line, so from its script
};
typedef uint8_t Direction;

// A file being edited, and the editor around it.
struct Scribe {
    PrKit* PR_NULLABLE kit = nullptr;
    PrUi* PR_NULLABLE ui = nullptr;
    PrApp* PR_NULLABLE app = nullptr;

    std::string path;               // empty for a new document
    bool crlf = false;              // the file ended its lines with CR LF, which saving restores
    uint64_t savedVersion = 0;      // the version of the text that the file holds
    PrTextEditBuffer* PR_NULLABLE buffer = nullptr;
    PrTextEditHistory* PR_NULLABLE history = nullptr;
    PrTextView* PR_NULLABLE view = nullptr;
    PrTextFont* PR_NULLABLE font = nullptr;     // what the text is set in

    bool wrap = true;
    Direction direction = Direction_Ltr;
    PrLayoutWritingMode writingMode = PrLayoutWritingMode_HorizontalTb;
    float menuHeight = 0;           // of the menu bar, at the last layout
    PrLayoutNode menuBar = kPrLayoutNodeNone;
    PrLayoutNode editor = kPrLayoutNodeNone;    // the node that shows the rows, in the last round
    PrCommandRegistry* PR_NULLABLE commands = nullptr;
    PrCommandKeymap* PR_NULLABLE keymap = nullptr;
    std::string status;             // what the last command had to say, until the next

    bool paletteRequested = false;  // a command asked to open the palette in the next round
    bool paletteOpen = false;       // the palette was open in the last round, and has focus
    bool menuOpen = false;          // a menu was open in the last round
    std::string paletteQuery;
};

// Reads the file at `path` into the document, or starts an empty one if `path` is empty. Returns false if the file
// cannot be read, or is past what a buffer holds.
bool openDocument(Scribe& s, const std::string& path);

// Starts a document that holds `text`, which belongs to no file.
void openText(Scribe& s, PrStringView text);

// Frees the document and the commands.
void closeDocument(Scribe& s);

// Registers the editor's commands and their key bindings.
void registerCommands(Scribe& s);

// Declares a round of the editor under `root`. Whole-tree keys (prUiKey with no base) name the parts that a driver of
// the interface finds: "scribe.menu.<title>" for an item of the menu bar, "scribe.menu.<title>.list" for its open menu,
// under which each command's item has the key of its id (prCommandMenuItem), and "scribe.scrollbar" for the track.
void buildEditor(Scribe& s, PrLayoutNode root, PrVec2 extent);

// Gives the view the layout of the frame. Returns true if another frame would show something different.
bool afterLayout(Scribe& s);

} // namespace scribe

PR_ASSUME_NONNULL_END
