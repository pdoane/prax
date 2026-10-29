// Floats: laying out a float, placing it among the floats of its block formatting context, and placing a box that
// avoids them.

#include "layout/src/layout_float.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

namespace {

// Return the line-left and the line-right margin of a box whose margins are `margin`, in `direction`.
PrLayoutUnit lineLeftMargin(const PrLayoutUnit* margin, PrTextDirection direction) {
    return direction == PrTextDirection_Ltr ? margin[LayoutSide_InlineStart] : margin[LayoutSide_InlineEnd];
}
PrLayoutUnit lineRightMargin(const PrLayoutUnit* margin, PrTextDirection direction) {
    return direction == PrTextDirection_Ltr ? margin[LayoutSide_InlineEnd] : margin[LayoutSide_InlineStart];
}

// Returns the span in which a box with line-left and line-right margins `marginLeft` and `marginRight` may put its
// border box over [y, y + h). The span lies within the containing block less the margins. If any floats meet the range,
// the span also lies beside them, but never past the containing block.
LayoutSpan borderBoxSpan(const LayoutContainingBlock& cb, PrLayoutUnit y, PrLayoutUnit h, PrLayoutUnit marginLeft,
                         PrLayoutUnit marginRight) {
    LayoutSpan span = layoutExclusionsAvailable(cb, y, h);
    PrLayoutUnit left = cb.lineLeft + marginLeft;
    PrLayoutUnit right = cb.lineRight - marginRight;
    if (!span.free) {
        left = layoutUnitMax(span.left, left);
        right = layoutUnitMin(span.right, right);
    }
    return LayoutSpan{left, layoutUnitMax(left, right), span.free};
}

// Returns true if `inner` is narrower than `outer` and within it.
bool narrows(const LayoutSpan& inner, const LayoutSpan& outer) {
    return inner.left.raw >= outer.left.raw && inner.right.raw <= outer.right.raw
           && (inner.left.raw > outer.left.raw || inner.right.raw < outer.right.raw);
}

} // namespace

LayoutAtomic layoutFloat(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& containing) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutAtomic f{};
    f.relative = layoutRelativeOffset(state.styles, box, containing);
    LayoutEdges e = layoutEdgesFromBox(state.styles, layoutLogicalBox(box, containing.mode), containing.percentInline);
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
        f.margin[side] = e.margin[side];
    LayoutSpace space = containing;
    space.available = containing.percentInline;
    space.bfc = nullptr;
    f.fragment = layoutBlock(state, node, space).fragment;
    return f;
}

LayoutExclusion layoutFloatPlace(const PrLayoutBoxStyle& box, const LayoutAtomic& f, const LayoutContainingBlock& cb,
                                 LayoutMode mode, PrLayoutUnit top) {
    LayoutLogicalSize size = layoutAtomicMarginBox(f, mode);
    return layoutExclusionsPlace(cb, size.inlineSize, size.blockSize, layoutFloatSide(box, mode.direction),
                                 layoutClearSides(box, mode.direction), top);
}

LayoutLogicalOffset layoutFloatOffset(const LayoutAtomic& f, const LayoutExclusion& placed,
                                      const LayoutContainingBlock& cb, LayoutMode mode,
                                      PrLayoutUnit contentTop) {
    LayoutLogicalSize size = layoutLogicalSize(mode, f.fragment->extent);
    LayoutLogicalOffset at{
        layoutInlineStartFromLeft(cb, mode.direction, placed.left + lineLeftMargin(f.margin, mode.direction),
                                  size.inlineSize),
        placed.top + f.margin[LayoutSide_BlockStart] - contentTop};
    return at + f.relative;
}

LayoutAvoider layoutAvoiderPlace(LayoutState& state, LayoutNodeIndex node, LayoutNodeIndex runLast,
                                 const LayoutSpace& content, const LayoutContainingBlock& cb, PrLayoutUnit top,
                                 const PrLayoutUnit* margin) {
    PrTextDirection direction = content.mode.direction;
    PrLayoutUnit marginLeft = lineLeftMargin(margin, direction);
    PrLayoutUnit marginRight = lineRightMargin(margin, direction);
    // At each offset, the box is laid out in the space beside the floats at that offset. It fits if the space over its
    // block size holds it. If that space is narrower, the box is laid out again in it. Otherwise the box moves down
    // past the next block-end edge of a float.
    for (PrLayoutUnit y = top;;) {
        LayoutSpan span = borderBoxSpan(cb, y, kLayoutUnitZero, marginLeft, marginRight);
        for (;;) {
            LayoutSpace space = content;
            space.available = span.right - span.left + marginLeft + marginRight;
            LayoutResult result = runLast != kPrLayoutNodeNone ? layoutAnonymousTable(state, node, runLast, space)
                                                               : layoutBlock(state, node, space);
            LayoutLogicalSize size = layoutLogicalSize(content.mode, result.fragment->extent);
            PrLayoutUnit left = span.left - marginLeft
                                + (direction == PrTextDirection_Ltr
                                       ? result.marginInlineStart
                                       : space.available - result.marginInlineStart - size.inlineSize);
            LayoutSpan need = borderBoxSpan(cb, y, size.blockSize, marginLeft, marginRight);
            if (need.free || (left.raw >= need.left.raw && (left + size.inlineSize).raw <= need.right.raw))
                return LayoutAvoider{result, y, left};
            layoutFragmentRelease(result.fragment);
            if (!narrows(need, span))
                break;
            span = need;
        }
        PrLayoutUnit next = layoutExclusionsNextEdge(cb, y);
        PR_ASSERT(next != kLayoutOffsetNone);
        y = next != kLayoutOffsetNone ? next : y + layoutUnitPx(1);
    }
}

} // namespace pr_impl
