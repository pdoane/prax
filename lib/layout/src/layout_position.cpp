// Positioning: boxes out of flow, held by the boxes that are their containing blocks, and laid out and placed once
// those boxes are sized.

#include "layout/src/layout_position.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Insets

// The two physical axes, x and y.
constexpr uint32_t kAxes = 2;

// The low and high sides of each physical axis: left and right, then top and bottom.
constexpr PrRectSide kLow[kAxes] = {PrRectSide_Left, PrRectSide_Top};
constexpr PrRectSide kHigh[kAxes] = {PrRectSide_Right, PrRectSide_Bottom};

// Returns the low and high bounds of `rect` along `axis`.
PrLayoutUnit lowOf(PrLayoutRect rect, uint32_t axis) {
    return axis == 0 ? rect.left : rect.top;
}
PrLayoutUnit highOf(PrLayoutRect rect, uint32_t axis) {
    return axis == 0 ? rect.right : rect.bottom;
}
PrLayoutUnit along(PrLayoutVec v, uint32_t axis) {
    return axis == 0 ? v.x : v.y;
}

// The low or high side of a physical axis (left or right, top or bottom), or its center, if a static position centers a
// box.
enum {
    AxisEdge_Low,
    AxisEdge_High,
    AxisEdge_Center,
};
typedef uint8_t AxisEdge;

// The insets of a box out of flow in one physical axis of its containing block (css-position-3/#abspos-insets). Records
// which insets are auto as specified, and the inset-modified containing block that they leave, from the holder's
// top-left corner. Also records the edge that the box's margin box goes to if it is over-constrained.
struct InsetAxis {
    bool autoLow;
    bool autoHigh;
    PrLayoutUnit low;               // the inset-modified containing block
    PrLayoutUnit high;
    AxisEdge anchor;                // the stronger inset's edge
    AxisEdge start;                 // the containing block's start edge in the axis
    bool inlineAxis;                // the axis is the containing block's inline axis
    bool safe;                      // the static position's alignment is start where the box overflows
};

// Resolves the insets of `box` in `axis` of `containing`, using the static position of `pending` if both are auto
// (css-position-3/#resolving-inset-auto). The size of the inset-modified containing block is floored at zero by moving
// the inset that is not the anchor (csswg-drafts#11478).
InsetAxis insetAxis(const LayoutStyles* styles, const PrLayoutBoxStyle& box, const LayoutPending& pending,
        const LayoutAbsoluteBlock& containing, uint32_t axis) {
    InsetAxis a{};
    PrLayoutLength low = box.inset[kLow[axis]];
    PrLayoutLength high = box.inset[kHigh[axis]];
    a.autoLow = layoutLengthIsAuto(low);
    a.autoHigh = layoutLengthIsAuto(high);
    PrLayoutUnit lo = lowOf(containing.rect, axis);
    PrLayoutUnit hi = highOf(containing.rect, axis);
    PrLayoutUnit size = layoutUnitMax(kLayoutUnitZero, hi - lo);
    a.inlineAxis = layoutIsHorizontal(containing.mode) == (axis == 0);
    LayoutSide startSide = a.inlineAxis ? LayoutSide_InlineStart : LayoutSide_BlockStart;
    PrRectSide start = layoutPhysicalSide(containing.mode, startSide);
    a.start = layoutRunsBackward(start) ? AxisEdge_High : AxisEdge_Low;
    PrLayoutUnit insetLow = a.autoLow ? kLayoutUnitZero : layoutResolve(styles, low, size);
    PrLayoutUnit insetHigh = a.autoHigh ? kLayoutUnitZero : layoutResolve(styles, high, size);
    if (a.autoLow && a.autoHigh) {
        // The edge that the static position gives is the anchor, at the static position. A centered box's
        // inset-modified containing block is centered on the position, and is as wide as the containing block allows on
        // its nearer side.
        bool staticHigh = (pending.edges & (axis == 0 ? LayoutStaticEdge_Right : LayoutStaticEdge_Bottom)) != 0;
        bool staticCenter = (pending.edges & (axis == 0 ? LayoutStaticEdge_CenterX : LayoutStaticEdge_CenterY)) != 0;
        a.anchor = staticCenter ? AxisEdge_Center : staticHigh ? AxisEdge_High : AxisEdge_Low;
        a.safe = (pending.edges & (axis == 0 ? LayoutStaticEdge_SafeX : LayoutStaticEdge_SafeY)) != 0;
        PrLayoutUnit position = along(pending.at, axis);
        if (staticCenter) {
            PrLayoutUnit half = layoutUnitMax(kLayoutUnitZero, layoutUnitMin(position - lo, hi - position));
            insetLow = position - half - lo;
            insetHigh = hi - position - half;
        } else if (staticHigh) {
            insetHigh = hi - position;
        } else {
            insetLow = position - lo;
        }
    } else if (a.autoLow || a.autoHigh) {
        a.anchor = a.autoLow ? AxisEdge_High : AxisEdge_Low;
    } else {
        a.anchor = a.start;
    }
    a.low = lo + insetLow;
    a.high = hi - insetHigh;
    if (a.high < a.low) {
        if (a.anchor == AxisEdge_High)
            a.low = a.high;
        else
            a.high = a.low;
    }
    return a;
}

// Returns the position of the border box of a box of length `size` in `a`, with margins `low` and `high`, which are
// auto as `autoLow` and `autoHigh` say (css-position-3/#abspos-margins). Auto margins are zero if an inset is auto.
// Otherwise they share the space that the inset-modified containing block leaves, except that if that space is
// negative, the start margin in the inline axis is zero (csswg-drafts#11478). The margin box goes to the anchor's edge,
// or is centered in the inset-modified containing block if the anchor is its center, or goes to its start edge if the
// box of a safe static position overflows it.
PrLayoutUnit place(const InsetAxis& a, PrLayoutUnit size, PrLayoutUnit low, PrLayoutUnit high, bool autoLow,
        bool autoHigh) {
    if (a.safe && size + low + high > a.high - a.low)
        return a.start == AxisEdge_High ? a.high - high - size : a.low + low;
    if (a.anchor == AxisEdge_Center)
        return a.low + (a.high - a.low - size - low - high) / 2 + low;
    if (!a.autoLow && !a.autoHigh && (autoLow || autoHigh)) {
        PrLayoutUnit free = a.high - a.low - size - low - high;
        if (autoLow && autoHigh) {
            if (free < kLayoutUnitZero && a.inlineAxis) {
                bool startHigh = a.start == AxisEdge_High;
                (startHigh ? high : low) = kLayoutUnitZero;
                (startHigh ? low : high) = free;
            } else {
                low = free / 2;
                high = free - low;
            }
        } else if (autoLow) {
            low = free;
        } else {
            high = free;
        }
    }
    return a.anchor == AxisEdge_High ? a.high - high - size : a.low + low;
}

// ---------------------------------------------------------------------------------------------------------------------
// Inline boxes as containing blocks

// The parts of an inline box found in a block container's content: the first and last of its parts on lines, and the
// first and last of its rows around blocks. Each records its rectangle from the container's top-left corner and the
// sides it breaks at.
struct InlineParts {
    PrLayoutRect firstPart;
    PrLayoutRect lastPart;
    LayoutSideFlags firstBroken;
    LayoutSideFlags lastBroken;
    bool parts;
    PrLayoutRect firstRow;
    PrLayoutRect lastRow;
    bool rows;
};

// Returns how far toward the inline start of `mode` a rectangle's inline-start edge is. A larger value is further
// toward the start.
int64_t startward(PrLayoutRect rect, LayoutMode mode) {
    switch (layoutPhysicalSide(mode, LayoutSide_InlineStart)) {
        case PrRectSide_Left: return -int64_t(rect.left.raw);
        case PrRectSide_Right: return int64_t(rect.right.raw);
        case PrRectSide_Top: return -int64_t(rect.top.raw);
        default: return int64_t(rect.bottom.raw);
    }
}

// Adds the parts of the inline box `node` among the children of `fragment`, at `at` from the container's top-left
// corner. Parts on its lines are found through anonymous boxes, and its rows through the rows around them. On a line,
// the first and last parts are visual, in the box's own direction (csswg-drafts#7917).
void findParts(const LayoutFragment* fragment, PrLayoutVec at, LayoutNodeIndex node, LayoutMode mode, InlineParts& found) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        const LayoutFragment* f = child.fragment;
        PrLayoutVec offset = at + child.offset;
        switch (f->kind) {
            case LayoutFragmentKind_Line: {
                bool any = false;
                PrLayoutRect first{};
                PrLayoutRect last{};
                LayoutSideFlags firstBroken = 0;
                LayoutSideFlags lastBroken = 0;
                for (uint32_t k = 0; k < f->childCount; ++k) {
                    const LayoutChild& part = f->children[k];
                    if (part.fragment->kind != LayoutFragmentKind_Inline || part.fragment->node != node)
                        continue;
                    PrLayoutRect rect = layoutRectAt(offset + part.offset, part.fragment->extent);
                    if (!any || startward(rect, mode) > startward(first, mode)) {
                        first = rect;
                        firstBroken = part.fragment->broken;
                    }
                    if (!any || startward(rect, mode) < startward(last, mode)) {
                        last = rect;
                        lastBroken = part.fragment->broken;
                    }
                    any = true;
                }
                if (!any)
                    break;
                if (!found.parts) {
                    found.firstPart = first;
                    found.firstBroken = firstBroken;
                }
                found.lastPart = last;
                found.lastBroken = lastBroken;
                found.parts = true;
                break;
            }
            case LayoutFragmentKind_Around:
                if (f->node == node) {
                    PrLayoutRect rect = layoutRectAt(offset, f->extent);
                    if (!found.rows)
                        found.firstRow = rect;
                    found.lastRow = rect;
                    found.rows = true;
                } else {
                    findParts(f, offset, node, mode, found);
                }
                break;
            case LayoutFragmentKind_Box:
                if (f->node == kPrLayoutNodeNone)
                    findParts(f, offset, node, mode, found);
                break;
            default: break;
        }
    }
}

// Returns the containing block that the positioned inline box `node` forms from its parts in `fragment`, at `at` from
// the container's top-left corner (css-position-3/#def-cb, CSS2/#containing-block-details). It runs from the start
// corner of the first part's padding box to the end edges of the last part's padding box, in the box's writing mode,
// and its inline size is at least zero (csswg-drafts#8284). The box's rows around blocks count as first or last only if
// it has no part on a line.
PrLayoutRect inlineBlockOf(const LayoutState& state, const LayoutFragment* fragment, PrLayoutVec at,
        LayoutNodeIndex node, LayoutMode mode) {
    InlineParts found{};
    findParts(fragment, at, node, mode, found);
    if (!found.parts && !found.rows)
        return layoutRectAt(at, kLayoutVecZero);
    PrLayoutRect first = found.parts ? found.firstPart : found.firstRow;
    PrLayoutRect last = found.parts ? found.lastPart : found.lastRow;
    LayoutSideFlags firstBroken = found.parts ? found.firstBroken : LayoutSideFlags(0xf);
    LayoutSideFlags lastBroken = found.parts ? found.lastBroken : LayoutSideFlags(0xf);
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    // Each side comes from the part that has it, less its border if the box does not break there.
    PrLayoutRect rect{};
    for (LayoutSide side = 0; side < LayoutSide_Count; ++side) {
        bool fromFirst = side == LayoutSide_InlineStart || side == LayoutSide_BlockStart;
        PrRectSide physical = layoutPhysicalSide(mode, side);
        const PrLayoutRect& from = fromFirst ? first : last;
        bool broken = ((fromFirst ? firstBroken : lastBroken) & (1u << physical)) != 0;
        PrLayoutUnit border = broken ? kLayoutUnitZero : box.border[physical];
        switch (physical) {
            case PrRectSide_Left: rect.left = from.left + border; break;
            case PrRectSide_Top: rect.top = from.top + border; break;
            case PrRectSide_Right: rect.right = from.right - border; break;
            default: rect.bottom = from.bottom - border; break;
        }
    }
    // The inline size is clamped at zero, keeping the start edge, and so is the block size.
    const LayoutSide starts[] = {LayoutSide_InlineStart, LayoutSide_BlockStart};
    for (LayoutSide side : starts) {
        switch (layoutPhysicalSide(mode, side)) {
            case PrRectSide_Left: rect.right = layoutUnitMax(rect.right, rect.left); break;
            case PrRectSide_Top: rect.bottom = layoutUnitMax(rect.bottom, rect.top); break;
            case PrRectSide_Right: rect.left = layoutUnitMin(rect.left, rect.right); break;
            default: rect.top = layoutUnitMin(rect.top, rect.bottom); break;
        }
    }
    return rect;
}

// Returns the innermost inline box below the block container `container` that holds the box out of flow `node`, or
// kPrLayoutNodeNone. The lines of `container` hold `node`. A positioned inline box holds an absolutely positioned box,
// and a filtered inline box holds either kind.
LayoutNodeIndex inlineHolderOf(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex container) {
    PrLayoutPosition position = layoutNodePositionScheme(tree, node);
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != container && at != kPrLayoutNodeNone;
            at = layoutTreeNode(tree, at).parent) {
        if (layoutIsInlineBox(tree, at) && layoutHoldsPosition(tree, at, position))
            return at;
    }
    return kPrLayoutNodeNone;
}

// ---------------------------------------------------------------------------------------------------------------------
// Holding

// Clears the out-of-flow change marks from the ancestors of `node` below `holder`, or below the viewport if `holder` is
// kPrLayoutNodeNone. The holder has laid out the change, and a reused ancestor's descendants keep their marks.
void clearOutOfFlow(LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex holder) {
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != holder && at != kPrLayoutNodeNone;
            at = layoutTreeNode(tree, at).parent)
        tree->nodes.data[at].dirty &= LayoutDirtyFlags(~LayoutDirtyFlag_OutOfFlow);
}

// Lays out the boxes out of flow in the scratch's pending list from `pendingMark` on that `holds` says `holder` holds,
// each against the containing block that `holds` gives. Also lays out the boxes that those boxes list in turn, until
// none remain. Appends their fragments to the scratch's held children, and leaves the rest in the pending list, in
// order.
template <typename Holds>
void holdPending(LayoutState& state, LayoutNodeIndex holder, uint32_t pendingMark, Holds holds) {
    LayoutScratch& s = *state.scratch;
    for (uint32_t i = pendingMark; i < s.pending.count; ++i) {
        LayoutPending entry = s.pending.data[i];
        LayoutAbsoluteBlock containing{};
        if (!holds(entry, containing))
            continue;
        s.pending.data[i].node = kPrLayoutNodeNone;
        if (layoutTreeNode(state.tree, entry.node).dirty != 0)
            clearOutOfFlow(state.tree, entry.node, holder);
        s.held.push(layoutAbsolute(state, entry, containing));
        const LayoutChild& held = s.held.last();
        // The boxes that the box lists are in its subtree, which the holder's lists do not reach.
        LayoutPendingList list = layoutFragmentPending(held.fragment);
        for (uint32_t k = 0; k < list.count; ++k) {
            LayoutPending inner = list.entries[k];
            inner.at = inner.at + held.offset;
            inner.inLines = false;
            s.pending.push(inner);
        }
    }
    uint32_t kept = pendingMark;
    for (uint32_t i = pendingMark; i < s.pending.count; ++i) {
        if (s.pending.data[i].node != kPrLayoutNodeNone)
            s.pending.data[kept++] = s.pending.data[i];
    }
    s.pending.count = kept;
}

// Lays out the box out of flow that `pending` names against `containing`, and caches its position.
PR_NOINLINE LayoutChild layOutAbsolute(LayoutState& state, const LayoutPending& pending,
        const LayoutAbsoluteBlock& containing) {
    LayoutNodeIndex node = pending.node;
    const LayoutTreeNode& n = layoutTreeNode(state.tree, node);
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, n.style);
    bool replaced = n.kind == PrLayoutNodeKind_Replaced;
    InsetAxis axes[kAxes] = {
        insetAxis(state.styles, box, pending, containing, 0),
        insetAxis(state.styles, box, pending, containing, 1),
    };
    PrLayoutVec extent{
        layoutUnitMax(kLayoutUnitZero, containing.rect.right - containing.rect.left),
        layoutUnitMax(kLayoutUnitZero, containing.rect.bottom - containing.rect.top),
    };

    // The space is in the box's own axes, which are crossed if it is an orthogonal flow root. Its percentages resolve
    // against the containing block's sizes, and its automatic sizes fill the inset-modified containing block
    // (css-position-3/#abspos-auto-size). A replaced box sizes in its containing block's axes.
    bool inlineX = replaced ? layoutIsHorizontal(containing.mode) : layoutIsHorizontal(layoutOwnMode(state.tree, node));
    uint32_t inlineAxis = inlineX ? 0 : 1;
    uint32_t blockAxis = 1 - inlineAxis;
    // A box with a preferred aspect ratio sizes its ratio-dependent axis through the ratio rather than by stretching.
    // That axis is the one whose size is auto. If both sizes are auto, it is the one with an auto inset while the other
    // axis has none, and otherwise it is the block axis.
    uint32_t dependent = kAxes;
    if (!replaced) {
        LayoutMode own = layoutOwnMode(state.tree, node);
        if (layoutNodeAspect(state.tree, node, box, own).ratio > 0) {
            LayoutLogicalBox logical = layoutLogicalBox(box, own);
            bool autoInline = layoutLengthIsAuto(logical.inlineSize);
            bool autoBlock = layoutLengthIsAuto(logical.blockSize);
            bool insetInline = axes[inlineAxis].autoLow || axes[inlineAxis].autoHigh;
            bool insetBlock = axes[blockAxis].autoLow || axes[blockAxis].autoHigh;
            if (autoInline && (!autoBlock || (insetInline && !insetBlock)))
                dependent = inlineAxis;
            else if (autoBlock)
                dependent = blockAxis;
        }
    }
    auto autoSizeIn = [&](uint32_t axis) {
        bool stretches = !replaced && axis != dependent && !axes[axis].autoLow && !axes[axis].autoHigh;
        return LayoutAutoSize(stretches ? LayoutAutoSize_Stretch : LayoutAutoSize_FitContent);
    };
    LayoutSpace space{
        .mode = containing.mode,
        .available = axes[inlineAxis].high - axes[inlineAxis].low,
        .percentInline = along(extent, inlineAxis),
        .percentBlock = along(extent, blockAxis),
        .containing = layoutLogicalSize(containing.mode, extent),
        .blockLimit = layoutUnit(-1),
        .fallback = containing.fallback,
        .autoInline = autoSizeIn(inlineAxis),
        .autoBlock = autoSizeIn(blockAxis),
        .stretchBlock = axes[blockAxis].high - axes[blockAxis].low,
    };
    LayoutResult result = layoutBlock(state, node, space);

    // Its margins resolve against the containing block's inline size (css-box-4/#margin-physical).
    PrLayoutUnit base = space.containing.inlineSize;
    PrLayoutVec at{};
    for (uint32_t axis = 0; axis < kAxes; ++axis) {
        PrLayoutLength low = box.margin[kLow[axis]];
        PrLayoutLength high = box.margin[kHigh[axis]];
        bool autoLow = layoutLengthIsAuto(low);
        bool autoHigh = layoutLengthIsAuto(high);
        PrLayoutUnit placed = place(axes[axis], along(result.fragment->extent, axis),
            autoLow ? kLayoutUnitZero : layoutResolve(state.styles, low, base),
            autoHigh ? kLayoutUnitZero : layoutResolve(state.styles, high, base), autoLow, autoHigh);
        (axis == 0 ? at.x : at.y) = placed;
    }
    layoutCacheStoreAbsolute(state, node, containing, pending.at, pending.edges, at);
    return LayoutChild{result.fragment, at};
}

} // namespace

LayoutStaticEdges layoutStaticEdges(LayoutMode mode) {
    LayoutStaticEdges edges = 0;
    const LayoutSide starts[] = {LayoutSide_InlineStart, LayoutSide_BlockStart};
    for (LayoutSide side : starts) {
        PrRectSide physical = layoutPhysicalSide(mode, side);
        if (physical == PrRectSide_Right)
            edges |= LayoutStaticEdge_Right;
        else if (physical == PrRectSide_Bottom)
            edges |= LayoutStaticEdge_Bottom;
    }
    return edges;
}

LayoutStaticEdges layoutStaticEdgesAligned(LayoutMode mode, LayoutStaticAlign inlineAlign,
        LayoutStaticAlign blockAlign, bool inlineSafe, bool blockSafe) {
    LayoutStaticEdges edges = 0;
    const LayoutSide starts[] = {LayoutSide_InlineStart, LayoutSide_BlockStart};
    const LayoutStaticAlign aligns[] = {inlineAlign, blockAlign};
    const bool safe[] = {inlineSafe, blockSafe};
    for (uint32_t i = 0; i < 2; ++i) {
        PrRectSide physical = layoutPhysicalSide(mode, starts[i]);
        bool horizontal = physical == PrRectSide_Left || physical == PrRectSide_Right;
        if (safe[i])
            edges |= horizontal ? LayoutStaticEdge_SafeX : LayoutStaticEdge_SafeY;
        if (aligns[i] == LayoutStaticAlign_Center) {
            edges |= horizontal ? LayoutStaticEdge_CenterX : LayoutStaticEdge_CenterY;
            continue;
        }
        // The aligned edge: the start side, or the side opposite it.
        bool high = physical == PrRectSide_Right || physical == PrRectSide_Bottom;
        if (aligns[i] == LayoutStaticAlign_End)
            high = !high;
        if (high)
            edges |= horizontal ? LayoutStaticEdge_Right : LayoutStaticEdge_Bottom;
    }
    return edges;
}

LayoutChild layoutAbsolute(LayoutState& state, const LayoutPending& pending, const LayoutAbsoluteBlock& containing) {
    LayoutChild cached{};
    if (layoutCacheFindAbsolute(state, pending.node, containing, pending.at, pending.edges, &cached))
        return cached;
    return layOutAbsolute(state, pending, containing);
}

LayoutFragment* layoutHolderBuild(LayoutState& state, LayoutNodeIndex node, uint32_t mark, LayoutMode mode,
        LayoutLogicalSize size, PrLayoutVec fallback, const LayoutGridLines* PR_NULLABLE grid) {
    LayoutFragment* built = layoutFragmentBuild(state.scratch->children, mark, LayoutFragmentKind_Box, node, mode, size);
    if ((built->flags & LayoutFragmentFlag_Pending) == 0)
        return built;
    // What it may hold: the boxes that its position makes it the containing block of, and the absolutely positioned
    // boxes in its lines, which may be in a positioned inline box. A static box holds nothing, and its list passes up
    // unchanged.
    bool holdsAbsolute = layoutHoldsAbsolute(state.tree, node);
    bool holdsFixed = layoutHoldsFixed(state.tree, node);
    LayoutPendingList list = layoutFragmentPending(built);
    bool any = false;
    for (uint32_t i = 0; i < list.count && !any; ++i) {
        const LayoutPending& entry = list.entries[i];
        any = (entry.fixed ? holdsFixed : holdsAbsolute) || entry.inLines;
    }
    if (!any)
        return built;

    LayoutScratch& s = *state.scratch;
    uint32_t pendingMark = s.pending.count;
    uint32_t heldMark = s.held.count;
    uint32_t inlineMark = s.inlineBlocks.count;
    s.pending.append(list.entries, list.count);
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    PrLayoutVec extent = built->extent;
    LayoutAbsoluteBlock own{
        PrLayoutRect{
            box.border[PrRectSide_Left], box.border[PrRectSide_Top],
            layoutUnitMax(box.border[PrRectSide_Left], extent.x - box.border[PrRectSide_Right]),
            layoutUnitMax(box.border[PrRectSide_Top], extent.y - box.border[PrRectSide_Bottom]),
        },
        mode, fallback,
    };
    // The containing block of the boxes that a table holds is the table's wrapper box, inside its margins
    // (css-tables-3/#abspos-boxes-in-table-root).
    if (layoutIsTable(box))
        own.rect = layoutRectAt(kLayoutVecZero, extent);
    // The containing block of each positioned inline box in its lines, found once from its parts.
    auto inlineBlock = [&](LayoutNodeIndex inlineBox) {
        for (uint32_t i = inlineMark; i < s.inlineBlocks.count; ++i) {
            if (s.inlineBlocks.data[i].node == inlineBox)
                return s.inlineBlocks.data[i].block;
        }
        LayoutMode inlineMode = layoutOwnMode(state.tree, inlineBox);
        LayoutAbsoluteBlock block{
            inlineBlockOf(state, built, kLayoutVecZero, inlineBox, inlineMode), inlineMode,
            fallback,
        };
        s.inlineBlocks.push(LayoutInlineBlock{inlineBox, block});
        return block;
    };
    // A positioned inline box in its lines is nearer than the container itself, and the container holds what no inline
    // box holds.
    holdPending(state, node, pendingMark, [&](const LayoutPending& entry, LayoutAbsoluteBlock& containing) {
        if (entry.inLines) {
            LayoutNodeIndex inlineBox = inlineHolderOf(state.tree, entry.node, node);
            if (inlineBox != kPrLayoutNodeNone) {
                containing = inlineBlock(inlineBox);
                return true;
            }
        }
        if (entry.fixed ? holdsFixed : holdsAbsolute) {
            containing = own;
            if (grid != nullptr)
                containing.rect = layoutGridAbsoluteArea(state, *grid, entry.node, own.rect, extent);
            return true;
        }
        return false;
    });
    s.inlineBlocks.count = inlineMark;
    // A container whose lines hold no positioned inline box holds nothing, and its fragment is the one built.
    if (s.held.count == heldMark) {
        s.pending.count = pendingMark;
        return built;
    }
    LayoutFragment* fragment = layoutFragmentHold(built, s.held.data + heldMark, s.held.count - heldMark,
        s.pending.data + pendingMark, s.pending.count - pendingMark);
    s.held.count = heldMark;
    s.pending.count = pendingMark;
    return fragment;
}

void layoutViewportHold(LayoutState& state, const LayoutFragment* root, PrLayoutVec at, PrLayoutVec extent,
        LayoutMode mode, RawArray<LayoutChild>& out) {
    LayoutScratch& s = *state.scratch;
    LayoutPendingList list = layoutFragmentPending(root);
    if (list.count == 0)
        return;
    uint32_t pendingMark = s.pending.count;
    uint32_t heldMark = s.held.count;
    for (uint32_t i = 0; i < list.count; ++i) {
        LayoutPending entry = list.entries[i];
        entry.at = entry.at + at;
        s.pending.push(entry);
    }
    // The initial containing block, and for fixed boxes the unscrolled viewport: one rectangle at the origin.
    LayoutAbsoluteBlock initial{layoutRectAt(kLayoutVecZero, extent), mode, extent};
    holdPending(state, kPrLayoutNodeNone, pendingMark, [&](const LayoutPending&, LayoutAbsoluteBlock& containing) {
        containing = initial;
        return true;
    });
    PR_ASSERT(s.pending.count == pendingMark);
    out.append(s.held.data + heldMark, s.held.count - heldMark);
    s.held.count = heldMark;
}

} // namespace pr_impl
