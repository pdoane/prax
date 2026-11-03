// Geometry queries: the boxes of a tree's last layout, and hit testing.
//
// A node's boxes are where the last layout placed it (LayoutPlacement), mapped through the boxes that place it: through
// their placements and, in viewport coordinates, through their transforms and scroll offsets. A query's cost grows with
// the depth of its node, and with the number of pieces of an inline box or a text. A hit test enters only the subtrees
// whose area holds the point. The tree caches each fragment's area while it holds the fragment and the effects of the
// fragment's nodes are unchanged.

#pragma once

#include "pr/array.h"
#include "pr/hash_index.h"
#include "pr/layout_query.h"

#include "layout/src/layout_fragment.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTree;

// The area in which a fragment's subtree can be hit, from its top-left corner.
struct LayoutHitBound {
    const LayoutFragment* fragment; // which the queries hold a reference to
    PrLayoutRect bounds;
};

struct LayoutInlineShaping;

// The item of a text node in the shaping of its context.
struct LayoutRenderedItem {
    const LayoutInlineShaping* PR_NULLABLE shaping;
    uint32_t item;
};

// The geometry queries' caches.
struct LayoutQueries {
    Array<PrLayoutRect> boxes;      // scratch: a query's boxes

    // The hit areas hit tests found. Each is valid while the tree holds its fragment and the effects of the tree's
    // nodes are those it was found for.
    uint32_t hitEffects;            // the tree's effects epoch that they were found for
    Array<LayoutHitBound> hitBounds;
    HashIndex hitIndex;             // by the fragment's pointer

    // Where each text node's item is, as of the layout `renderedLayouts` counts (prLayoutNodeRenderedText).
    uint32_t renderedLayouts;
    Array<LayoutRenderedItem> rendered;     // by LayoutNodeIndex
};

// Returns true if the node is in the tree's last layout: neither it nor any of its ancestors is absent from it, up to a
// root that the layout laid out.
bool layoutNodeLaidOut(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the text fragment of the text `node` in the last layout that a caret before `offset` stands in, where
// `affinity` places it (prLayoutNodeTextCaret), or null if the layout drew none of the node's text.
const LayoutFragment* PR_NULLABLE layoutCaretFragment(const LayoutTree* tree, LayoutNodeIndex node, uint32_t offset,
    PrLayoutTextAffinity affinity);

// Frees the queries' caches.
void layoutQueriesReset(LayoutQueries& queries);

// Drops the cache entries of the fragments that left the final tree in the tree's last layout (LayoutCache::retired).
void layoutQueriesRetire(LayoutTree* tree);

// Returns true if the placements that the last layout recorded match a fresh recording from its fragments. Writes the
// first node that differs.
bool layoutQueriesVerify(LayoutTree* tree, uint32_t* node);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
