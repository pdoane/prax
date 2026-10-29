// Markdown trees: the nodes of one top-level block, as the parsers build them and as a document stores them.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/markdown.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A node of a build tree, by its position in the tree's nodes.
typedef uint32_t MarkdownNodeIndex;

// The index of no node.
constexpr MarkdownNodeIndex kMarkdownNodeNone = 0xffffffffu;

// A range of a tree's text.
struct MarkdownRange {
    uint32_t start;
    uint32_t size;
};

// What a node holds besides its place in a tree: the fields of PrMarkdownStep that its kind sets, with each view as a
// range of its tree's text.
struct MarkdownValues {
    PrMarkdownNodeKind kind;
    PrMarkdownTextKind textKind;
    uint8_t level;
    PrMarkdownListKind list;
    bool tight;
    PrMarkdownTask task;
    PrMarkdownAlignment alignment;
    bool header;
    bool display;
    PrMarkdownAlert alert;
    uint32_t start;
    MarkdownRange text;
    MarkdownRange info;
    MarkdownRange destination;
    MarkdownRange title;
};

// A node of a build tree, which links to its parent, its siblings and its children so that the parsers can move nodes.
// Emphasis and links wrap nodes that were parsed before them.
struct MarkdownTreeNode {
    MarkdownValues values;
    MarkdownNodeIndex parent;
    MarkdownNodeIndex first;
    MarkdownNodeIndex last;
    MarkdownNodeIndex previous;
    MarkdownNodeIndex next;
};

// A tree under construction, whose root is node 0. The root stands for the document and is not walked.
struct MarkdownTree {
    Array<MarkdownTreeNode> nodes;      // by MarkdownNodeIndex
    Array<char> text;
};

// A node as a document stores it: in document order, followed by its descendants up to `end`.
struct MarkdownNode {
    MarkdownValues values;
    uint32_t end;                       // the position after the node's last descendant
};

// Empties `tree`, keeping its storage, and creates its root.
void markdownTreeClear(MarkdownTree* tree);

// Creates a node of `kind` with no parent, and returns it.
MarkdownNodeIndex markdownTreeCreate(MarkdownTree* tree, PrMarkdownNodeKind kind);

// Creates a node of `kind` as the last child of `parent`, and returns it.
MarkdownNodeIndex markdownTreeAppend(MarkdownTree* tree, MarkdownNodeIndex parent, PrMarkdownNodeKind kind);

// Makes the node `child`, which has no parent, the last child of `parent`.
void markdownTreeAppendChild(MarkdownTree* tree, MarkdownNodeIndex parent, MarkdownNodeIndex child);

// Makes the node `node`, which has no parent, the sibling after or before `sibling`.
void markdownTreeInsertAfter(MarkdownTree* tree, MarkdownNodeIndex sibling, MarkdownNodeIndex node);
void markdownTreeInsertBefore(MarkdownTree* tree, MarkdownNodeIndex sibling, MarkdownNodeIndex node);

// Removes `node` and its subtree from its parent. The node keeps its children.
void markdownTreeUnlink(MarkdownTree* tree, MarkdownNodeIndex node);

// Appends `text` to the tree's text and returns its range.
MarkdownRange markdownTreeAddText(MarkdownTree* tree, PrStringView text);

// Appends a copy of the range `range` of the tree's text to its end and returns the copy's range.
MarkdownRange markdownTreeCopyText(MarkdownTree* tree, MarkdownRange range);

// Returns the text of `range`.
inline PrStringView markdownTreeText(const MarkdownTree* tree, MarkdownRange range) {
    return PrStringView{tree->text.data + range.start, range.size};
}

// Appends the nodes under the root of `tree` to `out` in document order. Adjacent Text nodes of one kind whose ranges
// are adjacent in the text become one node. The ends of the nodes are positions in `out`.
void markdownTreeFlatten(const MarkdownTree* tree, Array<MarkdownNode>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
