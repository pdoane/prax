// Studio's text editor (text_editor.h): documents that stream in from files, their syntax and language servers, the
// commands of a code editor, and what a round shows of the document that shows.

#include "pr/text_editor.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/lsp.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/text_edit_command.h"
#include "pr/text_estimate.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/ui_text_view.h"

#include "studio/text_editor/src/text_editor_debug.h"
#include "studio/text_editor/src/text_editor_diff.h"
#include "studio/text_editor/src/text_editor_server.h"
#include "studio/text_editor/src/text_editor_style.h"

namespace pr_impl {

namespace {

// The width of the scrollbar, and the smallest length of its thumb, in px.
constexpr float kScrollbarWidth = 12;
constexpr float kThumbMinLength = 24;

// The height of a row of text, in px.
constexpr float kLineHeight = 19;

// The time that a frame gives to better estimates of the heights of lines outside the window, to the syntax of a text
// that changed, and to loading text, in milliseconds. A parse that runs out of time resumes in the next frame. A piece
// of loading text parses whole, so that no text shows before its syntax does.
constexpr double kRefineMilliseconds = 2;
constexpr double kParseMilliseconds = 4;
constexpr double kLoadMilliseconds = 8;

// The most bytes of a piece of loading text.
constexpr uint32_t kLoadPiece = 64 * 1024;

// The space of the gutter around its numbers, in px, and the fewest digits that it makes room for.
constexpr float kGutterPadding = 20;
constexpr uint32_t kGutterMinDigits = 3;

// The widest that a hover's text sets, in px.
constexpr float kHoverMaxWidth = 560;

// The space between the sides of a comparison, in px.
constexpr float kDiffDivider = 2;

// The height of the panel that a debugging session shows below the text, in px, and the most rows of each of its lists.
constexpr float kDebugPanelHeight = 200;
constexpr uint32_t kDebugPanelRows = 9;

struct TextEditor;

struct TextEditorDocument {
    PrTextEditorDocument* handle;
    TextEditor* editor;
    PrRawString path;               // null for a text of no file
    bool crlf;                      // the file ended its lines with CR LF, which saving restores
    uint64_t savedVersion;          // the version of the text that the file holds
    PrTextEditBuffer* buffer;
    PrTextEditHistory* history;
    PrTextView* view;

    const PrSyntaxLanguage* PR_NULLABLE language;
    PrSyntaxTree* PR_NULLABLE syntax;
    uint64_t syntaxChanges;         // the number of updates that changed the tree's spans

    // The text that streams into the document as it loads: the file, mapped, or a copy of the text that it was given,
    // and how much of it the buffer holds. The document is loading until the buffer holds all of it.
    PrFileMapping mapping;
    RawArray<char> source;
    uint32_t loaded;
    bool loading;
    RawArray<char> piece;              // the piece that the last load step appended, with its line endings made line
                                       // feeds

    // The syntax's spans of the last window, which a frame reuses while the text, the window and the syntax hold.
    RawArray<PrSyntaxSpan> spans;
    uint64_t spansVersion;
    uint64_t spansChanges;
    uint32_t spansStart;
    uint32_t spansEnd;

    // The document as its language server knows it, once it has loaded, and the server.
    TextEditorServer* PR_NULLABLE server;
    PrLspDocument* PR_NULLABLE lsp;

    // A place that a definition named in the document, shown once the buffer holds its line, in the encoding of the
    // server that named it.
    bool revealPending;
    PrLspPosition revealPosition;
    PrLspEncoding revealEncoding;

    bool readOnly;
    bool listed;                    // the editor's list of documents holds it, as it holds every document of its host

    // The breakpoints, as anchors at the starts of their lines, and the lines that the session was last sent.
    RawArray<PrTextEditAnchor> breakpoints;
    RawArray<uint32_t> sentLines;
    bool sent;
};

// A comparison of two documents, as it shows. A comparison with the saved file owns the document of the file's text.
struct TextEditorDiff {
    PrTextEditorDiff* handle;
    TextEditor* editor;
    TextEditorDocument* old;
    TextEditorDocument* young;
    bool ownsOld;
    TextEditorDiffModel model;
    PrLayoutNode oldPort;                     // the element that shows the old side, in the last frame

    // Of each side's scroll at the last layout, along the block axis and along the lines.
    double offsets[TextEditorDiffSide_Count];
    double inlineOffsets[TextEditorDiffSide_Count];
};

// A command of a key that the view carries out.
struct KeyCommand {
    const char* id;
    const char* title;
    const char* chord;
    PrTextEditCommand command;
    PrTextEditSelectionMove kind;
};

#if defined(__APPLE__)
#define TEXT_EDITOR_WORD "Alt"
#else
#define TEXT_EDITOR_WORD "Ctrl"
#endif

constexpr KeyCommand kKeyCommands[] = {
    {"cursor.left", "Move Left", "Left", PrTextEditCommand_Left, PrTextEditSelectionMove_Collapse},
    {"cursor.right", "Move Right", "Right", PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse},
    {"cursor.up", "Move Up", "Up", PrTextEditCommand_Up, PrTextEditSelectionMove_Collapse},
    {"cursor.down", "Move Down", "Down", PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse},
    {"select.left", "Select Left", "Shift Left", PrTextEditCommand_Left, PrTextEditSelectionMove_Extend},
    {"select.right", "Select Right", "Shift Right", PrTextEditCommand_Right, PrTextEditSelectionMove_Extend},
    {"select.up", "Select Up", "Shift Up", PrTextEditCommand_Up, PrTextEditSelectionMove_Extend},
    {"select.down", "Select Down", "Shift Down", PrTextEditCommand_Down, PrTextEditSelectionMove_Extend},
    {
        "cursor.wordLeft", "Move Word Left", TEXT_EDITOR_WORD " Left", PrTextEditCommand_WordLeft,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "cursor.wordRight", "Move Word Right", TEXT_EDITOR_WORD " Right", PrTextEditCommand_WordRight,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "select.wordLeft", "Select Word Left", TEXT_EDITOR_WORD " Shift Left", PrTextEditCommand_WordLeft,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.wordRight", "Select Word Right", TEXT_EDITOR_WORD " Shift Right", PrTextEditCommand_WordRight,
        PrTextEditSelectionMove_Extend,
    },
    {
        "edit.deleteWordBefore", "Delete Word Before", TEXT_EDITOR_WORD " Backspace",
        PrTextEditCommand_DeleteWordBefore, PrTextEditSelectionMove_Collapse,
    },
    {
        "edit.deleteWordAfter", "Delete Word After", TEXT_EDITOR_WORD " Delete", PrTextEditCommand_DeleteWordAfter,
        PrTextEditSelectionMove_Collapse,
    },
#if defined(__APPLE__)
        {
            "cursor.lineLeft", "Move to Line Start", "Cmd Left", PrTextEditCommand_EdgeLeft,
            PrTextEditSelectionMove_Collapse,
        },
    {
        "cursor.lineRight", "Move to Line End", "Cmd Right", PrTextEditCommand_EdgeRight,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "select.lineLeft", "Select to Line Start", "Cmd Shift Left", PrTextEditCommand_EdgeLeft,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.lineRight", "Select to Line End", "Cmd Shift Right", PrTextEditCommand_EdgeRight,
        PrTextEditSelectionMove_Extend,
    },
    {"cursor.textStart", "Move to Text Start", "Cmd Up", PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse},
    {"cursor.textEnd", "Move to Text End", "Cmd Down", PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse},
    {
        "select.textStart", "Select to Text Start", "Cmd Shift Up", PrTextEditCommand_TextStart,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.textEnd", "Select to Text End", "Cmd Shift Down", PrTextEditCommand_TextEnd,
        PrTextEditSelectionMove_Extend,
    },
#else
        {
            "cursor.textStart", "Move to Text Start", "Ctrl Home", PrTextEditCommand_TextStart,
            PrTextEditSelectionMove_Collapse,
        },
    {"cursor.textEnd", "Move to Text End", "Ctrl End", PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse},
    {
        "select.textStart", "Select to Text Start", "Ctrl Shift Home", PrTextEditCommand_TextStart,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.textEnd", "Select to Text End", "Ctrl Shift End", PrTextEditCommand_TextEnd,
        PrTextEditSelectionMove_Extend,
    },
#endif
        {"cursor.home", "Move to Line Start", "Home", PrTextEditCommand_LineStart, PrTextEditSelectionMove_Collapse},
    {"cursor.end", "Move to Line End", "End", PrTextEditCommand_LineEnd, PrTextEditSelectionMove_Collapse},
    {"select.home", "Select to Line Start", "Shift Home", PrTextEditCommand_LineStart, PrTextEditSelectionMove_Extend},
    {"select.end", "Select to Line End", "Shift End", PrTextEditCommand_LineEnd, PrTextEditSelectionMove_Extend},
    {"edit.deleteLeft", "Delete Left", "Backspace", PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse},
    {"edit.deleteRight", "Delete Right", "Delete", PrTextEditCommand_DeleteForward, PrTextEditSelectionMove_Collapse},
    {"edit.newline", "Insert Line Break", "Enter", PrTextEditCommand_InsertLineBreak, PrTextEditSelectionMove_Collapse},
    {"edit.selectAll", "Select All", "Cmd A", PrTextEditCommand_SelectAll, PrTextEditSelectionMove_Collapse},
};

#undef TEXT_EDITOR_WORD

// The commands that choose one of a set of values of the options.
struct DirectionCommand {
    const char* id;
    const char* title;
    PrTextEditorDirection direction;
};

struct WritingModeCommand {
    const char* id;
    const char* title;
    PrLayoutWritingMode mode;
};

constexpr DirectionCommand kDirections[] = {
    {"view.directionLtr", "Left to Right", PrTextEditorDirection_Ltr},
    {"view.directionRtl", "Right to Left", PrTextEditorDirection_Rtl},
    {"view.directionAuto", "Direction from Each Line", PrTextEditorDirection_Auto},
};

constexpr WritingModeCommand kWritingModes[] = {
    {"view.horizontal", "Horizontal Lines", PrLayoutWritingMode_HorizontalTb},
    {"view.verticalRl", "Vertical Lines, Right to Left", PrLayoutWritingMode_VerticalRl},
    {"view.verticalLr", "Vertical Lines, Left to Right", PrLayoutWritingMode_VerticalLr},
};

// A command's context: the editor, and which entry of the command's table.
struct ChoiceContext {
    TextEditor* editor;
    uint32_t index;
};

// The worst severity of the diagnostics on a line.
struct LineSeverity {
    uint32_t line;
    PrLspSeverity severity;
};

// The context of a view's gutter: the editor, and whether its numbers take the colors of the shown document's
// diagnostics.
struct GutterSide {
    TextEditor* editor;
    bool severities;
};

struct TextEditor {
    PrTextEditor* handle;
    PrKit* kit;
    PrUi* ui;
    PrTextFont* font;
    PrApp* PR_NULLABLE app;
    PrProcessLauncher* PR_NULLABLE launcher;
    PrSyntaxRegistry* PR_NULLABLE languages;
    PrCommandRegistry* commands;
    PrCommandKeymap* keymap;
    PrTextEditorOptions options;

    RawArray<TextEditorDocument*> documents;
    TextEditorDocument* PR_NULLABLE shown;
    bool focusAsked;                // the next frame gives the text focus

    // The debugger that the application set, the session, and the stops of the session that the editor has shown.
    bool hasDebugger;
    TextEditorDebugConfig debugger;
    TextEditorDebugSession* PR_NULLABLE debug;
    uint64_t debugStops;
    RawArray<uint32_t> breakpointLines;         // of the shown document, for a frame's gutter
    PrLayoutStyleIndex breakpointStyle;         // the record of a number on a line with a breakpoint

    // The comparisons, and the one that shows beside its new document, which is the one that shows.
    RawArray<TextEditorDiff*> diffs;
    TextEditorDiff* PR_NULLABLE diff;

    RawArray<TextEditorServerConfig> configs;
    RawArray<TextEditorServer*> servers;

    // By PrSyntaxCategory: the color of its text, or zero alpha for none, in the theme that `dark` says.
    RawArray<PrColor16> categoryColors;
    bool dark;

    PrLayoutNode port;              // the element that shows the text, in the last frame
    PrLayoutNode holder;
    PrRawString status;                // what the last command had to say
    PrRawString statusShown;

    // A hover that the server answered, and where: it shows while the caret stays there and the text holds.
    PrLspRequest hoverRequest;
    PrRawString hoverText;
    TextEditorDocument* PR_NULLABLE hoverDocument;
    uint32_t hoverOffset;
    uint64_t hoverVersion;
    bool hoverOpening;              // the popover has not shown yet, and each frame opens it until it does
    PrLspRequest definitionRequest;
    TextEditorDocument* PR_NULLABLE definitionDocument;

    // What a frame's gutter and diagnostics read, made once in each frame.
    PrTextEstimator* PR_NULLABLE estimator;
    PrLayoutStyleIndex numberStyle;
    PrLayoutStyleIndex numberStyles[PrLspSeverity_Hint + 1];   // by PrLspSeverity: a number's record on such a line
    RawArray<LineSeverity> lineSeverities;
    GutterSide gutters[TextEditorDiffSide_Count];               // the gutters' contexts: the old side has no severities

    ChoiceContext keyContexts[countOf(kKeyCommands)];
    ChoiceContext directionContexts[countOf(kDirections)];
    ChoiceContext writingModeContexts[countOf(kWritingModes)];
    ChoiceContext stepContexts[3];
};

TextEditor& editorOf(void* PR_NULLABLE context) {
    return *static_cast<TextEditor*>(context);
}

void setStatus(TextEditor* editor, PrStringView text) {
    prRawStringReplace(&editor->status, text);
}

PrStringView sourceOf(const TextEditorDocument* document) {
    if (document->mapping.data != nullptr)
        return PrStringView{reinterpret_cast<const char*>(document->mapping.data), document->mapping.size};
    return PrStringView{document->source.data, document->source.count};
}

// Returns the folder of `path`: what comes before its last separator, or empty.
PrStringView folderOf(PrStringView path) {
    size_t end = path.size;
    while (end > 0 && path.data[end - 1] != '/' && path.data[end - 1] != '\\')
        --end;
    return PrStringView{path.data, end > 1 ? end - 1 : end};
}

// Returns the workspace of the file at `path`: the nearest folder above it that holds .git, or its own folder. The
// caller releases it with prRawStringReset.
PrRawString workspaceOf(PrStringView path) {
    PrStringView folder = folderOf(path);
    PrRawString probe = prRawStringEmpty();
    for (PrStringView at = folder; at.size != 0;) {
        prRawStringReplace(&probe, at);
        prRawStringAppend(&probe, "/.git");
        PrFileInfo info{};
        if (prFileInfo(prRawStringView(&probe), &info)) {
            prRawStringReset(&probe);
            return prRawStringCopy(at);
        }
        PrStringView up = folderOf(at);
        if (up.size >= at.size)
            break;
        at = up;
    }
    prRawStringReset(&probe);
    return prRawStringCopy(folder);
}

// Returns the config of the server that serves `language`, or -1.
int32_t configOf(const TextEditor* editor, PrStringView language) {
    for (uint32_t i = editor->configs.count; i-- > 0;) {
        const TextEditorServerConfig& config = editor->configs.data[i];
        for (uint32_t k = 0; k < config.languages.count; ++k) {
            if (prRawStringView(&config.languages.data[k]) == language)
                return int32_t(i);
        }
    }
    return -1;
}

// Tells the document's language server of it, starting the server for its workspace if none runs.
void attachServer(TextEditor* editor, TextEditorDocument* document) {
    if (document->lsp != nullptr || document->language == nullptr || prRawStringIsNull(&document->path))
        return;
    int32_t config = configOf(editor, document->language->name);
    if (config < 0)
        return;
    PrRawString root = workspaceOf(prRawStringView(&document->path));
    TextEditorServer* server = nullptr;
    for (uint32_t i = 0; i < editor->servers.count && server == nullptr; ++i) {
        TextEditorServer* running = editor->servers.data[i];
        if (running->config == uint32_t(config) && prRawStringView(&running->root) == prRawStringView(&root))
            server = running;
    }
    if (server == nullptr) {
        server = textEditorServerCreate(editor->configs.data[config], uint32_t(config), prRawStringView(&root),
            editor->launcher, editor->app);
        editor->servers.push(server);
    }
    prRawStringReset(&root);
    document->server = server;
    document->lsp = prLspDocumentCreate(server->client, document->buffer, prRawStringView(&document->path),
        document->language->name);
}

// Ends the load: the file holds what the buffer does, and the source is no longer needed.
void finishLoad(TextEditorDocument* document) {
    document->loading = false;
    document->savedVersion = prTextEditBufferVersion(document->buffer);
    if (document->mapping.data != nullptr)
        prFileUnmap(&document->mapping);
    document->mapping = PrFileMapping{};
    document->source.reset();
    attachServer(document->editor, document);
}

// Appends pieces of the loading text to the end of the buffer for about `milliseconds`, and parses each before the
// next, so that the text and its syntax arrive together.
void loadPieces(TextEditorDocument* document, double milliseconds) {
    if (!document->loading)
        return;
    PrStringView source = sourceOf(document);
    double end = prTimeMonotonicMilliseconds() + milliseconds;
    do {
        uint32_t at = document->loaded;
        uint32_t stop = uint32_t(source.size) - at > kLoadPiece ? at + kLoadPiece : uint32_t(source.size);
        if (stop < source.size) {
            // A piece ends after a line feed where it can, so that lines arrive whole, and otherwise between code
            // points and never between a CR and its LF.
            PrStringView window{source.data + at, stop - at};
            size_t lineFeed = prStringFindLastByte(window, '\n');
            if (lineFeed != window.size) {
                stop = at + uint32_t(lineFeed) + 1;
            } else {
                uint32_t start = prTextEditCodePointStart(source, stop);
                stop = start > at ? start : stop;
                if (source.data[stop - 1] == '\r' && source.data[stop] == '\n')
                    ++stop;
            }
        }
        PrTextEditInputFlags keep =
            PrTextEditInputFlag_LineFeeds | PrTextEditInputFlag_Tabs | PrTextEditInputFlag_Controls;
        PrStringView part{source.data + at, stop - at};
        document->piece.resize(stop - at);
        document->piece.resize(prTextEditInputNormalize(part, keep, document->piece.data));
        uint32_t size = prTextEditBufferSize(document->buffer);
        PrTextEditChange change{
            .start = size,
            .end = size,
            .text = PrStringView{document->piece.data, document->piece.count},
        };
        prTextEditBufferApply(document->buffer, &change, 1, PrTextEditOrigin_Load);
        document->loaded = stop;
        if (document->syntax != nullptr && prSyntaxTreeUpdate(document->syntax, kPrF64Infinity))
            ++document->syntaxChanges;
    } while (document->loaded < source.size && prTimeMonotonicMilliseconds() < end);
    if (document->loaded == source.size)
        finishLoad(document);
}

uint32_t caretOf(const TextEditorDocument* document) {
    return prTextEditViewPrimary(prTextViewEdit(document->view)).caret;
}

// Moves the primary caret to `offset`, with one selection, and brings it into view.
void moveCaret(TextEditorDocument* document, uint32_t offset) {
    PrTextEditSelection caret{.anchor = offset, .caret = offset};
    prTextEditViewSetSelections(prTextViewEdit(document->view), &caret, 1);
    prTextViewReveal(document->view);
}

// Shows a place that a definition named once the buffer holds its line.
void revealPending(TextEditorDocument* document) {
    if (!document->revealPending)
        return;
    if (document->loading && prTextEditBufferLineCount(document->buffer) <= document->revealPosition.line + 1)
        return;
    document->revealPending = false;
    moveCaret(document, prLspOffsetFromPosition(document->buffer, document->revealPosition, document->revealEncoding));
}

void showDocument(TextEditor* editor, TextEditorDocument* document) {
    editor->shown = document;
    editor->diff = nullptr;
    editor->focusAsked = true;
    editor->hoverDocument = nullptr;
}

void showDiff(TextEditor* editor, TextEditorDiff* diff) {
    showDocument(editor, diff->young);
    editor->diff = diff;
    for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side)
        diff->offsets[side] = -1;
}

// Refuses every edit of the document's user.
bool refuseEdits(void* PR_NULLABLE, PrTextEditOrigin, PrTextEditChange*, uint32_t) {
    return false;
}

// Moves the caret to the start of the next change after the caret's line, or the previous before it.
void stepChange(TextEditor* editor, bool forward) {
    TextEditorDiff* diff = editor->diff;
    if (diff == nullptr || diff->model.changes.count == 0) {
        setStatus(editor, "no changes");
        return;
    }
    TextEditorDocument* young = diff->young;
    uint32_t line = prTextEditBufferPointFromOffset(young->buffer, caretOf(young)).line;
    const RawArray<PrTextDiffChange>& changes = diff->model.changes;
    uint32_t chosen = forward ? 0 : changes.count - 1;
    for (uint32_t i = 0; i < changes.count; ++i) {
        uint32_t k = forward ? i : changes.count - 1 - i;
        uint32_t start = changes.data[k].newStart;
        if (forward ? start > line : start < line) {
            chosen = k;
            break;
        }
    }
    uint32_t start = changes.data[chosen].newStart;
    uint32_t lines = prTextEditBufferLineCount(young->buffer);
    moveCaret(young, prTextEditBufferLineStart(young->buffer, start < lines ? start : lines - 1));
}

TextEditorDocument* PR_NULLABLE documentFromPath(const TextEditor* editor, PrStringView path) {
    for (uint32_t i = 0; i < editor->documents.count; ++i) {
        if (prRawStringView(&editor->documents.data[i]->path) == path)
            return editor->documents.data[i];
    }
    // A path from elsewhere, as a language server's URI gives one, may spell a document's file another way.
    for (uint32_t i = 0; i < editor->documents.count; ++i) {
        const PrRawString& own = editor->documents.data[i]->path;
        if (!prRawStringIsNull(&own) && prFileSame(prRawStringView(&own), path))
            return editor->documents.data[i];
    }
    return nullptr;
}

// Writes the document to its file, with the line endings that the file had.
PrStatus saveDocument(TextEditorDocument* document) {
    if (prRawStringIsNull(&document->path) || document->loading)
        return PrStatus_InvalidState;
    PrStringView text = prTextEditBufferText(document->buffer);
    bool ok = false;
    if (document->crlf) {
        RawArray<uint8_t> restored;
        for (size_t i = 0; i < text.size; ++i) {
            if (text.data[i] == '\n')
                restored.push('\r');
            restored.push(uint8_t(text.data[i]));
        }
        ok = prFileWrite(prRawStringView(&document->path), PrByteView{restored.data, restored.count});
        restored.reset();
    } else {
        ok = prFileWrite(prRawStringView(&document->path),
            PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    }
    if (!ok)
        return PrStatus_Io;
    document->savedVersion = prTextEditBufferVersion(document->buffer);
    if (document->lsp != nullptr)
        prLspDocumentSave(document->lsp);
    return PrStatus_Ok;
}

// Returns a record of the given box and look.
PrLayoutStyleIndex record(PrLayoutTree* tree, const PrLayoutBoxStyle& box,
        const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr) {
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    if (decoration != nullptr)
        style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, decoration);
    return prLayoutStyleIntern(tree, &style);
}

// Debugging

// Writes the lines of the document's breakpoints to `out`, in order, and drops the breakpoints whose lines were
// removed.
void breakpointLinesOf(TextEditorDocument* document, RawArray<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < document->breakpoints.count;) {
        PrTextEditAnchor anchor = document->breakpoints.data[i];
        if (!prTextEditAnchorIsLive(document->buffer, anchor)) {
            prTextEditAnchorRemove(document->buffer, anchor);
            document->breakpoints.erase(i);
            continue;
        }
        uint32_t offset = prTextEditAnchorOffset(document->buffer, anchor);
        out.push(prTextEditBufferPointFromOffset(document->buffer, offset).line);
        ++i;
    }
    for (uint32_t i = 1; i < out.count; ++i) {
        for (uint32_t k = i; k > 0 && out.data[k - 1] > out.data[k]; --k) {
            uint32_t swapped = out.data[k];
            out.data[k] = out.data[k - 1];
            out.data[k - 1] = swapped;
        }
    }
}

void toggleBreakpoint(TextEditorDocument* document, uint32_t line) {
    uint32_t lines = prTextEditBufferLineCount(document->buffer);
    line = line < lines ? line : lines - 1;
    for (uint32_t i = 0; i < document->breakpoints.count; ++i) {
        PrTextEditAnchor anchor = document->breakpoints.data[i];
        uint32_t at = prTextEditBufferPointFromOffset(document->buffer, prTextEditAnchorOffset(document->buffer, anchor))
            .line;
        if (at == line) {
            prTextEditAnchorRemove(document->buffer, anchor);
            document->breakpoints.erase(i);
            return;
        }
    }
    uint32_t start = prTextEditBufferLineStart(document->buffer, line);
    document->breakpoints.push(prTextEditAnchorCreate(document->buffer, start, PrTextEditBias_Left));
}

// Sends the session the breakpoints of each document of a file whose lines changed since they were last sent.
void syncBreakpoints(TextEditor* editor) {
    if (editor->debug == nullptr)
        return;
    RawArray<uint32_t> lines;
    for (uint32_t i = 0; i < editor->documents.count; ++i) {
        TextEditorDocument* document = editor->documents.data[i];
        if (prRawStringIsNull(&document->path))
            continue;
        breakpointLinesOf(document, lines);
        bool same = document->sent && lines.count == document->sentLines.count &&
            prMemEquals(lines.data, document->sentLines.data, lines.count * sizeof(uint32_t));
        if (same)
            continue;
        prDapClientSetBreakpoints(editor->debug->client, prRawStringView(&document->path), lines.data, lines.count);
        document->sentLines.clear();
        if (lines.count != 0)
            document->sentLines.append(lines.data, lines.count);
        document->sent = true;
    }
    lines.reset();
}

bool startDebugging(TextEditor* editor) {
    if (!editor->hasDebugger)
        return false;
    if (editor->debug != nullptr) {
        PrTextEditorDebugState state = textEditorDebugState(editor->debug);
        if (state != PrTextEditorDebugState_Ended && state != PrTextEditorDebugState_Failed)
            return false;
        textEditorDebugDestroy(editor->debug);
    }
    editor->debug = textEditorDebugCreate(editor->debugger, editor->launcher, editor->app);
    editor->debugStops = 0;
    for (uint32_t i = 0; i < editor->documents.count; ++i)
        editor->documents.data[i]->sent = false;
    syncBreakpoints(editor);
    return true;
}

void stopDebugging(TextEditor* editor) {
    if (editor->debug == nullptr)
        return;
    textEditorDebugDestroy(editor->debug);
    editor->debug = nullptr;
}

// Returns the top frame of the stopped thread, or null if the debuggee runs or the stack has not arrived.
const TextEditorDebugFrame* PR_NULLABLE stoppedFrame(const TextEditor* editor) {
    const TextEditorDebugSession* session = editor->debug;
    if (session == nullptr || textEditorDebugState(session) != PrTextEditorDebugState_Stopped ||
            session->stops == 0 || session->frames.count == 0)
        return nullptr;
    return &session->frames.data[0];
}

// Shows the frame of a stop that the editor has not shown: its file's document, with the caret at its line.
void revealStop(TextEditor* editor) {
    if (editor->debug == nullptr || editor->debug->stops == editor->debugStops)
        return;
    editor->debugStops = editor->debug->stops;
    const TextEditorDebugFrame* frame = stoppedFrame(editor);
    if (frame == nullptr || prRawStringIsEmpty(&frame->path))
        return;
    TextEditorDocument* document = documentFromPath(editor, prRawStringView(&frame->path));
    if (document == nullptr) {
        PrTextEditorDocumentDesc desc{.path = prRawStringView(&frame->path)};
        PrTextEditorDocument* opened = nullptr;
        if (prTextEditorDocumentCreate(editor->handle, &desc, &opened) != PrStatus_Ok)
            return;
        document = documentFromPath(editor, prRawStringView(&frame->path));
    }
    showDocument(editor, document);
    document->revealPending = true;
    document->revealPosition = PrLspPosition{frame->line, 0};
    document->revealEncoding = PrLspEncoding_Utf8;
    revealPending(document);
}

// Returns the color of the line of the document that the debuggee stopped on, or zero alpha for every other line.
PrColor16 executionLineColor(void* PR_NULLABLE context, uint32_t line) {
    const TextEditorDocument* document = static_cast<const TextEditorDocument*>(context);
    const TextEditorDebugFrame* frame = stoppedFrame(document->editor);
    if (frame == nullptr || frame->line != line || prRawStringView(&frame->path) != prRawStringView(&document->path))
        return PrColor16{};
    PrColor color = document->editor->kit->theme.palette.warning;
    color.a = 0.22f;
    return prColor16FromColor(color);
}

const char* debugStateText(PrTextEditorDebugState state) {
    switch (state) {
        case PrTextEditorDebugState_Starting: return "starting";
        case PrTextEditorDebugState_Running: return "running";
        case PrTextEditorDebugState_Stopped: return "stopped";
        case PrTextEditorDebugState_Ended: return "ended";
        case PrTextEditorDebugState_Failed: return "the debugger failed";
        default: return "";
    }
}

// Declares a column of the panel: a caption, then up to kDebugPanelRows lines of `lines`.
PrLayoutNode debugColumn(TextEditor* editor, PrLayoutNode row, PrStringView caption) {
    PrKitBoxStyle style{.gap = kPrKitSpace1, .place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode column = prKitBox(editor->kit, row, {}, style);
    prKitLabel(editor->kit, column, {.text = caption}, {.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted});
    return column;
}

void debugLine(TextEditor* editor, PrLayoutNode column, PrStringView text) {
    prKitLabel(editor->kit, column, {.text = text}, {.role = PrKitTextRole_Mono, .wrap = PrKitWrap_Clip});
}

// Declares the panel of the session below the text: its state, the stopped thread's stack, the top frame's variables,
// and the debuggee's last lines of output.
void buildDebugPanel(TextEditor* editor, PrLayoutNode parent, float top, PrVec2 extent) {
    const TextEditorDebugSession* session = editor->debug;
    PrKitSurfaceStyle style{
        .box = {
            .direction = PrKitDirection_Row,
            .overflow = PrKitOverflow_Clip,
            .gap = kPrKitSpace8,
            .padding = prKitEdgesXY(kPrKitSpace3, kPrKitSpace4),
            .place = {.width = prKitPx(extent.x), .height = prKitPx(extent.y)},
        },
        .ground = PrKitGround_Panel,
    };
    PrLayoutBoxStyle placed = prLayoutBoxStyleDefault();
    placed.display = PrLayoutDisplay_Block;
    placed.position = PrLayoutPosition_Absolute;
    placed.inset[PrRectSide_Left] = prLayoutLengthPx(0);
    placed.inset[PrRectSide_Top] = prLayoutLengthPx(top);
    PrLayoutTree* tree = prUiTree(editor->ui);
    PrLayoutNode holder = prLayoutTreeFrameElement(tree, parent, 0, record(tree, placed));
    PrLayoutNode panel = prKitSurface(editor->kit, holder, {}, style);

    PrTextEditorDebugState state = textEditorDebugState(session);
    PrLayoutNode stack = debugColumn(editor, panel, "CALL STACK");
    PrRawString line = prRawStringEmpty();
    prRawStringAppend(&line, debugStateText(state));
    PrStringView reason = prDapClientStopReason(session->client);
    if (reason.size != 0) {
        prRawStringAppend(&line, ": ");
        prRawStringAppend(&line, reason);
    }
    if (state == PrTextEditorDebugState_Ended && prDapClientExitCode(session->client) >= 0) {
        char digits[kPrU64TextMaxBytes];
        prRawStringAppend(&line, ", exit code ");
        uint64_t code = uint64_t(prDapClientExitCode(session->client));
        prRawStringAppend(&line, PrStringView{digits, prU64Format(code, digits, 10)});
    }
    debugLine(editor, stack, prRawStringView(&line));
    bool stopped = state == PrTextEditorDebugState_Stopped;
    for (uint32_t i = 0; stopped && i < session->frames.count && i < kDebugPanelRows; ++i) {
        const TextEditorDebugFrame& frame = session->frames.data[i];
        prRawStringReplace(&line, prRawStringView(&frame.name));
        if (!prRawStringIsEmpty(&frame.path)) {
            PrStringView path = prRawStringView(&frame.path);
            size_t slash = prStringFindLastByte(path, '/');
            prRawStringAppend(&line, "  ");
            prRawStringAppend(&line, slash != path.size ? prStringSlice(path, slash + 1, path.size - slash - 1) : path);
            char digits[kPrU64TextMaxBytes];
            prRawStringAppend(&line, ":");
            prRawStringAppend(&line, PrStringView{digits, prU64Format(uint64_t(frame.line) + 1, digits, 10)});
        }
        debugLine(editor, stack, prRawStringView(&line));
    }
    PrLayoutNode variables = debugColumn(editor, panel, "VARIABLES");
    for (uint32_t i = 0; stopped && i < session->variables.count && i < kDebugPanelRows; ++i) {
        const TextEditorDebugVariable& variable = session->variables.data[i];
        prRawStringReplace(&line, prRawStringView(&variable.name));
        prRawStringAppend(&line, " = ");
        prRawStringAppend(&line, prRawStringView(&variable.value));
        debugLine(editor, variables, prRawStringView(&line));
    }
    PrLayoutNode output = debugColumn(editor, panel, "OUTPUT");
    uint32_t first = session->output.count > kDebugPanelRows ? session->output.count - kDebugPanelRows : 0;
    for (uint32_t i = first; i < session->output.count; ++i)
        debugLine(editor, output, prRawStringView(&session->output.data[i]));
    prRawStringReset(&line);
}

// Language servers

// Returns the shown document if its server runs, or null.
TextEditorDocument* PR_NULLABLE servedDocument(TextEditor* editor) {
    TextEditorDocument* document = editor->shown;
    if (document == nullptr || document->lsp == nullptr ||
            textEditorServerState(document->server) != PrTextEditorServerState_Running)
        return nullptr;
    return document;
}

void hoverAnswered(void* PR_NULLABLE context, const PrLspResponse* response) {
    TextEditor* editor = static_cast<TextEditor*>(context);
    if (response->request != editor->hoverRequest)
        return;
    editor->hoverRequest = kPrLspRequestNone;
    PrRawString text = prLspHoverText(response->json, response->result);
    if (prRawStringIsEmpty(&text)) {
        setStatus(editor, "nothing to show here");
        prRawStringReset(&text);
        return;
    }
    prRawStringReset(&editor->hoverText);
    editor->hoverText = text;
    editor->hoverOpening = true;
}

void definitionAnswered(void* PR_NULLABLE context, const PrLspResponse* response) {
    TextEditor* editor = static_cast<TextEditor*>(context);
    if (response->request != editor->definitionRequest)
        return;
    editor->definitionRequest = kPrLspRequestNone;
    PrLspLocation location{};
    if (prLspLocations(response->json, response->result, &location, 1) == 0) {
        setStatus(editor, "no definition found");
        return;
    }
    PrRawString path = prLspPathFromUri(location.uri);
    if (prRawStringIsNull(&path)) {
        setStatus(editor, "the definition is not in a file");
        return;
    }
    PrLspEncoding encoding = PrLspEncoding_Utf16;
    TextEditorDocument* asked = editor->definitionDocument;
    if (asked != nullptr && asked->server != nullptr)
        encoding = prLspClientEncoding(asked->server->client);
    TextEditorDocument* target = documentFromPath(editor, prRawStringView(&path));
    if (target == nullptr) {
        PrTextEditorDocumentDesc desc{.path = prRawStringView(&path)};
        PrTextEditorDocument* opened = nullptr;
        if (prTextEditorDocumentCreate(editor->handle, &desc, &opened) != PrStatus_Ok) {
            setStatus(editor, "the definition's file cannot be read");
            prRawStringReset(&path);
            return;
        }
        target = documentFromPath(editor, prRawStringView(&path));
    }
    prRawStringReset(&path);
    target->revealPending = true;
    target->revealPosition = location.range.start;
    target->revealEncoding = encoding;
    showDocument(editor, target);
    revealPending(target);
}

// Moves the caret to the start of the next diagnostic after it, or the previous before it, around the end of the text.
void stepDiagnostic(TextEditor* editor, bool forward) {
    TextEditorDocument* document = editor->shown;
    if (document == nullptr || document->lsp == nullptr || prLspDocumentDiagnosticCount(document->lsp) == 0) {
        setStatus(editor, "no problems");
        return;
    }
    uint32_t caret = caretOf(document);
    uint32_t count = prLspDocumentDiagnosticCount(document->lsp);
    bool found = false;
    uint32_t best = 0;
    uint32_t wrap = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t start = prLspDocumentDiagnostic(document->lsp, i).start;
        if (i == 0 || (forward ? start < wrap : start > wrap))
            wrap = start;
        bool beyond = forward ? start > caret : start < caret;
        if (beyond && (!found || (forward ? start < best : start > best))) {
            best = start;
            found = true;
        }
    }
    moveCaret(document, found ? best : wrap);
}

// Commands

void addCommand(TextEditor* editor, const char* id, const char* title, const char* PR_NULLABLE chord,
        PrCommandRunFn run, void* context, PrCommandStateFn PR_NULLABLE state = nullptr) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Editor", .run = run, .state = state, .context = context};
    prCommandRegistryAdd(editor->commands, &desc);
    PrCommandChord parsed{};
    if (chord != nullptr && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(editor->keymap, parsed, id, "", PrCommandBindingKind_Normal);
}

PrCommandStateFlags needsDocument(void* PR_NULLABLE context) {
    return editorOf(context).shown != nullptr ? 0 : PrCommandStateFlag_Disabled;
}

PrCommandStateFlags needsServer(void* PR_NULLABLE context) {
    return servedDocument(&editorOf(context)) != nullptr ? 0 : PrCommandStateFlag_Disabled;
}

void registerCommands(TextEditor* e) {
    for (uint32_t i = 0; i < countOf(kKeyCommands); ++i) {
        e->keyContexts[i] = ChoiceContext{e, i};
        addCommand(e, kKeyCommands[i].id, kKeyCommands[i].title, kKeyCommands[i].chord,
            [](void* c, const PrCommandArgs*) {
                ChoiceContext& key = *static_cast<ChoiceContext*>(c);
                if (key.editor->shown != nullptr) {
                    const KeyCommand& command = kKeyCommands[key.index];
                    prTextViewRun(key.editor->shown->view, command.command, command.kind);
                }
            }, &e->keyContexts[i], needsDocument);
    }
    addCommand(e, "cursor.pageUp", "Page Up", "PageUp", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewRunPage(editorOf(c).shown->view, PrTextViewPage_Up, PrTextEditSelectionMove_Collapse);
    }, e, needsDocument);
    addCommand(e, "cursor.pageDown", "Page Down", "PageDown", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewRunPage(editorOf(c).shown->view, PrTextViewPage_Down, PrTextEditSelectionMove_Collapse);
    }, e, needsDocument);
    addCommand(e, "select.pageUp", "Select Page Up", "Shift PageUp", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewRunPage(editorOf(c).shown->view, PrTextViewPage_Up, PrTextEditSelectionMove_Extend);
    }, e, needsDocument);
    addCommand(e, "select.pageDown", "Select Page Down", "Shift PageDown", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewRunPage(editorOf(c).shown->view, PrTextViewPage_Down, PrTextEditSelectionMove_Extend);
    }, e, needsDocument);
    addCommand(e, "cursor.addBelow", "Add Cursor Below", "Cmd Alt Down", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewAddCaretOnLine(editorOf(c).shown->view, PrTextEditLineStep_Next);
    }, e, needsDocument);
    addCommand(e, "cursor.addAbove", "Add Cursor Above", "Cmd Alt Up", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewAddCaretOnLine(editorOf(c).shown->view, PrTextEditLineStep_Previous);
    }, e, needsDocument);
    addCommand(e, "cursor.single", "Keep Primary Cursor", "Escape", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        editor.hoverDocument = nullptr;
        if (editor.shown != nullptr)
            prTextViewKeepPrimary(editor.shown->view);
    }, e, needsDocument);
    addCommand(e, "edit.tab", "Insert Tab", "Tab", [](void* c, const PrCommandArgs*) {
        if (editorOf(c).shown != nullptr)
            prTextViewInsert(editorOf(c).shown->view, "\t", PrTextEditMerge_Typing);
    }, e, needsDocument);
    addCommand(e, "edit.undo", "Undo", "Cmd Z", [](void* c, const PrCommandArgs*) {
        TextEditorDocument* document = editorOf(c).shown;
        if (document != nullptr && prTextEditHistoryUndo(document->history, prTextViewEdit(document->view)))
            prTextViewReveal(document->view);
    }, e, [](void* c) -> PrCommandStateFlags {
        TextEditorDocument* document = editorOf(c).shown;
        return document != nullptr && prTextEditHistoryCanUndo(document->history) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "edit.redo", "Redo", "Cmd Shift Z", [](void* c, const PrCommandArgs*) {
        TextEditorDocument* document = editorOf(c).shown;
        if (document != nullptr && prTextEditHistoryRedo(document->history, prTextViewEdit(document->view)))
            prTextViewReveal(document->view);
    }, e, [](void* c) -> PrCommandStateFlags {
        TextEditorDocument* document = editorOf(c).shown;
        return document != nullptr && prTextEditHistoryCanRedo(document->history) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "edit.copy", "Copy", "Cmd C", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.shown == nullptr)
            return;
        PrRawString text = prTextViewSelectedText(editor.shown->view);
        if (editor.app != nullptr && prRawStringView(&text).size != 0)
            prAppSetClipboardText(editor.app, prRawStringView(&text));
        prRawStringReset(&text);
    }, e, needsDocument);
    addCommand(e, "edit.cut", "Cut", "Cmd X", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.shown == nullptr)
            return;
        PrRawString text = prTextViewSelectedText(editor.shown->view);
        if (editor.app != nullptr && prRawStringView(&text).size != 0)
            prAppSetClipboardText(editor.app, prRawStringView(&text));
        prRawStringReset(&text);
        prTextViewEraseSelection(editor.shown->view);
    }, e, needsDocument);
    addCommand(e, "edit.paste", "Paste", "Cmd V", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.port != kPrLayoutNodeNone)
            prUiTextViewPaste(editor.ui, editor.port);
    }, e, needsDocument);
    addCommand(e, "file.save", "Save", "Cmd S", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.shown == nullptr)
            return;
        PrStatus status = saveDocument(editor.shown);
        setStatus(&editor, status == PrStatus_Ok ? "saved" : "could not save");
    }, e, [](void* c) -> PrCommandStateFlags {
        const TextEditorDocument* document = editorOf(c).shown;
        // A file that is still loading would be saved cut short.
        bool savable = document != nullptr && !prRawStringIsNull(&document->path) && !document->loading;
        return savable ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "view.toggleWrap", "Word Wrap", "Alt Z", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        editor.options.wrap = !editor.options.wrap;
        if (editor.shown != nullptr)
            prTextViewReveal(editor.shown->view);
    }, e, [](void* c) -> PrCommandStateFlags { return editorOf(c).options.wrap ? PrCommandStateFlag_Checked : 0; });
    addCommand(e, "view.lineNumbers", "Line Numbers", nullptr, [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        editor.options.lineNumbers = !editor.options.lineNumbers;
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).options.lineNumbers ? PrCommandStateFlag_Checked : 0;
    });
    for (uint32_t i = 0; i < countOf(kDirections); ++i) {
        e->directionContexts[i] = ChoiceContext{e, i};
        addCommand(e, kDirections[i].id, kDirections[i].title, nullptr, [](void* c, const PrCommandArgs*) {
            ChoiceContext& choice = *static_cast<ChoiceContext*>(c);
            choice.editor->options.direction = kDirections[choice.index].direction;
            if (choice.editor->shown != nullptr)
                prTextViewReveal(choice.editor->shown->view);
        }, &e->directionContexts[i], [](void* c) -> PrCommandStateFlags {
            const ChoiceContext& choice = *static_cast<const ChoiceContext*>(c);
            bool chosen = choice.editor->options.direction == kDirections[choice.index].direction;
            return chosen ? PrCommandStateFlag_Checked : 0;
        });
    }
    for (uint32_t i = 0; i < countOf(kWritingModes); ++i) {
        e->writingModeContexts[i] = ChoiceContext{e, i};
        addCommand(e, kWritingModes[i].id, kWritingModes[i].title, nullptr, [](void* c, const PrCommandArgs*) {
            ChoiceContext& choice = *static_cast<ChoiceContext*>(c);
            choice.editor->options.writingMode = kWritingModes[choice.index].mode;
            if (choice.editor->shown != nullptr)
                prTextViewReveal(choice.editor->shown->view);
        }, &e->writingModeContexts[i], [](void* c) -> PrCommandStateFlags {
            const ChoiceContext& choice = *static_cast<const ChoiceContext*>(c);
            bool chosen = choice.editor->options.writingMode == kWritingModes[choice.index].mode;
            return chosen ? PrCommandStateFlag_Checked : 0;
        });
    }
    addCommand(e, "language.hover", "Show Hover", "Cmd I", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        TextEditorDocument* document = servedDocument(&editor);
        if (document == nullptr)
            return;
        editor.hoverDocument = document;
        editor.hoverOffset = caretOf(document);
        editor.hoverVersion = prTextEditBufferVersion(document->buffer);
        prRawStringReplace(&editor.hoverText, "");
        editor.hoverRequest = prLspDocumentHover(document->lsp, editor.hoverOffset, hoverAnswered, &editor);
    }, e, needsServer);
    addCommand(e, "language.definition", "Go to Definition", "F12", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        TextEditorDocument* document = servedDocument(&editor);
        if (document == nullptr)
            return;
        editor.definitionDocument = document;
        editor.definitionRequest = prLspDocumentDefinition(document->lsp, caretOf(document), definitionAnswered, &editor);
    }, e, needsServer);
    addCommand(e, "language.nextProblem", "Next Problem", "F8", [](void* c, const PrCommandArgs*) {
        stepDiagnostic(&editorOf(c), true);
    }, e, needsDocument);
    addCommand(e, "language.previousProblem", "Previous Problem", "Shift F8", [](void* c, const PrCommandArgs*) {
        stepDiagnostic(&editorOf(c), false);
    }, e, needsDocument);
    addCommand(e, "debug.toggleBreakpoint", "Toggle Breakpoint", "F9", [](void* c, const PrCommandArgs*) {
        TextEditorDocument* document = editorOf(c).shown;
        if (document != nullptr)
            toggleBreakpoint(document, prTextEditBufferPointFromOffset(document->buffer, caretOf(document)).line);
    }, e, needsDocument);
    addCommand(e, "debug.continue", "Start or Continue Debugging", "F5", [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.debug != nullptr && textEditorDebugState(editor.debug) == PrTextEditorDebugState_Stopped)
            prDapClientStep(editor.debug->client, prDapClientStoppedThread(editor.debug->client), PrDapStep_Continue);
        else if (!startDebugging(&editor))
            setStatus(&editor, editor.hasDebugger ? "a session runs" : "no debugger");
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).hasDebugger ? 0 : PrCommandStateFlag_Disabled;
    });
    struct StepCommand {
        const char* id;
        const char* title;
        const char* chord;
        PrDapStep step;
    };
    static const StepCommand kSteps[] = {
        {"debug.stepOver", "Step Over", "F10", PrDapStep_Over},
        {"debug.stepInto", "Step Into", "F11", PrDapStep_Into},
        {"debug.stepOut", "Step Out", "Shift F11", PrDapStep_Out},
    };
    for (uint32_t i = 0; i < countOf(kSteps); ++i) {
        e->stepContexts[i] = ChoiceContext{e, i};
        addCommand(e, kSteps[i].id, kSteps[i].title, kSteps[i].chord, [](void* c, const PrCommandArgs*) {
            ChoiceContext& choice = *static_cast<ChoiceContext*>(c);
            TextEditorDebugSession* session = choice.editor->debug;
            if (session != nullptr && textEditorDebugState(session) == PrTextEditorDebugState_Stopped)
                prDapClientStep(session->client, prDapClientStoppedThread(session->client), kSteps[choice.index].step);
        }, &e->stepContexts[i], [](void* c) -> PrCommandStateFlags {
            const TextEditorDebugSession* session = static_cast<ChoiceContext*>(c)->editor->debug;
            bool stopped = session != nullptr && textEditorDebugState(session) == PrTextEditorDebugState_Stopped;
            return stopped ? 0 : PrCommandStateFlag_Disabled;
        });
    }
    addCommand(e, "debug.stop", "Stop Debugging", "Shift F5", [](void* c, const PrCommandArgs*) {
        stopDebugging(&editorOf(c));
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).debug != nullptr ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "diff.compareSaved", "Compare with Saved", nullptr, [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        TextEditorDocument* young = editor.shown;
        if (young == nullptr || prRawStringIsNull(&young->path) || young->loading)
            return;
        // The saved text is a document of no file of its own, which the file's name gives the same language.
        PrFileMapping mapping{};
        if (!prFileMap(prRawStringView(&young->path), &mapping)) {
            setStatus(&editor, "the saved file cannot be read");
            return;
        }
        PrTextEditorDocumentDesc desc{
            .text = PrStringView{reinterpret_cast<const char*>(mapping.data), mapping.size},
            .name = prRawStringView(&young->path),
            .readOnly = true,
        };
        PrTextEditorDocument* saved = nullptr;
        PrStatus status = prTextEditorDocumentCreate(editor.handle, &desc, &saved);
        prFileUnmap(&mapping);
        if (status != PrStatus_Ok) {
            setStatus(&editor, "the saved file is too large to compare");
            return;
        }
        TextEditorDocument* old = editor.documents.last();
        editor.documents.pop();
        old->listed = false;
        prTextEditorDiffCreate(editor.handle, saved, young->handle);
        editor.diff->ownsOld = true;
    }, e, [](void* c) -> PrCommandStateFlags {
        const TextEditorDocument* document = editorOf(c).shown;
        bool comparable = document != nullptr && !prRawStringIsNull(&document->path) && !document->loading;
        return comparable ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "diff.close", "Close Comparison", nullptr, [](void* c, const PrCommandArgs*) {
        TextEditor& editor = editorOf(c);
        if (editor.diff != nullptr)
            prTextEditorDiffDestroy(editor.diff->handle);
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).diff != nullptr ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "diff.nextChange", "Next Change", "F7", [](void* c, const PrCommandArgs*) {
        stepChange(&editorOf(c), true);
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).diff != nullptr ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(e, "diff.previousChange", "Previous Change", "Shift F7", [](void* c, const PrCommandArgs*) {
        stepChange(&editorOf(c), false);
    }, e, [](void* c) -> PrCommandStateFlags {
        return editorOf(c).diff != nullptr ? 0 : PrCommandStateFlag_Disabled;
    });
}

// What a frame shows

// Writes the colored spans of the window from the syntax tree.
uint32_t syntaxSpans(void* PR_NULLABLE context, uint32_t start, uint32_t end, PrTextViewSpan* PR_NULLABLE out,
        uint32_t capacity) {
    TextEditorDocument* document = static_cast<TextEditorDocument*>(context);
    const TextEditor* editor = document->editor;
    uint64_t version = prTextEditBufferVersion(document->buffer);
    if (version != document->spansVersion || document->syntaxChanges != document->spansChanges ||
            start != document->spansStart || end != document->spansEnd) {
        document->spans.resize(prSyntaxTreeSpans(document->syntax, start, end, nullptr, 0));
        prSyntaxTreeSpans(document->syntax, start, end, document->spans.data, document->spans.count);
        document->spansVersion = version;
        document->spansChanges = document->syntaxChanges;
        document->spansStart = start;
        document->spansEnd = end;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < document->spans.count; ++i) {
        const PrSyntaxSpan& span = document->spans.data[i];
        if (span.category >= editor->categoryColors.count || editor->categoryColors.data[span.category].a == 0)
            continue;
        if (count < capacity)
            out[count] = PrTextViewSpan{
                .start = span.start,
                .end = span.end,
                .color = editor->categoryColors.data[span.category],
            };
        ++count;
    }
    return count;
}

PrColor16 severityColor(const TextEditor* editor, PrLspSeverity severity) {
    const PrKitPalette& palette = editor->kit->theme.palette;
    switch (severity) {
        case PrLspSeverity_Error: return prColor16FromColor(palette.danger);
        case PrLspSeverity_Warning: return prColor16FromColor(palette.warning);
        case PrLspSeverity_Information: return prColor16FromColor(palette.accent);
        default: return prColor16FromColor(palette.text3);
    }
}

// Returns the rank of a severity, from the least to the worst, so that None ranks below Hint.
uint32_t severityRank(PrLspSeverity severity) {
    return severity == PrLspSeverity_None ? 0 : uint32_t(PrLspSeverity_Hint + 1 - severity);
}

// Writes the underlines of the diagnostics that reach the window. Where diagnostics overlap, the worst underlines the
// bytes. An empty diagnostic underlines the character after it, or before it at the end of a line.
uint32_t diagnosticSpans(void* PR_NULLABLE context, uint32_t start, uint32_t end, PrTextViewSpan* PR_NULLABLE out,
        uint32_t capacity) {
    TextEditorDocument* document = static_cast<TextEditorDocument*>(context);
    TextEditor* editor = document->editor;
    struct Reach {
        uint32_t start;
        uint32_t end;
        PrLspSeverity severity;
    };
    RawArray<Reach> reaches;
    RawArray<uint32_t> cuts;
    uint32_t count = prLspDocumentDiagnosticCount(document->lsp);
    uint32_t size = prTextEditBufferSize(document->buffer);
    for (uint32_t i = 0; i < count; ++i) {
        PrLspDiagnostic diagnostic = prLspDocumentDiagnostic(document->lsp, i);
        uint32_t low = diagnostic.start;
        uint32_t high = diagnostic.end;
        if (low == high) {
            uint32_t line = prTextEditBufferPointFromOffset(document->buffer, low).line;
            if (low < prTextEditBufferLineEnd(document->buffer, line))
                high = low + 1;
            else if (low > prTextEditBufferLineStart(document->buffer, line))
                low = low - 1;
        }
        if (high > size)
            high = size;
        if (low >= high || high <= start || low >= end)
            continue;
        reaches.push(Reach{low, high, diagnostic.severity});
        cuts.push(low);
        cuts.push(high);
    }
    for (uint32_t i = 1; i < cuts.count; ++i) {
        for (uint32_t k = i; k > 0 && cuts.data[k - 1] > cuts.data[k]; --k) {
            uint32_t swapped = cuts.data[k];
            cuts.data[k] = cuts.data[k - 1];
            cuts.data[k - 1] = swapped;
        }
    }
    uint32_t written = 0;
    for (uint32_t i = 0; i + 1 < cuts.count; ++i) {
        uint32_t low = cuts.data[i];
        uint32_t high = cuts.data[i + 1];
        if (low == high)
            continue;
        bool covered = false;
        PrLspSeverity worst = PrLspSeverity_None;
        for (uint32_t k = 0; k < reaches.count; ++k) {
            if (reaches.data[k].start <= low && high <= reaches.data[k].end) {
                if (!covered || severityRank(reaches.data[k].severity) > severityRank(worst))
                    worst = reaches.data[k].severity;
                covered = true;
            }
        }
        if (!covered)
            continue;
        if (written < capacity) {
            out[written] = PrTextViewSpan{
                .start = low,
                .end = high,
                .underline = severityColor(editor, worst),
                .underlineStyle = PrDrawDecorationStyle(
                    worst == PrLspSeverity_Hint ? PrDrawDecorationStyle_Dotted : PrDrawDecorationStyle_Wavy),
            };
        }
        ++written;
    }
    reaches.reset();
    cuts.reset();
    return written;
}

// Records the worst severity of each line that has diagnostics, for the gutter.
void gatherLineSeverities(TextEditor* editor, TextEditorDocument* document) {
    editor->lineSeverities.clear();
    if (document->lsp == nullptr)
        return;
    uint32_t count = prLspDocumentDiagnosticCount(document->lsp);
    for (uint32_t i = 0; i < count; ++i) {
        PrLspDiagnostic diagnostic = prLspDocumentDiagnostic(document->lsp, i);
        uint32_t line = prTextEditBufferPointFromOffset(document->buffer, diagnostic.start).line;
        bool merged = false;
        for (uint32_t k = 0; k < editor->lineSeverities.count && !merged; ++k) {
            LineSeverity& known = editor->lineSeverities.data[k];
            if (known.line != line)
                continue;
            if (severityRank(diagnostic.severity) > severityRank(known.severity))
                known.severity = diagnostic.severity;
            merged = true;
        }
        if (!merged)
            editor->lineSeverities.push(LineSeverity{line, diagnostic.severity});
    }
}

// Declares a line's number in its gutter cell, in the color of its worst diagnostic.
void gutterOf(void* PR_NULLABLE context, PrLayoutTree* tree, PrLayoutNode cell, uint32_t line) {
    const GutterSide* side = static_cast<const GutterSide*>(context);
    TextEditor* editor = side->editor;
    PrLayoutStyleIndex style = editor->numberStyle;
    for (uint32_t i = 0; side->severities && i < editor->lineSeverities.count; ++i) {
        if (editor->lineSeverities.data[i].line == line)
            style = editor->numberStyles[editor->lineSeverities.data[i].severity];
    }
    // A breakpoint marks its line's number.
    bool marked = false;
    for (uint32_t i = 0; side->severities && i < editor->breakpointLines.count && !marked; ++i)
        marked = editor->breakpointLines.data[i] == line;
    char digits[kPrU64TextMaxBytes + 4];
    size_t length = 0;
    if (marked) {
        const char dot[] = "\xE2\x97\x8F ";
        for (size_t i = 0; i < sizeof(dot) - 1; ++i)
            digits[length++] = dot[i];
        style = editor->breakpointStyle;
    }
    length += prU64Format(uint64_t(line) + 1, digits + length, 10);
    PrLayoutNode number = prLayoutTreeFrameElement(tree, cell, 0, style);
    prLayoutTreeFrameText(tree, number, 0, PrStringView{digits, length});
}

// Makes the records of the gutter's numbers for a frame, from the rows' font.
void prepareGutter(TextEditor* editor, PrLayoutTree* tree, PrLayoutGroupIndex fontGroup) {
    const PrKitPalette& palette = editor->kit->theme.palette;
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.padding[PrRectSide_Right] = prLayoutLengthPx(kGutterPadding / 2);
    PrLayoutFontStyle font = prLayoutFontStyleRecord(tree, fontGroup);
    font.textAlign = PrLayoutTextAlign_Right;
    font.textAlignLast = PrLayoutTextAlign_Right;
    font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
    font.writingMode = PrLayoutWritingMode_HorizontalTb;
    font.direction = PrTextDirection_Ltr;
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    style.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
    for (uint32_t severity = 0; severity <= PrLspSeverity_Hint; ++severity) {
        PrLayoutColorStyle color = prLayoutColorStyleDefault();
        if (severity == PrLspSeverity_Error || severity == PrLspSeverity_Warning)
            color.color = severityColor(editor, PrLspSeverity(severity));
        else
            color.color = prColor16FromColor(palette.text3);
        style.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &color);
        editor->numberStyles[severity] = prLayoutStyleIntern(tree, &style);
    }
    editor->numberStyle = editor->numberStyles[PrLspSeverity_Information];
    PrLayoutColorStyle marked = prLayoutColorStyleDefault();
    marked.color = prColor16FromColor(palette.danger);
    style.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &marked);
    editor->breakpointStyle = prLayoutStyleIntern(tree, &style);
}

// Returns the gutter's size for the shown document's line count.
float gutterSize(TextEditor* editor, const TextEditorDocument* document) {
    if (editor->estimator == nullptr)
        editor->estimator = prTextEstimatorCreate(editor->font, PrTextAxis_Horizontal);
    uint32_t digits = 1;
    for (uint32_t lines = prTextEditBufferLineCount(document->buffer); lines >= 10; lines /= 10)
        ++digits;
    digits = digits > kGutterMinDigits ? digits : kGutterMinDigits;
    return float(digits) * prTextEstimatorAdvance(editor->estimator, '0') + kGutterPadding;
}

// Declares the scrollbar along the block axis of the text: at the right of horizontal lines, and below vertical ones,
// where the start of the document is at the side that the lines stack from. A drag moves the thumb. The document's size
// is an estimate outside the window, so the thumb can jump as estimates settle, while the reader's place holds.
void buildScrollbar(TextEditor* editor, TextEditorDocument* document, PrLayoutNode parent, PrVec2 extent) {
    PrLayoutTree* tree = prUiTree(editor->ui);
    const PrKitPalette& palette = editor->kit->theme.palette;
    PrTextViewInfo info = prTextViewInfo(document->view);
    bool vertical = editor->options.writingMode != PrLayoutWritingMode_HorizontalTb;
    bool reversed = editor->options.writingMode == PrLayoutWritingMode_VerticalRl;
    float size = vertical ? extent.x : extent.y;
    PrLayoutBoxStyle trackBox = prLayoutBoxStyleDefault();
    trackBox.display = PrLayoutDisplay_Block;
    trackBox.position = PrLayoutPosition_Absolute;
    trackBox.inset[PrRectSide_Right] = prLayoutLengthPx(0);
    trackBox.inset[vertical ? PrRectSide_Bottom : PrRectSide_Top] = prLayoutLengthPx(0);
    trackBox.width = prLayoutLengthPx(vertical ? size : kScrollbarWidth);
    trackBox.height = prLayoutLengthPx(vertical ? kScrollbarWidth : size);
    PrLayoutNode track = prUiElement(editor->ui, parent, 0, record(tree, trackBox), PrUiFlag_Pressable);
    if (info.documentSize <= double(size))
        return;
    float length = float(double(size) * double(size) / info.documentSize);
    length = length > kThumbMinLength ? length : kThumbMinLength;
    double range = info.documentSize - double(size);
    float room = size - length;
    float start = float(info.offset / range) * room;
    start = reversed ? room - start : start;
    PrUiDrag drag{};
    if (prUiDrag(editor->ui, track, 0, 0, &drag) && drag.active) {
        double at = double(vertical ? drag.position.x : drag.position.y) - double(length) / 2;
        at = reversed ? double(room) - at : at;
        prTextViewScrollTo(document->view, at / double(room) * range);
    }
    PrLayoutBoxStyle thumbBox = prLayoutBoxStyleDefault();
    thumbBox.display = PrLayoutDisplay_Block;
    thumbBox.position = PrLayoutPosition_Absolute;
    thumbBox.inset[PrRectSide_Left] = prLayoutLengthPx(vertical ? start : 2);
    thumbBox.inset[PrRectSide_Top] = prLayoutLengthPx(vertical ? 2 : start);
    thumbBox.width = prLayoutLengthPx(vertical ? length : kScrollbarWidth - 4);
    thumbBox.height = prLayoutLengthPx(vertical ? kScrollbarWidth - 4 : length);
    PrLayoutDecorationStyle thumbLook = prLayoutDecorationStyleDefault();
    PrColor thumbColor = palette.text3;
    thumbColor.a *= 0.5f;
    thumbLook.background = prColor16FromColor(thumbColor);
    prLayoutTreeFrameElement(tree, track, 0, record(tree, thumbBox, &thumbLook));
}

// Declares the hover's popover at `caret`, in viewport px, while the caret stays where the hover was asked for.
void buildHover(TextEditor* editor, TextEditorDocument* document, PrLayoutNode holder, PrRect caret) {
    if (editor->hoverDocument != document)
        return;
    if (caretOf(document) != editor->hoverOffset || prTextEditBufferVersion(document->buffer) != editor->hoverVersion) {
        editor->hoverDocument = nullptr;
        return;
    }
    if (prRawStringIsEmpty(&editor->hoverText))
        return;
    PrLayoutTree* tree = prUiTree(editor->ui);
    PrRect box = prLayoutRectToPx(prLayoutNodeBounds(tree, holder, PrLayoutCoordinates_Viewport));
    PrLayoutBoxStyle anchorBox = prLayoutBoxStyleDefault();
    anchorBox.display = PrLayoutDisplay_Block;
    anchorBox.position = PrLayoutPosition_Absolute;
    anchorBox.inset[PrRectSide_Left] = prLayoutLengthPx(caret.left - box.left);
    anchorBox.inset[PrRectSide_Top] = prLayoutLengthPx(caret.top - box.top);
    anchorBox.width = prLayoutLengthPx(1);
    anchorBox.height = prLayoutLengthPx(caret.bottom - caret.top);
    PrLayoutNode anchor = prUiElement(editor->ui, holder, 0, record(tree, anchorBox), 0);
    if (editor->hoverOpening)
        prKitSetOpen(editor->kit, anchor, true);
    PrLayoutNode popover = prKitPopover(editor->kit, anchor, {}, {});
    if (popover == kPrLayoutNodeNone) {
        // A popover that showed and no longer does was dismissed.
        if (!editor->hoverOpening)
            editor->hoverDocument = nullptr;
        return;
    }
    editor->hoverOpening = false;
    PrLayoutBoxStyle textBox = prLayoutBoxStyleDefault();
    textBox.display = PrLayoutDisplay_Block;
    textBox.maxWidth = prLayoutLengthPx(kHoverMaxWidth);
    PrLayoutNode column = prLayoutTreeFrameElement(tree, popover, 0, record(tree, textBox));
    prKitLabel(editor->kit, column, {.text = prRawStringView(&editor->hoverText)},
        {.role = PrKitTextRole_Mono, .wrap = PrKitWrap_Wrap, .preserve = true});
}

// A side of a comparison, as the context of its view's colors of lines and spans.
struct DiffSideView {
    TextEditorDiff* diff;
    TextEditorDiffSide side;
};

PrColor16 diffLineColor(void* PR_NULLABLE context, uint32_t line) {
    const DiffSideView* view = static_cast<const DiffSideView*>(context);
    return textEditorDiffLineColor(&view->diff->model, view->side, line);
}

uint32_t diffSpans(void* PR_NULLABLE context, uint32_t start, uint32_t end, PrTextViewSpan* PR_NULLABLE out,
        uint32_t capacity) {
    const DiffSideView* view = static_cast<const DiffSideView*>(context);
    return textEditorDiffSpans(&view->diff->model, view->side, start, end, out, capacity);
}

// Declares the view of `document` under `holder`, at `at` in px and in `extent`, and returns the element that shows its
// text. A side of a comparison is laid horizontally and does not wrap, and its lines take the comparison's colors.
PrLayoutNode buildDocument(TextEditor* editor, TextEditorDocument* document, PrLayoutNode holder, PrVec2 at,
        PrVec2 extent, const DiffSideView* PR_NULLABLE side) {
    PrLayoutTree* tree = prUiTree(editor->ui);
    const PrKitPalette& palette = editor->kit->theme.palette;
    const PrTextEditorOptions& options = editor->options;
    bool wrap = side == nullptr && options.wrap;
    PrLayoutWritingMode mode =
        side == nullptr ? options.writingMode : PrLayoutWritingMode(PrLayoutWritingMode_HorizontalTb);
    bool vertical = mode != PrLayoutWritingMode_HorizontalTb;

    // The port and the rows share a writing mode and a direction, which decide how the rows stack and scroll.
    PrLayoutFontStyle rowFont = prLayoutFontStyleDefault();
    rowFont.font = editor->font;
    rowFont.lineHeight = prLayoutLineHeightPx(kLineHeight);
    rowFont.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    rowFont.textWrapMode = wrap ? PrLayoutTextWrapMode_Wrap : PrLayoutTextWrapMode_Nowrap;
    rowFont.overflowWrap = PrTextOverflowWrap_Anywhere;
    rowFont.writingMode = mode;
    rowFont.direction = options.direction == PrTextEditorDirection_Rtl ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    PrLayoutGroupIndex fontGroup = prLayoutFontStyleIntern(tree, &rowFont);

    PrLayoutBoxStyle portBox = prLayoutBoxStyleDefault();
    portBox.display = PrLayoutDisplay_Block;
    portBox.width = prLayoutLengthPx(extent.x);
    portBox.height = prLayoutLengthPx(extent.y);
    portBox.overflowX = PrLayoutOverflow_Hidden;
    portBox.overflowY = PrLayoutOverflow_Hidden;
    if (side != nullptr) {
        portBox.position = PrLayoutPosition_Absolute;
        portBox.inset[PrRectSide_Left] = prLayoutLengthPx(at.x);
        portBox.inset[PrRectSide_Top] = prLayoutLengthPx(at.y);
    }
    PrLayoutStyle portStyle = prLayoutStyleDefault();
    portStyle.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &portBox);
    portStyle.groups[PrLayoutGroup_Font] = fontGroup;
    PrLayoutNode port = prUiElement(editor->ui, holder, 0, prLayoutStyleIntern(tree, &portStyle),
        PrUiFlag_Pressable | PrUiFlag_Focusable | PrUiFlag_Wheel);
    prUiSetCursor(editor->ui, port, PrAppCursor_Text);
    bool shown = document == editor->shown;
    if (shown && editor->focusAsked) {
        prUiSetFocus(editor->ui, port);
        editor->focusAsked = false;
    }

    PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
    rowBox.display = PrLayoutDisplay_Block;
    // Each row is a paragraph of its own, so that plaintext gives each line the direction of its script.
    rowBox.unicodeBidi = options.direction == PrTextEditorDirection_Auto ? PrTextBidi_Plaintext : PrTextBidi_Normal;
    PrLayoutColorStyle rowColor = prLayoutColorStyleDefault();
    rowColor.color = prColor16FromColor(palette.text1);
    PrLayoutStyle row = prLayoutStyleDefault();
    row.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &rowBox);
    row.groups[PrLayoutGroup_Font] = fontGroup;
    row.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &rowColor);
    PrLayoutStyleIndex rowStyle = prLayoutStyleIntern(tree, &row);

    PrTextViewSpanLayer layers[3];
    uint32_t layerCount = 0;
    if (side != nullptr)
        layers[layerCount++] = PrTextViewSpanLayer{diffSpans, const_cast<DiffSideView*>(side)};
    if (document->syntax != nullptr)
        layers[layerCount++] = PrTextViewSpanLayer{syntaxSpans, document};
    if (document->lsp != nullptr && prLspDocumentDiagnosticCount(document->lsp) != 0)
        layers[layerCount++] = PrTextViewSpanLayer{diagnosticSpans, document};
    // Numbers set along the lines' axis, so a vertical view shows none.
    bool numbered = options.lineNumbers && !vertical;
    GutterSide& gutter = editor->gutters[shown ? TextEditorDiffSide_New : TextEditorDiffSide_Old];
    gutter = GutterSide{editor, shown};
    if (numbered) {
        prepareGutter(editor, tree, fontGroup);
        if (shown) {
            gatherLineSeverities(editor, document);
            breakpointLinesOf(document, editor->breakpointLines);
        }
    }
    PrUiTextViewDesc view{
        .view = document->view,
        .control = port,
        .extent = extent,
        .rowStyle = rowStyle,
        .spanLayers = layerCount != 0 ? layers : nullptr,
        .spanLayerCount = layerCount,
        .gutter = numbered ? gutterOf : nullptr,
        .gutterContext = &gutter,
        .gutterSize = numbered ? gutterSize(editor, document) : 0,
        .gutterBackground = prColor16FromColor(palette.bg0),
        .lineColor = side != nullptr ? diffLineColor : (editor->debug != nullptr ? executionLineColor : nullptr),
        .lineColorContext = side != nullptr ? static_cast<void*>(const_cast<DiffSideView*>(side)) : document,
        .gapColor = prColor16FromColor(palette.bg2),
        .wrap = wrap,
        .selection = prColor16FromColor(palette.textSelection),
        .caret = prColor16FromColor(palette.text1),
    };
    prUiTextView(editor->ui, &view);
    return port;
}

// Brings the colors of the categories up to date with the theme, which is dark if its text is lighter than its ground.
void updateCategoryColors(TextEditor* editor) {
    if (editor->languages == nullptr)
        return;
    const PrKitPalette& palette = editor->kit->theme.palette;
    bool dark = palette.text1.r + palette.text1.g + palette.text1.b > palette.bg0.r + palette.bg0.g + palette.bg0.b;
    if (dark != editor->dark)
        editor->categoryColors.clear();
    editor->dark = dark;
    uint32_t categories = prSyntaxRegistryCategoryCount(editor->languages);
    for (uint32_t i = editor->categoryColors.count; i < categories; ++i) {
        PrStringView name = prSyntaxRegistryCategoryName(editor->languages, i);
        editor->categoryColors.push(textEditorStyleClassColor(textEditorStyleClassFromCategory(name), dark));
    }
}

void freeDocument(TextEditorDocument* document) {
    document->breakpoints.reset();
    document->sentLines.reset();
    prLspDocumentDestroy(document->lsp);
    if (document->mapping.data != nullptr)
        prFileUnmap(&document->mapping);
    prSyntaxTreeDestroy(document->syntax);
    prTextViewDestroy(document->view);
    prTextEditHistoryDestroy(document->history);
    prTextEditBufferDestroy(document->buffer);
    prRawStringReset(&document->path);
    document->source.reset();
    document->piece.reset();
    document->spans.reset();
    prMemoryFree(document);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextEditor : TextEditor {};
struct PrTextEditorDocument : TextEditorDocument {};
struct PrTextEditorDiff : TextEditorDiff {};

PrTextEditor* prTextEditorCreate(const PrTextEditorDesc* desc) {
    PrTextEditor* editor = allocZeroed<PrTextEditor>();
    editor->handle = editor;
    editor->kit = desc->kit;
    editor->ui = desc->kit->ui;
    editor->font = desc->font;
    prTextFontRetain(desc->font);
    editor->app = desc->app;
    editor->launcher = desc->launcher;
    editor->languages = desc->languages;
    editor->commands = desc->commands;
    editor->keymap = desc->keymap;
    editor->options = prTextEditorOptionsDefault();
    editor->port = kPrLayoutNodeNone;
    editor->holder = kPrLayoutNodeNone;
    editor->status = prRawStringEmpty();
    editor->statusShown = prRawStringEmpty();
    editor->hoverText = prRawStringEmpty();
    editor->dark = true;
    registerCommands(editor);
    return editor;
}

void prTextEditorDestroy(PrTextEditor* PR_NULLABLE editor) {
    if (editor == nullptr)
        return;
    stopDebugging(editor);
    prRawStringReset(&editor->debugger.program);
    prRawStringReset(&editor->debugger.adapterId);
    prRawStringReset(&editor->debugger.launch);
    for (uint32_t i = 0; i < editor->debugger.arguments.count; ++i)
        prRawStringReset(&editor->debugger.arguments.data[i]);
    editor->debugger.arguments.reset();
    editor->breakpointLines.reset();
    while (editor->diffs.count != 0)
        prTextEditorDiffDestroy(editor->diffs.last()->handle);
    editor->diffs.reset();
    for (uint32_t i = 0; i < editor->documents.count; ++i)
        freeDocument(editor->documents.data[i]);
    editor->documents.reset();
    for (uint32_t i = 0; i < editor->servers.count; ++i)
        textEditorServerDestroy(editor->servers.data[i]);
    editor->servers.reset();
    for (uint32_t i = 0; i < editor->configs.count; ++i) {
        TextEditorServerConfig& config = editor->configs.data[i];
        for (uint32_t k = 0; k < config.languages.count; ++k)
            prRawStringReset(&config.languages.data[k]);
        for (uint32_t k = 0; k < config.arguments.count; ++k)
            prRawStringReset(&config.arguments.data[k]);
        config.languages.reset();
        config.arguments.reset();
        prRawStringReset(&config.program);
    }
    editor->configs.reset();
    prTextEstimatorDestroy(editor->estimator);
    prTextFontRelease(editor->font);
    editor->categoryColors.reset();
    editor->lineSeverities.reset();
    prRawStringReset(&editor->status);
    prRawStringReset(&editor->statusShown);
    prRawStringReset(&editor->hoverText);
    prMemoryFree(editor);
}

PrTextEditorOptions prTextEditorOptionsDefault(void) {
    return PrTextEditorOptions{.wrap = true, .lineNumbers = true};
}

PrTextEditorOptions prTextEditorOptions(const PrTextEditor* editor) {
    return editor->options;
}

void prTextEditorSetOptions(PrTextEditor* editor, const PrTextEditorOptions* options) {
    editor->options = *options;
    if (editor->shown != nullptr)
        prTextViewReveal(editor->shown->view);
}

void prTextEditorAddServer(PrTextEditor* editor, const PrTextEditorServerDesc* desc) {
    TextEditorServerConfig config{};
    for (uint32_t i = 0; i < desc->languageCount; ++i)
        config.languages.push(prRawStringCopy(desc->languages[i]));
    config.program = prRawStringCopy(desc->program);
    for (uint32_t i = 0; i < desc->argumentCount; ++i)
        config.arguments.push(prRawStringCopy(desc->arguments[i]));
    editor->configs.push(config);
}

void prTextEditorFrame(PrTextEditor* editor, PrLayoutNode parent, PrVec2 extent) {
    PrLayoutTree* tree = prUiTree(editor->ui);
    const PrKitPalette& palette = editor->kit->theme.palette;
    for (uint32_t i = 0; i < editor->servers.count; ++i) {
        TextEditorServer* server = editor->servers.data[i];
        textEditorServerPump(server);
        if (!prRawStringIsEmpty(&server->message)) {
            setStatus(editor, prRawStringView(&server->message));
            prRawStringReplace(&server->message, "");
        }
    }
    for (uint32_t i = 0; i < editor->documents.count; ++i) {
        if (editor->documents.data[i]->lsp != nullptr)
            prLspDocumentSync(editor->documents.data[i]->lsp);
    }
    if (editor->debug != nullptr) {
        textEditorDebugPump(editor->debug);
        syncBreakpoints(editor);
        revealStop(editor);
    }

    PrLayoutBoxStyle holderBox = prLayoutBoxStyleDefault();
    holderBox.display = PrLayoutDisplay_Block;
    holderBox.position = PrLayoutPosition_Relative;
    holderBox.width = prLayoutLengthPx(extent.x);
    holderBox.height = prLayoutLengthPx(extent.y);
    PrLayoutNode holder = prLayoutTreeFrameElement(tree, parent, 0, record(tree, holderBox));
    editor->holder = holder;
    TextEditorDocument* document = editor->shown;
    if (document == nullptr) {
        editor->port = kPrLayoutNodeNone;
        return;
    }
    TextEditorDiff* diff = editor->diff;
    // A session's panel takes the bottom of the extent, and the text the rest.
    float panel = editor->debug != nullptr && extent.y > 2 * kDebugPanelHeight ? kDebugPanelHeight : 0;
    PrVec2 area{extent.x, extent.y - panel};
    loadPieces(document, kLoadMilliseconds);
    if (diff != nullptr)
        loadPieces(diff->old, kLoadMilliseconds);
    revealPending(document);
    updateCategoryColors(editor);
    if (document->syntax != nullptr && prSyntaxTreeUpdate(document->syntax, kParseMilliseconds))
        ++document->syntaxChanges;
    if (diff != nullptr && diff->old->syntax != nullptr && prSyntaxTreeUpdate(diff->old->syntax, kParseMilliseconds))
        ++diff->old->syntaxChanges;
    if (diff != nullptr && !diff->old->loading && !document->loading) {
        TextEditorDiffColors colors{};
        PrColor removed = palette.danger;
        PrColor added = palette.success;
        removed.a = 0.16f;
        added.a = 0.16f;
        colors.lines[TextEditorDiffSide_Old] = prColor16FromColor(removed);
        colors.lines[TextEditorDiffSide_New] = prColor16FromColor(added);
        removed.a = 0.36f;
        added.a = 0.36f;
        colors.bytes[TextEditorDiffSide_Old] = prColor16FromColor(removed);
        colors.bytes[TextEditorDiffSide_New] = prColor16FromColor(added);
        if (textEditorDiffModelUpdate(&diff->model, diff->old->buffer, document->buffer, colors, kLineHeight)) {
            for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side) {
                const RawArray<PrTextViewGap>& gaps = diff->model.sides[side].gaps;
                TextEditorDocument* shown = side == TextEditorDiffSide_Old ? diff->old : document;
                prTextViewSetGaps(shown->view, gaps.data, gaps.count);
            }
        }
    }

    bool vertical = editor->options.writingMode != PrLayoutWritingMode_HorizontalTb;
    float width = area.x - (vertical ? 0 : kScrollbarWidth);
    width = width > 0 ? width : 0;
    float height = area.y - (vertical ? kScrollbarWidth : 0);
    height = height > 0 ? height : 0;
    // The caret's place is of the last layout, which the view's rows of this frame replace.
    PrRect caret{};
    bool placed = prTextViewCaretRect(document->view, &caret);
    if (diff == nullptr) {
        editor->port = buildDocument(editor, document, holder, PrVec2{}, PrVec2{width, height}, nullptr);
    } else {
        // A comparison sets its sides horizontally, without wrap, so that each line of a side lies beside its own.
        float half = (area.x - kScrollbarWidth - kDiffDivider) / 2;
        half = half > 0 ? half : 0;
        DiffSideView sides[TextEditorDiffSide_Count] = {{diff, TextEditorDiffSide_Old}, {diff, TextEditorDiffSide_New}};
        // The shown document comes first, so that its element keeps its place, and its focus and scroll, whether or not
        // it is compared.
        editor->port = buildDocument(editor, document, holder, PrVec2{half + kDiffDivider, 0},
            PrVec2{half, area.y}, &sides[TextEditorDiffSide_New]);
        diff->oldPort = buildDocument(editor, diff->old, holder, PrVec2{}, PrVec2{half, area.y},
            &sides[TextEditorDiffSide_Old]);
    }
    buildScrollbar(editor, document, holder, area);
    if (placed)
        buildHover(editor, document, holder, caret);
    if (panel > 0)
        buildDebugPanel(editor, holder, area.y, PrVec2{extent.x, panel});
    bool pending = document->loading || (document->syntax != nullptr && prSyntaxTreeIsPending(document->syntax));
    pending = pending || (diff != nullptr && diff->old->loading);
    if (pending)
        prUiRequestMotionAt(editor->ui, editor->port, prUiCurrentRound(editor->ui)->time);
}

bool prTextEditorLaidOut(PrTextEditor* editor) {
    TextEditorDocument* document = editor->shown;
    if (document == nullptr)
        return false;
    bool again = prTextViewLaidOut(document->view);
    TextEditorDiff* diff = editor->diff;
    if (diff == nullptr)
        return prTextViewRefine(document->view, kRefineMilliseconds) || again || document->loading;
    again = prTextViewLaidOut(diff->old->view) || again;
    // The sides scroll together along both axes: whichever moved since the last layout leads, and the new side if both
    // did.
    TextEditorDocument* views[] = {diff->old, document};
    PrTextViewInfo infos[TextEditorDiffSide_Count];
    for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side)
        infos[side] = prTextViewInfo(views[side]->view);
    for (uint32_t axis = 0; axis < 2; ++axis) {
        double* last = axis == 0 ? diff->offsets : diff->inlineOffsets;
        double offsets[TextEditorDiffSide_Count];
        for (uint32_t side = 0; side < TextEditorDiffSide_Count; ++side)
            offsets[side] = axis == 0 ? infos[side].offset : infos[side].inlineOffset;
        bool oldMoved = offsets[TextEditorDiffSide_Old] != last[TextEditorDiffSide_Old];
        bool newMoved = offsets[TextEditorDiffSide_New] != last[TextEditorDiffSide_New];
        if (offsets[TextEditorDiffSide_Old] != offsets[TextEditorDiffSide_New] && (oldMoved || newMoved)) {
            TextEditorDiffSide leader = newMoved ? TextEditorDiffSide_New : TextEditorDiffSide_Old;
            TextEditorDiffSide follower =
                leader == TextEditorDiffSide_New ? TextEditorDiffSide_Old : TextEditorDiffSide_New;
            if (axis == 0)
                prTextViewScrollTo(views[follower]->view, offsets[leader]);
            else
                prTextViewScrollInlineTo(views[follower]->view, offsets[leader]);
            PrTextViewInfo followed = prTextViewInfo(views[follower]->view);
            offsets[follower] = axis == 0 ? followed.offset : followed.inlineOffset;
            again = true;
        }
        last[TextEditorDiffSide_Old] = offsets[TextEditorDiffSide_Old];
        last[TextEditorDiffSide_New] = offsets[TextEditorDiffSide_New];
    }
    return again || document->loading || diff->old->loading;
}

PrLayoutNode prTextEditorTextNode(const PrTextEditor* editor) {
    return editor->port;
}

PrStringView prTextEditorStatus(const PrTextEditor* editor) {
    PrTextEditor* e = const_cast<PrTextEditor*>(editor);
    if (!prRawStringIsEmpty(&e->status))
        return prRawStringView(&e->status);
    prRawStringReplace(&e->statusShown, "");
    const TextEditorDocument* document = e->shown;
    if (document == nullptr || document->lsp == nullptr)
        return prRawStringView(&e->statusShown);
    // The worst diagnostic that holds the caret.
    uint32_t caret = caretOf(document);
    uint32_t count = prLspDocumentDiagnosticCount(document->lsp);
    bool found = false;
    PrLspDiagnostic shown{};
    for (uint32_t i = 0; i < count; ++i) {
        PrLspDiagnostic diagnostic = prLspDocumentDiagnostic(document->lsp, i);
        if (caret < diagnostic.start || caret > diagnostic.end)
            continue;
        if (!found || severityRank(diagnostic.severity) > severityRank(shown.severity))
            shown = diagnostic;
        found = true;
    }
    if (found) {
        const char* kinds[] = {"", "error: ", "warning: ", "note: ", "hint: "};
        prRawStringAppend(&e->statusShown, kinds[shown.severity]);
        prRawStringAppend(&e->statusShown, shown.message);
    }
    return prRawStringView(&e->statusShown);
}

PrStatus prTextEditorDocumentCreate(PrTextEditor* editor, const PrTextEditorDocumentDesc* desc,
        PrTextEditorDocument* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrFileMapping mapping{};
    bool crlf = false;
    if (desc->path.data != nullptr) {
        if (!prFileMap(desc->path, &mapping))
            return PrStatus_NotFound;
        if (mapping.size > size_t(kPrTextEditBufferMaxBytes)) {
            prFileUnmap(&mapping);
            return PrStatus_Limit;
        }
        // A document records its line ending, and its buffer ends lines with a line feed.
        const char* bytes = reinterpret_cast<const char*>(mapping.data);
        for (size_t i = 0; i + 1 < mapping.size; ++i) {
            if (bytes[i] == '\n')
                break;
            if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
                crlf = true;
                break;
            }
        }
    } else if (desc->text.size > size_t(kPrTextEditBufferMaxBytes)) {
        return PrStatus_Limit;
    }
    PrTextEditorDocument* document = allocZeroed<PrTextEditorDocument>();
    document->handle = document;
    document->editor = editor;
    document->listed = true;
    document->path = desc->path.data != nullptr ? prRawStringCopy(desc->path) : prRawStringNull();
    document->crlf = crlf;
    document->buffer = prTextEditBufferCreate("");
    document->readOnly = desc->readOnly;
    if (desc->readOnly)
        prTextEditBufferSetPropose(document->buffer, refuseEdits, nullptr);
    document->history = prTextEditHistoryCreate(document->buffer);
    document->view = prTextViewCreate(document->buffer);
    document->savedVersion = prTextEditBufferVersion(document->buffer);
    document->spansVersion = UINT64_MAX;
    PrStringView name = desc->path.data != nullptr ? desc->path : desc->name;
    if (editor->languages != nullptr && name.size != 0)
        document->language = prSyntaxRegistryFromPath(editor->languages, name);
    if (document->language != nullptr)
        document->syntax = prSyntaxTreeCreate(document->language, editor->languages, document->buffer);
    document->mapping = mapping;
    if (desc->path.data == nullptr && desc->text.size != 0)
        document->source.append(desc->text.data, uint32_t(desc->text.size));
    document->loading = sourceOf(document).size != 0;
    editor->documents.push(document);
    if (!document->loading)
        attachServer(editor, document);
    if (editor->shown == nullptr)
        showDocument(editor, document);
    *out = document;
    return PrStatus_Ok;
}

void prTextEditorDocumentDestroy(PrTextEditorDocument* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    TextEditor* editor = document->editor;
    // A comparison does not outlive its documents.
    for (uint32_t i = editor->diffs.count; i-- > 0;) {
        TextEditorDiff* diff = editor->diffs.data[i];
        if (diff->young == document || diff->old == document)
            prTextEditorDiffDestroy(diff->handle);
    }
    editor->documents.removeFirst(document);
    if (editor->hoverDocument == document)
        editor->hoverDocument = nullptr;
    if (editor->definitionDocument == document)
        editor->definitionDocument = nullptr;
    if (editor->shown == document) {
        editor->shown = nullptr;
        if (editor->documents.count != 0)
            showDocument(editor, editor->documents.last());
    }
    freeDocument(document);
}

uint32_t prTextEditorDocumentCount(const PrTextEditor* editor) {
    return editor->documents.count;
}

PrTextEditorDocument* prTextEditorDocument(const PrTextEditor* editor, uint32_t index) {
    PR_ASSERT(index < editor->documents.count);
    return static_cast<PrTextEditorDocument*>(editor->documents.data[index]);
}

PrTextEditorDocument* PR_NULLABLE prTextEditorDocumentFromPath(const PrTextEditor* editor, PrStringView path) {
    return static_cast<PrTextEditorDocument*>(documentFromPath(editor, path));
}

PrTextEditorDocument* PR_NULLABLE prTextEditorShown(const PrTextEditor* editor) {
    return static_cast<PrTextEditorDocument*>(editor->shown);
}

void prTextEditorShow(PrTextEditor* editor, PrTextEditorDocument* document) {
    showDocument(editor, document);
}

PrTextEditBuffer* prTextEditorDocumentBuffer(const PrTextEditorDocument* document) {
    return document->buffer;
}

PrTextView* prTextEditorDocumentView(const PrTextEditorDocument* document) {
    return document->view;
}

PrTextEditHistory* prTextEditorDocumentHistory(const PrTextEditorDocument* document) {
    return document->history;
}

PrStringView prTextEditorDocumentPath(const PrTextEditorDocument* document) {
    return prRawStringView(&document->path);
}

const PrSyntaxLanguage* PR_NULLABLE prTextEditorDocumentLanguage(const PrTextEditorDocument* document) {
    return document->language;
}

PrTextEditorDocumentInfo prTextEditorDocumentInfo(const PrTextEditorDocument* document) {
    PrStringView source = sourceOf(document);
    PrTextEditorDocumentInfo info{
        .loading = document->loading,
        .loaded = document->loading && source.size != 0 ? float(double(document->loaded) / double(source.size)) : 1,
        .edited = prTextEditBufferVersion(document->buffer) != document->savedVersion,
        .crlf = document->crlf,
        .parsing = document->syntax != nullptr && prSyntaxTreeIsPending(document->syntax),
    };
    if (document->server != nullptr)
        info.server = textEditorServerState(document->server);
    if (document->lsp != nullptr) {
        uint32_t count = prLspDocumentDiagnosticCount(document->lsp);
        for (uint32_t i = 0; i < count; ++i) {
            PrLspSeverity severity = prLspDocumentDiagnostic(document->lsp, i).severity;
            info.errors += severity == PrLspSeverity_Error ? 1 : 0;
            info.warnings += severity == PrLspSeverity_Warning ? 1 : 0;
        }
    }
    return info;
}

PrStatus prTextEditorDocumentSave(PrTextEditorDocument* document) {
    return saveDocument(document);
}

PrTextEditorDiff* prTextEditorDiffCreate(PrTextEditor* editor, PrTextEditorDocument* old, PrTextEditorDocument* young) {
    PrTextEditorDiff* diff = allocZeroed<PrTextEditorDiff>();
    diff->handle = diff;
    diff->editor = editor;
    diff->old = old;
    diff->young = young;
    diff->oldPort = kPrLayoutNodeNone;
    textEditorDiffModelInit(&diff->model);
    editor->diffs.push(diff);
    showDiff(editor, diff);
    return diff;
}

void prTextEditorDiffDestroy(PrTextEditorDiff* PR_NULLABLE diff) {
    if (diff == nullptr)
        return;
    TextEditor* editor = diff->editor;
    editor->diffs.removeFirst(diff);
    if (editor->diff == diff)
        showDocument(editor, diff->young);
    // The documents keep the gaps that align them no longer.
    prTextViewSetGaps(diff->young->view, nullptr, 0);
    prTextViewSetGaps(diff->old->view, nullptr, 0);
    textEditorDiffModelReset(&diff->model);
    TextEditorDocument* old = diff->old;
    bool owned = diff->ownsOld;
    prMemoryFree(diff);
    if (owned)
        prTextEditorDocumentDestroy(old->handle);
}

PrTextEditorDiff* PR_NULLABLE prTextEditorShownDiff(const PrTextEditor* editor) {
    return editor->diff != nullptr ? editor->diff->handle : nullptr;
}

void prTextEditorShowDiff(PrTextEditor* editor, PrTextEditorDiff* diff) {
    showDiff(editor, diff);
}

uint32_t prTextEditorDiffChangeCount(const PrTextEditorDiff* diff) {
    return diff->model.changes.count;
}

PrTextDiffChange prTextEditorDiffChange(const PrTextEditorDiff* diff, uint32_t index) {
    PR_ASSERT(index < diff->model.changes.count);
    return diff->model.changes.data[index];
}

void prTextEditorSetDebugger(PrTextEditor* editor, const PrTextEditorDebuggerDesc* desc) {
    TextEditorDebugConfig& config = editor->debugger;
    prRawStringReplace(&config.program, desc->program);
    prRawStringReplace(&config.adapterId, desc->adapterId);
    prRawStringReplace(&config.launch, desc->launch);
    for (uint32_t i = 0; i < config.arguments.count; ++i)
        prRawStringReset(&config.arguments.data[i]);
    config.arguments.clear();
    for (uint32_t i = 0; i < desc->argumentCount; ++i)
        config.arguments.push(prRawStringCopy(desc->arguments[i]));
    editor->hasDebugger = true;
}

PrTextEditorDebugInfo prTextEditorDebugInfo(const PrTextEditor* editor) {
    if (editor->debug == nullptr)
        return PrTextEditorDebugInfo{.path = ""};
    PrTextEditorDebugInfo info{.state = textEditorDebugState(editor->debug), .path = ""};
    const TextEditorDebugFrame* frame = stoppedFrame(editor);
    if (frame != nullptr) {
        info.path = prRawStringView(&frame->path);
        info.line = frame->line;
    }
    return info;
}

bool prTextEditorDebugStart(PrTextEditor* editor) {
    return startDebugging(editor);
}

void prTextEditorDebugStop(PrTextEditor* editor) {
    stopDebugging(editor);
}

uint32_t prTextEditorDocumentBreakpoints(const PrTextEditorDocument* document, uint32_t* PR_NULLABLE out,
        uint32_t capacity) {
    RawArray<uint32_t> lines;
    breakpointLinesOf(const_cast<PrTextEditorDocument*>(document), lines);
    for (uint32_t i = 0; i < lines.count && i < capacity; ++i)
        out[i] = lines.data[i];
    uint32_t count = lines.count;
    lines.reset();
    return count;
}

void prTextEditorDocumentToggleBreakpoint(PrTextEditorDocument* document, uint32_t line) {
    toggleBreakpoint(document, line);
}
