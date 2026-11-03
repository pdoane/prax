// Positioning: boxes out of flow, held by the boxes that are their containing blocks, and laid out and placed once
// those boxes are sized.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-position-3 - https://drafts.csswg.org/css-position-3/
//
// A box out of flow takes no part in its parent's layout, except for a placeholder at its static position. Each
// fragment lists the boxes out of flow in its subtree that it does not hold. A box holds the listed boxes that its
// fragment gathers and that it is the containing block of, nearest first (layoutHoldsAbsolute, layoutHoldsFixed). It
// also holds those whose containing block is a positioned inline box in its lines. A holder lays out what it holds once
// its own size is known, against its padding box or against the inline box, and its fragment holds their fragments
// after its children in flow. The viewport holds the boxes that reach it.

#pragma once

#include "layout/src/layout_grid.h"
#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The part of a box that a static position aligns in one logical axis of its static context: its start edge, its end
// edge, or its center.
enum {
    LayoutStaticAlign_Start,
    LayoutStaticAlign_End,
    LayoutStaticAlign_Center,
};
typedef uint8_t LayoutStaticAlign;

// Returns the edges that a static position gives in a static context of `mode`: the inline-start and block-start edges.
LayoutStaticEdges layoutStaticEdges(LayoutMode mode);

// Returns the edges that a static position gives in a static context of `mode`, if the position aligns the box by
// `inlineAlign` in the context's inline axis and by `blockAlign` in its block axis. In an axis whose alignment is safe,
// a box that overflows aligns at the start.
LayoutStaticEdges layoutStaticEdgesAligned(LayoutMode mode, LayoutStaticAlign inlineAlign,
    LayoutStaticAlign blockAlign, bool inlineSafe, bool blockSafe);

// Lays out the box out of flow that `pending` lists, in its containing block `containing`. Returns its fragment, with a
// reference that the caller takes, positioned from the holder's top-left corner (css-position-3/#abspos-layout).
LayoutChild layoutAbsolute(LayoutState& state, const LayoutPending& pending, const LayoutAbsoluteBlock& containing);

// Creates the fragment of the block container `node`, of size `size` in its writing mode `mode`, from the scratch
// children from `mark` on, in the same way as layoutFragmentBuild. The boxes out of flow that it holds are laid out
// against its padding box, against the positioned inline boxes in its lines, or against the grid areas that their
// placements name if `node` is a grid container whose layout kept `grid` (css-grid-1/#abspos-items), in the fallback
// that its content is given. The rest stay in its list.
LayoutFragment* layoutHolderBuild(LayoutState& state, LayoutNodeIndex node, uint32_t mark, LayoutMode mode,
    LayoutLogicalSize size, PrLayoutVec fallback, const LayoutGridLines* PR_NULLABLE grid = nullptr);

// Lays out the boxes out of flow listed by the root's fragment `root`, which is at `at` in the viewport. Each is laid
// out against the initial containing block or the viewport, which has size `extent` at the origin in the root's writing
// mode `mode`. Appends their fragments to `out`.
void layoutViewportHold(LayoutState& state, const LayoutFragment* root, PrLayoutVec at, PrLayoutVec extent,
    LayoutMode mode, RawArray<LayoutChild>& out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
