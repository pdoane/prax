// Text editing: the caret, the selection, and the edits that keys, typing, the pointer and the clipboard make.

#include "pr/ui_text_edit.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
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
    uint32_t anchor;
    uint32_t caret;

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

TextEditState& stateOf(PrUi* ui, PrLayoutNode control) {
    return *static_cast<TextEditState*>(prUiState(ui, control, &kUiTextEditOwner, 0, sizeof(TextEditState)));
}

PrStringView readText(const PrUiTextBuffer& buffer) {
    return buffer.read(buffer.context);
}

uint32_t lowOf(const TextEditState& s) {
    return s.anchor < s.caret ? s.anchor : s.caret;
}

uint32_t highOf(const TextEditState& s) {
    return s.anchor < s.caret ? s.caret : s.anchor;
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

    PrStringView text() const { return readText(desc.buffer); }

    // Moves the caret to `at`, and the anchor with it unless `extend` is true.
    void moveTo(uint32_t at, bool extend, PrLayoutTextAffinity affinity = PrLayoutTextAffinity_Downstream) {
        state.caret = at;
        if (!extend)
            state.anchor = at;
        state.affinity = affinity;
        state.hasColumn = false;
        moved = true;
    }

    // Replaces the bytes from `start` to `end` with `value` as one gesture, and puts the caret after it.
    void replace(uint32_t start, uint32_t end, PrStringView value) {
        if (start == end && value.size == 0)
            return;
        prUiEdit(ui, desc.control, PrUiEditPhase_Begin);
        uint32_t before = uint32_t(text().size);
        desc.buffer.replace(desc.buffer.context, start, end, value);
        prUiEdit(ui, desc.control, PrUiEditPhase_Change);
        prUiEdit(ui, desc.control, PrUiEditPhase_Commit);
        // The caret follows what the buffer took, which may be less than `value`.
        uint32_t after = uint32_t(text().size);
        uint32_t taken = after + (end - start) > before ? after + (end - start) - before : 0;
        state.markStart = state.markEnd = 0;
        moveTo(start + taken, false);
        changed = true;
    }

    // Replaces the selection with `value`, less the control characters and, unless the edit is multiline, its line
    // feeds.
    void insert(PrStringView value, bool lineFeeds) {
        scratch.clear();
        for (size_t i = 0; i < value.size; ++i) {
            char c = value.data[i];
            bool control = uint8_t(c) < 0x20 || c == 0x7f;
            if (!control || (c == '\n' && lineFeeds && multiline))
                scratch.push(c);
        }
        replace(lowOf(state), highOf(state), PrStringView{scratch.data, scratch.count});
    }

    // Removes the selection, or the bytes between the caret and `at` if nothing is selected.
    void erase(uint32_t at) {
        if (state.anchor != state.caret)
            replace(lowOf(state), highOf(state), PrStringView{});
        else if (at != state.caret)
            replace(at < state.caret ? at : state.caret, at < state.caret ? state.caret : at, PrStringView{});
    }

    // Returns the position nearest `point` in viewport px, and the affinity that puts the caret on the line under it.
    bool positionAt(PrVec2 point, uint32_t* out, PrLayoutTextAffinity* affinity) {
        PrLayoutTextPosition position{};
        if (!prLayoutTextPositionFromPoint(tree, node, prLayoutVecFromPx(point), &position))
            return false;
        PrStringView t = text();
        *out = clampOffset(t, position.offset);
        // A position at a soft wrap stands at the end of the line above if the point is nearer that line.
        *affinity = PrLayoutTextAffinity_Downstream;
        PrRect down{};
        PrRect up{};
        if (caretAt(*out, PrLayoutTextAffinity_Downstream, &down) && caretAt(*out, PrLayoutTextAffinity_Upstream,
                &up) && down.top != up.top) {
            float downDistance = absF32((down.top + down.bottom) / 2 - point.y);
            float upDistance = absF32((up.top + up.bottom) / 2 - point.y);
            if (upDistance < downDistance)
                *affinity = PrLayoutTextAffinity_Upstream;
        }
        return true;
    }

    // Writes the caret at `offset` in viewport px.
    bool caretAt(uint32_t offset, PrLayoutTextAffinity affinity, PrRect* out) {
        PrLayoutRect rect{};
        if (!prLayoutNodeTextCaret(tree, node, offset, affinity, PrLayoutCoordinates_Viewport, &rect))
            return false;
        *out = prLayoutRectToPx(rect);
        return true;
    }

    // Writes the positions at the ends of the line that the caret is on.
    bool lineEnds(uint32_t* start, uint32_t* end) {
        PrRect rect{};
        if (!caretAt(state.caret, state.affinity, &rect))
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
        if (!caretAt(state.caret, state.affinity, &rect)) {
            moveTo(down ? uint32_t(t.size) : 0, extend);
            return;
        }
        float column = state.hasColumn ? state.column : rect.left;
        float step = (rect.bottom - rect.top) / 2;
        step = step > 1 ? step : 1;
        float y = down ? rect.bottom : rect.top;
        for (uint32_t i = 0; i < kLineSearchSteps; ++i) {
            y += down ? step : -step;
            uint32_t at = 0;
            PrLayoutTextAffinity affinity = 0;
            PrRect found{};
            if (!positionAt(PrVec2{column, y}, &at, &affinity) || !caretAt(at, affinity, &found))
                break;
            // A line's content area may overlap the next line's, so a line is told by the top of its caret.
            bool past = down ? found.top > rect.top + 0.5f : found.top < rect.top - 0.5f;
            if (past) {
                moveTo(at, extend, affinity);
                state.column = column;
                state.hasColumn = true;
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
        bool selected = state.anchor != state.caret;
        uint32_t caret = state.caret;
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
                    moveTo(right ? highOf(state) : lowOf(state), false);
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
                    moveTo(down ? highOf(state) : lowOf(state), false);
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
                    insert("\n", true);
                return;
            default:
                break;
        }
        if (!command && !control)
            return;
        PrApp* app = prUiApp(ui);
        switch (e.key) {
            case 'A':
                state.anchor = 0;
                moveTo(uint32_t(t.size), true);
                return;
            case 'C':
            case 'X':
                if (!selected)
                    return;
                if (app != nullptr)
                    prAppSetClipboardText(app, PrStringView{t.data + lowOf(state), highOf(state) - lowOf(state)});
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
        PrLayoutTextAffinity affinity = 0;
        if (!positionAt(e.position, &at, &affinity))
            return;
        PrStringView t = text();
        if (!press) {
            // A drag keeps whole the unit that the press selected.
            uint32_t low = 0;
            uint32_t high = 0;
            unitAround(t, at, state.unit, &low, &high);
            if (at < state.unitLow) {
                state.anchor = state.unitHigh;
                moveTo(low, true, affinity);
            } else {
                state.anchor = state.unitLow;
                moveTo(state.unit == TextUnit_Character ? at : high, true, affinity);
            }
            return;
        }
        state.dragging = true;
        if ((e.modifiers & PrAppModifierFlag_Shift) != 0) {
            state.unit = TextUnit_Character;
            state.unitLow = state.unitHigh = state.anchor;
            moveTo(at, true, affinity);
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
        state.anchor = state.unitLow;
        moveTo(state.unit == TextUnit_Character ? at : state.unitHigh, true, affinity);
    }

    // Replaces the text being composed, or the selection, with an update's text, and marks what it inserted. An update
    // with no text, and the end of the composition, remove the marked text. The text that a composition commits comes
    // after its end as a Text event.
    void compose(const PrAppEvent& e) {
        if (state.markEnd > state.markStart) {
            state.anchor = state.markStart;
            state.caret = state.markEnd;
        }
        uint32_t start = lowOf(state);
        if (e.kind == PrAppEventKind_CompositionEnd) {
            erase(state.caret);
            return;
        }
        insert(e.text, false);
        if (state.caret > start) {
            state.markStart = start;
            state.markEnd = state.caret;
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
    PR_ASSERT(desc->buffer.read != nullptr && desc->buffer.replace != nullptr);
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
    PrStringView text = editor.text();
    state.anchor = clampOffset(text, state.anchor);
    state.caret = clampOffset(text, state.caret);
    if (state.markEnd > text.size || state.markStart > state.markEnd)
        state.markStart = state.markEnd = 0;
    editor.node = prLayoutTreeFrameText(editor.tree, desc->parent, 0, editor.shown());

    // The layout that the last round's edits made is laid out now, so the caret can be brought into view.
    if (state.reveal) {
        state.reveal = false;
        PrLayoutRect caret{};
        if (prLayoutNodeTextCaret(editor.tree, editor.node, state.caret, state.affinity, PrLayoutCoordinates_Viewport, &caret)) {
            caret.right = caret.left + prLayoutUnitFromPx(1);
            prUiRevealRect(ui, editor.node, caret);
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
                state.anchor = 0;
                state.caret = uint32_t(text.size);
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
                        state.anchor = state.markStart;
                        state.caret = state.markEnd;
                    }
                    editor.insert(e.text, false);
                    break;
                case PrAppEventKind_Clipboard:
                    if (state.pasting)
                        editor.insert(e.text, true);
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

    // The selection shows while the control has focus, and the caret while it has focus and blinks on.
    if (focused && state.anchor != state.caret) {
        PrLayoutHighlight selection{.start = lowOf(state), .end = highOf(state), .background = desc->selection};
        prLayoutTreeFrameSetHighlights(editor.tree, editor.node, &selection, 1);
    }
    if (focused) {
        bool still = prUiUserPreferences(ui).reducedMotion;
        double phase = time - state.activity;
        bool on = still || phase - kBlinkSeconds * floorF64(phase / kBlinkSeconds) < kBlinkSeconds / 2;
        if (on && state.anchor == state.caret) {
            PrLayoutCaret caret{
                .position = {editor.node, state.caret},
                .affinity = state.affinity,
                .color = desc->caret,
            };
            prLayoutTreeFrameSetCaret(editor.tree, &caret);
        }
        if (!still)
            prUiRequestMotion(ui, desc->control);
    }
    return editor.node;
}

PrUiTextSelection prUiTextEditSelection(PrUi* ui, PrLayoutNode control) {
    const TextEditState& state = stateOf(ui, control);
    return PrUiTextSelection{state.anchor, state.caret};
}

void prUiTextEditSetSelection(PrUi* ui, PrLayoutNode control, PrUiTextSelection selection) {
    TextEditState& state = stateOf(ui, control);
    state.anchor = selection.anchor;
    state.caret = selection.caret;
    state.affinity = PrLayoutTextAffinity_Downstream;
    state.hasColumn = false;
    state.reveal = true;
}
