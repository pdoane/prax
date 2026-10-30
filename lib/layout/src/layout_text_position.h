// Positions in text fragments: where a caret before an offset of a text node's text stands along a fragment's line,
// which offset a position along the line is nearest, and the rectangles that ranges of the text cover.
//
// A position along a fragment is its distance in px from the fragment's line-left edge. That edge is the left edge of a
// horizontal fragment, the top edge of a vertical one, and the bottom edge of a fragment turned counter-clockwise. A
// cluster is the unit that a caret stops at. An offset inside a cluster, as in a ligature, stands at the fraction of
// the cluster's advance that the bytes before it make of the cluster's bytes.

#pragma once

#include "pr/layout_style.h"

#include "layout/src/layout_fragment.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTree;

// Returns the first offset of the text fragment's bytes in the order of the text.
uint32_t layoutTextStart(const LayoutFragment* fragment);

// Returns the extent of the text fragment along its line, in px.
float layoutTextLength(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns the position along the text fragment of a caret before `offset`, which is clamped to the fragment's bytes. A
// caret at the end of a fragment in the order of the text stands at its line-right edge, or at its line-left edge if
// the fragment is reversed.
float layoutTextCaretAlong(const LayoutTree* tree, const LayoutFragment* fragment, uint32_t offset);

// Returns the offset of the caret position in the text fragment nearest `along`: an edge of the cluster under it.
uint32_t layoutTextOffsetAt(const LayoutTree* tree, const LayoutFragment* fragment, float along);

// Returns the rectangle of the text fragment at `at` between the positions `from` and `to` along it, across the whole
// fragment. `from` may be past `to`.
PrLayoutRect layoutTextSpanRect(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec at, float from, float to);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
