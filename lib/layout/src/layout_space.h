// Layout's protocol: what a box is laid out and measured in, what it gives back, and what every step of a layout reads.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-align-3 - https://drafts.csswg.org/css-align-3/
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/
//
// Layout runs in logical terms, with an inline axis along lines and a block axis across them, and writes physical
// fragments. A box lays out in the writing mode of its containing block's space, and its content lays out in the box's
// own writing mode. Its children are placed at logical offsets, which become physical offsets when its fragment is made
// (layout_fragment.h).

#pragma once

#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/text.h"

#include "layout/src/layout_cache.h"
#include "layout/src/layout_exclusion.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_fragmentation.h"
#include "layout/src/layout_logical.h"
#include "layout/src/layout_margin.h"
#include "layout/src/layout_style.h"
#include "layout/src/layout_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutFlexScratch;
struct LayoutGridScratch;
struct LayoutInlineScratch;
struct LayoutTableScratch;
struct LayoutBandsMemo;
struct LayoutInlineShaping;
struct LayoutShapingScratch;

// ---------------------------------------------------------------------------------------------------------------------
// Spaces

// The position of a box in flow in a block formatting context that it does not establish (CSS2/#block-formatting): its
// containing block among the context's floats, which its layout adds to, and the margins before it, which its own
// margins join.
struct LayoutBfc {
    LayoutContainingBlock containing;
    LayoutMarginChain chain;
};

// What a container asks of a box's size in one of the box's own axes. A box in flow sizes itself by its style. A flex
// container decides an item's sizes, or asks for the size of its content.
enum {
    LayoutRequestKind_Own,          // the box sizes itself by its style, as in flow
    LayoutRequestKind_Fixed,        // the border-box size its container decides; its preferred size and limits unread
    LayoutRequestKind_Content,      // block axis only: its content's size, its preferred size and limits unread
};
typedef uint8_t LayoutRequestKind;

// A request in one of a box's own axes.
struct LayoutRequest {
    PrLayoutUnit size;              // Fixed: the border-box size
    LayoutRequestKind kind;
    bool definite;                  // Fixed: true if the content's percentages resolve against the size
};

// Returns true if two requests are the same.
inline bool layoutRequestEquals(const LayoutRequest& a, const LayoutRequest& b) {
    return a.kind == b.kind && (a.kind != LayoutRequestKind_Fixed || (a.size == b.size && a.definite == b.definite));
}

// What a flex container asks of an item's sizes, in the item's own axes. A box whose inline size is Fixed shares no
// auto margins: its container resolves them.
struct LayoutRequests {
    LayoutRequest inlineAxis;
    LayoutRequest blockAxis;
};

// Returns true if two sets of requests are the same.
inline bool layoutRequestsEqual(const LayoutRequests& a, const LayoutRequests& b) {
    return layoutRequestEquals(a.inlineAxis, b.inlineAxis) && layoutRequestEquals(a.blockAxis, b.blockAxis);
}

// Returns what `requests` asks in the box's inline axis, or in its block axis. Returns Own if there are no requests, as
// for a box that is not a flex item and sizes itself by its style.
inline LayoutRequest layoutInlineRequest(const LayoutRequests* PR_NULLABLE requests) {
    return requests != nullptr ? requests->inlineAxis : LayoutRequest{};
}
inline LayoutRequest layoutBlockRequest(const LayoutRequests* PR_NULLABLE requests) {
    return requests != nullptr ? requests->blockAxis : LayoutRequest{};
}

// What a box is laid out in, stated in the box's own axes. Every size that the box sizes itself by is in its own inline
// and block axes, whatever its containing block's axes are, and what its parent places it by is in the containing
// block's axes. For a box in its containing block's flow, the bases equal `containing`. An orthogonal flow root has
// them crossed (orthogonalSpace in layout_block.cpp), and so may a box out of flow (layoutAbsolute).
struct LayoutSpace {
    LayoutMode mode;         // the containing block's mode, which the margins, insets and placement are in

    // What an auto size is in each of the box's axes.
    LayoutAutoSize autoInline;
    LayoutAutoSize autoBlock;

    // What an auto inline size fills, along with auto inline margins if the box is in its containing block's flow: the
    // containing block's content inline size, or the space beside the floats in which a box that avoids them is placed.
    PrLayoutUnit available;

    PrLayoutUnit percentInline;     // what the box's inline-axis size percentages resolve against
    PrLayoutUnit percentBlock;      // what its block-axis size percentages resolve against; negative if indefinite

    // The containing block's content inline and block sizes, in its own axes, each negative if indefinite. The box's
    // margins and padding resolve against the inline size, as does its inline size, and each inset resolves against the
    // size of its own axis.
    LayoutLogicalSize containing;

    // The most that an orthogonal flow root in the containing block fills if the containing block's block size is
    // indefinite: its inner max block size, floored by its inner min block size, or negative if it has no fixed max.
    PrLayoutUnit blockLimit;

    // The physical extent that an orthogonal flow root below takes if its containing block's block size is indefinite
    // and `blockLimit` does not bound it: the nearest scroll container's fallback, or the initial containing block's
    // size (css-writing-modes-4/#orthogonal-auto). A result reports reading it (LayoutResult::readsFallback).
    PrLayoutVec fallback;

    // For a box out of flow, the margin box block size that a stretch-fit block size fills, which is the size of its
    // inset-modified containing block and is never indefinite.
    PrLayoutUnit stretchBlock;

    const LayoutBfc* PR_NULLABLE bfc;   // set if it is in flow in a context it does not establish

    // The outside markers that the box's first line holds (PrLayoutDisplay_Marker): the extent of their margin boxes
    // about the alphabetic baseline, or empty for none.
    LayoutLineExtent marker;

    // What a table gives a table cell beyond its size requests, or null for any other box. The inputs stay valid during
    // the call that receives the space. A cached result is keyed on them.
    const LayoutCellSpace* PR_NULLABLE cell;

    // What a fragmentation context gives the box, or null outside one. It stays valid during the call that receives the
    // space. A cached result is never laid out with it: one that a fragmentainer does not break is the result of an
    // unfragmented layout.
    const LayoutFragmentation* PR_NULLABLE fragmentation;

    // The stretch size constraint that a math parent lays out an embellished operator with, or null for none. It stays
    // valid during the call that receives the space. A cached result is keyed on it.
    const LayoutMathStretch* PR_NULLABLE stretch;
};

// A box's space is copied for each child, so its fields are ordered to leave no padding.
static_assert(sizeof(LayoutSpace) == 80);

// Returns the fragmentainer that a box laid out in `space` is laid out in, or null outside a fragmentation context.
inline const LayoutFragmentainer* PR_NULLABLE layoutSpaceFragmentainer(const LayoutSpace& space) {
    return space.fragmentation != nullptr ? space.fragmentation->fragmentainer : nullptr;
}

// Returns the token that a box laid out in `space` resumes from, or null for its first fragment or outside a
// fragmentation context.
inline const LayoutBreakToken* PR_NULLABLE layoutSpaceResume(const LayoutSpace& space) {
    return space.fragmentation != nullptr ? space.fragmentation->resume : nullptr;
}

// Returns the cell inputs of `space`, which are empty for a box that is not a table cell.
inline LayoutCellSpace layoutCellSpaceFromSpace(const LayoutSpace& space) {
    return space.cell != nullptr ? *space.cell : LayoutCellSpace{};
}

// Returns the stretch size constraint of `space`, which has no axis for a box that is not asked to stretch.
inline LayoutMathStretch layoutMathStretchFromSpace(const LayoutSpace& space) {
    return space.stretch != nullptr ? *space.stretch : LayoutMathStretch{};
}

// Returns true if two spaces hold equal stretch size constraints, or neither holds one.
inline bool layoutSpaceStretchesEqual(const LayoutSpace& a, const LayoutSpace& b) {
    if (a.stretch == b.stretch)
        return true;
    return a.stretch != nullptr && b.stretch != nullptr && layoutMathStretchesEqual(*a.stretch, *b.stretch);
}

// Returns true if a box laid out in `space` is out of flow, so that its containing block sizes the space.
inline bool layoutSpaceIsOutOfFlow(const LayoutSpace& space) {
    return space.autoInline != LayoutAutoSize_Style;
}

// Returns true if a percentage of `base` resolves. A block-axis base is negative if it is indefinite.
inline bool layoutIsDefinite(PrLayoutUnit base) {
    return base >= kLayoutUnitZero;
}

// ---------------------------------------------------------------------------------------------------------------------
// Results

// What laying out a block-level box gives its parent.
struct LayoutResult {
    const LayoutFragment* PR_NULLABLE fragment; // a reference the caller takes; null if it stopped

    // The margins that collapse through its block-start edge, including its own, and those that collapse through its
    // block-end edge. If the two are adjoining, the margins after it collapse with both.
    LayoutStrut start;
    LayoutStrut end;

    PrLayoutUnit marginInlineStart; // where its border box starts within its containing block's content box

    // In a context that it does not establish: its border box's block-start edge, if its layout found it, or
    // kLayoutOffsetNone. `cleared` says whether clearance put it there.
    PrLayoutUnit top = kLayoutOffsetNone;

    // Where the margins before it resolve, if it stopped there with floats waiting for them, or kLayoutOffsetNone. The
    // box that holds the floats lays out again with the margins resolved.
    PrLayoutUnit resolution = kLayoutOffsetNone;

    // What it read of its context, which its parent records when it places it: a block range from its border box's
    // block-start edge, empty if readLo >= readHi. A cached result that reads nothing at its position gives the range
    // it read when it was laid out, because only placing it says where that is.
    PrLayoutUnit readLo;
    PrLayoutUnit readHi;

    LayoutResultFlags flags;
    bool cleared;

    // The unplaced floats that it holds while collapsing through. Its parent lays it out again once it knows where to
    // place them.
    LayoutLineSides unplaced;

    // Where the box resumes in the next fragmentainer, a reference that the caller takes, or null if it finished in the
    // fragmentainer of its space (layout_fragmentation.h).
    const LayoutBreakToken* PR_NULLABLE token;
};

// Returns true if a layout stopped where the margins before the box resolve, and created no fragment.
inline bool layoutResultStopped(const LayoutResult& result) {
    return result.resolution != kLayoutOffsetNone;
}

// What laying out a box that is placed apart from the flow -- an atomic inline, or a float -- gives its parent.
struct LayoutAtomic {
    const LayoutFragment* PR_NULLABLE fragment; // a reference the caller takes
    PrLayoutUnit margin[LayoutSide_Count];      // by LayoutSide; auto margins are zero

    // The baseline it aligns by, from its border box's line-over edge (css-inline-3/#baseline-source). This is the
    // baseline that its baseline-source picks, with its baseline set. If it has none, it is its margin box's line-under
    // edge, with a set synthesized from its margin box (CSS2/#leading). An atomic inline's margins, baseline and
    // relative offset are in the frame that its line places it in (layoutLineFrame), and a float's are in its
    // containing block's writing mode.
    LayoutBaseline baseline;
    LayoutLogicalOffset relative;   // where relative positioning moves it from its place
};

// What measuring a box gives (layoutMeasure): the block size of its border box in its own block axis, and its baselines
// as it exports them, if the measurement asked for them.
struct LayoutMeasurement {
    PrLayoutUnit blockSize;
    LayoutBaseline first;
    LayoutBaseline last;
};

// Returns the margin box of an atomic layout, in `mode`.
inline LayoutLogicalSize layoutAtomicMarginBox(const LayoutAtomic& atomic, LayoutMode mode) {
    LayoutLogicalSize size = layoutLogicalSize(mode, atomic.fragment->extent);
    const PrLayoutUnit* margin = atomic.margin;
    return LayoutLogicalSize{
        margin[LayoutSide_InlineStart] + size.inlineSize + margin[LayoutSide_InlineEnd],
        margin[LayoutSide_BlockStart] + size.blockSize + margin[LayoutSide_BlockEnd],
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Intrinsic sizes

// What intrinsic sizing measures a box in (css-sizing-3/#intrinsic-sizes): its containing block, as a space states it,
// but with no inline base, because inline percentages are cyclic while the box's contribution decides the size of its
// containing block.
struct LayoutIntrinsicInput {
    LayoutMode mode;                // the containing block's writing mode, whose inline axis is the box's
    PrLayoutUnit percentBlock;      // what the box's block-axis percentages resolve against; negative if indefinite
    PrLayoutUnit blockLimit;        // same as LayoutSpace::blockLimit
    PrLayoutVec fallback;           // same as LayoutSpace::fallback
};

// Returns the input that measures a box laid out in `space`: its containing block, but with no inline base.
inline LayoutIntrinsicInput layoutIntrinsicInputFromSpace(const LayoutSpace& space) {
    return LayoutIntrinsicInput{space.mode, space.percentBlock, space.blockLimit, space.fallback};
}

// A box's min-content and max-content inline sizes or contributions, and which parts of the input they read, so that a
// measurement with another input may differ.
struct LayoutIntrinsicSizes {
    PrLayoutUnit minContent;
    PrLayoutUnit maxContent;
    bool readsPercentBlock;
    bool readsFallback;

    // True if the box's own fit-content size fills the available space, whatever its max-content size. A table sets it
    // if its percentage columns ask for more than the whole table. The max-content size stays what the content needs,
    // and is what the box contributes to its container.
    bool fillsAvailable;
};

// ---------------------------------------------------------------------------------------------------------------------
// State

// A containing block of boxes out of flow: its rectangle from its holder's top left, its writing mode, and the fallback
// that the content of its holder is given (LayoutSpace::fallback).
struct LayoutAbsoluteBlock {
    PrLayoutRect rect;
    LayoutMode mode;
    PrLayoutVec fallback;
};

// The containing block that a positioned inline box forms, found once while its holder lays out the boxes it holds.
struct LayoutInlineBlock {
    LayoutNodeIndex node;
    LayoutAbsoluteBlock block;
};

// A layout of a box that a measurement made during the running layout. The box's final layout takes it if its space is
// the same (layoutFinal). It holds a reference to the result's fragment.
struct LayoutHandoff {
    LayoutNodeIndex node;
    LayoutSpace space;
    LayoutRequests requests;
    LayoutResult result;
};

// A measurement made by a layout that keeps no records (LayoutState::reuse). The layout gives it again for the same
// request while it runs: nothing changes during a layout, so a box asked the same question again gives the same answer.
struct LayoutMemo {
    LayoutNodeIndex node;
    LayoutSpace space;
    LayoutRequests requests;
    LayoutMeasureKind kind;
    LayoutMeasurement answer;
};

// Intrinsic sizes found by a layout that keeps no records. The layout gives them again for the same input while it
// runs, in the same way as measurements (LayoutMemo).
struct LayoutIntrinsicMemo {
    LayoutNodeIndex node;
    LayoutIntrinsicInput input;
    LayoutIntrinsicSizes sizes;
};

// The scratch memory a tree keeps between layouts.
struct LayoutScratch {
    RawArray<LayoutLogicalChild> children; // the children of fragments being made, innermost last

    // The boxes out of flow that the holders being made gathered, the fragments of those they hold, and the containing
    // blocks of inline boxes that they found, each innermost last.
    RawArray<LayoutPending> pending;
    RawArray<LayoutChild> held;
    RawArray<LayoutInlineBlock> inlineBlocks;

    PrTextGlyphBuffer* glyphs;
    LayoutShapingScratch* shaping;
    LayoutInlineScratch* PR_NULLABLE inlines;     // made the first time a layout reaches an inline formatting context
    RawArray<LayoutInlineShaping*> shapings;      // the inline formatting contexts layoutShapeAhead shapes
    RawArray<LayoutHandoff> handoffs;             // the running layout's handoffs, released at its end
                                                  // (layoutEndLayout)
    HashIndex handoffIndex;                       // the handoffs not yet taken, by KeyIndex::hashOf their node
    RawArray<LayoutMemo> memos;                   // the running layout's memos, which its end empties (layoutEndLayout)
    HashIndex memoIndex;                          // the memos by KeyIndex::hashOf their node
    RawArray<LayoutIntrinsicMemo> intrinsicMemos; // the running layout's intrinsic memos, which its end empties
    HashIndex intrinsicMemoIndex;                 // the intrinsic memos by KeyIndex::hashOf their node
    RawArray<LayoutBandsMemo*> bands;             // the running layout's flex and grid content in fragmentation
                                                  // contexts, which its end frees (layoutEndLayout)
    HashIndex bandIndex;                          // the band memos by KeyIndex::hashOf their node
    LayoutFlexScratch* PR_NULLABLE flex;          // made the first time a layout reaches a flex container
    LayoutTableScratch* PR_NULLABLE table;        // created the first time that a layout reaches a table
    LayoutGridScratch* PR_NULLABLE grid;          // made the first time a layout reaches a grid container

    // The boxes that the running layout laid out with break tokens, whose results no cache entry keeps, and whose marks
    // its end clears (layoutEndLayout), since it laid them out from their current inputs.
    RawArray<LayoutNodeIndex> broken;

    // What the math boxes being made draw beside their children, innermost last (layoutMathFinish).
    RawArray<LayoutMathPaint> mathPaints;
    RawArray<PrFontGlyph> mathGlyphs;
};

// Creates empty scratch memory, and frees it.
LayoutScratch* layoutScratchCreate(void);
void layoutScratchDestroy(LayoutScratch* PR_NULLABLE scratch);

// What every step of a layout reads.
struct LayoutState {
    LayoutTree* tree;
    LayoutStyles* styles;
    LayoutScratch* scratch;
    PrLayoutTreeStats* stats;
    PrLayoutVec viewport;           // the extent of the layout, the initial containing block's

    // The least that a fragmentainer would have to grow by for the content that a break pushed past its end to fit, the
    // least over the breaks since a balancing column set last reset it, or INT32_MAX if none pushed anything.
    PrLayoutUnit fragmentShortage = PrLayoutUnit{INT32_MAX};

    // Whether anything laid out or measured since the innermost box that clears this flag read the fallback of the
    // space or input it was given (LayoutSpace::fallback). A box clears the flag before its content, and sets it again
    // afterward if it was set before, so that each box learns of its own subtree's reads.
    bool readsFallback;

    // Results are taken from the boxes' caches if they hold, and written to them. A layout from scratch neither reads
    // nor writes the caches, and does not read what has changed.
    bool reuse;
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
