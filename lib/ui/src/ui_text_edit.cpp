// Text editing: the caret, the selection, and the edits that keys, typing, the pointer and the clipboard make.

#include "pr/ui_text_edit.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/mem.h"
#include "pr/text_edit.h"
#include "pr/text_edit_command.h"
#include "pr/text_edit_layout.h"
#include "pr/text_edit_motion.h"

namespace pr_impl {

namespace {

// The key under which an edit keeps its state on its control.
constexpr char kUiTextEditOwner = 0;

// What an edit keeps between rounds.
struct TextEditState {
    // The selection of an edit of a value. A view holds the selection of an edit of the caller's buffer.
    uint32_t anchor;
    uint32_t caret;

    PrTextEditBuffer* PR_NULLABLE buffer;   // holds a value being edited, from the first edit until focus leaves
    PrTextEditView* PR_NULLABLE view;       // of the caller's buffer

    // The text that an input method is composing, which a composition's next update replaces. Empty if none is.
    uint32_t markStart;
    uint32_t markEnd;

    // The unit that the press selected, around where the press landed.
    uint32_t unitLow;
    uint32_t unitHigh;
    PrTextEditUnit unit;

    PrLayoutTextAffinity affinity;  // where the caret stands at a soft wrap
    float column;                   // the x in viewport px that Up and Down keep, if `hasColumn`
    bool hasColumn;
    bool changed;                   // the last prUiTextEdit changed the text
    bool dragging;                  // a press is moving the caret
    bool focused;                   // the control had focus in the last round
    bool pasting;                   // a paste asked for the clipboard's text, which a Clipboard event brings
    bool reveal;                    // the next round scrolls the caret into view, once the layout shows it
    double activity;                // the time of the last move of the caret, from which it blinks
};

// The period of the caret's blink, in seconds, half of it shown.
constexpr double kBlinkSeconds = 1.0;

// Ends the editing of the text, and frees what held it.
void closeBuffer(TextEditState& state) {
    prTextEditViewDestroy(state.view);
    prTextEditBufferDestroy(state.buffer);
    state.view = nullptr;
    state.buffer = nullptr;
}

// Returns true if the buffer holds `value`, apart from the marked text, which stays inside the edit until it commits.
bool holdsValue(const TextEditState& state, PrStringView value) {
    PrStringView text = prTextEditBufferText(state.buffer);
    uint32_t marked = state.markEnd - state.markStart;
    if (text.size - marked != value.size)
        return false;
    return memEquals(text.data, value.data, state.markStart) &&
        memEquals(text.data + state.markEnd, value.data + state.markStart, text.size - state.markEnd);
}

// Returns the selection of `view`, which holds one because only the edit selects in it.
PrTextEditSelection selectionOf(const PrTextEditView* view) {
    return prTextEditViewSelections(view)[0];
}

void destroyState(void* state) {
    closeBuffer(*static_cast<TextEditState*>(state));
}

TextEditState& stateOf(PrUi* ui, PrLayoutNode control) {
    return *static_cast<TextEditState*>(
        prUiStateWithDestroy(ui, control, &kUiTextEditOwner, 0, sizeof(TextEditState), destroyState));
}

// An edit in the round being built: its description, its state, and the context.
struct TextEditor {
    PrUi* ui;
    PrLayoutTree* tree;
    const PrUiTextEditDesc& desc;
    PrLayoutNode node;              // the text node
    TextEditState& state;
    bool multiline;
    bool changed;
    bool moved;
    Array<char> scratch;

    // The selection, as this round moves it.
    uint32_t anchor;
    uint32_t caret;
    PrLayoutTextAffinity affinity;
    float column;
    bool hasColumn;

    PrStringView text() const {
        if (desc.buffer != nullptr)
            return prTextEditBufferText(desc.buffer);
        return state.buffer != nullptr ? prTextEditBufferText(state.buffer) : desc.value;
    }

    uint32_t low() const { return anchor < caret ? anchor : caret; }
    uint32_t high() const { return anchor < caret ? caret : anchor; }

    // Reads the selection from its owner: the view of the caller's buffer, or the state.
    void loadSelection() {
        if (desc.buffer == nullptr) {
            anchor = state.anchor;
            caret = state.caret;
            affinity = state.affinity;
            column = state.column;
            hasColumn = state.hasColumn;
            return;
        }
        PrTextEditSelection s = selectionOf(state.view);
        anchor = s.anchor;
        caret = s.caret;
        affinity = s.affinity;
        column = s.goal;
        hasColumn = s.hasGoal;
    }

    // Writes the selection to its owner.
    void storeSelection() {
        if (desc.buffer == nullptr) {
            state.anchor = anchor;
            state.caret = caret;
            state.affinity = affinity;
            state.column = column;
            state.hasColumn = hasColumn;
            return;
        }
        PrTextEditSelection s{.anchor = anchor, .caret = caret, .goal = column, .hasGoal = hasColumn, .affinity = affinity};
        prTextEditViewSetSelections(state.view, &s, 1);
    }

    // Moves the caret to `at`, and the anchor with it unless `extend` is true.
    void moveTo(uint32_t at, bool extend, PrLayoutTextAffinity toward = PrLayoutTextAffinity_Downstream) {
        caret = at;
        if (!extend)
            anchor = at;
        affinity = toward;
        hasColumn = false;
        moved = true;
    }

    // Replaces the bytes from `start` to `end` with `value`, and puts the caret after what the buffer took, which may
    // differ from `value`. A local replacement is one gesture.
    void replace(uint32_t start, uint32_t end, PrStringView value, PrTextEditOrigin origin, PrTextEditMerge merge) {
        if (start == end && value.size == 0)
            return;
        if (desc.buffer == nullptr && state.buffer == nullptr)
            state.buffer = prTextEditBufferCreate(desc.value);
        PrTextEditChange change{.start = start, .end = end, .text = value};
        bool local = origin == PrTextEditOrigin_Local;
        if (local)
            prUiEdit(ui, desc.control, PrUiEditPhase_Begin);
        bool applied = false;
        if (desc.buffer != nullptr) {
            storeSelection();
            applied = prTextEditViewApply(state.view, &change, 1, origin, merge);
        } else {
            applied = prTextEditBufferApply(state.buffer, &change, 1, origin);
        }
        if (local && applied)
            prUiEdit(ui, desc.control, PrUiEditPhase_Change);
        if (local)
            prUiEdit(ui, desc.control, PrUiEditPhase_Commit);
        if (!applied)
            return;
        state.markStart = state.markEnd = 0;
        if (desc.buffer != nullptr)
            loadSelection();
        moveTo(desc.buffer != nullptr ? caret : start + uint32_t(value.size), false);
        changed = true;
        state.changed = local;
    }

    // Replaces the selection with `value`. Line endings become line feeds, and each is dropped unless `lineFeeds` is
    // true and the edit is multiline. A tab stays in a multiline edit, and every other control character is dropped.
    void insert(PrStringView value, bool lineFeeds, PrTextEditOrigin origin, PrTextEditMerge merge) {
        scratch.clear();
        for (size_t i = 0; i < value.size; ++i) {
            char c = value.data[i];
            if (c == '\r' && i + 1 < value.size && value.data[i + 1] == '\n')
                continue;
            if (c == '\r')
                c = '\n';
            bool control = uint8_t(c) < 0x20 || c == 0x7f;
            if (!control || (c == '\n' && lineFeeds && multiline) || (c == '\t' && multiline))
                scratch.push(c);
        }
        replace(low(), high(), PrStringView{scratch.data, scratch.count}, origin, merge);
    }

    // Removes the selection, or the bytes between the caret and `at` if nothing is selected.
    void erase(uint32_t at, PrTextEditOrigin origin = PrTextEditOrigin_Local) {
        if (anchor != caret)
            replace(low(), high(), PrStringView{}, origin, PrTextEditMerge_None);
        else if (at != caret)
            replace(at < caret ? at : caret, at < caret ? caret : at, PrStringView{}, origin, PrTextEditMerge_Deleting);
    }

    // Returns the position nearest `point` in viewport px, and the affinity that puts the caret on the line under it.
    bool positionAt(PrVec2 point, uint32_t* out, PrLayoutTextAffinity* side) {
        uint32_t at = 0;
        if (!prLayoutTextOffsetNear(tree, node, prLayoutVecFromPx(point), &at, side))
            return false;
        *out = prTextEditCodePointStart(text(), at);
        return true;
    }

    // Writes the caret at `offset` in viewport px.
    bool caretAt(uint32_t offset, PrLayoutTextAffinity side, PrRect* out) {
        PrLayoutRect rect{};
        if (!prLayoutNodeTextCaret(tree, node, offset, side, PrLayoutCoordinates_Viewport, &rect))
            return false;
        *out = prLayoutRectToPx(rect);
        return true;
    }

    // Carries out a motion of the caret, and returns false if the edit does not take it.
    bool move(PrTextEditCommand command, bool extend) {
        PrStringView t = text();
        PrTextEditGeometry geometry = prTextEditGeometryFromLayoutText(tree, node, t);
        PrTextEditPosition at{node, caret, affinity};
        PrTextEditMotion motion = prTextEditMotionFromCommand(&geometry, at, command, desc.arrowMotion);
        bool between = motion == PrTextEditMotion_LinePrevious || motion == PrTextEditMotion_LineNext;
        if (motion != PrTextEditMotion_None && (multiline || !between)) {
            PrTextEditSelection s{
                .anchor = anchor,
                .caret = caret,
                .goal = column,
                .hasGoal = hasColumn,
                .affinity = affinity,
            };
            PrTextEditSelectionMove kind = extend ? PrTextEditSelectionMove_Extend : PrTextEditSelectionMove_Collapse;
            s = prTextEditSelectionMove(&geometry, node, s, motion, kind);
            anchor = s.anchor;
            caret = s.caret;
            affinity = s.affinity;
            column = s.goal;
            hasColumn = s.hasGoal;
            moved = true;
        }
        prTextEditGeometryReset(&geometry);
        return motion != PrTextEditMotion_None;
    }

    void key(const PrAppEvent& e) {
        PrStringView t = text();
        bool extend = false;
        PrTextEditCommand command = prTextEditCommandFromKey(&e, &extend);
        bool selected = anchor != caret;
        if (move(command, extend))
            return;
        switch (command) {
            case PrTextEditCommand_DeleteBackward: erase(prTextEditBackspaceStart(t, caret)); return;
            case PrTextEditCommand_DeleteForward: erase(prTextEditClusterAfter(t, caret)); return;
            case PrTextEditCommand_DeleteWordBefore: erase(prTextEditWordBefore(t, caret)); return;
            case PrTextEditCommand_DeleteWordAfter: erase(prTextEditWordAfter(t, caret)); return;
            case PrTextEditCommand_InsertLineBreak:
                if (multiline)
                    insert("\n", true, PrTextEditOrigin_Local, PrTextEditMerge_None);
                return;
            case PrTextEditCommand_SelectAll:
                anchor = 0;
                moveTo(uint32_t(t.size), true);
                return;
            case PrTextEditCommand_Copy:
            case PrTextEditCommand_Cut: {
                PrApp* app = prUiApp(ui);
                if (!selected)
                    return;
                if (app != nullptr)
                    prAppSetClipboardText(app, PrStringView{t.data + low(), high() - low()});
                if (command == PrTextEditCommand_Cut)
                    erase(caret);
                return;
            }
            case PrTextEditCommand_Paste:
                if (PrApp* app = prUiApp(ui)) {
                    prAppRequestClipboardText(app);
                    state.pasting = true;
                }
                return;
            default:
                return;
        }
    }

    void pointer(const PrAppEvent& e) {
        if (e.kind == PrAppEventKind_PointerUp) {
            state.dragging = false;
            return;
        }
        bool press = e.kind == PrAppEventKind_PointerDown;
        if ((press && e.button != 0) || (!press && !state.dragging))
            return;
        uint32_t at = 0;
        PrLayoutTextAffinity side = 0;
        if (!positionAt(e.position, &at, &side))
            return;
        PrStringView t = text();
        if (!press) {
            // A drag keeps whole the unit that the press selected.
            uint32_t low = 0;
            uint32_t high = 0;
            prTextEditUnitAround(t, at, state.unit, &low, &high);
            if (at < state.unitLow) {
                anchor = state.unitHigh;
                moveTo(low, true, side);
            } else {
                anchor = state.unitLow;
                moveTo(state.unit == PrTextEditUnit_Character ? at : high, true, side);
            }
            return;
        }
        state.dragging = true;
        if ((e.modifiers & PrAppModifierFlag_Shift) != 0) {
            state.unit = PrTextEditUnit_Character;
            state.unitLow = state.unitHigh = anchor;
            moveTo(at, true, side);
            return;
        }
        uint32_t presses = prUiPressCount(ui, 0);
        if (presses <= 1)
            state.unit = PrTextEditUnit_Character;
        else if (presses == 2)
            state.unit = PrTextEditUnit_Word;
        else if (presses == 3 && multiline)
            state.unit = PrTextEditUnit_Line;
        else
            state.unit = PrTextEditUnit_All;
        prTextEditUnitAround(t, at, state.unit, &state.unitLow, &state.unitHigh);
        anchor = state.unitLow;
        moveTo(state.unit == PrTextEditUnit_Character ? at : state.unitHigh, true, side);
    }

    // Replaces the text being composed, or the selection, with an update's text, and marks what it inserted. An update
    // with no text, and the end of the composition, remove the marked text. The text that a composition commits comes
    // after its end as a Text event.
    void compose(const PrAppEvent& e) {
        if (state.markEnd > state.markStart) {
            anchor = state.markStart;
            caret = state.markEnd;
        }
        uint32_t start = low();
        if (e.kind == PrAppEventKind_CompositionEnd) {
            erase(caret, PrTextEditOrigin_Composition);
            return;
        }
        insert(e.text, false, PrTextEditOrigin_Composition, PrTextEditMerge_None);
        if (caret > start) {
            state.markStart = start;
            state.markEnd = caret;
        }
    }

    // Claims the keys that the edit takes, so that they come to it before any node that holds it.
    void claim() {
        static const PrAppKey kMoves[] = {
            PrAppKey_Left, PrAppKey_Right, PrAppKey_Home, PrAppKey_End,
            PrAppKey_Backspace, PrAppKey_Delete,
        };
        static const PrAppModifierFlags kMoveModifiers[] = {
            0, PrAppModifierFlag_Alt, PrAppModifierFlag_Control, PrAppModifierFlag_Super,
        };
        for (PrAppKey key : kMoves) {
            for (PrAppModifierFlags modifiers : kMoveModifiers) {
                prUiKeyPressed(ui, desc.control, key, modifiers);
                prUiKeyPressed(ui, desc.control, key, modifiers | PrAppModifierFlag_Shift);
            }
        }
        // Up and Down run along the line where lines are vertical, even in a one-line edit.
        PrTextEditGeometry geometry = prTextEditGeometryFromLayoutText(tree, node, text());
        PrTextEditLine line{};
        bool vertical = geometry.line(geometry.context, PrTextEditPosition{node, caret, affinity},
            PrTextEditLineStep_Same, &line) && line.flow != PrTextEditFlow_HorizontalTb;
        prTextEditGeometryReset(&geometry);
        static const PrAppKey kLines[] = {PrAppKey_Up, PrAppKey_Down};
        for (uint32_t i = 0; (multiline || vertical) && i < 2; ++i) {
            for (PrAppModifierFlags modifiers : kMoveModifiers) {
                if (!vertical && modifiers != 0 && modifiers != PrAppModifierFlag_Super)
                    continue;
                prUiKeyPressed(ui, desc.control, kLines[i], modifiers);
                prUiKeyPressed(ui, desc.control, kLines[i], modifiers | PrAppModifierFlag_Shift);
            }
        }
        if (multiline)
            prUiKeyPressed(ui, desc.control, PrAppKey_Enter, 0);
        static const PrAppKey kCommands[] = {'A', 'C', 'X', 'V'};
        for (PrAppKey key : kCommands) {
            prUiKeyPressed(ui, desc.control, key, PrAppModifierFlag_Super);
            prUiKeyPressed(ui, desc.control, key, PrAppModifierFlag_Control);
        }
    }

    // Returns the text that the text node shows: the text, with a space after it if it is empty or ends in a line feed,
    // so that the line the caret stands on has text to place it.
    PrStringView shown() {
        PrStringView t = text();
        if (t.size != 0 && t.data[t.size - 1] != '\n')
            return t;
        scratch.clear();
        scratch.append(t.data, uint32_t(t.size));
        scratch.push(' ');
        return PrStringView{scratch.data, scratch.count};
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prUiTextEdit(PrUi* ui, const PrUiTextEditDesc* desc) {
    TextEditState& state = stateOf(ui, desc->control);
    TextEditor editor{
        .ui = ui,
        .tree = prUiTree(ui),
        .desc = *desc,
        .node = kPrLayoutNodeNone,
        .state = state,
        .multiline = (desc->flags & PrUiTextEditFlag_Multiline) != 0,
    };
    double time = prUiCurrentRound(ui)->time;
    bool disabled = prUiIsDisabled(ui, desc->control);
    bool focused = !disabled && prUiFocus(ui) == desc->control;
    state.changed = false;
    if (desc->buffer != nullptr) {
        if (state.view == nullptr || prTextEditViewBuffer(state.view) != desc->buffer) {
            closeBuffer(state);
            state.view = prTextEditViewCreate(desc->buffer);
            state.markStart = state.markEnd = 0;
        }
    } else if (state.view != nullptr || !focused) {
        closeBuffer(state);
    } else if (state.buffer != nullptr && !holdsValue(state, desc->value)) {
        prTextEditBufferSetText(state.buffer, desc->value);
        state.markStart = state.markEnd = 0;
    }
    PrStringView text = editor.text();
    editor.loadSelection();
    editor.anchor = prTextEditCodePointStart(text, editor.anchor);
    editor.caret = prTextEditCodePointStart(text, editor.caret);
    if (state.markEnd > text.size || state.markStart > state.markEnd)
        state.markStart = state.markEnd = 0;
    editor.node = prLayoutTreeFrameText(editor.tree, desc->parent, 0, editor.shown());

    // The layout that the last round's edits made is laid out now, so the caret can be brought into view.
    if (state.reveal) {
        state.reveal = false;
        PrLayoutRect rect{};
        if (prLayoutNodeTextCaret(editor.tree, editor.node, editor.caret, editor.affinity, PrLayoutCoordinates_Viewport, &rect)) {
            rect.right = rect.left + prLayoutUnitFromPx(1);
            prUiRevealRect(ui, editor.node, rect);
        }
    }

    if (focused) {
        editor.claim();
        PrUiEvents events = prUiEvents(ui, desc->control);
        bool pressed = false;
        for (uint32_t i = 0; i < events.count; ++i)
            pressed = pressed || events.events[i].kind == PrAppEventKind_PointerDown;
        if (!state.focused) {
            state.activity = time;
            if ((desc->flags & PrUiTextEditFlag_SelectOnFocus) != 0 && !pressed) {
                editor.anchor = 0;
                editor.caret = uint32_t(text.size);
            }
        }
        for (uint32_t i = 0; i < events.count; ++i) {
            const PrAppEvent& e = events.events[i];
            switch (e.kind) {
                case PrAppEventKind_PointerDown:
                case PrAppEventKind_PointerMove:
                case PrAppEventKind_PointerUp: editor.pointer(e); break;
                case PrAppEventKind_KeyDown: editor.key(e); break;
                case PrAppEventKind_Text:
                    if (state.markEnd > state.markStart) {
                        editor.anchor = state.markStart;
                        editor.caret = state.markEnd;
                    }
                    editor.insert(e.text, false, PrTextEditOrigin_Local, PrTextEditMerge_Typing);
                    break;
                case PrAppEventKind_Clipboard:
                    if (state.pasting)
                        editor.insert(e.text, true, PrTextEditOrigin_Local, PrTextEditMerge_None);
                    state.pasting = false;
                    break;
                case PrAppEventKind_CompositionUpdate:
                case PrAppEventKind_CompositionEnd: editor.compose(e); break;
                default: break;
            }
        }
    } else {
        state.dragging = false;
        state.pasting = false;
        // Marked text in the caller's buffer leaves with focus, since nothing will commit it.
        if (desc->buffer != nullptr && state.markEnd > state.markStart) {
            PrTextEditChange marked{.start = state.markStart, .end = state.markEnd};
            prTextEditBufferApply(desc->buffer, &marked, 1, PrTextEditOrigin_Composition);
            editor.loadSelection();
            editor.changed = true;
        }
        state.markStart = state.markEnd = 0;
    }
    state.focused = focused;
    if (editor.moved || editor.changed) {
        state.activity = time;
        state.reveal = true;
    }
    if (editor.changed)
        prLayoutTreeFrameSetText(editor.tree, editor.node, editor.shown());
    editor.scratch.reset();
    editor.storeSelection();

    // The selection shows while the control has focus, and the caret while it has focus and blinks on.
    if (focused && editor.anchor != editor.caret) {
        PrLayoutHighlight selection{.start = editor.low(), .end = editor.high(), .background = desc->selection};
        prLayoutTreeFrameSetHighlights(editor.tree, editor.node, &selection, 1);
    }
    if (focused) {
        PrRect inputRect{};
        if (editor.caretAt(editor.caret, editor.affinity, &inputRect))
            prUiSetTextInput(ui, inputRect);
        bool still = prUiUserPreferences(ui).reducedMotion;
        double phase = time - state.activity;
        bool on = still || phase - kBlinkSeconds * floorF64(phase / kBlinkSeconds) < kBlinkSeconds / 2;
        if (on && editor.anchor == editor.caret) {
            PrLayoutCaret shownCaret{
                .position = {editor.node, editor.caret},
                .affinity = editor.affinity,
                .color = desc->caret,
            };
            prLayoutTreeFrameSetCaret(editor.tree, &shownCaret);
        }
        if (!still)
            prUiRequestMotion(ui, desc->control);
    }
    return editor.node;
}

bool prUiTextEditChanged(PrUi* ui, PrLayoutNode control, PrStringView* text) {
    const TextEditState& state = stateOf(ui, control);
    if (!state.changed)
        return false;
    *text = prTextEditBufferText(state.view != nullptr ? prTextEditViewBuffer(state.view) : state.buffer);
    return true;
}

bool prUiTextEditIsComposing(PrUi* ui, PrLayoutNode control) {
    const TextEditState& state = stateOf(ui, control);
    return state.markEnd > state.markStart;
}

PrUiTextSelection prUiTextEditSelection(PrUi* ui, PrLayoutNode control) {
    const TextEditState& state = stateOf(ui, control);
    if (state.view != nullptr) {
        PrTextEditSelection s = selectionOf(state.view);
        return PrUiTextSelection{s.anchor, s.caret};
    }
    return PrUiTextSelection{state.anchor, state.caret};
}

void prUiTextEditSetSelection(PrUi* ui, PrLayoutNode control, PrUiTextSelection selection) {
    TextEditState& state = stateOf(ui, control);
    state.reveal = true;
    if (state.view != nullptr) {
        PrTextEditSelection s{.anchor = selection.anchor, .caret = selection.caret};
        prTextEditViewSetSelections(state.view, &s, 1);
        return;
    }
    state.anchor = selection.anchor;
    state.caret = selection.caret;
    state.affinity = PrLayoutTextAffinity_Downstream;
    state.hasColumn = false;
}
