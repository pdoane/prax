// Sticky positioning: the offset that keeps a sticky box in its scrollport as its scroll container scrolls.

#include "layout/src/layout_sticky.h"

#include "layout/src/layout_cache.h"
#include "layout/src/layout_sizing.h"
#include "layout/src/layout_walk.h"

namespace pr_impl {

namespace {

// The scroll container of a sticky box, found along the containers around the box's own, and the nearest sticky box on
// the way, which moves it. A box that the viewport holds fixed is in the viewport as it is unscrolled
// (kLayoutContainerOutside). A box in a root other than the viewport's is in none.
struct StickyScroller {
    LayoutNodeIndex box;            // a scroll container, kLayoutContainerViewport, or kLayoutContainerOutside
    LayoutNodeIndex around;         // kPrLayoutNodeNone if no sticky box is on the way
    bool found;
};

StickyScroller scrollerOf(const LayoutTree* tree, LayoutNodeIndex node) {
    StickyScroller found{.box = kLayoutContainerOutside, .around = kPrLayoutNodeNone, .found = true};
    LayoutNodeIndex at = layoutStickyOuterContainer(tree, node);
    while (at != kLayoutContainerViewport && at != kLayoutContainerOutside) {
        LayoutNodeIndex box = layoutContainerBox(at);
        if (layoutContainerIsSticky(at)) {
            if (found.around == kPrLayoutNodeNone)
                found.around = box;
            at = layoutStickyOuterContainer(tree, box);
            continue;
        }
        if (layoutFragmentScrolls(tree, layoutCachePlaced(tree->cache, box))) {
            found.box = box;
            return found;
        }
        at = layoutBoxPositioned(tree, box).container;
    }
    // The viewport scrolls its root's content, and no other root's.
    found.box = at;
    found.found = layoutNodeRoot(tree, node) == tree->root;
    return found;
}

// Returns the scroll offset of `scroller` (StickyScroller::box).
PrLayoutVec scrollOf(const LayoutTree* tree, LayoutNodeIndex scroller) {
    if (scroller == kLayoutContainerOutside)
        return kLayoutVecZero;
    if (scroller == kLayoutContainerViewport)
        return layoutViewportScroll(tree, tree->fragment);
    return layoutFragmentScroll(tree, layoutCachePlaced(tree->cache, scroller));
}

// Returns `rect` less `inset`, by PrRectSide, and no smaller than empty.
PrLayoutRect deflated(PrLayoutRect rect, const PrLayoutUnit* inset) {
    rect.left += inset[PrRectSide_Left];
    rect.top += inset[PrRectSide_Top];
    rect.right = layoutUnitMax(rect.left, rect.right - inset[PrRectSide_Right]);
    rect.bottom = layoutUnitMax(rect.top, rect.bottom - inset[PrRectSide_Bottom]);
    return rect;
}

// Sets `out` to the edges `edges` of the box whose fragment is `fragment`, resolved against its containing block's
// inline size. An edge that does not resolve, as an auto margin does not, is zero.
void resolveEdges(const LayoutTree* tree, const LayoutFragment* fragment, const PrLayoutLength* edges, PrLayoutUnit* out) {
    PrLayoutUnit base = kLayoutUnitZero;
    if (fragment->kind == LayoutFragmentKind_Box)
        base = layoutUnitMax(fragment->box.containingInline, kLayoutUnitZero);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        out[side] =
            layoutResolves(edges[side], base) ? layoutResolve(tree->styles, edges[side], base) : kLayoutUnitZero;
}

// Returns the containing block of the sticky box `node` whose border box it stays in: its nearest ancestor that is
// neither an inline box nor a table track, so that a table's cells, rows and row groups stay in the table. Returns
// kPrLayoutNodeNone if there is none.
LayoutNodeIndex containingOf(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex at = layoutTreeNode(tree, node).parent;
    while (at != kPrLayoutNodeNone && (layoutIsInlineBox(tree, at) ||
            layoutDisplayIsTrack(layoutBoxStyle(tree->styles, layoutTreeNode(tree, at).style).display)))
        at = layoutTreeNode(tree, at).parent;
    return at;
}

// Returns the content box of the containing block `containing` of a sticky box in the scroll container `scroller`. A
// scroll container's content box is that of its scrollable overflow, so that a box in it sticks across the whole of
// what it scrolls.
PrLayoutRect contentOf(const LayoutTree* tree, LayoutNodeIndex containing, LayoutNodeIndex scroller) {
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, containing);
    PrLayoutRect unbounded{
        layoutUnit(-kLayoutClipUnbounded.raw), layoutUnit(-kLayoutClipUnbounded.raw), kLayoutClipUnbounded,
        kLayoutClipUnbounded,
    };
    if (fragment == nullptr || fragment->kind != LayoutFragmentKind_Box)
        return unbounded;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, layoutTreeNode(tree, containing).style);
    PrLayoutUnit padding[PrRectSide_Count];
    resolveEdges(tree, fragment, box.padding, padding);
    PrLayoutVec position = layoutNodePosition(tree, containing);
    if (containing == scroller) {
        PrLayoutRect port = layoutPaddingBox(tree, fragment, position);
        return deflated(layoutRectMoved(layoutScrollContent(fragment, box), PrLayoutVec{port.left, port.top}), padding);
    }
    return deflated(layoutPaddingBox(tree, fragment, position), padding);
}

// Returns true if the box `outer` is `node` or one of its ancestors.
bool holds(const LayoutTree* tree, LayoutNodeIndex outer, LayoutNodeIndex node) {
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
        if (at == outer)
            return true;
    }
    return false;
}

// Sets `out` to the border box of the inline box `node`, the bounds of its parts on lines and its rows, in the
// viewport's content as laid out. Returns false if the last layout gave it none.
bool inlineBorder(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutRect* out) {
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    const LayoutFragment* container = nullptr;
    if (placement.parent != kPrLayoutNodeNone)
        container = layoutCachePlaced(tree->cache, placement.parent);
    if (container == nullptr)
        return false;
    LayoutPieces pieces = layoutPiecesFrom(container, layoutNodePosition(tree, node), node, placement.at);
    LayoutPiece piece{};
    bool any = false;
    auto add = [&](PrLayoutRect rect) {
        *out = any ? layoutRectUnion(*out, rect) : rect;
        any = true;
    };
    while (layoutPiecesNext(pieces, &piece)) {
        if (piece.kind == LayoutPieceKind_Row) {
            add(layoutRectAt(piece.offset, piece.fragment->extent));
            continue;
        }
        for (uint32_t k = piece.first; k < piece.parts; ++k) {
            const LayoutChild& part = piece.fragment->children[k];
            add(layoutRectAt(piece.offset + part.offset, part.fragment->extent));
        }
    }
    return any;
}

// Sets `out` to what decides the offset of the sticky box `node` in the scroll container that `found` names. Returns
// false if its offset is always zero.
bool findIn(const LayoutTree* tree, LayoutNodeIndex node, const StickyScroller& found, LayoutSticky* out) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    if (!found.found)
        return false;
    // An inline box's border box is the bounds of its pieces, and its margins are not kept.
    const LayoutFragment* fragment = nullptr;
    PrLayoutRect border{};
    if (layoutIsInlineBox(tree, node)) {
        if (!inlineBorder(tree, node, &border))
            return false;
    } else {
        fragment = layoutCachePlaced(tree->cache, node);
        if (fragment == nullptr)
            return false;
        border = layoutRectAt(layoutNodePosition(tree, node), fragment->extent);
    }
    LayoutNodeIndex scroller = found.box;
    LayoutSticky sticky{};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        if (layoutLengthIsFixed(box.inset[side]))
            sticky.sides |= uint8_t(1u << side);
    }
    if (sticky.sides == 0)
        return false;
    // Percentages of the insets are of the scrollport's size in their axis. The insets are measured from inside the
    // scroll container's padding, as from its content box.
    PrLayoutRect port{};
    PrLayoutUnit padding[PrRectSide_Count] = {};
    if (scroller == kLayoutContainerViewport || scroller == kLayoutContainerOutside) {
        port = layoutRectAt(kLayoutVecZero, tree->fragment->extent);
    } else {
        const LayoutFragment* scrolled = layoutCachePlaced(tree->cache, scroller);
        port = layoutPaddingBox(tree, scrolled, layoutNodePosition(tree, scroller));
        resolveEdges(tree, scrolled, layoutBoxStyle(tree->styles, layoutTreeNode(tree, scroller).style).padding, padding);
    }
    sticky.port = deflated(port, padding);
    PrLayoutUnit width = layoutRectWidth(port);
    PrLayoutUnit height = layoutRectHeight(port);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        PrLayoutUnit base = side == PrRectSide_Left || side == PrRectSide_Right ? width : height;
        if ((sticky.sides & (1u << side)) != 0)
            sticky.inset[side] = layoutResolve(tree->styles, box.inset[side], base);
    }
    sticky.box = border;
    LayoutNodeIndex containing = containingOf(tree, node);
    PrLayoutRect content = sticky.box;
    LayoutMode mode = layoutOwnMode(tree, node);
    if (containing != kPrLayoutNodeNone) {
        content = contentOf(tree, containing, scroller);
        mode = layoutOwnMode(tree, containing);
    }
    PrLayoutUnit margin[PrRectSide_Count] = {};
    if (fragment != nullptr)
        resolveEdges(tree, fragment, box.margin, margin);
    sticky.limit = layoutRectUnion(deflated(content, margin), sticky.box);
    // A table's cell or track has the table as its containing block, which a sticky track around it does not move.
    for (LayoutNodeIndex at = found.around; at != kPrLayoutNodeNone && !holds(tree, at, containing);
            at = scrollerOf(tree, at).around)
        ++sticky.limitSkips;
    sticky.ends = uint8_t((1u << layoutPhysicalSide(mode, LayoutSide_InlineEnd)) |
        (1u << layoutPhysicalSide(mode, LayoutSide_BlockEnd)));
    *out = sticky;
    return true;
}

// Returns how far a sticky box moves in one axis: the sticky view rectangle runs from `low` to `high` in the axis, the
// border box from `boxLow` to `boxHigh`, and the limit from `limitLow` to `limitHigh`. `lowSide` and `highSide` are the
// sides at either end of the axis.
PrLayoutUnit shiftAlong(const LayoutSticky& sticky, PrLayoutUnit low, PrLayoutUnit high, PrLayoutUnit boxLow,
        PrLayoutUnit boxHigh, PrLayoutUnit limitLow, PrLayoutUnit limitHigh, PrRectSide lowSide, PrRectSide highSide) {
    bool lowSet = (sticky.sides & (1u << lowSide)) != 0;
    bool highSet = (sticky.sides & (1u << highSide)) != 0;
    if (!lowSet && !highSet)
        return kLayoutUnitZero;
    low += sticky.inset[lowSide];
    high -= sticky.inset[highSide];
    // A view rectangle smaller than the box grows past its end side (css-position-3/#stickypos-insets).
    PrLayoutUnit size = boxHigh - boxLow;
    if (high - low < size) {
        if ((sticky.ends & (1u << highSide)) != 0)
            high = low + size;
        else
            low = high - size;
    }
    PrLayoutUnit shift = kLayoutUnitZero;
    if (lowSet && boxLow < low)
        shift = low - boxLow;
    else if (highSet && boxHigh > high)
        shift = high - boxHigh;
    // The limit holds the box, so the range holds zero.
    return layoutUnitMax(limitLow - boxLow, layoutUnitMin(shift, limitHigh - boxHigh));
}

// Returns how far the sticky box `node` moves at the tree's scroll offsets, with the sticky boxes around it, and sets
// `own` to how far it moves of itself.
PrLayoutVec moved(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutVec* own) {
    StickyScroller scroller = scrollerOf(tree, node);
    PrLayoutVec ignored{};
    PrLayoutVec around = kLayoutVecZero;
    if (scroller.around != kPrLayoutNodeNone)
        around = moved(tree, scroller.around, &ignored);
    LayoutSticky sticky{};
    *own = kLayoutVecZero;
    if (findIn(tree, node, scroller, &sticky)) {
        // The limit moves with the sticky boxes around it past the first `limitSkips`.
        LayoutNodeIndex holder = scroller.around;
        for (uint32_t i = 0; i < sticky.limitSkips && holder != kPrLayoutNodeNone; ++i)
            holder = scrollerOf(tree, holder).around;
        PrLayoutVec limitAround = kLayoutVecZero;
        if (holder != kPrLayoutNodeNone)
            limitAround = holder == scroller.around ? around : moved(tree, holder, &ignored);
        *own = layoutStickyShift(sticky, scrollOf(tree, scroller.box), around, limitAround);
    }
    return around + *own;
}

} // namespace

bool layoutStickyFind(const LayoutTree* tree, LayoutNodeIndex node, LayoutSticky* out) {
    return findIn(tree, node, scrollerOf(tree, node), out);
}

PrLayoutVec layoutStickyShift(const LayoutSticky& sticky, PrLayoutVec scroll, PrLayoutVec around,
        PrLayoutVec limitAround) {
    PrLayoutRect view = layoutRectMoved(sticky.port, scroll);
    PrLayoutRect box = layoutRectMoved(sticky.box, around);
    PrLayoutRect limit = layoutRectMoved(sticky.limit, limitAround);
    return PrLayoutVec{
        shiftAlong(sticky, view.left, view.right, box.left, box.right, limit.left, limit.right, PrRectSide_Left,
            PrRectSide_Right),
        shiftAlong(sticky, view.top, view.bottom, box.top, box.bottom, limit.top, limit.bottom, PrRectSide_Top,
            PrRectSide_Bottom),
    };
}

PrLayoutVec layoutNodeStickyShift(const LayoutTree* tree, LayoutNodeIndex node) {
    PrLayoutVec own{};
    moved(tree, node, &own);
    return own;
}

} // namespace pr_impl
