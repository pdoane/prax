// Layout trees: the nodes, what has changed about them, and the state of each stage that reads them.

#include "pr/layout_tree.h"
#include "layout/src/layout_tree.h"

#include "pr/alloc.h"
#include "pr/bitset.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/mem.h"
#include "pr/raw_string.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_fragmentation.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_shaping.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

LayoutTreeNode& nodeAt(LayoutTree* tree, LayoutNodeIndex node) {
    PR_ASSERT(node != kPrLayoutNodeNone && node < tree->nodes.count && tree->nodes.data[node].live);
    return tree->nodes.data[node];
}

// Removes `node` from `list`, which holds it once.
void unlist(RawArray<uint32_t>& list, LayoutNodeIndex node) {
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.data[i] == node) {
            list.data[i] = list.last();
            list.pop();
            return;
        }
    }
}

// Sets the record of the node `index` to `style`, without retaining or releasing either record.
void setStyle(LayoutTree* tree, LayoutNodeIndex index, PrLayoutStyleIndex style) {
    LayoutTreeNode& n = tree->nodes.data[index];
    n.style = style;
    const LayoutStyleTraits& traits = layoutStyleTraits(tree->styles, style);
    bool box = layoutKindIsBox(n.kind);
    n.vertical = traits.vertical;
    n.positioned = box && traits.positioned;
    n.children = layoutKindIsElement(n.kind) ? traits.children & 0x3u : LayoutChildren_Flow;
    n.justifies = layoutKindIsElement(n.kind) && traits.justifies;
    n.justifiesItems = layoutKindIsElement(n.kind) && traits.justifiesItems;
    bool fixedBackground = box && traits.fixedBackground;
    if (fixedBackground != n.fixedBackground) {
        if (fixedBackground)
            tree->viewportPainters.push(index);
        else
            unlist(tree->viewportPainters, index);
        n.fixedBackground = fixedBackground;
    }
    // A line break takes part as inline content, unless its display is none.
    n.childKind = box || traits.childKind == LayoutChildKind_None ? traits.childKind & 0x7u : LayoutChildKind_Inline;
}

// Removes the record of the element `node` for the first formatted line, which it must have, and releases the tree's
// reference to the record.
void dropFirstLine(LayoutTree* tree, LayoutNodeIndex node) {
    uint32_t hash = layoutFirstLineHash(node);
    uint32_t at = tree->firstLineIndex.find(hash, [&](uint32_t i) { return tree->firstLines.data[i].node == node; });
    PR_ASSERT(at != kHashMissing);
    layoutStyleRelease(tree->styles, tree->firstLines.data[at].style);
    tree->firstLineIndex.erase(hash, at);
    uint32_t last = tree->firstLines.count - 1;
    if (at != last) {
        tree->firstLineIndex.relocate(layoutFirstLineHash(tree->firstLines.data[last].node), last, at);
        tree->firstLines.data[at] = tree->firstLines.data[last];
    }
    tree->firstLines.pop();
    tree->nodes.data[node].listed = false;
}

// Removes the highlights of the text `node`, which has some. Orders the ranges by their starts, and records how far the
// ranges up to each place reach.
void indexHighlights(LayoutHighlights& h) {
    h.byStart.resize(h.ranges.count);
    for (uint32_t i = 0; i < h.ranges.count; ++i)
        h.byStart.data[i] = i;
    const PrLayoutHighlight* ranges = h.ranges.data;
    sortItems(h.byStart.span(), [&](uint32_t a, uint32_t b) {
        return ranges[a].start < ranges[b].start || (ranges[a].start == ranges[b].start && a < b);
    });
    h.reach.resize(h.ranges.count);
    uint32_t furthest = 0;
    for (uint32_t i = 0; i < h.byStart.count; ++i) {
        uint32_t end = ranges[h.byStart.data[i]].end;
        furthest = end > furthest ? end : furthest;
        h.reach.data[i] = furthest;
    }
}

void dropHighlights(LayoutTree* tree, LayoutNodeIndex node) {
    uint32_t hash = layoutHighlightHash(node);
    uint32_t at = tree->highlightIndex.find(hash, [&](uint32_t i) { return tree->highlights.data[i].node == node; });
    PR_ASSERT(at != kHashMissing);
    tree->highlights.data[at].ranges.reset();
    tree->highlights.data[at].byStart.reset();
    tree->highlights.data[at].reach.reset();
    tree->highlightIndex.erase(hash, at);
    uint32_t last = tree->highlights.count - 1;
    if (at != last) {
        tree->highlightIndex.relocate(layoutHighlightHash(tree->highlights.data[last].node), last, at);
        tree->highlights.data[at] = tree->highlights.data[last];
    }
    tree->highlights.pop();
    tree->nodes.data[node].listed = false;
}

// Frees what a node holds for its kind: its reference to its record and to its record for the first formatted line, a
// text node's text and highlights, and a replaced element's entry.
void releaseKind(LayoutTree* tree, LayoutTreeNode& n) {
    if (n.style != kPrLayoutStyleNone)
        layoutStyleRelease(tree->styles, n.style);
    if (n.listed && n.kind == PrLayoutNodeKind_Text)
        dropHighlights(tree, LayoutNodeIndex(&n - tree->nodes.data));
    else if (n.listed)
        dropFirstLine(tree, LayoutNodeIndex(&n - tree->nodes.data));
    if (n.kind == PrLayoutNodeKind_Text) {
        prRawStringReset(&tree->texts.data[n.text]);
        tree->freeTexts.push(n.text);
    }
    if (n.kind == PrLayoutNodeKind_Replaced) {
        LayoutReplaced& replaced = tree->replaced.data[n.replaced];
        prDrawListRelease(replaced.content);
        prDrawImageRelease(replaced.image);
        prLayoutPainterRelease(replaced.painter);
        replaced = LayoutReplaced{};
        tree->freeReplaced.push(n.replaced);
    }
}

void unlink(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutTreeNode& n = nodeAt(tree, node);
    if (n.parent == kPrLayoutNodeNone)
        return;
    LayoutTreeNode& parent = nodeAt(tree, n.parent);
    if (n.previous != kPrLayoutNodeNone)
        nodeAt(tree, n.previous).next = n.next;
    else
        parent.first = n.next;
    if (n.next != kPrLayoutNodeNone)
        nodeAt(tree, n.next).previous = n.previous;
    else
        parent.last = n.previous;
    n.parent = kPrLayoutNodeNone;
    n.previous = kPrLayoutNodeNone;
    n.next = kPrLayoutNodeNone;
}

// Frees the node and its descendants, which have been removed from any parent.
void freeSubtree(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutTreeNode& n = nodeAt(tree, node);
    for (LayoutNodeIndex child = n.first; child != kPrLayoutNodeNone;) {
        LayoutNodeIndex next = nodeAt(tree, child).next;
        freeSubtree(tree, child);
        child = next;
    }
    releaseKind(tree, n);
    if (n.key != 0)
        tree->keys.erase(KeyIndex::hashOf(n.key), node);
    layoutCacheForget(tree->cache, node);
    if (n.readsViewport) {
        unlist(tree->viewportReaders, node);
        n.readsViewport = false;
    }
    if (n.fixedBackground) {
        unlist(tree->viewportPainters, node);
        n.fixedBackground = false;
    }
    layoutPaintForget(tree, node);
    layoutStackingForget(tree, node);
    if (node < tree->shapings.count) {
        layoutShapingDestroy(tree->shapings.data[node]);
        tree->shapings.data[node] = nullptr;
    }
    n.live = false;
    ++n.generation;
    n.next = tree->freeNodes;
    tree->freeNodes = node;
    if (tree->root == node) {
        tree->root = kPrLayoutNodeNone;
        tree->rootName = kPrLayoutNodeNone;
    }
    for (uint32_t i = 0; i < tree->frame.roots.count; ++i) {
        if (tree->frame.roots.data[i] == node) {
            tree->frame.roots.erase(i);
            break;
        }
    }
    for (uint32_t i = 0; i < tree->roots.count; ++i) {
        if (tree->roots.data[i].node != node)
            continue;
        layoutCacheRetire(tree->cache, tree->roots.data[i].fragment);
        layoutFragmentRelease(tree->roots.data[i].fragment);
        prDrawListRelease(tree->roots.data[i].drawing);
        tree->roots.erase(i);
        break;
    }
    for (uint32_t i = tree->rootReaders.count; i-- > 0;) {
        if (tree->rootReaders.data[i].replaced == node || tree->rootReaders.data[i].root == node)
            tree->rootReaders.erase(i);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

// How far a change reaches up the tree.
enum {
    // To the holder of each box out of flow that it is in. Beyond that holder, it reaches as a change in the holder.
    // The ancestors between the box and its holder hold only a change out of flow.
    Reach_Holders,
    // To every ancestor, as a change that may move a box out of flow to another holder: a change to its position or
    // display, a transform appearing, or a change to the children of its parent.
    Reach_Ancestors,
};
typedef uint8_t Reach;

// Returns true if `node` is a page-margin box (PrLayoutDisplay_PageMargin), which takes no part in its parent's layout.
bool isMarginBox(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    return layoutKindIsElement(n.kind) && layoutBoxStyle(tree->styles, n.style).display == PrLayoutDisplay_PageMargin;
}

// Records that a page-margin box among the children of `parent` changed, came or went.
void markMargins(LayoutTree* tree, LayoutNodeIndex parent) {
    tree->pendingLayout |= LayoutInputFlag_Layout;
    nodeAt(tree, parent).dirty |= LayoutDirtyFlag_Margins;
}

// Records that the inputs of the node's layout changed, and that its ancestors' subtrees did, as far as `reach` says. A
// change in a page-margin box reaches its parent only as a change of its margins. Margin boxes are children of the
// root, so the walk reads a child's style only when it reaches the root, and only if `kMargins` says that the tree has
// a margin box.
template <bool kMargins>
void markDirtyIn(LayoutTree* tree, LayoutNodeIndex node, Reach reach) {
    tree->pendingLayout |= LayoutInputFlag_Layout;
    LayoutTreeNode& n = nodeAt(tree, node);
    n.dirty |= LayoutDirtyFlag_Self;
    layoutCacheInvalidateIntrinsic(tree->cache, node);
    // A flex or grid container's text is laid out in anonymous items, each kept under its run's first node. A change to
    // any node of the run drops what that node keeps. The node's own mark does not guard it, because collecting the
    // run's text clears the mark.
    bool content = n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak;
    if (content && layoutInItemContainer(tree, node)) {
        LayoutNodeIndex first = layoutRunFirst(tree, node);
        if (first != kPrLayoutNodeNone)
            layoutCacheInvalidate(tree->cache, first);
    }
    if (reach == Reach_Ancestors) {
        // An ancestor whose ancestors all hold a change ends the walk.
        n.dirty |= LayoutDirtyFlag_Ancestors;
        for (uint32_t child = node, at = n.parent; at != kPrLayoutNodeNone; child = at, at = nodeAt(tree, at).parent) {
            if (kMargins && at == tree->root && isMarginBox(tree, child)) {
                markMargins(tree, at);
                break;
            }
            LayoutTreeNode& ancestor = nodeAt(tree, at);
            if ((ancestor.dirty & LayoutDirtyFlag_Ancestors) != 0)
                break;
            if ((ancestor.dirty & LayoutDirtyFlag_Descendant) == 0)
                layoutCacheInvalidateIntrinsic(tree->cache, at);
            ancestor.dirty |= LayoutDirtyFlag_Descendant | LayoutDirtyFlag_Ancestors;
        }
        return;
    }
    // The box out of flow whose holder the walk has not yet passed, or none. Until the walk passes the holder, an
    // ancestor holds only a change out of flow. A positioned inline box's block container is the holder of the boxes
    // that the inline box is the containing block of.
    LayoutNodeIndex box = layoutIsOutOfFlow(tree, node) ? node : kPrLayoutNodeNone;
    bool inlineHolder = false;
    for (uint32_t child = node, at = n.parent; at != kPrLayoutNodeNone; child = at, at = nodeAt(tree, at).parent) {
        if (kMargins && at == tree->root && isMarginBox(tree, child)) {
            markMargins(tree, at);
            break;
        }
        LayoutTreeNode& ancestor = nodeAt(tree, at);
        if (box != kPrLayoutNodeNone) {
            bool holds;
            if (inlineHolder)
                holds = !layoutIsInlineBox(tree, at);
            else
                holds = layoutHoldsPosition(tree, at, layoutNodePositionScheme(tree, box));
            if (holds && layoutIsInlineBox(tree, at)) {
                inlineHolder = true;
                holds = false;
            }
            if (!holds) {
                ancestor.dirty |= LayoutDirtyFlag_OutOfFlow;
                continue;
            }
            box = kPrLayoutNodeNone;
            inlineHolder = false;
        }
        // An ancestor already marked as holding a change had its intrinsic sizes dropped then, as had every ancestor
        // above it, and nothing measures them again before the next layout. Each ancestor above it holds the change as
        // far as this walk would mark it, or further.
        if ((ancestor.dirty & LayoutDirtyFlag_Descendant) != 0)
            break;
        ancestor.dirty |= LayoutDirtyFlag_Descendant;
        layoutCacheInvalidateIntrinsic(tree->cache, at);
        if (layoutIsOutOfFlow(tree, at))
            box = at;
    }
}

// Records that the inputs of the node's layout changed, as markDirtyIn does. A tree without page-margin boxes walks
// without looking for them.
void markDirty(LayoutTree* tree, LayoutNodeIndex node, Reach reach = Reach_Holders) {
    if (tree->styles->pageMarginGroups != 0)
        markDirtyIn<true>(tree, node, reach);
    else
        markDirtyIn<false>(tree, node, reach);
}

// Returns true if two records differ in their flex groups only in what a flex or grid container reads of its item: its
// order, its flexibility and its alignment.
bool onlyItemFlex(const LayoutStyles* styles, PrLayoutStyleIndex a, PrLayoutStyleIndex b) {
    PrLayoutFlexStyle x = layoutFlexStyle(styles, a);
    PrLayoutFlexStyle y = layoutFlexStyle(styles, b);
    x.grow = y.grow;
    x.shrink = y.shrink;
    x.basis = y.basis;
    x.order = y.order;
    x.alignSelf = y.alignSelf;
    return prMemEquals(&x, &y, sizeof x);
}

// Returns true if two records differ in their grid groups only in what a grid container reads of its item: its
// placement.
bool onlyItemGrid(const LayoutStyles* styles, PrLayoutStyleIndex a, PrLayoutStyleIndex b) {
    PrLayoutGridStyle x = layoutGridStyle(styles, a);
    PrLayoutGridStyle y = layoutGridStyle(styles, b);
    x.columnStart = y.columnStart;
    x.columnEnd = y.columnEnd;
    x.rowStart = y.rowStart;
    x.rowEnd = y.rowEnd;
    return prMemEquals(&x, &y, sizeof x);
}

// Marks the content of the flex or grid container `parent` that adjoins a position among its children, between
// `previous` and `next`, skipping the children that generate no box. This content is the runs that a change splits,
// joins or extends if it changes which children are there, or whether one generates a box or is out of flow
// (layoutEachItem).
void markRunsAround(LayoutTree* tree, LayoutNodeIndex parent, LayoutNodeIndex previous, LayoutNodeIndex next) {
    if (parent == kPrLayoutNodeNone || !layoutChildrenAreItems(nodeAt(tree, parent).children))
        return;
    for (uint32_t p = previous; p != kPrLayoutNodeNone; p = nodeAt(tree, p).previous) {
        if (layoutIsRunContent(nodeAt(tree, p)))
            markDirty(tree, p);
        if (layoutChildKind(tree, p) != LayoutChildKind_None)
            break;
    }
    for (uint32_t n = next; n != kPrLayoutNodeNone; n = nodeAt(tree, n).next) {
        if (layoutIsRunContent(nodeAt(tree, n)))
            markDirty(tree, n);
        if (layoutChildKind(tree, n) != LayoutChildKind_None)
            break;
    }
}

// Records that the children of `parent` changed by the coming or going of `child`.
void markChildren(LayoutTree* tree, LayoutNodeIndex parent, LayoutNodeIndex child) {
    if (tree->styles->pageMarginGroups != 0 && parent == tree->root && isMarginBox(tree, child))
        markMargins(tree, parent);
    else
        markDirty(tree, parent, Reach_Ancestors);
}

// Marks the child `node` of a flex or grid container if it is content, and the content around it, in the same way as
// markRunsAround.
void markRunNeighbours(LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = nodeAt(tree, node);
    if (n.parent == kPrLayoutNodeNone || !layoutChildrenAreItems(nodeAt(tree, n.parent).children))
        return;
    if (layoutIsRunContent(n))
        markDirty(tree, node);
    markRunsAround(tree, n.parent, n.previous, n.next);
}

// Links the new node `node` as the last child of element `parent`, or leaves it detached if `parent` is
// kPrLayoutNodeNone, and marks what prLayoutTreeInsert marks. A new last child joins the run of content before it if it
// is content itself, and otherwise ends no run that did not already end there.
void appendNew(LayoutTree* tree, LayoutNodeIndex parent, LayoutNodeIndex node) {
    if (parent == kPrLayoutNodeNone)
        return;
    LayoutTreeNode& p = nodeAt(tree, parent);
    PR_ASSERT(layoutKindIsElement(p.kind));
    markChildren(tree, parent, node);
    LayoutTreeNode& n = nodeAt(tree, node);
    n.parent = parent;
    n.previous = p.last;
    if (p.last != kPrLayoutNodeNone)
        nodeAt(tree, p.last).next = node;
    else
        p.first = node;
    p.last = node;
    if (layoutChildrenAreItems(p.children) && layoutIsRunContent(n))
        markDirty(tree, node);
    layoutStackingInserted(tree, node);
}

// Destroys each shaping that names a font group in the tree's changedFonts, and marks its context's first node dirty.
// Also marks dirty each node whose record's font group is one of those groups.
void markFontReaders(LayoutTree* tree) {
    const uint64_t* changed = tree->changedFonts.data;
    uint32_t groups = tree->changedFonts.count * 64;
    auto isChanged = [&](uint32_t group) { return group < groups && bitsetTest(changed, group); };
    for (uint32_t i = 0; i < tree->shapings.count; ++i) {
        LayoutInlineShaping* PR_NULLABLE shaping = tree->shapings.data[i];
        if (shaping == nullptr)
            continue;
        for (uint32_t k = 0; k < shaping->fonts.count; ++k) {
            if (isChanged(shaping->fonts.data[k])) {
                layoutShapingDestroy(shaping);
                tree->shapings.data[i] = nullptr;
                markDirty(tree, i);
                break;
            }
        }
    }
    for (uint32_t i = 0; i < tree->nodes.count; ++i) {
        const LayoutTreeNode& node = tree->nodes.data[i];
        if (node.live && layoutKindIsBox(node.kind) &&
                isChanged(layoutStyle(tree->styles, node.style).groups[PrLayoutGroup_Font]))
            markDirty(tree, i);
    }
}

// Returns true if a face of the font group hints its advances, which then depend on the target pixel ratio.
bool hintsAdvances(const LayoutStyles* styles, uint32_t group) {
    if (group >= styles->faces.count)
        return false;
    const LayoutFontFaces& faces = styles->faces.data[group];
    if (faces.font == nullptr)
        return false;
    for (uint32_t face = 0; face < faces.metrics.count; ++face)
        if (prFontHinting(layoutFaceFont(faces, face)) == PrFontHinting_Full)
            return true;
    return false;
}

// Drops each shaping set in a font that hints its advances, and marks its node dirty, because a new target pixel ratio
// changes those advances.
void forgetHintedShapings(LayoutTree* tree) {
    for (uint32_t i = 0; i < tree->shapings.count; ++i) {
        LayoutInlineShaping* PR_NULLABLE shaping = tree->shapings.data[i];
        if (shaping == nullptr)
            continue;
        for (uint32_t k = 0; k < shaping->fonts.count; ++k) {
            if (hintsAdvances(tree->styles, shaping->fonts.data[k])) {
                layoutShapingDestroy(shaping);
                tree->shapings.data[i] = nullptr;
                markDirty(tree, i);
                break;
            }
        }
    }
}

// Marks dirty each node whose layout or intrinsic sizes read the viewport's extent, and empties the list of them.
void markViewportReaders(LayoutTree* tree) {
    for (uint32_t i = 0; i < tree->viewportReaders.count; ++i) {
        LayoutNodeIndex node = tree->viewportReaders.data[i];
        tree->nodes.data[node].readsViewport = false;
        markDirty(tree, node);
    }
    tree->viewportReaders.clear();
}

void markPainted(LayoutTree* tree, LayoutNodeIndex node, bool beyondLayer);

// Takes the roots that a layout is given: the viewport's, and the others in order. A root that the last layout did not
// lay out is marked to be laid out, and joins the stacking contexts as a context of its own. A root that the layout no
// longer lays out gives up its fragment and its drawing. Returns true if the roots or their extents differ from the
// last layout's.
bool takeRoots(LayoutTree* tree, const PrLayoutRoot* roots, uint32_t count) {
    auto isRoot = [&](LayoutNodeIndex index) {
        return layoutKindIsBox(nodeAt(tree, index).kind) && nodeAt(tree, index).parent == kPrLayoutNodeNone;
    };
    bool changed = false;
    LayoutNodeIndex viewport = layoutNodeIndex(tree, roots[0].node);
    PR_ASSERT(viewport == kPrLayoutNodeNone || isRoot(viewport));
    if (viewport != tree->root) {
        if (viewport != kPrLayoutNodeNone) {
            markDirty(tree, viewport);
            layoutStackingInserted(tree, viewport);
        }
        tree->root = viewport;
        tree->rootName = roots[0].node;
        changed = true;
    }
    // The other roots of the last layout, in the same order and at the same extents, are taken as they are.
    bool same = count - 1 == tree->roots.count;
    for (uint32_t i = 1; same && i < count; ++i) {
        const LayoutLaidRoot& old = tree->roots.data[i - 1];
        same = old.node == layoutNodeIndex(tree, roots[i].node) && old.extent == roots[i].extent;
    }
    if (same) {
        if (changed)
            layoutStackingStaleAll(tree);
        return changed;
    }
    RawArray<LayoutLaidRoot> taken;
    for (uint32_t i = 1; i < count; ++i) {
        LayoutNodeIndex node = layoutNodeIndex(tree, roots[i].node);
        PR_ASSERT(node != kPrLayoutNodeNone && node != viewport && isRoot(node));
        LayoutLaidRoot root{.node = node, .extent = roots[i].extent};
        bool found = false;
        for (uint32_t k = 0; k < tree->roots.count && !found; ++k) {
            LayoutLaidRoot& old = tree->roots.data[k];
            if (old.node != node)
                continue;
            found = true;
            changed = changed || k != i - 1 || old.extent != root.extent;
            root.fragment = old.fragment;
            root.drawing = old.drawing;
            old = LayoutLaidRoot{};
        }
        if (!found) {
            markDirty(tree, node);
            layoutStackingInserted(tree, node);
            changed = true;
        }
        taken.push(root);
    }
    for (uint32_t k = 0; k < tree->roots.count; ++k) {
        const LayoutLaidRoot& old = tree->roots.data[k];
        if (old.node == kPrLayoutNodeNone)
            continue;
        layoutCacheRetire(tree->cache, old.fragment);
        layoutFragmentRelease(old.fragment);
        prDrawListRelease(old.drawing);
        changed = true;
    }
    // The array takes the storage of the roots taken.
    tree->roots.reset();
    tree->roots = taken;
    if (changed)
        layoutStackingStaleAll(tree);
    return changed;
}

// Marks painted each box that has a background layer fixed to the viewport, because its recording depends on the
// viewport's extent.
void markViewportPainters(LayoutTree* tree) {
    for (uint32_t i = 0; i < tree->viewportPainters.count; ++i)
        markPainted(tree, tree->viewportPainters.data[i], false);
}

// Records that what recordings read of `node`'s subtree changed, and of each ancestor's subtree, up to the first
// ancestor with a layer of its own, whose recording holds the node's. If `beyondLayer` is true, because `node` itself
// may have lost or gained its layer, the marks continue past that ancestor.
void markPainted(LayoutTree* tree, LayoutNodeIndex node, bool beyondLayer) {
    tree->pendingPaint |= LayoutInputFlag_Paint;
    uint32_t paints = tree->paint.paints;
    for (uint32_t at = node; at != kPrLayoutNodeNone;) {
        LayoutTreeNode& n = nodeAt(tree, at);
        // An ancestor marked by this paint already has its own ancestors marked.
        if (at != node && n.paintSubtree == paints)
            break;
        n.paintSubtree = paints;
        if (layoutHasLayer(tree, at) && (at != node || !beyondLayer))
            break;
        at = n.parent;
    }
}

// Returns true if two records differ, for layout, only in whether the box is transformed or filtered. Those decide
// whether the box holds the boxes out of flow in it.
bool sameButHolding(const LayoutStyles* styles, PrLayoutStyleIndex a, PrLayoutStyleIndex b) {
    if (layoutStyle(styles, a).groups[PrLayoutGroup_Font] != layoutStyle(styles, b).groups[PrLayoutGroup_Font] ||
            layoutStyle(styles, a).groups[PrLayoutGroup_Flex] != layoutStyle(styles, b).groups[PrLayoutGroup_Flex])
        return false;
    PrLayoutBoxStyle x = layoutBoxStyle(styles, a);
    PrLayoutBoxStyle y = layoutBoxStyle(styles, b);
    x.transformed = y.transformed;
    x.filtered = y.filtered;
    return prMemEquals(&x, &y, sizeof(PrLayoutBoxStyle));
}

// Returns true if the styles `a` and `b` of a table or a table part differ in the style or color of a border. `box` is
// the box style of the table or part. A table that collapses its borders resolves them during layout
// (css-tables-3/#border-collapsing), so the borders' styles and colors are inputs of the table's layout.
bool collapsingBorders(const LayoutStyles* styles, const PrLayoutBoxStyle& box, PrLayoutStyleIndex a, PrLayoutStyleIndex b) {
    if (!layoutIsTable(box) && !layoutDisplayIsTablePart(box.display))
        return false;
    const PrLayoutDecorationStyle& x = layoutDecorationStyle(styles, a);
    const PrLayoutDecorationStyle& y = layoutDecorationStyle(styles, b);
    return !prMemEquals(x.borderStyle, y.borderStyle, sizeof x.borderStyle) ||
        !prMemEquals(x.borderColor, y.borderColor, sizeof x.borderColor);
}

// Returns true if a box out of flow may be in the subtree of `node`: the fragment that the last layout placed it with
// found one there, through a placeholder, a box it holds or one it lists, or it was placed with no fragment.
bool mayHoldOutOfFlow(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutCache& cache = tree->cache;
    const LayoutFragment* fragment = layoutCachePlaced(cache, node);
    LayoutFragmentFlags any = LayoutFragmentFlag_Placeholders | LayoutFragmentFlag_Holds | LayoutFragmentFlag_Pending;
    return fragment == nullptr || (fragment->flags & any) != 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// Returns the scratch memory of the tree's layouts, created by the first layout.
LayoutScratch* scratchOf(LayoutTree* tree) {
    if (tree->scratch == nullptr)
        tree->scratch = layoutScratchCreate();
    return tree->scratch;
}

// ---------------------------------------------------------------------------------------------------------------------
// Verification

// The first difference between two results, as prLayoutTreeVerify reports it.
struct Mismatch {
    LayoutNodeIndex node;           // the node it concerns, or kPrLayoutNodeNone
    PrTextBuffer what;
};

// Appends a layout unit as pixels, to two decimals.
void appendUnit(PrTextBuffer* out, PrLayoutUnit unit) {
    int64_t raw = unit.raw;
    if (raw < 0) {
        prTextBufferAppendChar(out, '-');
        raw = -raw;
    }
    prTextBufferAppendU64(out, uint64_t(raw / kPrLayoutUnitsPerPx), 10);
    int64_t hundredths = (raw % kPrLayoutUnitsPerPx) * 100 / kPrLayoutUnitsPerPx;
    if (hundredths != 0) {
        prTextBufferAppendChar(out, '.');
        if (hundredths < 10)
            prTextBufferAppendChar(out, '0');
        prTextBufferAppendU64(out, uint64_t(hundredths), 10);
    }
    prTextBufferAppend(out, "px");
}

void appendVec(PrTextBuffer* out, PrLayoutVec v) {
    appendUnit(out, v.x);
    prTextBufferAppend(out, " x ");
    appendUnit(out, v.y);
}

// Records the first mismatch: `field` of `node`'s fragment is `cached` in the cached result and `fresh` in a fresh one.
void mismatch(Mismatch* out, LayoutNodeIndex node, const char* field, PrLayoutVec cached, PrLayoutVec fresh) {
    out->node = node;
    prTextBufferAppend(&out->what, field);
    prTextBufferAppend(&out->what, ": cached ");
    appendVec(&out->what, cached);
    prTextBufferAppend(&out->what, ", fresh ");
    appendVec(&out->what, fresh);
}

void mismatch(Mismatch* out, LayoutNodeIndex node, const char* what) {
    out->node = node;
    prTextBufferAppend(&out->what, what);
}

// The page area of one page that brokenLine checks lines against: its block range in the page's coordinates, along the
// physical axis of the root's block flow.
struct PageArea {
    PrLayoutUnit start;
    PrLayoutUnit end;
    bool horizontal;                // true if the root's writing mode is horizontal, with a vertical block axis
    LayoutMode mode;                // the writing mode of the root, which monolithic boxes are found by
};

// Returns the box whose line in the subtree of `fragment`, at `offset` in its page, crosses an edge of `area`, or none.
// A line that is taller than the page area may cross its end. `box` is the nearest box around `fragment`. Lines inside
// a monolithic box and boxes out of flow are not checked, since no break falls inside them.
LayoutNodeIndex brokenLine(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset,
        const PageArea& area, LayoutNodeIndex box) {
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        const LayoutFragment* c = child.fragment;
        PrLayoutVec at = offset + child.offset;
        if (c->kind == LayoutFragmentKind_Line) {
            PrLayoutUnit start = area.horizontal ? at.y : at.x;
            PrLayoutUnit size = area.horizontal ? c->extent.y : c->extent.x;
            PrLayoutUnit end = start + size;
            bool crosses = (start < area.start && end > area.start) || (start < area.end && end > area.end);
            if (crosses && size <= area.end - area.start)
                return box;
            continue;
        }
        if ((c->kind != LayoutFragmentKind_Box && c->kind != LayoutFragmentKind_Around) ||
                (c->flags & LayoutFragmentFlag_OutOfFlow) != 0)
            continue;
        LayoutNodeIndex inner = box;
        if (c->kind == LayoutFragmentKind_Box && c->node != kPrLayoutNodeNone) {
            // A margin box lies outside the page area, and is not fragmented.
            if (layoutIsMonolithic(tree, c->node, area.mode) || isMarginBox(tree, c->node))
                continue;
            inner = c->node;
        }
        LayoutNodeIndex found = brokenLine(tree, c, at, area, inner);
        if (found != kPrLayoutNodeNone)
            return found;
    }
    return kPrLayoutNodeNone;
}

// Returns true if no line of the tree's pages crosses an edge of its page area (css-break-4/#possible-breaks: a break
// between lines never falls inside one). Otherwise records the box that holds the first such line.
bool pagesVerify(const LayoutTree* tree, Mismatch* found) {
    if (tree->pages.count == 0 || tree->fragment == nullptr || tree->fragment->childCount == 0)
        return true;
    const LayoutFragment* stack = tree->fragment->children[0].fragment;
    PR_ASSERT(stack->childCount == tree->pages.count);
    LayoutMode mode = layoutOwnMode(tree, tree->root);
    bool horizontal = layoutIsHorizontal(mode);
    for (uint32_t i = 0; i < stack->childCount; ++i) {
        const PrLayoutPageBox& box = tree->pages.data[i].box;
        PrLayoutUnit extent = horizontal ? box.extent.y : box.extent.x;
        PageArea area{
            .start = horizontal ? box.margin[PrRectSide_Top] : box.margin[PrRectSide_Left],
            .end = extent - (horizontal ? box.margin[PrRectSide_Bottom] : box.margin[PrRectSide_Right]),
            .horizontal = horizontal,
            .mode = mode,
        };
        LayoutNodeIndex node = brokenLine(tree, stack->children[i].fragment, kLayoutVecZero, area, tree->root);
        if (node != kPrLayoutNodeNone) {
            mismatch(found, node, "a line crosses an edge of the page area of page ");
            prTextBufferAppendU64(&found->what, i + 1, 10);
            return false;
        }
    }
    return true;
}

// Returns true if two kept baseline sets are the same, with the same clamps.
bool sameBaselines(const LayoutBaselines& a, const LayoutBaselines& b) {
    for (uint32_t edge = 0; edge < LayoutBaselineEdge_Count; ++edge) {
        if (a.offset[edge] != b.offset[edge] || a.source[edge] != b.source[edge])
            return false;
    }
    const LayoutBaselineClamps* x = a.clamps;
    const LayoutBaselineClamps* y = b.clamps;
    if (x == nullptr || y == nullptr)
        return x == y;
    for (uint32_t edge = 0; edge < LayoutBaselineEdge_Count; ++edge) {
        if (x->clamped[edge] != y->clamped[edge] || (x->clamped[edge] &&
                (x->unclamped[edge] != y->unclamped[edge] || x->lo[edge] != y->lo[edge] || x->hi[edge] != y->hi[edge])))
            return false;
    }
    return true;
}

// Returns true if two text fragments hold the same glyphs in the same fonts, writing the first difference to `out`.
bool sameText(const LayoutFragment* x, const LayoutFragment* y, LayoutNodeIndex node, Mismatch* out) {
    const LayoutFragmentText& a = x->text;
    const LayoutFragmentText& b = y->text;
    if (a.size != b.size || a.glyphCount != b.glyphCount || a.fontCount != b.fontCount || a.rangeCount != b.rangeCount) {
        mismatch(out, node, "the text run's size, glyph count, font count or font range count");
        return false;
    }
    const PrFont* const* fa = layoutFragmentTextFonts(x);
    const PrFont* const* fb = layoutFragmentTextFonts(y);
    for (uint32_t i = 0; i < a.fontCount; ++i) {
        if (fa[i] != fb[i]) {
            mismatch(out, node, "a font of the text run");
            return false;
        }
    }
    const LayoutFragmentFontRange* ra = layoutFragmentTextRanges(x);
    const LayoutFragmentFontRange* rb = layoutFragmentTextRanges(y);
    for (uint32_t i = 0; i < a.rangeCount; ++i) {
        if (ra[i].font != rb[i].font || ra[i].glyphEnd != rb[i].glyphEnd || ra[i].scale != rb[i].scale) {
            mismatch(out, node, "a font range's font, glyphs or scale");
            return false;
        }
    }
    for (uint32_t i = 0; i < a.glyphCount; ++i) {
        const PrFontGlyph& g = a.glyphs[i];
        const PrFontGlyph& h = b.glyphs[i];
        if (g.id != h.id || g.position.x != h.position.x || g.position.y != h.position.y) {
            mismatch(out, node, "a glyph's id or position");
            return false;
        }
    }
    if (a.stopCount != b.stopCount || a.end != b.end || x->flags != y->flags) {
        mismatch(out, node, "the text run's caret stop count, bytes or flags");
        return false;
    }
    const LayoutCaretStop* sa = layoutFragmentTextStops(x);
    const LayoutCaretStop* sb = layoutFragmentTextStops(y);
    for (uint32_t i = 0; i < a.stopCount; ++i) {
        if (sa[i].byte != sb[i].byte || sa[i].along != sb[i].along) {
            mismatch(out, node, "a caret stop's offset or position");
            return false;
        }
    }
    return true;
}

// Returns true if two fragment trees are the same layout. `a` is the cached result and `b` the fresh one. On the first
// difference, writes it to `out`, naming the nearest node at or above it, which is `node` for an anonymous fragment.
bool sameFragments(const LayoutFragment* a, const LayoutFragment* b, LayoutNodeIndex node, Mismatch* out) {
    if (a->node != kPrLayoutNodeNone)
        node = a->node;
    if (a->kind != b->kind || a->node != b->node) {
        mismatch(out, node, "the fragment's kind or node");
        return false;
    }
    if (a->extent != b->extent) {
        mismatch(out, node, "extent", a->extent, b->extent);
        return false;
    }
    if (a->overflow != b->overflow) {
        mismatch(out, node, "overflow");
        return false;
    }
    if (a->broken != b->broken || a->turn != b->turn || a->childCount != b->childCount) {
        mismatch(out, node, "the fragment's children or broken sides");
        return false;
    }
    switch (a->kind) {
        case LayoutFragmentKind_Box:
            if (!sameBaselines(a->box.baselines, b->box.baselines)) {
                mismatch(out, node, "the box's baselines or their sets");
                return false;
            }
            break;
        case LayoutFragmentKind_Text:
            if (!sameText(a, b, node, out))
                return false;
            break;
        case LayoutFragmentKind_Replaced:
            if (a->replaced.content != b->replaced.content) {
                mismatch(out, node, "content box");
                return false;
            }
            break;
        default:
            break;
    }
    for (uint32_t i = 0; i < a->childCount; ++i) {
        const LayoutChild& c = a->children[i];
        const LayoutChild& d = b->children[i];
        if (c.offset != d.offset) {
            uint32_t at = c.fragment->node != kPrLayoutNodeNone ? c.fragment->node : node;
            mismatch(out, at, "offset in its parent", c.offset, d.offset);
            return false;
        }
        if (!sameFragments(c.fragment, d.fragment, node, out))
            return false;
    }
    return true;
}

// Creates a node of `kind` under `parent`, styled by `style`: an element, a replaced element or a line break.
PrLayoutNode createStyled(LayoutTree* t, PrLayoutNode parent, PrLayoutStyleIndex style, PrLayoutNodeKind kind) {
    LayoutNodeIndex at = layoutNodeIndex(t, parent);
    LayoutNodeIndex node = layoutTreeCreateNode(t, kind);
    setStyle(t, node, style);
    layoutStyleRetain(t->styles, style);
    appendNew(t, at, node);
    return layoutNodeName(t, node);
}

} // namespace

LayoutNodeIndex layoutTreeCreateNode(LayoutTree* tree, PrLayoutNodeKind kind) {
    LayoutNodeIndex node = tree->freeNodes;
    if (node != kPrLayoutNodeNone) {
        tree->freeNodes = tree->nodes.data[node].next;
    } else {
        node = tree->nodes.count;
        // A sticky box's container is named by its node with a bit that no node index reaches.
        if (node >= kLayoutContainerSticky)
            PR_FATAL("layout tree has too many nodes");
        tree->nodes.emplace();
    }
    LayoutTreeNode& n = tree->nodes.data[node];
    uint32_t generation = n.generation;
    n = LayoutTreeNode{};
    n.generation = generation;
    n.kind = kind;
    n.dirty = LayoutDirtyFlag_Self;
    n.live = true;
    if (kind == PrLayoutNodeKind_Text) {
        uint32_t entry = tree->texts.count;
        if (tree->freeTexts.count != 0) {
            entry = tree->freeTexts.last();
            tree->freeTexts.pop();
        } else {
            tree->texts.emplace() = PrRawString{};
        }
        n.style = kPrLayoutStyleNone;
        n.text = entry;
        n.childKind = LayoutChildKind_Inline;
    } else if (kind == PrLayoutNodeKind_Replaced) {
        uint32_t entry = tree->replaced.count;
        if (tree->freeReplaced.count != 0) {
            entry = tree->freeReplaced.last();
            tree->freeReplaced.pop();
        } else {
            tree->replaced.emplace();
        }
        tree->replaced.data[entry] = LayoutReplaced{};
        tree->replaced.data[entry].natural = prLayoutNaturalSizeNone();
        n.replaced = entry;
    }
    layoutCacheForget(tree->cache, node);
    return node;
}

void layoutTreeSetNewStyle(LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    LayoutTreeNode& n = nodeAt(tree, node);
    if (n.kind == PrLayoutNodeKind_Text) {
        n.style = style;
        if (style != kPrLayoutStyleNone)
            layoutStyleRetain(tree->styles, style);
        return;
    }
    setStyle(tree, node, style);
    layoutStyleRetain(tree->styles, style);
}

void layoutTreeChangeStyle(LayoutTree* tree, LayoutNodeIndex index, PrLayoutStyleIndex style) {
    tree->frame.preorderStale = true;
    LayoutTreeNode& n = nodeAt(tree, index);
    PrLayoutStyleIndex was = n.style;
    if (style == was)
        return;
    // A text node has no box: its record decides only how its text is set and painted.
    if (n.kind == PrLayoutNodeKind_Text) {
        if (style != kPrLayoutStyleNone)
            layoutStyleRetain(tree->styles, style);
        if (was != kPrLayoutStyleNone)
            layoutStyleRelease(tree->styles, was);
        n.style = style;
        markDirty(tree, index);
        nodeAt(tree, index).paintChanged = tree->paint.paints;
        markPainted(tree, index, false);
        return;
    }
    PR_ASSERT(style != kPrLayoutStyleNone);
    layoutAnchoringStyleChanged(tree, index, was, style);
    // The groups that differ decide what the change reaches.
    const PrLayoutStyle& before = layoutStyle(tree->styles, was);
    const PrLayoutStyle& after = layoutStyle(tree->styles, style);
    uint32_t changed = 0;
    for (uint32_t g = 0; g < PrLayoutGroup_Count; ++g)
        changed |= before.groups[g] != after.groups[g] ? 1u << g : 0u;
    constexpr uint32_t kFont = 1u << PrLayoutGroup_Font;
    constexpr uint32_t kBox = 1u << PrLayoutGroup_Box;
    constexpr uint32_t kFlex = 1u << PrLayoutGroup_Flex;
    constexpr uint32_t kEffect = (1u << PrLayoutGroup_Effect) | (1u << PrLayoutGroup_Compositing);
    constexpr uint32_t kTable = 1u << PrLayoutGroup_Table;
    constexpr uint32_t kGrid = 1u << PrLayoutGroup_Grid;
    constexpr uint32_t kDecoration = 1u << PrLayoutGroup_Decoration;
    const PrLayoutBoxStyle& boxBefore = layoutBoxStyle(tree->styles, was);
    const PrLayoutBoxStyle& boxAfter = layoutBoxStyle(tree->styles, style);

    // Layout reads the font, box, flex, grid and table groups, and the border styles and colors of a table and its
    // parts.
    if ((changed & kBox) != 0) {
        // A box's container reads whether its ancestors clip and whether the box that the viewport holds is fixed.
        LayoutClips clipsBefore = layoutClips(boxBefore);
        LayoutClips clipsAfter = layoutClips(boxAfter);
        if (boxBefore.position != boxAfter.position || clipsBefore.x != clipsAfter.x || clipsBefore.y != clipsAfter.y ||
                boxBefore.clipped != boxAfter.clipped)
            ++tree->cache.placed;
        // What the placeholder of a box out of flow reads, and what decides which box holds the boxes out of flow, are
        // inputs of its parent as well as its own, and may move a box to another holder.
        bool holds = boxBefore.transformed != boxAfter.transformed || boxBefore.filtered != boxAfter.filtered;
        bool holding = boxBefore.position != boxAfter.position || boxBefore.display != boxAfter.display ||
            boxBefore.staticInline != boxAfter.staticInline || holds;
        // A transform or a filter appearing or disappearing changes which box holds the boxes out of flow in its
        // subtree, and nothing else of layout. If nothing else that layout reads changed, and the last layout found no
        // box out of flow in the box's subtree, the change affects only how the box paints.
        bool onlyHolding = holding && holds && sameButHolding(tree->styles, was, style) && !mayHoldOutOfFlow(tree, index);
        if (onlyHolding) {
            // Nothing for layout to do.
        } else if (holding) {
            markDirty(tree, index, Reach_Ancestors);
            if (n.parent != kPrLayoutNodeNone)
                markDirty(tree, n.parent, Reach_Ancestors);
        } else {
            markDirty(tree, index);
        }
    } else if ((changed & (kFont | kTable)) != 0 || ((changed & kFlex) != 0 && !onlyItemFlex(tree->styles, was,
            style)) || ((changed & kGrid) != 0 && !onlyItemGrid(tree->styles, was, style)) ||
            ((changed & kDecoration) != 0 && collapsingBorders(tree->styles, boxAfter, was, style))) {
        markDirty(tree, index);
    } else if ((changed & (kFlex | kGrid)) != 0) {
        // What a flex or grid container reads of its item is the container's input, not the item's. The item is laid
        // out again only if its request changes. A box out of flow whose containing block is a grid container is placed
        // in the area that its placement names, which its holder finds when it lays the box out again.
        if (n.parent != kPrLayoutNodeNone && layoutChildrenAreItems(nodeAt(tree, n.parent).children))
            markDirty(tree, n.parent);
        if ((changed & kGrid) != 0 && layoutIsOutOfFlow(tree, index))
            markDirty(tree, index);
    }

    // Which boxes paint whole, at which levels and in which contexts, is recorded in the lists. A box's layer, level
    // and context decide the recordings around it. A box whose records can neither paint whole nor stack has none of
    // these, before or after. Whether a child is laid out as an inline box, which decides how it paints whole, reads
    // this box's writing mode, and whether the child is an item reads this box's display. A display of none, or a
    // collapsed item, takes the box's subtree out of every list.
    bool modes = (changed & kFont) != 0 &&
        layoutFontStyle(tree->styles, was).writingMode != layoutFontStyle(tree->styles, style).writingMode;
    bool items = layoutChildrenFromBox(boxBefore) != layoutChildrenFromBox(boxAfter);
    const LayoutStyleTraits& traitsBefore = layoutStyleTraits(tree->styles, was);
    const LayoutStyleTraits& traitsAfter = layoutStyleTraits(tree->styles, style);
    bool layerChanged = false;
    bool stacking = false;
    bool listed = boxBefore.display != boxAfter.display || boxBefore.collapsed != boxAfter.collapsed;
    bool contextBefore = false;
    bool contextAfter = false;
    if (traitsBefore.mayPaintWhole || traitsAfter.mayPaintWhole || traitsBefore.mayStack || traitsAfter.mayStack) {
        int32_t levelBefore = layoutStackLevelAs(tree, index, was);
        int32_t levelAfter = layoutStackLevelAs(tree, index, style);
        contextBefore = layoutIsStackingContextAs(tree, index, was);
        contextAfter = layoutIsStackingContextAs(tree, index, style);
        layerChanged = layoutHasLayerAs(tree, index, was) != layoutHasLayerAs(tree, index, style);
        // A stack level, or whether the box is a stacking context, orders the boxes of the stacking context around it,
        // so the recording that names the box changes, whether or not anything is laid out. A box that becomes or stops
        // being a stacking context changes no list if no box in its subtree paints whole. Whether a box blends decides
        // whether the stacking context around it takes a layer to isolate it (layoutIsolatesBlending), which the
        // recording around that context names.
        stacking = levelBefore != levelAfter || contextBefore != contextAfter ||
            layoutStyleBlends(tree->styles, was) != layoutStyleBlends(tree->styles, style);
        listed = listed || levelBefore != levelAfter ||
            layoutNodePaintsWholeAs(tree, index, was) != layoutNodePaintsWholeAs(tree, index, style) ||
            (contextBefore != contextAfter && layoutNodeLeadsWhole(tree, index));
    }

    // A recording reads a box's record except its effects. Effects apply around the recording of a box with a layer of
    // its own, and decide only whether a box has one. A clip rectangle is the clip of the box's container, which the
    // recording around the box holds.
    bool clipMoved = (changed & (1u << PrLayoutGroup_Compositing)) != 0 && (boxBefore.clipped || boxAfter.clipped) &&
        !prMemEquals(layoutCompositingStyle(tree->styles, was).clip,
            layoutCompositingStyle(tree->styles, style).clip, sizeof(PrLayoutLength) * 4);
    if ((changed & ~kEffect) != 0 || clipMoved) {
        n.paintChanged = tree->paint.paints;
        markPainted(tree, index, layerChanged || stacking);
    } else if (layerChanged || stacking) {
        markPainted(tree, index, true);
    }
    // Effects move boxes without a layout: a transform moves where they are, and a layer of its own moves the order in
    // which they paint. Every other change that moves a box lays the tree out again.
    if ((changed & kEffect) != 0) {
        tree->pendingPaint |= LayoutInputFlag_Effects;
        ++tree->effects;
    }
    // The lists drop the boxes that the change moves under the style before it, and take them again under the style
    // after it. The children are moved separately if the node's own move does not reach them: if it is a stacking
    // context, whose children are in its own list, or if it stays in its list.
    bool childrenBefore = (modes || items) && (!listed || contextBefore);
    bool childrenAfter = (modes || items) && (!listed || contextAfter);
    if (listed)
        layoutStackingRemoving(tree, index);
    for (LayoutNodeIndex child = n.first; childrenBefore && child != kPrLayoutNodeNone; child =
            nodeAt(tree, child).next)
        layoutStackingRemoving(tree, child);
    layoutStyleRetain(tree->styles, style);
    layoutStyleRelease(tree->styles, was);
    setStyle(tree, index, style);
    // Whether the box is out of flow, or generates a box at all, ends the runs of text around it in a flex or grid
    // container, and the container's font is the font of its anonymous items.
    if (n.parent != kPrLayoutNodeNone &&
            (boxBefore.display != boxAfter.display || boxBefore.position != boxAfter.position))
        markRunsAround(tree, n.parent, n.previous, n.next);
    if ((changed & kFont) != 0 && !items && layoutChildrenAreItems(n.children)) {
        for (LayoutNodeIndex child = n.first; child != kPrLayoutNodeNone; child = nodeAt(tree, child).next) {
            if (layoutIsRunContent(nodeAt(tree, child)))
                markDirty(tree, child);
        }
    }
    if (listed)
        layoutStackingInserted(tree, index);
    for (LayoutNodeIndex child = n.first; items && child != kPrLayoutNodeNone; child = nodeAt(tree, child).next)
        markDirty(tree, child);
    // A child whose justify-self is auto takes the box's justify-items.
    if ((changed & kFlex) != 0 &&
            layoutFlexStyle(tree->styles, was).justifyItems != layoutFlexStyle(tree->styles, style).justifyItems) {
        for (LayoutNodeIndex child = n.first; child != kPrLayoutNodeNone; child = nodeAt(tree, child).next) {
            const LayoutTreeNode& c = nodeAt(tree, child);
            if (layoutKindIsElement(c.kind) &&
                    (layoutFlexStyle(tree->styles, c.style).justifySelf & kPrLayoutAlignmentValueMask) ==
                    PrLayoutAlignment_Auto)
                markDirty(tree, child);
        }
    }
    for (LayoutNodeIndex child = n.first; childrenAfter && child != kPrLayoutNodeNone; child = nodeAt(tree, child).next)
        layoutStackingInserted(tree, child);
}

void layoutTreeInsert(LayoutTree* tree, LayoutNodeIndex parentIndex, LayoutNodeIndex index, LayoutNodeIndex beforeIndex) {
    tree->frame.preorderStale = true;
    LayoutTreeNode& p = nodeAt(tree, parentIndex);
    LayoutTreeNode& n = nodeAt(tree, index);
    PR_ASSERT(layoutKindIsElement(p.kind) && n.parent == kPrLayoutNodeNone && tree->root != index &&
        !layoutTreeIsLaidOutRoot(tree, index));
    PR_ASSERT(beforeIndex == kPrLayoutNodeNone || nodeAt(tree, beforeIndex).parent == parentIndex);
    markChildren(tree, parentIndex, index);
    n.parent = parentIndex;
    n.next = beforeIndex;
    if (beforeIndex == kPrLayoutNodeNone) {
        n.previous = p.last;
        if (p.last != kPrLayoutNodeNone)
            nodeAt(tree, p.last).next = index;
        else
            p.first = index;
        p.last = index;
    } else {
        LayoutTreeNode& b = nodeAt(tree, beforeIndex);
        n.previous = b.previous;
        if (b.previous != kPrLayoutNodeNone)
            nodeAt(tree, b.previous).next = index;
        else
            p.first = index;
        b.previous = index;
    }
    markRunNeighbours(tree, index);
    layoutStackingInserted(tree, index);
}

void layoutTreeRemove(LayoutTree* tree, LayoutNodeIndex index) {
    tree->frame.preorderStale = true;
    LayoutTreeNode& n = nodeAt(tree, index);
    LayoutNodeIndex parent = n.parent;
    LayoutNodeIndex previous = n.previous;
    LayoutNodeIndex next = n.next;
    if (parent != kPrLayoutNodeNone)
        markChildren(tree, parent, index);
    // Content removed from a flex or grid container keeps nothing of its run's anonymous item.
    if (parent != kPrLayoutNodeNone && layoutChildrenAreItems(nodeAt(tree, parent).children) && layoutIsRunContent(n))
        layoutCacheInvalidate(tree->cache, index);
    layoutStackingRemoving(tree, index);
    unlink(tree, index);
    markRunsAround(tree, parent, previous, next);
}

void layoutTreeSetKey(LayoutTree* tree, LayoutNodeIndex index, uint64_t key) {
    tree->frame.preorderStale = true;
    LayoutTreeNode& n = nodeAt(tree, index);
    if (n.key == key)
        return;
    if (n.key != 0)
        tree->keys.erase(KeyIndex::hashOf(n.key), index);
    n.key = key;
    if (key == 0)
        return;
    PR_ASSERT(prLayoutNodeFromKey(static_cast<PrLayoutTree*>(tree), key) == kPrLayoutNodeNone);
    tree->keys.insert(KeyIndex::hashOf(key), index);
}

void layoutTreeRepaintContent(LayoutTree* tree, LayoutNodeIndex replaced, uint32_t paints) {
    LayoutReplaced& r = layoutReplaced(tree, replaced);
    prDrawListRelease(r.content);
    r.content = nullptr;
    nodeAt(tree, replaced).paintChanged = paints;
    for (uint32_t at = replaced; at != kPrLayoutNodeNone;) {
        LayoutTreeNode& n = nodeAt(tree, at);
        if (at != replaced && n.paintSubtree >= paints)
            break;
        n.paintSubtree = paints;
        if (layoutHasLayer(tree, at))
            break;
        at = n.parent;
    }
}

bool layoutTreeIsLaidOutRoot(const LayoutTree* tree, LayoutNodeIndex index) {
    for (uint32_t i = 0; i < tree->roots.count; ++i) {
        if (tree->roots.data[i].node == index)
            return true;
    }
    return false;
}

void layoutTreeEraseDetached(LayoutTree* tree, LayoutNodeIndex node) {
    PR_ASSERT(nodeAt(tree, node).parent == kPrLayoutNodeNone);
    freeSubtree(tree, node);
}

void layoutTreeReadViewport(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutTreeNode& n = nodeAt(tree, node);
    if (n.readsViewport)
        return;
    n.readsViewport = true;
    tree->viewportReaders.push(node);
}

void layoutTreeForgetLayout(LayoutTree* tree) {
    tree->pendingLayout |= LayoutInputFlag_Layout;
    layoutTreeForgetPages(tree);
    for (uint32_t i = 0; i < tree->nodes.count; ++i) {
        if (tree->nodes.data[i].live)
            layoutCacheForget(tree->cache, i);
    }
    if (tree->root != kPrLayoutNodeNone)
        markDirty(tree, tree->root);
}

void layoutTreeForgetPages(LayoutTree* tree) {
    layoutFragmentRelease(tree->pageStack);
    tree->pageStack = nullptr;
}

const LayoutHighlights* PR_NULLABLE layoutTextHighlights(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!n.listed || n.kind != PrLayoutNodeKind_Text)
        return nullptr;
    uint32_t at = tree->highlightIndex.find(layoutHighlightHash(node),
        [&](uint32_t i) { return tree->highlights.data[i].node == node; });
    return at != kHashMissing ? &tree->highlights.data[at] : nullptr;
}

void layoutHighlightsReaching(const LayoutHighlights* highlights, uint32_t start, uint32_t end, RawArray<uint32_t>& out) {
    out.clear();
    const RawArray<uint32_t>& order = highlights->byStart;
    const PrLayoutHighlight* ranges = highlights->ranges.data;
    // The ranges that begin before `end` come first in the order, and those before the first whose reach passes `start`
    // all end at or before it.
    uint32_t last = partitionPoint(0, order.count, [&](uint32_t i) { return ranges[order.data[i]].start < end; });
    uint32_t first = partitionPoint(0, last, [&](uint32_t i) { return highlights->reach.data[i] <= start; });
    for (uint32_t i = first; i < last; ++i) {
        if (ranges[order.data[i]].end > start)
            out.push(order.data[i]);
    }
    sortItems(out.span(), [](uint32_t a, uint32_t b) { return a < b; });
}

void layoutTreeSetHighlights(LayoutTree* tree, LayoutNodeIndex node, const PrLayoutHighlight* PR_NULLABLE highlights,
        uint32_t count, uint32_t frame) {
    LayoutTreeNode& n = nodeAt(tree, node);
    PR_ASSERT(n.kind == PrLayoutNodeKind_Text);
    LayoutHighlights* kept = const_cast<LayoutHighlights*>(layoutTextHighlights(tree, node));
    if (kept != nullptr) {
        kept->frame = frame;
        if (kept->ranges.count == count && prMemEquals(kept->ranges.data, highlights, sizeof(PrLayoutHighlight) * count))
            return;
    }
    if (count == 0) {
        if (kept == nullptr)
            return;
        dropHighlights(tree, node);
    } else if (kept != nullptr) {
        kept->ranges.clear();
        kept->ranges.append(highlights, count);
        indexHighlights(*kept);
    } else {
        tree->highlightIndex.insert(layoutHighlightHash(node), tree->highlights.count);
        LayoutHighlights& added = tree->highlights.emplace();
        added = LayoutHighlights{.node = node, .frame = frame};
        added.ranges.append(highlights, count);
        indexHighlights(added);
        n.listed = true;
    }
    markPainted(tree, node, false);
}

void layoutTreeSetCarets(LayoutTree* tree, const PrLayoutCaret* PR_NULLABLE carets, uint32_t count, uint32_t frame) {
    LayoutTreeCarets& kept = tree->carets;
    kept.frame = frame;
    // A caret has padding, so carets are compared by their fields.
    bool same = kept.carets.count == count;
    for (uint32_t i = 0; same && i < count; ++i) {
        const PrLayoutCaret& a = kept.carets.data[i];
        const PrLayoutCaret& b = carets[i];
        same = a.position.node == b.position.node && a.position.offset == b.position.offset &&
            a.affinity == b.affinity && prMemEquals(&a.color, &b.color, sizeof(PrColor16));
    }
    if (same)
        return;
    // The recordings of the text that a caret leaves and of the text that it comes to change.
    auto mark = [&](PrLayoutNode node) {
        if (prLayoutNodeIsValid(static_cast<PrLayoutTree*>(tree), node))
            markPainted(tree, layoutNodeIndex(tree, node), false);
    };
    for (uint32_t i = 0; i < kept.carets.count; ++i)
        mark(kept.carets.data[i].position.node);
    kept.carets.clear();
    for (uint32_t i = 0; i < count; ++i) {
        kept.carets.push(carets[i]);
        mark(carets[i].position.node);
    }
}

void layoutTreeExpireFrameMarks(LayoutTree* tree, uint32_t frame) {
    for (uint32_t i = tree->highlights.count; i-- > 0;) {
        const LayoutHighlights& kept = tree->highlights.data[i];
        if (kept.frame != 0 && kept.frame != frame)
            layoutTreeSetHighlights(tree, kept.node, nullptr, 0, 0);
    }
    if (tree->carets.carets.count != 0 && tree->carets.frame != 0 && tree->carets.frame != frame)
        layoutTreeSetCarets(tree, nullptr, 0, 0);
}

PrLayoutStyleIndex layoutFirstLineStyle(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!n.listed || n.kind == PrLayoutNodeKind_Text)
        return kPrLayoutStyleNone;
    uint32_t at = tree->firstLineIndex.find(layoutFirstLineHash(node),
        [&](uint32_t i) { return tree->firstLines.data[i].node == node; });
    return at != kHashMissing ? tree->firstLines.data[at].style : kPrLayoutStyleNone;
}

uint32_t layoutNodeLineStyle(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    LayoutNodeIndex styled = n.kind == PrLayoutNodeKind_Text && n.style == kPrLayoutStyleNone ? n.parent : node;
    PrLayoutStyleIndex line = kPrLayoutStyleNone;
    if (styled != kPrLayoutNodeNone && (n.kind != PrLayoutNodeKind_Text || styled != node))
        line = layoutFirstLineStyle(tree, styled);
    return line != kPrLayoutStyleNone ? line : layoutNodeStyle(tree, node);
}

} // namespace pr_impl

using namespace pr_impl;

PrLayoutTree* prLayoutTreeCreate(const PrLayoutTreeDesc* desc) {
    PrLayoutTree* tree = allocZeroed<PrLayoutTree>();
    tree->styles = layoutStylesCreate(desc->font, desc->shaper, desc->targetPixelRatio);
    tree->nodes.emplace() = LayoutTreeNode{};
    layoutCacheInit(tree->cache);
    return tree;
}

void prLayoutTreeDestroy(PrLayoutTree* PR_NULLABLE tree) {
    LayoutTree* t = tree;
    if (t == nullptr)
        return;
    for (uint32_t i = 1; i < t->nodes.count; ++i) {
        LayoutTreeNode& n = t->nodes.data[i];
        if (!n.live)
            continue;
        releaseKind(t, n);
    }
    t->nodes.reset();
    t->firstLines.reset();
    t->firstLineIndex.reset();
    for (uint32_t i = 0; i < t->highlights.count; ++i) {
        t->highlights.data[i].ranges.reset();
        t->highlights.data[i].byStart.reset();
        t->highlights.data[i].reach.reset();
    }
    t->highlights.reset();
    t->carets.carets.reset();
    t->highlightIndex.reset();
    t->replaced.reset();
    t->freeReplaced.reset();
    t->texts.reset();
    t->freeTexts.reset();
    t->keys.reset();
    layoutFrameReset(t->frame);
    t->viewportReaders.reset();
    t->viewportPainters.reset();
    layoutFragmentRelease(t->fragment);
    for (uint32_t i = 0; i < t->roots.count; ++i) {
        layoutFragmentRelease(t->roots.data[i].fragment);
        prDrawListRelease(t->roots.data[i].drawing);
    }
    t->roots.reset();
    t->pages.reset();
    layoutTreeForgetPages(t);
    t->pageMargins.reset();
    t->runningPages.reset();
    t->rootReaders.reset();
    layoutAnchoringReset(t->anchoring);
    layoutCacheReset(t->cache);
    for (uint32_t i = 0; i < t->shapings.count; ++i)
        layoutShapingDestroy(t->shapings.data[i]);
    t->shapings.reset();
    t->changedFonts.reset();
    layoutScratchDestroy(t->scratch);
    prLayoutSceneRelease(t->scene);
    layoutPaintCacheReset(t->paint);
    layoutStackingReset(t->stacking);
    layoutQueriesReset(t->queries);
    layoutStylesDestroy(t->styles);
    prMemoryFree(t);
}

void prLayoutTreeSetTargetPixelRatio(PrLayoutTree* tree, float ratio) {
    if (ratio != tree->styles->targetPixelRatio)
        tree->pendingPaint |= LayoutInputFlag_Paint;
    layoutStylesSetTargetPixelRatio(tree->styles, ratio);
}

float prLayoutTreeTargetPixelRatio(const PrLayoutTree* tree) {
    return tree->styles->targetPixelRatio;
}

void prLayoutTreeSetCanvasColor(PrLayoutTree* tree, PrColor16 color) {
    if (prMemEquals(&color, &tree->canvasColor, sizeof color))
        return;
    tree->canvasColor = color;
    tree->pendingPaint |= LayoutInputFlag_Paint;
}

PrColor16 prLayoutTreeCanvasColor(const PrLayoutTree* tree) {
    return tree->canvasColor;
}

void prLayoutTreeSetMarking(PrLayoutTree* tree, PrLayoutMarking marking) {
    if (marking == tree->marking)
        return;
    tree->marking = marking;
    // Every recording marks or not as the tree did when it was made, so none is current.
    uint32_t paints = tree->paint.paints;
    layoutPaintCacheReset(tree->paint);
    tree->paint.paints = paints;
    tree->pendingPaint |= LayoutInputFlag_Paint;
}

PrLayoutMarking prLayoutTreeMarking(const PrLayoutTree* tree) {
    return tree->marking;
}

void prLayoutTreeSetViewportWheelEvents(PrLayoutTree* tree, PrLayoutWheelEvents events) {
    if (events == tree->viewportWheelEvents)
        return;
    tree->viewportWheelEvents = events;
    tree->pendingPaint |= LayoutInputFlag_Paint;
}

PrLayoutWheelEvents prLayoutTreeViewportWheelEvents(const PrLayoutTree* tree) {
    return tree->viewportWheelEvents;
}

PrTextFont* PR_NULLABLE prLayoutTreeDefaultFont(const PrLayoutTree* tree) {
    return tree->styles->defaultFont;
}

PrLayoutNode prLayoutNodeCreateElement(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style) {
    return createStyled(tree, parent, style, PrLayoutNodeKind_Element);
}

PrLayoutNode prLayoutNodeCreateText(PrLayoutTree* tree, PrLayoutNode parent, PrStringView text) {
    LayoutTree* t = tree;
    LayoutNodeIndex at = layoutNodeIndex(t, parent);
    LayoutNodeIndex node = layoutTreeCreateNode(t, PrLayoutNodeKind_Text);
    t->texts.data[t->nodes.data[node].text] = prRawStringCopy(text);
    appendNew(t, at, node);
    return layoutNodeName(t, node);
}

PrLayoutNode prLayoutNodeCreateReplaced(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style) {
    return createStyled(tree, parent, style, PrLayoutNodeKind_Replaced);
}

PrLayoutNode prLayoutNodeCreateSemiReplaced(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style) {
    return createStyled(tree, parent, style, PrLayoutNodeKind_SemiReplaced);
}

PrLayoutNode prLayoutNodeCreateLineBreak(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutStyleIndex style) {
    return createStyled(tree, parent, style, PrLayoutNodeKind_LineBreak);
}

PrLayoutNode prLayoutTreeCopy(PrLayoutTree* tree, PrLayoutNode node, PrLayoutNode parent) {
    LayoutTree* t = tree;
    PR_ASSERT(!t->frame.open);
    PrLayoutNode copy = kPrLayoutNodeNone;
    const LayoutTreeNode& n = nodeAt(t, layoutNodeIndex(t, node));
    switch (n.kind) {
        case PrLayoutNodeKind_Text: {
            copy = prLayoutNodeCreateText(tree, parent, prLayoutNodeText(tree, node));
            PrLayoutStyleIndex style = prLayoutNodeStyle(tree, node);
            if (style != kPrLayoutStyleNone)
                prLayoutNodeSetStyle(tree, copy, style);
            break;
        }
        case PrLayoutNodeKind_Replaced: {
            copy = createStyled(tree, parent, n.style, n.kind);
            prLayoutNodeSetNaturalSize(tree, copy, prLayoutNodeNaturalSize(tree, node));
            PrLayoutContent content = prLayoutNodeContent(tree, node);
            prLayoutNodeSetContent(tree, copy, &content);
            break;
        }
        default: copy = createStyled(tree, parent, n.style, n.kind); break;
    }
    LayoutNodeIndex index = layoutNodeIndex(t, node);
    nodeAt(t, layoutNodeIndex(t, copy)).rendered = nodeAt(t, index).rendered;
    if (layoutKindIsElement(nodeAt(t, index).kind)) {
        PrLayoutStyleIndex firstLine = layoutFirstLineStyle(t, index);
        if (firstLine != kPrLayoutStyleNone)
            prLayoutNodeSetFirstLineStyle(tree, copy, firstLine);
    }
    for (LayoutNodeIndex child = nodeAt(t, index).first; child != kPrLayoutNodeNone; child = nodeAt(t, child).next)
        prLayoutTreeCopy(tree, layoutNodeName(t, child), copy);
    return copy;
}

void prLayoutTreeErase(PrLayoutTree* tree, PrLayoutNode node) {
    PR_ASSERT(!tree->frame.open);
    LayoutTree* t = tree;
    LayoutNodeIndex index = layoutNodeIndex(t, node);
    LayoutTreeNode& n = nodeAt(t, index);
    LayoutNodeIndex parent = n.parent;
    LayoutNodeIndex previous = n.previous;
    LayoutNodeIndex next = n.next;
    if (parent != kPrLayoutNodeNone)
        markChildren(t, parent, index);
    layoutStackingRemoving(t, index);
    unlink(t, index);
    markRunsAround(t, parent, previous, next);
    freeSubtree(t, index);
}

bool prLayoutNodeIsValid(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutNodeIsValid(tree, node);
}

PrLayoutNodeKind prLayoutNodeKind(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutTreeNode(tree, layoutNodeIndex(tree, node)).kind;
}

PrLayoutNode prLayoutTreeRoot(const PrLayoutTree* tree) {
    return layoutNodeName(tree, tree->root);
}

void prLayoutTreeInsert(PrLayoutTree* tree, PrLayoutNode parent, PrLayoutNode node, PrLayoutNode before) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeInsert(tree, layoutNodeIndex(tree, parent), layoutNodeIndex(tree, node), layoutNodeIndex(tree, before));
}

void prLayoutTreeRemove(PrLayoutTree* tree, PrLayoutNode node) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeRemove(tree, layoutNodeIndex(tree, node));
}

PrLayoutNode prLayoutNodeParent(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    return layoutNodeName(t, layoutTreeNode(t, layoutNodeIndex(t, node)).parent);
}

PrLayoutNode prLayoutNodeFirstChild(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    return layoutNodeName(t, layoutTreeNode(t, layoutNodeIndex(t, node)).first);
}

PrLayoutNode prLayoutNodeLastChild(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    return layoutNodeName(t, layoutTreeNode(t, layoutNodeIndex(t, node)).last);
}

PrLayoutNode prLayoutNodePreviousSibling(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    return layoutNodeName(t, layoutTreeNode(t, layoutNodeIndex(t, node)).previous);
}

PrLayoutNode prLayoutNodeNextSibling(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    return layoutNodeName(t, layoutTreeNode(t, layoutNodeIndex(t, node)).next);
}

void prLayoutNodeSetStyle(PrLayoutTree* tree, PrLayoutNode node, PrLayoutStyleIndex style) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeChangeStyle(tree, layoutNodeIndex(tree, node), style);
}

void prLayoutNodeSetText(PrLayoutTree* tree, PrLayoutNode text, PrStringView value) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, text);
    LayoutTreeNode& n = nodeAt(t, index);
    PR_ASSERT(n.kind == PrLayoutNodeKind_Text);
    if (prRawStringView(&t->texts.data[n.text]) == value)
        return;
    prRawStringReplace(&t->texts.data[n.text], value);
    markDirty(t, index);
    markPainted(t, index, false);
}

void prLayoutNodeSetFirstLineStyle(PrLayoutTree* tree, PrLayoutNode element, PrLayoutStyleIndex style) {
    LayoutTree* t = tree;
    PR_ASSERT(!t->frame.open);
    uint32_t index = layoutNodeIndex(t, element);
    LayoutTreeNode& n = nodeAt(t, index);
    PR_ASSERT(layoutKindIsElement(n.kind));
    if (layoutFirstLineStyle(t, index) == style)
        return;
    uint32_t hash = layoutFirstLineHash(index);
    if (n.listed) {
        uint32_t at = t->firstLineIndex.find(hash, [&](uint32_t i) { return t->firstLines.data[i].node == index; });
        PrLayoutStyleIndex was = t->firstLines.data[at].style;
        if (style != kPrLayoutStyleNone) {
            layoutStyleRetain(t->styles, style);
            t->firstLines.data[at].style = style;
            layoutStyleRelease(t->styles, was);
        } else {
            dropFirstLine(t, index);
        }
    } else if (style != kPrLayoutStyleNone) {
        layoutStyleRetain(t->styles, style);
        t->firstLineIndex.insert(hash, t->firstLines.count);
        t->firstLines.push(LayoutFirstLine{index, style});
        n.listed = true;
    }
    // The node's inline formatting context, and the context it is the container of, are collected again.
    markDirty(t, index);
    for (LayoutNodeIndex c = n.first; c != kPrLayoutNodeNone; c = nodeAt(t, c).next)
        markDirty(t, c);
    nodeAt(t, index).paintChanged = t->paint.paints;
    markPainted(t, index, false);
}

uint32_t prLayoutNodeStyle(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutTreeNode(tree, layoutNodeIndex(tree, node)).style;
}

PrStringView prLayoutNodeText(const PrLayoutTree* tree, PrLayoutNode text) {
    return layoutNodeText(tree, layoutNodeIndex(tree, text));
}

void prLayoutNodeSetHighlights(PrLayoutTree* tree, PrLayoutNode text, const PrLayoutHighlight* PR_NULLABLE highlights,
        uint32_t count) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeSetHighlights(tree, layoutNodeIndex(tree, text), highlights, count, 0);
}

uint32_t prLayoutNodeHighlights(const PrLayoutTree* tree, PrLayoutNode text, PrLayoutHighlight* PR_NULLABLE out,
        uint32_t capacity) {
    const LayoutHighlights* kept = layoutTextHighlights(tree, layoutNodeIndex(tree, text));
    if (kept == nullptr)
        return 0;
    for (uint32_t i = 0; i < kept->ranges.count && i < capacity; ++i)
        out[i] = kept->ranges.data[i];
    return kept->ranges.count;
}

void prLayoutTreeSetCarets(PrLayoutTree* tree, const PrLayoutCaret* PR_NULLABLE carets, uint32_t count) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeSetCarets(tree, carets, count, 0);
}

uint32_t prLayoutTreeCarets(const PrLayoutTree* tree, PrLayoutCaret* PR_NULLABLE out, uint32_t capacity) {
    const RawArray<PrLayoutCaret>& carets = tree->carets.carets;
    for (uint32_t i = 0; i < carets.count && i < capacity; ++i)
        out[i] = carets.data[i];
    return carets.count;
}

void prLayoutTreeSetCaretsHidden(PrLayoutTree* tree, bool hidden) {
    if (tree->carets.hidden == hidden)
        return;
    tree->carets.hidden = hidden;
    tree->pendingPaint |= LayoutInputFlag_Effects;
}

bool prLayoutTreeCaretsHidden(const PrLayoutTree* tree) {
    return tree->carets.hidden;
}

void prLayoutNodeSetRendered(PrLayoutTree* tree, PrLayoutNode node, PrLayoutRenderedFlags flags) {
    tree->nodes.data[layoutNodeIndex(tree, node)].rendered = flags;
}

void prLayoutNodeSetKey(PrLayoutTree* tree, PrLayoutNode node, uint64_t key) {
    PR_ASSERT(!tree->frame.open);
    layoutTreeSetKey(tree, layoutNodeIndex(tree, node), key);
}

uint64_t prLayoutNodeKey(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutTreeNode(tree, layoutNodeIndex(tree, node)).key;
}

PrLayoutNode prLayoutNodeFromKey(const PrLayoutTree* tree, uint64_t key) {
    const LayoutTree* t = tree;
    if (key == 0)
        return kPrLayoutNodeNone;
    uint32_t index = t->keys.find(KeyIndex::hashOf(key), [&](uint32_t at) { return t->nodes.data[at].key == key; });
    return index != kHashMissing ? layoutNodeName(t, index) : kPrLayoutNodeNone;
}

void prLayoutNodeSetNaturalSize(PrLayoutTree* tree, PrLayoutNode replaced, PrLayoutNaturalSize size) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, replaced);
    LayoutReplaced& r = layoutReplaced(t, index);
    if (r.natural.width == size.width && r.natural.height == size.height && r.natural.ratio == size.ratio)
        return;
    r.natural = size;
    markDirty(t, index);
    // Its content is placed by its natural size.
    nodeAt(t, index).paintChanged = t->paint.paints;
    markPainted(t, index, false);
}

void prLayoutNodeSetContent(PrLayoutTree* tree, PrLayoutNode replaced, const PrLayoutContent* content) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, replaced);
    LayoutReplaced& r = layoutReplaced(t, index);
    PrDrawList* list = content->kind == PrLayoutContentKind_DrawList ? content->list : nullptr;
    PR_ASSERT(content->kind != PrLayoutContentKind_DrawList || list != nullptr);
    if (list != nullptr) {
        prDrawListFreeze(list);
        prDrawListRetain(list);
    }
    const PrDrawImage* image = content->kind == PrLayoutContentKind_Image ? content->image : nullptr;
    PR_ASSERT(content->kind != PrLayoutContentKind_Image || image != nullptr);
    if (image != nullptr)
        prDrawImageRetain(image);
    prDrawImageRelease(r.image);
    r.image = image;
    PrLayoutPainter painter = content->kind == PrLayoutContentKind_Painter ? content->painter : PrLayoutPainter{};
    PR_ASSERT(content->kind != PrLayoutContentKind_Painter || painter.paint != nullptr);
    prLayoutPainterRetain(painter);
    prLayoutPainterRelease(r.painter);
    r.painter = painter;
    r.limit = image != nullptr ? content->limit : 0;
    r.rendering = image != nullptr ? content->rendering : PrLayoutImageRendering_Auto;
    prDrawListRelease(r.content);
    r.contentKind = content->kind;
    r.content = list;
    r.embed = content->kind == PrLayoutContentKind_Embed ? content->embed : 0;
    r.revision = content->revision;
    r.text = content->text;
    r.textContext = content->textContext;
    PR_ASSERT(content->kind != PrLayoutContentKind_Embed || r.embed != 0);
    nodeAt(t, index).paintChanged = t->paint.paints;
    markPainted(t, index, false);
}

PrLayoutNaturalSize prLayoutNodeNaturalSize(const PrLayoutTree* tree, PrLayoutNode replaced) {
    return layoutReplaced(tree, layoutNodeIndex(tree, replaced)).natural;
}

PrLayoutContent prLayoutNodeContent(const PrLayoutTree* tree, PrLayoutNode replaced) {
    const LayoutReplaced& n = layoutReplaced(tree, layoutNodeIndex(tree, replaced));
    PrLayoutContent content{};
    content.kind = n.contentKind;
    content.revision = n.revision;
    content.text = n.text;
    content.textContext = n.textContext;
    if (n.contentKind == PrLayoutContentKind_DrawList) {
        content.list = n.content;
    } else if (n.contentKind == PrLayoutContentKind_Painter) {
        content.painter = n.painter;
    } else if (n.contentKind == PrLayoutContentKind_Embed) {
        content.embed = n.embed;
    } else if (n.contentKind == PrLayoutContentKind_Image) {
        content.image = n.image;
        content.limit = n.limit;
        content.rendering = n.rendering;
    }
    return content;
}

void prLayoutNodeInvalidateContent(PrLayoutTree* tree, PrLayoutNode replaced) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, replaced);
    LayoutReplaced& r = layoutReplaced(t, index);
    if (r.contentKind != PrLayoutContentKind_Painter)
        return;
    prDrawListRelease(r.content);
    r.content = nullptr;
    nodeAt(t, index).paintChanged = t->paint.paints;
    markPainted(t, index, false);
}

void prLayoutNodeSetScroll(PrLayoutTree* tree, PrLayoutNode element, PrLayoutVec offset) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, element);
    if (index == kPrLayoutNodeNone) {
        t->viewportScroll = offset;
    } else {
        PR_ASSERT(layoutKindIsElement(nodeAt(t, index).kind));
        nodeAt(t, index).scroll = offset;
    }
    layoutAnchoringScrolled(t, index, offset);
}

PrLayoutVec prLayoutNodeScroll(const PrLayoutTree* tree, PrLayoutNode element) {
    return layoutNodeScroll(tree, layoutNodeIndex(tree, element));
}

uint32_t prLayoutTreeTakeScrollAdjustments(PrLayoutTree* tree, PrLayoutScrollAdjustment* PR_NULLABLE out,
        uint32_t capacity) {
    RawArray<PrLayoutScrollAdjustment>& untaken = tree->anchoring.untaken;
    uint32_t count = untaken.count < capacity ? untaken.count : capacity;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = untaken.data[i];
    untaken.erase(0, count);
    return count;
}

void prLayoutTreeLayout(PrLayoutTree* tree, const PrLayoutRoot* roots, uint32_t count) {
    PR_ASSERT(tree->styles->targetPixelRatio > 0);
    PR_ASSERT(count >= 1);
    LayoutTree* t = tree;
    t->stats = PrLayoutTreeStats{};
    t->stacking.listsFound = 0;
    if (t->frame.open)
        layoutFrameCommit(t);
    PrLayoutVec extent = roots[0].extent;
    if (extent != t->viewport)
        t->pendingLayout |= LayoutInputFlag_Layout;
    // A root that the last layout did not lay out is laid out whole, and is the stacking context of its subtree. The
    // common case, the viewport's root alone as the last layout laid it out, takes nothing.
    bool same = count == 1 && t->roots.count == 0 && roots[0].node == t->rootName;
    bool rootsChanged = !same && takeRoots(t, roots, count);
    if (rootsChanged)
        t->pendingLayout |= LayoutInputFlag_Layout;
    // Frees the records and groups that nothing names before layout reads them.
    layoutStylesCollect(t->styles);
    // A group whose text font's answers changed sets its text in other faces, with other metrics. The shapings that
    // name the group shape again, and the nodes it styles lay out again, because their struts, baselines and lengths
    // read its faces.
    if (layoutSyncFonts(t->styles, &t->changedFonts))
        markFontReaders(t);
    // Every laid-out box holds font metrics, which a new target pixel ratio changes and no mark reaches. The boxes are
    // therefore laid out again, and each result is cached for the next layout. A shaping holds no metrics, and stays
    // cached unless a font that it is set in hints its advances, which the ratio changes.
    bool metricsChanged = t->metricsGeneration != t->styles->metricsGeneration;
    if (metricsChanged) {
        t->metricsGeneration = t->styles->metricsGeneration;
        forgetHintedShapings(t);
        layoutTreeForgetPages(t);
        t->pendingLayout |= LayoutInputFlag_Layout;
        for (uint32_t i = 0; i < t->nodes.count; ++i) {
            if (t->nodes.data[i].live)
                layoutCacheForget(t->cache, i);
        }
    }
    t->laidOutFor = t->pendingLayout;
    t->pendingLayout = 0;
    // A change under the root marks the root, so a clean root in the same viewport, at the same metrics, keeps the last
    // layout's fragments. A change outside the root's tree waits for the insert that brings it in, which marks it.
    bool clean = t->fragment != nullptr && extent == t->viewport && !metricsChanged && !rootsChanged &&
        t->root != kPrLayoutNodeNone && nodeAt(t, t->root).dirty == 0;
    for (uint32_t i = 0; clean && i < t->roots.count; ++i)
        clean = t->roots.data[i].fragment != nullptr && nodeAt(t, t->roots.data[i].node).dirty == 0;
    if (clean) {
        layoutAnchoringIdle(t);
        return;
    }
    layoutAnchoringChoose(t);
    // What read the viewport's extent is laid out again, because no space carries the extent for the caches to key on.
    if (extent != t->viewport) {
        markViewportReaders(t);
        markViewportPainters(t);
    }
    LayoutState state{
        .tree = t,
        .styles = t->styles,
        .scratch = scratchOf(t),
        .stats = &t->stats,
        .viewport = extent,
        .reuse = true,
    };
    layoutShapeAhead(state, t->root);
    for (uint32_t i = 0; i < t->roots.count; ++i)
        layoutShapeAhead(state, t->roots.data[i].node);
    const LayoutFragment* fragment = layoutViewport(state, t->root, extent);
    // Each other root is laid out in its containing block as the viewport's root is in the viewport.
    bool rootMoved = false;
    for (uint32_t i = 0; i < t->roots.count; ++i) {
        LayoutLaidRoot& root = t->roots.data[i];
        const LayoutFragment* laid = layoutViewport(state, root.node, root.extent);
        rootMoved = rootMoved || laid != root.fragment;
        if (root.fragment != nullptr && laid != root.fragment)
            layoutCacheRetire(t->cache, root.fragment);
        layoutFragmentRelease(root.fragment);
        root.fragment = laid;
    }
    layoutEndLayout(state);
    // The final tree is published once it is whole.
    t->cache.published = 0;
    layoutCachePlace(t->cache, kPrLayoutNodeNone, fragment);
    for (uint32_t i = 0; i < t->roots.count; ++i)
        layoutCachePlace(t->cache, kPrLayoutNodeNone, t->roots.data[i].fragment);
    t->stats.childrenPublished = t->cache.published;
    // The root's fragment, and those of the boxes that the viewport holds, are the old ones if nothing in them changed.
    // The old fragments are compared while they are alive, so a new fragment cannot have the address of an old one.
    auto sameChildren = [](const LayoutFragment* a, const LayoutFragment* PR_NULLABLE b) {
        if (b == nullptr || a->childCount != b->childCount)
            return false;
        for (uint32_t i = 0; i < a->childCount; ++i) {
            if (a->children[i].fragment != b->children[i].fragment || a->children[i].offset != b->children[i].offset)
                return false;
        }
        return true;
    };
    bool moved = !sameChildren(fragment, t->fragment) || extent != t->viewport || rootMoved;
    if (moved) {
        ++t->layouts;
        t->pendingPaint |= LayoutInputFlag_Layout;
    }
    if (fragment != t->fragment)
        layoutCacheRetire(t->cache, t->fragment);
    layoutFragmentRelease(t->fragment);
    t->fragment = fragment;
    // The caches drop their entries for the replaced fragments, which frees them.
    layoutPaintRetire(t);
    layoutQueriesRetire(t);
    t->cache.retired.clear();
    bool resized = extent != t->viewport;
    t->viewport = extent;
    layoutAnchoringAdjust(t, resized);
}

PrLayoutTreeStats prLayoutTreeStats(const PrLayoutTree* tree) {
    PrLayoutTreeStats stats = tree->stats;
    stats.stackingListsFound = tree->stacking.listsFound;
    return stats;
}

bool prLayoutTreeVerify(PrLayoutTree* tree, PrLayoutTreeDifference* PR_NULLABLE out) {
    LayoutTree* t = tree;
    PR_ASSERT(!t->frame.open);
    // What verifying finds and records does not count as the last layout's or paint's work.
    uint32_t listsFound = t->stacking.listsFound;
    Mismatch found{kPrLayoutNodeNone, {}};
    const PrLayoutTreeStats& done = t->stats;
    bool same = pagesVerify(t, &found);
    // Each stage did no work unless an input it reads changed.
    if (same && (t->laidOutFor & LayoutInputFlag_Layout) == 0 &&
            (done.blocksLaidOut != 0 || done.linesLaidOut != 0 || done.inlinesShaped != 0 || done.intrinsicsComputed != 0)) {
        mismatch(&found, kPrLayoutNodeNone, "the last layout did work, but no input that layout reads had changed:");
        const uint32_t counts[] = {done.blocksLaidOut, done.linesLaidOut, done.inlinesShaped, done.intrinsicsComputed};
        const char* names[] = {" blocks ", ", lines ", ", shapings ", ", intrinsic sizes "};
        for (uint32_t i = 0; i < 4; ++i) {
            prTextBufferAppend(&found.what, names[i]);
            prTextBufferAppendU64(&found.what, counts[i], 10);
        }
        same = false;
    }
    if (same && (t->paintedFor & (LayoutInputFlag_Layout | LayoutInputFlag_Paint)) == 0 &&
            (done.layersRecorded != 0 || done.subtreesRecorded != 0)) {
        mismatch(&found, kPrLayoutNodeNone, "the last paint recorded, but no input that recordings read had changed");
        same = false;
    }
    if (same && t->fragment != nullptr) {
        PrLayoutTreeStats stats{};
        LayoutState state{
            .tree = t,
            .styles = t->styles,
            .scratch = scratchOf(t),
            .stats = &stats,
            .viewport = t->viewport,
            .reuse = false,
        };
        const LayoutFragment* scratch = layoutViewport(state, t->root, t->viewport);
        same = sameFragments(t->fragment, scratch, kPrLayoutNodeNone, &found);
        layoutFragmentRelease(scratch);
        for (uint32_t i = 0; same && i < t->roots.count; ++i) {
            const LayoutLaidRoot& root = t->roots.data[i];
            const LayoutFragment* fresh = layoutViewport(state, root.node, root.extent);
            same = sameFragments(root.fragment, fresh, kPrLayoutNodeNone, &found);
            layoutFragmentRelease(fresh);
        }
        if (same) {
            LayoutNodeIndex node = layoutIntrinsicVerify(state);
            if (node != kPrLayoutNodeNone) {
                mismatch(&found, node, "the cached intrinsic sizes differ from a fresh measurement");
                same = false;
            }
        }
        layoutEndLayout(state);
        if (same) {
            LayoutNodeIndex node = layoutPositionsVerify(t);
            if (node != kPrLayoutNodeNone) {
                mismatch(&found, node, "the memoized position of the box differs from its placements");
                same = false;
            }
        }
        if (same) {
            LayoutNodeIndex node = layoutStackingVerify(t);
            if (node != kPrLayoutNodeNone) {
                mismatch(&found, node, "the cached stacking list differs from finding it again");
                same = false;
            }
        }
        if (same) {
            LayoutNodeIndex node = layoutPaintVerify(t);
            if (node != kPrLayoutNodeNone) {
                mismatch(&found, node, "the cached recording of the layer differs from a fresh recording");
                same = false;
            }
        }
        if (same && !layoutStylesVerify(t->styles)) {
            mismatch(&found, kPrLayoutNodeNone, "an entry the styles hold no reference to is not listed to be collected");
            same = false;
        }
        LayoutNodeIndex node = kPrLayoutNodeNone;
        if (same && !layoutQueriesVerify(t, &node)) {
            mismatch(&found, node, "the placement the last layout recorded differs from its fragments");
            same = false;
        }
    }
    t->stacking.listsFound = listsFound;
    if (!same && out != nullptr) {
        out->node = found.node != kPrLayoutNodeNone ? layoutNodeName(t, found.node) : kPrLayoutNodeNone;
        out->what = prTextBufferTake(&found.what);
    }
    prTextBufferReset(&found.what);
    return same;
}
