// Block layout: the shell of every block-level box, which sizes the box and finishes it around its content, and the
// initial containing block.
//
// css-box-4 - https://drafts.csswg.org/css-box-4/
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/
//
// A block-level box is laid out in three steps. Its shell finds its inline size from its containing block, the block
// size that its style or its container decides, and its content box. The algorithm that its children name lays out its
// content (layout_content.h). The shell then finds its block size from its content, and makes its fragment.

#include "layout/src/layout_block.h"

#include "pr/alloc.h"
#include "pr/sort.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_cache.h"
#include "layout/src/layout_flex.h"
#include "layout/src/layout_flow.h"
#include "layout/src/layout_fragmentation.h"
#include "layout/src/layout_grid.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_multicol.h"
#include "layout/src/layout_page.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_replaced.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

// What a box's content gave its shell, which the shell finishes the box from.
struct LayoutShellContent {
    PrLayoutUnit blockSize;                         // of the border box
    PrLayoutUnit contentBlock;                      // the end of the in-flow content in the content box, before
                                                    // alignment moved it
    PrLayoutUnit shift;                             // how far alignment moved the content along the block axis
    LayoutBaseline edges[LayoutBaselineEdge_Count]; // by LayoutBaselineEdge: the box's baselines, measured from the
                                                    // block-start edge of its border box

    // The bounds of the margin boxes of the items that a scroll container holds (LayoutContentOutcome).
    LayoutLogicalOffset itemsStart;
    LayoutLogicalOffset itemsEnd;
};

// The content of a flex or grid container that the running layout laid out in a fragmentation context, which its later
// fragmentainers take instead of laying the container's items out again. Its key is the container's content space and
// what its shell gives its content. Nothing changes during a layout, so the memo holds until the layout's end frees it.
// The children are references that the memo holds, in the order that the content left them.
struct LayoutBandsMemo {
    LayoutNodeIndex node;
    LayoutNodeIndex parent;
    LayoutSpace space;
    LayoutContentInput input;
    PrLayoutUnit axisSize;
    PrLayoutUnit axisEdges;
    PrLayoutUnit percentBlock;
    bool clamps;

    LayoutShellContent content;
    LayoutGridLinesCopy* PR_NULLABLE lines;
    RawArray<LayoutLogicalChild> children;
    RawArray<uint32_t> band;            // by child: its band, in block order
    RawArray<uint32_t> bandFirst;       // by band: its first member, and the end of the members after the last band
    RawArray<uint32_t> members;         // the children of each band in turn
    RawArray<PrLayoutUnit> starts;      // by band: its block-start offset
    RawArray<PrLayoutUnit> ends;        // by band: its block-end offset
};

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Sizes

// Returns the justify-self of the in-flow block-level box `node` (css-align-3/#justify-block). This is its own value
// or, if that is auto, the justify-items of its parent. An inline box has no justify-items, so a block inside one takes
// normal (csswg-drafts#11462).
PrLayoutAlignment justifySelfOf(const LayoutState& state, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    if (n.justifies)
        return layoutFlexStyle(state.styles, n.style).justifySelf;
    // Its justify-self is auto or normal, and auto takes the justify-items of the parent.
    if (n.parent == kPrLayoutNodeNone || !layoutTreeNode(state.tree, n.parent).justifiesItems ||
            (layoutFlexStyle(state.styles, n.style).justifySelf & kPrLayoutAlignmentValueMask) ==
            PrLayoutAlignment_Normal || layoutIsInlineBox(state.tree, n.parent))
        return PrLayoutAlignment_Normal;
    return layoutFlexStyle(state.styles, layoutTreeNode(state.tree, n.parent).style).justifyItems;
}

// Returns the share of `free` that goes before a box aligned by `justify` (css-align-3/#justify-block). `free` is the
// room that the margin box leaves in the inline axis of its containing block. The start values and the baseline values,
// which have no baseline to share in this axis, put nothing before the box. The end values put all of `free` before it,
// and center puts half. Nothing goes before the box if it overflows and its alignment is safe. `left` and `right` are
// the line-left and line-right sides of the mode of `space`, and `self-start` and `self-end` are the sides of the box's
// own `mode`.
PrLayoutUnit justifyOffset(PrLayoutAlignment justify, LayoutMode mode, const LayoutSpace& space, PrLayoutUnit free) {
    if (free < kLayoutUnitZero && (justify & PrLayoutAlignmentFlag_Safe) != 0)
        return kLayoutUnitZero;
    bool ltr = space.mode.direction == PrTextDirection_Ltr;
    bool sameDirection = mode.direction == space.mode.direction;
    switch (PrLayoutAlignment(justify & kPrLayoutAlignmentValueMask)) {
        case PrLayoutAlignment_Center: return free / 2;
        case PrLayoutAlignment_End:
        case PrLayoutAlignment_FlexEnd:
        case PrLayoutAlignment_LastBaseline: return free;
        case PrLayoutAlignment_SelfStart: return sameDirection ? kLayoutUnitZero : free;
        case PrLayoutAlignment_SelfEnd: return sameDirection ? free : kLayoutUnitZero;
        case PrLayoutAlignment_Left: return ltr ? kLayoutUnitZero : free;
        case PrLayoutAlignment_Right: return ltr ? free : kLayoutUnitZero;
        default: return kLayoutUnitZero;
    }
}

// Returns true if the in-flow block-level box `node` takes fit-content for an auto inline size, and sets `*justify` to
// its justify-self. This is the case if it is aligned by a value other than normal or stretch
// (css-align-3/#justify-block), or if it is semi-replaced and aligned by normal (html/#button-layout). Kept out of line
// from inlineSizeOf, because most boxes set no alignment.
PR_NOINLINE bool fitsByJustifying(const LayoutState& state, LayoutNodeIndex node, PrLayoutAlignment* justify) {
    *justify = justifySelfOf(state, node);
    uint32_t value = *justify & kPrLayoutAlignmentValueMask;
    if (value == PrLayoutAlignment_Normal)
        return layoutTreeNode(state.tree, node).kind == PrLayoutNodeKind_SemiReplaced;
    return value != PrLayoutAlignment_Stretch;
}

// Returns the used inline size of the block container `node` in `space` (CSS2/#blockwidth). `logical` and `e` are its
// style and edges in the writing mode whose inline axis is its own, and `measure` is the mode its intrinsic sizes are
// measured in. If auto margins share the room that a given size leaves, also sets the inline-start margin in `e`.
//
// An auto size fills what the margins leave of the available space, as stretch does. A box that shrinks to fit, which
// is an inline-level box, a float, an outside marker or an orthogonal flow root, takes fit-content for an auto size and
// zero for auto margins (CSS2/#inlineblock-width, CSS2/#float-width, css-writing-modes-4/#orthogonal-auto). An in-flow
// block-level box whose justify-self is not normal or stretch takes fit-content for an auto size. Its auto margins
// share the room it leaves, and its justify-self places it in whatever room remains after that, leaving the
// over-constrained margin as it is (css-align-3/#justify-block). A semi-replaced element also takes fit-content for an
// auto size at normal, and is placed as a block of that size (html/#button-layout). A box out of flow takes the size
// its space gives, and its placement shares its auto margins (css-position-3/#abspos-auto-size).
//
// The auto inline size of a table is fit-content, and the table's auto margins share the space that it leaves. A table
// is never narrower than its min-content size, which its columns and captions need (css-tables-3/#layout-principles).
//
// Sets `*fills` if the size is what the available space leaves, so that a different available size changes it
// (LayoutResultFlag_FillsAvailable).
PrLayoutUnit inlineSizeOf(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const PrLayoutBoxStyle& box, const LayoutLogicalBox& logical, LayoutEdges& e,
        LayoutMode measure, bool* readsPercentBlock, bool* fills, LayoutNodeIndex runLast = kPrLayoutNodeNone) {
    // The root is laid out by block rules whatever its display (css-display-4/#root), which the cascade of a document
    // blockifies. It still floats as any box does.
    bool root = layoutTreeNode(state.tree, node).parent == kPrLayoutNodeNone;
    bool outOfFlow = layoutSpaceIsOutOfFlow(space);
    bool shrinks;
    if (outOfFlow)
        shrinks = space.autoInline == LayoutAutoSize_FitContent;
    else
        shrinks = (!root && prLayoutDisplayIsInlineLevel(box.display)) || box.floating != PrLayoutFloat_None ||
            box.display == PrLayoutDisplay_Marker || layoutIsOrthogonal(measure, space.mode);
    // Only an in-flow block-level box is justified, and only if an alignment is set or the box is semi-replaced.
    PrLayoutAlignment justify = PrLayoutAlignment_Normal;
    bool fits = false;
    if (!outOfFlow && !shrinks && !root) {
        const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
        if (n.justifies || n.kind == PrLayoutNodeKind_SemiReplaced ||
                (n.parent != kPrLayoutNodeNone && layoutTreeNode(state.tree, n.parent).justifiesItems))
            fits = fitsByJustifying(state, node, &justify);
    }
    bool aligned = fits && (justify & kPrLayoutAlignmentValueMask) != PrLayoutAlignment_Normal;
    bool table = layoutIsTable(box);
    PrLayoutUnit edges = layoutEdgesInline(e);
    PrLayoutUnit stretch =
        layoutStretchFit(space.available, e.margin[LayoutSide_InlineStart], e.margin[LayoutSide_InlineEnd]);
    // A size or limit of min-content, max-content or fit-content takes the intrinsic sizes of the box, which are
    // measured only in that case. The sizes use the edges that the box resolves here, not the edges that intrinsic
    // sizing resolves against zero.
    LayoutIntrinsicSizes intrinsic{};
    bool measured = false;
    auto keyword = [&](PrLayoutLength length) {
        if (!measured) {
            LayoutIntrinsicInput input{measure, space.percentBlock, layoutUnit(-1), space.fallback};
            // An anonymous table has no edges, and nothing caches its sizes.
            if (runLast != kPrLayoutNodeNone)
                intrinsic = layoutTableIntrinsic(state, node, runLast, input);
            else
                intrinsic = layoutWithEdges(layoutIntrinsicSizes(state, node, input),
                    layoutEdgesInline(layoutEdgesFromBox(state.styles, logical, layoutUnit(-1))), edges);
            if (intrinsic.fillsAvailable && layoutIsDefinite(stretch))
                intrinsic.maxContent = layoutUnitMax(intrinsic.maxContent, stretch);
        }
        measured = true;
        return layoutKeywordSize(length, intrinsic, stretch);
    };
    // An auto size with a preferred aspect ratio is the size that a definite block size gives through the ratio
    // (css-sizing-4/#aspect-ratio-automatic). The block size is clamped by its limits first, because the limits
    // transfer through the ratio.
    bool readsBlock = false;
    auto transferred = [&]() -> PrLayoutUnit {
        LayoutAspect aspect = layoutNodeAspect(state.tree, node, box, measure);
        if (aspect.ratio <= 0)
            return layoutUnit(-1);
        PrLayoutUnit stretchBase = outOfFlow ? space.stretchBlock : space.percentBlock;
        LayoutBlockBase base =
            layoutBlockBase(state.styles, box, logical, e, space.percentBlock, stretchBase, space.autoBlock);
        readsBlock = base.readsPercentBlock;
        if (!layoutIsDefinite(base.size))
            return layoutUnit(-1);
        return layoutAspectInline(aspect, base.size, edges, layoutEdgesBlock(e));
    };
    // Stretch fills what the margins leave, whether or not the box shrinks to fit (css-sizing-3/#stretch-fit-size).
    bool autoSize = !layoutLengthIsFixed(logical.inlineSize) && !layoutLengthIsContentBased(logical.inlineSize);
    bool ratioSized = false;
    PrLayoutUnit size = kLayoutUnitZero;
    if (layoutLengthIsStretch(logical.inlineSize)) {
        size = layoutUnitMax(kLayoutUnitZero, stretch);
    } else if (autoSize && layoutIsDefinite(size = transferred())) {
        // The automatic minimum in the ratio-dependent axis is the min-content size, unless the box scrolls
        // (css-sizing-4/#aspect-ratio-minimum). This also applies out of flow.
        ratioSized = true;
        if (layoutLengthIsAuto(logical.minInlineSize) && !layoutIsScrollContainer(box))
            size = layoutUnitMax(size, keyword(prLayoutLengthMinContent()));
    } else if (autoSize && (shrinks || fits || table)) {
        size = keyword(prLayoutLengthFitContent());
    } else if (autoSize) {
        size = layoutUnitMax(kLayoutUnitZero, stretch);
    } else if (layoutLengthIsContentBased(logical.inlineSize)) {
        size = keyword(logical.inlineSize);
    } else {
        size = layoutBorderSize(box, layoutResolve(state.styles, logical.inlineSize, space.percentInline), edges);
    }
    // A size equal to the stretch-fit size follows the available space, unless zero floors it.
    *fills = !ratioSized && size == stretch && stretch > kLayoutUnitZero;
    PrLayoutUnit filled = size;
    size = layoutClampSize(state.styles, box, size, logical.minInlineSize, logical.maxInlineSize,
        space.percentInline, edges, stretch, keyword);
    // A limit clamps it to a size of its own, which follows the available space only if the limit is a keyword.
    if (size != filled)
        *fills = layoutLengthIsStretch(logical.minInlineSize) || layoutLengthIsStretch(logical.maxInlineSize) ||
            layoutLengthIsContentBased(logical.minInlineSize) || layoutLengthIsContentBased(logical.maxInlineSize);
    if (table) {
        PrLayoutUnit least = keyword(prLayoutLengthMinContent());
        *fills = *fills && least <= size;
        size = layoutUnitMax(size, least);
    }
    // Auto margins share the room left by a size that does not fill the space.
    if (!outOfFlow && !shrinks && (!autoSize || ratioSized || fits || table || size != filled))
        layoutShareAutoMargins(e, space.available, size);
    if (aligned && !e.marginAuto[LayoutSide_InlineStart] && !e.marginAuto[LayoutSide_InlineEnd]) {
        PrLayoutUnit free = space.available - size - e.margin[LayoutSide_InlineStart] - e.margin[LayoutSide_InlineEnd];
        e.margin[LayoutSide_InlineStart] += justifyOffset(justify, measure, space, free);
    }
    *readsPercentBlock = readsBlock || (measured && intrinsic.readsPercentBlock);
    return size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Baselines

// Returns true if clamping `baseline` to [lo, hi] changes a baseline of its set (css-align-3/#baseline-export), either
// its alphabetic baseline or one at an offset from it in its font. Also returns true if it already carries a range,
// which the clamp composes with. The baselines of most scroll containers lie inside their border boxes, and keep no
// range.
bool clampChanges(LayoutState& state, const LayoutBaseline& baseline, PrLayoutUnit lo, PrLayoutUnit hi) {
    auto outside = [&](PrLayoutUnit at) { return at < lo || at > hi; };
    if (baseline.clamped || outside(baseline.offset))
        return true;
    if (baseline.source == kLayoutBaselineMarginEdge || baseline.source == kLayoutBaselineSynthesized)
        return false;
    const LayoutFontFaces& faces = layoutFontFacesOfGroup(state.styles, baseline.source);
    const PrLayoutUnit named[] = {faces.first.central, faces.first.ideographic, faces.first.hanging, faces.first.math};
    for (PrLayoutUnit offset : named) {
        if (outside(baseline.offset + offset))
            return true;
    }
    return false;
}

// Turns `edges`, the baselines of the content of the box indexed by LayoutBaselineEdge and measured from the
// block-start edge of its border box, into the baselines the box exports to its parent (css-align-3/#baseline-export).
// There are none if its writing mode is orthogonal to that of the space. They are clamped to its border box if it is a
// scroll container. They are measured in the block direction of the space, which is the reverse of the content's if one
// stacks blocks right to left and the other left to right. Sets their flags in `result`.
void exportBaselines(LayoutState& state, const LayoutSpace& space, const PrLayoutBoxStyle& box, LayoutMode mode,
        PrLayoutUnit blockSize, LayoutBaseline* edges, LayoutResult& result) {
    bool orthogonal = layoutIsOrthogonal(mode, space.mode);
    for (uint32_t edge = 0; edge < LayoutBaselineEdge_Count; ++edge) {
        LayoutBaseline& baseline = edges[edge];
        if (!baseline.present)
            continue;
        if (orthogonal) {
            baseline.present = false;
            continue;
        }
        if (!layoutSameBlockFlow(mode, space.mode)) {
            baseline.offset = blockSize - baseline.offset;
            baseline.unclamped = blockSize - baseline.unclamped;
            PrLayoutUnit lo = baseline.lo;
            baseline.lo = blockSize - baseline.hi;
            baseline.hi = blockSize - lo;
        }
        if (layoutIsScrollContainer(box) && clampChanges(state, baseline, kLayoutUnitZero, blockSize))
            baseline = layoutBaselineClamped(baseline, kLayoutUnitZero, blockSize);
    }
    const LayoutBaseline& first = edges[LayoutBaselineEdge_First];
    const LayoutBaseline& last = edges[LayoutBaselineEdge_Last];
    if (layoutIsScrollContainer(box) && !orthogonal)
        result.flags |= LayoutResultFlag_Scrolls;
    if (first.present)
        result.flags |= LayoutResultFlag_FirstBaseline;
    if (last.present)
        result.flags |= LayoutResultFlag_LastBaseline;
}

// ---------------------------------------------------------------------------------------------------------------------
// The shell

// Replaces the margins and borders of a table cell in `e`, indexed by LayoutSide in its containing block's mode, and in
// `inner`, indexed by LayoutSide in the cell's own `mode`. A cell has no margins
// (css-tables-3/#global-style-overrides). If its table collapses its borders, the cell has the borders that the table
// resolves (LayoutSpace::cellBorders).
PR_NOINLINE void layoutCellEdges(LayoutEdges& e, LayoutEdges& inner, const LayoutSpace& space, LayoutMode mode) {
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        e.margin[side] = kLayoutUnitZero;
        e.marginAuto[side] = false;
        inner.margin[side] = kLayoutUnitZero;
        inner.marginAuto[side] = false;
        if (space.cell != nullptr && space.cell->collapsed) {
            e.border[side] = space.cell->borders[layoutPhysicalSide(space.mode, side)];
            inner.border[side] = space.cell->borders[layoutPhysicalSide(mode, side)];
        }
    }
}

// The shell of a block-level box whose content is being laid out (layout_content.h): what the box's style, its space
// and its container's requests decide before its content is laid out.
struct Shell {
    LayoutNodeIndex node;                          // the box, or the first node of the run that an anonymous box wraps
    LayoutNodeIndex parent;                        // the box whose children make up the content
    LayoutNodeIndex runLast;                       // the last node of the run that an anonymous box wraps, or
                                                   // kPrLayoutNodeNone
    const PrLayoutBoxStyle* PR_NULLABLE anonymous; // the style of an anonymous box, or null
    const PrLayoutBoxStyle* box;
    const LayoutSpace* space;
    LayoutChildren children;        // the algorithm that lays out the content
    LayoutOuterMode content;        // the mode of the content, which is the box's own mode
    bool orthogonal;                // true if the box's writing mode is orthogonal to its space's

    // Its style and edges in its containing block's mode, which its margins are in, and in its own mode. Its sizes are
    // found in the mode whose inline axis is its own (sizing, edges).
    LayoutLogicalBox logical;
    LayoutLogicalBox own;
    LayoutEdges e;
    LayoutEdges inner;

    bool requested;                 // true if its container asks for its sizes
    LayoutRequest blockRequest;
    bool clamps;                    // true if its limits clamp a block size that its content decides
    bool scrolls;
    bool intrinsicReads;            // true if its inline size read the block-axis percentage base
    bool fills;                     // true if its inline size follows the available space (FillsAvailable)
    PrLayoutUnit inlineSize;        // of its border box
    LayoutBlockAxis axis;
    LayoutSpace contentSpace;       // its content box, the containing block of its children
    LayoutLogicalOffset origin;     // the start corner of its content box in its border box

    const LayoutLogicalBox& sizing() const { return orthogonal ? own : logical; }
    LayoutEdges& edges() { return orthogonal ? inner : e; }
};

// Finds the shell `s` of the box that layoutContainer lays out: its inline size, the block size that its style or its
// container decides, and its content box.
PR_FORCE_INLINE void shellOf(LayoutState& state, Shell& s, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, LayoutNodeIndex runLast,
        const PrLayoutBoxStyle* PR_NULLABLE anonymous) {
    // The cache keeps an anonymous flex item under the first node of its run, and keeps no anonymous table cell. The
    // content of either is made of children of its container.
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    if (runLast == kPrLayoutNodeNone && (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)) {
        runLast = layoutRunLast(state.tree, node);
        anonymous = &layoutAnonymousBoxStyle();
    }
    PR_ASSERT((runLast != kPrLayoutNodeNone) == (anonymous != nullptr));
    s.node = node;
    s.parent = anonymous != nullptr ? n.parent : node;
    s.runLast = runLast;
    s.anonymous = anonymous;
    s.box = anonymous != nullptr ? anonymous : &layoutBoxStyle(state.styles, n.style);
    s.space = &space;
    s.children = anonymous != nullptr ? layoutChildrenFromBox(*anonymous) : LayoutChildren(n.children);
    const PrLayoutBoxStyle& box = *s.box;
    PR_ASSERT(anonymous == nullptr || layoutIsTable(box) ||
        (requests != nullptr && requests->inlineAxis.kind == LayoutRequestKind_Fixed));

    // Its content and its sizes are in its own writing mode. Its margins are in the writing mode of its containing
    // block, whose axes differ if the box is an orthogonal flow root.
    s.content = layoutNodeOuterMode(state.tree, s.parent);
    LayoutMode mode = s.content.mode;
    s.orthogonal = layoutIsOrthogonal(mode, space.mode);
    s.logical = layoutLogicalBox(box, space.mode);
    s.e = layoutEdgesFromBox(state.styles, s.logical, space.containing.inlineSize);
    s.own = mode == space.mode ? s.logical : layoutLogicalBox(box, mode);
    s.inner = mode == space.mode ? s.e : layoutEdgesFromBox(state.styles, s.own, space.containing.inlineSize);
    // A table cell has no margins (css-tables-3/#global-style-overrides). If its table collapses its borders, the cell
    // has the borders that the table resolves.
    if (box.display == PrLayoutDisplay_TableCell)
        layoutCellEdges(s.e, s.inner, space, mode);
    // A box that resumes in a fragmentainer has no block-start margin there, and no block-start border or padding
    // unless it clones its edges (css-break-4/#break-decoration).
    if (layoutSpaceResume(space) != nullptr) {
        bool clones = s.anonymous == nullptr &&
            layoutFragmentationStyle(state.styles, n.style).boxDecorationBreak == PrLayoutBoxDecorationBreak_Clone;
        LayoutEdges* both[] = {&s.e, &s.inner};
        for (LayoutEdges* edges : both) {
            edges->margin[LayoutSide_BlockStart] = kLayoutUnitZero;
            if (!clones) {
                edges->border[LayoutSide_BlockStart] = kLayoutUnitZero;
                edges->padding[LayoutSide_BlockStart] = kLayoutUnitZero;
            }
        }
    }
    const LayoutLogicalBox& sizing = s.sizing();
    LayoutEdges& es = s.edges();

    // The inline size, and the block size if it does not depend on the content. The container of a flex item or a table
    // cell asks for its sizes. Every other box decides its own sizes.
    LayoutMode sizingMode = s.orthogonal ? mode : space.mode;
    s.requested = requests != nullptr;
    s.intrinsicReads = false;
    s.fills = false;
    LayoutRequest inlineRequest = layoutInlineRequest(requests);
    if (s.requested && inlineRequest.kind == LayoutRequestKind_Fixed)
        s.inlineSize = layoutUnitMax(inlineRequest.size, layoutEdgesInline(es));
    else
        s.inlineSize = inlineSizeOf(state, node, space, box, sizing, es, sizingMode, &s.intrinsicReads, &s.fills, runLast);
    PrLayoutUnit contentInline = layoutUnitMax(kLayoutUnitZero, s.inlineSize - layoutEdgesInline(es));
    // A box out of flow stretches into its inset-modified containing block rather than its containing block.
    PrLayoutUnit stretchBase = layoutSpaceIsOutOfFlow(space) ? space.stretchBlock : space.percentBlock;
    s.blockRequest = layoutBlockRequest(requests);
    s.clamps = s.blockRequest.kind == LayoutRequestKind_Own;
    LayoutBlockAxis& axis = s.axis;
    axis = LayoutBlockAxis{};
    axis.edges = layoutEdgesBlock(es);
    if (s.clamps)
        axis.size =
            layoutBlockBase(state.styles, box, sizing, es, space.percentBlock, stretchBase, space.autoBlock).size;
    else if (s.blockRequest.kind == LayoutRequestKind_Fixed)
        axis.size = layoutUnitMax(s.blockRequest.size, axis.edges);
    else
        axis.size = layoutUnit(-1);
    axis.stretch = layoutStretchFit(stretchBase, es.margin[LayoutSide_BlockStart], es.margin[LayoutSide_BlockEnd]);
    // An automatic block size with a preferred aspect ratio is the size that the inline size gives through the ratio,
    // clamped by its limits (css-sizing-4/#aspect-ratio-automatic). It is definite, and does not count as auto for
    // margin collapsing (css-sizing-4/#aspect-ratio-margin-collapse). Its automatic minimum is the size of its content,
    // unless the box scrolls, whether in flow or out of flow (css-sizing-4/#aspect-ratio-minimum). A size that its
    // container asks of its content has no ratio.
    s.scrolls = layoutIsScrollContainer(box);
    if (!layoutIsDefinite(axis.size) && s.clamps && !layoutLengthIsContentBased(sizing.blockSize)) {
        LayoutAspect aspect = layoutNodeAspect(state.tree, node, box, sizingMode);
        if (aspect.ratio > 0) {
            PrLayoutUnit size = layoutAspectBlock(aspect, s.inlineSize, layoutEdgesInline(es), axis.edges);
            axis.size = layoutClampSize(state.styles, box, size, sizing.minBlockSize, sizing.maxBlockSize,
                space.percentBlock, axis.edges, axis.stretch, layoutNoKeyword);
            axis.growsToContent = layoutLengthIsAuto(sizing.minBlockSize) && !s.scrolls;
        }
    }

    // The content box, in the box's own writing mode, on whose sides its padding and borders are taken. A scroll
    // container provides the fallback for the orthogonal flow roots in it.
    PrLayoutUnit contentBlock;
    if (layoutIsDefinite(axis.size) && (s.blockRequest.kind != LayoutRequestKind_Fixed || s.blockRequest.definite))
        contentBlock = layoutUnitMax(kLayoutUnitZero, axis.size - axis.edges);
    else
        contentBlock = layoutUnit(-1);
    s.contentSpace = LayoutSpace{
        .mode = mode,
        .available = contentInline,
        .percentInline = contentInline,
        .percentBlock = contentBlock,
        .containing = LayoutLogicalSize{contentInline, contentBlock},
        .blockLimit = layoutBlockLimit(state.styles, box, s.own, space.percentBlock, axis.edges),
        .fallback = space.fallback,
    };
    if (s.scrolls) {
        s.contentSpace.fallback = layoutScrollFallback(state.styles, box, s.own, mode,
            LayoutLogicalSize{space.percentInline, space.percentBlock},
            LayoutLogicalSize{layoutEdgesInline(s.inner), axis.edges}, state.viewport);
    }
    s.origin = LayoutLogicalOffset{
        layoutEdgesContent(s.inner, LayoutSide_InlineStart),
        layoutEdgesContent(s.inner, LayoutSide_BlockStart),
    };
}

// Lays out the content of the block container `s` in flow (layoutFlowContent), which may stop where the margins before
// it resolve. In flow in a context, the box records its reads of the context in `reads`. Sets `content` to what the
// shell finishes the box from.
PR_FORCE_INLINE LayoutFlowOutcome flowContent(LayoutState& state, Shell& s, LayoutReads* PR_NULLABLE reads,
        LayoutShellContent& content) {
    // Content in a block size that its style or its container fixes is aligned in it.
    bool fixedBlock;
    if (s.requested)
        fixedBlock = s.blockRequest.kind == LayoutRequestKind_Fixed;
    else
        fixedBlock = layoutIsDefinite(s.axis.size) && !s.axis.growsToContent;
    LayoutFlowInput input{
        .node = s.parent,
        .runFirst = s.anonymous != nullptr ? s.node : kPrLayoutNodeNone,
        .runLast = s.runLast,
        .box = s.box,
        .space = s.space,
        .content = &s.contentSpace,
        .children = s.content,
        .origin = s.origin,
        .inlineSize = s.inlineSize,
        .sizing = &s.sizing(),
        .inner = &s.inner,
        .outer = &s.e,
        .axis = s.axis,
        .clamps = s.clamps,
        .fixedBlock = fixedBlock,
        .marked = s.anonymous == nullptr,
        .reads = reads,
    };
    // A multi-column container lays its content out as column sets and spanners (css-multicol-1).
    bool columns = s.anonymous == nullptr && layoutHasColumns(state.styles, layoutTreeNode(state.tree, s.node).style);
    LayoutFlowOutcome flow = columns ? layoutMulticolContent(state, input) : layoutFlowContent(state, input);
    content.blockSize = flow.blockSize;
    content.contentBlock = flow.contentBlock;
    content.shift = flow.shift;
    content.edges[LayoutBaselineEdge_First] = flow.first;
    content.edges[LayoutBaselineEdge_Last] = flow.last;
    return flow;
}

// Returns where the box `s` resumes in the next fragmentainer, a reference that the caller takes, or null if the box
// ends in the fragmentainer of its space. The box's space has a fragmentainer, and its content in flow gave `flow` and
// `content`. A box whose content breaks fills the fragmentainer to its end. So does a box that reaches past the end
// with a block size that its content does not decide, such as a fixed block size or a minimum that its content does not
// reach, and the rest of that box goes to the next fragmentainer. Sets the block size in `content` to what the box's
// fragment takes.
PR_NOINLINE LayoutBreakToken* PR_NULLABLE fragmentedEnd(const Shell& s, LayoutFlowOutcome& flow, LayoutShellContent& content) {
    const LayoutSpace& space = *s.space;
    PR_ASSERT(space.fragmentation != nullptr);
    const LayoutFragmentainer& fragmentainer = *space.fragmentation->fragmentainer;
    const LayoutBreakToken* PR_NULLABLE resume = space.fragmentation->resume;
    // The fragmentainer's end from the block-start edge of the border box.
    PrLayoutUnit end = fragmentainer.end;
    if (space.bfc != nullptr)
        end = flow.top != kLayoutOffsetNone ? end - flow.top : layoutUnit(INT32_MAX);
    PrLayoutUnit consumed = resume != nullptr ? resume->consumed : kLayoutUnitZero;
    LayoutBreakToken* token = flow.token;
    bool fixed = layoutIsDefinite(s.axis.size);
    // The size that the content reaches here, through the box's block-end edges.
    PrLayoutUnit reached =
        s.origin.blockOffset + content.contentBlock + layoutEdgesContent(s.inner, LayoutSide_BlockEnd);
    if (fixed) {
        content.blockSize = layoutUnitMax(kLayoutUnitZero, s.axis.size - consumed);
    } else if (consumed > kLayoutUnitZero) {
        // A minimum that the content does not reach applies to the box's fragments together, so a box that resumes
        // keeps what its earlier fragments left of it (css-break-4/#varying-size-boxes).
        content.blockSize = layoutUnitMax(reached, content.blockSize - consumed);
    }
    // A box whose size its content does not reach continues past the end of the fragmentainer, and so does a box whose
    // content ends in it but whose block-end edges do not: the break goes after its content
    // (css-break-4/#possible-breaks, class C).
    bool beyond = fixed || content.blockSize > reached || s.origin.blockOffset + content.contentBlock <= end;
    if (token == nullptr && beyond && content.blockSize > end && end > kLayoutUnitZero)
        token = layoutBreakTokenCreate(s.parent, kLayoutUnitZero, kPrLayoutNodeNone, nullptr);
    if (token == nullptr)
        return nullptr;
    content.blockSize = layoutUnitMax(kLayoutUnitZero, end);
    token->consumed = consumed + content.blockSize;
    return token;
}

// Marks the fragment of the box `s` in a fragmentation context: a fragment after the first is a continuation, and a box
// that slices its edges breaks at the sides where its fragment starts after a break or ends before one.
void markBroken(const LayoutState& state, const Shell& s, LayoutFragment* fragment, bool breaks) {
    const LayoutBreakToken* PR_NULLABLE resume = layoutSpaceResume(*s.space);
    LayoutMode mode = s.content.mode;
    bool clones = layoutFragmentationStyle(state.styles, layoutTreeNode(state.tree, s.node).style).boxDecorationBreak ==
        PrLayoutBoxDecorationBreak_Clone;
    if (resume != nullptr)
        fragment->flags |= LayoutFragmentFlag_Continuation;
    if (clones || fragment->kind != LayoutFragmentKind_Box)
        return;
    LayoutSideFlags broken = 0;
    if (resume != nullptr)
        broken |= LayoutSideFlags(1u << layoutPhysicalSide(mode, LayoutSide_BlockStart));
    if (breaks)
        broken |= LayoutSideFlags(1u << layoutPhysicalSide(mode, LayoutSide_BlockEnd));
    fragment->broken = broken;
}

// Returns true if two spaces of a flex item or a table cell are the same. Neither is in flow in a context.
bool sameSpace(const LayoutSpace& a, const LayoutSpace& b) {
    PR_ASSERT(a.bfc == nullptr && b.bfc == nullptr);
    return a.mode == b.mode && a.available == b.available && a.percentInline == b.percentInline &&
        a.percentBlock == b.percentBlock && a.containing.inlineSize == b.containing.inlineSize &&
        a.containing.blockSize == b.containing.blockSize && a.blockLimit == b.blockLimit &&
        a.fallback == b.fallback && a.autoInline == b.autoInline && a.autoBlock == b.autoBlock &&
        a.stretchBlock == b.stretchBlock && layoutCellSpacesEqual(layoutCellSpaceFromSpace(a),
            layoutCellSpaceFromSpace(b));
}

// Returns true if the band memo `m` holds the content of the box `s` for `input`.
bool sameBandsKey(const LayoutBandsMemo& m, const Shell& s, const LayoutContentInput& input) {
    if (m.node != s.node || m.parent != s.parent || !sameSpace(m.space, s.contentSpace) ||
            m.input.origin.inlineOffset != input.origin.inlineOffset ||
            m.input.origin.blockOffset != input.origin.blockOffset || m.input.blockSize != input.blockSize ||
            m.input.minBlock != input.minBlock || m.input.maxBlock != input.maxBlock ||
            m.input.scrolls != input.scrolls || m.axisSize != s.axis.size || m.axisEdges != s.axis.edges ||
            m.percentBlock != s.space->percentBlock || m.clamps != s.clamps)
        return false;
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        if (m.input.padding[side] != input.padding[side])
            return false;
    }
    return true;
}

// Finds the bands of the children that the content of the box `s` left in the scratch children from `mark`, and moves
// the children into a new memo of that content, which the running layout keeps. The items whose block ranges overlap
// form a band, such as a flex line or a grid row.
LayoutBandsMemo* keepBands(LayoutState& state, const Shell& s, const LayoutContentInput& input, uint32_t mark,
        const LayoutShellContent& content, const LayoutGridLines& lines) {
    LayoutScratch& scratch = *state.scratch;
    RawArray<LayoutLogicalChild>& children = scratch.children;
    LayoutMode mode = s.content.mode;
    LayoutBandsMemo* m = allocZeroed<LayoutBandsMemo>();
    m->node = s.node;
    m->parent = s.parent;
    m->space = s.contentSpace;
    m->input = input;
    m->input.content = nullptr;
    m->axisSize = s.axis.size;
    m->axisEdges = s.axis.edges;
    m->percentBlock = s.space->percentBlock;
    m->clamps = s.clamps;
    m->content = content;
    if (lines.node != kPrLayoutNodeNone)
        m->lines = layoutGridLinesCopyCreate(state, lines);
    uint32_t count = children.count - mark;
    m->children.append(children.data + mark, count);
    children.count = mark;
    // The children in block order.
    RawArray<uint32_t> order;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = order.count;
        order.push(i);
        while (at > 0 && m->children.data[order.data[at - 1]].offset.blockOffset >
                m->children.data[i].offset.blockOffset) {
            order.data[at] = order.data[at - 1];
            order.data[--at] = i;
        }
    }
    m->band.resize(count);
    for (uint32_t k = 0; k < order.count; ++k) {
        const LayoutLogicalChild& child = m->children.data[order.data[k]];
        PrLayoutUnit top = child.offset.blockOffset;
        PrLayoutUnit bottom = top + layoutLogicalSize(mode, child.fragment->extent).blockSize;
        if (m->ends.count != 0 && top < m->ends.last()) {
            m->ends.last() = layoutUnitMax(m->ends.last(), bottom);
        } else {
            m->starts.push(top);
            m->ends.push(bottom);
        }
        m->band.data[order.data[k]] = m->starts.count - 1;
    }
    order.reset();
    // The members of each band, in the children's order.
    uint32_t bands = m->starts.count;
    m->bandFirst.resize(bands + 1);
    for (uint32_t b = 0; b <= bands; ++b)
        m->bandFirst.data[b] = 0;
    for (uint32_t i = 0; i < count; ++i)
        m->bandFirst.data[m->band.data[i] + 1] += 1;
    for (uint32_t b = 0; b < bands; ++b)
        m->bandFirst.data[b + 1] += m->bandFirst.data[b];
    m->members.resize(count);
    RawArray<uint32_t> fill;
    fill.append(m->bandFirst.data, bands);
    for (uint32_t i = 0; i < count; ++i)
        m->members.data[fill.data[m->band.data[i]]++] = i;
    fill.reset();
    scratch.bandIndex.insert(KeyIndex::hashOf(s.node), scratch.bands.count);
    scratch.bands.push(m);
    return m;
}

// Frees the band memo `m` and the references that it holds.
void destroyBands(LayoutBandsMemo* m) {
    for (uint32_t i = 0; i < m->children.count; ++i)
        layoutFragmentRelease(m->children.data[i].fragment);
    m->children.reset();
    m->band.reset();
    m->bandFirst.reset();
    m->members.reset();
    m->starts.reset();
    m->ends.reset();
    layoutGridLinesCopyDestroy(m->lines);
    prMemoryFree(m);
}

// Appends to the scratch children the items of the flex or grid container `s` that go in the fragmentainer of its
// space, from the content that `m` holds. Returns where the container resumes in the next fragmentainer, a reference or
// null. The container breaks between its bands. The fragmentainer holds the bands from the one that the container
// resumes at that end in it, and at least one band. In a fragmentainer after the first, the bands move up to the start
// of the content box. Sets the block size in `content` to the fragment's. The work is in the bands that the
// fragmentainer holds, so a container across many fragmentainers lays out in each only its own items.
PR_NOINLINE LayoutBreakToken* PR_NULLABLE breakBetweenBands(LayoutState& state, const Shell& s, LayoutShellContent& content,
        const LayoutBandsMemo& m) {
    const LayoutSpace& space = *s.space;
    LayoutMode mode = s.content.mode;
    uint32_t bands = m.starts.count;
    const LayoutFragmentainer& fragmentainer = *space.fragmentation->fragmentainer;
    const LayoutBreakToken* PR_NULLABLE resume = space.fragmentation->resume;
    uint32_t first = resume != nullptr ? resume->segment : 0;
    PrLayoutUnit shift = kLayoutUnitZero;
    if (first != 0 && first < bands)
        shift = m.starts.data[first] - s.origin.blockOffset;
    PrLayoutUnit end = fragmentainer.end;
    // The items of the bands that break inside, laid out again, in place of their fragments in the memo, even if they
    // end in the fragmentainer. An item that ended in an earlier fragmentainer has a null fragment.
    RawArray<LayoutLogicalChild> pieces;
    RawArray<uint32_t> pieceOf;         // by piece: its child
    RawArray<uint32_t> pieced;          // the bands laid out again

    // Breaks inside the band `b`, whose items are laid out again as fragmentainers that end where this one does, each
    // from its token in `resumed` if the band resumes. Returns the tokens of the items, chained by their siblings, or
    // null if they all end here, and sets `*height` to the band's block size from its top. Returns false if an item is
    // not a box, which the band does not break in, or if the band starts here and no item placed any of its content.
    auto breakInside = [&](uint32_t b, const LayoutBreakToken* PR_NULLABLE resumed, LayoutBreakToken** chain,
            PrLayoutUnit* height) {
        for (uint32_t k = m.bandFirst.data[b]; k < m.bandFirst.data[b + 1]; ++k) {
            const LayoutFragment* f = m.children.data[m.members.data[k]].fragment;
            if (f->kind != LayoutFragmentKind_Box || f->node == kPrLayoutNodeNone)
                return false;
        }
        PrLayoutUnit top = m.starts.data[b] - shift;
        bool breaks = false;
        bool placed = false;
        uint32_t pieceMark = pieces.count;
        *height = kLayoutUnitZero;
        *chain = nullptr;
        for (uint32_t k = m.bandFirst.data[b]; k < m.bandFirst.data[b + 1]; ++k) {
            uint32_t i = m.members.data[k];
            const LayoutLogicalChild& child = m.children.data[i];
            LayoutNodeIndex node = child.fragment->node;
            const LayoutBreakToken* from = nullptr;
            for (const LayoutBreakToken* t = resumed; t != nullptr && from == nullptr; t = t->sibling) {
                if (t->node == node)
                    from = t;
            }
            if (resumed != nullptr && (from == nullptr || from->segment == UINT32_MAX)) {
                // The item ended before: it keeps an ended token for the fragmentainers after this one.
                pieces.push(LayoutLogicalChild{nullptr, child.offset});
                pieceOf.push(i);
                LayoutBreakToken* ended = layoutBreakTokenCreate(node, kLayoutUnitZero, kPrLayoutNodeNone, nullptr);
                ended->segment = UINT32_MAX;
                ended->sibling = *chain;
                *chain = ended;
                continue;
            }
            PrLayoutUnit at = resumed != nullptr ? top : child.offset.blockOffset - shift;
            LayoutFragmentainer itemFragmentainer = layoutFragmentainerMoved(fragmentainer, at,
                fragmentainer.atTop && b == first);
            LayoutSpace itemSpace = s.contentSpace;
            LayoutFragmentation itemFragmentation{&itemFragmentainer, from};
            itemSpace.fragmentation = &itemFragmentation;
            LayoutRequests requests{
                .inlineAxis = LayoutRequest{
                    .size = layoutLogicalSize(mode, child.fragment->extent).inlineSize,
                    .kind = LayoutRequestKind_Fixed,
                    .definite = true,
                },
                .blockAxis = LayoutRequest{.kind = LayoutRequestKind_Content},
            };
            LayoutResult result = layoutBlockFragmented(state, node, itemSpace, &requests);
            LayoutLogicalOffset offset = child.offset;
            offset.blockOffset = at + shift;
            pieces.push(LayoutLogicalChild{result.fragment, offset});
            pieceOf.push(i);
            *height = layoutUnitMax(*height, at - top + layoutLogicalSize(mode, result.fragment->extent).blockSize);
            LayoutBreakToken* token = const_cast<LayoutBreakToken*>(result.token);
            placed = placed || token == nullptr || !token->atStart;
            if (token == nullptr) {
                token = layoutBreakTokenCreate(node, kLayoutUnitZero, kPrLayoutNodeNone, nullptr);
                token->segment = UINT32_MAX;
            } else {
                breaks = true;
            }
            token->sibling = *chain;
            *chain = token;
        }
        if (!breaks || (resumed == nullptr && !placed)) {
            layoutBreakTokenRelease(*chain);
            *chain = nullptr;
        }
        if (resumed == nullptr && !placed) {
            for (uint32_t p = pieceMark; p < pieces.count; ++p)
                layoutFragmentRelease(pieces.data[p].fragment);
            pieces.count = pieceMark;
            pieceOf.count = pieceMark;
            return false;
        }
        pieced.push(b);
        return true;
    };
    // A band that resumes inside lays its items out from their tokens, and the bands after it follow its new height.
    LayoutBreakToken* items = nullptr;
    uint32_t split = UINT32_MAX;
    PrLayoutUnit after = shift;     // the shift of the bands after the first
    PrLayoutUnit firstEnd = first < bands ? m.ends.data[first] : kLayoutUnitZero;
    if (resume != nullptr && resume->childToken != nullptr && first < bands) {
        PrLayoutUnit height = kLayoutUnitZero;
        if (breakInside(first, resume->childToken, &items, &height)) {
            split = first;
            after = shift + (firstEnd - m.starts.data[first]) - height;
            firstEnd = m.starts.data[first] + height;
        }
    }
    auto endOf = [&](uint32_t b) { return b == first ? firstEnd : m.ends.data[b]; };
    uint32_t next = items != nullptr ? first + 1 : first;
    while (next < bands && items == nullptr) {
        PrLayoutUnit bandShift = next == first ? shift : after;
        bool fits = endOf(next) - bandShift <= end;
        if (!fits && split != next) {
            // A band that does not fit breaks inside if it is the first here and nothing comes before the container in
            // the fragmentainer, or if no fragmentainer could hold it. Otherwise it goes to the next fragmentainer, and
            // so does the container if the band is its first.
            bool top = fragmentainer.atTop || resume != nullptr;
            bool alone = (next == first && top) ||
                endOf(next) - m.starts.data[next] > fragmentainer.blockSize - s.origin.blockOffset;
            PrLayoutUnit height = kLayoutUnitZero;
            if (alone && breakInside(next, nullptr, &items, &height) && items != nullptr) {
                split = next;
                ++next;
                break;
            }
        }
        if (!fits && next != first)
            break;
        ++next;
    }
    // The items of the bands from the first to the next, in the children's order, which is the order that they paint
    // in. The items of the other bands go to other fragmentainers.
    RawArray<uint32_t> kept;
    for (uint32_t b = first; b < next && b < bands; ++b) {
        for (uint32_t k = m.bandFirst.data[b]; k < m.bandFirst.data[b + 1]; ++k) {
            uint32_t i = m.members.data[k];
            uint32_t at = kept.count;
            kept.push(i);
            for (; at > 0 && kept.data[at - 1] > i; --at)
                kept.data[at] = kept.data[at - 1];
            kept.data[at] = i;
        }
    }
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    for (uint32_t k = 0; k < kept.count; ++k) {
        uint32_t i = kept.data[k];
        uint32_t b = m.band.data[i];
        LayoutLogicalChild child = m.children.data[i];
        if (pieced.find(b) != kArrayMissing) {
            uint32_t p = pieceOf.find(i);
            if (p != kArrayMissing) {
                child = pieces.data[p];
                pieces.data[p].fragment = nullptr;
            } else {
                layoutFragmentRetain(child.fragment);
            }
        } else {
            layoutFragmentRetain(child.fragment);
        }
        if (child.fragment == nullptr)
            continue;
        child.offset.blockOffset -= b == first ? shift : after;
        children.push(child);
    }
    for (uint32_t p = 0; p < pieces.count; ++p)
        layoutFragmentRelease(pieces.data[p].fragment);
    pieces.reset();
    pieceOf.reset();
    pieced.reset();
    kept.reset();
    LayoutBreakToken* token = nullptr;
    if (items != nullptr) {
        token = layoutBreakTokenCreate(s.parent, kLayoutUnitZero, kPrLayoutNodeNone, items);
        token->segment = split;
        content.blockSize = layoutUnitMax(kLayoutUnitZero, end);
    } else if (next < bands) {
        state.fragmentShortage = layoutUnitMin(state.fragmentShortage, endOf(next) - after - end);
        token = layoutBreakTokenCreate(s.parent, kLayoutUnitZero, kPrLayoutNodeNone, nullptr);
        token->segment = next;
        content.blockSize = layoutUnitMax(kLayoutUnitZero, end);
    } else {
        content.blockSize = layoutUnitMax(kLayoutUnitZero, content.blockSize - after);
    }
    return token;
}

// Lays out or solves the content of the box `s`, which establishes an independent formatting context: a flex or grid
// container's items, for `goal`. Sets `content` to what the shell finishes the box from, and `lines` to the lines of a
// grid container that builds. The block size of the content box is the box's if its style or its container decides it,
// and otherwise its content decides it within the box's limits. A size that its container decides or asks of its
// content has no limits.
//
// If `bands` is not null, the content is built in a fragmentation context, and its children go to a memo of the running
// layout instead of the scratch children (keepBands). The memo is set to `*bands`. A later layout of the same content,
// as in the next fragmentainer, takes the memo, and lays out nothing.
PR_NOINLINE void independentContent(LayoutState& state, Shell& s, LayoutContentGoal goal, LayoutShellContent& content,
        LayoutGridLines* lines, const LayoutBandsMemo** PR_NULLABLE bands = nullptr) {
    const PrLayoutBoxStyle& box = *s.box;
    const LayoutLogicalBox& sizing = s.sizing();
    const LayoutBlockAxis& axis = s.axis;
    PrLayoutUnit percentBlock = s.space->percentBlock;
    auto limit = [&](PrLayoutLength length, PrLayoutUnit otherwise) {
        if (!s.clamps || !layoutResolves(length, percentBlock))
            return otherwise;
        PrLayoutUnit size = layoutBorderSize(box, layoutResolve(state.styles, length, percentBlock), axis.edges);
        return layoutUnitMax(kLayoutUnitZero, size - axis.edges);
    };
    LayoutContentInput input{
        .content = &s.contentSpace,
        .origin = s.origin,
        .blockSize =
            layoutIsDefinite(axis.size) ? layoutUnitMax(kLayoutUnitZero, axis.size - axis.edges) : layoutUnit(-1),
        .minBlock = limit(sizing.minBlockSize, kLayoutUnitZero),
        .maxBlock = limit(sizing.maxBlockSize, layoutUnit(-1)),
        .scrolls = s.scrolls,
        .goal = goal,
    };
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
        input.padding[side] = s.inner.padding[side];
    // A block size from its aspect ratio that its content may exceed is the minimum for the size its content decides.
    if (axis.growsToContent) {
        input.minBlock = layoutUnitMax(input.minBlock, input.blockSize);
        input.blockSize = layoutUnit(-1);
    }
    LayoutScratch& scratch = *state.scratch;
    if (bands != nullptr) {
        uint32_t at = scratch.bandIndex.find(KeyIndex::hashOf(s.node),
            [&](uint32_t i) { return sameBandsKey(*scratch.bands.data[i], s, input); });
        if (at != kHashMissing) {
            const LayoutBandsMemo& m = *scratch.bands.data[at];
            content = m.content;
            if (m.lines != nullptr)
                *lines = layoutGridLinesRestore(state, *m.lines);
            *bands = &m;
            return;
        }
    }
    uint32_t mark = scratch.children.count;
    LayoutContentOutcome outcome;
    if (s.children == LayoutChildren_Flex)
        outcome = layoutFlexContent(state, s.parent, input);
    else
        outcome = layoutGridContent(state, s.parent, input, lines);
    PrLayoutUnit size = outcome.contentBlock + axis.edges;
    content.blockSize = layoutContentBlockSize(state.styles, box, sizing, axis, percentBlock, s.clamps, size, size);
    content.contentBlock = outcome.contentBlock;
    content.edges[LayoutBaselineEdge_First] = layoutBaselineMoved(outcome.first, s.origin.blockOffset);
    content.edges[LayoutBaselineEdge_Last] = layoutBaselineMoved(outcome.last, s.origin.blockOffset);
    content.itemsStart = outcome.itemsStart;
    content.itemsEnd = outcome.itemsEnd;
    if (bands != nullptr)
        *bands = keepBands(state, s, input, mark, content, *lines);
}

// Lays out the parts of the table `s` as its content (layoutTableContent). Sets `content` to what the shell finishes
// the box from. The table's captions and grid are placed relative to its border box, not inside its edges, and its
// border box holds them. Returns what layoutTableFinish needs to make the table's fragment.
PR_NOINLINE LayoutTableOutcome tableContent(LayoutState& state, const Shell& s, LayoutShellContent& content) {
    LayoutTableInput input{
        .space = s.space,
        .inlineSize = s.inlineSize,
        .blockKind = s.blockRequest.kind,
        .blockSize = s.axis.size,
    };
    LayoutTableOutcome outcome = layoutTableContent(state, s.node, s.runLast, input);
    content.blockSize = outcome.blockSize;
    content.edges[LayoutBaselineEdge_First] = outcome.first;
    content.edges[LayoutBaselineEdge_Last] = outcome.last;
    return outcome;
}

// Lays out the block-level box `node` in `space`: a block container, a flex or grid container, or a table. If
// `anonymous` is given, it lays out instead the anonymous box of that style that wraps the siblings from `node` through
// `runLast`. If `node` is text or a line break, the box is the anonymous block of a flex item that wraps the run of
// text that starts at `node`, whatever `runLast` and `anonymous` say. If the box is a flex item or a table cell,
// `requests` is what its container asks of its sizes. If the box is in flow in a context, its reads of the context are
// recorded in `reads`.
//
// The box's shell finds its sizes, the algorithm that its children name lays out its content, and the shell then
// finishes the box from what the content gave: its fragment, the boxes out of flow that it holds, its overflow, and the
// baselines that it exports.
//
// If `measure` is given, a flex or grid container is solved alone and `measure` is set to its answer to a measurement
// of `kind`. The result then has no fragment, and the items are measured but not laid out. Kept out of line from
// layoutBlock: most work in most layouts is reuse of a cached result, and this keeps that path cheap.
PR_NOINLINE LayoutResult layoutContainer(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, LayoutReads* PR_NULLABLE reads, LayoutMeasurement* PR_NULLABLE measure,
        LayoutMeasureKind kind = LayoutMeasureKind_Baselines, LayoutNodeIndex runLast = kPrLayoutNodeNone,
        const PrLayoutBoxStyle* PR_NULLABLE anonymous = nullptr) {
    bool readsBefore = state.readsFallback;
    state.readsFallback = false;
    Shell s;
    shellOf(state, s, node, space, requests, runLast, anonymous);
    const PrLayoutBoxStyle& box = *s.box;
    LayoutMode mode = s.content.mode;
    PR_ASSERT(space.bfc == nullptr ||
        !layoutEstablishesContext(state.tree, node, box, LayoutOuterMode{space.mode, kLayoutFontGroupNone}));

    // The content, laid out by the algorithm that the box's children name, and the fragment, which each algorithm
    // finishes with what its content left. The fragment is built before the box's reads of the fallback are settled.
    // Building it lays out the boxes out of flow that the box holds, in the fallback that its content is given, so
    // their reads are the box's own. A measurement builds no fragment. The run of an anonymous item holds no box out of
    // flow, because a box out of flow ends a run.
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    uint32_t mark = children.count;
    LayoutShellContent content{};
    LayoutFragment* fragment = nullptr;
    auto build = [&](const LayoutGridLines* PR_NULLABLE lines) -> LayoutFragment* {
        if (measure != nullptr)
            return nullptr;
        LayoutLogicalSize size{s.inlineSize, content.blockSize};
        if (s.anonymous != nullptr)
            return layoutFragmentBuild(children, mark, LayoutFragmentKind_Box, kPrLayoutNodeNone, mode, size);
        return layoutHolderBuild(state, node, mark, mode, size, s.contentSpace.fallback, lines);
    };
    LayoutResult result{};
    result.start = layoutStrut(s.e.margin[LayoutSide_BlockStart]);
    result.end = layoutStrut(s.e.margin[LayoutSide_BlockEnd]);
    switch (s.children) {
        case LayoutChildren_Flow: {
            LayoutFlowOutcome flow = flowContent(state, s, reads, content);
            if (flow.stopAt != kLayoutOffsetNone) {
                state.readsFallback = readsBefore || state.readsFallback;
                LayoutResult stopped{};
                stopped.resolution = flow.stopAt;
                return stopped;
            }
            result.start = result.start + flow.start;
            result.end = flow.end + result.end;
            if (flow.collapsedThrough)
                result.flags |= LayoutResultFlag_CollapsedThrough;
            result.top = flow.top;
            result.cleared = flow.cleared;
            result.unplaced = flow.unplaced;
            PR_ASSERT(space.fragmentation != nullptr || flow.token == nullptr);
            if (space.fragmentation != nullptr)
                result.token = fragmentedEnd(s, flow, content);
            fragment = build(nullptr);
            break;
        }
        case LayoutChildren_Flex:
        case LayoutChildren_Grid: {
            LayoutContentGoal goal = LayoutContentGoal_Size;
            if (measure == nullptr)
                goal = LayoutContentGoal_Build;
            else if (kind == LayoutMeasureKind_Baselines)
                goal = LayoutContentGoal_Baselines;
            LayoutGridLines lines{kPrLayoutNodeNone, 0};
            if (measure == nullptr && space.fragmentation != nullptr) {
                const LayoutBandsMemo* bands = nullptr;
                independentContent(state, s, goal, content, &lines, &bands);
                result.token = breakBetweenBands(state, s, content, *bands);
            } else {
                independentContent(state, s, goal, content, &lines);
            }
            if (lines.node == kPrLayoutNodeNone) {
                fragment = build(nullptr);
                break;
            }
            fragment = layoutGridFinish(state, lines, build(&lines));
            break;
        }
        default: {
            LayoutTableOutcome table = tableContent(state, s, content);
            result.token = table.token;
            if (table.token != nullptr && space.fragmentation != nullptr)
                content.blockSize = layoutUnitMax(content.blockSize, layoutSpaceFragmentainer(space)->end);
            fragment = layoutTableFinish(state, build(nullptr), table);
            break;
        }
    }
    // The fallback that the content of a scroll container reads comes from the container's own sizes, read here, and
    // from the size of the initial containing block, which no space carries.
    bool readsFallback = state.readsFallback;
    bool scrollerReads = s.scrolls && readsFallback;
    if (scrollerReads) {
        if (state.reuse)
            layoutTreeReadViewport(state.tree, node);
        readsFallback = false;
    }
    state.readsFallback = readsBefore || readsFallback;

    if (readsFallback)
        result.flags |= LayoutResultFlag_ReadsFallback;
    // The fallback of a scroll container, if its content read it, and the sizes of an orthogonal flow root also read
    // the sizes of the containing block.
    if (s.intrinsicReads || scrollerReads || s.orthogonal ||
            (s.clamps && layoutBlockReadsBase(s.sizing(), space.autoBlock)))
        result.flags |= LayoutResultFlag_ReadsBlockBase;
    if (s.fills)
        result.flags |= LayoutResultFlag_FillsAvailable;
    result.marginInlineStart = s.e.margin[LayoutSide_InlineStart];
    if (measure != nullptr) {
        exportBaselines(state, space, box, mode, content.blockSize, content.edges, result);
        *measure = LayoutMeasurement{
            content.blockSize, content.edges[LayoutBaselineEdge_First],
            content.edges[LayoutBaselineEdge_Last],
        };
        return result;
    }
    ++state.stats->boxFragmentsMade;
    if (s.anonymous == nullptr && layoutIsFloat(state.tree, node))
        fragment->flags |= LayoutFragmentFlag_Float;
    if (layoutSpaceIsOutOfFlow(space))
        fragment->flags |= LayoutFragmentFlag_OutOfFlow;
    // Its in-flow content runs from the block-start edge of its content box through the margins it holds. Its overflow
    // includes those margins, as the overflow of a scroll container does, so that a margin past the end of a box can be
    // scrolled to in any scroll container that holds it (css-overflow-3/#scrollable-overflow lets a UA include such
    // margins). The in-flow content of a flex container holds the margin boxes of its items, which css-overflow-3
    // requires.
    PrLayoutUnit contentInline = s.contentSpace.containing.inlineSize;
    LayoutLogicalOffset inFlowStart{
        layoutUnitMin(kLayoutUnitZero, content.itemsStart.inlineOffset),
        layoutUnitMin(kLayoutUnitZero, content.itemsStart.blockOffset) + content.shift,
    };
    LayoutLogicalSize inFlow{
        layoutUnitMax(contentInline, content.itemsEnd.inlineOffset) - inFlowStart.inlineOffset,
        layoutUnitMax(content.contentBlock, content.itemsEnd.blockOffset) - inFlowStart.blockOffset,
    };
    PrLayoutVec at = layoutPhysicalOffset(mode, s.origin + inFlowStart, inFlow,
        LayoutLogicalSize{s.inlineSize, content.blockSize});
    PrLayoutRect flowRect = layoutRectAt(at, layoutPhysicalSize(mode, inFlow));
    if (s.scrolls) {
        PrLayoutUnit padding[PrRectSide_Count];
        for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
            padding[layoutPhysicalSide(mode, side)] = s.inner.padding[side];
        layoutScrollerSetOverflow(state.tree, fragment, box, padding, mode, flowRect);
    } else {
        layoutFragmentSetOverflow(state.tree, fragment);
        fragment->overflow = layoutRectUnion(fragment->overflow, flowRect);
    }
    fragment->box.containingInline = space.containing.inlineSize;
    if (space.fragmentation != nullptr && s.anonymous == nullptr && measure == nullptr)
        markBroken(state, s, fragment, result.token != nullptr);
    result.fragment = fragment;
    exportBaselines(state, space, box, mode, content.blockSize, content.edges, result);
    layoutFragmentSetBaselines(fragment, content.edges[LayoutBaselineEdge_First],
        content.edges[LayoutBaselineEdge_Last]);
    return result;
}

// Takes the result that the cache holds for the box `node` from a layout when a different size was available, if the
// box resolves to the same inline size in `space` at `requests`. The result takes the inline-start margin that the box
// resolves to there, in the flow and writing mode of its containing block, as its layout resolves them
// (layoutContainer). Returns false if the inline size differs.
bool findResized(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, LayoutResult* out) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutLogicalBox logical = layoutLogicalBox(box, space.mode);
    LayoutEdges e = layoutEdgesFromBox(state.styles, logical, space.containing.inlineSize);
    bool readsPercentBlock = false;
    bool fills = false;
    PrLayoutUnit size = inlineSizeOf(state, node, space, box, logical, e, space.mode, &readsPercentBlock, &fills);
    LayoutResolvedInline resolved{size, e.margin[LayoutSide_InlineStart]};
    return layoutCacheFindResized(state, node, space, requests, resolved, out);
}

// Lays out the block-level box `node` in `space` at `requests`, as layoutBlock does if its cached result is not valid,
// and counts the layout. Keeps the result in its layout entry if `keeps` is true, which it is not for a measurement. A
// result laid out when a different size was available is taken instead if it is still valid. Kept out of line from
// layoutBlock, because most work in most layouts is reuse of a cached result.
PR_NOINLINE LayoutResult layOut(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests, bool keeps = true) {
    LayoutResult result{};
    // A result laid out when a different size was available is valid if the box resolves to the same inline size.
    if (layoutCacheMayResize(state, node, space, requests) && findResized(state, node, space, requests, &result))
        return result;
    if (layoutTreeNode(state.tree, node).kind == PrLayoutNodeKind_Replaced) {
        PR_ASSERT(space.bfc == nullptr);
        result = layoutReplacedBlock(state, node, space, requests);
        ++state.stats->blocksLaidOut;
        ++state.stats->boxFragmentsMade;
        if (keeps)
            layoutCacheStore(state, node, space, requests, result, nullptr);
        return result;
    }
    if (space.bfc == nullptr) {
        result = layoutContainer(state, node, space, requests, nullptr, nullptr);
        ++state.stats->blocksLaidOut;
        if (keeps)
            layoutCacheStore(state, node, space, requests, result, nullptr);
        return result;
    }
    // A flex item is in flow in no context.
    PR_ASSERT(keeps && requests == nullptr);
    // In flow in a context, what the box reads of the context is added to the reads of its parent, and becomes the
    // dependencies of its cache entry.
    LayoutReads reads = layoutReadsBegin(*space.bfc->containing.exclusions);
    result = layoutContainer(state, node, space, nullptr, &reads, nullptr);
    if (layoutResultStopped(result))
        return result;
    layoutReadsMerge(*space.bfc->containing.reads, reads);
    ++state.stats->blocksLaidOut;
    // A result that holds floats not yet placed is laid out again once its parent knows where they go.
    if (result.unplaced == 0)
        layoutCacheStore(state, node, space, nullptr, result, &reads);
    return result;
}

// Returns the space that an orthogonal flow root is laid out in, in a containing block whose content is `content`
// (css-writing-modes-4/#orthogonal-auto). The block axis of the root is the inline axis of the containing block, and
// its block-axis percentages resolve against that inline size. The inline axis of the root is the block axis of the
// containing block, within which an auto inline size fits its content. Its inline-axis percentages also resolve against
// that size, which csswg-drafts#555 leaves open, so that a percentage and an auto size fill the same space. If the
// block size of the containing block is indefinite, the space is the smaller of its limit and the fallback, and the
// function records that it read the fallback.
LayoutSpace orthogonalSpace(LayoutState& state, const LayoutSpace& content) {
    LayoutSpace space = content;
    space.percentBlock = content.containing.inlineSize;
    space.available = content.containing.blockSize;
    if (!layoutIsDefinite(space.available)) {
        space.available = layoutLogicalSize(content.mode, content.fallback).blockSize;
        if (layoutIsDefinite(content.blockLimit))
            space.available = layoutUnitMin(space.available, content.blockLimit);
        state.readsFallback = true;
    }
    space.percentInline = space.available;
    space.bfc = nullptr;
    space.marker = LayoutLineExtent{};
    return space;
}

// Returns the space that the flex item `node` is laid out in, in a flex container whose content is `content`. An
// orthogonal flow root gets a space of its own, as resultOf lays one out.
LayoutSpace ownSpace(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& content) {
    return layoutIsOrthogonalRoot(state.tree, node, content.mode) ? orthogonalSpace(state, content) : content;
}

// Returns where the border box of an orthogonal flow root, laid out as `result`, starts in the inline axis of its
// containing block, whose content is `content`. It starts past its margin there. Auto margins share what its block size
// leaves of the available space, as the margins of a block-level box do. Its own layout holds neither that size nor
// that space.
PrLayoutUnit orthogonalMarginStart(const LayoutState& state, LayoutNodeIndex node, const LayoutSpace& content,
        const LayoutResult& result) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, content.mode),
        content.containing.inlineSize);
    if (!prLayoutDisplayIsInlineLevel(box.display) && box.floating == PrLayoutFloat_None)
        layoutShareAutoMargins(e, content.available,
            layoutLogicalSize(content.mode, result.fragment->extent).inlineSize);
    return e.margin[LayoutSide_InlineStart];
}

// Returns the result of the block-level box `node` in `space` at `requests`, as layoutBlock gives it.
PR_FORCE_INLINE LayoutResult resultOf(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests) {
    LayoutResult result{};
    // The space of a box out of flow is already made in its own axes (layoutAbsolute).
    if (layoutSpaceIsOutOfFlow(space) || !layoutIsOrthogonalRoot(state.tree, node, space.mode)) {
        if (!layoutCacheFind(state, node, space, requests, &result))
            result = layOut(state, node, space, requests);
        return result;
    }
    LayoutSpace own = orthogonalSpace(state, space);
    if (!layoutCacheFind(state, node, own, requests, &result))
        result = layOut(state, node, own, requests);
    if (result.fragment != nullptr)
        result.marginInlineStart = orthogonalMarginStart(state, node, space, result);
    return result;
}

// Measures `node` as layoutMeasure does, without the memo that a layout that keeps no records uses.
LayoutMeasurement measure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& container,
        const LayoutRequests& requests, LayoutMeasureKind kind) {
    LayoutSpace space = ownSpace(state, node, container);
    LayoutMeasurement out{};
    LayoutBaselines kept{};
    if (layoutCacheFindMeasure(state, node, space, requests, kind, &out.blockSize, &kept)) {
        out.first = layoutBaselinesAt(kept, LayoutBaselineEdge_First);
        out.last = layoutBaselinesAt(kept, LayoutBaselineEdge_Last);
        return out;
    }
    ++state.stats->measuresComputed;
    // A layout entry at this request serves as the measurement. Otherwise the box is laid out, and the layout is handed
    // to its final layout instead of being kept in its entry.
    LayoutResult result{};
    bool handed = !layoutCacheFind(state, node, space, &requests, &result);
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    if (handed && layoutKindIsElement(n.kind) && layoutChildrenAreItems(n.children)) {
        // A flex or grid container is solved alone. Its items are measured, and none is laid out.
        result = layoutContainer(state, node, space, &requests, nullptr, &out, kind);
        layoutCacheStoreMeasure(state, node, space, requests, kind, result.flags, out.blockSize, out.first, out.last);
        return out;
    }
    if (handed)
        result = layOut(state, node, space, &requests, false);
    out.blockSize = layoutLogicalSize(layoutOwnMode(state.tree, node), result.fragment->extent).blockSize;
    out.first = layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_First);
    out.last = layoutFragmentBaseline(result.fragment, LayoutBaselineEdge_Last);
    layoutCacheStoreMeasure(state, node, space, requests, LayoutMeasureKind_Baselines, result.flags, out.blockSize,
        out.first, out.last);
    if (handed && state.reuse) {
        LayoutScratch& scratch = *state.scratch;
        scratch.handoffIndex.insert(KeyIndex::hashOf(node), scratch.handoffs.count);
        scratch.handoffs.push(LayoutHandoff{node, container, requests, result});
    }
    else
        layoutFragmentRelease(result.fragment);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// The initial containing block and pages

// Lays out the root in a viewport of `extent`, and returns a fragment of the initial containing block that holds it.
LayoutFragment* viewportOf(LayoutState& state, uint32_t root, PrLayoutVec extent) {
    if (root == kPrLayoutNodeNone) {
        LayoutFragment* viewport = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, 0);
        viewport->extent = extent;
        viewport->overflow = layoutRectAt(kLayoutVecZero, extent);
        return viewport;
    }
    // The root is a block box whatever its display (css-display-4/#root). It is laid out in the initial containing
    // block, whose writing mode is the root's (css-writing-modes-4/#principal-flow).
    LayoutMode mode = layoutOwnMode(state.tree, root);
    LayoutLogicalSize initial = layoutLogicalSize(mode, extent);
    LayoutSpace space{
        .mode = mode,
        .available = initial.inlineSize,
        .percentInline = initial.inlineSize,
        .percentBlock = initial.blockSize,
        .containing = initial,
        .blockLimit = layoutUnit(-1),
        .fallback = extent,
    };
    LayoutResult result = layoutBlock(state, root, space);
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, root).style);
    LayoutLogicalSize size = layoutLogicalSize(mode, result.fragment->extent);
    LayoutLogicalOffset offset{result.marginInlineStart, layoutStrutSize(result.start)};
    // A root that floats shrinks to fit, and goes to its own side of the initial containing block
    // (CSS2/#float-position).
    if (box.floating != PrLayoutFloat_None && box.display != PrLayoutDisplay_None) {
        PrLayoutFloat floating = box.floating;
        bool ltr = mode.direction == PrTextDirection_Ltr;
        bool end = floating == PrLayoutFloat_InlineEnd || (floating == PrLayoutFloat_Right && ltr) ||
            (floating == PrLayoutFloat_Left && !ltr);
        if (end) {
            LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, mode), initial.inlineSize);
            offset.inlineOffset = initial.inlineSize - size.inlineSize - e.margin[LayoutSide_InlineEnd];
        }
    }
    PrLayoutVec at = layoutPhysicalOffset(mode, offset + layoutRelativeOffset(state.styles, box, space), size, initial);
    // Holds the boxes out of flow that no box in the root holds, after the root: absolutely positioned boxes in the
    // initial containing block, and fixed boxes in the viewport (css-position-3/#def-cb).
    RawArray<LayoutChild> held;
    layoutViewportHold(state, result.fragment, at, extent, mode, held);
    LayoutFragment* viewport = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, 1 + held.count);
    viewport->extent = extent;
    viewport->overflow = layoutRectAt(kLayoutVecZero, extent);
    viewport->children[0] = LayoutChild{result.fragment, at};
    for (uint32_t i = 0; i < held.count; ++i)
        viewport->children[1 + i] = held.data[i];
    layoutFragmentSummarize(viewport);
    // The viewport scrolls through the margin box of the root and what the root reaches without clipping. Scrolling
    // starts from the corner of the initial containing block that is the origin of the principal writing mode.
    LayoutLogicalSize margins{
        offset.inlineOffset + size.inlineSize,
        layoutStrutSize(result.start) + size.blockSize + layoutStrutSize(result.end),
    };
    PrLayoutRect reach = layoutRectAt(layoutPhysicalOffset(mode, kLayoutLogicalOffsetZero, margins, initial),
        layoutPhysicalSize(mode, margins));
    reach = layoutRectUnion(reach, layoutRectMoved(result.fragment->overflow, at));
    // The parts of a fixed box that lie outside the viewport cannot be scrolled to.
    for (uint32_t i = 0; i < held.count; ++i) {
        const LayoutFragment* f = held.data[i].fragment;
        if (!layoutIsFixed(state.tree, f->node))
            reach = layoutRectUnion(reach, layoutRectMoved(layoutPassedOverflow(state.tree, f), held.data[i].offset));
    }
    held.reset();
    PrLayoutRect scrollable = layoutRectUnion(viewport->overflow, reach);
    const PrRectSide origin[] = {
        layoutPhysicalSide(mode, LayoutSide_BlockStart),
        layoutPhysicalSide(mode, LayoutSide_InlineStart),
    };
    for (PrRectSide side : origin) {
        switch (side) {
            case PrRectSide_Left: scrollable.left = kLayoutUnitZero; break;
            case PrRectSide_Top: scrollable.top = kLayoutUnitZero; break;
            case PrRectSide_Right: scrollable.right = extent.x; break;
            default: scrollable.bottom = extent.y; break;
        }
    }
    viewport->overflow = scrollable;
    return viewport;
}

// Returns the box of the page that `context` describes, from the tree's pagination, with margins that leave a page area
// of at least zero.
PrLayoutPageBox pageBoxOf(const LayoutState& state, const PrLayoutPageContext& context) {
    PrLayoutPageBox box = prLayoutPageBoxDefault();
    const PrLayoutPagination& pagination = state.tree->pagination;
    pagination.box(pagination.context, &context, &box);
    return layoutPageBoxFit(box);
}

// Returns the side of the page after one on `side`.
PrLayoutPageSide nextSide(PrLayoutPageSide side) {
    if (side == PrLayoutPageSide_Left)
        return PrLayoutPageSide_Right;
    return PrLayoutPageSide_Left;
}

// Returns true if a page on `side` is on the side that a forced break asks for (css-break-4/#page-break-values). The
// recto page is the right page of a left-to-right root.
bool sideFits(LayoutBreakSide wanted, PrLayoutPageSide side, PrTextDirection direction) {
    PrLayoutPageSide recto = PrLayoutPageSide_Left;
    if (direction == PrTextDirection_Ltr)
        recto = PrLayoutPageSide_Right;
    switch (wanted) {
        case LayoutBreakSide_Left: return side == PrLayoutPageSide_Left;
        case LayoutBreakSide_Right: return side == PrLayoutPageSide_Right;
        case LayoutBreakSide_Recto: return side == recto;
        case LayoutBreakSide_Verso: return side != recto;
        default: return true;
    }
}

// Returns a blank page fragment of `box`.
LayoutFragment* blankPageOf(const LayoutState& state, const PrLayoutPageBox& box) {
    LayoutFragment* page = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, 0);
    page->extent = box.extent;
    page->flags |= LayoutFragmentFlag_Page;
    layoutFragmentSetOverflow(state.tree, page);
    return page;
}

// Returns the node of the first content of `fragment` in its children's order: a text, a replaced box, or a box with a
// size and no content inside it. Placeholders and boxes with no size hold no content. Returns kPrLayoutNodeNone if it
// holds none.
LayoutNodeIndex firstContentOf(const LayoutFragment* fragment) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutFragment* child = fragment->children[i].fragment;
        switch (child->kind) {
            case LayoutFragmentKind_Text:
            case LayoutFragmentKind_Replaced: return child->node;
            case LayoutFragmentKind_Box:
            case LayoutFragmentKind_Line:
            case LayoutFragmentKind_Around: {
                LayoutNodeIndex found = firstContentOf(child);
                if (found != kPrLayoutNodeNone)
                    return found;
                if (child->kind == LayoutFragmentKind_Box && child->node != kPrLayoutNodeNone && child->childCount == 0 &&
                        child->extent.x > kLayoutUnitZero && child->extent.y > kLayoutUnitZero)
                    return child->node;
                break;
            }
            default: break;
        }
    }
    return kPrLayoutNodeNone;
}

// Lays the content of the root out as pages (pr/layout_page.h): one page at a time, each in its own page area, from the
// token of the page before. Appends each page's fragment to `pages`, at its offset down the stack of pages, with its
// sheet the pagination's gap below the sheet before. The fragment holds the page's part of the root's content, the
// boxes out of flow that the page holds, and the fixed boxes that repeat on it. Appends what each page is to `records`,
// and each running box whose placeholder a page holds, with the page, to `running`.
void paginate(LayoutState& state, uint32_t root, RawArray<LayoutChild>& pages, RawArray<PrLayoutPage>& records,
        RawArray<LayoutRunningPage>& running) {
    LayoutTree* tree = state.tree;
    LayoutMode mode = layoutOwnMode(tree, root);
    PrLayoutPageContext context = layoutStartPageContext(tree, root);
    PrLayoutPageBox box = pageBoxOf(state, context);
    RawArray<LayoutChild> held;
    RawArray<LayoutChild> fixed;        // the fixed boxes of the pages so far, each holding a reference
    RawArray<uint32_t> fixedFrom;       // by fixed box: the index among `pages` of the page that holds it first
    RawArray<LayoutNodeIndex> placed;   // scratch: the running boxes whose placeholders a page holds
    PrLayoutUnit y = kLayoutUnitZero;   // the top of the next page's sheet
    auto add = [&](LayoutFragment* page, const PrLayoutPageContext& at, const PrLayoutPageBox& pageBox) {
        PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&pageBox);
        PrLayoutVec offset{sheet.trim.left, y + sheet.trim.top};
        PrLayoutPage record{
            .context = at,
            .box = pageBox,
            .offset = offset,
            .first = layoutNodeName(tree, firstContentOf(page)),
        };
        pages.push(LayoutChild{page, offset});
        records.push(record);
        y += sheet.extent.y + tree->pagination.gap;
    };
    const PrLayoutBoxStyle& rootBox = layoutBoxStyle(state.styles, layoutTreeNode(tree, root).style);
    const LayoutBreakToken* resume = nullptr;
    constexpr uint32_t kMaxPages = 100000;
    for (uint32_t index = 0; index < kMaxPages; ++index) {
        PrLayoutVec area = prLayoutPageBoxArea(&box);
        LayoutLogicalSize initial = layoutLogicalSize(mode, area);
        LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(rootBox, mode), initial.inlineSize);
        PrLayoutUnit marginStart = resume != nullptr ? kLayoutUnitZero : e.margin[LayoutSide_BlockStart];
        LayoutFragmentainer page{
            .blockSize = initial.blockSize,
            .end = layoutUnitMax(initial.blockSize - marginStart, layoutUnitPx(1)),
            .kind = LayoutFragmentainerKind_Page,
            .atTop = true,
        };
        LayoutFragmentation fragmentation{&page, resume};
        LayoutSpace space{
            .mode = mode,
            .available = initial.inlineSize,
            .percentInline = initial.inlineSize,
            .percentBlock = initial.blockSize,
            .containing = initial,
            .blockLimit = layoutUnit(-1),
            .fallback = area,
            .fragmentation = &fragmentation,
        };
        LayoutResult result = layoutBlockFragmented(state, root, space);
        LayoutLogicalSize size = layoutLogicalSize(mode, result.fragment->extent);
        LayoutLogicalOffset offset{result.marginInlineStart, marginStart};
        PrLayoutVec at = layoutPhysicalOffset(mode, offset, size, initial);
        // The page holds the boxes out of flow whose placeholders are on it, and the fixed boxes of the pages before,
        // which repeat on every page after the one that they are found on (CSS2/#fixed-positioning).
        placed.clear();
        layoutViewportHold(state, result.fragment, at, area, mode, held, &placed);
        for (uint32_t i = 0; i < placed.count; ++i)
            running.push(LayoutRunningPage{placed.data[i], pages.count});
        uint32_t found = held.count;
        for (uint32_t i = 0; i < fixed.count; ++i) {
            layoutFragmentRetain(fixed.data[i].fragment);
            held.push(fixed.data[i]);
        }
        for (uint32_t i = 0; i < found; ++i) {
            if (layoutIsFixed(tree, held.data[i].fragment->node)) {
                layoutFragmentRetain(held.data[i].fragment);
                fixed.push(held.data[i]);
                fixedFrom.push(pages.count);
            }
        }
        // The page holds the content of the root's fragment, in its page area.
        const LayoutFragment* piece = result.fragment;
        PrLayoutVec margin{box.margin[PrRectSide_Left], box.margin[PrRectSide_Top]};
        uint32_t heldCount = held.count;
        LayoutFragment* fragment = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone,
            piece->childCount + heldCount);
        fragment->extent = box.extent;
        fragment->flags |= LayoutFragmentFlag_Page;
        for (uint32_t i = 0; i < piece->childCount; ++i) {
            layoutFragmentRetain(piece->children[i].fragment);
            fragment->children[i] = LayoutChild{piece->children[i].fragment, margin + at + piece->children[i].offset};
        }
        for (uint32_t i = 0; i < heldCount; ++i)
            fragment->children[piece->childCount + i] =
                LayoutChild{held.data[i].fragment, margin + held.data[i].offset};
        layoutFragmentSummarize(fragment);
        held.clear();
        layoutFragmentSetOverflow(tree, fragment);
        layoutFragmentRelease(piece);
        add(fragment, context, box);
        layoutBreakTokenRelease(resume);
        resume = result.token;
        if (resume == nullptr)
            break;
        PrLayoutName name = resume->hasPage ? resume->page : context.name;
        PrLayoutPageContext next{.index = context.index + 1, .name = name, .side = nextSide(context.side)};
        if (!sideFits(resume->side, next.side, mode.direction)) {
            // A break to a side of the spread that the next page is not on inserts a blank page.
            PrLayoutPageContext blank = next;
            blank.blank = true;
            PrLayoutPageBox blankBox = pageBoxOf(state, blank);
            add(blankPageOf(state, blankBox), blank, blankBox);
            next.index += 1;
            next.side = nextSide(next.side);
        }
        context = next;
        box = pageBoxOf(state, context);
    }
    layoutBreakTokenRelease(resume);
    held.reset();
    placed.reset();
    // A fixed box repeats on the pages before the one that holds it too (css-position-3/#fixed-pos).
    for (uint32_t p = 0; p < pages.count; ++p) {
        uint32_t extra = 0;
        for (uint32_t i = 0; i < fixed.count; ++i)
            extra += fixedFrom.data[i] > p ? 1 : 0;
        if (extra == 0)
            continue;
        const LayoutFragment* old = pages.data[p].fragment;
        LayoutFragment* page = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, old->childCount + extra);
        page->extent = old->extent;
        page->flags = old->flags;
        for (uint32_t i = 0; i < old->childCount; ++i) {
            layoutFragmentRetain(old->children[i].fragment);
            page->children[i] = old->children[i];
        }
        PrLayoutVec margin{records.data[p].box.margin[PrRectSide_Left], records.data[p].box.margin[PrRectSide_Top]};
        uint32_t at = old->childCount;
        for (uint32_t i = 0; i < fixed.count; ++i) {
            if (fixedFrom.data[i] <= p)
                continue;
            layoutFragmentRetain(fixed.data[i].fragment);
            page->children[at++] = LayoutChild{fixed.data[i].fragment, margin + fixed.data[i].offset};
        }
        layoutFragmentSummarize(page);
        layoutFragmentSetOverflow(tree, page);
        layoutFragmentRelease(old);
        pages.data[p].fragment = page;
    }
    fixedFrom.reset();
    for (uint32_t i = 0; i < fixed.count; ++i)
        layoutFragmentRelease(fixed.data[i].fragment);
    fixed.reset();
    sortItems(running.span(), [](const LayoutRunningPage& a, const LayoutRunningPage& b) { return a.node < b.node; });
}

// Returns an anonymous fragment of the extent of the page box `box` that holds the fragments of the margin boxes
// `boxes`, by PrLayoutMarginBox, laid out in its margins, or null if the page generates no margin box. A margin box is
// a child of the root whose display is PrLayoutDisplay_PageMargin, and any other node generates none.
const LayoutFragment* PR_NULLABLE marginsOf(LayoutState& state, uint32_t root, const PrLayoutPageBox& box,
        const LayoutNodeIndex* boxes) {
    LayoutTree* tree = state.tree;
    LayoutNodeIndex generated[PrLayoutMarginBox_Count]{};
    bool any = false;
    for (uint32_t i = 0; i < PrLayoutMarginBox_Count; ++i) {
        LayoutNodeIndex node = boxes[i];
        if (node == kPrLayoutNodeNone)
            continue;
        const LayoutTreeNode& n = layoutTreeNode(tree, node);
        if (n.parent != root || !layoutKindIsElement(n.kind) ||
                layoutBoxStyle(state.styles, n.style).display != PrLayoutDisplay_PageMargin)
            continue;
        generated[i] = node;
        any = true;
    }
    if (!any)
        return nullptr;
    RawArray<LayoutChild> margins;
    layoutPageMargins(state, box, generated, margins);
    LayoutFragment* page = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, margins.count);
    page->extent = box.extent;
    for (uint32_t i = 0; i < margins.count; ++i)
        page->children[i] = margins.data[i];
    margins.reset();
    layoutFragmentSummarize(page);
    layoutFragmentSetOverflow(tree, page);
    return page;
}

// Lays out the root as pages (pr/layout_page.h), and returns a fragment of a viewport of `extent` that holds them. The
// fragment's first child is the root's own box, which holds the pages' content. The pages' sheets are stacked down that
// box in order, each centered across the widest sheet, so that the root stays one box, whose layer paints every page.
// The box is centered across the viewport if it is narrower. The children after it hold the margin boxes of each page
// that has any, at the page's offset. The viewport scrolls through the root's box. A layout that keeps its results
// records the pages in the tree, with the root's box. A later layout in which nothing but margin boxes changed takes
// the root's box from there, and lays out only the margins. Neither that layout nor the placements that it publishes
// reach the content, whatever the viewport's extent.
const LayoutFragment* pagesOf(LayoutState& state, uint32_t root, PrLayoutVec extent) {
    LayoutTree* tree = state.tree;
    layoutStackingAddFragmentationRoot(tree, root);
    RawArray<PrLayoutPage> records;
    const LayoutFragment* stack = nullptr;
    bool kept = state.reuse && tree->pageStack != nullptr &&
        (layoutTreeNode(tree, root).dirty & ~LayoutDirtyFlag_Margins) == 0;
    if (kept) {
        stack = tree->pageStack;
        layoutFragmentRetain(stack);
        records.append(tree->pages.data, tree->pages.count);
    } else {
        RawArray<LayoutChild> pages;
        RawArray<LayoutRunningPage> running;
        paginate(state, root, pages, records, running);
        PrLayoutUnit width = kLayoutUnitZero;
        PrLayoutUnit height = kLayoutUnitZero;
        for (uint32_t i = 0; i < records.count; ++i) {
            PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&records.data[i].box);
            width = layoutUnitMax(width, sheet.extent.x);
            height = records.data[i].offset.y - sheet.trim.top + sheet.extent.y;
        }
        for (uint32_t i = 0; i < pages.count; ++i) {
            PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&records.data[i].box);
            pages.data[i].offset.x = (width - sheet.extent.x) / 2 + sheet.trim.left;
        }
        LayoutMode mode = layoutOwnMode(tree, root);
        LayoutFragment* made = layoutFragmentCreate(LayoutFragmentKind_Box, root, pages.count);
        for (uint32_t i = 0; i < pages.count; ++i)
            made->children[i] = pages.data[i];
        layoutFragmentSummarize(made);
        made->extent = PrLayoutVec{width, height};
        made->box.containingInline = layoutLogicalSize(mode, made->extent).inlineSize;
        layoutFragmentSetOverflow(tree, made);
        pages.reset();
        stack = made;
        if (state.reuse) {
            layoutTreeForgetPages(tree);
            layoutFragmentRetain(stack);
            tree->pageStack = stack;
            tree->runningPages.reset();
            tree->runningPages = running;
        } else {
            running.reset();
        }
    }
    // A page's offset in the viewport's content is its offset in the stack, moved by the stack's offset across the
    // viewport, which the viewport's extent decides.
    PrLayoutVec at{layoutUnitMax(kLayoutUnitZero, (extent.x - stack->extent.x) / 2), kLayoutUnitZero};
    for (uint32_t i = 0; i < records.count; ++i)
        records.data[i].offset = stack->children[i].offset + at;
    RawArray<LayoutChild> margined;
    uint32_t setFor = tree->pageMargins.count / PrLayoutMarginBox_Count;
    for (uint32_t p = 0; p < records.count && p < setFor; ++p) {
        LayoutNodeIndex boxes[PrLayoutMarginBox_Count];
        for (uint32_t i = 0; i < PrLayoutMarginBox_Count; ++i) {
            PrLayoutNode node = tree->pageMargins.data[p * PrLayoutMarginBox_Count + i];
            boxes[i] = layoutNodeIsValid(tree, node) ? layoutNodeIndex(tree, node) : kPrLayoutNodeNone;
        }
        if (const LayoutFragment* margins = marginsOf(state, root, records.data[p].box, boxes))
            margined.push(LayoutChild{margins, records.data[p].offset});
    }
    LayoutFragment* viewport = layoutFragmentCreate(LayoutFragmentKind_Box, kPrLayoutNodeNone, 1 + margined.count);
    viewport->children[0] = LayoutChild{stack, at};
    for (uint32_t i = 0; i < margined.count; ++i)
        viewport->children[1 + i] = margined.data[i];
    margined.reset();
    layoutFragmentSummarize(viewport);
    viewport->extent = extent;
    viewport->overflow = layoutRectUnion(layoutRectAt(kLayoutVecZero, extent), layoutRectAt(at, stack->extent));
    if (state.reuse) {
        tree->pages.reset();
        tree->pages = records;
        tree->nodes.data[root].dirty &= LayoutDirtyFlags(~LayoutDirtyFlag_Margins);
    } else {
        records.reset();
    }
    return viewport;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Block-level boxes

LayoutResult layoutBlock(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space) {
    return resultOf(state, node, space, nullptr);
}

LayoutResult layoutBlockFragmented(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests) {
    PR_ASSERT(space.fragmentation != nullptr);
    if (layoutIsMonolithic(state.tree, node, space.mode)) {
        LayoutSpace whole = space;
        whole.fragmentation = nullptr;
        return requests != nullptr ? layoutFinal(state, node, whole, *requests) : layoutBlock(state, node, whole);
    }
    ++state.stats->blocksLaidOut;
    // A box laid out across fragmentainers keeps no result, so its marks would outlast the layout that took its changes
    // in. Its cache entry is from before those changes, if it has any.
    if (state.reuse && layoutTreeNode(state.tree, node).dirty != 0) {
        layoutCacheForget(state.tree->cache, node);
        state.scratch->broken.push(node);
    }
    if (space.bfc == nullptr)
        return layoutContainer(state, node, space, requests, nullptr, nullptr);
    LayoutReads reads = layoutReadsBegin(*space.bfc->containing.exclusions);
    LayoutResult result = layoutContainer(state, node, space, requests, &reads, nullptr);
    if (!layoutResultStopped(result))
        layoutReadsMerge(*space.bfc->containing.reads, reads);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Measurements and final layouts

LayoutMeasurement layoutMeasure(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests& requests, LayoutMeasureKind kind) {
    PR_ASSERT(space.bfc == nullptr && requests.inlineAxis.kind == LayoutRequestKind_Fixed);
    if (state.reuse)
        return measure(state, node, space, requests, kind);
    // A layout that keeps no records still measures each box once for each request, as the records would.
    LayoutScratch& s = *state.scratch;
    uint32_t hash = KeyIndex::hashOf(node);
    uint32_t at = s.memoIndex.find(hash, [&](uint32_t i) {
        const LayoutMemo& memo = s.memos.data[i];
        return memo.node == node && memo.kind >= kind && layoutRequestsEqual(memo.requests, requests) &&
            sameSpace(memo.space, space);
    });
    if (at != kHashMissing)
        return s.memos.data[at].answer;
    LayoutMeasurement answer = measure(state, node, space, requests, kind);
    s.memoIndex.insert(hash, s.memos.count);
    s.memos.push(LayoutMemo{node, space, requests, kind, answer});
    return answer;
}

LayoutResult layoutFinal(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests& requests) {
    LayoutScratch& scratch = *state.scratch;
    uint32_t hash = KeyIndex::hashOf(node);
    uint32_t at = scratch.handoffIndex.find(hash, [&](uint32_t i) {
        const LayoutHandoff& handoff = scratch.handoffs.data[i];
        return handoff.node == node && sameSpace(handoff.space, space) && layoutRequestsEqual(handoff.requests, requests);
    });
    if (at == kHashMissing)
        return resultOf(state, node, space, &requests);
    // A handoff that is taken leaves its slot, which no lookup finds again, and holds no fragment to release.
    LayoutHandoff& handoff = scratch.handoffs.data[at];
    LayoutResult result = handoff.result;
    handoff.result.fragment = nullptr;
    scratch.handoffIndex.erase(hash, at);
    layoutCacheStore(state, node, ownSpace(state, node, space), &requests, result, nullptr);
    ++state.stats->handoffsTaken;
    return result;
}

LayoutResult layoutAnonymousBlock(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last,
        const LayoutSpace& space, const LayoutRequests& requests) {
    ++state.stats->blocksLaidOut;
    return layoutContainer(state, first, space, &requests, nullptr, nullptr, LayoutMeasureKind_Baselines, last,
        &layoutAnonymousCellStyle());
}

LayoutResult layoutAnonymousTable(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last, const LayoutSpace& space) {
    ++state.stats->blocksLaidOut;
    return layoutContainer(state, first, space, nullptr, nullptr, nullptr, LayoutMeasureKind_Baselines, last,
        &layoutAnonymousTableStyle());
}

void layoutEndLayout(LayoutState& state) {
    LayoutScratch& scratch = *state.scratch;
    for (uint32_t i = 0; i < scratch.broken.count; ++i)
        state.tree->nodes.data[scratch.broken.data[i]].dirty = 0;
    scratch.broken.clear();
    for (uint32_t i = 0; i < scratch.handoffs.count; ++i)
        layoutFragmentRelease(scratch.handoffs.data[i].result.fragment);
    scratch.handoffs.clear();
    scratch.handoffIndex.clear();
    scratch.memos.clear();
    scratch.memoIndex.clear();
    scratch.intrinsicMemos.clear();
    scratch.intrinsicMemoIndex.clear();
    for (uint32_t i = 0; i < scratch.bands.count; ++i)
        destroyBands(scratch.bands.data[i]);
    scratch.bands.clear();
    scratch.bandIndex.clear();
    layoutTableEndLayout(scratch.table);
}

// ---------------------------------------------------------------------------------------------------------------------
// The initial containing block

const LayoutFragment* layoutViewport(LayoutState& state, uint32_t root, PrLayoutVec extent) {
    LayoutTree* tree = state.tree;
    if (root == kPrLayoutNodeNone || root != tree->root || tree->pagination.box == nullptr) {
        if (root == tree->root && state.reuse)
            tree->pages.clear();
        return viewportOf(state, root, extent);
    }
    return pagesOf(state, root, extent);
}

} // namespace pr_impl
