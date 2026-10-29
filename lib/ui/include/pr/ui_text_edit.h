// Text editing: the caret and the selection in text that the caller stores, and the edits that the input routed to a
// control makes to that text. Component libraries build text fields and areas on it.
//
// The text shows in a text node of the layout tree, which the edit creates, and which the last layout placed. Positions
// in the text are byte offsets (PrLayoutTextPosition). The pointer and the keys move the caret by what the last layout
// shows: a press places the caret at the position under the pointer, and Up and Down move it between the lines as they
// wrap. The layout tree paints the selection as a highlight of the text node and the caret as the tree's caret, for the
// frame that the edit runs in (prLayoutTreeFrameSetHighlights, prLayoutTreeFrameSetCaret).
//
// Keys follow one map on every platform. Left and Right move by a grapheme cluster, with Option or Control by a word,
// and with Command to the ends of the line. Home and End move to the ends of the line, and with Command or Control to
// the ends of the text. Up and Down move between lines, and with Command to the ends of the text. Shift extends the
// selection with each of them. Backspace and Delete remove the selection, or the grapheme cluster or, with Option or
// Control, the word beside the caret. Command or Control with A selects the whole text, and with X, C and V cuts,
// copies and pastes through the application's clipboard (prUiSetApp).

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The text that an edit changes. `read` returns its bytes, which stay valid until the next call of either function, and
// `replace` replaces the bytes from `start` to `end` with `value`. The storage is the caller's.
typedef struct PrUiTextBuffer {
    void* PR_NULLABLE context;
    PrStringView (*read)(void* PR_NULLABLE context);
    void (*replace)(void* PR_NULLABLE context, uint32_t start, uint32_t end, PrStringView value);
} PrUiTextBuffer;

// How an edit treats its text.
enum {
    PrUiTextEditFlag_Multiline = 1u << 0,       // Return inserts a line feed, and Up and Down move between lines
    PrUiTextEditFlag_SelectOnFocus = 1u << 1,   // focus that comes other than by a press selects the whole text
};
typedef uint8_t PrUiTextEditFlags;

// What an edit changes and where it shows.
typedef struct PrUiTextEditDesc {
    // The node that takes the edit's input, which is focusable and pressable, and the element in it that the edit
    // creates the text node in, as that element's next child. Both are created in the current round.
    PrLayoutNode control;
    PrLayoutNode parent;

    PrUiTextBuffer buffer;
    PrUiTextEditFlags flags;

    // The background of the selected text and the color of the caret. The selected text keeps its color.
    PrColor16 selection;
    PrColor16 caret;
} PrUiTextEditDesc;

// The selection of an edit: the bytes between the anchor, where it started, and the caret, where it ends. The caret is
// at the anchor if nothing is selected.
typedef struct PrUiTextSelection {
    uint32_t anchor;
    uint32_t caret;
} PrUiTextSelection;

// Creates the text node that shows the edit's text, applies the input that the current round routed to the edit's
// control, and shows its selection and caret while the control has focus. Each change of the text is a gesture of its
// own (prUiEdit). Requests frames while the caret blinks. Returns the text node. Call once in each round for each edit.
// The edit keeps its selection between rounds while it is called in each round.
//
// The text node holds the text and, if the text is empty or ends in a line feed, a space after it, so that the line
// that the caret stands on has text to place it.
PrLayoutNode prUiTextEdit(PrUi* ui, const PrUiTextEditDesc* desc);

// Returns the selection of the edit of `control`, or sets it, clamped to the text in the next prUiTextEdit.
PrUiTextSelection prUiTextEditSelection(PrUi* ui, PrLayoutNode control);
void prUiTextEditSetSelection(PrUi* ui, PrLayoutNode control, PrUiTextSelection selection);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
