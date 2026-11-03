// Text editing: the caret, the selection, and the edits that keys, typing, the pointer and the clipboard make.

#include "pr/ui_text_edit.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/mem.h"
#include "pr/text_edit.h"
#include "pr/unicode_grapheme.h"

namespace pr_impl {

namespace {

// The key under which an edit keeps its state on its control.
constexpr char kUiTextEditOwner = 0;

// The unit that a run of presses selects, and that a drag after it keeps whole.
enum : uint8_t {
    TextUnit_Character,
    TextUnit_Word,
    TextUnit_Line,
    TextUnit_All,
};
typedef uint8_t TextUnit;

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
    TextUnit unit;

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

// How many steps of half a line Up and Down take, at most, to find the next line.
constexpr uint32_t kLineSearchSteps = 8;

// How far left or right of the caret a point lies to reach the ends of its line, in px.
constexpr float kLineReach = 1.0e6f;

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

bool isContinuation(char byte) {
    return (uint8_t(byte) & 0xc0) == 0x80;
}

// Returns `at` clamped to the text and moved back to the start of the code point it is in.
uint32_t clampOffset(PrStringView text, uint32_t at) {
    at = at < text.size ? at : uint32_t(text.size);
    while (at > 0 && at < text.size && isContinuation(text.data[at]))
        --at;
    return at;
}

// Returns the start of the line of the text that holds `at`, after the line feed before it.
uint32_t lineStart(PrStringView text, uint32_t at) {
    while (at > 0 && text.data[at - 1] != '\n')
        --at;
    return at;
}

// Returns the end of the line of the text that holds `at`, at the line feed after it.
uint32_t lineEnd(PrStringView text, uint32_t at) {
    while (at < text.size && text.data[at] != '\n')
        ++at;
    return at;
}

// Returns the boundary of the grapheme cluster after `at`, or the end of the text.
uint32_t nextGrapheme(PrStringView text, uint32_t at) {
    if (at >= text.size)
        return uint32_t(text.size);
    // Clusters never cross a line feed, so the clusters of a line are found from its start.
    uint32_t b = lineStart(text, at);
    while (b <= at)
        b = uint32_t(prUnicodeGraphemeNext(text, b));
    return b;
}

// Returns the boundary of the grapheme cluster before `at`, or the start of the text.
uint32_t previousGrapheme(PrStringView text, uint32_t at) {
    if (at == 0)
        return 0;
    uint32_t start = lineStart(text, at);
    if (start == at)
        return at - 1;
    uint32_t b = start;
    for (;;) {
        uint32_t next = uint32_t(prUnicodeGraphemeNext(text, b));
        if (next >= at)
            return b;
        b = next;
    }
}

// Returns true if the byte belongs to a word: an ASCII letter, digit or underscore, or a byte of a non-ASCII character.
bool isWordByte(char byte) {
    uint8_t value = uint8_t(byte);
    return value >= 0x80 || (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '_';
}

// Returns where a word motion leftward from `at` ends: past the bytes outside words before it, then past the word.
uint32_t wordLeft(PrStringView text, uint32_t at) {
    while (at > 0 && !isWordByte(text.data[at - 1]))
        --at;
    while (at > 0 && isWordByte(text.data[at - 1]))
        --at;
    return at;
}

// Returns where a word motion rightward from `at` ends.
uint32_t wordRight(PrStringView text, uint32_t at) {
    while (at < text.size && !isWordByte(text.data[at]))
        ++at;
    while (at < text.size && isWordByte(text.data[at]))
        ++at;
    return at;
}

// Writes the bytes of `unit` around `at`: the word or the run of bytes outside words, the line, or the whole text.
void unitAround(PrStringView text, uint32_t at, TextUnit unit, uint32_t* low, uint32_t* high) {
    switch (unit) {
        case TextUnit_Word: {
            bool word = at < text.size ? isWordByte(text.data[at]) : at > 0 && isWordByte(text.data[at - 1]);
            uint32_t lo = at;
            while (lo > 0 && isWordByte(text.data[lo - 1]) == word && text.data[lo - 1] != '\n')
                --lo;
            uint32_t hi = at;
            while (hi < text.size && isWordByte(text.data[hi]) == word && text.data[hi] != '\n')
                ++hi;
            *low = lo;
            *high = hi;
            return;
        }
        case TextUnit_Line:
            *low = lineStart(text, at);
            *high = lineEnd(text, at);
            return;
        case TextUnit_All:
            *low = 0;
            *high = uint32_t(text.size);
            return;
        default:
            *low = at;
            *high = at;
            return;
    }
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
        PrLayoutTextPosition position{};
        if (!prLayoutTextPositionFromPoint(tree, node, prLayoutVecFromPx(point), &position))
            return false;
        PrStringView t = text();
        *out = clampOffset(t, position.offset);
        // A position at a soft wrap stands at the end of the line above if the point is nearer that line.
        *side = PrLayoutTextAffinity_Downstream;
        PrRect down{};
        PrRect up{};
        if (caretAt(*out, PrLayoutTextAffinity_Downstream, &down) && caretAt(*out, PrLayoutTextAffinity_Upstream,
                &up) && down.top != up.top) {
            float downDistance = absF32((down.top + down.bottom) / 2 - point.y);
            float upDistance = absF32((up.top + up.bottom) / 2 - point.y);
            if (upDistance < downDistance)
                *side = PrLayoutTextAffinity_Upstream;
        }
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

    // Writes the positions at the ends of the line that the caret is on.
    bool lineEnds(uint32_t* start, uint32_t* end) {
        PrRect rect{};
        if (!caretAt(caret, affinity, &rect))
            return false;
        float y = (rect.top + rect.bottom) / 2;
        uint32_t a = 0;
        uint32_t b = 0;
        PrLayoutTextAffinity ignored = 0;
        if (!positionAt(PrVec2{rect.left - kLineReach, y}, &a, &ignored) ||
                !positionAt(PrVec2{rect.left + kLineReach, y}, &b, &ignored))
            return false;
        *start = a < b ? a : b;
        *end = a < b ? b : a;
        return true;
    }

    // Moves the caret to the line above or below, at the column it keeps, or to the end of the text if there is none.
    void moveLine(bool down, bool extend) {
        PrStringView t = text();
        PrRect rect{};
        if (!caretAt(caret, affinity, &rect)) {
            moveTo(down ? uint32_t(t.size) : 0, extend);
            return;
        }
        float kept = hasColumn ? column : rect.left;
        float step = (rect.bottom - rect.top) / 2;
        step = step > 1 ? step : 1;
        float y = down ? rect.bottom : rect.top;
        for (uint32_t i = 0; i < kLineSearchSteps; ++i) {
            y += down ? step : -step;
            uint32_t at = 0;
            PrLayoutTextAffinity side = 0;
            PrRect found{};
            if (!positionAt(PrVec2{kept, y}, &at, &side) || !caretAt(at, side, &found))
                break;
            // A line's content area may overlap the next line's, so a line is told by the top of its caret.
            bool past = down ? found.top > rect.top + 0.5f : found.top < rect.top - 0.5f;
            if (past) {
                moveTo(at, extend, side);
                column = kept;
                hasColumn = true;
                return;
            }
        }
        moveTo(down ? uint32_t(t.size) : 0, extend);
    }

    void key(const PrAppEvent& e) {
        PrStringView t = text();
        PrAppModifierFlags modifiers = PrAppModifierFlags(e.modifiers & ~PrAppModifierFlag_CapsLock);
        bool extend = (modifiers & PrAppModifierFlag_Shift) != 0;
        bool command = (modifiers & PrAppModifierFlag_Super) != 0;
        bool control = (modifiers & PrAppModifierFlag_Control) != 0;
        bool byWord = (modifiers & (PrAppModifierFlag_Alt | PrAppModifierFlag_Control)) != 0;
        bool selected = anchor != caret;
        switch (e.key) {
            case PrAppKey_Left:
            case PrAppKey_Right: {
                bool right = e.key == PrAppKey_Right;
                uint32_t start = 0;
                uint32_t end = 0;
                if (command && multiline && lineEnds(&start, &end))
                    moveTo(right ? end : start, extend,
                        right ? PrLayoutTextAffinity_Upstream : PrLayoutTextAffinity_Downstream);
                else if (command)
                    moveTo(right ? lineEnd(t, caret) : lineStart(t, caret), extend);
                else if (byWord)
                    moveTo(right ? wordRight(t, caret) : wordLeft(t, caret), extend);
                else if (selected && !extend)
                    moveTo(right ? high() : low(), false);
                else
                    moveTo(right ? nextGrapheme(t, caret) : previousGrapheme(t, caret), extend);
                return;
            }
            case PrAppKey_Home:
            case PrAppKey_End: {
                bool end = e.key == PrAppKey_End;
                uint32_t first = 0;
                uint32_t last = uint32_t(t.size);
                if (!command && !control && multiline && !lineEnds(&first, &last)) {
                    first = lineStart(t, caret);
                    last = lineEnd(t, caret);
                }
                moveTo(end ? last : first, extend,
                    end ? PrLayoutTextAffinity_Upstream : PrLayoutTextAffinity_Downstream);
                return;
            }
            case PrAppKey_Up:
            case PrAppKey_Down: {
                bool down = e.key == PrAppKey_Down;
                if (!multiline)
                    return;
                if (command) {
                    moveTo(down ? uint32_t(t.size) : 0, extend);
                    return;
                }
                if (selected && !extend) {
                    moveTo(down ? high() : low(), false);
                    return;
                }
                moveLine(down, extend);
                return;
            }
            case PrAppKey_Backspace:
                erase(byWord ? wordLeft(t, caret) : previousGrapheme(t, caret));
                return;
            case PrAppKey_Delete:
                erase(byWord ? wordRight(t, caret) : nextGrapheme(t, caret));
                return;
            case PrAppKey_Enter:
                if (multiline)
                    insert("\n", true, PrTextEditOrigin_Local, PrTextEditMerge_None);
                return;
            default:
                break;
        }
        if (!command && !control)
            return;
        PrApp* app = prUiApp(ui);
        switch (e.key) {
            case 'A':
                anchor = 0;
                moveTo(uint32_t(t.size), true);
                return;
            case 'C':
            case 'X':
                if (!selected)
                    return;
                if (app != nullptr)
                    prAppSetClipboardText(app, PrStringView{t.data + low(), high() - low()});
                if (e.key == 'X')
                    erase(caret);
                return;
            case 'V':
                if (app != nullptr) {
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
            unitAround(t, at, state.unit, &low, &high);
            if (at < state.unitLow) {
                anchor = state.unitHigh;
                moveTo(low, true, side);
            } else {
                anchor = state.unitLow;
                moveTo(state.unit == TextUnit_Character ? at : high, true, side);
            }
            return;
        }
        state.dragging = true;
        if ((e.modifiers & PrAppModifierFlag_Shift) != 0) {
            state.unit = TextUnit_Character;
            state.unitLow = state.unitHigh = anchor;
            moveTo(at, true, side);
            return;
        }
        uint32_t presses = prUiPressCount(ui, 0);
        if (presses <= 1)
            state.unit = TextUnit_Character;
        else if (presses == 2)
            state.unit = TextUnit_Word;
        else if (presses == 3 && multiline)
            state.unit = TextUnit_Line;
        else
            state.unit = TextUnit_All;
        unitAround(t, at, state.unit, &state.unitLow, &state.unitHigh);
        anchor = state.unitLow;
        moveTo(state.unit == TextUnit_Character ? at : state.unitHigh, true, side);
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
        static const PrAppKey kLines[] = {PrAppKey_Up, PrAppKey_Down};
        static const PrAppModifierFlags kLineModifiers[] = {0, PrAppModifierFlag_Super};
        for (uint32_t i = 0; multiline && i < 2; ++i) {
            for (PrAppModifierFlags modifiers : kLineModifiers) {
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
    editor.anchor = clampOffset(text, editor.anchor);
    editor.caret = clampOffset(text, editor.caret);
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
