// Intrinsic sizes: the inline sizes that the content of a box asks for, and the contribution of a box to the intrinsic
// sizes of its parent.

#include "layout/src/layout_intrinsic.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_cache.h"
#include "layout/src/layout_flex.h"
#include "layout/src/layout_grid.h"
#include "layout/src/layout_inline.h"
#include "layout/src/layout_replaced.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_table.h"

namespace pr_impl {

namespace {

// Returns the intrinsic sizes of the content of the block container `node`, or of the anonymous box that wraps its
// children from `runFirst` through `runLast` if they are not kPrLayoutNodeNone, measured in `children` in the mode
// `mode` that the container gives its content. For each kind, the size is the larger of the widest contribution of an
// in-flow child and the size of the inline content. Sets `*childrenRead` if a contribution read its percentage base.
LayoutIntrinsicSizes flowIntrinsic(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runFirst,
                                   LayoutNodeIndex runLast, const LayoutIntrinsicInput& children, LayoutOuterMode mode,
                                   bool* childrenRead) {
    LayoutIntrinsicSizes sizes{};
    auto add = [&](const LayoutIntrinsicSizes& child) {
        sizes.minContent = layoutUnitMax(sizes.minContent, child.minContent);
        sizes.maxContent = layoutUnitMax(sizes.maxContent, child.maxContent);
        *childrenRead = *childrenRead || child.readsPercentBlock;
    };
    // Floats that follow one another add up, line-left and line-right together. A float that clears a side starts the
    // sum for that side again. A block in flow ends both sums, and adds them to its own contribution if it avoids
    // floats. CSS2 leaves this undefined.
    PrLayoutUnit floats[2] = {kLayoutUnitZero, kLayoutUnitZero};
    PrTextDirection direction = children.mode.direction;
    bool blockBefore = false;       // true after a block-level box in flow: no later line is the first formatted

    // Adds a child that is block-level or floats, or a block-level box in flow inside an inline box, which counts as
    // one of the children of the container (CSS2/#anonymous-block-level). The floats in an inline formatting context
    // sit beside its lines, and are not added to these sums. A box out of flow adds nothing
    // (css-position-3/#abspos-layout).
    auto blockChild = [&](uint32_t child, const PrLayoutBoxStyle& box) {
        if (layoutPositionIsOutOfFlow(box.position))
            return;
        LayoutIntrinsicSizes contribution = layoutContribution(state, child, children);
        if (box.floating != PrLayoutFloat_None) {
            LayoutLineSides clears = layoutClearSides(box, direction);
            for (uint32_t side = 0; side < 2; ++side) {
                if ((clears & (1 << side)) != 0)
                    floats[side] = kLayoutUnitZero;
            }
            uint32_t side = layoutFloatSide(box, direction) == LayoutLineSide_Left ? 0 : 1;
            floats[side] = floats[side] + contribution.maxContent;
            contribution.maxContent = floats[0] + floats[1];
            add(contribution);
            return;
        }
        if (layoutAvoidsFloats(state.tree, child, box, mode))
            contribution.maxContent = contribution.maxContent + floats[0] + floats[1];
        floats[0] = floats[1] = kLayoutUnitZero;
        blockBefore = true;
        add(contribution);
    };
    auto inlines = [&](uint32_t first, uint32_t last, bool) {
        // Adds each segment of the run, and the block-level box after each segment but the last.
        LayoutInlineRun run = layoutInlineMeasureBegin(state, first, last);
        uint32_t segments = layoutInlineSegmentCount(run);
        for (uint32_t k = 0; k < segments; ++k) {
            add(layoutInlineSegmentIntrinsic(state, run, k, children, k == 0 && !blockBefore));
            if (k + 1 == segments)
                continue;
            uint32_t block = layoutInlineBlockAfter(run, k);
            blockChild(block, layoutBoxStyle(state.styles, layoutTreeNode(state.tree, block).style));
        }
        layoutInlineEnd(state, run);
    };
    // An anonymous table has no edges, so its contribution is its intrinsic sizes. It avoids floats.
    auto tables = [&](uint32_t first, uint32_t last) {
        LayoutIntrinsicSizes contribution = layoutTableIntrinsic(state, first, last, children);
        contribution.maxContent = contribution.maxContent + floats[0] + floats[1];
        floats[0] = floats[1] = kLayoutUnitZero;
        blockBefore = true;
        add(contribution);
    };
    if (runFirst != kPrLayoutNodeNone)
        layoutEachChildIn(state.tree, runFirst, runLast, blockChild, inlines, tables);
    else
        layoutEachChild(state.tree, node, blockChild, inlines, tables);
    return sizes;
}

// Returns the content inline size of `node`, whose box has inline-size containment, measured in `mode`: its count of
// characters of its first available face, or else its contain-intrinsic size of the axis, or zero.
PrLayoutUnit containedInline(LayoutState& state, LayoutNodeIndex node, const PrLayoutBoxStyle& box, LayoutMode mode) {
    if (box.intrinsicCharacters != 0) {
        const LayoutFaceMetrics& face = layoutFontFaces(state.styles, layoutTreeNode(state.tree, node).style).first;
        return face.averageAdvance * int32_t(box.intrinsicCharacters - 1) + face.maxAdvance;
    }
    PrLayoutLength size = layoutIsHorizontal(mode) ? box.containIntrinsicWidth : box.containIntrinsicHeight;
    return layoutLengthIsFixed(size) && !layoutLengthReadsBase(size)
               ? layoutUnitMax(kLayoutUnitZero, layoutResolve(state.styles, size, kLayoutUnitZero))
               : kLayoutUnitZero;
}

// Returns the intrinsic sizes of the block container `node`. For each kind, the size is the larger of the widest
// contribution of an in-flow child and the size of the inline content, plus the inline padding and borders. A
// dependency of a child on its percentage base or its block limit becomes a dependency of the container only if the
// value that the container gave the child came from the container's own input.
//
// A container with a preferred aspect ratio and a definite block size instead has the size that the block size gives
// through the ratio. Unless the container scrolls, an automatic minimum floors that size at the min-content size of its
// content (css-sizing-4/#aspect-ratio-automatic, css-sizing-4/#aspect-ratio-minimum). The sizes of a container with a
// preferred aspect ratio read the percentage base if its block size does.
LayoutIntrinsicSizes containerIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    // Table layout finds the sizes of a table from its columns, its captions and its edges. A table has no preferred
    // aspect ratio.
    if (layoutIsTable(box))
        return layoutTableIntrinsic(state, node, kPrLayoutNodeNone, input);
    LayoutLogicalBox logical = layoutLogicalBox(box, input.mode);
    LayoutEdges e = layoutEdgesFromBox(state.styles, logical, layoutUnit(-1));
    LayoutBlockBase base = layoutBlockBase(state.styles, box, logical, e, input.percentBlock);
    LayoutAspect aspect = layoutNodeAspect(state.tree, node, box, input.mode);
    auto transferred = [&](LayoutIntrinsicSizes content) {
        if (aspect.ratio <= 0)
            return content;
        content.readsPercentBlock = content.readsPercentBlock || base.readsPercentBlock;
        if (!layoutIsDefinite(base.size))
            return content;
        PrLayoutUnit size = layoutAspectInline(aspect, base.size, layoutEdgesInline(e), layoutEdgesBlock(e));
        if (layoutLengthIsAuto(logical.minInlineSize) && !layoutIsScrollContainer(box))
            size = layoutUnitMax(size, content.minContent);
        content.minContent = size;
        content.maxContent = size;
        return content;
    };
    LayoutOuterMode mode = layoutNodeOuterMode(state.tree, node);
    LayoutLogicalBox own = mode.mode == input.mode ? logical : layoutLogicalBox(box, mode.mode);
    PrLayoutUnit edgesBlock = layoutEdgesBlock(e);
    LayoutIntrinsicInput children{
        .mode = mode.mode,
        .percentBlock =
            layoutIsDefinite(base.size) ? layoutUnitMax(kLayoutUnitZero, base.size - edgesBlock) : layoutUnit(-1),
        .blockLimit = layoutBlockLimit(state.styles, box, own, input.percentBlock, edgesBlock),
        .fallback = layoutIsScrollContainer(box)
                        ? layoutScrollFallback(state.styles, box, own, mode.mode,
                                               LayoutLogicalSize{layoutUnit(-1), input.percentBlock},
                                               LayoutLogicalSize{layoutEdgesInline(e), edgesBlock}, state.viewport)
                        : input.fallback,
    };
    LayoutIntrinsicSizes sizes{};
    // A box with inline-size containment measures no content in its inline axis.
    if (box.containInline && layoutIsHorizontal(mode.mode) == layoutIsHorizontal(input.mode)) {
        PrLayoutUnit size = containedInline(state, node, box, input.mode) + layoutEdgesInline(e);
        sizes.minContent = size;
        sizes.maxContent = size;
        return transferred(sizes);
    }
    bool childrenRead = false;
    // A read by the content of the block size or limit that the container gives its children counts as a read of the
    // container's own input only if that size or limit reads the container's base.
    bool givesInput = base.readsPercentBlock || layoutLengthReadsBase(own.minBlockSize)
                      || layoutLengthReadsBase(own.maxBlockSize);
    // The sizes of a flex or grid container come from the contributions of its items, as its layout finds them.
    if (layoutIsFlexContainer(box) || layoutIsGridContainer(box)) {
        sizes = layoutIsFlexContainer(box) ? layoutFlexIntrinsic(state, node, children)
                                           : layoutGridIntrinsic(state, node, children);
        PrLayoutUnit edges = layoutEdgesInline(e);
        sizes.maxContent = sizes.maxContent + edges;
        sizes.minContent = sizes.minContent + edges;
        sizes.readsPercentBlock = givesInput && sizes.readsPercentBlock;
        return transferred(sizes);
    }
    sizes = flowIntrinsic(state, node, kPrLayoutNodeNone, kPrLayoutNodeNone, children, mode, &childrenRead);
    PrLayoutUnit edges = layoutEdgesInline(e);
    sizes.maxContent = layoutUnitMax(sizes.maxContent, sizes.minContent) + edges;
    sizes.minContent = sizes.minContent + edges;
    sizes.readsPercentBlock = givesInput && childrenRead;
    return transferred(sizes);
}

// Returns the intrinsic sizes of the anonymous box that wraps the run of siblings from `first` through `last`, measured
// in the writing mode of their parent, with no edges.
LayoutIntrinsicSizes anonymousIntrinsic(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last,
                                        const LayoutIntrinsicInput& input) {
    LayoutOuterMode mode = layoutNodeOuterMode(state.tree, layoutTreeNode(state.tree, first).parent);
    LayoutIntrinsicInput content{.mode = mode.mode,
                                 .percentBlock = layoutUnit(-1),
                                 .blockLimit = layoutUnit(-1),
                                 .fallback = input.fallback};
    bool read = false;
    LayoutIntrinsicSizes sizes = flowIntrinsic(state, first, first, last, content, mode, &read);
    sizes.maxContent = layoutUnitMax(sizes.maxContent, sizes.minContent);
    sizes.readsPercentBlock = false;
    return sizes;
}

// Returns the contribution of the orthogonal flow root `node`. Its block size, with its margins, is both its
// min-content and its max-content contribution (css-writing-modes-4/#orthogonal-flows). The box is laid out in the
// space that its containing block, measured in `input`, makes for it.
//
// The layout goes through the cache of the box, so the layout that follows the measurement reuses it if its space is
// the same. The percentages of its edges and its block-axis percentages are cyclic, because the inline size of the
// containing block is indefinite.
LayoutIntrinsicSizes orthogonalContribution(LayoutState& state, LayoutNodeIndex node,
                                            const LayoutIntrinsicInput& input) {
    LayoutSpace content{
        .mode = input.mode,
        .available = layoutUnit(-1),
        .percentInline = layoutUnit(-1),
        .percentBlock = input.percentBlock,
        .containing = LayoutLogicalSize{layoutUnit(-1), input.percentBlock},
        .blockLimit = input.blockLimit,
        .fallback = input.fallback,
    };
    bool readsBefore = state.readsFallback;
    state.readsFallback = false;
    LayoutResult result = layoutBlock(state, node, content);
    bool readsFallback = state.readsFallback;
    state.readsFallback = readsBefore || readsFallback;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, input.mode), layoutUnit(-1));
    PrLayoutUnit size = layoutLogicalSize(input.mode, result.fragment->extent).inlineSize;
    // The layout ignored the minimum block size if it reads the cyclic base. Here the minimum instead resolves its
    // percentages against zero, as a minimum does in any contribution.
    LayoutLogicalBox own = layoutLogicalBox(box, layoutOwnMode(state.tree, node));
    if (layoutLengthIsFixed(own.minBlockSize) && layoutLengthReadsBase(own.minBlockSize)) {
        PrLayoutUnit edges = layoutEdgesBlock(layoutEdgesFromBox(state.styles, own, layoutUnit(-1)));
        PrLayoutUnit least = layoutResolve(state.styles, own.minBlockSize, kLayoutUnitZero);
        size = layoutUnitMax(size, layoutBorderSize(box, least, edges));
    }
    size = size + e.margin[LayoutSide_InlineStart] + e.margin[LayoutSide_InlineEnd];
    layoutFragmentRelease(result.fragment);
    return LayoutIntrinsicSizes{
        .minContent = size, .maxContent = size, .readsPercentBlock = true, .readsFallback = readsFallback};
}

// Measures the intrinsic sizes of `node` in `input`, as layoutIntrinsicSizes does if it finds none cached, and stores
// them if the layout keeps records.
LayoutIntrinsicSizes computeIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutIntrinsicSizes sizes{};
    bool readsBefore = state.readsFallback;
    state.readsFallback = false;
    PrLayoutNodeKind kind = layoutTreeNode(state.tree, node).kind;
    bool replaced = kind == PrLayoutNodeKind_Replaced;
    if (replaced)
        sizes = layoutReplacedIntrinsic(state, node, input);
    else if (layoutKindIsElement(kind))
        sizes = containerIntrinsic(state, node, input);
    else
        sizes = anonymousIntrinsic(state, node, layoutRunLast(state.tree, node), input);
    // The fallback that the content of a scroll container reads comes from the container's own sizes and the size of
    // the initial containing block.
    sizes.readsFallback = state.readsFallback;
    if (sizes.readsFallback && layoutKindIsElement(kind)
        && layoutIsScrollContainer(layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style))) {
        if (state.reuse)
            layoutTreeReadViewport(state.tree, node);
        sizes.readsFallback = false;
    }
    state.readsFallback = readsBefore || sizes.readsFallback;
    ++state.stats->intrinsicsComputed;
    layoutCacheStoreIntrinsic(state, node, input, sizes);
    return sizes;
}

// Returns true if two intrinsic inputs are the same.
bool sameInput(const LayoutIntrinsicInput& a, const LayoutIntrinsicInput& b) {
    return a.mode == b.mode && a.percentBlock == b.percentBlock && a.blockLimit == b.blockLimit
           && a.fallback == b.fallback;
}

} // namespace

LayoutIntrinsicSizes layoutIntrinsicSizes(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutIntrinsicSizes sizes{};
    if (layoutCacheFindIntrinsic(state, node, input, &sizes))
        return sizes;
    if (state.reuse)
        return computeIntrinsic(state, node, input);
    // A layout that keeps no records still finds the sizes of each box once for each input, as the cache would.
    LayoutScratch& s = *state.scratch;
    uint32_t hash = KeyIndex::hashOf(node);
    uint32_t at = s.intrinsicMemoIndex.find(hash, [&](uint32_t i) {
        const LayoutIntrinsicMemo& memo = s.intrinsicMemos.data[i];
        return memo.node == node && sameInput(memo.input, input);
    });
    if (at != kHashMissing) {
        sizes = s.intrinsicMemos.data[at].sizes;
        state.readsFallback = state.readsFallback || sizes.readsFallback;
        return sizes;
    }
    sizes = computeIntrinsic(state, node, input);
    s.intrinsicMemoIndex.insert(hash, s.intrinsicMemos.count);
    s.intrinsicMemos.push(LayoutIntrinsicMemo{node, input, sizes});
    return sizes;
}

LayoutIntrinsicSizes layoutAnonymousIntrinsic(LayoutState& state, LayoutNodeIndex first, LayoutNodeIndex last,
                                              const LayoutIntrinsicInput& input) {
    bool readsBefore = state.readsFallback;
    state.readsFallback = false;
    LayoutIntrinsicSizes sizes = anonymousIntrinsic(state, first, last, input);
    sizes.readsFallback = state.readsFallback;
    state.readsFallback = readsBefore || sizes.readsFallback;
    return sizes;
}

LayoutIntrinsicSizes layoutContribution(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input,
                                        LayoutIntrinsicSizes* PR_NULLABLE content) {
    if (layoutIsOrthogonalRoot(state.tree, node, input.mode)) {
        LayoutIntrinsicSizes contribution = orthogonalContribution(state, node, input);
        if (content != nullptr)
            *content = layoutIntrinsicSizes(state, node, input);
        return contribution;
    }
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutLogicalBox logical = layoutLogicalBox(box, input.mode);
    bool replaced = layoutTreeNode(state.tree, node).kind == PrLayoutNodeKind_Replaced;
    // Cyclic percentages of the edges resolve against zero (css-sizing-3/#cyclic-percentage-contribution), and auto
    // margins are zero.
    LayoutEdges e = layoutEdgesFromBox(state.styles, logical, layoutUnit(-1));
    PrLayoutUnit edges = layoutEdgesInline(e);
    PrLayoutUnit margins = e.margin[LayoutSide_InlineStart] + e.margin[LayoutSide_InlineEnd];
    LayoutIntrinsicSizes sizes{};
    bool measured = false;
    auto intrinsic = [&]() -> const LayoutIntrinsicSizes& {
        if (!measured)
            sizes = layoutIntrinsicSizes(state, node, input);
        measured = true;
        return sizes;
    };
    bool reads = false;
    LayoutIntrinsicSizes result{};
    for (uint32_t kind = 0; kind < 2; ++kind) {
        bool min = kind == 0;
        auto ofKind = [&](const LayoutIntrinsicSizes& of) { return min ? of.minContent : of.maxContent; };
        // A keyword names an intrinsic size. In a contribution, fit-content is the size of the kind being computed.
        auto keyword = [&](PrLayoutLength length) {
            return length.bits == prLayoutLengthMinContent().bits   ? intrinsic().minContent
                   : length.bits == prLayoutLengthMaxContent().bits ? intrinsic().maxContent
                                                                    : ofKind(intrinsic());
        };
        PrLayoutLength preferred = logical.inlineSize;
        PrLayoutUnit size = kLayoutUnitZero;
        if (layoutLengthIsFixed(preferred) && !layoutLengthReadsBase(preferred))
            size = layoutBorderSize(box, layoutResolve(state.styles, preferred, kLayoutUnitZero), edges);
        else if (layoutLengthIsContentBased(preferred))
            size = keyword(preferred);
        else if (layoutLengthIsFixed(preferred))
            // A cyclic percentage. At min-content, a replaced box compresses to its edges, and the whole value
            // compresses with it. css-sizing-3/#cyclic-percentage-contribution resolves only the percentage against
            // zero, and csswg-drafts#5665 leaves this open. Otherwise the size behaves as auto.
            size = replaced && min ? edges : ofKind(intrinsic());
        else if (replaced && layoutReplacedHasUnsizedRatio(state, node, logical, input, reads))
            size = edges;
        else
            size = ofKind(intrinsic());
        // A maximum with a cyclic percentage counts as none, but at min-content it compresses a replaced box, as a
        // cyclic preferred size does. A minimum resolves its percentages against zero.
        PrLayoutLength maxLength = logical.maxInlineSize;
        if (layoutLengthIsFixed(maxLength) && !layoutLengthReadsBase(maxLength))
            size = layoutUnitMin(size, layoutBorderSize(box, layoutResolve(state.styles, maxLength, kLayoutUnitZero),
                                                        edges));
        else if (layoutLengthIsContentBased(maxLength))
            size = layoutUnitMin(size, keyword(maxLength));
        else if (layoutLengthIsFixed(maxLength) && replaced && min)
            size = edges;
        PrLayoutLength minLength = logical.minInlineSize;
        PrLayoutUnit least = edges;
        if (layoutLengthIsFixed(minLength))
            least = layoutBorderSize(box, layoutResolve(state.styles, minLength, kLayoutUnitZero), edges);
        else if (layoutLengthIsContentBased(minLength))
            least = keyword(minLength);
        size = layoutUnitMax(layoutUnitMax(size, least), edges) + margins;
        (min ? result.minContent : result.maxContent) = size;
    }
    result.maxContent = layoutUnitMax(result.maxContent, result.minContent);
    result.readsPercentBlock = reads || (measured && sizes.readsPercentBlock);
    result.readsFallback = measured && sizes.readsFallback;
    if (content != nullptr)
        *content = intrinsic();
    return result;
}

uint32_t layoutIntrinsicVerify(LayoutState& fresh) {
    const LayoutTree* tree = fresh.tree;
    for (LayoutNodeIndex node = 0; node < tree->cache.intrinsics.count; ++node) {
        const LayoutIntrinsicEntry cached = tree->cache.intrinsics.data[node];
        if (!cached.valid || !tree->nodes.data[node].live)
            continue;
        LayoutIntrinsicSizes sizes = layoutIntrinsicSizes(
            fresh, node, LayoutIntrinsicInput{cached.mode, cached.percentBlock, layoutUnit(-1), cached.fallback});
        if (sizes.minContent != cached.minContent || sizes.maxContent != cached.maxContent)
            return node;
    }
    // The cached sizes of an inline formatting context are read only through the sizes of its container, which this
    // function checks.
    return kPrLayoutNodeNone;
}

} // namespace pr_impl
