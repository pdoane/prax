// A windowed text view in an immediate-mode frame: the pointer, typing, an input method's marked text and the caret's
// blink.

#include "pr/ui_text_view.h"

#include "pr/array.h"
#include "pr/text_edit.h"
#include "pr/text_edit_command.h"

namespace pr_impl {

namespace {

// The key under which a view keeps its state on its control.
constexpr char kUiTextViewOwner = 0;

// What a view keeps between rounds.
struct TextViewState {
    // The text that an input method is composing, which its next update replaces. Empty if none is.
    uint32_t markStart;
    uint32_t markEnd;

    // The unit that the press selected, around where the press landed.
    uint32_t unitLow;
    uint32_t unitHigh;
    PrTextEditUnit unit;

    bool dragging;                  // a press is moving the caret
    bool pasting;                   // a paste asked for the clipboard's text
    double activity;                // the time of the last change of the caret, from which it blinks
    uint32_t caret;                 // where the caret stood in the last round
};

TextViewState& stateOf(PrUi* ui, PrLayoutNode control) {
    return *static_cast<TextViewState*>(prUiState(ui, control, &kUiTextViewOwner, 0, sizeof(TextViewState)));
}

void select(PrTextView* view, uint32_t anchor, uint32_t caret, PrTextEditAffinity affinity) {
    PrTextEditSelection s{.anchor = anchor, .caret = caret, .affinity = affinity};
    prTextEditViewSetSelections(prTextViewEdit(view), &s, 1);
    prTextViewReveal(view);
}

void pointerEvent(PrUi* ui, PrTextView* view, TextViewState& state, const PrAppEvent& e) {
    if (e.kind == PrAppEventKind_PointerUp) {
        state.dragging = false;
        return;
    }
    bool press = e.kind == PrAppEventKind_PointerDown;
    if ((press && e.button != 0) || (!press && !state.dragging))
        return;
    uint32_t at = 0;
    PrTextEditAffinity side = 0;
    if (!prTextViewOffsetFromPoint(view, e.position, &at, &side))
        return;
    if (!press) {
        // A drag keeps whole the unit that the press selected.
        uint32_t low = 0;
        uint32_t high = 0;
        prTextViewUnitAround(view, at, state.unit, &low, &high);
        if (at < state.unitLow)
            select(view, state.unitHigh, low, side);
        else
            select(view, state.unitLow, state.unit == PrTextEditUnit_Character ? at : high, side);
        return;
    }
    if ((e.modifiers & PrAppModifierFlag_Alt) != 0) {
        prTextViewAddCaret(view, at, side);
        return;
    }
    state.dragging = true;
    if ((e.modifiers & PrAppModifierFlag_Shift) != 0) {
        uint32_t anchor = prTextEditViewPrimary(prTextViewEdit(view)).anchor;
        state.unit = PrTextEditUnit_Character;
        state.unitLow = state.unitHigh = anchor;
        select(view, anchor, at, side);
        return;
    }
    uint32_t presses = prUiPressCount(ui, 0);
    state.unit = PrTextEditUnit_Character;
    if (presses == 2)
        state.unit = PrTextEditUnit_Word;
    else if (presses == 3)
        state.unit = PrTextEditUnit_Line;
    else if (presses > 3)
        state.unit = PrTextEditUnit_All;
    prTextViewUnitAround(view, at, state.unit, &state.unitLow, &state.unitHigh);
    select(view, state.unitLow, state.unit == PrTextEditUnit_Character ? at : state.unitHigh, side);
}

// Inserts text from outside the program, with its line endings and tabs, and without its other control characters.
void insertOutside(PrTextView* view, PrStringView text, PrTextEditMerge merge) {
    Array<char> clean;
    clean.resize(uint32_t(text.size));
    clean.count = prTextEditInputNormalize(text, PrTextEditInputFlag_LineFeeds | PrTextEditInputFlag_Tabs, clean.data);
    prTextViewInsert(view, PrStringView{clean.data, clean.count}, merge);
    clean.reset();
}

// Replaces what an input method is composing, or the selection, with an update's text, and marks what it inserted. The
// text that a composition commits comes after its end as a Text event.
void composeEvent(PrTextView* view, TextViewState& state, const PrAppEvent& e) {
    PrTextEditView* edit = prTextViewEdit(view);
    if (state.markEnd > state.markStart)
        select(view, state.markStart, state.markEnd, PrTextEditAffinity_Downstream);
    PrTextEditSelection s = prTextEditViewPrimary(prTextViewEdit(view));
    uint32_t low = prTextEditSelectionStart(s);
    uint32_t high = prTextEditSelectionEnd(s);
    PrStringView text = e.kind == PrAppEventKind_CompositionEnd ? PrStringView{} : e.text;
    PrTextEditChange change{.start = low, .end = high, .text = text};
    prTextEditViewApply(edit, &change, 1, PrTextEditOrigin_Composition, PrTextEditMerge_None);
    state.markStart = low;
    state.markEnd = low + uint32_t(text.size);
    prTextViewReveal(view);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prUiTextView(PrUi* ui, const PrUiTextViewDesc* desc) {
    PrTextView* view = desc->view;
    TextViewState& state = stateOf(ui, desc->control);
    double time = prUiCurrentRound(ui)->time;
    bool focused = prUiFocus(ui) == desc->control;
    PrVec2 wheel{};
    if (prUiWheel(ui, desc->control, &wheel))
        prTextViewScrollBy(view, wheel);
    PrUiEvents events = prUiEvents(ui, desc->control);
    for (uint32_t i = 0; i < events.count; ++i) {
        const PrAppEvent& e = events.events[i];
        switch (e.kind) {
            case PrAppEventKind_PointerDown:
            case PrAppEventKind_PointerMove:
            case PrAppEventKind_PointerUp: pointerEvent(ui, view, state, e); break;
            case PrAppEventKind_Text:
                if (!focused)
                    break;
                if (state.markEnd > state.markStart) {
                    select(view, state.markStart, state.markEnd, PrTextEditAffinity_Downstream);
                    state.markStart = state.markEnd = 0;
                }
                insertOutside(view, e.text, PrTextEditMerge_Typing);
                break;
            case PrAppEventKind_Clipboard:
                if (state.pasting)
                    insertOutside(view, e.text, PrTextEditMerge_None);
                state.pasting = false;
                break;
            case PrAppEventKind_CompositionUpdate:
            case PrAppEventKind_CompositionEnd:
                if (focused)
                    composeEvent(view, state, e);
                break;
            default: break;
        }
    }
    uint32_t caret = prTextEditViewPrimary(prTextViewEdit(view)).caret;
    if (caret != state.caret) {
        state.caret = caret;
        state.activity = time;
    }
    bool still = prUiUserPreferences(ui).reducedMotion;
    PrTextEditCaretBlink blink = prTextEditCaretBlinkAt(state.activity, time);
    PrTextViewFrameDesc frame{
        .tree = prUiTree(ui),
        .parent = desc->control,
        .extent = desc->extent,
        .rowStyle = desc->rowStyle,
        .styles = desc->styles,
        .styleCount = desc->styleCount,
        .spanLayers = desc->spanLayers,
        .spanLayerCount = desc->spanLayerCount,
        .gutter = desc->gutter,
        .gutterContext = desc->gutterContext,
        .gutterSize = desc->gutterSize,
        .gutterBackground = desc->gutterBackground,
        .lineColor = desc->lineColor,
        .lineColorContext = desc->lineColorContext,
        .gapColor = desc->gapColor,
        .wrap = desc->wrap,
        .showCaret = focused,
        .caretHidden = !still && !blink.on,
        .selection = desc->selection,
        .caret = desc->caret,
        .arrowMotion = desc->arrowMotion,
    };
    prTextViewFrame(view, &frame);
    if (focused) {
        PrRect rect{};
        if (prTextViewCaretRect(view, &rect))
            prUiSetTextInput(ui, rect);
        PrTextEditView* edit = prTextViewEdit(view);
        bool anyCaret = false;
        for (uint32_t i = 0; i < prTextEditViewSelectionCount(edit); ++i) {
            const PrTextEditSelection& s = prTextEditViewSelections(edit)[i];
            anyCaret = anyCaret || s.anchor == s.caret;
        }
        if (!still && anyCaret)
            prUiRequestMotionAt(ui, desc->control, blink.next);
    }
}

void prUiTextViewPaste(PrUi* ui, PrLayoutNode control) {
    PrApp* app = prUiApp(ui);
    if (app == nullptr)
        return;
    prAppRequestClipboardText(app);
    stateOf(ui, control).pasting = true;
}
