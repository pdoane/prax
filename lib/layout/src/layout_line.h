// Lines: the content of a line of an inline formatting context, as found by line breaking. Line alignment and
// justification read this content.

#pragma once

#include "pr/raw_array.h"

#include "layout/src/layout_index.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An item on a line. For a text item, the bounds are the bytes of the item that the line draws. They exclude the
// collapsible spaces that end the line, and the range may be empty. For any other item, both bounds are the item's
// position in the text.
struct LayoutLineItem {
    LayoutItemIndex item;
    uint32_t from;
    uint32_t to;
};

// The start of a line: its first byte, the item that contains that byte or the first item after it, and the number of
// bytes of that item that earlier lines placed.
struct LayoutLineStart {
    uint32_t byte;
    LayoutItemIndex item;
    uint32_t consumed;
};

// Returns true if two lines start at the same place.
inline bool operator==(LayoutLineStart a, LayoutLineStart b) {
    return a.byte == b.byte && a.item == b.item && a.consumed == b.consumed;
}

// The content of the line from `from` to byte `end` of a run's text.
struct LayoutLineContent {
    LayoutLineStart from;
    LayoutLineStart next;           // where the line after it starts
    uint32_t end;
    uint32_t segmentEnd;               // the end of the line's segment, which no grapheme cluster crosses
    RawArray<uint32_t> carried;        // Open items of boxes still open from earlier lines, outermost first
    RawArray<LayoutLineItem> items;    // in order
};

// Frees the memory that the line's content holds.
inline void layoutLineContentReset(LayoutLineContent& line) {
    line.carried.reset();
    line.items.reset();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
