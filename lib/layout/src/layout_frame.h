// Frames: the state that a layout tree keeps for the frame an immediate-mode producer declares, alongside its committed
// nodes.

#pragma once

#include "pr/array.h"
#include "pr/layout_frame.h"

#include "layout/src/layout_index.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutTree;

// A committed node, as an entry of the committed tree in preorder: what a creation is compared with.
struct LayoutPreorderNode {
    LayoutNodeIndex node;
    LayoutNodeIndex parent;
    uint64_t key;
    PrLayoutStyleIndex style;
    uint32_t generation;
    PrLayoutNodeKind kind;
};

// One creation of a frame: the node, the parent it was created under, the record the frame set on it, and the record
// the node had when the frame began, which the commit compares with. A node that the frame made has no committed
// record.
struct LayoutCreation {
    LayoutNodeIndex node;
    LayoutNodeIndex parent;
    PrLayoutStyleIndex style;
    PrLayoutStyleIndex committed;
};

// A range of positions in the committed preorder whose nodes the open frame created in order.
struct LayoutPreorderRun {
    uint32_t start;
    uint32_t end;
};

// What a frame holds for a node whose parent's children may change: whether the commit relinks the node's children, the
// children the frame gave it, in order, and the next child the frame gave its parent. An entry is valid only if it is
// stamped with the frame.
struct LayoutFrameLinks {
    uint32_t frame;
    LayoutNodeIndex first;
    LayoutNodeIndex last;
    LayoutNodeIndex next;
    bool relinked;
};

// The frame of a tree.
//
// A frame compares each creation with the committed tree in preorder. A creation that is the next committed node in
// preorder, with the same key, kind and parent, continues that node, and costs one comparison and one append. Any other
// creation finds its node by its key, or by the parent's next unkeyed committed child of its kind, and the frame goes
// on in preorder from there. A creation that leaves its parent's children out of their committed order, a new node, or
// a node moved from another parent, marks its parent for the commit to relink. So a frame that declares the committed
// tree in another order across parents relinks nothing, and a change costs only where it is.
struct LayoutFrame {
    uint32_t frame;                     // incremented as each frame begins, starting at 1
    bool open;                          // true if a frame has begun and has not been committed
    Array<LayoutNodeIndex> roots;       // the roots of the committed tree, in the order the frame created them
    Array<LayoutNodeIndex> openRoots;   // the roots that the open frame found away from the cursor or made, in order

    // Scratch: the roots that the open frame created (gatherRoots).
    mutable Array<LayoutNodeIndex> gathered;

    // The committed tree in preorder, and the position of each committed node in it, by LayoutNodeIndex. They are
    // retaken at the next frame once an edit of the tree marks them stale.
    Array<LayoutPreorderNode> preorder;
    Array<uint32_t> preorderAt;
    bool preorderStale;

    // The frame's creations in order, and the position of each node's creation, by LayoutNodeIndex. A position names a
    // creation of the open frame only if the creation names the node.
    Array<LayoutCreation> created;
    Array<uint32_t> positions;

    // Where the frame is in the committed preorder: the position the next creation is compared with, the start of the
    // run of positions in order that it ends, the runs before it, and the start of the first earlier run after the
    // cursor, which the cursor must not reach, or the end of the preorder.
    uint32_t cursor;
    uint32_t runStart;
    uint32_t limit;
    Array<LayoutPreorderRun> runs;

    // The parents whose children the commit relinks, the nodes the frame made, and the number of creations that found
    // their node away from the cursor.
    Array<LayoutNodeIndex> relinks;
    Array<uint32_t> made;           // positions in `created`
    uint32_t jumps;

    // The keys that the open frame gave twice, and those of the last committed frame, which prLayoutTreeDuplicateKeys
    // reports.
    Array<PrLayoutDuplicateKey> duplicates;
    Array<PrLayoutDuplicateKey> reported;

    Array<LayoutFrameLinks> links;  // by LayoutNodeIndex
    Array<LayoutNodeIndex> gone;    // scratch: the committed nodes the commit takes out of the tree
};

// Commits the open frame (pr/layout_frame.h). Sets the values held by its creations on the committed nodes they
// continue, relinks the parents whose children changed, and erases the committed nodes that no creation continued.
void layoutFrameCommit(LayoutTree* tree);

// Frees the memory held by the frame state of a tree.
void layoutFrameReset(LayoutFrame& frame);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
