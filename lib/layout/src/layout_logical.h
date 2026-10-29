// Logical geometry: the axes and sides a box lays out in, and their mapping to physical ones.
//
// css-writing-modes-4 - https://drafts.csswg.org/css-writing-modes-4/
//
// Layout reads a box's style through its logical view and places children at logical offsets. Fragments are physical,
// so this file holds every mapping between the two: a box's sides and sizes on the way in, and a child's offset and
// extent on the way out, once the parent's size is known.

#pragma once

#include "pr/layout_style.h"

#include "layout/src/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A box's used mode: its writing mode and direction, which decide its logical axes and sides. Zero is horizontal-tb and
// ltr, the initial values.
struct LayoutMode {
    PrLayoutWritingMode writingMode;
    PrTextDirection direction;    // the used direction, which upright text makes ltr (layoutUsedMode)
};

// Returns true if two modes are equal (==) or different (!=).
inline bool operator==(LayoutMode a, LayoutMode b) {
    return a.writingMode == b.writingMode && a.direction == b.direction;
}
inline bool operator!=(LayoutMode a, LayoutMode b) {
    return !(a == b);
}

// Returns true if the inline axis is horizontal.
inline bool layoutIsHorizontal(LayoutMode mode) {
    return mode.writingMode == PrLayoutWritingMode_HorizontalTb;
}

// Returns true if lines in `mode` are in a vertical typographic mode, which sets glyphs by text-orientation
// (css-writing-modes-4/#vertical-typographic-mode). Lines of the sideways modes are vertical, but typographically
// horizontal.
inline bool layoutIsVerticalTypographic(LayoutMode mode) {
    return mode.writingMode == PrLayoutWritingMode_VerticalRl || mode.writingMode == PrLayoutWritingMode_VerticalLr;
}

// Returns the used writing mode of a box whose font group is `font` (css-writing-modes-4/#writing-mode,
// #valdef-text-orientation-upright). It holds the box's writing-mode and direction, except that the direction is ltr if
// upright text sets a vertical typographic mode. Descendants still inherit the computed direction.
inline LayoutMode layoutUsedMode(const PrLayoutFontStyle& font) {
    LayoutMode mode{font.writingMode, font.direction};
    if (layoutIsVerticalTypographic(mode) && font.textOrientation == PrLayoutTextOrientation_Upright)
        mode.direction = PrTextDirection_Ltr;
    return mode;
}

// The sides of a box in logical terms (css-writing-modes-4/#logical-directions).
enum {
    LayoutSide_InlineStart,
    LayoutSide_InlineEnd,
    LayoutSide_BlockStart,
    LayoutSide_BlockEnd,
    LayoutSide_Count,
};
typedef uint8_t LayoutSide;

// The physical side of each logical side, by writing mode and direction (css-writing-modes-4/#logical-to-physical).
constexpr PrRectSide kLayoutPhysicalSides[5][2][LayoutSide_Count] = {
    {{PrRectSide_Left, PrRectSide_Right, PrRectSide_Top, PrRectSide_Bottom},
     {PrRectSide_Right, PrRectSide_Left, PrRectSide_Top, PrRectSide_Bottom}},
    {{PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Right, PrRectSide_Left},
     {PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Right, PrRectSide_Left}},
    {{PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Left, PrRectSide_Right},
     {PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Left, PrRectSide_Right}},
    {{PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Right, PrRectSide_Left},
     {PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Right, PrRectSide_Left}},
    {{PrRectSide_Bottom, PrRectSide_Top, PrRectSide_Left, PrRectSide_Right},
     {PrRectSide_Top, PrRectSide_Bottom, PrRectSide_Left, PrRectSide_Right}},
};

// Returns the physical side that the logical `side` maps to in `mode`, or the logical side that the physical `side`
// maps to.
inline PrRectSide layoutPhysicalSide(LayoutMode mode, LayoutSide side) {
    return kLayoutPhysicalSides[mode.writingMode][mode.direction][side];
}
LayoutSide layoutLogicalSide(LayoutMode mode, PrRectSide side);

// Returns true if an axis whose start is on physical side `start` runs from the right or the bottom.
inline bool layoutRunsBackward(PrRectSide start) {
    return start == PrRectSide_Right || start == PrRectSide_Bottom;
}

// Returns true if blocks stack the same way in two writing modes, because their block-start sides are the same.
inline bool layoutSameBlockFlow(LayoutMode a, LayoutMode b) {
    return layoutPhysicalSide(a, LayoutSide_BlockStart) == layoutPhysicalSide(b, LayoutSide_BlockStart);
}

// Returns true if a box in `mode` is in an orthogonal flow in a containing block in `outer`, which means that its lines
// would cross the lines of its containing block (css-writing-modes-4/#orthogonal-flows).
inline bool layoutIsOrthogonal(LayoutMode mode, LayoutMode outer) {
    return layoutIsHorizontal(mode) != layoutIsHorizontal(outer);
}

// Line-relative sides (css-writing-modes-4/#line-directions). Line-left is the inline-start side in ltr and the
// inline-end side in rtl, in every mode. Line-over is the top of horizontal lines, the right of vertical lines, and the
// left of sideways-lr lines, which read from the bottom.
inline LayoutSide layoutLineLeft(LayoutMode mode) {
    return mode.direction == PrTextDirection_Ltr ? LayoutSide_InlineStart : LayoutSide_InlineEnd;
}
inline LayoutSide layoutLineRight(LayoutMode mode) {
    return mode.direction == PrTextDirection_Ltr ? LayoutSide_InlineEnd : LayoutSide_InlineStart;
}
inline PrRectSide layoutLineOverSide(LayoutMode mode) {
    switch (mode.writingMode) {
        case PrLayoutWritingMode_HorizontalTb: return PrRectSide_Top;
        case PrLayoutWritingMode_SidewaysLr: return PrRectSide_Left;
        default: return PrRectSide_Right;
    }
}

// Returns true if the line-over side is the block-start side. Lines in such a mode are laid out from their line-over
// edge down the block axis, and lines in other modes are laid out from it up.
inline bool layoutLineOverIsBlockStart(LayoutMode mode) {
    return layoutLineOverSide(mode) == layoutPhysicalSide(mode, LayoutSide_BlockStart);
}

// Returns the writing mode that a line's content is placed in, for lines in `mode`
// (css-writing-modes-4/#line-mappings). Its inline-start side is the lines' line-left side, from which their content is
// placed in visual order, and its block-start side is their line-over side. Its direction is ltr. Its writing mode is
// the writing mode of `mode`, except that vertical-lr becomes vertical-rl, because vertical-lr lines are also laid out
// from their line-over side, the right.
inline LayoutMode layoutLineFrame(LayoutMode mode) {
    if (mode.writingMode == PrLayoutWritingMode_VerticalLr)
        mode.writingMode = PrLayoutWritingMode_VerticalRl;
    mode.direction = PrTextDirection_Ltr;
    return mode;
}

// An extent in logical terms.
struct LayoutLogicalSize {
    PrLayoutUnit inlineSize;
    PrLayoutUnit blockSize;
};

// How a box's auto size is determined in one of its axes: by its style and formatting context, as for a box in flow,
// or, if its containing block decides it, as the stretch-fit size or fit-content (css-position-3/#abspos-auto-size).
enum {
    LayoutAutoSize_Style,
    LayoutAutoSize_Stretch,
    LayoutAutoSize_FitContent,
};
typedef uint8_t LayoutAutoSize;

// An offset in logical terms: from the inline-start and block-start edges of what it is in.
struct LayoutLogicalOffset {
    PrLayoutUnit inlineOffset;
    PrLayoutUnit blockOffset;
};

// Returns the sum of two offsets, with saturating unit arithmetic.
inline LayoutLogicalOffset operator+(LayoutLogicalOffset a, LayoutLogicalOffset b) {
    return LayoutLogicalOffset{a.inlineOffset + b.inlineOffset, a.blockOffset + b.blockOffset};
}
inline LayoutLogicalOffset& operator+=(LayoutLogicalOffset& a, LayoutLogicalOffset b) {
    return a = a + b;
}

// Returns the offset of `a` from `b`, with saturating unit arithmetic.
inline LayoutLogicalOffset operator-(LayoutLogicalOffset a, LayoutLogicalOffset b) {
    return LayoutLogicalOffset{a.inlineOffset - b.inlineOffset, a.blockOffset - b.blockOffset};
}

// No offset.
constexpr LayoutLogicalOffset kLayoutLogicalOffsetZero = LayoutLogicalOffset{{0}, {0}};

// How far a box reaches over and under a baseline, in the frame of a line (layoutLineFrame).
struct LayoutLineExtent {
    PrLayoutUnit above;
    PrLayoutUnit below;
};

// Grows `extent` to hold `more`.
inline void layoutLineExtend(LayoutLineExtent& extent, LayoutLineExtent more) {
    extent.above = layoutUnitMax(extent.above, more.above);
    extent.below = layoutUnitMax(extent.below, more.below);
}

// Returns true if `extent` reaches neither over nor under its baseline.
inline bool layoutLineExtentIsEmpty(LayoutLineExtent extent) {
    return extent.above <= kLayoutUnitZero && extent.below <= kLayoutUnitZero;
}

// Returns true if two extents are the same.
inline bool operator==(LayoutLineExtent a, LayoutLineExtent b) {
    return a.above == b.above && a.below == b.below;
}

// Returns the physical extent of the logical `size` in `mode`, or the logical size of the physical `extent`.
inline PrLayoutVec layoutPhysicalSize(LayoutMode mode, LayoutLogicalSize size) {
    return layoutIsHorizontal(mode) ? PrLayoutVec{size.inlineSize, size.blockSize}
                                    : PrLayoutVec{size.blockSize, size.inlineSize};
}
inline LayoutLogicalSize layoutLogicalSize(LayoutMode mode, PrLayoutVec extent) {
    return layoutIsHorizontal(mode) ? LayoutLogicalSize{extent.x, extent.y} : LayoutLogicalSize{extent.y, extent.x};
}

// Returns the physical offset of the top-left corner of a child of size `size` at `offset` in a parent of size `outer`,
// both in the parent's `mode`.
inline PrLayoutVec layoutPhysicalOffset(LayoutMode mode, LayoutLogicalOffset offset, LayoutLogicalSize size,
                                        LayoutLogicalSize outer) {
    // On an axis that runs from the right or the bottom, the child's far edge is placed at its offset from the parent's
    // far edge.
    PrLayoutUnit inlineOffset = layoutRunsBackward(layoutPhysicalSide(mode, LayoutSide_InlineStart))
                                    ? outer.inlineSize - offset.inlineOffset - size.inlineSize
                                    : offset.inlineOffset;
    PrLayoutUnit blockOffset = layoutRunsBackward(layoutPhysicalSide(mode, LayoutSide_BlockStart))
                                   ? outer.blockSize - offset.blockOffset - size.blockSize
                                   : offset.blockOffset;
    return layoutIsHorizontal(mode) ? PrLayoutVec{inlineOffset, blockOffset} : PrLayoutVec{blockOffset, inlineOffset};
}

// A box's style in logical terms: its sizes and their limits by axis, and its edges and insets by LayoutSide.
struct LayoutLogicalBox {
    PrLayoutLength inlineSize;
    PrLayoutLength blockSize;
    PrLayoutLength minInlineSize;
    PrLayoutLength minBlockSize;
    PrLayoutLength maxInlineSize;
    PrLayoutLength maxBlockSize;
    PrLayoutLength margin[LayoutSide_Count];
    PrLayoutLength padding[LayoutSide_Count];
    PrLayoutLength inset[LayoutSide_Count];
    PrLayoutUnit border[LayoutSide_Count];
};

// Returns the logical view of `box` in `mode`, the box's own writing mode.
LayoutLogicalBox layoutLogicalBox(const PrLayoutBoxStyle& box, LayoutMode mode);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
