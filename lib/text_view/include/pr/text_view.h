// The windowed view of a text buffer: a document of any size, shown as layout rows for the lines near the viewport.
//
// A row is a block for one logical line, and it may wrap to several visual lines. The rows in the window have exact
// heights, and the height of every other line is an estimate. The scroll position is anchored to a line of the text, so
// that a correction of an estimate does not move what the reader sees, and the cost of a frame follows the window and
// not the document. The view decides nothing about how the text looks: the caller gives it the records of its rows.
//
// Rows stack along the block axis of the rows' writing mode, and lines run along its inline axis. So a height, a scroll
// offset and the port's size below are along the block axis, which is horizontal in a vertical writing mode.
//
// A frame calls prTextViewFrame while it builds its tree, prTextViewLaidOut once the tree is laid out, and
// prTextViewRefine with the time that it can spare. One thread uses a view at a time.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/layout_tree.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_edit.h"
#include "pr/text_edit_command.h"
#include "pr/text_edit_motion.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A view of a buffer.
typedef struct PrTextView PrTextView;

// Creates a view of `buffer`, which must outlive the view, or frees one.
PrTextView* prTextViewCreate(PrTextEditBuffer* buffer);
void prTextViewDestroy(PrTextView* PR_NULLABLE view);

// The selections of the view. A caller may edit through it. The view follows every transaction of the buffer.
PrTextEditView* prTextViewEdit(const PrTextView* view);

// A range of the document that the caller paints, such as the span of a syntax highlight or of a diagnostic. A span
// paints as a highlight of the text's node (PrLayoutHighlight): its glyphs in `color`, its boxes filled with
// `background`, and a line under it in `underline`, each left out for zero alpha. So a span never changes how the text
// sets, and a change of spans costs no layout. A style that changes metrics, such as a weight, is a style run of the
// buffer.
typedef struct PrTextViewSpan {
    uint32_t start;
    uint32_t end;
    PrColor16 color;
    PrColor16 background;
    PrColor16 underline;
    PrDrawDecorationStyle underlineStyle;
} PrTextViewSpan;

// Writes the spans that intersect the bytes from `start` to `end` to `out`, up to `capacity`, and returns their number.
// The spans are in order and do not overlap. The call has no side effects on what later calls return.
typedef uint32_t (*PrTextViewSpansFn)(void* PR_NULLABLE context, uint32_t start, uint32_t end,
    PrTextViewSpan* PR_NULLABLE out, uint32_t capacity);

// A source of spans. The spans of one layer do not overlap, and layers paint in order, each over those before it, so
// that the spans of syntax and of diagnostics can cover the same text.
typedef struct PrTextViewSpanLayer {
    PrTextViewSpansFn fn;
    void* PR_NULLABLE context;
} PrTextViewSpanLayer;

// The most layers of spans that a frame shows.
enum { kPrTextViewMaxSpanLayers = 8 };

// Declares the content of the gutter cell of `line`, such as its number, under `cell`, in the frame of `tree`.
typedef void (*PrTextViewGutterFn)(void* PR_NULLABLE context, PrLayoutTree* tree, PrLayoutNode cell, uint32_t line);

// Returns the color that fills the row of `line` behind its text and its gutter, or zero alpha for none.
typedef PrColor16 (*PrTextViewLineColorFn)(void* PR_NULLABLE context, uint32_t line);

// What a frame shows.
typedef struct PrTextViewFrameDesc {
    PrLayoutTree* tree;

    // The element that shows the rows, a scroll container whose scroll offset the view sets. Its size is `extent` in
    // px, and its writing mode and direction are those of the rows.
    PrLayoutNode parent;
    PrVec2 extent;

    // The record of a row, a block whose font, writing mode, direction and white space lay out one line of text.
    PrLayoutStyleIndex rowStyle;

    // By PrTextEditStyle: the records of the inline boxes that show the buffer's style runs. A style at or past the
    // count shows as style zero. With no records, or on a line that is all of style zero, the row holds its text
    // directly. A style that changes a line's metrics changes the height of its row once the row is measured.
    const PrLayoutStyleIndex* PR_NULLABLE styles;
    uint32_t styleCount;

    // The layers of spans of the window's text, at most kPrTextViewMaxSpanLayers. The view asks each once in each
    // frame, for the window's range. The selection paints over them.
    const PrTextViewSpanLayer* PR_NULLABLE spanLayers;
    uint32_t spanLayerCount;

    // The gutter: a cell of `gutterSize` px along the inline axis at the inline start of each row, filled with
    // `gutterBackground`, which stays in place as lines scroll along it. The text wraps in the room that the gutter
    // leaves. With no function, rows have no gutter.
    PrTextViewGutterFn PR_NULLABLE gutter;
    void* PR_NULLABLE gutterContext;
    float gutterSize;
    PrColor16 gutterBackground;

    // The color of each line's row, or null for none.
    PrTextViewLineColorFn PR_NULLABLE lineColor;
    void* PR_NULLABLE lineColorContext;

    PrColor16 gapColor;             // what fills the view's gaps (prTextViewSetGaps)

    bool wrap;                     // the rows wrap at the width of the extent, which the estimates of heights assume
    bool showCaret;
    bool caretHidden;               // the shown carets keep their places and paint nothing

    PrColor16 selection;            // the background of selected text
    PrColor16 caret;
    PrTextEditArrowMotion arrowMotion;
} PrTextViewFrameDesc;

// Declares the rows of the window as children of the description's parent, with their highlights and carets, under a
// frame of the tree (pr/layout_frame.h).
void prTextViewFrame(PrTextView* view, const PrTextViewFrameDesc* desc);

// Measures the rows that the last layout placed, which makes their heights exact, and places the scroll offset by them.
// Returns true if another frame would show something different, because a row entered the window or an estimate moved.
bool prTextViewLaidOut(PrTextView* view);

// Improves the estimates of the heights of lines outside the window from their text, for about `milliseconds`. The
// scroll position holds its text. Returns true if more is left to improve, or if the estimates changed, so that a
// scrollbar has something new to show. A change of the width or the font starts the work again.
bool prTextViewRefine(PrTextView* view, double milliseconds);

// The scroll state along the block axis, in px, from estimates outside the window.
typedef struct PrTextViewInfo {
    double offset;                  // from the block start of the document to that of the viewport
    double inlineOffset;            // along the inline axis while lines do not wrap, as the port's scroll offset
    double documentSize;
    float portSize;
    uint32_t lineCount;
    uint32_t rowCount;              // in the window
    PrTextEditPoint caret;          // of the primary selection
} PrTextViewInfo;

// Returns the view's scroll state.
PrTextViewInfo prTextViewInfo(const PrTextView* view);

// Scrolls to the offset `offset` along the block axis, or by a physical distance along both axes. Each is clamped to
// the document. The inline axis scrolls only if lines do not wrap. If lines wrap in a vertical writing mode, the y
// distance also moves forward through the document.
void prTextViewScrollTo(PrTextView* view, double offset);
void prTextViewScrollBy(PrTextView* view, PrVec2 delta);

// Scrolls lines that do not wrap to `offset` along the inline axis, as prTextViewInfo gives it, clamped to the lines in
// the window once they are laid out.
void prTextViewScrollInlineTo(PrTextView* view, double offset);

// Brings the primary caret into view in the next frame.
void prTextViewReveal(PrTextView* view);

// Carries out a command of a key at every selection: a motion, whose `kind` says what happens to the anchor, a
// deletion, a line break or selecting all. Selections that meet join. The arrows run as the frame's arrow motion says.
// Returns false if the view does not carry out such a command, as with the clipboard and undo.
bool prTextViewRun(PrTextView* view, PrTextEditCommand command, PrTextEditSelectionMove kind);

// Moves each caret a viewport's size back, or forward, with the page's scroll, and keeps its goal along the line. Up is
// toward the start of the document.
enum {
    PrTextViewPage_Up,
    PrTextViewPage_Down,
};
typedef uint8_t PrTextViewPage;

// Carries out `page`, with the anchor as `kind` says.
void prTextViewRunPage(PrTextView* view, PrTextViewPage page, PrTextEditSelectionMove kind);

// Replaces each selection with `text`, whose line endings the caller has normalized, as a local transaction.
void prTextViewInsert(PrTextView* view, PrStringView text, PrTextEditMerge merge);

// Removes what the selections hold, as a local transaction.
void prTextViewEraseSelection(PrTextView* view);

// Returns the text that the selections hold, in the order of the document, with a line feed between selections. The
// caller releases it with prRawStringReset.
PrRawString prTextViewSelectedText(const PrTextView* view);

// Adds a caret on the visual line after or before the primary caret, at its goal along the line, which becomes the
// primary selection. Does nothing if there is no such line, or for PrTextEditLineStep_Same.
void prTextViewAddCaretOnLine(PrTextView* view, PrTextEditLineStep step);

// Adds a caret at `offset`, which becomes the primary selection, and joins any selection that holds it.
void prTextViewAddCaret(PrTextView* view, uint32_t offset, PrTextEditAffinity affinity);

// Drops every selection but the primary.
void prTextViewKeepPrimary(PrTextView* view);

// Writes the offset in the document nearest `point`, in viewport px, and the affinity that puts a caret on the visual
// line under it. Returns false if the last layout placed no row.
bool prTextViewOffsetFromPoint(PrTextView* view, PrVec2 point, uint32_t* offset, PrTextEditAffinity* affinity);

// Writes the bytes of the document of `unit` around `offset`.
void prTextViewUnitAround(PrTextView* view, uint32_t offset, PrTextEditUnit unit, uint32_t* low, uint32_t* high);

// Writes the primary caret in viewport px. Returns false if its row is not in the last layout.
bool prTextViewCaretRect(PrTextView* view, PrRect* out);

// Space along the block axis before a line, which shows no text, as a diff leaves where the other side has lines that
// this one does not. A gap at the line count is after the last line.
typedef struct PrTextViewGap {
    uint32_t line;
    float size;                     // in px
} PrTextViewGap;

// Replaces the view's gaps with `gaps`, which ascend by line, one to a line at most. Gaps are by line number, so an
// edit that adds or removes lines leaves them at their numbers until the caller sets them again. The scroll position
// holds its text.
void prTextViewSetGaps(PrTextView* view, const PrTextViewGap* PR_NULLABLE gaps, uint32_t count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
