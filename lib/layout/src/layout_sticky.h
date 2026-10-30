// Sticky positioning: the offset that keeps a sticky box in its scrollport as its scroll container scrolls.
//
// css-position-3 - https://drafts.csswg.org/css-position-3/
//
// A sticky box is laid out as a relatively positioned box with no offset (css-position-3/#stickypos-insets). Its offset
// depends on the scroll offset of its nearest scroll container, which layout does not read, so the offset applies where
// scroll offsets do. A sticky box paints in a container of its own (layoutNodeContainer), which moves the box and what
// paints in it, and which a scene moves as it plays, from the offset of its scroll container. Hit testing and geometry
// in viewport coordinates move it by the tree's offsets. A box out of flow that a box outside the sticky box holds does
// not move with it.
//
// A sticky box inside another, in the same scroll container, moves with the outer box: its box, and its containing
// block if the outer box holds it, are moved by the outer box's offset before its own offset is found.

#pragma once

#include "layout/src/layout_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What decides a sticky box's offset, from the last layout. The rectangles are in one space, in which the scroll
// container's content moves back by its scroll offset.
struct LayoutSticky {
    PrLayoutRect port;              // the scrollport less the scroll container's padding, unscrolled
    PrLayoutRect box;               // the border box

    // The rectangle that the border box stays in: its containing block's content box less its margins, grown to hold
    // the border box where it was laid out (css-position-3/#position-box).
    PrLayoutRect limit;

    // The insets of the sticky view rectangle from the scrollport's sides, by PrRectSide. An inset that is auto is
    // zero, and moves nothing.
    PrLayoutUnit inset[PrRectSide_Count];
    uint8_t sides;                  // the sides whose inset is not auto, as bits by PrRectSide

    // The end side of each axis in the containing block's writing mode, as bits by PrRectSide. A sticky view rectangle
    // smaller than the border box grows past that side.
    uint8_t ends;

    // The number of the sticky boxes nearest around it, in its scroll container, that do not hold its containing block,
    // as a sticky table track does not hold the table of the cells in it. Their moves do not move the limit.
    uint32_t limitSkips;
};

// Returns true and sets `out` to what decides the offset of the sticky box `node` (layoutNodeSticks) in the last
// layout. Its scroll container is the nearest around it, or the viewport. A box in a fixed box that the viewport holds
// is in the viewport as it is unscrolled. Returns false if its offset is always zero: its insets are all auto, or it is
// in a root other than the viewport's, which nothing scrolls.
bool layoutStickyFind(const LayoutTree* tree, LayoutNodeIndex node, LayoutSticky* out);

// Returns how far a sticky box moves from where it was laid out, at its scroll container's offset `scroll`, if the
// sticky boxes around it in the same scroll container move it by `around` and its limit by `limitAround`
// (LayoutSticky::limitSkips). The offset does not include `around`.
PrLayoutVec layoutStickyShift(const LayoutSticky& sticky, PrLayoutVec scroll, PrLayoutVec around,
    PrLayoutVec limitAround);

// Returns how far the sticky box `node` moves from where it was laid out at the tree's scroll offsets, not counting the
// sticky boxes around it, which move it as well. The cost grows with the number of sticky boxes around it in its scroll
// container.
PrLayoutVec layoutNodeStickyShift(const LayoutTree* tree, LayoutNodeIndex node);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
