// What an editor does with a key press, and the units of its text that it moves by and selects. Every editor over a
// text's bytes shares these, so that the keys and the motions mean one thing everywhere.

#pragma once

#include "pr/app.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What an editor moves by and selects in the bytes of its text. `at` is clamped to the text. Words are the word
// segments of UAX #29 that hold a letter or a number, with a run of Han or of Hiragana taken as one.

// Returns `at` moved back to the start of the code point that it is in.
uint32_t prTextEditCodePointStart(PrStringView text, uint32_t at);

// Returns the start of the line that holds `at`, after the line feed before it, or its end, at the line feed after it.
uint32_t prTextEditLineStart(PrStringView text, uint32_t at);
uint32_t prTextEditLineEnd(PrStringView text, uint32_t at);

// Returns the boundary of the grapheme cluster after or before `at`, or the end of the text.
uint32_t prTextEditClusterAfter(PrStringView text, uint32_t at);
uint32_t prTextEditClusterBefore(PrStringView text, uint32_t at);

// Returns where a Backspace at `at` starts to delete: the last code point of the grapheme cluster before it, so that a
// writer can correct a mark or a vowel. A cluster that means one thing only as a whole goes whole: an emoji sequence, a
// flag, a keycap, or a character with a variation selector.
uint32_t prTextEditBackspaceStart(PrStringView text, uint32_t at);

// Returns where a word motion from `at` ends: past what is not a word, and then past the word.
uint32_t prTextEditWordAfter(PrStringView text, uint32_t at);
uint32_t prTextEditWordBefore(PrStringView text, uint32_t at);

// The unit that a run of presses selects, and that a drag after it keeps whole.
enum {
    PrTextEditUnit_Character,
    PrTextEditUnit_Word,            // a word segment, within its line
    PrTextEditUnit_Line,
    PrTextEditUnit_All,
};
typedef uint8_t PrTextEditUnit;

// Writes the bytes of `unit` around `at`.
void prTextEditUnitAround(PrStringView text, uint32_t at, PrTextEditUnit unit, uint32_t* low, uint32_t* high);

// What a key press asks of the text being edited, by the platform's key bindings.
enum {
    PrTextEditCommand_None,

    // The arrows, as they lie on the screen: what each means depends on how lines lie (prTextEditMotionFromCommand). A
    // plain arrow moves the caret, or collapses the selection. The second group is with the word modifier, and the
    // third is with the modifier that goes to an edge.
    PrTextEditCommand_Left,
    PrTextEditCommand_Right,
    PrTextEditCommand_Up,
    PrTextEditCommand_Down,
    PrTextEditCommand_WordLeft,
    PrTextEditCommand_WordRight,
    PrTextEditCommand_WordUp,
    PrTextEditCommand_WordDown,
    PrTextEditCommand_EdgeLeft,
    PrTextEditCommand_EdgeRight,
    PrTextEditCommand_EdgeUp,
    PrTextEditCommand_EdgeDown,

    PrTextEditCommand_LineStart,         // of the visual line, in the paragraph's direction
    PrTextEditCommand_LineEnd,
    PrTextEditCommand_TextStart,
    PrTextEditCommand_TextEnd,
    PrTextEditCommand_DeleteBackward,    // prTextEditBackspaceStart to the caret
    PrTextEditCommand_DeleteForward,     // the caret to the end of its grapheme cluster
    PrTextEditCommand_DeleteWordBefore,
    PrTextEditCommand_DeleteWordAfter,
    PrTextEditCommand_InsertLineBreak,
    PrTextEditCommand_SelectAll,
    PrTextEditCommand_Copy,
    PrTextEditCommand_Cut,
    PrTextEditCommand_Paste,
    PrTextEditCommand_Undo,
    PrTextEditCommand_Redo,
};
typedef uint8_t PrTextEditCommand;

// Returns the text command of a KeyDown event, and writes whether a motion extends the selection to `extend`.
PrTextEditCommand prTextEditCommandFromKey(const PrAppEvent* event, bool* extend);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
