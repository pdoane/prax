// Scribe's document and its interface: a file in a windowed view, its commands, and what a round shows.

#pragma once

#include "pr/app.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/file.h"
#include "pr/kit.h"
#include "pr/layout_style.h"
#include "pr/syntax.h"
#include "pr/text_edit.h"
#include "pr/text_view.h"
#include "pr/ui.h"

#include <string>
#include <vector>

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

    // The syntax of the document, if its file names a language that the build has.
    PrSyntaxRegistry* PR_NULLABLE languages = nullptr;
    const PrSyntaxLanguage* PR_NULLABLE language = nullptr;
    PrSyntaxTree* PR_NULLABLE syntax = nullptr;
    uint64_t syntaxChanges = 0;            // the number of updates that changed the tree's spans
    std::vector<PrColor16> categoryColors; // by PrSyntaxCategory: the color of its text, or zero alpha for none
    bool darkColors = true;                // the theme that `categoryColors` is of

    // The text that streams into the document as it loads: the file, mapped, or a copy of the text that a caller gave,
    // and how much of it the buffer holds. The document is loading until the buffer holds all of it.
    PrFileMapping mapping{};
    std::string source;
    size_t loaded = 0;
    bool loading = false;
    std::string piece;              // the piece that the last load step appended, with its line endings made line feeds

    // The spans of the last window, which a frame reuses while the text, the window and the syntax stay the same.
    std::vector<PrSyntaxSpan> spans;
    uint64_t spansVersion = UINT64_MAX;
    uint64_t spansChanges = 0;     // the tree's changes that the spans are of
    uint32_t spansStart = 0;
    uint32_t spansEnd = 0;

    bool paletteRequested = false;  // a command asked to open the palette in the next round
    bool paletteOpen = false;       // the palette was open in the last round, and has focus
    bool menuOpen = false;          // a menu was open in the last round
    std::string paletteQuery;
};

// Opens the file at `path` as the document, which streams in over the frames that follow, or starts an empty one if
// `path` is empty. Returns false if the file cannot be read, or is past what a buffer holds.
bool openDocument(Scribe& s, const std::string& path);

// Starts a document that streams in `text` over the frames that follow, as a file does, and that belongs to no file.
// `name` is a file name that chooses its language, or empty for none.
void openText(Scribe& s, PrStringView text, const std::string& name);

// Frees the document and the commands.
void closeDocument(Scribe& s);

// Registers the editor's commands and their key bindings.
void registerCommands(Scribe& s);

// Declares a round of the editor under `root`. Whole-tree keys (prUiKey with no base) name the parts that a driver of
// the interface finds: "scribe.menu.<title>" for an item of the menu bar, "scribe.menu.<title>.list" for its open menu,
// under which each command's item has the key of its id (prCommandMenuItem), and "scribe.scrollbar" for the track.
void buildEditor(Scribe& s, PrLayoutNode root, PrVec2 extent);

// Gives the view the layout of the frame. Returns true if another frame would show something different, as while the
// document loads.
bool afterLayout(Scribe& s);

} // namespace scribe

PR_ASSUME_NONNULL_END
