// Frames: a layout tree declared whole, every frame, by an immediate-mode producer.
//
// A frame creates the tree from scratch, and the next prLayoutTreeLayout commits it. Each creation matches a node of
// the committed tree that it continues, and returns that node's handle. The match is the next committed node in
// preorder, if it has the same key, kind and parent. Failing that, it is the committed node with its key, under any
// parent, or else the parent's next unkeyed committed child of its kind. A continued node keeps its caches, its scroll
// offset and its handle. A committed node that no creation continues is erased at the commit. A frame that creates the
// committed tree again changes nothing, and the layout after it costs the same as a layout after no change.
//
// Until the commit, the tree's queries, hit tests and painting read the committed tree. A frame's creations may come in
// any order across parents, each parent's children in their order. Creating in the committed tree's preorder costs
// least. A creation out of preorder, an insertion, a removal or a move costs a lookup and a relink of its parent.

#pragma once

#include "pr/base.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Begins a frame of the tree, which must have no frame open. While the frame is open, the tree is changed only through
// this header's functions and prLayoutNodeSetScroll.
void prLayoutTreeBeginFrame(PrLayoutTree* tree);

// Returns true if a frame of the tree has begun and not been committed.
bool prLayoutTreeIsInFrame(const PrLayoutTree* tree);

// Creates a node as the last child of `parent`, an element that the frame created, or as a root if `parent` is
// kPrLayoutNodeNone. Every root that a frame creates is a root of the tree that it commits, which a layout may lay out
// (prLayoutTreeLayout), and a root of the committed tree that the frame does not create again is erased. The node is an
// element styled by record `style`, a text node of `text` styled by its parent's record, a replaced element styled by
// `style` with no natural dimensions and no content, a semi-replaced element, or a line break. `key` is the node's
// identity, or 0 if its position among its siblings identifies it. A nonzero key is unique in the tree
// (prLayoutNodeSetKey). Returns the handle of the committed node that it continues, or of a new node.
PrLayoutNode prLayoutTreeFrameElement(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style);
PrLayoutNode prLayoutTreeFrameText(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrStringView text);
PrLayoutNode prLayoutTreeFrameReplaced(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style);
PrLayoutNode prLayoutTreeFrameSemiReplaced(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style);
PrLayoutNode prLayoutTreeFrameLineBreak(PrLayoutTree* tree, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex style);

// Sets the record of a node that the frame created. A later call may set it again. The commit compares the last record
// set with the committed node's record, so a caller that restyles a component's node costs only a store if the result
// matches the previous frame. A text node's record may be kPrLayoutStyleNone, meaning its parent's.
void prLayoutTreeFrameSetStyle(PrLayoutTree* tree, PrLayoutNode node, PrLayoutStyleIndex style);

// Returns the record that the frame last set on a node that it created.
PrLayoutStyleIndex prLayoutTreeFrameStyle(const PrLayoutTree* tree, PrLayoutNode node);

// Writes up to `capacity` roots to `out`: those that the open frame has created, or without an open frame those of the
// committed tree, in the order the frame created them. Returns the number of roots.
uint32_t prLayoutTreeFrameRoots(const PrLayoutTree* tree, PrLayoutNode* PR_NULLABLE out, uint32_t capacity);

// Returns the parent that a node was created under in the open frame, or none for a node created with none.
PrLayoutNode prLayoutTreeFrameParent(const PrLayoutTree* tree, PrLayoutNode node);

// Sets the text of a text node, or the natural size or content of a replaced element, that the frame created, if it
// differs from what the node holds. Each takes effect immediately, in the same way as the setters of pr/layout_tree.h.
void prLayoutTreeFrameSetText(PrLayoutTree* tree, PrLayoutNode text, PrStringView value);
void prLayoutTreeFrameSetNaturalSize(PrLayoutTree* tree, PrLayoutNode replaced, PrLayoutNaturalSize size);
void prLayoutTreeFrameSetContent(PrLayoutTree* tree, PrLayoutNode replaced, const PrLayoutContent* content);

// Sets the highlights of a text node that the frame created, or the tree's carets, which hold for the frame. Each takes
// effect immediately, as prLayoutNodeSetHighlights and prLayoutTreeSetCarets do. The commit removes the highlights of a
// node, and the carets, that its frame did not set.
void prLayoutTreeFrameSetHighlights(PrLayoutTree* tree, PrLayoutNode text,
    const PrLayoutHighlight* PR_NULLABLE highlights, uint32_t count);
void prLayoutTreeFrameSetCarets(PrLayoutTree* tree, const PrLayoutCaret* PR_NULLABLE carets, uint32_t count);

// Returns a node's slot: its index among the tree's nodes, below prLayoutTreeSlotCount. No two live nodes share a slot,
// and a node that a frame continues keeps its slot. A client can therefore keep data about nodes in arrays indexed by
// slot, each entry stored beside the handle it is for. An entry whose handle is not the node's belongs to a node erased
// since.
uint32_t prLayoutNodeSlot(PrLayoutNode node);

// Returns the number of slots, one past the greatest slot that a node of the tree has had.
uint32_t prLayoutTreeSlotCount(const PrLayoutTree* tree);

// Returns the slot of the committed node that the open frame's next creation continues if it follows the committed
// tree's preorder, or kPrLayoutFrameCursorEnd if the frame has passed the end of it. A frame that declares the tree of
// the last frame meets the same slots in the same order, so a client can key a cache of what it computes for each
// creation by this slot, with no node yet in hand, and find last frame's entry.
uint32_t prLayoutTreeFrameCursor(const PrLayoutTree* tree);

// The value of prLayoutTreeFrameCursor past the end of the committed preorder.
static const uint32_t kPrLayoutFrameCursorEnd = 0xffffffffu;

// A key that two creations of one frame gave. A key names one node of a frame, so the second creation is a caller's
// error. `first` is the node that holds the key. `second` is the node of the later creation, which the frame created
// without a key.
typedef struct PrLayoutDuplicateKey {
    uint64_t key;
    PrLayoutNode first;
    PrLayoutNode second;
} PrLayoutDuplicateKey;

// Writes up to `capacity` of the keys that the last committed frame gave twice to `out`, and returns how many there
// are. The nodes stay valid until the next commit, so a client can show them while it builds the next frame.
uint32_t prLayoutTreeDuplicateKeys(const PrLayoutTree* tree, PrLayoutDuplicateKey* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
