// Studio's text editor: the code editor of tools, over files in windowed views.
//
// The editor makes the choices of a code editor for an application: how documents open, load and save, the style
// classes that one theme colors every language's syntax by, a gutter of line numbers and markers, the commands and
// their default key bindings, and the language servers of its languages, whose diagnostics, hovers and definitions it
// shows. An application gives it the kit that it draws with and the command registry that its menus and palette run,
// and declares it in a round as one part of its window.
//
// One thread uses an editor and its documents. The processes of language servers and debug adapters wake the
// application (prAppWake) when they have something to read, and the editor reads it in its next frame.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/kit.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/process.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/syntax.h"
#include "pr/text_diff.h"
#include "pr/text_edit.h"
#include "pr/text_font.h"
#include "pr/text_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Editors

// The documents of an application's editor, the one of them that shows, and the language servers that they use.
typedef struct PrTextEditor PrTextEditor;

// What an editor draws with and registers into.
typedef struct PrTextEditorDesc {
    PrKit* kit;
    PrTextFont* font;                           // what text is set in

    // The application, which gives the clipboard and wakes for language servers, or null for none.
    PrApp* PR_NULLABLE app;

    // What runs language servers and debug adapters, which outlives the editor, or null to run none, so that they fail
    // to start.
    PrProcessLauncher* PR_NULLABLE launcher;

    // The languages that color documents, which outlive the editor, or null for none.
    PrSyntaxRegistry* PR_NULLABLE languages;

    // Where the editor adds its commands, in the category "Editor", and binds their default chords.
    PrCommandRegistry* commands;
    PrCommandKeymap* keymap;
} PrTextEditorDesc;

// Creates an editor with no documents, or frees one and its documents. Freeing an editor asks its language servers to
// exit, and ends those that have not after a short wait.
PrTextEditor* prTextEditorCreate(const PrTextEditorDesc* desc);
void prTextEditorDestroy(PrTextEditor* PR_NULLABLE editor);

// How each line of a document takes its paragraph direction.
enum {
    PrTextEditorDirection_Ltr,
    PrTextEditorDirection_Rtl,
    PrTextEditorDirection_Auto,                 // from the first strong character of the line, so from its script
};
typedef uint8_t PrTextEditorDirection;

// How the editor shows text, which its commands change.
typedef struct PrTextEditorOptions {
    bool wrap;
    bool lineNumbers;
    PrTextEditorDirection direction;
    PrLayoutWritingMode writingMode;
} PrTextEditorOptions;

// Wrapped lines with numbers, left to right and horizontal.
PrTextEditorOptions prTextEditorOptionsDefault(void);

// The editor's options.
PrTextEditorOptions prTextEditorOptions(const PrTextEditor* editor);
void prTextEditorSetOptions(PrTextEditor* editor, const PrTextEditorOptions* options);

// A language server, which serves the documents of `languages`, by their names in the syntax registry, such as "cpp".
// The editor starts one process of the program for each workspace that a document of those languages opens in: the
// nearest folder above the file that holds .git, or else the file's own folder. The protocol's language id of a
// document is its language's name.
typedef struct PrTextEditorServerDesc {
    const PrStringView* languages;
    uint32_t languageCount;
    PrStringView program;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;
} PrTextEditorServerDesc;

// Adds a language server, which the editor copies. A server added later wins a language that an earlier one serves.
void prTextEditorAddServer(PrTextEditor* editor, const PrTextEditorServerDesc* desc);

// Declares the editor in a round under `parent`, in `extent` px: the shown document's gutter, text, scrollbar and
// hover. Reads what language servers sent, loads more of each document that streams in, and parses what changed.
void prTextEditorFrame(PrTextEditor* editor, PrLayoutNode parent, PrVec2 extent);

// Gives the shown document the layout of the frame, and improves the estimates of the heights of its lines for the time
// that a frame can spare. Returns true if another frame would show something different, as while a document loads.
bool prTextEditorLaidOut(PrTextEditor* editor);

// The element that shows the text of the shown document, in the last frame, or kPrLayoutNodeNone. It takes focus.
PrLayoutNode prTextEditorTextNode(const PrTextEditor* editor);

// What the last command had to say, until the next, or what the diagnostic at the caret says, or empty. The view is
// valid until the next frame.
PrStringView prTextEditorStatus(const PrTextEditor* editor);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// A text that the editor holds: a file's, or a text of no file.
typedef struct PrTextEditorDocument PrTextEditorDocument;

// What a document holds. A document of a file streams in over the frames that follow, and its language is its file's. A
// document of `text` streams in as a file does, and `name`, a file name, chooses its language, or empty for none.
typedef struct PrTextEditorDocumentDesc {
    PrStringView path;                          // the file, or the null view for a text of no file
    PrStringView text;
    PrStringView name;
    bool readOnly;                              // the document refuses its user's edits
} PrTextEditorDocumentDesc;

// Creates a document, which shows if the editor shows none, and writes it to `*out`. Returns NotFound if the file
// cannot be read, and Limit if it is past what a buffer holds. On failure, `*out` is null. Destroying a document shows
// another if it showed.
PrStatus prTextEditorDocumentCreate(PrTextEditor* editor, const PrTextEditorDocumentDesc* desc,
    PrTextEditorDocument* PR_NULLABLE* PR_NONNULL out);
void prTextEditorDocumentDestroy(PrTextEditorDocument* PR_NULLABLE document);

// The documents of the editor, in the order of their creation.
uint32_t prTextEditorDocumentCount(const PrTextEditor* editor);
PrTextEditorDocument* prTextEditorDocument(const PrTextEditor* editor, uint32_t index);

// Returns the document of the file at `path`, or null.
PrTextEditorDocument* PR_NULLABLE prTextEditorDocumentFromPath(const PrTextEditor* editor, PrStringView path);

// The document that shows, or null if the editor has none. Showing a document gives its text focus.
PrTextEditorDocument* PR_NULLABLE prTextEditorShown(const PrTextEditor* editor);
void prTextEditorShow(PrTextEditor* editor, PrTextEditorDocument* document);

// The parts of a document. Its view's selections are the document's own, and its history is its local edits'.
PrTextEditBuffer* prTextEditorDocumentBuffer(const PrTextEditorDocument* document);
PrTextView* prTextEditorDocumentView(const PrTextEditorDocument* document);
PrTextEditHistory* prTextEditorDocumentHistory(const PrTextEditorDocument* document);

// The document's file, or the null view for a text of no file.
PrStringView prTextEditorDocumentPath(const PrTextEditorDocument* document);

// The document's language, or null.
const PrSyntaxLanguage* PR_NULLABLE prTextEditorDocumentLanguage(const PrTextEditorDocument* document);

// Where a document's language server stands.
enum {
    PrTextEditorServerState_None,               // no server serves the document's language
    PrTextEditorServerState_Starting,
    PrTextEditorServerState_Running,
    PrTextEditorServerState_Failed,             // the program could not start, or it exited, or broke the protocol
};
typedef uint8_t PrTextEditorServerState;

// What a status line says of a document.
typedef struct PrTextEditorDocumentInfo {
    bool loading;
    float loaded;                               // the part of the text that the buffer holds, from 0 to 1
    bool edited;                                // the text differs from what its file holds
    bool crlf;                                  // the file ends its lines with CR LF, which saving restores
    bool parsing;                               // a parse of its syntax has not finished
    PrTextEditorServerState server;
    uint32_t errors;
    uint32_t warnings;
} PrTextEditorDocumentInfo;

// Returns what a status line says of the document.
PrTextEditorDocumentInfo prTextEditorDocumentInfo(const PrTextEditorDocument* document);

// Writes the document to its file, with the line endings that the file had. Returns InvalidState if it has no file or
// is still loading, and Io if the file cannot be written.
PrStatus prTextEditorDocumentSave(PrTextEditorDocument* document);

// ---------------------------------------------------------------------------------------------------------------------
// Diffs

// A comparison of two documents, shown side by side: the old on the left and the new on the right, each line beside its
// counterpart, with space where one side has lines that the other does not, and what changed tinted, the lines and the
// bytes within them. The sides scroll together, and lines do not wrap. The comparison follows both documents' edits,
// and the new document takes the editor's commands while the comparison shows.
typedef struct PrTextEditorDiff PrTextEditorDiff;

// Creates a comparison of `old` with `young`, documents of the editor that outlive it, and shows it, or frees one. A
// comparison that shows when it is freed shows its new document.
PrTextEditorDiff* prTextEditorDiffCreate(PrTextEditor* editor, PrTextEditorDocument* old, PrTextEditorDocument* young);
void prTextEditorDiffDestroy(PrTextEditorDiff* PR_NULLABLE diff);

// The comparison that shows, or null if a document shows alone. Showing a document ends the showing of a comparison.
PrTextEditorDiff* PR_NULLABLE prTextEditorShownDiff(const PrTextEditor* editor);
void prTextEditorShowDiff(PrTextEditor* editor, PrTextEditorDiff* diff);

// The runs of lines that changed, as of the last frame, in the order of the documents (pr/text_diff.h).
uint32_t prTextEditorDiffChangeCount(const PrTextEditorDiff* diff);
PrTextDiffChange prTextEditorDiffChange(const PrTextEditorDiff* diff, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Debugging

// A debug adapter, which the editor runs as a process of `program` for each session (pr/dap.h), and the arguments of
// the session's launch request as JSON text, such as {"program": "/path/to/program"}.
typedef struct PrTextEditorDebuggerDesc {
    PrStringView program;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;
    PrStringView adapterId;                     // the kind of adapter, as the protocol names it, such as "lldb-dap"
    PrStringView launch;
} PrTextEditorDebuggerDesc;

// Sets the debugger that the editor's commands start sessions of, which the editor copies.
void prTextEditorSetDebugger(PrTextEditor* editor, const PrTextEditorDebuggerDesc* desc);

// Where the editor's debugging stands.
enum {
    PrTextEditorDebugState_None,                // no session
    PrTextEditorDebugState_Starting,
    PrTextEditorDebugState_Running,
    PrTextEditorDebugState_Stopped,             // at a breakpoint or a step, whose line the editor shows
    PrTextEditorDebugState_Ended,               // the debuggee exited
    PrTextEditorDebugState_Failed,              // the adapter did not start, refused to launch, or broke the protocol
};
typedef uint8_t PrTextEditorDebugState;

// What the debugging shows: its state, and while the debuggee is stopped, the file and line of the frame that it
// stopped in, as the last frame read them.
typedef struct PrTextEditorDebugInfo {
    PrTextEditorDebugState state;
    PrStringView path;                          // empty if the frame has no file, or the debuggee runs
    uint32_t line;
} PrTextEditorDebugInfo;

// Returns what the debugging shows. Its path is valid until the next frame.
PrTextEditorDebugInfo prTextEditorDebugInfo(const PrTextEditor* editor);

// Starts a session of the editor's debugger with the breakpoints of every document of a file, in place of any session
// that has ended. Returns false if a session runs, or the editor has no debugger.
bool prTextEditorDebugStart(PrTextEditor* editor);

// Ends the session, which ends the debuggee.
void prTextEditorDebugStop(PrTextEditor* editor);

// Writes up to `capacity` of the lines of the document's breakpoints to `out`, in order, and returns their number. A
// breakpoint follows its line through edits, and goes with it if the line is removed.
uint32_t prTextEditorDocumentBreakpoints(const PrTextEditorDocument* document, uint32_t* PR_NULLABLE out, uint32_t capacity);

// Sets a breakpoint on `line`, or removes the one there.
void prTextEditorDocumentToggleBreakpoint(PrTextEditorDocument* document, uint32_t line);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
