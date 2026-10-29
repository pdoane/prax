// Logical geometry: the axes and sides a box lays out in, and their mapping to physical ones.
//
// css-writing-modes-4 - https://drafts.csswg.org/css-writing-modes-4/

#include "layout/src/layout_logical.h"

namespace pr_impl {

LayoutSide layoutLogicalSide(LayoutMode mode, PrRectSide side) {
    LayoutSide logical = 0;
    while (logical + 1 < LayoutSide_Count && layoutPhysicalSide(mode, logical) != side)
        ++logical;
    return logical;
}

LayoutLogicalBox layoutLogicalBox(const PrLayoutBoxStyle& box, LayoutMode mode) {
    LayoutLogicalBox logical{};
    bool horizontal = layoutIsHorizontal(mode);
    logical.inlineSize = horizontal ? box.width : box.height;
    logical.blockSize = horizontal ? box.height : box.width;
    logical.minInlineSize = horizontal ? box.minWidth : box.minHeight;
    logical.minBlockSize = horizontal ? box.minHeight : box.minWidth;
    logical.maxInlineSize = horizontal ? box.maxWidth : box.maxHeight;
    logical.maxBlockSize = horizontal ? box.maxHeight : box.maxWidth;
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        PrRectSide physical = layoutPhysicalSide(mode, side);
        logical.margin[side] = box.margin[physical];
        logical.padding[side] = box.padding[physical];
        logical.inset[side] = box.inset[physical];
        logical.border[side] = box.border[physical];
    }
    return logical;
}

} // namespace pr_impl
