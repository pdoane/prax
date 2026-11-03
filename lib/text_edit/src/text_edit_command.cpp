#include "pr/text_edit_command.h"

#include "pr/locale_common.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Returns the script whose runs make one word: UAX #29 ends a word at each Han and Hiragana character, and a reader
// selecting Japanese or Chinese expects the run. A dictionary could divide these runs further. Returns zero for others.
PrScript runScript(PrStringView text, uint32_t at) {
    uint32_t codePoint = 0;
    prUtf8Decode(text, at, &codePoint);
    PrScript script = prUnicodeScript(codePoint);
    return script == PrScript_Hani || script == PrScript_Hira ? script : PrScript(0);
}

// Returns where the word segment that starts at the boundary `at` ends, with runs of one script taken whole.
uint32_t segmentEnd(PrStringView text, uint32_t at) {
    PrScript run = runScript(text, at);
    uint32_t end = uint32_t(prUnicodeWordNext(text, at));
    while (run != PrScript(0) && end < text.size && runScript(text, end) == run)
        end = uint32_t(prUnicodeWordNext(text, end));
    return end;
}

// Returns true if the segment that starts at `at` is a word: it holds a letter, a number or a mark rather than spaces
// or punctuation alone.
bool isWord(PrStringView text, uint32_t at) {
    uint32_t codePoint = 0;
    prUtf8Decode(text, at, &codePoint);
    switch (prUnicodeGeneralCategory(codePoint)) {
        case PrUnicodeGeneralCategory_UppercaseLetter:
        case PrUnicodeGeneralCategory_LowercaseLetter:
        case PrUnicodeGeneralCategory_TitlecaseLetter:
        case PrUnicodeGeneralCategory_ModifierLetter:
        case PrUnicodeGeneralCategory_OtherLetter:
        case PrUnicodeGeneralCategory_DecimalNumber:
        case PrUnicodeGeneralCategory_LetterNumber:
        case PrUnicodeGeneralCategory_OtherNumber:
        case PrUnicodeGeneralCategory_ConnectorPunctuation: return true;
        default: return false;
    }
}

// Returns true if the code point makes its cluster mean one thing only as a whole.
bool bindsCluster(uint32_t codePoint) {
    if ((codePoint >= 0xfe00 && codePoint <= 0xfe0f) || (codePoint >= 0xe0100 && codePoint <= 0xe01ef))
        return true;
    if (codePoint == 0x20e3)
        return true;
    if ((prUnicodeFlags(codePoint) & PrUnicodeFlag_ExtendedPictographic) != 0)
        return true;
    return prUnicodeGraphemeBreak(codePoint) == PrUnicodeGraphemeBreak_RegionalIndicator;
}

uint32_t clamped(PrStringView text, uint32_t at) {
    return at < text.size ? at : uint32_t(text.size);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prTextEditCodePointStart(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    while (at > 0 && at < text.size && (uint8_t(text.data[at]) & 0xc0) == 0x80)
        --at;
    return at;
}

uint32_t prTextEditLineStart(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    while (at > 0 && text.data[at - 1] != '\n')
        --at;
    return at;
}

uint32_t prTextEditLineEnd(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    while (at < text.size && text.data[at] != '\n')
        ++at;
    return at;
}

uint32_t prTextEditClusterAfter(PrStringView text, uint32_t at) {
    if (at >= text.size)
        return uint32_t(text.size);
    // Clusters never cross a line feed, so the clusters of a line are found from its start.
    uint32_t b = prTextEditLineStart(text, at);
    while (b <= at)
        b = uint32_t(prUnicodeGraphemeNext(text, b));
    return b;
}

uint32_t prTextEditClusterBefore(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    if (at == 0)
        return 0;
    uint32_t start = prTextEditLineStart(text, at);
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

uint32_t prTextEditBackspaceStart(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    uint32_t cluster = prTextEditClusterBefore(text, at);
    uint32_t last = cluster;
    for (uint32_t b = cluster; b < at;) {
        uint32_t codePoint = 0;
        size_t size = prUtf8Decode(text, b, &codePoint);
        if (bindsCluster(codePoint))
            return cluster;
        last = b;
        b += uint32_t(size != 0 ? size : 1);
    }
    return last;
}

uint32_t prTextEditWordAfter(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    uint32_t b = prTextEditLineStart(text, at);
    while (b < text.size) {
        uint32_t end = segmentEnd(text, b);
        if (end > at && isWord(text, b))
            return end;
        b = end;
    }
    return uint32_t(text.size);
}

uint32_t prTextEditWordBefore(PrStringView text, uint32_t at) {
    at = clamped(text, at);
    for (;;) {
        uint32_t line = prTextEditLineStart(text, at);
        uint32_t found = at;
        for (uint32_t b = line; b < at; b = segmentEnd(text, b)) {
            if (isWord(text, b))
                found = b;
        }
        if (found != at)
            return found;
        if (line == 0)
            return 0;
        at = line - 1;
    }
}

void prTextEditUnitAround(PrStringView text, uint32_t at, PrTextEditUnit unit, uint32_t* low, uint32_t* high) {
    at = clamped(text, at);
    switch (unit) {
        case PrTextEditUnit_Word: {
            // At the end of a line, the unit is the segment before the position.
            uint32_t line = prTextEditLineStart(text, at);
            uint32_t end = prTextEditLineEnd(text, at);
            uint32_t within = at == end && at > line ? at - 1 : at;
            uint32_t lo = line;
            uint32_t hi = line;
            while (hi < end) {
                lo = hi;
                hi = segmentEnd(text, lo);
                if (hi > within)
                    break;
            }
            *low = lo;
            *high = hi < end ? hi : end;
            return;
        }
        case PrTextEditUnit_Line:
            *low = prTextEditLineStart(text, at);
            *high = prTextEditLineEnd(text, at);
            return;
        case PrTextEditUnit_All:
            *low = 0;
            *high = uint32_t(text.size);
            return;
        default:
            *low = at;
            *high = at;
            return;
    }
}

PrTextEditCommand prTextEditCommandFromKey(const PrAppEvent* event, bool* extend) {
    PrAppModifierFlags modifiers = PrAppModifierFlags(event->modifiers & ~PrAppModifierFlag_CapsLock);
    *extend = (modifiers & PrAppModifierFlag_Shift) != 0;
    bool command = (modifiers & PrAppModifierFlag_Super) != 0;
    bool control = (modifiers & PrAppModifierFlag_Control) != 0;
    bool byWord = (modifiers & (PrAppModifierFlag_Alt | PrAppModifierFlag_Control)) != 0;
    uint32_t group = command ? 2 : byWord ? 1 : 0;
    switch (event->key) {
        case PrAppKey_Left: return PrTextEditCommand(PrTextEditCommand_Left + 4 * group);
        case PrAppKey_Right: return PrTextEditCommand(PrTextEditCommand_Right + 4 * group);
        case PrAppKey_Up: return PrTextEditCommand(PrTextEditCommand_Up + 4 * group);
        case PrAppKey_Down: return PrTextEditCommand(PrTextEditCommand_Down + 4 * group);
        case PrAppKey_Home: return command || control ? PrTextEditCommand_TextStart : PrTextEditCommand_LineStart;
        case PrAppKey_End: return command || control ? PrTextEditCommand_TextEnd : PrTextEditCommand_LineEnd;
        case PrAppKey_Backspace: return byWord ? PrTextEditCommand_DeleteWordBefore : PrTextEditCommand_DeleteBackward;
        case PrAppKey_Delete: return byWord ? PrTextEditCommand_DeleteWordAfter : PrTextEditCommand_DeleteForward;
        case PrAppKey_Enter: return PrTextEditCommand_InsertLineBreak;
        default: break;
    }
    if (!command && !control)
        return PrTextEditCommand_None;
    switch (event->key) {
        case 'A': return PrTextEditCommand_SelectAll;
        case 'C': return PrTextEditCommand_Copy;
        case 'X': return PrTextEditCommand_Cut;
        case 'V': return PrTextEditCommand_Paste;
        case 'Z': return *extend ? PrTextEditCommand_Redo : PrTextEditCommand_Undo;
        default: return PrTextEditCommand_None;
    }
}
