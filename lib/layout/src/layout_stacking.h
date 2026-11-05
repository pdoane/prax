// Stacking: the boxes each stacking context paints whole, sorted by stack level and tree order.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-position-4 - https://drafts.csswg.org/css-position-4/
//
// A stacking context paints the boxes in it that are below level 0 after its own decoration, and those at level 0 and
// above after its content, each level in tree order (CSS2/#elaborate-stacking-contexts,
// css-position-4/#paint-a-stacking-context). Which boxes those are, at which levels and in which contexts, depends only
// on the box tree and the styles. A context's list is found from the box tree when a paint first needs it. The tree
// keeps the list current as the tree changes: inserting, removing or restyling a box moves the entries of its subtree,
// each to its place by level and tree order. Layout changes no list. Each box of a list is where the last layout placed
// it (LayoutPlacement).

#pragma once

#include "pr/hash_index.h"
#include "pr/raw_array.h"

#include "layout/src/layout_fragment.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTree;

// A box that paints whole in a stacking context (layoutNodePaintsWhole), at its stack level.
struct LayoutStackedBox {
    LayoutNodeIndex node;
    int32_t level;
};

// A stacking context's list: the boxes that paint whole in it and that no stacking context inside it contains, sorted
// by level and then by tree order (CSS2/#elaborate-stacking-contexts). A positioned box at stack level auto is one such
// box, and the boxes that paint whole in it belong to its context's list, after it. The list depends only on the box
// tree and the styles, which layout does not change.
struct LayoutStackingList {
    LayoutNodeIndex context;        // kPrLayoutNodeNone if the list is free
    RawArray<LayoutStackedBox> boxes;
    uint32_t zero;                  // the first box at level 0 or above
    uint32_t version;               // raised when it is found or changes, from the tree's LayoutStacking::versions
    bool stale;                     // true if it must be found again before it is read
};

// A piece of a box that paints whole, other than the one that its placement gives (LayoutPlacement): a piece after a
// fragmentation break, or a fixed box that repeats on each page. Its position is in the viewport's content.
struct LayoutWholePiece {
    LayoutNodeIndex node;
    PrLayoutVec position;
    const LayoutFragment* fragment; // in the viewport's fragment, which holds it
};

// The other pieces of a box that paints whole, valid until the next layout or paint.
struct LayoutWholePieces {
    const LayoutWholePiece* PR_NULLABLE pieces;
    uint32_t count;
};

// The stacking contexts' lists. Each is found when a paint or a hit test first needs it, kept current as the tree
// changes, and found again if stale.
//
// A box that paints whole paints in its list each of its pieces. Its placement gives the first, and the others are in
// the fragments of the roots of fragmentation contexts that layout registers: a paginated root and multi-column
// containers. They are found from those fragments once per layout that changes the fragments, when a walk first needs
// them, at a cost proportional to the fragments that the roots hold.
struct LayoutStacking {
    RawArray<LayoutStackingList> lists;
    RawArray<uint32_t> freeLists;
    HashIndex byContext;              // `lists` by the context's node
    uint32_t versions;                // number of list versions given out since the tree was created
    uint32_t listsFound;              // number of lists found since the last layout or paint began
    RawArray<LayoutStackedBox> moved; // the boxes a change moves, which each change empties

    RawArray<LayoutNodeIndex> fragmentationRoots; // sorted, and may hold a node that is no longer a root
    RawArray<LayoutWholePiece> pieces;            // sorted by node
    uint32_t piecesLayout;                        // the LayoutTree::layouts that `pieces` were found at
    bool piecesFound;
};

// Returns true if the box of `node` paints whole in its stacking context. This is the case for an element or a replaced
// element, other than the root, that layoutPaintsWholeAs says paints whole, and for an inline box that
// layoutInlinePaintsWhole says paints whole. The As form asks this of the box as styled by `style`, as a change to its
// record would leave it.
bool layoutNodePaintsWholeAs(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);
bool layoutNodePaintsWhole(const LayoutTree* tree, LayoutNodeIndex node);

// Returns true if a box in the subtree of `node`, other than `node` itself, may paint whole
// (LayoutTreeNode::leadsWhole). If `node` is a stacking context, these are the boxes that its list holds.
bool layoutNodeLeadsWhole(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the stacking context that `node` paints in: its nearest ancestor that makes one, or kPrLayoutNodeNone for the
// root.
LayoutNodeIndex layoutNodeStackingContext(const LayoutTree* tree, LayoutNodeIndex node);

// The boxes of a stacking context's list. They are valid until the next change to the tree. Finding other lists does
// not change the tree.
struct LayoutStackedBoxes {
    const LayoutStackedBox* PR_NULLABLE boxes;
    uint32_t count;
    uint32_t zero;                  // the first box at level 0 or above
    uint32_t version;               // LayoutStackingList::version
};

// Returns the list of the stacking context `context`, finding it if the cache holds none or it is stale. The cost of
// finding it is proportional to the number of boxes in the context's subtree that lead to a box that paints whole
// (LayoutTreeNode::leadsWhole).
LayoutStackedBoxes layoutStackingList(const LayoutTree* tree, LayoutNodeIndex context);

// Returns true if the list of `context` that the cache holds is current and is the one found as `version`. A recording
// that painted from that list depends on this.
bool layoutStackingListIs(const LayoutTree* tree, LayoutNodeIndex context, uint32_t version);

// Removes the boxes of the subtree of `node` from the list of the context it paints in. Called because `node` is about
// to be removed, or because a change to its style or its parent's style is about to change which of the boxes paint
// whole, at which levels, and in which contexts. The cost is the number of the subtree's nodes that lead to a box that
// paints whole, plus, for each box removed, the logarithm of the number of boxes in the list.
void layoutStackingRemoving(LayoutTree* tree, LayoutNodeIndex node);

// Inserts the boxes of the subtree of `node` into the list of the context it paints in, each at its place by level and
// tree order. Called after `node` is inserted, or after the change that layoutStackingRemoving was told of is made.
// Marks the boxes around it as leading to it if it paints whole or leads to a box that does, and drops its own list if
// it makes no stacking context.
void layoutStackingInserted(LayoutTree* tree, LayoutNodeIndex node);

// Marks every list stale, because a change of the root makes and unmakes stacking contexts.
void layoutStackingStaleAll(LayoutTree* tree);

// Frees the list of `node`, which is being freed.
void layoutStackingForget(LayoutTree* tree, LayoutNodeIndex node);

// Frees the lists.
void layoutStackingReset(LayoutStacking& stacking);

// Records that `node` is the root of a fragmentation context, whose fragment may hold pieces of boxes that paint whole
// other than the ones that their placements give.
void layoutStackingAddFragmentationRoot(LayoutTree* tree, LayoutNodeIndex node);

// Returns the pieces of the box `node`, which paints whole, other than the one that its placement gives.
LayoutWholePieces layoutWholePiecesOf(const LayoutTree* tree, LayoutNodeIndex node);

// Checks each list that is not stale against finding it again from the box tree. Returns the context of the first list
// that differs, or kPrLayoutNodeNone.
uint32_t layoutStackingVerify(const LayoutTree* tree);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
