// Text editing: an edited text, the views that select in it, and the record of its changes.
//
// A buffer holds UTF-8 text and an index of its lines. Every change is a transaction that replaces byte ranges, and it
// reaches everything that depends on the text: anchors, the selections of each view, the edit streams, and the history.
// Nothing here knows how the text is shown.
//
// One thread uses a buffer and everything attached to it at a time. A buffer may be destroyed before what is attached
// to it. A view, stream or history of a destroyed buffer stays safe to destroy and to read, and it reads as empty.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

// An edited text.
typedef struct PrTextEditBuffer PrTextEditBuffer;

// A line of the text and a byte offset within that line. A line starts after a line feed, and its line feed is its last
// byte.
typedef struct PrTextEditPoint {
    uint32_t line;
    uint32_t column;
} PrTextEditPoint;

// The most bytes that a buffer holds. A caller checks text from outside the program against it.
enum { kPrTextEditBufferMaxBytes = 0x7fffffff };

// Creates a buffer that holds `text`, which is at most kPrTextEditBufferMaxBytes, or frees one.
PrTextEditBuffer* prTextEditBufferCreate(PrStringView text);
void prTextEditBufferDestroy(PrTextEditBuffer* PR_NULLABLE buffer);

// The size of the text in bytes.
uint32_t prTextEditBufferSize(const PrTextEditBuffer* buffer);

// The version of the text, which every transaction that changes the text increases.
uint64_t prTextEditBufferVersion(const PrTextEditBuffer* buffer);

// Returns the contiguous bytes that start at `offset`, which may end before the end of the text. A reader continues
// from the end of the result. The result is empty at the end of the text. It is valid until the text changes.
PrStringView prTextEditBufferChunk(const PrTextEditBuffer* buffer, uint32_t offset);

// Copies the bytes from `start` to `end` to `out`, which holds `end - start` bytes.
void prTextEditBufferCopy(const PrTextEditBuffer* buffer, uint32_t start, uint32_t end, char* PR_NULLABLE out);

// Returns the whole text as contiguous bytes, valid until the text changes. This costs a move of bytes up to the size
// of the text if an edit was made since the last call. It is for text that is shown whole. A reader of a large document
// uses prTextEditBufferChunk or prTextEditBufferCopy.
PrStringView prTextEditBufferText(PrTextEditBuffer* buffer);

// The number of lines, which is at least one. Text that ends in a line feed has an empty last line.
uint32_t prTextEditBufferLineCount(const PrTextEditBuffer* buffer);

// The offset of the first byte of `line`, which must be less than the number of lines.
uint32_t prTextEditBufferLineStart(const PrTextEditBuffer* buffer, uint32_t line);

// The offset of the end of the content of `line`: its line feed, or the end of the text for the last line.
uint32_t prTextEditBufferLineEnd(const PrTextEditBuffer* buffer, uint32_t line);

// Returns the point of `offset`, which must not be past the end of the text. The cost is logarithmic in the number of
// lines.
PrTextEditPoint prTextEditBufferPointFromOffset(const PrTextEditBuffer* buffer, uint32_t offset);

// Returns the offset of `point`, whose line must exist. A column past the end of the line's content stands at that end.
uint32_t prTextEditBufferOffsetFromPoint(const PrTextEditBuffer* buffer, PrTextEditPoint point);

// ---------------------------------------------------------------------------------------------------------------------
// Transactions

// The replacement of the bytes from `start` to `end` with `text`.
typedef struct PrTextEditChange {
    uint32_t start;
    uint32_t end;
    PrStringView text;
} PrTextEditChange;

// Where a transaction comes from, which decides what else it does. A local transaction can be proposed against and
// enters the history. A composition is an input method's marked text, which can be proposed against and stays out of
// the history. An undo comes from a history. A remote or program transaction is not proposed against, and it empties
// the history, because the positions that the history holds no longer describe the text. A load appends text that
// arrives at the end of the text, as a file that streams in does: it is not proposed against, and it keeps the history,
// since text after every position that the history holds moves none of them. A load's changes insert at the end of the
// text.
enum {
    PrTextEditOrigin_Local,
    PrTextEditOrigin_Composition,
    PrTextEditOrigin_Undo,
    PrTextEditOrigin_Remote,
    PrTextEditOrigin_Program,
    PrTextEditOrigin_Load,
};
typedef uint8_t PrTextEditOrigin;

// Asked before a local or composition transaction applies, with the changes that it would make. The function may
// replace the text of a change, and it may narrow a change's range. The text that it gives must stay valid until the
// transaction has applied. Returns false to reject the transaction. Rejecting leaves the text and every selection as
// they were.
typedef bool (*PrTextEditProposeFn)(void* PR_NULLABLE context, PrTextEditOrigin origin, PrTextEditChange* changes,
    uint32_t count);

// Sets the function that is asked before each proposed transaction, or none for null.
void prTextEditBufferSetPropose(PrTextEditBuffer* buffer, PrTextEditProposeFn PR_NULLABLE fn, void* PR_NULLABLE context);

// Applies `count` changes as one transaction. The changes are in ascending order of position in the text before the
// transaction, they do not overlap, and they lie in the text. Applying moves only the bytes and line starts between the
// last edit and the changes, so a run of nearby edits does not cost in proportion to the size of the text. Returns
// false if the transaction was rejected (PrTextEditProposeFn) or would pass kPrTextEditBufferMaxBytes.
bool prTextEditBufferApply(PrTextEditBuffer* buffer, const PrTextEditChange* PR_NULLABLE changes, uint32_t count,
    PrTextEditOrigin origin);

// Replaces the whole text with `text`, as a program transaction.
void prTextEditBufferSetText(PrTextEditBuffer* buffer, PrStringView text);

// ---------------------------------------------------------------------------------------------------------------------
// Text from outside the program

// What of text from outside the program an edit keeps, besides its printable text.
enum {
    PrTextEditInputFlag_LineFeeds = 1 << 0,     // its line endings, each a line feed
    PrTextEditInputFlag_Tabs = 1 << 1,
    PrTextEditInputFlag_Controls = 1 << 2,      // the other control characters
};
typedef uint8_t PrTextEditInputFlags;

// Writes `text` to `out` as a buffer takes it, and returns the number of bytes written, which is at most `text.size`.
// CR LF and a lone CR are line endings. A line ending, a tab or another control character that `keep` does not name is
// dropped. `out` holds `text.size` bytes. A caller that writes a text in pieces does not end a piece between a CR and
// its LF.
uint32_t prTextEditInputNormalize(PrStringView text, PrTextEditInputFlags keep, char* PR_NULLABLE out);

// ---------------------------------------------------------------------------------------------------------------------
// The edit stream

// One replacement of a transaction, as an incremental consumer of the text needs it. Replacements are given in the
// order in which they applied, so the positions of each are correct in the text as the replacements before it left it.
// The old end's point is in the text before the replacement, and the new end's point is in the text after it.
typedef struct PrTextEditRecord {
    uint64_t version;               // the version of the text after the record's transaction
    uint32_t start;
    uint32_t oldEnd;
    uint32_t newEnd;
    PrTextEditPoint startPoint;
    PrTextEditPoint oldEndPoint;
    PrTextEditPoint newEndPoint;
    PrStringView text;              // the new text, so that a replica needs no access to the buffer
    PrTextEditOrigin origin;
} PrTextEditRecord;

// A consumer's position in the changes of a buffer. A buffer keeps each record until every stream has read it, so each
// consumer can read at its own pace. A buffer without streams keeps no records.
typedef struct PrTextEditStream PrTextEditStream;

// Creates a stream that starts at the buffer's current version, or frees one.
PrTextEditStream* prTextEditStreamCreate(PrTextEditBuffer* buffer);
void prTextEditStreamDestroy(PrTextEditStream* PR_NULLABLE stream);

// Removes the stream's oldest records and writes them to `out`, up to `capacity`. Returns the number that it wrote, and
// is called until it writes none. The text of a record is valid until the next take by any stream of the buffer, or
// until the buffer changes.
uint32_t prTextEditStreamTake(PrTextEditStream* stream, PrTextEditRecord* out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Anchors

// A position in the text that follows its text through changes.
typedef uint32_t PrTextEditAnchor;

// No anchor. No live or removed anchor has this value.
enum { kPrTextEditAnchorNone = 0 };

// Which way an anchor at the start of a replaced range goes. Left stays at the start of the new text, and Right moves
// to its end. A range uses a Right anchor for its start and a Left anchor for its end, so that text inserted at either
// boundary stays outside it.
enum {
    PrTextEditBias_Left,
    PrTextEditBias_Right,
};
typedef uint8_t PrTextEditBias;

// Creates an anchor at `offset`, or frees one. A removed anchor's value can be reused.
PrTextEditAnchor prTextEditAnchorCreate(PrTextEditBuffer* buffer, uint32_t offset, PrTextEditBias bias);
void prTextEditAnchorRemove(PrTextEditBuffer* buffer, PrTextEditAnchor anchor);

// The offset of the anchor. A change that starts after the anchor leaves it in place, and a change that ends at or
// before the anchor moves it by the change in length. A change whose range holds the anchor strictly inside moves it to
// the start of the new text.
uint32_t prTextEditAnchorOffset(const PrTextEditBuffer* buffer, PrTextEditAnchor anchor);

// Returns true unless a change replaced a range that held the anchor strictly inside. An anchor that stops being live
// never becomes live again, and its offset continues to follow the text.
bool prTextEditAnchorIsLive(const PrTextEditBuffer* buffer, PrTextEditAnchor anchor);

// ---------------------------------------------------------------------------------------------------------------------
// Views

// One user's selections in a buffer. Views of one buffer edit the same text and keep their own selections.
typedef struct PrTextEditView PrTextEditView;

// Which side of a position a caret stands on, if one offset has two places in what is shown, as at a soft wrap. A
// downstream caret stands with the text after it, and an upstream caret with the text before it.
enum {
    PrTextEditAffinity_Downstream,
    PrTextEditAffinity_Upstream,
};
typedef uint8_t PrTextEditAffinity;

// The bytes between the anchor, where the selection started, and the caret, where it ends. The caret is at the anchor
// if nothing is selected. Comparing the two gives the direction of the selection.
typedef struct PrTextEditSelection {
    uint32_t anchor;
    uint32_t caret;

    // The position along the line that vertical motion keeps, in the units of whatever shows the text. It is along the
    // inline axis, so that it holds in every writing mode. It is used if `hasGoal` is true.
    float goal;
    bool hasGoal;

    PrTextEditAffinity affinity;
} PrTextEditSelection;

// How a local transaction joins the history's last step. The caller says which run of edits a gesture belongs to, and
// the history joins it if it continues the last step from where that step left the selections.
enum {
    PrTextEditMerge_None,
    PrTextEditMerge_Typing,     // an insertion at the end of the text that the last step inserted
    PrTextEditMerge_Deleting,   // a deletion next to the text that the last step deleted
};
typedef uint8_t PrTextEditMerge;

// Returns the first or the last offset of what `selection` holds, in the order of the text, whichever way the selection
// runs.
uint32_t prTextEditSelectionStart(PrTextEditSelection selection);
uint32_t prTextEditSelectionEnd(PrTextEditSelection selection);

// Creates a view with one caret at the start of the text, or frees one.
PrTextEditView* prTextEditViewCreate(PrTextEditBuffer* buffer);
void prTextEditViewDestroy(PrTextEditView* PR_NULLABLE view);

// The buffer of the view, or null if the buffer was destroyed.
PrTextEditBuffer* PR_NULLABLE prTextEditViewBuffer(const PrTextEditView* view);

// The view's selections, which are at least one. They do not overlap. They are in ascending order, except that the last
// is the primary selection, which motion and scrolling follow. The pointer is valid until the view or its buffer
// changes.
uint32_t prTextEditViewSelectionCount(const PrTextEditView* view);
const PrTextEditSelection* prTextEditViewSelections(const PrTextEditView* view);

// Returns the view's primary selection.
PrTextEditSelection prTextEditViewPrimary(const PrTextEditView* view);

// Replaces the view's selections, with the last as primary. Offsets are clamped to the text. Selections that overlap
// join, and the joined selection is primary if either was.
void prTextEditViewSetSelections(PrTextEditView* view, const PrTextEditSelection* selections, uint32_t count);

// Applies `count` changes as prTextEditBufferApply does, and records the view's selections in the history. A selection
// of this view that touches a change moves to the end of the change's new text, and loses its goal and its affinity.
// This is where a caret lands after the view makes an edit. The selections of other views follow the rules of anchors.
// Returns false if the transaction was rejected.
bool prTextEditViewApply(PrTextEditView* view, const PrTextEditChange* PR_NULLABLE changes, uint32_t count,
    PrTextEditOrigin origin, PrTextEditMerge merge);

// Replaces each selection with `text`, as one transaction. Returns false if the transaction was rejected.
bool prTextEditViewInsert(PrTextEditView* view, PrStringView text, PrTextEditOrigin origin, PrTextEditMerge merge);

// ---------------------------------------------------------------------------------------------------------------------
// Styles

// What a range of the text looks like, as a value that means something only to whatever shows the text. Zero is the
// style of text that nothing has styled.
typedef uint32_t PrTextEditStyle;

// A style from `start` up to the next run's start, or the end of the text. The first run starts at zero, and two runs
// in a row differ.
typedef struct PrTextEditStyleRun {
    uint32_t start;
    PrTextEditStyle style;
} PrTextEditStyleRun;

// The buffer's style runs, valid until the buffer changes. A buffer whose text is all of style zero has none, and costs
// nothing for them. The runs follow every transaction. Inserted text takes the style of what it replaces, or of the
// text before it, or at the start of the text, of the text after it. The cost of an edit is proportional to the number
// of runs.
uint32_t prTextEditBufferStyleRunCount(const PrTextEditBuffer* buffer);
const PrTextEditStyleRun* PR_NULLABLE prTextEditBufferStyleRuns(const PrTextEditBuffer* buffer);

// Returns the style of the byte at `offset`, or at the end of the text, of its last byte.
PrTextEditStyle prTextEditBufferStyleAt(const PrTextEditBuffer* buffer, uint32_t offset);

// Sets the style of the bytes from `start` to `end`, outside the history. This is for text that a program loads.
void prTextEditBufferSetStyle(PrTextEditBuffer* buffer, uint32_t start, uint32_t end, PrTextEditStyle style);

// Sets the style of what each of the view's selections holds, as one step of the history. If no selection holds text,
// it sets the style that the view's next local insertion takes, until the view's selections move.
void prTextEditViewSetStyle(PrTextEditView* view, PrTextEditStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// History

// The steps of a buffer's local transactions, which can be undone and redone. The owner of the model that the buffer
// edits owns the history. A buffer has at most one history.
typedef struct PrTextEditHistory PrTextEditHistory;

// Creates a history of `buffer`, which has none, or frees one.
PrTextEditHistory* prTextEditHistoryCreate(PrTextEditBuffer* buffer);
void prTextEditHistoryDestroy(PrTextEditHistory* PR_NULLABLE history);

// Returns true if there is a step to undo or to redo.
bool prTextEditHistoryCanUndo(const PrTextEditHistory* history);
bool prTextEditHistoryCanRedo(const PrTextEditHistory* history);

// Reverts or repeats the last step, and gives `view` the selections from before or after that step. Returns false if
// there is no such step.
bool prTextEditHistoryUndo(PrTextEditHistory* history, PrTextEditView* PR_NULLABLE view);
bool prTextEditHistoryRedo(PrTextEditHistory* history, PrTextEditView* PR_NULLABLE view);

// Ends the last step, so that the next transaction starts a step of its own.
void prTextEditHistoryBreak(PrTextEditHistory* history);

// Removes every step.
void prTextEditHistoryClear(PrTextEditHistory* history);

// ---------------------------------------------------------------------------------------------------------------------
// Caret

// The part of its blink that a caret stands in.
typedef struct PrTextEditCaretBlink {
    bool on;                        // true if the caret shows
    double next;                    // when the caret next turns on or off, on the clock of the call's times
} PrTextEditCaretBlink;

// The blink of a caret at `time`, in seconds, whose last move was at `activity`. It shows from the move, and through
// any time before the move.
PrTextEditCaretBlink prTextEditCaretBlinkAt(double activity, double time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
