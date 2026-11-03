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

#include "layout/src/layout_box.h"
#include "layout/src/layout_cache.h"
#include "layout/src/layout_flex.h"
#include "layout/src/layout_flow.h"
#include "layout/src/layout_grid.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_replaced.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

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

// What a box's content gave its shell, which the shell finishes the box from.
struct Content {
    PrLayoutUnit blockSize;         // of the border box
    PrLayoutUnit contentBlock;      // the end of the in-flow content in the content box, before alignment moved it
    PrLayoutUnit shift;             // how far alignment moved the content along the block axis

    // The box's baselines, indexed by LayoutBaselineEdge and measured from the block-start edge of its border box.
    LayoutBaseline edges[LayoutBaselineEdge_Count];

    // The bounds of the margin boxes of the items that a scroll container holds (LayoutContentOutcome).
    LayoutLogicalOffset itemsStart;
    LayoutLogicalOffset itemsEnd;
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
PR_FORCE_INLINE LayoutFlowOutcome flowContent(LayoutState& state, Shell& s, LayoutReads* PR_NULLABLE reads, Content& content) {
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
    LayoutFlowOutcome flow = layoutFlowContent(state, input);
    content.blockSize = flow.blockSize;
    content.contentBlock = flow.contentBlock;
    content.shift = flow.shift;
    content.edges[LayoutBaselineEdge_First] = flow.first;
    content.edges[LayoutBaselineEdge_Last] = flow.last;
    return flow;
}

// Lays out or solves the content of the box `s`, which establishes an independent formatting context: a flex or grid
// container's items, for `goal`. Sets `content` to what the shell finishes the box from, and `lines` to the lines of a
// grid container that builds. The block size of the content box is the box's if its style or its container decides it,
// and otherwise its content decides it within the box's limits. A size that its container decides or asks of its
// content has no limits.
PR_NOINLINE void independentContent(LayoutState& state, Shell& s, LayoutContentGoal goal, Content& content,
        LayoutGridLines* lines) {
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
}

// Lays out the parts of the table `s` as its content (layoutTableContent). Sets `content` to what the shell finishes
// the box from. The table's captions and grid are placed relative to its border box, not inside its edges, and its
// border box holds them. Returns what layoutTableFinish needs to make the table's fragment.
PR_NOINLINE LayoutTableOutcome tableContent(LayoutState& state, const Shell& s, Content& content) {
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
    Content content{};
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
            independentContent(state, s, goal, content, &lines);
            if (lines.node == kPrLayoutNodeNone) {
                fragment = build(nullptr);
                break;
            }
            fragment = layoutGridFinish(state, lines, build(&lines));
            break;
        }
        default: {
            LayoutTableOutcome table = tableContent(state, s, content);
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
    result.fragment = fragment;
    exportBaselines(state, space, box, mode, content.blockSize, content.edges, result);
    layoutFragmentSetBaselines(fragment, content.edges[LayoutBaselineEdge_First],
        content.edges[LayoutBaselineEdge_Last]);
    return result;
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

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Block-level boxes

LayoutResult layoutBlock(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space) {
    return resultOf(state, node, space, nullptr);
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

void layoutEndLayout(LayoutScratch& scratch) {
    for (uint32_t i = 0; i < scratch.handoffs.count; ++i)
        layoutFragmentRelease(scratch.handoffs.data[i].result.fragment);
    scratch.handoffs.clear();
    scratch.handoffIndex.clear();
    scratch.memos.clear();
    scratch.memoIndex.clear();
    scratch.intrinsicMemos.clear();
    scratch.intrinsicMemoIndex.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// The initial containing block

const LayoutFragment* layoutViewport(LayoutState& state, uint32_t root, PrLayoutVec extent) {
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
    if (held.count != 0)
        viewport->flags |= LayoutFragmentFlag_Holds;
    viewport->flags |= LayoutFragmentFlags(result.fragment->flags & LayoutFragmentFlag_Placeholders);
    for (uint32_t i = 0; i < held.count; ++i)
        viewport->children[1 + i] = held.data[i];
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

} // namespace pr_impl
