// Fragmentation: fragmentainers, break tokens and break values.

#include "layout/src/layout_fragmentation.h"

#include "pr/alloc.h"
#include "pr/memory.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_style.h"
#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// Returns what `value` asks in a context of `kind` (css-break-4/#break-between), and sets `*side` for a page break to a
// side of the spread.
LayoutBreakAsk askOf(PrLayoutBreak value, LayoutFragmentainerKind kind, LayoutBreakSide* side) {
    bool page = kind == LayoutFragmentainerKind_Page;
    LayoutBreakAsk pageAsk = page ? LayoutBreakAsk(LayoutBreakAsk_Force) : LayoutBreakAsk(LayoutBreakAsk_Auto);
    *side = LayoutBreakSide_Any;
    switch (value) {
        case PrLayoutBreak_Avoid: return LayoutBreakAsk_Avoid;
        case PrLayoutBreak_AvoidPage: return page ? LayoutBreakAsk(LayoutBreakAsk_Avoid) : LayoutBreakAsk_Auto;
        case PrLayoutBreak_AvoidColumn: return page ? LayoutBreakAsk(LayoutBreakAsk_Auto) : LayoutBreakAsk_Avoid;
        case PrLayoutBreak_Always:
        case PrLayoutBreak_All: return LayoutBreakAsk_Force;
        case PrLayoutBreak_Page: return pageAsk;
        case PrLayoutBreak_Left: *side = LayoutBreakSide_Left; return pageAsk;
        case PrLayoutBreak_Right: *side = LayoutBreakSide_Right; return pageAsk;
        case PrLayoutBreak_Recto: *side = LayoutBreakSide_Recto; return pageAsk;
        case PrLayoutBreak_Verso: *side = LayoutBreakSide_Verso; return pageAsk;
        case PrLayoutBreak_Column: return page ? LayoutBreakAsk(LayoutBreakAsk_Auto) : LayoutBreakAsk_Force;
        default: return LayoutBreakAsk_Auto;
    }
}

// Returns true if `node` is a box in flow at block level, whose break values propagate.
bool inFlowBlock(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    if (!layoutChildKindIsBlock(layoutChildKind(tree, node)))
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    return box.floating == PrLayoutFloat_None && !layoutPositionIsOutOfFlow(box.position);
}

// Returns true if `node` is text of white space that collapses away beside block-level boxes, which generates no box
// (css-text-3/#white-space-phase-1).
bool collapsesAway(const LayoutTree* tree, LayoutNodeIndex node, const LayoutTreeNode& parent) {
    if (!layoutIsBlankText(tree, node))
        return false;
    PrLayoutWhiteSpaceCollapse collapse = layoutFontStyle(tree->styles, parent.style).whiteSpaceCollapse;
    return collapse == PrLayoutWhiteSpaceCollapse_Collapse || collapse == PrLayoutWhiteSpaceCollapse_Discard;
}

// The edge of a box from which the values of its children propagate to it.
enum {
    Propagation_None,           // nothing propagates
    Propagation_Order,          // the first child in document order is at the block-start edge
    Propagation_Reversed,       // the last child in document order is at the block-start edge
};
typedef uint8_t Propagation;

// Returns how the values of the children of `node` at its edges propagate to it. Nothing propagates from monolithic
// content, whose breaks are ignored (css-break-4/#monolithic), or from a row flex container whose lines are reversed,
// whose first line in the block axis is its last. A column-reverse flex container has its last item at its block-start
// edge.
Propagation propagationOf(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Replaced || n.kind == PrLayoutNodeKind_SemiReplaced)
        return Propagation_None;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    if (layoutIsScrollContainer(box) || box.containBlock)
        return Propagation_None;
    if (n.children != LayoutChildren_Flex)
        return Propagation_Order;
    const PrLayoutFlexStyle& flex = layoutFlexStyle(tree->styles, n.style);
    if (flex.direction == PrLayoutFlexDirection_ColumnReverse)
        return Propagation_Reversed;
    bool row = flex.direction == PrLayoutFlexDirection_Row || flex.direction == PrLayoutFlexDirection_RowReverse;
    return row && flex.wrap == PrLayoutFlexWrap_WrapReverse ? Propagation(Propagation_None) : Propagation_Order;
}

// Returns the child of `node` in flow at block level at its block-start edge, or at its block-end edge if `last`, or
// none if its content starts or ends with inline content, it has none, or its children's values do not propagate to it.
LayoutNodeIndex edgeChild(const LayoutTree* tree, LayoutNodeIndex node, bool last) {
    Propagation propagation = propagationOf(tree, node);
    if (propagation == Propagation_None)
        return kPrLayoutNodeNone;
    if (propagation == Propagation_Reversed)
        last = !last;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    for (LayoutNodeIndex c = last ? n.last : n.first; c != kPrLayoutNodeNone;
            c = last ? layoutTreeNode(tree, c).previous : layoutTreeNode(tree, c).next) {
        LayoutChildKind kind = layoutChildKind(tree, c);
        if (kind == LayoutChildKind_None || kind == LayoutChildKind_OutOfFlow || collapsesAway(tree, c, n))
            continue;
        return inFlowBlock(tree, c) ? c : kPrLayoutNodeNone;
    }
    return kPrLayoutNodeNone;
}

// Returns what `node`'s break-before, or its break-after if `after`, asks, with the values that propagate to it.
LayoutBreakAsk edgeAsk(const LayoutTree* tree, LayoutNodeIndex node, bool after, LayoutFragmentainerKind kind,
        LayoutBreakSide* side) {
    LayoutBreakAsk ask = LayoutBreakAsk_Auto;
    *side = LayoutBreakSide_Any;
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = edgeChild(tree, at, after)) {
        const PrLayoutFragmentationStyle& own = layoutFragmentationStyle(tree->styles, layoutTreeNode(tree, at).style);
        LayoutBreakSide atSide = LayoutBreakSide_Any;
        LayoutBreakAsk atAsk = askOf(after ? own.breakAfter : own.breakBefore, kind, &atSide);
        if (atAsk > ask || (atAsk == ask && *side == LayoutBreakSide_Any)) {
            ask = atAsk > ask ? atAsk : ask;
            if (atSide != LayoutBreakSide_Any)
                *side = atSide;
        }
    }
    return ask;
}

// Returns the used page value of `node`, whose parent's is `page`: its own if it is not auto, and `page` otherwise.
PrLayoutName ownPage(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutName page) {
    if (layoutTreeNode(tree, node).style == kPrLayoutStyleNone)
        return page;
    PrLayoutName own = layoutFragmentationStyle(tree->styles, layoutTreeNode(tree, node).style).page;
    return own != kPrLayoutNameNone ? own : page;
}

// Returns the start page value of `node`, or its end page value if `last`, whose parent's used page value is `page`:
// the used value of its first or last block-level child at the greatest depth (css-page-3/#using-named-pages). Only the
// children of a block container propagate their values, since `page` applies only to the boxes between which class A
// breaks fall.
PrLayoutName edgePage(const LayoutTree* tree, LayoutNodeIndex node, bool last, PrLayoutName page) {
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone;) {
        page = ownPage(tree, at, page);
        if (layoutTreeNode(tree, at).children != LayoutChildren_Flow)
            break;
        at = edgeChild(tree, at, last);
    }
    return page;
}

// Returns what the break values of `before` and `after`, siblings in flow, ask of the opportunity between them, as
// layoutBreakBetween does without their page values.
LayoutBreakAsk breakAsk(const LayoutTree* tree, LayoutNodeIndex before, LayoutNodeIndex after,
        LayoutFragmentainerKind kind, LayoutBreakSide* side) {
    LayoutBreakSide afterSide = LayoutBreakSide_Any;
    LayoutBreakSide beforeSide = LayoutBreakSide_Any;
    LayoutBreakAsk a = edgeAsk(tree, before, true, kind, &afterSide);
    LayoutBreakAsk b = edgeAsk(tree, after, false, kind, &beforeSide);
    *side = beforeSide != LayoutBreakSide_Any ? beforeSide : afterSide;
    return a > b ? a : b;
}

// Returns true if a forced break of `kind` may fall inside the box of `node`, whose used page value is `page`, at any
// depth.
bool forcedBreakInside(const LayoutTree* tree, LayoutNodeIndex node, LayoutFragmentainerKind kind, PrLayoutName page) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    bool flow = n.children == LayoutChildren_Flow;
    // The sibling before `c`: a block-level box, an anonymous block box of inline content if `anonymous`, or none.
    LayoutNodeIndex before = kPrLayoutNodeNone;
    bool anonymous = false;
    for (LayoutNodeIndex c = n.first; c != kPrLayoutNodeNone; c = layoutTreeNode(tree, c).next) {
        if (!inFlowBlock(tree, c)) {
            if (flow && layoutChildKind(tree, c) == LayoutChildKind_Inline && !collapsesAway(tree, c, n)) {
                LayoutBreakSide side = LayoutBreakSide_Any;
                if (before != kPrLayoutNodeNone &&
                        layoutBreakBetween(tree, before, kPrLayoutNodeNone, kind, page, &side) == LayoutBreakAsk_Force)
                    return true;
                before = kPrLayoutNodeNone;
                anonymous = true;
            }
            continue;
        }
        LayoutBreakSide side = LayoutBreakSide_Any;
        LayoutBreakAsk ask = LayoutBreakAsk_Auto;
        if ((before != kPrLayoutNodeNone || anonymous) && flow)
            ask = layoutBreakBetween(tree, before, c, kind, page, &side);
        else if (before != kPrLayoutNodeNone)
            ask = breakAsk(tree, before, c, kind, &side);
        if (ask == LayoutBreakAsk_Force)
            return true;
        if (forcedBreakInside(tree, c, kind, ownPage(tree, c, page)))
            return true;
        before = c;
        anonymous = false;
    }
    return false;
}

} // namespace

LayoutBreakToken* layoutBreakTokenCreate(LayoutNodeIndex node, PrLayoutUnit consumed, LayoutNodeIndex child,
        const LayoutBreakToken* PR_NULLABLE childToken) {
    LayoutBreakToken* token = allocZeroed<LayoutBreakToken>();
    token->references.storeRelaxed(1);
    token->node = node;
    token->consumed = consumed;
    token->child = child;
    token->childToken = childToken;
    if (childToken != nullptr) {
        token->forced = childToken->forced;
        token->side = childToken->side;
        token->hasPage = childToken->hasPage;
        token->page = childToken->page;
    }
    return token;
}

void layoutBreakTokenRetain(const LayoutBreakToken* token) {
    token->references.fetchAddRelaxed(1);
}

void layoutBreakTokenRelease(const LayoutBreakToken* PR_NULLABLE token) {
    while (token != nullptr && token->references.fetchSub(1) == 1) {
        const LayoutBreakToken* child = token->childToken;
        layoutBreakTokenRelease(token->sibling);
        layoutBreakTokenRelease(token->floats);
        prMemoryFree(const_cast<LayoutBreakToken*>(token));
        token = child;
    }
}

LayoutBreakAsk layoutBreakBetween(const LayoutTree* tree, LayoutNodeIndex before, LayoutNodeIndex after,
        LayoutFragmentainerKind kind, PrLayoutName page, LayoutBreakSide* side) {
    LayoutBreakAsk ask = breakAsk(tree, before, after, kind, side);
    if (kind == LayoutFragmentainerKind_Page && edgePage(tree, before, true, page) != edgePage(tree, after, false, page))
        ask = LayoutBreakAsk_Force;
    return ask;
}

PrLayoutName layoutUsedPage(const LayoutTree* tree, LayoutNodeIndex node) {
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
        PrLayoutName own = ownPage(tree, at, kPrLayoutNameNone);
        if (own != kPrLayoutNameNone)
            return own;
    }
    return kPrLayoutNameNone;
}

PrLayoutName layoutStartPage(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutName page) {
    return edgePage(tree, node, false, page);
}

PrLayoutPageContext layoutStartPageContext(const LayoutTree* tree, LayoutNodeIndex root) {
    bool ltr = root == kPrLayoutNodeNone || layoutOwnMode(tree, root).direction == PrTextDirection_Ltr;
    return PrLayoutPageContext{
        .name = layoutStartPage(tree, root, kPrLayoutNameNone),
        .side = PrLayoutPageSide(ltr ? PrLayoutPageSide_Right : PrLayoutPageSide_Left),
        .first = true,
    };
}

bool layoutRunGeneratesBox(const LayoutTree* tree, LayoutNodeIndex parent, LayoutNodeIndex first, LayoutNodeIndex last) {
    const LayoutTreeNode& p = layoutTreeNode(tree, parent);
    LayoutNodeIndex end = layoutTreeNode(tree, last).next;
    for (LayoutNodeIndex c = first; c != end; c = layoutTreeNode(tree, c).next) {
        if (layoutChildKind(tree, c) == LayoutChildKind_Inline && !collapsesAway(tree, c, p))
            return true;
    }
    return false;
}

bool layoutAvoidsBreakInside(const LayoutTree* tree, LayoutNodeIndex node, LayoutFragmentainerKind kind) {
    PrLayoutBreakInside inside = layoutFragmentationStyle(tree->styles, layoutTreeNode(tree, node).style).breakInside;
    bool page = kind == LayoutFragmentainerKind_Page;
    return inside == PrLayoutBreakInside_Avoid || (inside == PrLayoutBreakInside_AvoidPage && page) ||
        (inside == PrLayoutBreakInside_AvoidColumn && !page);
}

bool layoutIsMonolithic(const LayoutTree* tree, LayoutNodeIndex node, LayoutMode mode) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Replaced || n.kind == PrLayoutNodeKind_SemiReplaced)
        return true;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    if (layoutIsScrollContainer(box) || box.containBlock)
        return true;
    LayoutMode own = layoutOwnMode(tree, node);
    return layoutIsOrthogonal(own, mode) || !layoutSameBlockFlow(own, mode);
}

bool layoutHasForcedBreakInside(const LayoutTree* tree, LayoutNodeIndex node, LayoutFragmentainerKind kind) {
    if (tree->styles->forcedBreakGroups == 0)
        return false;
    return forcedBreakInside(tree, node, kind, layoutUsedPage(tree, node));
}

} // namespace pr_impl
