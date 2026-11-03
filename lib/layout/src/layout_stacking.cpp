// Stacking: the lists of the boxes that paint whole in each stacking context, kept on the box tree.

#include "layout/src/layout_stacking.h"

#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// Appends to `out` the boxes in the subtree of `node` that paint whole in the stacking context that `node` paints in,
// in tree order: `node` if it paints whole, and the boxes inside it unless it makes a stacking context of its own. A
// node absent from the final tree, and everything it holds, contributes none (layoutIsAbsent). A fresh search also
// visits the nodes that do not lead to a box that paints whole.
void collect(const LayoutTree* tree, LayoutNodeIndex node, bool fresh, Array<LayoutStackedBox>& out) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if ((!fresh && !n.leadsWhole) || n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak ||
            layoutIsAbsent(tree, node))
        return;
    if (layoutNodePaintsWhole(tree, node))
        out.push(LayoutStackedBox{node, layoutStackLevel(tree, node)});
    if (layoutIsStackingContext(tree, node))
        return;
    for (LayoutNodeIndex child = n.first; child != kPrLayoutNodeNone; child = layoutTreeNode(tree, child).next)
        collect(tree, child, fresh, out);
}

// A box found for a list, with its position in tree order among the list's boxes.
struct FoundBox {
    LayoutStackedBox box;
    uint32_t order;
};

// Sets `out` to the list of the stacking context `context`, found from the box tree.
void findList(const LayoutTree* tree, LayoutNodeIndex context, bool fresh, Array<LayoutStackedBox>& out, uint32_t* zero) {
    out.clear();
    for (LayoutNodeIndex child = layoutTreeNode(tree, context).first; child != kPrLayoutNodeNone;
            child = layoutTreeNode(tree, child).next)
        collect(tree, child, fresh, out);
    Array<FoundBox> found;
    for (uint32_t i = 0; i < out.count; ++i)
        found.push(FoundBox{out.data[i], i});
    sortItems(found.data, found.count, [](const FoundBox& a, const FoundBox& b) {
        return a.box.level < b.box.level || (a.box.level == b.box.level && a.order < b.order);
    });
    for (uint32_t i = 0; i < found.count; ++i)
        out.data[i] = found.data[i].box;
    found.reset();
    *zero = partitionPoint(0, out.count, [&](uint32_t i) { return out.data[i].level < 0; });
}

// Returns the index of the list of `context`, or kHashMissing.
uint32_t listAt(const LayoutStacking& s, LayoutNodeIndex context) {
    return s.byContext.find(KeyIndex::hashOf(context), [&](uint32_t i) { return s.lists.data[i].context == context; });
}

// Frees the list at `at`.
void dropList(LayoutStacking& s, uint32_t at) {
    LayoutStackingList& list = s.lists.data[at];
    s.byContext.erase(KeyIndex::hashOf(list.context), at);
    list.context = kPrLayoutNodeNone;
    list.boxes.clear();
    s.freeLists.push(at);
}

// Returns the depth of `node` in the box tree: 0 for a node with no parent.
uint32_t depthOf(const LayoutTree* tree, LayoutNodeIndex node) {
    uint32_t depth = 0;
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != kPrLayoutNodeNone; at =
            layoutTreeNode(tree, at).parent)
        ++depth;
    return depth;
}

// Returns true if `a` is before `b` in tree order, given that both are in one tree. The cost is their depths plus the
// siblings between their subtrees, walked from both ends.
bool treeBefore(const LayoutTree* tree, LayoutNodeIndex a, LayoutNodeIndex b) {
    if (a == b)
        return false;
    uint32_t da = depthOf(tree, a);
    uint32_t db = depthOf(tree, b);
    for (; da > db; --da) {
        a = layoutTreeNode(tree, a).parent;
        if (a == b)
            return false;
    }
    for (; db > da; --db) {
        b = layoutTreeNode(tree, b).parent;
        if (b == a)
            return true;
    }
    while (layoutTreeNode(tree, a).parent != layoutTreeNode(tree, b).parent) {
        a = layoutTreeNode(tree, a).parent;
        b = layoutTreeNode(tree, b).parent;
    }
    // Siblings: each walks forward from its own position until it meets the other or ends.
    for (LayoutNodeIndex x = a, y = b;;) {
        x = layoutTreeNode(tree, x).next;
        if (x == b || y == kPrLayoutNodeNone)
            return true;
        y = layoutTreeNode(tree, y).next;
        if (y == a || x == kPrLayoutNodeNone)
            return false;
    }
}

// Returns the index in `list` at which the box `node` at `level` is or would go: after the boxes at lower levels, and
// after those at its level that precede it in tree order.
uint32_t placeOf(const LayoutTree* tree, const LayoutStackingList& list, LayoutNodeIndex node, int32_t level) {
    const LayoutStackedBox* boxes = list.boxes.data;
    uint32_t first = partitionPoint(0, list.boxes.count, [&](uint32_t i) { return boxes[i].level < level; });
    uint32_t end = partitionPoint(first, list.boxes.count, [&](uint32_t i) { return boxes[i].level == level; });
    return partitionPoint(first, end, [&](uint32_t i) { return treeBefore(tree, boxes[i].node, node); });
}

// Returns the list of the context that `node` paints in if the cache holds it current, and null otherwise. A stale list
// is found again whole, and a list that is not held is found when it is first read.
LayoutStackingList* PR_NULLABLE heldListAround(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutStacking& s = tree->stacking;
    if (s.lists.count == 0)
        return nullptr;
    LayoutNodeIndex context = layoutNodeStackingContext(tree, node);
    uint32_t at = context != kPrLayoutNodeNone ? listAt(s, context) : kHashMissing;
    return at != kHashMissing && !s.lists.data[at].stale ? &s.lists.data[at] : nullptr;
}

// Sets which box of `list` is the first at level 0 or above, and gives the list a new version.
void listChanged(LayoutStacking& s, LayoutStackingList& list) {
    list.zero = partitionPoint(0, list.boxes.count, [&](uint32_t i) { return list.boxes.data[i].level < 0; });
    list.version = ++s.versions;
}

} // namespace

bool layoutNodePaintsWholeAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (!layoutKindIsBox(n.kind) || n.parent == kPrLayoutNodeNone ||
            !layoutStyleTraits(tree->styles, style).mayPaintWhole)
        return false;
    if (layoutIsInlineBoxAs(tree, node, style))
        return layoutInlinePaintsWhole(tree->styles, style);
    return layoutPaintsWholeAs(tree, node, style);
}

bool layoutNodePaintsWhole(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutNodePaintsWholeAs(tree, node, layoutTreeNode(tree, node).style);
}

bool layoutNodeLeadsWhole(const LayoutTree* tree, LayoutNodeIndex node) {
    for (LayoutNodeIndex child = layoutTreeNode(tree, node).first; child != kPrLayoutNodeNone;
            child = layoutTreeNode(tree, child).next) {
        if (layoutTreeNode(tree, child).leadsWhole)
            return true;
    }
    return false;
}

LayoutNodeIndex layoutNodeStackingContext(const LayoutTree* tree, LayoutNodeIndex node) {
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != kPrLayoutNodeNone; at =
            layoutTreeNode(tree, at).parent) {
        if (layoutIsStackingContext(tree, at))
            return at;
    }
    return kPrLayoutNodeNone;
}

LayoutStackedBoxes layoutStackingList(const LayoutTree* tree, LayoutNodeIndex context) {
    LayoutStacking& s = tree->stacking;
    uint32_t at = listAt(s, context);
    if (at == kHashMissing) {
        if (s.freeLists.count != 0) {
            at = s.freeLists.last();
            s.freeLists.pop();
        } else {
            at = s.lists.count;
            s.lists.emplace() = LayoutStackingList{};
        }
        s.lists.data[at].context = context;
        s.lists.data[at].stale = true;
        s.byContext.insert(KeyIndex::hashOf(context), at);
    }
    LayoutStackingList& list = s.lists.data[at];
    if (list.stale) {
        findList(tree, context, false, list.boxes, &list.zero);
        list.stale = false;
        list.version = ++s.versions;
        ++s.listsFound;
    }
    return LayoutStackedBoxes{list.boxes.data, list.boxes.count, list.zero, list.version};
}

bool layoutStackingListIs(const LayoutTree* tree, LayoutNodeIndex context, uint32_t version) {
    uint32_t at = listAt(tree->stacking, context);
    return at != kHashMissing && !tree->stacking.lists.data[at].stale && tree->stacking.lists.data[at].version == version;
}

void layoutStackingRemoving(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutStackingList* list = heldListAround(tree, node);
    if (list == nullptr || !tree->nodes.data[node].leadsWhole)
        return;
    LayoutStacking& s = tree->stacking;
    s.moved.clear();
    collect(tree, node, false, s.moved);
    for (uint32_t i = 0; i < s.moved.count; ++i) {
        const LayoutStackedBox& box = s.moved.data[i];
        uint32_t at = placeOf(tree, *list, box.node, box.level);
        // A current list holds every box that the tree says paints whole in it. A list that does not is found again.
        bool held = at != list->boxes.count && list->boxes.data[at].node == box.node;
        PR_ASSERT(held);
        if (!held) {
            list->stale = true;
            return;
        }
        list->boxes.erase(at);
    }
    if (s.moved.count != 0)
        listChanged(s, *list);
}

void layoutStackingInserted(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutStacking& s = tree->stacking;
    LayoutTreeNode& n = tree->nodes.data[node];
    // If no list is held, as while a tree is built, only the marks change.
    if (s.lists.count != 0 && !layoutIsStackingContext(tree, node)) {
        uint32_t own = listAt(s, node);
        if (own != kHashMissing)
            dropList(s, own);
    }
    // The marks are a superset of the boxes that paint whole, and the record's traits alone decide them.
    if (layoutKindIsBox(n.kind) && layoutStyleTraits(tree->styles, n.style).mayPaintWhole)
        n.leadsWhole = true;
    // A node that leads to no box that paints whole is in no list, and changes none.
    if (!n.leadsWhole)
        return;
    // Its ancestors also lead to it. They are marked up to the first ancestor that is already marked, whose own
    // ancestors are marked too.
    for (LayoutNodeIndex at = n.parent; at != kPrLayoutNodeNone && !tree->nodes.data[at].leadsWhole;
            at = tree->nodes.data[at].parent)
        tree->nodes.data[at].leadsWhole = true;
    LayoutStackingList* list = heldListAround(tree, node);
    if (list == nullptr)
        return;
    s.moved.clear();
    collect(tree, node, false, s.moved);
    for (uint32_t i = 0; i < s.moved.count; ++i) {
        const LayoutStackedBox& box = s.moved.data[i];
        list->boxes.insert(placeOf(tree, *list, box.node, box.level), box);
    }
    if (s.moved.count != 0)
        listChanged(s, *list);
}

void layoutStackingStaleAll(LayoutTree* tree) {
    for (uint32_t i = 0; i < tree->stacking.lists.count; ++i)
        tree->stacking.lists.data[i].stale = true;
}

void layoutStackingForget(LayoutTree* tree, LayoutNodeIndex node) {
    if (tree->stacking.lists.count == 0)
        return;
    uint32_t at = listAt(tree->stacking, node);
    if (at != kHashMissing)
        dropList(tree->stacking, at);
}

void layoutStackingReset(LayoutStacking& stacking) {
    for (uint32_t i = 0; i < stacking.lists.count; ++i)
        stacking.lists.data[i].boxes.reset();
    stacking.lists.reset();
    stacking.freeLists.reset();
    stacking.byContext.reset();
    stacking.moved.reset();
}

uint32_t layoutStackingVerify(const LayoutTree* tree) {
    const LayoutStacking& stacking = tree->stacking;
    Array<LayoutStackedBox> boxes;
    LayoutNodeIndex differs = kPrLayoutNodeNone;
    for (uint32_t i = 0; i < stacking.lists.count && differs == kPrLayoutNodeNone; ++i) {
        const LayoutStackingList& list = stacking.lists.data[i];
        if (list.context == kPrLayoutNodeNone || list.stale || !tree->nodes.data[list.context].live)
            continue;
        uint32_t zero = 0;
        findList(tree, list.context, true, boxes, &zero);
        if (boxes.count != list.boxes.count || zero != list.zero ||
                !prMemEquals(boxes.data, list.boxes.data, sizeof(LayoutStackedBox) * boxes.count))
            differs = list.context;
    }
    boxes.reset();
    return differs;
}

} // namespace pr_impl
