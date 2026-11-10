// Text editing: the caret, the selection, and the edits that keys, typing, the pointer and the clipboard make.

#include "pr/ui_text_edit.h"

#include "pr/assert.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/text_edit.h"
#include "pr/text_edit_command.h"
#include "pr/text_edit_layout.h"
#include "pr/text_edit_motion.h"

namespace pr_impl {

namespace {

// The key under which an edit keeps its state on its control.
constexpr char kUiTextEditOwner = 0;

// The character that a masked edit shows for each code point, U+2022 BULLET.
constexpr PrStringView kMask = "\xE2\x80\xA2";

// Returns true if `byte` begins a code point in UTF-8.
bool startsCodePoint(char byte) {
    return (uint8_t(byte) & 0xC0) != 0x80;
}

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
    bool reveal;                    // the next round scrolls the caret into view, once the layout shows it
    double activity;                // the time of the last move of the caret, from which it blinks
};

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
    return prMemEquals(text.data, value.data, state.markStart) &&
        prMemEquals(text.data + state.markEnd, value.data + state.markStart, text.size - state.markEnd);
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
    PrLayoutNode node;                    // the first text node
    RawArray<PrTextEditLayoutRun> pieces; // the text nodes, each with where its text starts in the text
    RawArray<PrLayoutNode> boxes;         // the inline box of each of `pieces`, if the text has style runs
    TextEditState& state;
    bool multiline;
    bool masked;                          // a password's edit, which shows a mask in place of each code point
    bool changed;
    bool moved;
    RawArray<char> scratch;
    RawArray<char> mask;                  // the text's mask, if `masked`

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

    // Converts an offset in the text to the offset in the text node that shows it, and back. They differ only in a
    // masked edit.
    uint32_t shownOffset(uint32_t offset) const {
        if (!masked)
            return offset;
        PrStringView t = text();
        uint32_t points = 0;
        for (uint32_t i = 0; i < offset && i < t.size; ++i)
            points += startsCodePoint(t.data[i]) ? 1 : 0;
        return points * uint32_t(kMask.size);
    }
    uint32_t textOffset(uint32_t offset) const {
        if (!masked)
            return offset;
        PrStringView t = text();
        uint32_t points = offset / uint32_t(kMask.size);
        uint32_t i = 0;
        for (; i < t.size; ++i) {
            if (startsCodePoint(t.data[i]) && points-- == 0)
                break;
        }
        return i;
    }

    // Returns the text, or its mask in a masked edit.
    PrStringView shownText() {
        PrStringView t = text();
        if (!masked)
            return t;
        mask.clear();
        for (uint32_t i = 0; i < t.size; ++i) {
            if (startsCodePoint(t.data[i]))
                mask.append(kMask.data, uint32_t(kMask.size));
        }
        return PrStringView{mask.data, mask.count};
    }

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
        PrTextEditInputFlags keep = 0;
        if (lineFeeds && multiline)
            keep |= PrTextEditInputFlag_LineFeeds;
        if (multiline)
            keep |= PrTextEditInputFlag_Tabs;
        scratch.resize(uint32_t(value.size));
        scratch.count = prTextEditInputNormalize(value, keep, scratch.data);
        replace(low(), high(), PrStringView{scratch.data, scratch.count}, origin, merge);
    }

    // Removes the selection, or the bytes between the caret and `at` if nothing is selected.
    void erase(uint32_t at, PrTextEditOrigin origin = PrTextEditOrigin_Local) {
        if (anchor != caret)
            replace(low(), high(), PrStringView{}, origin, PrTextEditMerge_None);
        else if (at != caret)
            replace(at < caret ? at : caret, at < caret ? caret : at, PrStringView{}, origin, PrTextEditMerge_Deleting);
    }

    // Returns the piece that shows `offset`. An offset where two pieces meet is in the second, or in the first if it
    // stands upstream.
    const PrTextEditLayoutRun& pieceAt(uint32_t offset, PrLayoutTextAffinity side) const {
        uint32_t i = 0;
        while (i + 1 < pieces.count && pieces.data[i + 1].start <= offset)
            ++i;
        if (side == PrLayoutTextAffinity_Upstream && i > 0 && pieces.data[i].start == offset)
            --i;
        return pieces.data[i];
    }

    // Returns the geometry of the text as the last layout shows it.
    PrTextEditGeometry geometry(PrStringView t) const {
        if (pieces.count == 1)
            return prTextEditGeometryFromLayoutText(tree, node, t);
        return prTextEditGeometryFromLayoutRuns(tree, desc.parent, pieces.data, pieces.count, t);
    }

    // Returns the position nearest `point` in viewport px, and the affinity that puts the caret on the line under it.
    bool positionAt(PrVec2 point, uint32_t* out, PrLayoutTextAffinity* side) {
        PrLayoutVec at = prLayoutVecFromPx(point);
        uint32_t offset = 0;
        uint32_t start = 0;
        if (pieces.count == 1) {
            if (!prLayoutTextOffsetNear(tree, node, at, &offset, side))
                return false;
        } else {
            PrLayoutTextPosition position{};
            if (!prLayoutTextPositionFromPoint(tree, desc.parent, at, &position) ||
                    !prLayoutTextOffsetNear(tree, position.node, at, &offset, side))
                return false;
            for (uint32_t i = 0; i < pieces.count; ++i) {
                if (pieces.data[i].node == position.node)
                    start = pieces.data[i].start;
            }
        }
        *out = prTextEditCodePointStart(text(), textOffset(start + offset));
        return true;
    }

    // Writes the caret at `offset` in viewport px.
    bool caretAt(uint32_t offset, PrLayoutTextAffinity side, PrRect* out) {
        PrLayoutRect rect{};
        const PrTextEditLayoutRun& piece = pieceAt(offset, side);
        if (!prLayoutNodeTextCaret(tree, piece.node, shownOffset(offset) - piece.start, side,
                PrLayoutCoordinates_Viewport, &rect))
            return false;
        *out = prLayoutRectToPx(rect);
        return true;
    }

    // Carries out a motion of the caret, and returns false if the edit does not take it. The motion runs over the text
    // that the node shows, so a mask moves as one word.
    bool move(PrTextEditCommand command, bool extend) {
        PrTextEditGeometry shape = geometry(shownText());
        PrTextEditPosition at{node, shownOffset(caret), affinity};
        PrTextEditMotion motion = prTextEditMotionFromCommand(&shape, at, command, desc.arrowMotion);
        bool between = motion == PrTextEditMotion_LinePrevious || motion == PrTextEditMotion_LineNext;
        if (motion != PrTextEditMotion_None && (multiline || !between)) {
            PrTextEditSelection s{
                .anchor = shownOffset(anchor),
                .caret = shownOffset(caret),
                .goal = column,
                .hasGoal = hasColumn,
                .affinity = affinity,
            };
            PrTextEditSelectionMove kind = extend ? PrTextEditSelectionMove_Extend : PrTextEditSelectionMove_Collapse;
            s = prTextEditSelectionMove(&shape, node, s, motion, kind);
            anchor = textOffset(s.anchor);
            caret = textOffset(s.caret);
            affinity = s.affinity;
            column = s.goal;
            hasColumn = s.hasGoal;
            moved = true;
        }
        prTextEditGeometryReset(&shape);
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
            // A masked edit hides where its words end, so it deletes to the ends of the text.
            case PrTextEditCommand_DeleteWordBefore: erase(masked ? 0 : prTextEditWordBefore(t, caret)); return;
            case PrTextEditCommand_DeleteWordAfter:
                erase(masked ? uint32_t(t.size) : prTextEditWordAfter(t, caret));
                return;
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
                // A password stays out of the clipboard.
                PrAppWindow* window = prUiWindow(ui);
                if (!selected || masked)
                    return;
                if (window != nullptr)
                    prAppDataWriteText(prAppWindowApp(window), kPrAppDataSourceClipboard,
                        PrStringView{t.data + low(), high() - low()});
                if (command == PrTextEditCommand_Cut)
                    erase(caret);
                return;
            }
            case PrTextEditCommand_Paste:
                prUiPaste(ui, desc.control);
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
        if ((press && e.pointer.button != 0) || (!press && !state.dragging))
            return;
        uint32_t at = 0;
        PrLayoutTextAffinity side = 0;
        if (!positionAt(e.pointer.position, &at, &side))
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
        if (masked && state.unit != PrTextEditUnit_Character)
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
        insert(e.input.text, false, PrTextEditOrigin_Composition, PrTextEditMerge_None);
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
        PrTextEditGeometry shape = geometry(shownText());
        PrTextEditLine line{};
        bool vertical = shape.line(shape.context, PrTextEditPosition{node, shownOffset(caret), affinity},
            PrTextEditLineStep_Same, &line) && line.flow != PrTextEditFlow_HorizontalTb;
        prTextEditGeometryReset(&shape);
        static const PrAppKey kLines[] = {PrAppKey_Up, PrAppKey_Down};
        for (uint32_t i = 0; (multiline || vertical) && i < 2; ++i) {
            for (PrAppModifierFlags modifiers : kMoveModifiers) {
                if (!vertical && modifiers != 0 && modifiers != PrAppModifierFlag_Super)
                    continue;
                prUiKeyPressed(ui, desc.control, kLines[i], modifiers);
                prUiKeyPressed(ui, desc.control, kLines[i], modifiers | PrAppModifierFlag_Shift);
            }
        }
        // Return inserts a line break, with Shift or without, so that an application that captures Return for itself
        // leaves Shift-Return to the edit.
        if (multiline) {
            prUiKeyPressed(ui, desc.control, PrAppKey_Enter, 0);
            prUiKeyPressed(ui, desc.control, PrAppKey_Enter, PrAppModifierFlag_Shift);
        }
        static const PrAppKey kCommands[] = {'A', 'C', 'X', 'V'};
        for (PrAppKey key : kCommands) {
            prUiKeyPressed(ui, desc.control, key, PrAppModifierFlag_Super);
            prUiKeyPressed(ui, desc.control, key, PrAppModifierFlag_Control);
        }
    }

    // Returns the text that the text node shows: the text or its mask, with a space after it if it is empty or ends in
    // a line feed, so that the line the caret stands on has text to place it.
    PrStringView shown() {
        PrStringView t = shownText();
        if (t.size != 0 && t.data[t.size - 1] != '\n')
            return t;
        scratch.clear();
        scratch.append(t.data, uint32_t(t.size));
        scratch.push(' ');
        return PrStringView{scratch.data, scratch.count};
    }

    // Returns the record of a run's style.
    PrLayoutStyleIndex boxStyle(PrTextEditStyle style) const {
        PR_ASSERT(desc.styleCount != 0);
        return desc.styles[style < desc.styleCount ? style : 0];
    }

    // Writes the text of each run's node, and its style, creating what the runs need beyond what `pieces` has. A piece
    // that the runs no longer need goes at the commit of a later round, and shows nothing until then.
    void showRuns() {
        const PrTextEditStyleRun* runs = prTextEditBufferStyleRuns(desc.buffer);
        uint32_t count = prTextEditBufferStyleRunCount(desc.buffer);
        PrStringView all = shown();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t start = runs[i].start;
            uint32_t end = i + 1 < count ? runs[i + 1].start : uint32_t(all.size);
            PrStringView part{all.data + start, end - start};
            if (i < pieces.count) {
                prLayoutTreeFrameSetStyle(tree, boxes.data[i], boxStyle(runs[i].style));
                prLayoutTreeFrameSetText(tree, pieces.data[i].node, part);
                pieces.data[i].start = start;
            } else {
                PrLayoutNode box = prLayoutTreeFrameElement(tree, desc.parent, 0, boxStyle(runs[i].style));
                boxes.push(box);
                pieces.push(PrTextEditLayoutRun{prLayoutTreeFrameText(tree, box, 0, part), start});
            }
        }
        for (uint32_t i = count; i < pieces.count; ++i)
            prLayoutTreeFrameSetText(tree, pieces.data[i].node, "");
        pieces.count = count;
        boxes.count = count;
    }

    // Declares the nodes that show the text, as the runs stand when the round begins.
    void declare() {
        bool styled = !masked && desc.buffer != nullptr && prTextEditBufferStyleRunCount(desc.buffer) != 0;
        if (styled) {
            showRuns();
        } else {
            pieces.push(PrTextEditLayoutRun{prLayoutTreeFrameText(tree, desc.parent, 0, shown()), 0});
        }
        node = pieces.data[0].node;
    }

    // Shows what this round's edits left. The next round declares the nodes for them.
    void showEdited() {
        if (!masked && boxes.count != 0 && prTextEditBufferStyleRunCount(desc.buffer) != 0) {
            showRuns();
            return;
        }
        pieces.count = 1;
        pieces.data[0].start = 0;
        prLayoutTreeFrameSetText(tree, node, shown());
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
        .masked = desc->inputKind == PrAppTextInputKind_Password,
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
    editor.declare();

    // The layout that the last round's edits made is laid out now, so the caret can be brought into view.
    if (state.reveal) {
        state.reveal = false;
        PrLayoutRect rect{};
        const PrTextEditLayoutRun& piece = editor.pieceAt(editor.caret, editor.affinity);
        if (prLayoutNodeTextCaret(editor.tree, piece.node, editor.shownOffset(editor.caret) - piece.start,
                editor.affinity, PrLayoutCoordinates_Viewport, &rect)) {
            rect.right = rect.left + prLayoutUnitFromPx(1);
            prUiRevealRect(ui, piece.node, rect);
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
                case PrAppEventKind_KeyDown:
                    // A key that the input method takes edits the composition, not the text.
                    if (!e.keyboard.composing)
                        editor.key(e);
                    break;
                case PrAppEventKind_Text:
                    if (state.markEnd > state.markStart) {
                        editor.anchor = state.markStart;
                        editor.caret = state.markEnd;
                    }
                    editor.insert(e.input.text, false, PrTextEditOrigin_Local, PrTextEditMerge_Typing);
                    break;
                case PrAppEventKind_Paste:
                    // A clipboard without text pastes the empty string, which leaves the selection alone.
                    if (e.input.text.size != 0)
                        editor.insert(e.input.text, true, PrTextEditOrigin_Local, PrTextEditMerge_None);
                    break;
                case PrAppEventKind_CompositionUpdate:
                case PrAppEventKind_CompositionEnd: editor.compose(e); break;
                default: break;
            }
        }
    } else {
        state.dragging = false;
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
        editor.showEdited();
    editor.storeSelection();

    // The selection shows while the control has focus, and the caret while it has focus and blinks on.
    if (focused && editor.anchor != editor.caret) {
        // A highlight is of one text node, so the selection has one in each piece that it reaches.
        for (uint32_t i = 0; i < editor.pieces.count; ++i) {
            uint32_t start = editor.pieces.data[i].start;
            uint32_t end = i + 1 < editor.pieces.count ? editor.pieces.data[i + 1].start : editor.high();
            uint32_t from = editor.low() > start ? editor.low() : start;
            uint32_t to = editor.high() < end ? editor.high() : end;
            if (from >= to)
                continue;
            PrLayoutHighlight selection{
                .start = editor.shownOffset(from) - start,
                .end = editor.shownOffset(to) - start,
                .background = desc->selection,
            };
            prLayoutTreeFrameSetHighlights(editor.tree, editor.pieces.data[i].node, &selection, 1);
        }
    }
    if (focused) {
        PrRect inputRect{};
        if (editor.caretAt(editor.caret, editor.affinity, &inputRect))
            prUiSetTextInput(ui, inputRect, desc->inputKind);
        bool still = prUiUserPreferences(ui).reducedMotion;
        PrTextEditCaretBlink blink = prTextEditCaretBlinkAt(state.activity, time);
        if (editor.anchor == editor.caret) {
            const PrTextEditLayoutRun& piece = editor.pieceAt(editor.caret, editor.affinity);
            PrLayoutCaret shownCaret{
                .position = {piece.node, editor.shownOffset(editor.caret) - piece.start},
                .affinity = editor.affinity,
                .color = desc->caret,
            };
            prLayoutTreeFrameSetCarets(editor.tree, &shownCaret, 1);
            prLayoutTreeSetCaretsHidden(editor.tree, !still && !blink.on);
            if (!still)
                prUiRequestMotionAt(ui, desc->control, blink.next);
        }
    }
    PrLayoutNode first = editor.node;
    editor.scratch.reset();
    editor.mask.reset();
    editor.pieces.reset();
    editor.boxes.reset();
    return first;
}

bool prUiTextEditChanged(PrUi* ui, PrLayoutNode control, PrStringView* text) {
    const TextEditState& state = stateOf(ui, control);
    if (!state.changed)
        return false;
    *text = prTextEditBufferText(state.view != nullptr ? prTextEditViewBuffer(state.view) : state.buffer);
    return true;
}

PrTextEditView* PR_NULLABLE prUiTextEditView(PrUi* ui, PrLayoutNode control) {
    return stateOf(ui, control).view;
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
