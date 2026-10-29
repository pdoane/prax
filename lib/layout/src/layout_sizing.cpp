// Sizing: a box's used edges, the sizes its sizing properties give, and the limits that clamp them.

#include "layout/src/layout_sizing.h"

namespace pr_impl {

LayoutEdges layoutEdgesFromBox(const LayoutStyles* styles, const LayoutLogicalBox& box, PrLayoutUnit containingInline) {
    LayoutEdges edges{};
    containingInline = layoutUnitMax(kLayoutUnitZero, containingInline);
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        edges.marginAuto[side] = layoutLengthIsAuto(box.margin[side]);
        edges.margin[side] =
            edges.marginAuto[side] ? kLayoutUnitZero : layoutResolve(styles, box.margin[side], containingInline);
        edges.padding[side] =
            layoutUnitMax(kLayoutUnitZero, layoutResolve(styles, box.padding[side], containingInline));
        edges.border[side] = box.border[side];
    }
    return edges;
}

PrLayoutUnit layoutKeywordSize(PrLayoutLength keyword, const LayoutIntrinsicSizes& sizes, PrLayoutUnit available) {
    if (keyword.bits == prLayoutLengthMinContent().bits)
        return sizes.minContent;
    if (keyword.bits == prLayoutLengthMaxContent().bits || !layoutIsDefinite(available))
        return sizes.maxContent;
    return layoutUnitMin(layoutUnitMax(sizes.minContent, available), sizes.maxContent);
}

LayoutBlockBase layoutBlockBase(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
                                const LayoutLogicalBox& logical, const LayoutEdges& e, PrLayoutUnit percentBlock,
                                PrLayoutUnit stretchBase, LayoutAutoSize autoSize) {
    PrLayoutUnit edges = layoutEdgesBlock(e);
    PrLayoutUnit stretch =
        layoutStretchFit(stretchBase, e.margin[LayoutSide_BlockStart], e.margin[LayoutSide_BlockEnd]);
    LayoutBlockBase base{layoutUnit(-1),
                         layoutLengthReadsBase(logical.blockSize) || layoutLengthIsStretch(logical.blockSize)};
    bool isAuto = !layoutLengthIsFixed(logical.blockSize) && !layoutLengthIsContentBased(logical.blockSize)
                  && !layoutLengthIsStretch(logical.blockSize);
    bool stretchSize = (layoutLengthIsStretch(logical.blockSize) || (isAuto && autoSize == LayoutAutoSize_Stretch))
                       && layoutIsDefinite(stretch);
    if (!stretchSize && !layoutResolves(logical.blockSize, percentBlock))
        return base;
    PrLayoutUnit size = stretchSize
                            ? layoutUnitMax(stretch, edges)
                            : layoutBorderSize(box, layoutResolve(styles, logical.blockSize, percentBlock), edges);
    base.size = layoutClampSize(styles, box, size, logical.minBlockSize, logical.maxBlockSize, percentBlock, edges,
                                stretch, layoutNoKeyword);
    const PrLayoutLength limits[] = {logical.minBlockSize, logical.maxBlockSize};
    for (PrLayoutLength limit : limits)
        base.readsPercentBlock = base.readsPercentBlock || layoutLengthReadsBase(limit) || layoutLengthIsStretch(limit);
    return base;
}

bool layoutBlockReadsBase(const LayoutLogicalBox& logical, LayoutAutoSize autoSize) {
    auto reads = [](PrLayoutLength length) {
        return layoutLengthReadsBase(length) || layoutLengthIsStretch(length) || layoutLengthIsContentBased(length);
    };
    bool isAuto = !layoutLengthIsFixed(logical.blockSize) && !reads(logical.blockSize);
    return reads(logical.blockSize) || (isAuto && autoSize == LayoutAutoSize_Stretch) || reads(logical.minBlockSize)
           || reads(logical.maxBlockSize);
}

PrLayoutUnit layoutContentBlockSize(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
                                    const LayoutLogicalBox& logical, const LayoutBlockAxis& axis,
                                    PrLayoutUnit percentBlock, bool clamps, PrLayoutUnit content, PrLayoutUnit grown) {
    PrLayoutUnit size = layoutIsDefinite(axis.size) ? axis.size : content;
    if (clamps)
        size = layoutClampSize(styles, box, size, logical.minBlockSize, logical.maxBlockSize, percentBlock, axis.edges,
                               axis.stretch, [&](PrLayoutLength) { return content; });
    if (!axis.growsToContent)
        return size;
    return layoutUnitMax(size, layoutClampSize(styles, box, grown, logical.minBlockSize, logical.maxBlockSize,
                                               percentBlock, axis.edges, axis.stretch,
                                               [&](PrLayoutLength) { return grown; }));
}

PrLayoutUnit layoutBlockLimit(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const LayoutLogicalBox& logical,
                              PrLayoutUnit percentBlock, PrLayoutUnit edges) {
    PrLayoutUnit max = layoutFixedInner(styles, box, logical.maxBlockSize, percentBlock, edges);
    if (!layoutIsDefinite(max))
        return max;
    return layoutUnitMax(max, layoutFixedInner(styles, box, logical.minBlockSize, percentBlock, edges));
}

PrLayoutVec layoutScrollFallback(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
                                 const LayoutLogicalBox& logical, LayoutMode mode, LayoutLogicalSize bases,
                                 LayoutLogicalSize edges, PrLayoutVec viewport) {
    LayoutLogicalSize initial = layoutLogicalSize(mode, viewport);
    auto axis = [&](PrLayoutLength size, PrLayoutLength min, PrLayoutLength max, PrLayoutUnit base,
                    PrLayoutUnit edge, PrLayoutUnit icb) {
        PrLayoutUnit fixed = layoutFixedInner(styles, box, size, base, edge);
        PrLayoutUnit limit = layoutFixedInner(styles, box, max, base, edge);
        if (layoutIsDefinite(fixed) && layoutIsDefinite(limit))
            fixed = layoutUnitMin(fixed, limit);
        else if (!layoutIsDefinite(fixed))
            fixed = limit;
        if (!layoutIsDefinite(fixed))
            return icb;
        return layoutUnitMin(layoutUnitMax(fixed, layoutFixedInner(styles, box, min, base, edge)), icb);
    };
    LayoutLogicalSize fallback{
        axis(logical.inlineSize, logical.minInlineSize, logical.maxInlineSize, bases.inlineSize, edges.inlineSize,
             initial.inlineSize),
        axis(logical.blockSize, logical.minBlockSize, logical.maxBlockSize, bases.blockSize, edges.blockSize,
             initial.blockSize)};
    return layoutPhysicalSize(mode, fallback);
}

void layoutShareAutoMargins(LayoutEdges& e, PrLayoutUnit available, PrLayoutUnit size) {
    PrLayoutUnit start = e.marginAuto[LayoutSide_InlineStart] ? kLayoutUnitZero : e.margin[LayoutSide_InlineStart];
    PrLayoutUnit end = e.marginAuto[LayoutSide_InlineEnd] ? kLayoutUnitZero : e.margin[LayoutSide_InlineEnd];
    PrLayoutUnit remaining = available - size - start - end;
    if (e.marginAuto[LayoutSide_InlineStart] && e.marginAuto[LayoutSide_InlineEnd])
        e.margin[LayoutSide_InlineStart] = layoutUnitMax(kLayoutUnitZero, remaining / 2);
    else if (e.marginAuto[LayoutSide_InlineStart])
        e.margin[LayoutSide_InlineStart] = remaining;
}

LayoutLogicalOffset layoutRelativeInsets(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
                                         const LayoutSpace& space) {
    LayoutLogicalOffset offset = kLayoutLogicalOffsetZero;
    PrLayoutLength inlineStart = box.inset[layoutPhysicalSide(space.mode, LayoutSide_InlineStart)];
    PrLayoutLength inlineEnd = box.inset[layoutPhysicalSide(space.mode, LayoutSide_InlineEnd)];
    PrLayoutLength blockStart = box.inset[layoutPhysicalSide(space.mode, LayoutSide_BlockStart)];
    PrLayoutLength blockEnd = box.inset[layoutPhysicalSide(space.mode, LayoutSide_BlockEnd)];
    PrLayoutUnit inlineBase = space.containing.inlineSize;
    PrLayoutUnit blockBase = space.containing.blockSize;
    if (layoutResolves(inlineStart, inlineBase))
        offset.inlineOffset = layoutResolve(styles, inlineStart, inlineBase);
    else if (layoutResolves(inlineEnd, inlineBase))
        offset.inlineOffset = -layoutResolve(styles, inlineEnd, inlineBase);
    if (layoutResolves(blockStart, blockBase))
        offset.blockOffset = layoutResolve(styles, blockStart, blockBase);
    else if (layoutResolves(blockEnd, blockBase))
        offset.blockOffset = -layoutResolve(styles, blockEnd, blockBase);
    return offset;
}

} // namespace pr_impl
