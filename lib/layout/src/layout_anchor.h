// Scroll anchoring: keeping what a scrolled container shows in place as its content's layout changes
// (css-scroll-anchoring-1).
//
// A layout that runs chooses, before it lays out, an anchor for each scrolled container that has none, from the last
// layout's boxes. After it lays out, it moves each container's offset in its block axis by as far as the anchor moved,
// unless a change that suppresses anchoring reached the anchor. The cost is nothing for a tree whose containers are not
// scrolled, and a choice after each scroll that a layout follows.

#pragma once

#include "pr/array.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/layout_unit.h"

#include "layout/src/layout_index.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTree;

// A scrolled container's anchor.
struct LayoutAnchor {
    PrLayoutNode container;         // an element's handle, or kPrLayoutNodeNone for the viewport
    PrLayoutNode node;              // the anchor, or kPrLayoutNodeNone if none is chosen

    // The position of the block-start edge of the anchor's bounding rect, along the container's block axis, in the
    // container's unscrolled content, and the container's offset within its range, as of the layout that last measured
    // them. A layout that shrinks the range moves the offset from where it was, not from where the new range clamps it.
    PrLayoutUnit edge;
    PrLayoutVec offset;

    bool provisional;               // chosen among content that the last layout had not laid out
};

// The scroll anchoring of a tree.
struct LayoutAnchoring {
    Array<LayoutAnchor> anchors;    // of the containers whose offsets are not zero

    // The moves that anchoring made since a client last took them, and since the tree's last scene, one per container.
    Array<PrLayoutScrollAdjustment> untaken;
    Array<PrLayoutScrollAdjustment> unscened;

    // The nodes whose record changed, since the last layout, a value whose change suppresses anchoring along the path
    // from an anchor to its container (LayoutTreeNode::anchorSuppressed), and the nodes that became or stopped being
    // absolutely positioned, which suppress anchoring for every container around them.
    Array<LayoutNodeIndex> suppressing;
    Array<LayoutNodeIndex> repositioned;
};

// Frees the anchoring's storage.
void layoutAnchoringReset(LayoutAnchoring& anchoring);

// Records that the offset of `container`, an element or kPrLayoutNodeNone for the viewport, was set to `offset` from
// outside the tree, which drops its anchor.
void layoutAnchoringScrolled(LayoutTree* tree, LayoutNodeIndex container, PrLayoutVec offset);

// Records that the record of the element `node` changed from `was` to `style`, marking what suppresses anchoring.
void layoutAnchoringStyleChanged(LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex was, PrLayoutStyleIndex style);

// Chooses the anchors of the scrolled containers that have none, from the last layout. A layout calls this before it
// lays out.
void layoutAnchoringChoose(LayoutTree* tree);

// Moves the offset of each container whose anchor moved in the layout that just finished, and clears the marks. A
// layout calls this once it has published its boxes. `resized` is true if the layout was in a new viewport extent.
void layoutAnchoringAdjust(LayoutTree* tree, bool resized);

// Clears the marks, for a layout that had nothing to lay out and moved nothing.
void layoutAnchoringIdle(LayoutTree* tree);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
