// Markdown trees: the nodes of one top-level block, as the parsers build them and as a document stores them.

#include "markdown/src/markdown_tree.h"

#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// Returns true if `node` is a paragraph of an item of a tight list, which is not stored as a node.
bool isTightParagraph(const MarkdownTree* tree, MarkdownNodeIndex node) {
    const MarkdownTreeNode& paragraph = tree->nodes.data[node];
    if (paragraph.values.kind != PrMarkdownNodeKind_Paragraph || paragraph.parent == kMarkdownNodeNone)
        return false;
    const MarkdownTreeNode& item = tree->nodes.data[paragraph.parent];
    if (item.values.kind != PrMarkdownNodeKind_Item || item.parent == kMarkdownNodeNone)
        return false;
    return tree->nodes.data[item.parent].values.tight;
}

} // namespace

void markdownTreeClear(MarkdownTree* tree) {
    tree->nodes.clear();
    tree->text.clear();
    markdownTreeCreate(tree, PrMarkdownNodeKind_Paragraph);
}

MarkdownNodeIndex markdownTreeCreate(MarkdownTree* tree, PrMarkdownNodeKind kind) {
    MarkdownNodeIndex index = tree->nodes.count;
    MarkdownTreeNode& node = tree->nodes.emplace();
    node = MarkdownTreeNode{};
    node.values.kind = kind;
    node.parent = kMarkdownNodeNone;
    node.first = kMarkdownNodeNone;
    node.last = kMarkdownNodeNone;
    node.previous = kMarkdownNodeNone;
    node.next = kMarkdownNodeNone;
    return index;
}

MarkdownNodeIndex markdownTreeAppend(MarkdownTree* tree, MarkdownNodeIndex parent, PrMarkdownNodeKind kind) {
    MarkdownNodeIndex node = markdownTreeCreate(tree, kind);
    markdownTreeAppendChild(tree, parent, node);
    return node;
}

void markdownTreeAppendChild(MarkdownTree* tree, MarkdownNodeIndex parent, MarkdownNodeIndex child) {
    MarkdownTreeNode& node = tree->nodes.data[child];
    PR_ASSERT(node.parent == kMarkdownNodeNone);
    MarkdownTreeNode& owner = tree->nodes.data[parent];
    node.parent = parent;
    node.previous = owner.last;
    node.next = kMarkdownNodeNone;
    if (owner.last != kMarkdownNodeNone)
        tree->nodes.data[owner.last].next = child;
    else
        owner.first = child;
    owner.last = child;
}

void markdownTreeInsertAfter(MarkdownTree* tree, MarkdownNodeIndex sibling, MarkdownNodeIndex node) {
    MarkdownTreeNode& inserted = tree->nodes.data[node];
    MarkdownTreeNode& before = tree->nodes.data[sibling];
    PR_ASSERT(inserted.parent == kMarkdownNodeNone && before.parent != kMarkdownNodeNone);
    inserted.parent = before.parent;
    inserted.previous = sibling;
    inserted.next = before.next;
    if (before.next != kMarkdownNodeNone)
        tree->nodes.data[before.next].previous = node;
    else
        tree->nodes.data[before.parent].last = node;
    before.next = node;
}

void markdownTreeInsertBefore(MarkdownTree* tree, MarkdownNodeIndex sibling, MarkdownNodeIndex node) {
    MarkdownTreeNode& inserted = tree->nodes.data[node];
    MarkdownTreeNode& after = tree->nodes.data[sibling];
    PR_ASSERT(inserted.parent == kMarkdownNodeNone && after.parent != kMarkdownNodeNone);
    inserted.parent = after.parent;
    inserted.next = sibling;
    inserted.previous = after.previous;
    if (after.previous != kMarkdownNodeNone)
        tree->nodes.data[after.previous].next = node;
    else
        tree->nodes.data[after.parent].first = node;
    after.previous = node;
}

void markdownTreeUnlink(MarkdownTree* tree, MarkdownNodeIndex node) {
    MarkdownTreeNode& unlinked = tree->nodes.data[node];
    if (unlinked.parent == kMarkdownNodeNone)
        return;
    MarkdownTreeNode& parent = tree->nodes.data[unlinked.parent];
    if (unlinked.previous != kMarkdownNodeNone)
        tree->nodes.data[unlinked.previous].next = unlinked.next;
    else
        parent.first = unlinked.next;
    if (unlinked.next != kMarkdownNodeNone)
        tree->nodes.data[unlinked.next].previous = unlinked.previous;
    else
        parent.last = unlinked.previous;
    unlinked.parent = kMarkdownNodeNone;
    unlinked.previous = kMarkdownNodeNone;
    unlinked.next = kMarkdownNodeNone;
}

MarkdownRange markdownTreeAddText(MarkdownTree* tree, PrStringView text) {
    MarkdownRange range{tree->text.count, uint32_t(text.size)};
    if (text.size != 0)
        tree->text.append(text.data, uint32_t(text.size));
    return range;
}

MarkdownRange markdownTreeCopyText(MarkdownTree* tree, MarkdownRange range) {
    MarkdownRange copy{tree->text.count, range.size};
    tree->text.reserve(tree->text.count + range.size);
    memCopy(tree->text.data + tree->text.count, tree->text.data + range.start, range.size);
    tree->text.count += range.size;
    return copy;
}

void markdownTreeFlatten(const MarkdownTree* tree, Array<MarkdownNode>* out) {
    // The stored nodes whose subtrees are open, with the tree node that each came from. A paragraph that is left out
    // has no stored node and opens no subtree.
    struct Open {
        MarkdownNodeIndex node;
        uint32_t stored;
    };
    Array<Open> open;
    // The position in `out` at which a stored subtree last closed. A Text node before that position is not a sibling of
    // the node that follows it.
    uint32_t closedAt = 0;
    uint32_t first = out->count;
    MarkdownNodeIndex node = tree->nodes.data[0].first;
    while (node != kMarkdownNodeNone) {
        const MarkdownTreeNode& current = tree->nodes.data[node];
        bool stored = !isTightParagraph(tree, node);
        if (stored) {
            const MarkdownValues& values = current.values;
            MarkdownNode* previous = out->count != first ? &out->last() : nullptr;
            bool merges = values.kind == PrMarkdownNodeKind_Text && previous != nullptr
                && previous->values.kind == PrMarkdownNodeKind_Text && previous->values.textKind == values.textKind
                && previous->values.text.start + previous->values.text.size == values.text.start
                && closedAt != out->count;
            if (merges) {
                previous->values.text.size += values.text.size;
            } else {
                MarkdownNode& added = out->emplace();
                added.values = values;
                added.end = out->count;
            }
        }
        if (current.first != kMarkdownNodeNone) {
            open.push(Open{node, stored ? out->count - 1 : kMarkdownNodeNone});
            node = current.first;
            continue;
        }
        // Close the subtrees that end with this node.
        while (node != kMarkdownNodeNone && tree->nodes.data[node].next == kMarkdownNodeNone) {
            if (open.count == 0) {
                node = kMarkdownNodeNone;
                break;
            }
            Open closed = open.last();
            open.pop();
            if (closed.stored != kMarkdownNodeNone) {
                out->data[closed.stored].end = out->count;
                closedAt = out->count;
            }
            node = closed.node;
        }
        if (node != kMarkdownNodeNone)
            node = tree->nodes.data[node].next;
    }
    open.reset();
}

} // namespace pr_impl
