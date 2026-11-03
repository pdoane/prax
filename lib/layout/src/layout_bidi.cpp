// Bidirectional text: finding where reordering splits the inline boxes of an inline formatting context.

#include "layout/src/layout_bidi.h"

#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/utf8.h"

#include "layout/src/layout_logical.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Splits

// Resolves the levels of the paragraph `paragraph` of `t` with rule L1, as for a line, so that the white space at its
// end takes the paragraph level. Writes the levels to `s.lineLevels`, indexed by byte from the start of the paragraph.
void paragraphLevels(const LayoutInlineShaping& t, LayoutShapingScratch& s, const PrTextParagraph& paragraph) {
    PrStringView text{t.text.data, paragraph.end};
    s.classes.clear();
    s.bidiLevels.clear();
    for (uint32_t at = paragraph.start; at < paragraph.end;) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(text, at, &codePoint);
        s.classes.push(prUnicodeBidiClass(codePoint));
        s.bidiLevels.push(t.info.levels[at]);
        at += size;
    }
    prUnicodeBidiLineLevels(s.classes.data, s.classes.count, paragraph.level, s.bidiLevels.data);
    s.lineLevels.resize(paragraph.end - paragraph.start);
    for (uint32_t at = paragraph.start, code = 0; at < paragraph.end; ++code) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(text, at, &codePoint);
        for (uint32_t b = at; b < at + size; ++b)
            s.lineLevels.data[b - paragraph.start] = s.bidiLevels.data[code];
        at += size;
    }
}

// The units of a paragraph, as they are found: where each unit starts, and its level. A new unit starts wherever the
// level changes, and wherever an item that is not text begins or ends.
struct Units {
    LayoutShapingScratch& s;
    bool fresh;                     // true if the next byte starts a new unit

    void push(uint32_t start, uint8_t level) {
        s.unitStarts.push(start);
        s.unitLevels.push(level);
    }
    void byte(uint32_t b, uint8_t level) {
        if (fresh || s.unitLevels.count == 0 || s.unitLevels.last() != level)
            push(b, level);
        fresh = false;
    }
};

} // namespace

// The content of an isolate is reordered within the isolate, so nothing outside the isolate splits it.
void layoutBidiFindSplits(LayoutInlineShaping& t, LayoutShapingScratch& s) {
    t.splits.clear();
    // `open` holds the Open items of the boxes that are open across paragraphs. `boxes` holds the units of each box in
    // the current paragraph as three values per box: its Open item, its first unit, and the unit after its last.
    RawArray<uint32_t>& open = s.bidiOpen;
    RawArray<uint32_t>& boxes = s.boxBytes;
    open.clear();
    LayoutItemIndex item = 0;
    for (uint32_t k = 0; k < t.info.paragraphCount; ++k) {
        const PrTextParagraph& paragraph = t.info.paragraphs[k];
        paragraphLevels(t, s, paragraph);
        auto levelAt = [&](uint32_t b) { return s.lineLevels.data[b - paragraph.start]; };
        s.unitStarts.clear();
        s.unitLevels.clear();
        boxes.clear();
        // Boxes that are open across the start of the paragraph begin at its first unit.
        RawArray<uint32_t>& first = s.unitRuns;
        first.resize(open.count);
        prMemZero(first.data, open.count * sizeof(uint32_t));
        Units units{.s = s, .fresh = true};
        bool last = k + 1 == t.info.paragraphCount;
        for (bool more = true; more && item < t.items.count;) {
            const LayoutInlineItem& it = t.items.data[item];
            if (it.start > paragraph.end || (it.start == paragraph.end && !last))
                break;
            switch (it.kind) {
                case LayoutInlineItemKind_Text: {
                    // Text after a preserved line feed belongs to the next paragraph.
                    uint32_t from = it.start > paragraph.start ? it.start : paragraph.start;
                    for (uint32_t b = from; b < it.end && b < paragraph.end; ++b)
                        units.byte(b, levelAt(b));
                    more = it.end <= paragraph.end;
                    break;
                }
                case LayoutInlineItemKind_Atomic:
                    units.fresh = true;
                    units.byte(it.start, levelAt(it.start));
                    units.fresh = true;
                    break;
                case LayoutInlineItemKind_Open:
                    open.push(item);
                    first.push(s.unitStarts.count);
                    if (t.info.marks[item] != kLayoutMarkNone)
                        units.push(it.start, t.info.marks[item]);
                    units.fresh = true;
                    break;
                case LayoutInlineItemKind_Close:
                    if (t.info.marks[item] != kLayoutMarkNone)
                        units.push(it.start, t.info.marks[item]);
                    units.fresh = true;
                    if (open.count != 0) {
                        boxes.push(open.last());
                        boxes.push(first.last());
                        boxes.push(s.unitStarts.count);
                        open.pop();
                        first.pop();
                    }
                    break;
                default: break;
            }
            if (more)
                ++item;
        }
        // Boxes that are still open at the end of the paragraph end at its last unit.
        for (uint32_t i = 0; i < open.count; ++i) {
            boxes.push(open.data[i]);
            boxes.push(first.data[i]);
            boxes.push(s.unitStarts.count);
        }
        uint32_t count = s.unitStarts.count;
        if (count < 2)
            continue;
        s.unitOrder.resize(count);
        s.unitVisual.resize(count);
        prUnicodeBidiReorder(s.unitLevels.data, count, s.unitOrder.data);
        for (uint32_t p = 0; p < count; ++p)
            s.unitVisual.data[s.unitOrder.data[p]] = p;
        for (uint32_t i = 0; i < boxes.count; i += 3) {
            uint32_t from = boxes.data[i + 1];
            uint32_t to = boxes.data[i + 2];
            if (to - from < 2)
                continue;
            // Finds the runs of consecutive visual positions among the units of the box. The box splits wherever its
            // text passes from one run to another.
            RawArray<uint32_t>& runs = s.positions;
            runs.clear();
            for (uint32_t u = from; u < to; ++u)
                runs.push(s.unitVisual.data[u]);
            sortItems(runs.data, runs.count, [](uint32_t a, uint32_t b) { return a < b; });
            auto runOf = [&](uint32_t u) {
                uint32_t at = upperBound(runs.data, runs.count, s.unitVisual.data[u]) - 1;
                uint32_t run = 0;
                for (uint32_t j = 1; j <= at; ++j)
                    run += runs.data[j] != runs.data[j - 1] + 1 ? 1 : 0;
                return run;
            };
            for (uint32_t u = from + 1; u < to; ++u) {
                if (runOf(u) != runOf(u - 1))
                    t.splits.push(LayoutBidiSplit{boxes.data[i], s.unitStarts.data[u]});
            }
        }
    }
    sortItems(t.splits.data, t.splits.count, [](const LayoutBidiSplit& a, const LayoutBidiSplit& b) {
        return a.open != b.open ? a.open < b.open : a.byte < b.byte;
    });
}

uint32_t layoutBidiSplitsBefore(const LayoutInlineShaping& t, uint32_t open, uint32_t byte) {
    // The splits are sorted by the Open item of their box, then by byte.
    const LayoutBidiSplit* splits = t.splits.data;
    uint32_t first = partitionPoint(0, t.splits.count, [&](uint32_t i) { return splits[i].open < open; });
    uint32_t end = partitionPoint(first, t.splits.count, [&](uint32_t i) {
        return splits[i].open == open && splits[i].byte <= byte;
    });
    return end - first;
}

const PrTextParagraph& layoutBidiParagraphAt(const LayoutInlineShaping& t, uint32_t byte) {
    PR_ASSERT(t.bidi && t.info.paragraphCount != 0);
    const PrTextParagraph* paragraphs = t.info.paragraphs;
    uint32_t after = partitionPoint(1, t.info.paragraphCount, [&](uint32_t i) { return paragraphs[i].start <= byte; });
    return paragraphs[after - 1];
}

} // namespace pr_impl
