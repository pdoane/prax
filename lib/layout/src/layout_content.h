// Content: what the shell of a block-level box gives the algorithm that lays out the box's content, and what the
// algorithm gives back.
//
// css-display-4 - https://drafts.csswg.org/css-display-4/
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/
//
// Every block-level box has a shell (layout_block.h): its edges, its inline size, the block size that its style or its
// container decides, its content box, the clamp of a block size that its content decides, its fragment, the boxes out
// of flow that it holds, its overflow, and the baselines that it exports. Its content is laid out by one algorithm,
// which LayoutChildren names: flow (layout_flow.h), flex (layout_flex.h), grid (layout_grid.h) or table
// (layout_table.h). Flex and grid containers establish independent formatting contexts, and their algorithms take the
// protocol in this file. Flow takes its own, because a block container in its parent's context finds its position
// through its content. A table takes its own, because its wrapper box has no edges and holds its captions.

#pragma once

#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The block axis of a box's border box, as its own style and its container give it before its content is laid out.
struct LayoutBlockAxis {
    PrLayoutUnit size;              // negative if the content decides it
    PrLayoutUnit edges;             // the block-axis padding and borders
    PrLayoutUnit stretch;           // the stretch-fit size, or negative if indefinite
    bool growsToContent;            // true if the size comes from an aspect ratio, and the content may exceed it
};

// What a layout of a box's content is for. A layout either builds the fragments of the content, or solves the content
// alone for a measurement of the box, to find its size and baselines or its size alone. Only flex and grid containers
// are solved alone.
enum {
    LayoutContentGoal_Build,
    LayoutContentGoal_Baselines,
    LayoutContentGoal_Size,
};
typedef uint8_t LayoutContentGoal;

// What the shell of a box that establishes an independent formatting context gives the layout of its content. `content`
// is the box's content box, which is the space that the content is laid out in. Its percentages resolve against its
// block size if that size is definite. `blockSize` is the block size if the box's style or request decides it.
// Otherwise `minBlock` and `maxBlock` limit the block size that the content decides.
struct LayoutContentInput {
    const LayoutSpace* content;
    LayoutLogicalOffset origin;             // the start corner of the content box in the box's border box
    PrLayoutUnit padding[LayoutSide_Count]; // the box's padding, by LayoutSide in its own mode
    PrLayoutUnit blockSize;                 // of the content box; negative if the content decides it
    PrLayoutUnit minBlock;                  // of the content box; zero if there is none
    PrLayoutUnit maxBlock;                  // negative if there is none
    bool scrolls;                           // true for a scroll container, whose scrollable overflow holds the item
                                            // margin boxes
    LayoutContentGoal goal;
};

// What the content of a box that establishes an independent formatting context gives its shell: the block size of the
// content, before the box's limits clamp it, and the box's first and last baselines, measured from the block-start edge
// of its content box. A baseline comes from the fragment of an item if the content was built, and from the measurement
// of an item if it was solved for its baselines. There are no baselines if it was solved for its size alone.
struct LayoutContentOutcome {
    PrLayoutUnit contentBlock;
    LayoutBaseline first;
    LayoutBaseline last;

    // The bounds of the margin boxes of the items in the content box, measured from its start corner, if the items were
    // built in a scroll container. The box's scrollable overflow holds them (css-overflow-3/#scrollable-overflow). Both
    // are zero if there are no items or the box does not scroll.
    LayoutLogicalOffset itemsStart;
    LayoutLogicalOffset itemsEnd;
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
