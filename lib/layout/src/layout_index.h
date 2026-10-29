// Index domains: typedefs that name what an index counts, so that a signature, a field, or the `by` comment of an array
// says which kind of index it means.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A node of a tree, by its place in the tree's nodes (LayoutTree::nodes). Index 0 is no node.
typedef uint32_t LayoutNodeIndex;

// An item of an inline formatting context, by its place in the context's items (LayoutInlineShaping::items).
typedef uint32_t LayoutItemIndex;

// A face of the text font of a font group, by its match index in that text font (prTextFontMatchedFont).
typedef uint16_t LayoutFaceIndex;

} // namespace pr_impl

PR_ASSUME_NONNULL_END
