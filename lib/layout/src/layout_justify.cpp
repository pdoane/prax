// Justification: the justification opportunities of a line (css-text-3/#justification).

#include "layout/src/layout_justify.h"

#include "pr/search.h"
#include "pr/text_shaping.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Returns true if `codePoint` is a letter of a cursive script that joins or is joined by its neighbors. Justification
// opens no gap between two such letters (css-text-3/#justify-cursive).
bool isCursiveLetter(uint32_t codePoint) {
    PrUnicodeJoiningType joining = prUnicodeJoiningType(codePoint);
    return joining != PrUnicodeJoiningType_NonJoining && joining != PrUnicodeJoiningType_Transparent;
}

} // namespace

void layoutJustifierReset(LayoutJustifier& justifier) {
    justifier.opportunities.reset();
    justifier.open.reset();
}

PR_NOINLINE void layoutJustifyFind(LayoutState& state, LayoutJustifier& justifier, const LayoutInlineShaping& t,
        const PrLayoutStyleIndex* styles, PrLayoutStyleIndex containerStyle, const LayoutLineContent& line) {
    RawArray<LayoutJustifyOpportunity>& opportunities = justifier.opportunities;
    opportunities.clear();
    uint32_t stop = line.end;
    while (stop > line.from.byte && ((t.spaces.data[stop - 1] & (PrTextSpaceFlag_Collapsible | PrTextSpaceFlag_Hangs)) != 0 ||
            t.text.data[stop - 1] == '\n'))
        --stop;
    // The last tab may be in the white space that ends the line, which leaves nothing to stretch.
    uint32_t from = line.from.byte;
    for (uint32_t b = line.from.byte; t.tabs && b < line.end; ++b) {
        if ((t.spaces.data[b] & PrTextSpaceFlag_Tab) != 0)
            from = b + 1;
    }
    // The boxes open around the text. Depth zero is the root inline box.
    RawArray<uint32_t>& open = justifier.open;
    open.clear();
    for (uint32_t i = 0; i < line.carried.count; ++i)
        open.push(line.carried.data[i]);
    const PrLayoutFontStyle& root = layoutFontStyle(state.styles, containerStyle);
    auto fontAt = [&](uint32_t depth) -> const PrLayoutFontStyle& {
        return depth == 0 ? root : layoutFontStyle(state.styles, styles[open.data[depth - 1]]);
    };
    // A gap and a separator's opportunity at the same byte merge into one opportunity, which is the separator's.
    auto add = [&](uint32_t place, bool gap) {
        if (opportunities.count != 0 && opportunities.last().byte == place)
            opportunities.last().gap &= gap;
        else
            opportunities.push(LayoutJustifyOpportunity{place, gap});
    };
    PrStringView text{t.text.data, line.segmentEnd};
    bool previous = false;          // true if a character unit precedes, since the last atomic inline or break
    uint32_t previousCode = 0;      // the first code point of that unit
    uint32_t shallowest = 0;        // the smallest number of boxes open since that unit
    uint32_t unitEnd = 0;           // the end of the last unit begun, which may lie past the item that began it
    for (uint32_t i = 0; i < line.items.count; ++i) {
        const LayoutLineItem& on = line.items.data[i];
        const LayoutInlineItem& it = t.items.data[on.item];
        switch (it.kind) {
            case LayoutInlineItemKind_Open: open.push(on.item); continue;
            case LayoutInlineItemKind_Close:
                if (open.count != 0)
                    open.pop();
                shallowest = shallowest < open.count ? shallowest : open.count;
                continue;
            case LayoutInlineItemKind_Text: break;
            case LayoutInlineItemKind_Float:
            case LayoutInlineItemKind_OutOfFlow: continue;
            default: previous = false; continue;
        }
        PrLayoutTextJustify own = layoutFontStyle(state.styles, styles[on.item]).textJustify;
        bool words = own == PrLayoutTextJustify_Auto || own == PrLayoutTextJustify_InterWord;
        uint32_t g = it.glyphStart;
        uint32_t last = on.to < stop ? on.to : stop;
        for (uint32_t b = on.from; b < last;) {
            // A unit that an earlier item began continues in this one, with no gap inside it.
            bool starts = b >= unitEnd;
            if (starts)
                unitEnd = uint32_t(prUnicodeGraphemeNext(text, b));
            uint32_t next = unitEnd < it.end ? unitEnd : it.end;
            if (!starts) {
                b = next;
                continue;
            }
            uint32_t codePoint = 0;
            prUtf8Decode(text, b, &codePoint);
            if (previous && b >= from) {
                while (g < it.glyphEnd && t.info.glyphs[g].cluster < b)
                    ++g;
                bool unsplit = b == it.start || (g < it.glyphEnd && t.info.glyphs[g].cluster == b);
                bool cursive = isCursiveLetter(previousCode) && isCursiveLetter(codePoint);
                if (unsplit && !cursive && fontAt(shallowest).textJustify == PrLayoutTextJustify_InterCharacter)
                    add(b, true);
            }
            if (words && next > from && prTextIsWordSeparator(codePoint))
                add(next, false);
            previous = true;
            previousCode = codePoint;
            shallowest = open.count;
            b = next;
        }
    }
}

} // namespace pr_impl
