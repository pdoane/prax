// A windowed text view in an immediate-mode frame: its rows, and what the pointer, typing and an input method do to it.
//
// Key commands are the caller's: an application binds them, and runs them through prTextViewRun.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/layout_tree.h"
#include "pr/text_view.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a view shows in a round.
typedef struct PrUiTextViewDesc {
    PrTextView* view;

    // The element that shows the rows and takes the input: a scroll container that is focusable and pressable, and
    // takes the wheel (PrUiFlag_Wheel). Its size is `extent` in px.
    PrLayoutNode control;
    PrVec2 extent;

    PrLayoutStyleIndex rowStyle;

    // By PrTextEditStyle: the records of the inline boxes that show the buffer's style runs (PrTextViewFrameDesc).
    const PrLayoutStyleIndex* PR_NULLABLE styles;
    uint32_t styleCount;

    // The colored spans of the window's text, such as syntax highlights (PrTextViewFrameDesc).
    PrTextViewSpansFn PR_NULLABLE spans;
    void* PR_NULLABLE spansContext;

    bool wrap;
    PrTextEditArrowMotion arrowMotion;
    PrColor16 selection;
    PrColor16 caret;
} PrUiTextViewDesc;

// Applies the input that the current round routed to the control, and declares the view's rows. A press with Alt adds a
// caret. Requests frames while a caret blinks. Call once in each round. After the frame's layout, the caller calls
// prTextViewLaidOut and prTextViewRefine, and requests another frame if either returns true.
void prUiTextView(PrUi* ui, const PrUiTextViewDesc* desc);

// Asks the application's clipboard for its text, which the view inserts when it arrives.
void prUiTextViewPaste(PrUi* ui, PrLayoutNode control);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
