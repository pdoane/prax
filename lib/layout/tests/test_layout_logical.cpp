#include "pr/layout_style.h"

#include "layout/src/layout_logical.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

constexpr LayoutMode kHorizontalLtr{PrLayoutWritingMode_HorizontalTb, PrTextDirection_Ltr};
constexpr LayoutMode kHorizontalRtl{PrLayoutWritingMode_HorizontalTb, PrTextDirection_Rtl};
constexpr LayoutMode kVerticalRlLtr{PrLayoutWritingMode_VerticalRl, PrTextDirection_Ltr};
constexpr LayoutMode kVerticalRlRtl{PrLayoutWritingMode_VerticalRl, PrTextDirection_Rtl};
constexpr LayoutMode kVerticalLrLtr{PrLayoutWritingMode_VerticalLr, PrTextDirection_Ltr};
constexpr LayoutMode kVerticalLrRtl{PrLayoutWritingMode_VerticalLr, PrTextDirection_Rtl};

PrLayoutUnit px(int32_t value) {
    return layoutUnitPx(value);
}

// Checks the physical sides of the logical ones, in LayoutSide order.
void checkSides(LayoutMode mode, PrRectSide inlineStart, PrRectSide inlineEnd, PrRectSide blockStart,
        PrRectSide blockEnd) {
    CHECK(layoutPhysicalSide(mode, LayoutSide_InlineStart) == inlineStart);
    CHECK(layoutPhysicalSide(mode, LayoutSide_InlineEnd) == inlineEnd);
    CHECK(layoutPhysicalSide(mode, LayoutSide_BlockStart) == blockStart);
    CHECK(layoutPhysicalSide(mode, LayoutSide_BlockEnd) == blockEnd);
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
        CHECK(layoutLogicalSide(mode, layoutPhysicalSide(mode, side)) == side);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("layout logical: sides map as css-writing-modes-4's table") {
    checkSides(kHorizontalLtr, PrRectSide_Left, PrRectSide_Right, PrRectSide_Top, PrRectSide_Bottom);
    checkSides(kHorizontalRtl, PrRectSide_Right, PrRectSide_Left, PrRectSide_Top, PrRectSide_Bottom);
    checkSides(kVerticalRlLtr, PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Right, PrRectSide_Left);
    checkSides(kVerticalRlRtl, PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Right, PrRectSide_Left);
    checkSides(kVerticalLrLtr, PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Left, PrRectSide_Right);
    checkSides(kVerticalLrRtl, PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Left, PrRectSide_Right);
}

TEST_CASE("layout logical: zero is horizontal-tb and ltr") {
    CHECK(LayoutMode{} == kHorizontalLtr);
    CHECK(LayoutMode{} != kHorizontalRtl);
}

TEST_CASE("layout logical: sizes swap axes in vertical modes") {
    LayoutLogicalSize size{px(30), px(10)};
    CHECK(layoutPhysicalSize(kHorizontalRtl, size) == PrLayoutVec{px(30), px(10)});
    CHECK(layoutPhysicalSize(kVerticalRlLtr, size) == PrLayoutVec{px(10), px(30)});
    LayoutLogicalSize back = layoutLogicalSize(kVerticalLrRtl, PrLayoutVec{px(10), px(30)});
    CHECK(back.inlineSize == px(30));
    CHECK(back.blockSize == px(10));
}

TEST_CASE("layout logical: offsets place the child from its parent's start corner") {
    // A child 30 along the inline axis and 10 across it, 5 from the inline start and 2 from the block start, in a
    // parent 100 by 50.
    LayoutLogicalOffset offset{px(5), px(2)};
    LayoutLogicalSize size{px(30), px(10)};
    LayoutLogicalSize outer{px(100), px(50)};
    CHECK(layoutPhysicalOffset(kHorizontalLtr, offset, size, outer) == PrLayoutVec{px(5), px(2)});
    CHECK(layoutPhysicalOffset(kHorizontalRtl, offset, size, outer) == PrLayoutVec{px(65), px(2)});
    CHECK(layoutPhysicalOffset(kVerticalRlLtr, offset, size, outer) == PrLayoutVec{px(38), px(5)});
    CHECK(layoutPhysicalOffset(kVerticalRlRtl, offset, size, outer) == PrLayoutVec{px(38), px(65)});
    CHECK(layoutPhysicalOffset(kVerticalLrLtr, offset, size, outer) == PrLayoutVec{px(2), px(5)});
    CHECK(layoutPhysicalOffset(kVerticalLrRtl, offset, size, outer) == PrLayoutVec{px(2), px(65)});
}

TEST_CASE("layout logical: a box's style maps by its writing mode") {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.width = prLayoutLengthPx(100);
    box.height = prLayoutLengthPx(50);
    box.minWidth = prLayoutLengthPx(10);
    box.maxHeight = prLayoutLengthPx(80);
    box.margin[PrRectSide_Top] = prLayoutLengthPx(1);
    box.padding[PrRectSide_Right] = prLayoutLengthPx(2);
    box.inset[PrRectSide_Left] = prLayoutLengthPx(3);
    box.border[PrRectSide_Bottom] = px(4);

    LayoutLogicalBox horizontal = layoutLogicalBox(box, kHorizontalRtl);
    CHECK(horizontal.inlineSize.bits == box.width.bits);
    CHECK(horizontal.blockSize.bits == box.height.bits);
    CHECK(horizontal.minInlineSize.bits == box.minWidth.bits);
    CHECK(horizontal.maxBlockSize.bits == box.maxHeight.bits);
    CHECK(horizontal.margin[LayoutSide_BlockStart].bits == box.margin[PrRectSide_Top].bits);
    CHECK(horizontal.padding[LayoutSide_InlineStart].bits == box.padding[PrRectSide_Right].bits);
    CHECK(horizontal.inset[LayoutSide_InlineEnd].bits == box.inset[PrRectSide_Left].bits);
    CHECK(horizontal.border[LayoutSide_BlockEnd] == px(4));

    LayoutLogicalBox vertical = layoutLogicalBox(box, kVerticalRlLtr);
    CHECK(vertical.inlineSize.bits == box.height.bits);
    CHECK(vertical.blockSize.bits == box.width.bits);
    CHECK(vertical.minBlockSize.bits == box.minWidth.bits);
    CHECK(vertical.maxInlineSize.bits == box.maxHeight.bits);
    CHECK(vertical.margin[LayoutSide_InlineStart].bits == box.margin[PrRectSide_Top].bits);
    CHECK(vertical.padding[LayoutSide_BlockStart].bits == box.padding[PrRectSide_Right].bits);
    CHECK(vertical.inset[LayoutSide_BlockEnd].bits == box.inset[PrRectSide_Left].bits);
    CHECK(vertical.border[LayoutSide_InlineEnd] == px(4));
}
