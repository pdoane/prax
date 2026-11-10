// Frames: a layout tree that an immediate-mode producer declares in full every frame.

#include "pr/layout_frame.h"
#include "layout/src/layout_frame.h"

#include "pr/assert.h"
#include "pr/hash_index.h"
#include "pr/sort.h"

#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// Returns true if the open frame created `node`.
bool created(const LayoutFrame& frame, LayoutNodeIndex node) {
    uint32_t at = frame.positions.data[node];
    return at < frame.created.count && frame.created.data[at].node == node;
}

// Returns the creation of `node`, which the open frame must have created.
LayoutCreation& creationOf(LayoutTree* tree, LayoutNodeIndex node) {
    LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open && node != kPrLayoutNodeNone && node < frame.positions.count && created(frame, node));
    return frame.created.data[frame.positions.data[node]];
}

// Grows the arrays that the frame keeps by node index to the tree's nodes.
void growByNode(LayoutTree* tree) {
    LayoutFrame& frame = tree->frame;
    uint32_t count = tree->nodes.count;
    while (frame.positions.count < count)
        frame.positions.push(0);
    while (frame.preorderAt.count < count)
        frame.preorderAt.push(0);
    // An entry that belongs to no frame is stamped 0, which is not the number of any frame.
    while (frame.links.count < count)
        frame.links.push(LayoutFrameLinks{});
}

// Returns the links of `node` in the open frame, empty if the frame has not yet given it any.
LayoutFrameLinks& linksOf(LayoutFrame& frame, LayoutNodeIndex node) {
    LayoutFrameLinks& l = frame.links.data[node];
    if (l.frame != frame.frame)
        l = LayoutFrameLinks{
            .frame = frame.frame,
            .first = kPrLayoutNodeNone,
            .last = kPrLayoutNodeNone,
            .next = kPrLayoutNodeNone,
        };
    return l;
}

// Marks `parent` for the commit to relink, because its children in the frame may not be its committed children in
// order.
void markRelink(LayoutFrame& frame, LayoutNodeIndex parent) {
    if (parent == kPrLayoutNodeNone)
        return;
    LayoutFrameLinks& l = linksOf(frame, parent);
    if (!l.relinked) {
        l.relinked = true;
        frame.relinks.push(parent);
    }
}

// Appends the subtree of `root` to the committed preorder.
void takeSubtree(LayoutTree* tree, LayoutNodeIndex root) {
    LayoutFrame& frame = tree->frame;
    for (LayoutNodeIndex node = root; node != kPrLayoutNodeNone;) {
        const LayoutTreeNode& n = tree->nodes.data[node];
        frame.preorderAt.data[node] = frame.preorder.count;
        frame.preorder.push(LayoutPreorderNode{
            .node = node,
            .parent = n.parent,
            .key = n.key,
            .style = n.style,
            .generation = n.generation,
            .kind = n.kind,
        });
        if (n.first != kPrLayoutNodeNone) {
            node = n.first;
            continue;
        }
        LayoutNodeIndex at = node;
        while (at != root && tree->nodes.data[at].next == kPrLayoutNodeNone)
            at = tree->nodes.data[at].parent;
        node = at == root ? kPrLayoutNodeNone : tree->nodes.data[at].next;
    }
}

// Takes the committed tree in preorder.
void takePreorder(LayoutTree* tree) {
    LayoutFrame& frame = tree->frame;
    growByNode(tree);
    frame.preorder.clear();
    frame.preorderStale = false;
    for (uint32_t r = 0; r < frame.roots.count; ++r)
        takeSubtree(tree, frame.roots.data[r]);
}

// Returns true if the frame may take the committed node at position `at` of the preorder for a creation of `kind` keyed
// by `key` under `parent`.
bool matches(const LayoutFrame& frame, uint32_t at, LayoutNodeIndex parent, uint64_t key, PrLayoutNodeKind kind) {
    if (at >= frame.limit)
        return false;
    const LayoutPreorderNode& p = frame.preorder.data[at];
    return p.key == key && p.kind == kind && p.parent == parent;
}

// Records the creation under `parent` of the committed node at position `at` of the preorder, moves the cursor past it,
// and returns its handle.
PrLayoutNode takeAt(LayoutFrame& frame, uint32_t at, LayoutNodeIndex parent, PrLayoutStyleIndex style) {
    const LayoutPreorderNode& p = frame.preorder.data[at];
    frame.positions.data[p.node] = frame.created.count;
    frame.created.push(LayoutCreation{.node = p.node, .parent = parent, .style = style, .committed = p.style});
    frame.cursor = at + 1;
    return PrLayoutNode(p.generation) << 32 | p.node;
}

// Moves the cursor to position `at` of the preorder, ending the run of positions in order that the frame was in.
void moveCursor(LayoutFrame& frame, uint32_t at) {
    if (frame.cursor > frame.runStart)
        frame.runs.push(LayoutPreorderRun{frame.runStart, frame.cursor});
    frame.cursor = at;
    frame.runStart = at;
    // A run after the new position bounds how far the frame goes on in order, because its nodes are already created.
    frame.limit = frame.preorder.count;
    for (uint32_t i = 0; i < frame.runs.count; ++i) {
        uint32_t start = frame.runs.data[i].start;
        if (start > at && start < frame.limit)
            frame.limit = start;
    }
}

// Returns the node that holds `key`, or kPrLayoutNodeNone if no node does.
LayoutNodeIndex keyHolder(const LayoutTree* tree, uint64_t key) {
    uint32_t found = tree->keys.find(KeyIndex::hashOf(key), [&](uint32_t node) { return tree->nodes.data[node].key == key; });
    return found == kHashMissing ? kPrLayoutNodeNone : found;
}

// Finds the committed node that a creation of `kind` keyed by `key` under `parent` continues, where it is not the one
// at the cursor: a keyed node by its key, wherever it is, and an unkeyed node at the next committed child of the parent
// of its kind that the frame has not created. Returns kPrLayoutNodeNone if the creation continues no node. A keyed node
// that the frame created already is not found again (createAway).
LayoutNodeIndex find(LayoutTree* tree, LayoutNodeIndex parent, uint64_t key, PrLayoutNodeKind kind) {
    const LayoutFrame& frame = tree->frame;
    if (key != 0) {
        LayoutNodeIndex found = keyHolder(tree, key);
        if (found == kPrLayoutNodeNone)
            return kPrLayoutNodeNone;
        PR_ASSERT(!created(frame, found));
        if (tree->nodes.data[found].kind == kind)
            return found;
        // The committed node with the key is of another kind, so no creation continues it. It gives up the key to the
        // new node, and is erased at the commit.
        layoutTreeSetKey(tree, found, 0);
        return kPrLayoutNodeNone;
    }
    // An unkeyed root continues the committed tree's first root of its kind that the frame has not created.
    if (parent == kPrLayoutNodeNone) {
        for (uint32_t i = 0; i < frame.roots.count; ++i) {
            LayoutNodeIndex root = frame.roots.data[i];
            if (tree->nodes.data[root].key == 0 && tree->nodes.data[root].kind == kind && !created(frame, root))
                return root;
        }
        return kPrLayoutNodeNone;
    }
    // The search starts at the cursor if the cursor is among the parent's children, where the frame left the preorder.
    LayoutNodeIndex start = tree->nodes.data[parent].first;
    if (frame.cursor < frame.preorder.count && frame.preorder.data[frame.cursor].parent == parent)
        start = frame.preorder.data[frame.cursor].node;
    for (LayoutNodeIndex child = start; child != kPrLayoutNodeNone; child = tree->nodes.data[child].next) {
        const LayoutTreeNode& c = tree->nodes.data[child];
        if (c.key == 0 && c.kind == kind && !created(frame, child))
            return child;
    }
    return kPrLayoutNodeNone;
}

// Creates a node of `kind` under `parent` that continues no committed node, and gives it `key` unless the key is 0.
LayoutNodeIndex createNew(LayoutTree* tree, LayoutNodeIndex parent, uint64_t key, PrLayoutNodeKind kind,
        PrLayoutStyleIndex style) {
    LayoutFrame& frame = tree->frame;
    LayoutNodeIndex node = layoutTreeCreateNode(tree, kind);
    if (key != 0)
        layoutTreeSetKey(tree, node, key);
    growByNode(tree);
    frame.made.push(frame.created.count);
    frame.positions.data[node] = frame.created.count;
    frame.created.push(LayoutCreation{.node = node, .parent = parent, .style = style, .committed = kPrLayoutStyleNone});
    markRelink(frame, parent);
    return node;
}

// Creates a node where create's inline case does not: a committed node away from the cursor, which find finds, a new
// node, or the node at the cursor when the storage for creations must grow.
PrLayoutNode createAwayFrom(LayoutTree* tree, LayoutNodeIndex parent, uint64_t key, PrLayoutNodeKind kind,
        PrLayoutStyleIndex style) {
    LayoutFrame& frame = tree->frame;
    if (matches(frame, frame.cursor, parent, key, kind))
        return takeAt(frame, frame.cursor, parent, style);
    // A key names one node of the frame. A later creation with a key that the frame gave already is a caller's error.
    // It makes a new node without the key, so that the tree stays whole, and the frame records both nodes for the
    // caller to show (prLayoutTreeDuplicateKeys).
    if (key != 0) {
        LayoutNodeIndex holder = keyHolder(tree, key);
        if (holder != kPrLayoutNodeNone && created(frame, holder)) {
            LayoutNodeIndex node = createNew(tree, parent, 0, kind, style);
            frame.duplicates.push(PrLayoutDuplicateKey{
                .key = key,
                .first = layoutNodeName(tree, holder),
                .second = layoutNodeName(tree, node),
            });
            return layoutNodeName(tree, node);
        }
    }
    LayoutNodeIndex node = find(tree, parent, key, kind);
    if (node == kPrLayoutNodeNone)
        return layoutNodeName(tree, createNew(tree, parent, key, kind, style));
    ++frame.jumps;
    // The node keeps its parent's children in their committed order if it stays under its parent and its committed
    // previous sibling was created before it, under the same parent. A frame in which every creation does so gives each
    // parent its committed children in order, in whatever order it creates across parents.
    const LayoutTreeNode& n = tree->nodes.data[node];
    bool inOrder = n.parent == parent && (n.previous == kPrLayoutNodeNone ||
        (created(frame, n.previous) && creationOf(tree, n.previous).parent == parent));
    if (!inOrder)
        markRelink(frame, parent);
    uint32_t at = frame.preorderAt.data[node];
    if (at < frame.preorder.count && frame.preorder.data[at].node == node) {
        moveCursor(frame, at);
        return takeAt(frame, at, parent, style);
    }
    // A keyed node outside the committed tree joins it under the parent.
    frame.positions.data[node] = frame.created.count;
    frame.created.push(LayoutCreation{.node = node, .parent = parent, .style = style, .committed = n.style});
    return layoutNodeName(tree, node);
}

// Creates a node where create's inline case does not, as createAwayFrom does, and lists a root among the roots that the
// frame found away from the cursor or made. The roots that the inline case continues are found from the committed roots
// (gatherRoots), so that the common case pays nothing for roots.
PR_NOINLINE PrLayoutNode createAway(LayoutTree* tree, LayoutNodeIndex parent, uint64_t key, PrLayoutNodeKind kind,
        PrLayoutStyleIndex style) {
    PrLayoutNode node = createAwayFrom(tree, parent, key, kind, style);
    if (parent == kPrLayoutNodeNone)
        tree->frame.openRoots.push(layoutNodeIndex(tree, node));
    return node;
}

// Writes to `out` the roots that the open frame created, in the order created: the committed roots that it continued,
// and the roots that it found away from the cursor or made (LayoutFrame::openRoots).
void gatherRoots(const LayoutTree* tree, RawArray<LayoutNodeIndex>* out) {
    const LayoutFrame& frame = tree->frame;
    out->clear();
    for (uint32_t i = 0; i < frame.roots.count; ++i) {
        LayoutNodeIndex root = frame.roots.data[i];
        if (created(frame, root) && frame.created.data[frame.positions.data[root]].parent == kPrLayoutNodeNone)
            out->push(root);
    }
    for (uint32_t i = 0; i < frame.openRoots.count; ++i) {
        LayoutNodeIndex root = frame.openRoots.data[i];
        bool listed = false;
        for (uint32_t k = 0; k < out->count && !listed; ++k)
            listed = out->data[k] == root;
        if (!listed)
            out->push(root);
    }
    sortItems(out->span(), [&](LayoutNodeIndex a, LayoutNodeIndex b) {
        return frame.positions.data[a] < frame.positions.data[b];
    });
}

// Creates a node of `kind` keyed by `key` under `parent` in the open frame, styled by `style`, and returns its handle.
// The node is the committed node that the creation continues, or a new one. The common case, the next committed node in
// preorder, is inline, and costs one comparison and one append.
inline PrLayoutNode create(LayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutNodeKind kind,
        PrLayoutStyleIndex style) {
    LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open);
    LayoutNodeIndex parentIndex = layoutNodeIndex(tree, parent);
    PR_ASSERT(parentIndex == kPrLayoutNodeNone ||
        (created(frame, parentIndex) && layoutKindIsElement(tree->nodes.data[parentIndex].kind)));
    uint32_t at = frame.cursor;
    uint32_t count = frame.created.count;
    if (matches(frame, at, parentIndex, key, kind) && count != frame.created.capacity) {
        const LayoutPreorderNode& p = frame.preorder.data[at];
        frame.positions.data[p.node] = count;
        frame.created.data[count] = LayoutCreation{.node = p.node, .parent = parentIndex, .style = style, .committed = p.style};
        frame.created.count = count + 1;
        frame.cursor = at + 1;
        return PrLayoutNode(p.generation) << 32 | p.node;
    }
    return createAway(tree, parentIndex, key, kind, style);
}

// Gives the committed node `parent` its children from the frame, in the frame's order, using the tree's own edits and
// their marks. Removes each committed child that the frame did not give it, then moves or inserts each child from the
// frame that is not in its place.
void relink(LayoutTree* tree, LayoutNodeIndex parent) {
    LayoutFrame& frame = tree->frame;
    for (LayoutNodeIndex child = tree->nodes.data[parent].first; child != kPrLayoutNodeNone;) {
        LayoutNodeIndex next = tree->nodes.data[child].next;
        if (!created(frame, child) || frame.created.data[frame.positions.data[child]].parent != parent)
            layoutTreeRemove(tree, child);
        child = next;
    }
    LayoutNodeIndex at = tree->nodes.data[parent].first;
    for (LayoutNodeIndex child = frame.links.data[parent].first; child != kPrLayoutNodeNone;
            child = frame.links.data[child].next) {
        if (child == at) {
            at = tree->nodes.data[child].next;
            continue;
        }
        if (tree->nodes.data[child].parent != kPrLayoutNodeNone)
            layoutTreeRemove(tree, child);
        layoutTreeInsert(tree, parent, child, at);
    }
}

} // namespace

void layoutFrameCommit(LayoutTree* tree) {
    LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open);
    frame.open = false;
    frame.reported.clear();
    frame.reported.append(frame.duplicates.span());
    layoutTreeExpireFrameMarks(tree, frame.frame);
    PrLayoutTreeStats& stats = tree->stats;
    uint32_t kept = frame.created.count - frame.made.count;
    stats.frameNodesKept += kept;
    stats.frameNodesMade += frame.made.count;
    stats.frameNodesJumped += frame.jumps;
    // A frame that continued every committed node, under its committed parent and in order, changes only records.
    bool sameTree = frame.relinks.count == 0 && frame.made.count == 0 && kept == frame.preorder.count;
    if (sameTree) {
        for (uint32_t i = 0; i < frame.created.count; ++i) {
            const LayoutCreation& c = frame.created.data[i];
            if (c.style != c.committed) {
                layoutTreeChangeStyle(tree, c.node, c.style);
                frame.preorder.data[frame.preorderAt.data[c.node]].style = c.style;
            }
        }
        // The preorder was current when the frame began, and now holds the records just set.
        frame.preorderStale = false;
        return;
    }
    // A new node takes its record before it joins the tree, because the tree reads the record when it links the node.
    for (uint32_t i = 0; i < frame.made.count; ++i) {
        const LayoutCreation& c = frame.created.data[frame.made.data[i]];
        layoutTreeSetNewStyle(tree, c.node, c.style);
    }
    // A committed node that no creation continued leaves its parent, which the frame created, and is erased with its
    // subtree, but for the nodes that creations moved out of it first.
    frame.gone.clear();
    for (uint32_t i = 0; i < frame.preorder.count; ++i) {
        const LayoutPreorderNode& p = frame.preorder.data[i];
        if (created(frame, p.node) || p.parent == kPrLayoutNodeNone || !created(frame, p.parent))
            continue;
        markRelink(frame, p.parent);
        frame.gone.push(p.node);
    }
    // The children that the frame gave each parent it relinks, in order.
    for (uint32_t i = 0; i < frame.created.count; ++i) {
        const LayoutCreation& c = frame.created.data[i];
        if (c.parent == kPrLayoutNodeNone)
            continue;
        LayoutFrameLinks& p = frame.links.data[c.parent];
        if (p.frame != frame.frame || !p.relinked)
            continue;
        linksOf(frame, c.node).next = kPrLayoutNodeNone;
        if (p.last != kPrLayoutNodeNone)
            frame.links.data[p.last].next = c.node;
        else
            p.first = c.node;
        p.last = c.node;
    }
    for (uint32_t i = 0; i < frame.relinks.count; ++i) {
        LayoutNodeIndex parent = frame.relinks.data[i];
        relink(tree, parent);
        // A parent that the frame made has no committed children to relink.
        if (frame.created.data[frame.positions.data[parent]].committed != kPrLayoutStyleNone)
            ++stats.frameParentsRelinked;
    }
    // The roots that the frame did not create again are gone, and those that it created are the committed roots.
    for (uint32_t i = 0; i < frame.roots.count; ++i) {
        if (!created(frame, frame.roots.data[i]))
            frame.gone.push(frame.roots.data[i]);
    }
    gatherRoots(tree, &frame.gathered);
    frame.roots.clear();
    if (frame.gathered.count != 0)
        frame.roots.append(frame.gathered.span());
    // None of the gone nodes holds a node that the frame created, because each such node was moved out first.
    for (uint32_t i = 0; i < frame.gone.count; ++i) {
        LayoutNodeIndex node = frame.gone.data[i];
        if (tree->nodes.data[node].live && tree->nodes.data[node].parent == kPrLayoutNodeNone) {
            layoutTreeEraseDetached(tree, node);
            ++stats.frameNodesGone;
        }
    }
    frame.gone.clear();
    // Records are set last, once every node is where the frame put it, because a change of record marks the children
    // and siblings of the node.
    for (uint32_t i = 0; i < frame.created.count; ++i) {
        const LayoutCreation& c = frame.created.data[i];
        if (c.style != c.committed && tree->nodes.data[c.node].live)
            layoutTreeChangeStyle(tree, c.node, c.style);
    }
    takePreorder(tree);
}

void layoutFrameReset(LayoutFrame& frame) {
    frame.preorder.reset();
    frame.preorderAt.reset();
    frame.created.reset();
    frame.positions.reset();
    frame.runs.reset();
    frame.relinks.reset();
    frame.made.reset();
    frame.duplicates.reset();
    frame.reported.reset();
    frame.links.reset();
    frame.gone.reset();
    frame.roots.reset();
    frame.openRoots.reset();
    frame.gathered.reset();
}

} // namespace pr_impl

using namespace pr_impl;

void prLayoutTreeBeginFrame(PrLayoutTree* tree) {
    LayoutFrame& frame = tree->frame;
    PR_ASSERT(!frame.open);
    frame.open = true;
    ++frame.frame;
    frame.openRoots.clear();
    if (frame.preorderStale)
        takePreorder(tree);
    growByNode(tree);
    frame.created.clear();
    frame.cursor = 0;
    frame.runStart = 0;
    frame.limit = frame.preorder.count;
    frame.runs.clear();
    frame.relinks.clear();
    frame.made.clear();
    frame.duplicates.clear();
    frame.jumps = 0;
}

bool prLayoutTreeIsInFrame(const PrLayoutTree* tree) {
    return tree->frame.open;
}

PrLayoutNode prLayoutTreeFrameElement(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style) {
    return create(tree, parent, key, PrLayoutNodeKind_Element, style);
}

PrLayoutNode prLayoutTreeFrameText(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrStringView text) {
    PrLayoutNode node = create(tree, parent, key, PrLayoutNodeKind_Text, kPrLayoutStyleNone);
    prLayoutNodeSetText(tree, node, text);
    return node;
}

PrLayoutNode prLayoutTreeFrameReplaced(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style) {
    return create(tree, parent, key, PrLayoutNodeKind_Replaced, style);
}

PrLayoutNode prLayoutTreeFrameSemiReplaced(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style) {
    return create(tree, parent, key, PrLayoutNodeKind_SemiReplaced, style);
}

PrLayoutNode prLayoutTreeFrameLineBreak(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style) {
    return create(tree, parent, key, PrLayoutNodeKind_LineBreak, style);
}

void prLayoutTreeFrameSetStyle(PrLayoutTree* tree, PrLayoutNode node, PrLayoutStyleIndex style) {
    creationOf(tree, layoutNodeIndex(tree, node)).style = style;
}

PrLayoutStyleIndex prLayoutTreeFrameStyle(const PrLayoutTree* tree, PrLayoutNode node) {
    LayoutNodeIndex index = layoutNodeIndex(tree, node);
    const LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open && index != kPrLayoutNodeNone && index < frame.positions.count && created(frame, index));
    return frame.created.data[frame.positions.data[index]].style;
}

uint32_t prLayoutTreeFrameRoots(const PrLayoutTree* tree, PrLayoutNode* PR_NULLABLE out, uint32_t capacity) {
    const LayoutFrame& frame = tree->frame;
    if (frame.open)
        gatherRoots(tree, &frame.gathered);
    const RawArray<LayoutNodeIndex>& roots = frame.open ? frame.gathered : frame.roots;
    for (uint32_t i = 0; i < roots.count && i < capacity; ++i)
        out[i] = layoutNodeName(tree, roots.data[i]);
    return roots.count;
}

PrLayoutNode prLayoutTreeFrameParent(const PrLayoutTree* tree, PrLayoutNode node) {
    LayoutNodeIndex index = layoutNodeIndex(tree, node);
    const LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open && index != kPrLayoutNodeNone && index < frame.positions.count && created(frame, index));
    return layoutNodeName(tree, frame.created.data[frame.positions.data[index]].parent);
}

void prLayoutTreeFrameSetText(PrLayoutTree* tree, PrLayoutNode text, PrStringView value) {
    PR_ASSERT(creationOf(tree, layoutNodeIndex(tree, text)).node != kPrLayoutNodeNone);
    prLayoutNodeSetText(tree, text, value);
}

void prLayoutTreeFrameSetNaturalSize(PrLayoutTree* tree, PrLayoutNode replaced, PrLayoutNaturalSize size) {
    PR_ASSERT(creationOf(tree, layoutNodeIndex(tree, replaced)).node != kPrLayoutNodeNone);
    prLayoutNodeSetNaturalSize(tree, replaced, size);
}

void prLayoutTreeFrameSetContent(PrLayoutTree* tree, PrLayoutNode replaced, const PrLayoutContent* content) {
    PR_ASSERT(creationOf(tree, layoutNodeIndex(tree, replaced)).node != kPrLayoutNodeNone);
    // The same content at the same revision draws the same output. A draw list is frozen, and the drawing of a painter
    // changes only when it is invalidated or its revision changes.
    PrLayoutContent was = prLayoutNodeContent(tree, replaced);
    bool same = was.kind == content->kind && was.revision == content->revision &&
        (content->kind != PrLayoutContentKind_DrawList || was.list == content->list) &&
        (content->kind != PrLayoutContentKind_Painter ||
            (was.painter.paint == content->painter.paint && was.painter.measure == content->painter.measure &&
                was.painter.context == content->painter.context)) &&
        (content->kind != PrLayoutContentKind_Embed || was.embed == content->embed) &&
        (content->kind != PrLayoutContentKind_Image || was.image == content->image);
    if (!same)
        prLayoutNodeSetContent(tree, replaced, content);
}

void prLayoutTreeFrameSetHighlights(PrLayoutTree* tree, PrLayoutNode text,
        const PrLayoutHighlight* PR_NULLABLE highlights, uint32_t count) {
    LayoutNodeIndex index = layoutNodeIndex(tree, text);
    PR_ASSERT(tree->frame.open && creationOf(tree, index).node != kPrLayoutNodeNone);
    layoutTreeSetHighlights(tree, index, highlights, count, tree->frame.frame);
}

void prLayoutTreeFrameSetCarets(PrLayoutTree* tree, const PrLayoutCaret* PR_NULLABLE carets, uint32_t count) {
    PR_ASSERT(tree->frame.open);
    layoutTreeSetCarets(tree, carets, count, tree->frame.frame);
}

uint32_t prLayoutNodeSlot(PrLayoutNode node) {
    return LayoutNodeIndex(node);
}

uint32_t prLayoutTreeSlotCount(const PrLayoutTree* tree) {
    return tree->nodes.count;
}

uint32_t prLayoutTreeFrameCursor(const PrLayoutTree* tree) {
    const LayoutFrame& frame = tree->frame;
    PR_ASSERT(frame.open);
    return frame.cursor < frame.limit ? frame.preorder.data[frame.cursor].node : kPrLayoutFrameCursorEnd;
}

uint32_t prLayoutTreeDuplicateKeys(const PrLayoutTree* tree, PrLayoutDuplicateKey* out, uint32_t capacity) {
    const RawArray<PrLayoutDuplicateKey>& reported = tree->frame.reported;
    for (uint32_t i = 0; i < reported.count && i < capacity; ++i)
        out[i] = reported.data[i];
    return reported.count;
}
