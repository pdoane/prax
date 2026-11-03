// Replaced boxes: sized from their style and their content's natural dimensions, and laid out as one box.

#include "layout/src/layout_replaced.h"

#include "pr/float.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

namespace {

// A replaced box's size and its edges, by LayoutSide.
struct ReplacedBox {
    LayoutLogicalSize size;         // the border box
    LayoutLogicalSize contentSize;
    PrLayoutUnit contentEdge[LayoutSide_Count];  // the border and padding between the border box and the content box
    PrLayoutUnit margin[LayoutSide_Count];       // auto margins are zero
    bool marginAuto[LayoutSide_Count];
};

// How a replaced box's inline size is found: as layout sizes it, or as its min-content or max-content size, with an
// auto inline size and no inline limits (css-sizing-3/#intrinsic-sizes).
enum {
    ReplacedFit_Layout,
    ReplacedFit_MinContent,
    ReplacedFit_MaxContent,
};
typedef uint8_t ReplacedFit;

// The inputs, beyond its space, that size a replaced box.
struct ReplacedPolicy {
    ReplacedFit fit;
    bool autoBlock;                 // true to size the block axis as auto with no limits, for a keyword block limit

    // What its flex container asks of its sizes, if it is a flex item.
    const LayoutRequests* PR_NULLABLE requests;
};

// Returns the size of the replaced box of `node` in `space` under `policy` (CSS2/#inline-replaced-width,
// CSS2/#inline-replaced-height, CSS2/#min-max-widths). Sets `reads` if finding it read the space's block-axis
// percentage base, whether or not that base was resolved.
ReplacedBox replacedBox(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space, ReplacedPolicy policy, bool& reads) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutLogicalBox logical = layoutLogicalBox(box, space.mode);
    LayoutEdges e = layoutEdgesFromBox(state.styles, logical, space.containing.inlineSize);
    ReplacedBox r{};
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        r.margin[side] = e.margin[side];
        r.marginAuto[side] = e.marginAuto[side];
        r.contentEdge[side] = e.border[side] + e.padding[side];
    }
    PrLayoutUnit edgesInline = layoutEdgesInline(e);
    PrLayoutUnit edgesBlock = layoutEdgesBlock(e);
    PrLayoutUnit stretchInline =
        layoutStretchFit(space.available, e.margin[LayoutSide_InlineStart], e.margin[LayoutSide_InlineEnd]);
    PrLayoutUnit stretchBlock =
        layoutStretchFit(space.percentBlock, e.margin[LayoutSide_BlockStart], e.margin[LayoutSide_BlockEnd]);

    // Sizes of the content box in units, or below zero if the style leaves the size to the content.
    auto contentSize = [&](PrLayoutLength length, PrLayoutUnit base, PrLayoutUnit stretch, PrLayoutUnit edges) -> double {
        if (layoutLengthIsStretch(length))
            return layoutIsDefinite(stretch) ? double(layoutUnitMax(kLayoutUnitZero, stretch - edges).raw) : -1;
        if (!layoutResolves(length, base))
            return -1;
        PrLayoutUnit size = layoutResolve(state.styles, length, base);
        if (box.boxSizing == PrLayoutBoxSizing_BorderBox)
            size = size - edges;
        return double(layoutUnitMax(kLayoutUnitZero, size).raw);
    };
    // The content inline size that min-content, max-content or fit-content gives, from the box's intrinsic sizes.
    auto keywordInline = [&](PrLayoutLength length) -> double {
        LayoutIntrinsicSizes sizes =
            layoutWithEdges(layoutIntrinsicSizes(state, node, layoutIntrinsicInputFromSpace(space)),
                layoutEdgesInline(layoutEdgesFromBox(state.styles, logical, layoutUnit(-1))), edgesInline);
        reads = reads || sizes.readsPercentBlock;
        PrLayoutUnit size = layoutKeywordSize(length, sizes, stretchInline) - edgesInline;
        return double(layoutUnitMax(kLayoutUnitZero, size).raw);
    };
    // The content block size as auto with no block limits, which a block limit that is a keyword uses.
    auto keywordBlock = [&](PrLayoutLength) -> double {
        ReplacedPolicy unlimited{.fit = policy.fit, .autoBlock = true, .requests = policy.requests};
        return double(replacedBox(state, node, space, unlimited, reads).contentSize.blockSize.raw);
    };
    // A minimum that is a <length>, as a content size, or negative. An intrinsic size with no natural size falls back
    // to it (css-sizing-3/#intrinsic-sizes).
    auto lengthMinimum = [&](PrLayoutLength length, PrLayoutUnit edges) -> double {
        if (!layoutLengthIsFixed(length) || layoutLengthReadsBase(length))
            return -1;
        return contentSize(length, kLayoutUnitZero, layoutUnit(-1), edges);
    };
    bool intrinsic = policy.fit != ReplacedFit_Layout;
    // A size that its container decides is used as it is, with no limits, and a size that the container asks of the
    // content is auto with no limits.
    LayoutRequest inlineRequest = layoutInlineRequest(policy.requests);
    LayoutRequest blockRequest = layoutBlockRequest(policy.requests);
    bool fixedInline = !intrinsic && inlineRequest.kind == LayoutRequestKind_Fixed;
    bool fixedBlock = !policy.autoBlock && blockRequest.kind == LayoutRequestKind_Fixed;
    bool requestsBlock = blockRequest.kind != LayoutRequestKind_Own;
    double inlineSize = -1;
    if (fixedInline) {
        inlineSize = double(layoutUnitMax(kLayoutUnitZero, inlineRequest.size - edgesInline).raw);
    } else if (!intrinsic) {
        if (layoutLengthIsContentBased(logical.inlineSize))
            inlineSize = keywordInline(logical.inlineSize);
        else
            inlineSize = contentSize(logical.inlineSize, space.percentInline, stretchInline, edgesInline);
    }
    double blockSize = -1;
    if (fixedBlock) {
        blockSize = double(layoutUnitMax(kLayoutUnitZero, blockRequest.size - edgesBlock).raw);
    } else if (!policy.autoBlock && !requestsBlock) {
        blockSize = contentSize(logical.blockSize, space.percentBlock, stretchBlock, edgesBlock);
        const PrLayoutLength lengths[] = {logical.blockSize, logical.minBlockSize, logical.maxBlockSize};
        for (PrLayoutLength length : lengths)
            reads = reads || layoutLengthReadsBase(length) || layoutLengthIsStretch(length);
    }
    const PrLayoutNaturalSize& natural = layoutReplaced(state.tree, node).natural;
    LayoutLogicalSize naturalSize = layoutLogicalSize(space.mode, PrLayoutVec{natural.width, natural.height});
    double naturalInline = naturalSize.inlineSize.raw >= 0 ? double(naturalSize.inlineSize.raw) : -1;
    double naturalBlock = naturalSize.blockSize.raw >= 0 ? double(naturalSize.blockSize.raw) : -1;
    LayoutAspect aspect = layoutNodeAspect(state.tree, node, box, space.mode);
    double ratio = aspect.ratio;
    // A preferred aspect ratio of the box's own replaces the ratio of a natural size. The size keeps the natural inline
    // size, or else the natural block size (css-sizing-4/#aspect-ratio).
    if (ratio > 0 && !aspect.natural && naturalInline >= 0)
        naturalBlock = -1;
    // The content size that the other axis's content size gives through the ratio. The ratio relates border boxes if
    // box-sizing says so.
    auto toBlock = [&](double inlineSize) {
        if (!aspect.borderBox)
            return inlineSize / ratio;
        return prF64Max(0, (inlineSize + double(edgesInline.raw)) / ratio - double(edgesBlock.raw));
    };
    auto toInline = [&](double blockSize) {
        if (!aspect.borderBox)
            return blockSize * ratio;
        return prF64Max(0, (blockSize + double(edgesBlock.raw)) * ratio - double(edgesInline.raw));
    };

    // The limits, with a maximum never below its minimum. If both sizes are left to a ratio, they keep the ratio as
    // they meet the limits (CSS2/#min-max-widths), and other sizes meet each limit independently. Intrinsic sizes have
    // no inline limits, and a block size measured for a block limit that is a keyword has no block limits.
    auto limit = [&](PrLayoutLength length, PrLayoutUnit base, PrLayoutUnit stretch, PrLayoutUnit edges,
            double otherwise, bool inlineAxis) {
        if ((inlineAxis && (intrinsic || fixedInline)) || (!inlineAxis && (policy.autoBlock || requestsBlock)))
            return otherwise;
        double size;
        if (!layoutLengthIsContentBased(length))
            size = contentSize(length, base, stretch, edges);
        else if (inlineAxis)
            size = keywordInline(length);
        else
            size = keywordBlock(length);
        return size >= 0 ? size : otherwise;
    };
    constexpr double kNoLimit = 1e18;
    double minInline = limit(logical.minInlineSize, space.percentInline, stretchInline, edgesInline, 0, true);
    double maxInline = prF64Max(minInline,
        limit(logical.maxInlineSize, space.percentInline, stretchInline, edgesInline, kNoLimit, true));
    double minBlock = limit(logical.minBlockSize, space.percentBlock, stretchBlock, edgesBlock, 0, false);
    double maxBlock =
        prF64Max(minBlock, limit(logical.maxBlockSize, space.percentBlock, stretchBlock, edgesBlock, kNoLimit, false));

    // An auto size is the natural size, or the size that the other size and the ratio give, or else the default object
    // size of 300 by 150 (css-images-3/#default-object-size). With only a ratio, the inline size fills the containing
    // block in layout, is zero at min-content, and at max-content is a length minimum through the ratio, or else the
    // initial containing block's size. With neither a natural size nor a ratio, an intrinsic size is a length minimum
    // before it falls back to the default.
    LayoutLogicalSize defaultSize = layoutLogicalSize(space.mode, PrLayoutVec{layoutUnitPx(300), layoutUnitPx(150)});
    double defaultInline = double(defaultSize.inlineSize.raw);
    double defaultBlock = double(defaultSize.blockSize.raw);
    bool autoInline = inlineSize < 0;
    bool autoBlock = blockSize < 0;
    if (autoInline && autoBlock) {
        if (naturalInline >= 0) {
            inlineSize = naturalInline;
            blockSize = naturalBlock >= 0 ? naturalBlock : ratio > 0 ? toBlock(inlineSize) : defaultBlock;
        } else if (naturalBlock >= 0) {
            blockSize = naturalBlock;
            inlineSize = ratio > 0 ? toInline(blockSize) : defaultInline;
        } else if (ratio > 0) {
            if (policy.fit == ReplacedFit_MinContent) {
                inlineSize = 0;
            } else if (policy.fit == ReplacedFit_MaxContent) {
                double fromBlock = lengthMinimum(logical.minBlockSize, edgesBlock);
                inlineSize = prF64Max(lengthMinimum(logical.minInlineSize, edgesInline),
                    fromBlock >= 0 ? toInline(fromBlock) : -1);
                if (inlineSize < 0) {
                    inlineSize = double(layoutLogicalSize(space.mode, state.viewport).inlineSize.raw);
                    if (state.reuse)
                        layoutTreeReadViewport(state.tree, node);
                }
            } else {
                if (layoutIsDefinite(stretchInline))
                    inlineSize = double(layoutUnitMax(kLayoutUnitZero, stretchInline - edgesInline).raw);
                else
                    inlineSize = defaultInline;
            }
            blockSize = toBlock(inlineSize);
        } else {
            double minimum = intrinsic ? lengthMinimum(logical.minInlineSize, edgesInline) : -1;
            inlineSize = minimum >= 0 ? minimum : defaultInline;
            blockSize = defaultBlock;
        }
    } else if (autoInline) {
        // The ratio transfers the used block size, with its limits applied (CSS2/#inline-replaced-width).
        double fallback = defaultInline;
        if (intrinsic && lengthMinimum(logical.minInlineSize, edgesInline) >= 0)
            fallback = lengthMinimum(logical.minInlineSize, edgesInline);
        blockSize = prF64Min(prF64Max(blockSize, minBlock), maxBlock);
        inlineSize = ratio > 0 ? toInline(blockSize) : naturalInline >= 0 ? naturalInline : fallback;
    } else if (autoBlock) {
        inlineSize = prF64Min(prF64Max(inlineSize, minInline), maxInline);
        blockSize = ratio > 0 ? toBlock(inlineSize) : naturalBlock >= 0 ? naturalBlock : defaultBlock;
    }

    if (autoInline && autoBlock && ratio > 0 && inlineSize > 0 && blockSize > 0) {
        // CSS2's table, which names the inline size w and the block size h.
        double w = inlineSize;
        double h = blockSize;
        if (w > maxInline && h > maxBlock) {
            if (maxInline / w <= maxBlock / h) {
                inlineSize = maxInline;
                blockSize = prF64Max(minBlock, toBlock(maxInline));
            } else {
                inlineSize = prF64Max(minInline, toInline(maxBlock));
                blockSize = maxBlock;
            }
        } else if (w < minInline && h < minBlock) {
            if (minInline / w <= minBlock / h) {
                inlineSize = prF64Min(maxInline, toInline(minBlock));
                blockSize = minBlock;
            } else {
                inlineSize = minInline;
                blockSize = prF64Min(maxBlock, toBlock(minInline));
            }
        } else if (w < minInline && h > maxBlock) {
            inlineSize = minInline;
            blockSize = maxBlock;
        } else if (w > maxInline && h < minBlock) {
            inlineSize = maxInline;
            blockSize = minBlock;
        } else if (w > maxInline) {
            inlineSize = maxInline;
            blockSize = prF64Max(toBlock(maxInline), minBlock);
        } else if (w < minInline) {
            inlineSize = minInline;
            blockSize = prF64Min(toBlock(minInline), maxBlock);
        } else if (h > maxBlock) {
            inlineSize = prF64Max(toInline(maxBlock), minInline);
            blockSize = maxBlock;
        } else if (h < minBlock) {
            inlineSize = prF64Min(toInline(minBlock), maxInline);
            blockSize = minBlock;
        }
    } else {
        inlineSize = prF64Min(prF64Max(inlineSize, minInline), maxInline);
        blockSize = prF64Min(prF64Max(blockSize, minBlock), maxBlock);
    }

    // Sizing computed counts of units in doubles, each rounded to the nearest unit.
    r.contentSize = LayoutLogicalSize{
        prLayoutUnitFromPxRoundDouble(inlineSize / double(kPrLayoutUnitsPerPx)),
        prLayoutUnitFromPxRoundDouble(blockSize / double(kPrLayoutUnitsPerPx)),
    };
    r.size = LayoutLogicalSize{r.contentSize.inlineSize + edgesInline, r.contentSize.blockSize + edgesBlock};
    return r;
}

// Creates a fragment of the replaced box of `node` sized as `box` in `mode`, with one reference.
LayoutFragment* replacedFragment(LayoutNodeIndex node, const ReplacedBox& box, LayoutMode mode) {
    LayoutFragment* fragment = layoutFragmentCreate(LayoutFragmentKind_Replaced, node, 0);
    fragment->extent = layoutPhysicalSize(mode, box.size);
    PrLayoutVec origin{
        box.contentEdge[layoutLogicalSide(mode, PrRectSide_Left)],
        box.contentEdge[layoutLogicalSide(mode, PrRectSide_Top)],
    };
    fragment->replaced.content = layoutRectAt(origin, layoutPhysicalSize(mode, box.contentSize));
    return fragment;
}

} // namespace

LayoutResult layoutReplacedBlock(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& space,
        const LayoutRequests* PR_NULLABLE requests) {
    bool reads = false;
    ReplacedPolicy policy{.fit = ReplacedFit_Layout, .requests = requests};
    ReplacedBox r = replacedBox(state, node, space, policy, reads);
    bool floats = layoutIsFloat(state.tree, node);
    PrLayoutUnit remaining = space.available - r.size.inlineSize - r.margin[LayoutSide_InlineStart] -
        r.margin[LayoutSide_InlineEnd];
    PrLayoutUnit start = r.margin[LayoutSide_InlineStart];
    if (!floats && layoutInlineRequest(requests).kind != LayoutRequestKind_Fixed) {
        if (r.marginAuto[LayoutSide_InlineStart] && r.marginAuto[LayoutSide_InlineEnd])
            start = layoutUnitMax(kLayoutUnitZero, remaining / 2);
        else if (r.marginAuto[LayoutSide_InlineStart])
            start = remaining;
    }
    LayoutFragment* fragment = replacedFragment(node, r, space.mode);
    if (floats)
        fragment->flags |= LayoutFragmentFlag_Float;
    if (layoutSpaceIsOutOfFlow(space))
        fragment->flags |= LayoutFragmentFlag_OutOfFlow;
    LayoutResult result{};
    result.fragment = fragment;
    // A box out of flow's containing block decides its percentages.
    if (reads || layoutSpaceIsOutOfFlow(space))
        result.flags |= LayoutResultFlag_ReadsBlockBase;
    result.start = layoutStrut(r.margin[LayoutSide_BlockStart]);
    result.end = layoutStrut(r.margin[LayoutSide_BlockEnd]);
    result.marginInlineStart = start;
    return result;
}

LayoutIntrinsicSizes layoutReplacedIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutSpace space{
        .mode = input.mode,
        .available = layoutUnit(-1),
        .percentInline = layoutUnit(-1),
        .percentBlock = input.percentBlock,
        .containing = LayoutLogicalSize{layoutUnit(-1), input.percentBlock},
        .blockLimit = input.blockLimit,
        .fallback = input.fallback,
    };
    LayoutIntrinsicSizes sizes{};
    sizes.minContent = replacedBox(state, node, space, ReplacedPolicy{.fit = ReplacedFit_MinContent},
        sizes.readsPercentBlock).size.inlineSize;
    sizes.maxContent = replacedBox(state, node, space, ReplacedPolicy{.fit = ReplacedFit_MaxContent},
        sizes.readsPercentBlock).size.inlineSize;
    return sizes;
}

bool layoutReplacedHasUnsizedRatio(const LayoutState& state, LayoutNodeIndex node, const LayoutLogicalBox& logical,
        const LayoutIntrinsicInput& input, bool& reads) {
    const PrLayoutNaturalSize& natural = layoutReplaced(state.tree, node).natural;
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    if (natural.width.raw >= 0 || natural.height.raw >= 0 || layoutNodeAspect(state.tree, node, box, input.mode).ratio <= 0)
        return false;
    reads = reads || layoutLengthReadsBase(logical.blockSize) || layoutLengthIsStretch(logical.blockSize);
    bool stretch = layoutLengthIsStretch(logical.blockSize) && layoutIsDefinite(input.percentBlock);
    return !stretch && !layoutResolves(logical.blockSize, input.percentBlock);
}

} // namespace pr_impl
