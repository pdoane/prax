// Boxes: what the style of a node makes of it. This covers the kind of box it generates, how it takes part in the
// layout of its parent, and what it does to painting.

#include "layout/src/layout_box.h"

namespace pr_impl {

namespace {

// Adds to `bounds` the border boxes of the in-flow content of `fragment`, placed at `at`. Descends through anonymous
// boxes, rows and lines to the boxes they hold. Boxes out of flow and placeholders are not in flow
// (css-overflow-3/#scrollable).
void addInFlowBounds(const LayoutFragment* fragment, PrLayoutVec at, PrLayoutRect& bounds, bool& any) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        const LayoutFragment* f = child.fragment;
        if ((f->flags & LayoutFragmentFlag_OutOfFlow) != 0 || !layoutFragmentReachesOverflow(f))
            continue;
        PrLayoutVec offset = at + child.offset;
        bool through = f->kind == LayoutFragmentKind_Line || f->kind == LayoutFragmentKind_Around ||
            (f->kind == LayoutFragmentKind_Box && f->node == kPrLayoutNodeNone);
        if (through) {
            addInFlowBounds(f, offset, bounds, any);
            continue;
        }
        PrLayoutRect box = layoutRectAt(offset, f->extent);
        bounds = any ? layoutRectUnion(bounds, box) : box;
        any = true;
    }
}

// Returns `area` cut at the sides of `port` where the scroll origin of `fragment`, laid out in `mode`, lies. Nothing
// scrolls past those sides. They are the block-start and inline-start sides, or for a flex container its main-start and
// cross-start sides (css-overflow-3/#scroll-origin), which a reverse flex-direction or flex-wrap moves to the end.
PrLayoutRect cutAtOrigin(const LayoutTree* tree, const LayoutFragment* fragment, const PrLayoutBoxStyle& box,
        PrLayoutRect area, PrLayoutRect port, LayoutMode mode) {
    LayoutSide block = LayoutSide_BlockStart;
    LayoutSide inlineSide = LayoutSide_InlineStart;
    if (layoutIsFlexContainer(box) && fragment->node != kPrLayoutNodeNone) {
        const PrLayoutFlexStyle& flex = layoutFlexStyle(tree->styles, layoutTreeNode(tree, fragment->node).style);
        bool row = flex.direction == PrLayoutFlexDirection_Row || flex.direction == PrLayoutFlexDirection_RowReverse;
        bool mainReversed = flex.direction == PrLayoutFlexDirection_RowReverse ||
            flex.direction == PrLayoutFlexDirection_ColumnReverse;
        bool crossReversed = flex.wrap == PrLayoutFlexWrap_WrapReverse;
        bool blockReversed = row ? crossReversed : mainReversed;
        bool inlineReversed = row ? mainReversed : crossReversed;
        block = blockReversed ? LayoutSide_BlockEnd : LayoutSide_BlockStart;
        inlineSide = inlineReversed ? LayoutSide_InlineEnd : LayoutSide_InlineStart;
    }
    const PrRectSide origin[] = {layoutPhysicalSide(mode, block), layoutPhysicalSide(mode, inlineSide)};
    for (PrRectSide side : origin) {
        switch (side) {
            case PrRectSide_Left: area.left = port.left; break;
            case PrRectSide_Top: area.top = port.top; break;
            case PrRectSide_Right: area.right = port.right; break;
            default: area.bottom = port.bottom; break;
        }
    }
    return area;
}

// Returns the padding box of `fragment`, styled by `box`, from its top left.
PrLayoutRect paddingBox(const LayoutFragment* fragment, const PrLayoutBoxStyle& box) {
    return PrLayoutRect{
        box.border[PrRectSide_Left], box.border[PrRectSide_Top],
        fragment->extent.x - box.border[PrRectSide_Right],
        fragment->extent.y - box.border[PrRectSide_Bottom],
    };
}

// Returns `area` extended by what each child of `fragment` adds to its overflow, from its top left. The border box of
// the fragment itself is not included, because the scrollable overflow of a box starts from its padding box.
PrLayoutRect childrenReach(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutRect area) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        if (layoutFragmentReachesOverflow(child.fragment))
            area = layoutRectUnion(area, layoutRectMoved(layoutPassedOverflow(tree, child.fragment), child.offset));
    }
    return area;
}

// Returns the container of a node that the viewport holds: outside the content of the viewport for a fixed box, and
// otherwise that content. A paginated tree's fixed box is fixed to its page (pr/layout_page.h), which the viewport
// scrolls with its content.
LayoutNodeIndex topContainer(const LayoutTree* tree, LayoutNodeIndex node) {
    bool paged = tree->pagination.box != nullptr;
    if (layoutTreeNode(tree, node).parent != kPrLayoutNodeNone && layoutIsFixed(tree, node) && !paged)
        return kLayoutContainerOutside;
    return kLayoutContainerViewport;
}

// Returns the innermost sticky inline box that moves `node` with it, or kPrLayoutNodeNone. Placements pass over inline
// boxes, so these are found in the tree: the inline boxes between a node in flow and its block container, or the
// containing block of a box out of flow if that is an inline box.
LayoutNodeIndex stickyInlineAround(const LayoutTree* tree, LayoutNodeIndex node) {
    if (tree->styles->stickyGroups == 0)
        return kPrLayoutNodeNone;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind != PrLayoutNodeKind_Text && layoutIsOutOfFlow(tree, node)) {
        LayoutNodeIndex containing = kPrLayoutNodeNone;
        layoutNodeHolder(tree, node, &containing);
        bool moves = containing != kPrLayoutNodeNone && layoutIsInlineBox(tree, containing) &&
            layoutNodeSticks(tree, containing);
        return moves ? containing : kPrLayoutNodeNone;
    }
    for (LayoutNodeIndex at = n.parent; at != kPrLayoutNodeNone && layoutIsInlineBox(tree, at);
            at = layoutTreeNode(tree, at).parent) {
        if (layoutNodeSticks(tree, at))
            return at;
    }
    return kPrLayoutNodeNone;
}

// Returns the container that `node`, placed in the box `parent`, paints in if it does not stick itself, given the
// container `up` that `parent` paints in.
LayoutNodeIndex containerAround(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex parent, LayoutNodeIndex up) {
    LayoutNodeIndex inlineBox = stickyInlineAround(tree, node);
    if (inlineBox != kPrLayoutNodeNone)
        return inlineBox | kLayoutContainerSticky;
    if (parent == kPrLayoutNodeNone)
        return topContainer(tree, node);
    return layoutNodeClips(tree, parent) ? parent : up;
}

// Returns the container of the box `node`, as its memo holds it, found again from its placements without the memos.
LayoutNodeIndex freshContainer(const LayoutTree* tree, LayoutNodeIndex node) {
    if (layoutNodeSticks(tree, node))
        return node | kLayoutContainerSticky;
    LayoutNodeIndex parent = layoutCachePlacement(tree->cache, node).parent;
    LayoutNodeIndex up = parent != kPrLayoutNodeNone ? freshContainer(tree, parent) : kLayoutContainerViewport;
    return containerAround(tree, node, parent, up);
}

// Returns the memo of the position and container of the box `node`. Recomputes the memos of the box and of its
// ancestors along its placements if they are stale. A read costs the number of boxes up to the nearest one with a
// current memo.
const LayoutPositioned& positioned(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutCache& cache = tree->cache;
    auto current = [&](LayoutNodeIndex at) {
        return at < cache.positioned.count && cache.positioned.data[at].version == cache.placed;
    };
    if (current(node))
        return cache.positioned.data[node];
    if (cache.positioned.count < tree->nodes.count) {
        uint32_t from = cache.positioned.count;
        cache.positioned.resize(tree->nodes.count);
        for (uint32_t i = from; i < cache.positioned.count; ++i)
            cache.positioned.data[i] = LayoutPositioned{};
    }
    RawArray<LayoutNodeIndex>& path = cache.unpositioned;
    path.clear();
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone && !current(at); at =
            layoutCachePlacement(cache, at).parent)
        path.push(at);
    // A tree with no sticky box finds each container from its placement alone.
    bool sticks = tree->styles->stickyGroups != 0;
    for (uint32_t i = path.count; i-- > 0;) {
        LayoutNodeIndex at = path.data[i];
        LayoutPlacement placement = layoutCachePlacement(cache, at);
        LayoutPositioned memo{.position = placement.offset, .version = cache.placed};
        LayoutNodeIndex up = kLayoutContainerViewport;
        if (placement.parent != kPrLayoutNodeNone) {
            const LayoutPositioned& above = cache.positioned.data[placement.parent];
            memo.position = above.position + placement.offset;
            up = above.container;
        }
        if (!sticks && placement.parent == kPrLayoutNodeNone)
            memo.container = topContainer(tree, at);
        else if (!sticks)
            memo.container = layoutNodeClips(tree, placement.parent) ? placement.parent : up;
        else if (layoutNodeSticks(tree, at))
            memo.container = at | kLayoutContainerSticky;
        else
            memo.container = containerAround(tree, at, placement.parent, up);
        cache.positioned.data[at] = memo;
    }
    return cache.positioned.data[node];
}

} // namespace

PrLayoutRect layoutPassedOverflow(const LayoutTree* tree, const LayoutFragment* fragment) {
    PrLayoutRect own = layoutRectAt(kLayoutVecZero, fragment->extent);
    switch (fragment->kind) {
        case LayoutFragmentKind_Line:
        case LayoutFragmentKind_Around: return fragment->overflow;
        case LayoutFragmentKind_Box: break;
        default: return own;
    }
    if (fragment->node == kPrLayoutNodeNone)
        return fragment->overflow;
    LayoutClips clips = layoutClips(layoutBoxStyle(tree->styles, layoutTreeNode(tree, fragment->node).style));
    PrLayoutRect passed = fragment->overflow;
    if (clips.x) {
        passed.left = own.left;
        passed.right = own.right;
    }
    if (clips.y) {
        passed.top = own.top;
        passed.bottom = own.bottom;
    }
    return passed;
}

void layoutFragmentSetOverflow(const LayoutTree* tree, LayoutFragment* fragment) {
    PrLayoutRect overflow = layoutRectAt(kLayoutVecZero, fragment->extent);
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        // A placeholder or an anchor reaches nothing. Its box is a child of another fragment, and reaches as far as it
        // does there. White space that hangs reaches nothing either.
        if (!layoutFragmentReachesOverflow(child.fragment))
            continue;
        overflow = layoutRectUnion(overflow, layoutRectMoved(layoutPassedOverflow(tree, child.fragment), child.offset));
    }
    fragment->overflow = overflow;
}

void layoutScrollerSetOverflow(const LayoutTree* tree, LayoutFragment* fragment, const PrLayoutBoxStyle& box,
        const PrLayoutUnit* padding, LayoutMode mode, PrLayoutRect flow) {
    PrLayoutRect port = paddingBox(fragment, box);
    PrLayoutRect bounds = flow;
    bool any = true;
    addInFlowBounds(fragment, kLayoutVecZero, bounds, any);
    bounds = PrLayoutRect{
        bounds.left - padding[PrRectSide_Left], bounds.top - padding[PrRectSide_Top],
        bounds.right + padding[PrRectSide_Right], bounds.bottom + padding[PrRectSide_Bottom],
    };
    fragment->overflow =
        cutAtOrigin(tree, fragment, box, layoutRectUnion(childrenReach(tree, fragment, port), bounds), port, mode);
}

PrLayoutRect layoutScrollingArea(const LayoutTree* tree, const LayoutFragment* fragment, const PrLayoutBoxStyle& box,
        LayoutMode mode) {
    PrLayoutRect port = paddingBox(fragment, box);
    return layoutRectMoved(cutAtOrigin(tree, fragment, box, childrenReach(tree, fragment, port), port, mode),
        PrLayoutVec{-port.left, -port.top});
}

PrLayoutRect layoutScrollRange(PrLayoutVec portExtent, PrLayoutRect content) {
    return PrLayoutRect{
        layoutUnitMin(kLayoutUnitZero, content.left), layoutUnitMin(kLayoutUnitZero, content.top),
        layoutUnitMax(kLayoutUnitZero, content.right - portExtent.x),
        layoutUnitMax(kLayoutUnitZero, content.bottom - portExtent.y),
    };
}

PrLayoutVec layoutScrollClamp(PrLayoutVec offset, PrLayoutRect range) {
    return PrLayoutVec{
        layoutUnitMax(range.left, layoutUnitMin(offset.x, range.right)),
        layoutUnitMax(range.top, layoutUnitMin(offset.y, range.bottom)),
    };
}

PrLayoutScrollFlags layoutUserAxes(const PrLayoutBoxStyle& box) {
    PrLayoutScrollFlags axes = 0;
    if (box.overflowX == PrLayoutOverflow_Scroll)
        axes |= PrLayoutScrollFlag_X;
    if (box.overflowY == PrLayoutOverflow_Scroll)
        axes |= PrLayoutScrollFlag_Y;
    return axes;
}

PrLayoutScrollFlags layoutChainAxes(const PrLayoutBoxStyle& box) {
    auto chains = [](PrLayoutOverscroll overscroll) {
        return overscroll == PrLayoutOverscroll_Auto || overscroll == PrLayoutOverscroll_Chain;
    };
    PrLayoutScrollFlags user = layoutUserAxes(box);
    PrLayoutScrollFlags axes = 0;
    if ((user & PrLayoutScrollFlag_X) == 0 || chains(box.overscrollX))
        axes |= PrLayoutScrollFlag_X;
    if ((user & PrLayoutScrollFlag_Y) == 0 || chains(box.overscrollY))
        axes |= PrLayoutScrollFlag_Y;
    return axes;
}

bool layoutIsInColumn(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    PrLayoutDisplay parent = layoutBoxStyle(tree->styles, layoutTreeNode(tree, n.parent).style).display;
    if (parent == PrLayoutDisplay_TableColumn)
        return true;
    return parent == PrLayoutDisplay_TableColumnGroup && (!layoutKindIsElement(n.kind) ||
        layoutBoxStyle(tree->styles, n.style).display != PrLayoutDisplay_TableColumn);
}

double layoutNaturalRatio(const LayoutTree* tree, LayoutNodeIndex node) {
    const PrLayoutNaturalSize& natural = layoutReplaced(tree, node).natural;
    if (natural.width.raw > 0 && natural.height.raw > 0)
        return double(natural.width.raw) / double(natural.height.raw);
    return natural.ratio > 0 ? double(natural.ratio) : 0;
}

bool layoutIsInlineLevel(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return true;
    return prLayoutDisplayIsInlineLevel(layoutBoxStyle(tree->styles, n.style).display) && !layoutIsFloat(tree, node) &&
        !(n.positioned && n.parent != kPrLayoutNodeNone) && !layoutInItemContainer(tree, node);
}

bool layoutIsAtomicInline(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak || layoutIsFloat(tree, node) ||
            (n.positioned && n.parent != kPrLayoutNodeNone))
        return false;
    // The check for a flex or grid container parent comes last, and runs only for the few boxes that would otherwise be
    // atomic.
    PrLayoutDisplay display = layoutBoxStyle(tree->styles, n.style).display;
    bool atomic = n.kind == PrLayoutNodeKind_Replaced || display == PrLayoutDisplay_InlineBlock ||
        display == PrLayoutDisplay_InlineFlex || display == PrLayoutDisplay_InlineGrid ||
        display == PrLayoutDisplay_InlineTable || (display == PrLayoutDisplay_Inline && n.parent != kPrLayoutNodeNone &&
            layoutFontStyle(tree->styles, n.style).writingMode !=
            layoutFontStyle(tree->styles, layoutTreeNode(tree, n.parent).style).writingMode);
    return atomic && !layoutInItemContainer(tree, node);
}

bool layoutIsInlineBoxAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!layoutKindIsElement(n.kind) || n.parent == kPrLayoutNodeNone)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, style);
    if (box.display != PrLayoutDisplay_Inline || box.floating != PrLayoutFloat_None ||
            layoutStyleTraits(tree->styles, style).positioned)
        return false;
    // An inline box whose writing mode differs from the writing mode of its parent is laid out as an inline-block.
    return layoutFontStyle(tree->styles, style).writingMode ==
        layoutFontStyle(tree->styles, layoutTreeNode(tree, n.parent).style).writingMode && !layoutInItemContainer(tree, node);
}

LayoutLineSides layoutLineSides(bool left, bool right, bool start, bool end, PrTextDirection direction) {
    bool ltr = direction == PrTextDirection_Ltr;
    return LayoutLineSides((left || (start && ltr) || (end && !ltr) ? LayoutLineSide_Left : 0) |
        (right || (end && ltr) || (start && !ltr) ? LayoutLineSide_Right : 0));
}

LayoutNodeIndex layoutOutOfFlowNoRun(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex last = node;
    for (uint32_t n = layoutTreeNode(tree, node).next; n != kPrLayoutNodeNone; n = layoutTreeNode(tree, n).next) {
        LayoutChildKind kind = layoutChildKind(tree, n);
        if (kind == LayoutChildKind_OutOfFlow)
            last = n;
        else if (kind != LayoutChildKind_None)
            return kind == LayoutChildKind_Inline ? kPrLayoutNodeNone : last;
    }
    return last;
}

LayoutNodeIndex layoutRunFirst(const LayoutTree* tree, LayoutNodeIndex node) {
    LayoutNodeIndex first = layoutIsRunContent(layoutTreeNode(tree, node)) ? node : kPrLayoutNodeNone;
    for (uint32_t p = layoutTreeNode(tree, node).previous; p != kPrLayoutNodeNone; p = layoutTreeNode(tree, p).previous) {
        if (layoutIsRunContent(layoutTreeNode(tree, p)))
            first = p;
        else if (layoutChildKind(tree, p) != LayoutChildKind_None)
            break;
    }
    return first;
}

LayoutNodeIndex layoutRunLast(const LayoutTree* tree, LayoutNodeIndex first) {
    LayoutNodeIndex last = first;
    for (uint32_t n = layoutTreeNode(tree, first).next; n != kPrLayoutNodeNone; n = layoutTreeNode(tree, n).next) {
        if (layoutIsRunContent(layoutTreeNode(tree, n)))
            last = n;
        else if (layoutChildKind(tree, n) != LayoutChildKind_None)
            break;
    }
    return last;
}

bool layoutRunIsBlank(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex last) {
    for (uint32_t c = first;; c = layoutTreeNode(tree, c).next) {
        const LayoutTreeNode& n = layoutTreeNode(tree, c);
        if (n.kind == PrLayoutNodeKind_LineBreak && n.childKind != LayoutChildKind_None)
            return false;
        if (n.kind == PrLayoutNodeKind_Text) {
            // The document white space characters, which white-space affects (css-text-3/#white-space-processing).
            PrStringView text = layoutNodeText(tree, c);
            for (size_t i = 0; i < text.size; ++i) {
                char ch = text.data[i];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f')
                    return false;
            }
        }
        if (c == last)
            return true;
    }
}

const PrLayoutBoxStyle& layoutAnonymousBoxStyle(void) {
    static const PrLayoutBoxStyle box = [] {
        PrLayoutBoxStyle b = prLayoutBoxStyleDefault();
        b.display = PrLayoutDisplay_Block;
        return b;
    }();
    return box;
}

const PrLayoutFlexStyle& layoutAnonymousFlexStyle(void) {
    static const PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    return flex;
}

const PrLayoutBoxStyle& layoutAnonymousCellStyle(void) {
    static const PrLayoutBoxStyle box = [] {
        PrLayoutBoxStyle b = prLayoutBoxStyleDefault();
        b.display = PrLayoutDisplay_TableCell;
        return b;
    }();
    return box;
}

const PrLayoutBoxStyle& layoutAnonymousTableStyle(void) {
    static const PrLayoutBoxStyle box = [] {
        PrLayoutBoxStyle b = prLayoutBoxStyleDefault();
        b.display = PrLayoutDisplay_Table;
        return b;
    }();
    return box;
}

bool layoutIsBlankText(const LayoutTree* tree, LayoutNodeIndex node) {
    if (layoutTreeNode(tree, node).kind != PrLayoutNodeKind_Text)
        return false;
    PrStringView text = layoutNodeText(tree, node);
    for (size_t i = 0; i < text.size; ++i) {
        char ch = text.data[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f')
            return false;
    }
    return true;
}

LayoutNodeIndex layoutTablePartRunLast(const LayoutTree* tree, LayoutNodeIndex first, LayoutNodeIndex end) {
    LayoutNodeIndex last = first;
    for (uint32_t n = layoutTreeNode(tree, first).next; n != end; n = layoutTreeNode(tree, n).next) {
        LayoutChildKind kind = layoutChildKind(tree, n);
        if (kind == LayoutChildKind_TablePart)
            last = n;
        else if (kind != LayoutChildKind_None && !layoutIsBlankText(tree, n))
            break;
    }
    return last;
}

int32_t layoutStackLevelAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return 0;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, style);
    if (!box.zIndexed || !layoutZIndexApplies(tree, node, box))
        return 0;
    return layoutEffectStyle(tree->styles, style).zIndex;
}

bool layoutIsStackingContextAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    if (n.parent == kPrLayoutNodeNone)
        return true;
    if (!layoutStyleTraits(tree->styles, style).mayStack)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, style);
    if (box.position == PrLayoutPosition_Fixed || box.position == PrLayoutPosition_Sticky ||
            box.display == PrLayoutDisplay_PageMargin || (box.zIndexed && layoutZIndexApplies(tree, node, box)))
        return true;
    const LayoutStyleTraits& traits = layoutStyleTraits(tree->styles, style);
    if (traits.composites)
        return true;
    return (box.transformed || traits.transforms) && !layoutIsInlineBoxAs(tree, node, style);
}

bool layoutPaintsWholeAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, style);
    return box.position != PrLayoutPosition_Static || box.transformed || layoutOwnsLayer(tree->styles, style) ||
        box.display == PrLayoutDisplay_PageMargin || (box.zIndexed && layoutInItemContainer(tree, node));
}

bool layoutInlinePaintsWhole(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutBoxStyle(styles, style).position != PrLayoutPosition_Static || layoutInlineOwnsLayer(styles, style);
}

bool layoutOwnsLayer(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    const LayoutStyleTraits& traits = layoutStyleTraits(styles, style);
    return traits.composites || traits.transforms;
}

bool layoutInlineOwnsLayer(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    return layoutStyleTraits(styles, style).composites;
}

bool layoutHasLayerAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!layoutKindIsBox(n.kind) || !layoutStyleTraits(tree->styles, style).mayPaintWhole)
        return false;
    if (layoutIsInlineBoxAs(tree, node, style))
        return layoutInlineOwnsLayer(tree->styles, style);
    return layoutOwnsLayer(tree->styles, style);
}

LayoutStyleTraits layoutStyleTraitsFromIndex(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    const PrLayoutStyle& record = layoutStyle(styles, style);
    const PrLayoutBoxStyle& box = layoutTableAt(styles->box, record.groups[PrLayoutGroup_Box]);
    const PrLayoutEffectStyle& effects = layoutTableAt(styles->effects, record.groups[PrLayoutGroup_Effect]);
    bool positioned = layoutPositionIsOutOfFlow(box.position) && box.display != PrLayoutDisplay_None;
    // The properties that give the box a stacking context or a layer of its own, whatever its position. This is the
    // union of what layoutPaintsWholeAs and layoutInlinePaintsWhole read besides position. A z-index may apply to a
    // static box, such as a flex item. A record that names the default effects group, at index 0, has no transform.
    bool transforms = record.groups[PrLayoutGroup_Effect] != 0 && layoutHasTransform(effects);
    // Most records name the default compositing group, which makes no group.
    bool grouped = record.groups[PrLayoutGroup_Compositing] != 0 && layoutCompositingGroups(
        layoutTableAt(styles->compositing, record.groups[PrLayoutGroup_Compositing]));
    bool layered = effects.opacity < 1 || effects.layer || grouped || box.transformed || transforms;
    // A page-margin box makes a stacking context whatever its position (css-page-3/#painting).
    bool moved = box.position != PrLayoutPosition_Static || box.display == PrLayoutDisplay_PageMargin;
    const PrLayoutFlexStyle& flex = layoutTableAt(styles->flex, record.groups[PrLayoutGroup_Flex]);
    uint32_t justifySelf = flex.justifySelf & kPrLayoutAlignmentValueMask;
    PrLayoutBackgroundIndex backgrounds =
        layoutTableAt(styles->decoration, record.groups[PrLayoutGroup_Decoration]).backgroundLayers;
    bool fixedBackground = false;
    if (backgrounds != kPrLayoutBackgroundNone) {
        const LayoutBackground& layers = layoutBackgroundAt(&styles->backgrounds, backgrounds);
        for (uint32_t i = 0; i < layers.count && !fixedBackground; ++i)
            fixedBackground = layers.layers[i].attachment == PrLayoutBackgroundAttachment_Fixed;
    }
    // Only a block container has columns (css-multicol-1/#the-multi-column-model). The default table group, which most
    // records name, has none.
    bool multiColumn = false;
    if (record.groups[PrLayoutGroup_Table] != 0) {
        const PrLayoutFragmentationStyle& fragmentation =
            layoutTableAt(styles->table, record.groups[PrLayoutGroup_Table]).fragmentation;
        bool columns = fragmentation.columnCount != 0 || !layoutLengthIsAuto(fragmentation.columnWidth);
        bool blockContainer = layoutChildrenFromBox(box) == LayoutChildren_Flow &&
            box.display != PrLayoutDisplay_Inline && box.display != PrLayoutDisplay_None &&
            box.display != PrLayoutDisplay_Marker && !prLayoutDisplayIsLayoutInternal(box.display);
        multiColumn = columns && (blockContainer || box.display == PrLayoutDisplay_TableCell ||
            box.display == PrLayoutDisplay_TableCaption);
    }
    return LayoutStyleTraits{
        .childKind = layoutChildKindFromStyle(PrLayoutNodeKind_Element, &box, positioned),
        .vertical = layoutTableAt(styles->font, record.groups[PrLayoutGroup_Font]).writingMode !=
            PrLayoutWritingMode_HorizontalTb,
        .positioned = positioned,
        .mayPaintWhole = moved || layered || box.zIndexed,
        .mayStack = box.position == PrLayoutPosition_Fixed || box.position == PrLayoutPosition_Sticky || box.zIndexed ||
            layered || box.display == PrLayoutDisplay_PageMargin,
        .children = layoutChildrenFromBox(box),
        .justifies = justifySelf != PrLayoutAlignment_Auto && justifySelf != PrLayoutAlignment_Normal,
        .justifiesItems = (flex.justifyItems & kPrLayoutAlignmentValueMask) != PrLayoutAlignment_Normal,
        .composites = effects.opacity < 1 || effects.layer || grouped,
        .transforms = transforms,
        .fixedBackground = fixedBackground,
        .wholeTablePart = (moved || layered || box.zIndexed) && (layoutDisplayIsRowGroup(box.display) ||
            box.display == PrLayoutDisplay_TableRow || box.display == PrLayoutDisplay_TableCell),
        .multiColumn = multiColumn,
    };
}

bool layoutIsTransformed(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    return layoutBoxStyle(tree->styles, n.style).transformed && !layoutIsInlineBox(tree, node);
}

bool layoutHoldsAll(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    // A transform does not apply to an inline box, and a filter does. A filtered root holds nothing: its fixed boxes
    // stay the viewport's (filter-effects-1/#FilterProperty).
    if (box.filtered && n.parent != kPrLayoutNodeNone)
        return true;
    return box.transformed && !layoutIsInlineBox(tree, node);
}

LayoutNodeIndex layoutNodeHolder(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex* PR_NULLABLE containing) {
    PrLayoutPosition position = layoutNodePositionScheme(tree, node);
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != kPrLayoutNodeNone; at =
            layoutTreeNode(tree, at).parent) {
        if (!layoutHoldsPosition(tree, at, position))
            continue;
        if (containing != nullptr)
            *containing = at;
        // The block container of a positioned inline box lays out the boxes it holds, once its lines are laid out.
        LayoutNodeIndex holder = at;
        while (layoutIsInlineBox(tree, holder))
            holder = layoutTreeNode(tree, holder).parent;
        return holder;
    }
    if (containing != nullptr)
        *containing = kPrLayoutNodeNone;
    return kPrLayoutNodeNone;
}

bool layoutNodeClips(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!layoutKindIsElement(n.kind) || n.parent == kPrLayoutNodeNone)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    LayoutClips clips = layoutClips(box);
    return clips.x || clips.y || box.clipped;
}

bool layoutNodeSticks(const LayoutTree* tree, LayoutNodeIndex node) {
    if (tree->styles->stickyGroups == 0)
        return false;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!layoutKindIsBox(n.kind) || n.parent == kPrLayoutNodeNone)
        return false;
    return layoutBoxStyle(tree->styles, n.style).position == PrLayoutPosition_Sticky;
}

LayoutNodeIndex layoutStickyOuterContainer(const LayoutTree* tree, LayoutNodeIndex box) {
    LayoutNodeIndex parent = layoutCachePlacement(tree->cache, box).parent;
    LayoutNodeIndex up = parent != kPrLayoutNodeNone ? positioned(tree, parent).container : kLayoutContainerViewport;
    return containerAround(tree, box, parent, up);
}

LayoutNodeIndex layoutStickyInlineAround(const LayoutTree* tree, LayoutNodeIndex node) {
    return stickyInlineAround(tree, node);
}

PrLayoutVec layoutNodePosition(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind != PrLayoutNodeKind_Text && !layoutIsInlineBox(tree, node))
        return positioned(tree, node).position;
    LayoutNodeIndex parent = layoutCachePlacement(tree->cache, node).parent;
    return parent != kPrLayoutNodeNone ? positioned(tree, parent).position : kLayoutVecZero;
}

LayoutNodeIndex layoutNodeContainer(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind != PrLayoutNodeKind_Text && !layoutIsInlineBox(tree, node))
        return positioned(tree, node).container;
    if (layoutNodeSticks(tree, node))
        return node | kLayoutContainerSticky;
    return layoutStickyOuterContainer(tree, node);
}

LayoutPositioned layoutNodePositioned(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind != PrLayoutNodeKind_Text && !layoutIsInlineBox(tree, node))
        return positioned(tree, node);
    LayoutNodeIndex parent = layoutCachePlacement(tree->cache, node).parent;
    LayoutPositioned at{.position = kLayoutVecZero, .container = kLayoutContainerViewport};
    if (parent != kPrLayoutNodeNone)
        at = positioned(tree, parent);
    if (layoutNodeSticks(tree, node))
        at.container = node | kLayoutContainerSticky;
    else
        at.container = containerAround(tree, node, parent, at.container);
    return at;
}

LayoutPositioned layoutBoxPositioned(const LayoutTree* tree, LayoutNodeIndex box) {
    return positioned(tree, box);
}

LayoutNodeIndex layoutPositionsVerify(const LayoutTree* tree) {
    const LayoutCache& cache = tree->cache;
    for (LayoutNodeIndex node = 0; node < cache.positioned.count; ++node) {
        const LayoutPositioned& memo = cache.positioned.data[node];
        if (memo.version != cache.placed)
            continue;
        PrLayoutVec position = kLayoutVecZero;
        for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutCachePlacement(cache, at).parent)
            position = position + layoutCachePlacement(cache, at).offset;
        if (position != memo.position || freshContainer(tree, node) != memo.container)
            return node;
    }
    return kPrLayoutNodeNone;
}

} // namespace pr_impl
