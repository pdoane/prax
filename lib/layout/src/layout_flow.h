// Flow layout: the content of a block container, laid out in normal flow. Its block-level children are placed as
// blocks, its floats beside them, and its runs of inline-level children as inline formatting contexts.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-display-4 - https://drafts.csswg.org/css-display-4/
//
// A block container's shell (layout_block.h) finds its inline size and its content box, and this file lays out its
// children in order, each placed past the content and margins before it. The content's end then decides the block size
// that the shell clamps.
//
// While the container's block-start margin collapses with the margins of its children, the position of its border box
// in its block formatting context is not known, and the layout of its children finds it (layout_margin.h). That can
// stop its layout, or start it over, to place the floats that wait for the margins. So unlike the content of an
// independent formatting context (layout_content.h), flow gives its shell where the box went in its context and which
// margins collapse through its edges.

#pragma once

#include "layout/src/layout_box.h"
#include "layout/src/layout_content.h"
#include "layout/src/layout_sizing.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What the shell of a block container gives the layout of its content in flow.
struct LayoutFlowInput {
    LayoutNodeIndex node;           // the container, or the parent of the run that an anonymous box wraps

    // An anonymous box: the first and last node of the siblings that it wraps. Otherwise kPrLayoutNodeNone, and the
    // content is the container's children.
    LayoutNodeIndex runFirst;
    LayoutNodeIndex runLast;

    const PrLayoutBoxStyle* box;
    const LayoutSpace* space;       // the space that the container is laid out in
    const LayoutSpace* content;     // its content box, the containing block of its children
    LayoutOuterMode children;       // the mode of its content, which its children are laid out in
    LayoutLogicalOffset origin;     // the start corner of its content box in its border box
    PrLayoutUnit inlineSize;        // of its border box

    // Its style and edges in the mode whose inline axis is its own, and its edges in its own mode and in its containing
    // block's mode.
    const LayoutLogicalBox* sizing;
    const LayoutEdges* inner;
    const LayoutEdges* outer;

    LayoutBlockAxis axis;
    bool clamps;                    // true if its limits clamp a block size that its content decides
    bool fixedBlock;                // true if its style or its container fixes its block size, which aligns its content
    bool marked;                    // true if it may be a list item, whose outside marker it lays out

    // Set if it is in flow in a context that it does not establish: what its subtree reads of that context.
    LayoutReads* PR_NULLABLE reads;
};

// What the content of a block container in flow gives its shell.
struct LayoutFlowOutcome {
    // Where the margins before the container resolve, if its layout stopped there with floats waiting for them, or
    // kLayoutOffsetNone. The content then made no fragments.
    PrLayoutUnit stopAt;

    PrLayoutUnit contentBlock;      // the end of its in-flow content in its content box, before alignment moved it
    PrLayoutUnit blockSize;         // of its border box, clamped
    PrLayoutUnit shift;             // how far alignment moved its content along its block axis
    bool collapsedThrough;

    // The margins of its content that collapse through its block-start edge, and those that collapse through its
    // block-end edge. Neither includes its own.
    LayoutStrut start;
    LayoutStrut end;

    // In a context that it does not establish: its border box's block-start edge, if its layout found it, or
    // kLayoutOffsetNone; whether clearance put it there; and the floats that it holds unplaced while it collapses
    // through.
    PrLayoutUnit top;
    bool cleared;
    LayoutLineSides unplaced;

    // Its first and last baselines, measured from the block-start edge of its border box.
    LayoutBaseline first;
    LayoutBaseline last;
};

// Lays out the content of the block container that `input` describes, and appends the fragments of its children to the
// scratch children, at their logical offsets in its border box. A list item's outside marker comes first among them.
LayoutFlowOutcome layoutFlowContent(LayoutState& state, const LayoutFlowInput& input);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
