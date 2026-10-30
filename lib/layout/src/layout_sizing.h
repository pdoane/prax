// Sizing: a box's used edges, the sizes its sizing properties give, and the limits that clamp them.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-box-4 - https://drafts.csswg.org/css-box-4/
// css-position-3 - https://drafts.csswg.org/css-position-3/
// css-sizing-3 - https://drafts.csswg.org/css-sizing-3/
//
// Sizes are border-box sizes in units, whatever box-sizing measures. A negative size stands for a size that is not
// definite: an indefinite percentage base, or a size that its content decides.

#pragma once

#include "layout/src/layout_content.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Edges

// A box's used margins, padding and borders, by LayoutSide.
struct LayoutEdges {
    PrLayoutUnit margin[LayoutSide_Count];  // auto margins are zero
    PrLayoutUnit padding[LayoutSide_Count];
    PrLayoutUnit border[LayoutSide_Count];
    bool marginAuto[LayoutSide_Count];
};

// Returns the edges of `box` in a containing block of inline size `containingInline`, resolved against zero if that
// size is negative, as intrinsic sizing resolves them (css-sizing-3/#cyclic-percentage-contribution). Percentages of
// margins and padding on every side resolve against the containing block's inline size (css-box-4/#margin-physical).
LayoutEdges layoutEdgesFromBox(const LayoutStyles* styles, const LayoutLogicalBox& box, PrLayoutUnit containingInline);

// Returns the padding and borders across the inline axis, or across the block axis.
inline PrLayoutUnit layoutEdgesInline(const LayoutEdges& e) {
    return e.padding[LayoutSide_InlineStart] + e.padding[LayoutSide_InlineEnd] + e.border[LayoutSide_InlineStart] +
        e.border[LayoutSide_InlineEnd];
}
inline PrLayoutUnit layoutEdgesBlock(const LayoutEdges& e) {
    return e.padding[LayoutSide_BlockStart] + e.padding[LayoutSide_BlockEnd] + e.border[LayoutSide_BlockStart] +
        e.border[LayoutSide_BlockEnd];
}

// Returns the border and padding on `side`, between the border box and the content box.
inline PrLayoutUnit layoutEdgesContent(const LayoutEdges& e, LayoutSide side) {
    return e.border[side] + e.padding[side];
}

// ---------------------------------------------------------------------------------------------------------------------
// Sizes

// Returns a size given for the content box or the border box as a border-box size, using `edges` as the padding and
// borders.
inline PrLayoutUnit layoutBorderSize(const PrLayoutBoxStyle& box, PrLayoutUnit size, PrLayoutUnit edges) {
    return box.boxSizing == PrLayoutBoxSizing_BorderBox ? layoutUnitMax(size, edges) : size + edges;
}

// Returns true if `length` resolves against `base`: it is fixed, and it either needs no base or `base` is definite.
inline bool layoutResolves(PrLayoutLength length, PrLayoutUnit base) {
    return layoutLengthIsFixed(length) && (layoutIsDefinite(base) || !layoutLengthReadsBase(length));
}

// Returns the border-box size that fills `available` less the margins `start` and `end`, with auto margins taken as
// zero (css-sizing-3/#stretch-fit-size), or a negative size if `available` is negative.
inline PrLayoutUnit layoutStretchFit(PrLayoutUnit available, PrLayoutUnit start, PrLayoutUnit end) {
    return layoutIsDefinite(available) ? layoutUnitMax(kLayoutUnitZero, available - start - end) : layoutUnit(-1);
}

// Returns the border-box size that min-content, max-content or fit-content gives from the box's intrinsic sizes
// (css-sizing-3/#sizing-values). Fit-content shrinks to fit `available`, or is max-content if `available` is negative.
PrLayoutUnit layoutKeywordSize(PrLayoutLength keyword, const LayoutIntrinsicSizes& sizes, PrLayoutUnit available);

// Returns the intrinsic sizes `sizes`, measured with the inline edges `measured`, with those edges replaced by the used
// inline edges `used`. The content's sizes are intrinsic, but a used size has the edges that the box resolves in its
// containing block.
inline LayoutIntrinsicSizes layoutWithEdges(LayoutIntrinsicSizes sizes, PrLayoutUnit measured, PrLayoutUnit used) {
    sizes.minContent = sizes.minContent - measured + used;
    sizes.maxContent = sizes.maxContent - measured + used;
    return sizes;
}

// Returns a border-box size clamped by the box's minimum and maximum, with the minimum winning over the maximum
// (css-sizing-3/#min-size-properties). Each limit resolves against `base`, and is ignored if it reads the base and
// `base` is negative. A limit of stretch is the stretch-fit size `stretch`, and is ignored if that is negative. A limit
// of min-content, max-content or fit-content is the border-box size that `keyword` gives for it, and is ignored if that
// is negative.
template <typename Keyword>
PrLayoutUnit layoutClampSize(const LayoutStyles* styles, const PrLayoutBoxStyle& box, PrLayoutUnit size,
        PrLayoutLength min, PrLayoutLength max, PrLayoutUnit base, PrLayoutUnit edges, PrLayoutUnit stretch, Keyword keyword) {
    PrLayoutUnit limit = layoutUnit(-1);
    if (layoutLengthIsStretch(max) && layoutIsDefinite(stretch))
        size = layoutUnitMin(size, layoutUnitMax(stretch, edges));
    else if (layoutLengthIsContentBased(max) && layoutIsDefinite(limit = keyword(max)))
        size = layoutUnitMin(size, layoutUnitMax(limit, edges));
    else if (layoutResolves(max, base))
        size = layoutUnitMin(size, layoutBorderSize(box, layoutResolve(styles, max, base), edges));
    PrLayoutUnit least = edges;
    if (layoutLengthIsStretch(min) && layoutIsDefinite(stretch))
        least = layoutUnitMax(stretch, edges);
    else if (layoutLengthIsContentBased(min) && layoutIsDefinite(limit = keyword(min)))
        least = layoutUnitMax(limit, edges);
    else if (layoutResolves(min, base))
        least = layoutBorderSize(box, layoutResolve(styles, min, base), edges);
    return layoutUnitMax(size, least);
}

// A `keyword` function for layoutClampSize, for use if no limit that is a keyword applies.
inline PrLayoutUnit layoutNoKeyword(PrLayoutLength) {
    return layoutUnit(-1);
}

// The border-box block size that a box's own style gives before its content is laid out, and whether finding it read
// the percentage base, whether or not that base was resolved.
struct LayoutBlockBase {
    PrLayoutUnit size;              // negative if it depends on the content
    bool readsPercentBlock;
};

// Returns the block size of the box with edges `e` in a containing block whose block size is `percentBlock`, which is
// indefinite if negative (CSS2/#the-height-property). The size is a preferred size that resolves against that block
// size, or, if `stretchBase` is definite, a stretch to fill that margin box size. Either is clamped by the limits that
// resolve. An auto size also stretches if `autoSize` is Stretch, as the size of a box out of flow may
// (css-position-3/#abspos-auto-size). A stretched size is the border box less the margins, with auto margins as zero,
// and is never smaller than the box's padding and borders. A limit that is a keyword depends on the content, and clamps
// after layout instead. The box's content box, if definite, is what the children's percentages resolve against.
LayoutBlockBase layoutBlockBase(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
    const LayoutLogicalBox& logical, const LayoutEdges& e, PrLayoutUnit percentBlock,
    PrLayoutUnit stretchBase, LayoutAutoSize autoSize);
inline LayoutBlockBase layoutBlockBase(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
        const LayoutLogicalBox& logical, const LayoutEdges& e, PrLayoutUnit percentBlock) {
    return layoutBlockBase(styles, box, logical, e, percentBlock, percentBlock, LayoutAutoSize_Style);
}

// Returns the content-box size that a size or limit `length` of a box styled by `box` fixes against `base`, with
// `edges` of padding and borders in its axis. Returns a negative size if `length` is not fixed: it is auto, a keyword,
// or a percentage of an indefinite base.
inline PrLayoutUnit layoutFixedInner(const LayoutStyles* styles, const PrLayoutBoxStyle& box, PrLayoutLength length,
        PrLayoutUnit base, PrLayoutUnit edges) {
    if (!layoutResolves(length, base))
        return layoutUnit(-1);
    return layoutUnitMax(kLayoutUnitZero, layoutBorderSize(box, layoutResolve(styles, length, base), edges) - edges);
}

// Returns true if a box reads its containing block's block size or the size it stretches to, as layoutBlockBase and its
// clamp do. `logical` gives the box's block-axis sizes and `autoSize` its automatic block size. The box reads the base
// if a size or limit resolves against it, stretches, or depends on the content, or if its automatic size stretches.
bool layoutBlockReadsBase(const LayoutLogicalBox& logical, LayoutAutoSize autoSize);

// Returns the block size of the border box of a box styled by `box`, given the block axis that its style and its
// container decide before its content (`axis`) and the border-box size of its content, `content`. The size is the
// axis's size if that is definite, and otherwise the content's. If `clamps` is true, the box's limits in `logical`,
// resolved against `percentBlock`, clamp it, and a limit that is a keyword resolves to the content's size. A size that
// comes from an aspect ratio grows to `grown`, clamped in the same way. The content was laid out against the size
// before the clamp, and is not laid out again for it (css-sizing-3/#cyclic-percentage-contribution leaves this case
// open).
PrLayoutUnit layoutContentBlockSize(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
    const LayoutLogicalBox& logical, const LayoutBlockAxis& axis,
    PrLayoutUnit percentBlock, bool clamps, PrLayoutUnit content, PrLayoutUnit grown);

// Returns the limit that a block container styled by `box` gives an orthogonal flow root among its children if its
// block size is indefinite (LayoutSpace::blockLimit). The limit is its inner max block size, floored by its inner min
// block size, each resolved against `percentBlock` if fixed, or a negative size if its max block size is not fixed.
// `logical` is its style in its own writing mode, and `edges` its block-axis padding and borders.
PrLayoutUnit layoutBlockLimit(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const LayoutLogicalBox& logical,
    PrLayoutUnit percentBlock, PrLayoutUnit edges);

// Returns the fallback size that a scroll container gives its content (LayoutSpace::fallback,
// css-writing-modes-4/#orthogonal-auto) in each physical axis. If its size is fixed, the fallback is its inner size,
// capped by its inner max size if that is fixed. Otherwise, if its max size is fixed, the fallback is its inner max
// size, floored by its inner min size if that is fixed. Either way the fallback is capped by the initial containing
// block's `viewport`, which is used as is if neither its size nor its max size is fixed. `logical` is its style in its
// own writing mode `mode`, `bases` holds what its sizes resolve against, and `edges` holds its padding and borders
// across each axis.
PrLayoutVec layoutScrollFallback(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
    const LayoutLogicalBox& logical, LayoutMode mode, LayoutLogicalSize bases, LayoutLogicalSize edges, PrLayoutVec viewport);

// Sets the inline-start margin of `e` if auto margins share the space that a block-level box of inline size `size`
// leaves of `available` (CSS2/#blockwidth). Two auto margins center the box, never past the start, and an auto start
// margin alone takes all the space.
void layoutShareAutoMargins(LayoutEdges& e, PrLayoutUnit available, PrLayoutUnit size);

// ---------------------------------------------------------------------------------------------------------------------
// Positioning

// Returns how far relative positioning moves the box from its position in `space` (css-position-3/#relpos-insets). In
// each axis of the containing block's writing mode, the start inset wins over the end inset. The inset resolves against
// the containing block's size in that axis, or is auto if that size is indefinite. layoutRelativeOffset gives the
// offset for any box: the insets for a box that is relatively positioned, and zero otherwise.
LayoutLogicalOffset layoutRelativeInsets(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const LayoutSpace& space);
inline LayoutLogicalOffset layoutRelativeOffset(const LayoutStyles* styles, const PrLayoutBoxStyle& box,
        const LayoutSpace& space) {
    if (box.position == PrLayoutPosition_Relative)
        return layoutRelativeInsets(styles, box, space);
    return kLayoutLogicalOffsetZero;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
