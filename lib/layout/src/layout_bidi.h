// Bidirectional text: finding where reordering splits the inline boxes of an inline formatting context.
//
// css-writing-modes-4 - https://drafts.csswg.org/css-writing-modes-4/
// UAX9 - https://www.unicode.org/reports/tr9/
//
// Levels are resolved once, when the context is itemized (pr/text_itemize.h). Text in an ltr container that uses only
// left-to-right scripts resolves no levels. Every level would be even, and lines are placed in the order of the text.

#pragma once

#include "layout/src/layout_shaping.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Finds where reordering each paragraph of `t`, laid out on one unbroken line, splits its inline boxes
// (csswg-drafts#1509), and writes the splits to `t.splits`. Reads `t.levels`, `t.paragraphs` and `t.marks`.
//
// The levels of the paragraph are adjusted by rule L1. The paragraph is then divided into units. Each unit has one
// level and lies in the same boxes throughout. An empty unit is added at each isolate initiator and terminator, at the
// level of that initiator or terminator. The units are reordered by their levels (L2). The units of a box then fall in
// runs of consecutive visual positions, and the box splits wherever the order of its text passes from one run to
// another.
void layoutBidiFindSplits(LayoutInlineShaping& t, LayoutShapingScratch& s);

// Returns how many splits of the inline box whose Open item is `open` are at or before byte `byte` of its content. The
// result is the index of the piece of the box, on one unbroken line, that holds the byte.
uint32_t layoutBidiSplitsBefore(const LayoutInlineShaping& t, uint32_t open, uint32_t byte);

// Returns the paragraph of `t` that holds byte `byte`, or the last paragraph if the byte is past them all. Requires
// `t.bidi` to be set.
const PrTextParagraph& layoutBidiParagraphAt(const LayoutInlineShaping& t, uint32_t byte);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
