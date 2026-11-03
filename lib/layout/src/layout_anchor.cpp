// Scroll anchoring: keeping what a scrolled container shows in place as its content's layout changes.

#include "layout/src/layout_anchor.h"

#include "pr/layout_query.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_query.h"
#include "layout/src/layout_tree.h"
#include "layout/src/layout_walk.h"

namespace pr_impl {

namespace {

// Returns the anchor entry of `container`, or null if the container is not scrolled.
LayoutAnchor* anchorOf(LayoutAnchoring& anchoring, PrLayoutNode container) {
    for (uint32_t i = 0; i < anchoring.anchors.count; ++i) {
        if (anchoring.anchors.data[i].container == container)
            return &anchoring.anchors.data[i];
    }
    return nullptr;
}

// Returns true if `node` names a live node of the tree.
bool isLive(const LayoutTree* tree, PrLayoutNode node) {
    LayoutNodeIndex index = LayoutNodeIndex(node);
    return index != kPrLayoutNodeNone && index < tree->nodes.count && tree->nodes.data[index].live &&
        tree->nodes.data[index].generation == uint32_t(node >> 32);
}

// The axis and the edge that a container's block-start edge is at.
enum {
    BlockStart_Top,
    BlockStart_Left,
    BlockStart_Right,
};
typedef uint8_t BlockStart;

// Returns where the block-start edge of the container's content is: the viewport's follows the root's writing mode.
BlockStart blockStartOf(const LayoutTree* tree, LayoutNodeIndex container) {
    LayoutNodeIndex node = container != kPrLayoutNodeNone ? container : tree->root;
    if (node == kPrLayoutNodeNone)
        return BlockStart_Top;
    switch (layoutFontStyle(tree->styles, layoutTreeNode(tree, node).style).writingMode) {
        case PrLayoutWritingMode_VerticalRl:
        case PrLayoutWritingMode_SidewaysRl:
            return BlockStart_Right;
        case PrLayoutWritingMode_VerticalLr:
        case PrLayoutWritingMode_SidewaysLr:
            return BlockStart_Left;
        default:
            return BlockStart_Top;
    }
}

PrLayoutUnit edgeOf(PrLayoutRect rect, BlockStart start) {
    switch (start) {
        case BlockStart_Left:
            return rect.left;
        case BlockStart_Right:
            return rect.right;
        default:
            return rect.top;
    }
}

// What a choice of an anchor reads of its container: what it shows, in the laid-out content of the viewport before
// transforms and scrolling, and where its unscrolled content starts.
struct Container {
    LayoutNodeIndex node;           // kPrLayoutNodeNone for the viewport
    PrLayoutRect shown;             // the scrollport, at its offset (css-scroll-anchoring-1/#anchor-node-selection)

    // The scroll origin: the corner of the padding box at the block start, from which positions in the content are
    // measured, so that a container that grows at its start does not move them (css-overflow-3/#scroll-origin).
    PrLayoutVec origin;

    PrLayoutVec offset;             // its offset, within its range
    BlockStart start;
};

// Measures `container` as of the last layout. Returns false if it is not a laid-out scroll container.
bool measure(LayoutTree* tree, PrLayoutNode container, Container* out) {
    PrLayoutScrollInfo info{};
    if (!prLayoutNodeScrollInfo(static_cast<PrLayoutTree*>(tree), container, &info) || !info.scrollContainer)
        return false;
    LayoutNodeIndex index = layoutNodeIndex(tree, container);
    PrLayoutVec port{};
    if (index != kPrLayoutNodeNone) {
        if (!layoutNodeLaidOut(tree, index))
            return false;
        PrLayoutTree* api = static_cast<PrLayoutTree*>(tree);
        PrLayoutRect border = prLayoutNodeBounds(api, container, PrLayoutCoordinates_Layout);
        const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, layoutTreeNode(tree, index).style);
        port = PrLayoutVec{border.left + box.border[PrRectSide_Left], border.top + box.border[PrRectSide_Top]};
    }
    out->node = index;
    out->start = blockStartOf(tree, index);
    out->offset = info.offset;
    PrLayoutVec at = port + info.offset;
    out->shown = PrLayoutRect{at.x, at.y, at.x + info.portExtent.x, at.y + info.portExtent.y};
    out->origin = port;
    if (out->start == BlockStart_Right)
        out->origin.x = port.x + info.portExtent.x;
    return true;
}

// Returns the offset's component along the container's block axis.
PrLayoutUnit blockOffset(const Container& c) {
    return c.start == BlockStart_Top ? c.offset.y : c.offset.x;
}

// How a node takes part in a container's choice of an anchor.
enum {
    Candidate_Skipped,              // it and its subtree are not examined (css-scroll-anchoring-1/#excluded-subtree)
    Candidate_Selectable,           // it may be the anchor
    Candidate_Through,              // it is not an anchor, but its children are examined, as an inline box's are
};
typedef uint8_t Candidate;

// Returns true if `node` is a descendant of `ancestor`, or `ancestor` is kPrLayoutNodeNone for the viewport.
bool isWithin(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex ancestor) {
    if (ancestor == kPrLayoutNodeNone)
        return true;
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
        if (at == ancestor)
            return true;
    }
    return false;
}

Candidate candidateOf(const LayoutTree* tree, LayoutNodeIndex node, const Container& container) {
    if (!layoutNodeLaidOut(tree, node))
        return Candidate_Skipped;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_LineBreak)
        return Candidate_Skipped;
    if (n.kind == PrLayoutNodeKind_Text)
        return Candidate_Selectable;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    if (box.overflowAnchor == PrLayoutOverflowAnchor_None || box.position == PrLayoutPosition_Fixed ||
            box.position == PrLayoutPosition_Sticky)
        return Candidate_Skipped;
    // A box out of flow whose containing block is outside the container does not move with its content.
    if (box.position == PrLayoutPosition_Absolute && layoutIsOutOfFlow(tree, node)) {
        LayoutNodeIndex containing = kPrLayoutNodeNone;
        layoutNodeHolder(tree, node, &containing);
        bool outside;
        if (containing == kPrLayoutNodeNone)
            outside = container.node != kPrLayoutNodeNone;
        else
            outside = !isWithin(tree, containing, container.node);
        if (outside)
            return Candidate_Skipped;
    }
    if (layoutIsInlineBox(tree, node) || layoutCachePlaced(tree->cache, node) == nullptr)
        return Candidate_Through;
    return Candidate_Selectable;
}

// Returns the bounding rect of `node` for anchoring (css-scroll-anchoring-1/#scroll-anchoring-bounding-rect), in the
// laid-out content of the viewport: its scrollable overflow rectangle, cut at the box's own start edges, as scrollable
// overflow is cut at a scroll container's, or the border box of a box that is itself a scroll container, which clips
// its content. Sets `*outScrolls` if the box is a scroll container that is scrolled. `start` is where the container's
// block-start edge is.
PrLayoutRect boundsOf(LayoutTree* tree, LayoutNodeIndex node, BlockStart start, bool* outScrolls) {
    *outScrolls = false;
    PrLayoutNode name = layoutNodeName(tree, node);
    PrLayoutRect bounds = prLayoutNodeBounds(static_cast<PrLayoutTree*>(tree), name, PrLayoutCoordinates_Layout);
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || layoutIsInlineBox(tree, node))
        return bounds;
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
    if (fragment == nullptr)
        return bounds;
    // A scroll container clips its content to its border box. One that is scrolled places its content at its offset,
    // which moves without a layout, so its content is no anchor for the containers around it.
    if (layoutFragmentScrolls(tree, fragment)) {
        *outScrolls = layoutFragmentScroll(tree, fragment) != kLayoutVecZero;
        return bounds;
    }
    if (fragment->kind != LayoutFragmentKind_Box)
        return bounds;
    PrLayoutRect overflow = fragment->overflow;
    PrLayoutRect rect{
        bounds.left + overflow.left, bounds.top + overflow.top, bounds.left + overflow.right,
        bounds.top + overflow.bottom,
    };
    // The start edges are the block axis's and the inline axis's, which is the top in a vertical writing mode.
    if (start == BlockStart_Right)
        rect.right = layoutUnitMin(rect.right, bounds.right);
    else
        rect.left = layoutUnitMax(rect.left, bounds.left);
    rect.top = layoutUnitMax(rect.top, bounds.top);
    return rect;
}

bool fullyVisible(PrLayoutRect rect, PrLayoutRect shown) {
    return rect.left >= shown.left && rect.top >= shown.top && rect.right <= shown.right && rect.bottom <= shown.bottom;
}

bool fullyClipped(PrLayoutRect rect, PrLayoutRect shown) {
    return rect.right <= shown.left || rect.left >= shown.right || rect.bottom <= shown.top || rect.top >= shown.bottom;
}

// Chooses the anchor among `first` and its following siblings for `container`, by the candidate examination algorithm
// (css-scroll-anchoring-1/#candidate-examination). Returns kPrLayoutNodeNone if none is chosen. The examination
// descends with a stack of its own, since documents nest deeply. Sets `*outDirty` if a node that it examined holds a
// change that the last layout did not lay out, as content that replaced the content around the anchor does.
LayoutNodeIndex examine(LayoutTree* tree, LayoutNodeIndex first, const Container& container, bool* outDirty) {
    // The candidates being examined, each partially visible: its node, and its next sibling, where the examination
    // resumes once its children hold no anchor.
    struct Frame {
        LayoutNodeIndex node;
        LayoutNodeIndex resume;
        bool selectable;
    };
    Array<Frame> stack;
    LayoutNodeIndex chosen = kPrLayoutNodeNone;
    LayoutNodeIndex next = first;
    while (chosen == kPrLayoutNodeNone) {
        if (next == kPrLayoutNodeNone) {
            // The children of the innermost partially visible candidate held no anchor, so it is the anchor itself.
            if (stack.count == 0)
                break;
            Frame done = stack.last();
            stack.pop();
            if (done.selectable) {
                chosen = done.node;
                break;
            }
            next = done.resume;
            continue;
        }
        LayoutNodeIndex node = next;
        next = layoutTreeNode(tree, node).next;
        *outDirty = *outDirty || layoutTreeNode(tree, node).dirty != 0;
        Candidate candidate = candidateOf(tree, node, container);
        if (candidate == Candidate_Skipped)
            continue;
        bool scrolls = false;
        PrLayoutRect rect = boundsOf(tree, node, container.start, &scrolls);
        bool selectable = candidate == Candidate_Selectable;
        // A box or text that occupies no space is no anchor: nothing the reader sees stays put by keeping it in place.
        // Its rect says nothing of where its children are either: a root whose body is out of flow is empty, and holds
        // the whole document.
        bool empty = rect.right <= rect.left || rect.bottom <= rect.top;
        if (empty) {
            if (layoutTreeNode(tree, node).first == kPrLayoutNodeNone)
                continue;
            selectable = false;
        } else if (fullyClipped(rect, container.shown)) {
            continue;
        }
        if (selectable && (fullyVisible(rect, container.shown) || scrolls)) {
            chosen = node;
            break;
        }
        // A partially visible candidate's descendants are examined before it. A scrolled scroll container is the anchor
        // rather than anything inside it, whose place on screen its own scrolling moves.
        LayoutNodeIndex child = layoutTreeNode(tree, node).first;
        if (child == kPrLayoutNodeNone) {
            if (selectable) {
                chosen = node;
                break;
            }
            continue;
        }
        stack.push(Frame{node, next, selectable});
        next = child;
    }
    stack.reset();
    return chosen;
}

// Returns the position of the block-start edge of the anchor's bounding rect in the container's unscrolled content.
PrLayoutUnit anchorEdge(LayoutTree* tree, LayoutNodeIndex anchor, const Container& container) {
    bool scrolls = false;
    PrLayoutRect rect = boundsOf(tree, anchor, container.start, &scrolls);
    PrLayoutUnit origin = container.start == BlockStart_Top ? container.origin.y : container.origin.x;
    return edgeOf(rect, container.start) - origin;
}

// Returns true if a change since the last layout suppresses the adjustment of `container` for `anchor`: a change of a
// value that moves boxes on the path from the anchor to the container, or a box inside the container that became or
// stopped being absolutely positioned (css-scroll-anchoring-1/#suppression-triggers).
bool suppressed(const LayoutTree* tree, LayoutNodeIndex anchor, LayoutNodeIndex container) {
    for (LayoutNodeIndex at = anchor; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
        if (layoutTreeNode(tree, at).anchorSuppressed)
            return true;
        if (at == container)
            break;
    }
    const Array<LayoutNodeIndex>& repositioned = tree->anchoring.repositioned;
    for (uint32_t i = 0; i < repositioned.count; ++i) {
        LayoutNodeIndex node = repositioned.data[i];
        if (node < tree->nodes.count && tree->nodes.data[node].live && isWithin(tree, node, container))
            return true;
    }
    return false;
}

// Adds `delta` to the move of `container` in `moves`.
void addMove(Array<PrLayoutScrollAdjustment>& moves, PrLayoutNode container, PrLayoutVec delta) {
    for (uint32_t i = 0; i < moves.count; ++i) {
        if (moves.data[i].node == container) {
            moves.data[i].delta = moves.data[i].delta + delta;
            return;
        }
    }
    moves.push(PrLayoutScrollAdjustment{container, delta});
}

void clearMarks(LayoutTree* tree) {
    LayoutAnchoring& anchoring = tree->anchoring;
    for (uint32_t i = 0; i < anchoring.suppressing.count; ++i) {
        LayoutNodeIndex node = anchoring.suppressing.data[i];
        if (node < tree->nodes.count)
            tree->nodes.data[node].anchorSuppressed = false;
    }
    anchoring.suppressing.clear();
    anchoring.repositioned.clear();
}

// Chooses the anchor of `anchor`'s container from the last layout, or none. An anchor chosen among content that the
// last layout did not lay out is provisional, and is chosen again once the layout that follows has laid it out.
void choose(LayoutTree* tree, LayoutAnchor& anchor) {
    anchor.node = kPrLayoutNodeNone;
    anchor.provisional = false;
    if (anchor.container != kPrLayoutNodeNone && !isLive(tree, anchor.container))
        return;
    Container container{};
    if (!measure(tree, anchor.container, &container) || blockOffset(container) == kLayoutUnitZero)
        return;
    // A container that opts out has no anchor (css-scroll-anchoring-1/#anchoring-algorithm). The viewport's children
    // are the root, and an element's are its children.
    LayoutNodeIndex first = kPrLayoutNodeNone;
    if (container.node == kPrLayoutNodeNone) {
        first = tree->root;
    } else {
        const LayoutTreeNode& n = layoutTreeNode(tree, container.node);
        if (layoutBoxStyle(tree->styles, n.style).overflowAnchor == PrLayoutOverflowAnchor_None)
            return;
        first = n.first;
    }
    bool dirty = false;
    LayoutNodeIndex chosen = first != kPrLayoutNodeNone ? examine(tree, first, container, &dirty) : kPrLayoutNodeNone;
    if (chosen == kPrLayoutNodeNone)
        return;
    anchor.node = layoutNodeName(tree, chosen);
    anchor.edge = anchorEdge(tree, chosen, container);
    anchor.offset = container.offset;
    anchor.provisional = dirty;
}

} // namespace

void layoutAnchoringReset(LayoutAnchoring& anchoring) {
    anchoring.anchors.reset();
    anchoring.untaken.reset();
    anchoring.unscened.reset();
    anchoring.suppressing.reset();
    anchoring.repositioned.reset();
}

void layoutAnchoringScrolled(LayoutTree* tree, LayoutNodeIndex container, PrLayoutVec offset) {
    LayoutAnchoring& anchoring = tree->anchoring;
    PrLayoutNode name = layoutNodeName(tree, container);
    LayoutAnchor* anchor = anchorOf(anchoring, name);
    if (offset == kLayoutVecZero) {
        if (anchor != nullptr)
            anchoring.anchors.erase(uint32_t(anchor - anchoring.anchors.data));
        return;
    }
    if (anchor == nullptr) {
        anchoring.anchors.push(LayoutAnchor{name, kPrLayoutNodeNone, kLayoutUnitZero, kLayoutVecZero, false});
        return;
    }
    anchor->node = kPrLayoutNodeNone;
}

void layoutAnchoringStyleChanged(LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex was, PrLayoutStyleIndex style) {
    if (tree->anchoring.anchors.count == 0)
        return;
    const PrLayoutStyle& before = layoutStyle(tree->styles, was);
    const PrLayoutStyle& after = layoutStyle(tree->styles, style);
    bool boxChanged = before.groups[PrLayoutGroup_Box] != after.groups[PrLayoutGroup_Box];
    bool effectChanged = before.groups[PrLayoutGroup_Effect] != after.groups[PrLayoutGroup_Effect];
    if (!boxChanged && !effectChanged)
        return;
    bool suppresses = false;
    if (boxChanged) {
        const PrLayoutBoxStyle& a = layoutBoxStyle(tree->styles, was);
        const PrLayoutBoxStyle& b = layoutBoxStyle(tree->styles, style);
        auto same = [](PrLayoutLength x, PrLayoutLength y) { return x.bits == y.bits; };
        suppresses = !same(a.width, b.width) || !same(a.height, b.height) || !same(a.minWidth, b.minWidth) ||
            !same(a.minHeight, b.minHeight) || !same(a.maxWidth, b.maxWidth) || !same(a.maxHeight, b.maxHeight) ||
            a.position != b.position || a.transformed != b.transformed;
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            suppresses = suppresses || !same(a.margin[side], b.margin[side]) || !same(a.padding[side],
                b.padding[side]) || !same(a.inset[side], b.inset[side]);
        }
        bool absoluteBefore = layoutPositionIsOutOfFlow(a.position);
        bool absoluteAfter = layoutPositionIsOutOfFlow(b.position);
        if (absoluteBefore != absoluteAfter)
            tree->anchoring.repositioned.push(node);
    }
    if (!suppresses && effectChanged) {
        const PrLayoutEffectStyle& a = layoutEffectStyle(tree->styles, was);
        const PrLayoutEffectStyle& b = layoutEffectStyle(tree->styles, style);
        suppresses = !prMemEquals(&a.transform, &b.transform, sizeof(PrLayoutTransform));
    }
    LayoutTreeNode& n = tree->nodes.data[node];
    if (suppresses && !n.anchorSuppressed) {
        n.anchorSuppressed = true;
        tree->anchoring.suppressing.push(node);
    }
}

void layoutAnchoringChoose(LayoutTree* tree) {
    LayoutAnchoring& anchoring = tree->anchoring;
    if (tree->fragment == nullptr)
        return;
    for (uint32_t i = 0; i < anchoring.anchors.count; ++i) {
        LayoutAnchor& anchor = anchoring.anchors.data[i];
        if (anchor.node != kPrLayoutNodeNone && isLive(tree, anchor.node) &&
                layoutNodeLaidOut(tree, layoutNodeIndex(tree, anchor.node)))
            continue;
        choose(tree, anchor);
    }
}

void layoutAnchoringAdjust(LayoutTree* tree, bool resized) {
    LayoutAnchoring& anchoring = tree->anchoring;
    for (uint32_t i = 0; i < anchoring.anchors.count; ++i) {
        LayoutAnchor& anchor = anchoring.anchors.data[i];
        if (anchor.node == kPrLayoutNodeNone)
            continue;
        LayoutNodeIndex node = isLive(tree, anchor.node) ? layoutNodeIndex(tree, anchor.node) : kPrLayoutNodeNone;
        Container container{};
        bool measured = node != kPrLayoutNodeNone && layoutNodeLaidOut(tree, node) &&
            (anchor.container == kPrLayoutNodeNone || isLive(tree, anchor.container)) &&
            measure(tree, anchor.container, &container);
        // An anchor that is no longer laid out, or whose move a change suppresses, is dropped without an adjustment,
        // and so is an anchor of a container at its origin. The suppression triggers are for pages that move content as
        // they scroll. A new viewport extent changes the values that trigger them on any page that adapts to its width,
        // as its media queries and its relative lengths resolve anew, and keeping the reader's place is then what
        // anchoring is for, so a layout in a new extent suppresses nothing.
        container.offset = anchor.offset;
        bool suppresses = !resized && suppressed(tree, node, container.node);
        if (!measured || blockOffset(container) == kLayoutUnitZero || suppresses) {
            anchor.node = kPrLayoutNodeNone;
            continue;
        }
        // An anchor that a change excluded, as by overflow-anchor: none on it or on a box around it, is dropped too.
        bool excluded = container.node != kPrLayoutNodeNone &&
            layoutBoxStyle(tree->styles, layoutTreeNode(tree, container.node).style).overflowAnchor ==
            PrLayoutOverflowAnchor_None;
        for (LayoutNodeIndex at = node; at != container.node && at != kPrLayoutNodeNone && !excluded;
                at = layoutTreeNode(tree, at).parent)
            excluded = candidateOf(tree, at, container) == Candidate_Skipped;
        if (excluded) {
            anchor.node = kPrLayoutNodeNone;
            continue;
        }
        PrLayoutUnit edge = anchorEdge(tree, node, container);
        PrLayoutUnit moved = edge - anchor.edge;
        anchor.edge = edge;
        PrLayoutScrollInfo info{};
        prLayoutNodeScrollInfo(static_cast<PrLayoutTree*>(tree), anchor.container, &info);
        if (moved == kLayoutUnitZero) {
            anchor.offset = info.offset;
            continue;
        }
        PrLayoutVec base = anchor.offset;
        PrLayoutVec wanted = base;
        if (container.start == BlockStart_Top)
            wanted.y = wanted.y + moved;
        else
            wanted.x = wanted.x + moved;
        PrLayoutVec to = layoutScrollClamp(wanted, info.range);
        anchor.offset = to;
        if (container.node == kPrLayoutNodeNone)
            tree->viewportScroll = to;
        else
            tree->nodes.data[container.node].scroll = to;
        PrLayoutVec delta = to - base;
        if (delta == kLayoutVecZero)
            continue;
        addMove(anchoring.untaken, anchor.container, delta);
        addMove(anchoring.unscened, anchor.container, delta);
    }
    // A provisional anchor is chosen again from this layout, among the content that it laid out.
    for (uint32_t i = 0; i < anchoring.anchors.count; ++i) {
        LayoutAnchor& anchor = anchoring.anchors.data[i];
        if (anchor.provisional)
            choose(tree, anchor);
    }
    clearMarks(tree);
}

void layoutAnchoringIdle(LayoutTree* tree) {
    clearMarks(tree);
}

} // namespace pr_impl
