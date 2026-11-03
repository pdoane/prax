// Layout trees: the nodes, what has changed about them, and the state of each stage that reads them.
//
// A change to a node marks it for layout, and marks its ancestors as holding a change. For painting, it marks the node
// and its ancestors up to the first with a layer of its own. Each stage keeps its own state in the tree (the layout
// cache, the paint cache, the geometry queries' caches) and reads the marks to bound its work.

#pragma once

#include "pr/hash_index.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "layout/src/layout_anchor.h"
#include "layout/src/layout_cache.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_frame.h"
#include "layout/src/layout_paint.h"
#include "layout/src/layout_query.h"
#include "layout/src/layout_stacking.h"
#include "layout/src/layout_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutInlineShaping;
struct LayoutScratch;

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// What has changed about a node since the last layout.
enum {
    // Its own layout's inputs changed: its record's layout groups, its text, or its children.
    LayoutDirtyFlag_Self = 1 << 0,

    LayoutDirtyFlag_Descendant = 1 << 1,    // a descendant's layout inputs changed

    // A descendant's layout inputs changed, in a box out of flow whose holder is an ancestor of the node. The node's
    // own result, which holds only the box's placeholder, stays current.
    LayoutDirtyFlag_OutOfFlow = 1 << 2,

    // Every ancestor of the node is marked as holding a change (LayoutDirtyFlag_Descendant), whatever boxes out of flow
    // lie between.
    LayoutDirtyFlag_Ancestors = 1 << 3,
};
typedef uint8_t LayoutDirtyFlags;

// What the tree holds for a node, by its index. Links are node indices, or kPrLayoutNodeNone if there is none.
struct LayoutTreeNode {
    LayoutNodeIndex parent;
    LayoutNodeIndex first;
    LayoutNodeIndex last;
    LayoutNodeIndex previous;
    LayoutNodeIndex next;           // for a freed node, the next freed node
    PrLayoutStyleIndex style;       // the record, held by reference; for Text, kPrLayoutStyleNone means its parent's
    uint64_t key;
    union {
        uint32_t text;              // Text: its entry in the tree's texts
        PrLayoutVec scroll;         // Element: the offset its content is scrolled by, as set or as anchoring moved it
        uint32_t replaced;          // Replaced: its entry in the tree's replaced elements
    };

    // The paint count (LayoutPaintCache::paints) when what recordings read of the node last changed. Recordings read
    // its record's groups other than its effects, which its text also paints by, and what the recordings of its subtree
    // read. A recording made by a later paint is current. A change in the subtree of a box that has a layer of its own
    // reaches no further than the box, whose own recording holds the change.
    uint32_t paintChanged;
    uint32_t paintSubtree;

    uint32_t generation;            // raised when the node is freed, so that its old names name nothing (PrLayoutNode)
    PrLayoutNodeKind kind;
    LayoutDirtyFlags dirty : 4;
    PrLayoutRenderedFlags rendered : 4;
    bool live : 1;                 // true if the index names a node rather than a freed slot
    bool readsViewport : 1;        // its layout or intrinsic sizes read the viewport's extent, and it is listed

    // Element, Replaced, LineBreak: true if its record's writing mode is vertical. Layout reads this for every box it
    // reaches without reading the record.
    bool vertical : 1;

    // Element, Replaced: true if its record's position is absolute or fixed and its display is not none, which takes
    // its box out of flow unless it is the root (layoutIsOutOfFlow).
    bool positioned : 1;

    // How it takes part in its parent's layout as a node that is not the root, as a LayoutChildKind (layoutChildKind).
    // It is kept when its record is set, because every walk over a container's children reads it.
    uint8_t childKind : 3;

    // Element: how its record lays out its children, as a LayoutChildren. layoutInItemContainer and layoutInTable read
    // it from a child's parent.
    uint8_t children : 2;

    // Element: `justifies` is set if its record sets a justify-self other than auto or normal, and `justifiesItems` if
    // it sets a justify-items other than normal, so that block layout reads neither if neither is set
    // (LayoutStyleTraits).
    bool justifies : 1;
    bool justifiesItems : 1;

    // Element, Replaced: true if its record has a background layer fixed to the viewport. The node is then listed in
    // LayoutTree::viewportPainters.
    bool fixedBackground : 1;

    // True if it or a box in its subtree may paint whole (layoutNodePaintsWhole). Finding a stacking context's list
    // follows these flags. A change sets the flag along the ancestors. Nothing clears it, so it may lead to nothing.
    bool leadsWhole : 1;

    // Element, Replaced: true if its record changed, since the last layout, a value whose change suppresses scroll
    // anchoring for an anchor in its subtree (LayoutAnchoring::suppressing).
    bool anchorSuppressed : 1;

    // True if the table of its kind lists it: an element that has a record for the first formatted line
    // (LayoutTree::firstLines), or text that has highlights (LayoutTree::highlights).
    bool listed : 1;
};

// Every node pays for each byte of the struct, so data that only one kind holds shares a union, or lives in a table of
// its own.
static_assert(sizeof(LayoutTreeNode) == 56);

// An element's record for the first formatted line of its block container (prLayoutNodeSetFirstLineStyle), which the
// tree holds a reference to.
struct LayoutFirstLine {
    LayoutNodeIndex node;
    PrLayoutStyleIndex style;
};

// The highlights of a text node (prLayoutNodeSetHighlights), in paint order, and the frame that set them, or 0 if no
// frame did (LayoutFrame::frame).
struct LayoutHighlights {
    LayoutNodeIndex node;
    uint32_t frame;
    RawArray<PrLayoutHighlight> ranges;

    // The ranges in order of their starts, by index of `ranges`, and the furthest end of the ranges up to each place in
    // that order, so that a part of the text finds the ranges that reach it without reading the others.
    RawArray<uint32_t> byStart;
    RawArray<uint32_t> reach;
};

// The carets that a tree paints (prLayoutTreeSetCarets), and the frame that set them, or 0 if no frame did.
struct LayoutTreeCarets {
    RawArray<PrLayoutCaret> carets;
    uint32_t frame;
    bool hidden;                    // the carets paint nothing (prLayoutTreeSetCaretsHidden)
};

// Returns true if a node of `kind` is an element, whose children are laid out in it, or a box that its record styles.
inline bool layoutKindIsElement(PrLayoutNodeKind kind) {
    return (kind & PrLayoutNodeKindFlag_Element) != 0;
}
inline bool layoutKindIsBox(PrLayoutNodeKind kind) {
    return (kind & PrLayoutNodeKindFlag_Box) != 0;
}

// What a replaced element holds beyond a node.
struct LayoutReplaced {
    PrLayoutNaturalSize natural;

    // What it draws: a list, or the drawing of a painter, which it holds a reference to; the painter, whose context it
    // holds a reference to if the painter shares it; the image, which it holds a reference to; and the extent that a
    // painter's drawing is for, the size that fitting gives it.
    PrLayoutContentKind contentKind;
    mutable PrDrawList* PR_NULLABLE content;
    PrLayoutPainter painter;
    const PrDrawImage* PR_NULLABLE image;
    mutable PrLayoutVec painted;
    float limit;                      // Image: the brightest that the image paints (PrLayoutContent::limit)
    PrLayoutImageRendering rendering; // Image
    uint64_t embed;                   // Embed: its id
    uint64_t revision;                // PrLayoutContent::revision

    // PrLayoutContent::text
    PrRawString (*PR_NULLABLE text)(void* PR_NULLABLE textContext);
    void* PR_NULLABLE textContext;
};

// ---------------------------------------------------------------------------------------------------------------------
// Trees

// The kinds of input that a change to a tree reaches. Each stage's work is bounded by the inputs it reads: layout reads
// Layout inputs, and painting reads Layout and Paint inputs. Effects inputs apply around recordings, so neither stage
// needs them. Scroll offsets are not an input of either stage, nor of a scene. prLayoutTreeVerify checks the work
// counters against these rules.
enum {
    LayoutInputFlag_Layout = 1 << 0,    // layout groups, text, children, natural sizes, the viewport's extent
    LayoutInputFlag_Paint = 1 << 1,     // paint groups, replaced content, and whether a box has a layer
    LayoutInputFlag_Effects = 1 << 2,   // a layer's opacity or transform, and whether the caret is hidden
};
typedef uint8_t LayoutInputFlags;

// A root that the last layout laid out other than the viewport's: its node, the extent of its containing block, the
// fragment that layoutViewport made for it, and its drawing as of the last paint (prLayoutNodeDraw), or null if no
// paint has drawn it. The tree holds the fragment and the drawing.
struct LayoutLaidRoot {
    LayoutNodeIndex node;
    PrLayoutVec extent;
    const LayoutFragment* PR_NULLABLE fragment;
    PrDrawList* PR_NULLABLE drawing;
};

// A root that the last drawing of a replaced node's painter drew.
struct LayoutRootReader {
    LayoutNodeIndex replaced;
    LayoutNodeIndex root;
};

// A layout tree.
struct LayoutTree {
    LayoutStyles* styles;           // owned by the tree

    // --- Nodes ---

    RawArray<LayoutTreeNode> nodes;    // by LayoutNodeIndex; the first is unused
    LayoutNodeIndex freeNodes;         // the first freed node, or kPrLayoutNodeNone
    RawArray<LayoutReplaced> replaced; // of the replaced nodes, which name their entries
    RawArray<uint32_t> freeReplaced;   // entries that no node names
    RawArray<PrRawString> texts;       // of the text nodes, which name their entries
    RawArray<uint32_t> freeTexts;      // entries that no node names
    HashIndex keys;                    // the nodes that have keys, by their keys
    LayoutNodeIndex root;              // the viewport's root in the last layout, or kPrLayoutNodeNone
    PrLayoutNode rootName;             // the handle of `root`, which a layout given the same root compares
    PrLayoutVec viewportScroll;        // as set, or as scroll anchoring moved it
    PrColor16 canvasColor;             // the canvas beneath the viewport's root, straight alpha, as set
    PrLayoutWheelEvents viewportWheelEvents;
    LayoutFrame frame;

    // --- Changes ---

    // The inputs changed since the last layout and since the last paint, and the inputs that the last layout and the
    // last paint consumed.
    LayoutInputFlags pendingLayout;
    LayoutInputFlags pendingPaint;
    LayoutInputFlags laidOutFor;
    LayoutInputFlags paintedFor;

    uint32_t effects;                // raised when a node's effects change, which layout does not see
    uint32_t metricsGeneration;      // the styles' metrics generation at the last layout, which all laid-out boxes use
    RawArray<uint64_t> changedFonts; // scratch: a bit set of the font groups whose answers changed (layoutSyncFonts)

    // The nodes whose layout or intrinsic sizes read the viewport's extent, which no space carries. A change of the
    // extent marks each of them dirty.
    RawArray<uint32_t> viewportReaders;

    // The boxes that have a background layer fixed to the viewport (LayoutTreeNode::fixedBackground). Their recordings
    // depend on the viewport's extent, so a change of the extent marks each of them painted.
    RawArray<uint32_t> viewportPainters;

    // --- Layout ---

    const LayoutFragment* PR_NULLABLE fragment; // the viewport's fragment from the last layout, held by the tree
    PrLayoutVec viewport;                       // the extent the last layout was given
    RawArray<LayoutLaidRoot> roots;             // the last layout's other roots, in the order it was given them
    uint32_t layouts;                           // raised by each layout that changes the fragments
    LayoutCache cache;

    // By the first node of each inline formatting context: the shaping of its text, which does not depend on its width,
    // or null.
    RawArray<LayoutInlineShaping* PR_NULLABLE> shapings;

    // The records of the elements that have one for the first formatted line (prLayoutNodeSetFirstLineStyle), indexed
    // by node. Few elements have one.
    RawArray<LayoutFirstLine> firstLines;
    HashIndex firstLineIndex;

    // The highlights of the text nodes that have them, indexed by node, and the caret. Few nodes have highlights.
    RawArray<LayoutHighlights> highlights;
    HashIndex highlightIndex;
    LayoutTreeCarets carets;

    LayoutScratch* PR_NULLABLE scratch;   // memory that layouts reuse, created by the first layout

    // --- Painting and queries ---

    LayoutPaintCache paint;
    mutable LayoutStacking stacking; // filled by the walks of painting and queries
    mutable LayoutQueries queries;   // filled by queries, which do not change the tree
    PrLayoutTreeStats stats;         // of the last layout and the last paint

    // The roots that replaced nodes' painters drew in their last drawings, and the replaced node whose painter runs, or
    // kPrLayoutNodeNone. A paint that draws a root again draws again the replaced nodes that drew it.
    mutable RawArray<LayoutRootReader> rootReaders;
    mutable LayoutNodeIndex painting;

    LayoutAnchoring anchoring;

    // The last paint's scene, which the tree holds a reference to, and the viewport's fragment that it painted. A paint
    // returns the scene again if nothing that a scene reads has changed.
    const PrLayoutScene* PR_NULLABLE scene;
    const LayoutFragment* PR_NULLABLE sceneFragment;
};

// Takes a freed node, or a new one, of `kind`. The node is detached, marked as changed, and has no record. It has an
// empty text or replaced entry if its kind has one.
LayoutNodeIndex layoutTreeCreateNode(LayoutTree* tree, PrLayoutNodeKind kind);

// Sets the first record of a node that layoutTreeCreateNode made, holding a reference to it. A text node's record may
// be kPrLayoutStyleNone, meaning its parent's.
void layoutTreeSetNewStyle(LayoutTree* tree, LayoutNodeIndex node, PrLayoutStyleIndex style);

// The tree's edits, by node index. Each makes the edit of the pr/layout_tree.h function that it is named for, including
// the marks.
void layoutTreeChangeStyle(LayoutTree* tree, LayoutNodeIndex index, PrLayoutStyleIndex style);
void layoutTreeInsert(LayoutTree* tree, LayoutNodeIndex parentIndex, LayoutNodeIndex index,
    LayoutNodeIndex beforeIndex);
void layoutTreeRemove(LayoutTree* tree, LayoutNodeIndex index);
void layoutTreeSetKey(LayoutTree* tree, LayoutNodeIndex index, uint64_t key);

// Drops the drawing of the replaced node's painter, and marks the node painted as of paint `paints`, so that the next
// recording that holds it draws it again. A paint that has begun marks a node as of the paint before it, which leaves
// the recordings that it makes current at the next paint.
void layoutTreeRepaintContent(LayoutTree* tree, LayoutNodeIndex replaced, uint32_t paints);

// Returns true if the node is a root that the last layout laid out other than the viewport's (LayoutTree::roots).
bool layoutTreeIsLaidOutRoot(const LayoutTree* tree, LayoutNodeIndex index);

// Erases a detached node and its descendants.
void layoutTreeEraseDetached(LayoutTree* tree, LayoutNodeIndex node);

// Notes that the node's layout or intrinsic sizes read the viewport's extent, so that a change of it lays the node out
// again (LayoutTree::viewportReaders).
void layoutTreeReadViewport(LayoutTree* tree, LayoutNodeIndex node);

// Returns what the tree holds for a live node.
inline const LayoutTreeNode& layoutTreeNode(const LayoutTree* tree, LayoutNodeIndex node) {
    PR_ASSERT(node != kPrLayoutNodeNone && node < tree->nodes.count && tree->nodes.data[node].live);
    return tree->nodes.data[node];
}

// Returns the root of the node's tree: the node itself or its ancestor that has no parent.
inline LayoutNodeIndex layoutNodeRoot(const LayoutTree* tree, LayoutNodeIndex node) {
    while (tree->nodes.data[node].parent != kPrLayoutNodeNone)
        node = tree->nodes.data[node].parent;
    return node;
}

// Returns the index of the node that `node` names, or kPrLayoutNodeNone for none. The node is live.
inline LayoutNodeIndex layoutNodeIndex(const LayoutTree* tree, PrLayoutNode node) {
    LayoutNodeIndex index = LayoutNodeIndex(node);
    PR_ASSERT(node == kPrLayoutNodeNone ||
        (index != kPrLayoutNodeNone && index < tree->nodes.count && tree->nodes.data[index].live &&
            tree->nodes.data[index].generation == uint32_t(node >> 32)));
    return index;
}

// Returns the handle that names the node at `index`, or kPrLayoutNodeNone for none.
inline PrLayoutNode layoutNodeName(const LayoutTree* tree, LayoutNodeIndex index) {
    if (index == kPrLayoutNodeNone)
        return kPrLayoutNodeNone;
    return PrLayoutNode(tree->nodes.data[index].generation) << 32 | index;
}

// Returns a node's scroll offset as set: an element's, the viewport's for kPrLayoutNodeNone, and zero for any other
// node.
inline PrLayoutVec layoutNodeScroll(const LayoutTree* tree, LayoutNodeIndex node) {
    if (node == kPrLayoutNodeNone)
        return tree->viewportScroll;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    return layoutKindIsElement(n.kind) ? n.scroll : PrLayoutVec{};
}

// Returns a text node's text, valid until the tree's text next changes.
inline PrStringView layoutNodeText(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    PR_ASSERT(n.kind == PrLayoutNodeKind_Text);
    return prRawStringView(&tree->texts.data[n.text]);
}

// Returns what the replaced node `node` holds beyond a node.
inline LayoutReplaced& layoutReplaced(LayoutTree* tree, LayoutNodeIndex node) {
    PR_ASSERT(layoutTreeNode(tree, node).kind == PrLayoutNodeKind_Replaced);
    return tree->replaced.data[layoutTreeNode(tree, node).replaced];
}
inline const LayoutReplaced& layoutReplaced(const LayoutTree* tree, LayoutNodeIndex node) {
    PR_ASSERT(layoutTreeNode(tree, node).kind == PrLayoutNodeKind_Replaced);
    return tree->replaced.data[layoutTreeNode(tree, node).replaced];
}

// Returns the record that styles a node: the node's own record for an element, a replaced element or a line break, and
// for a text node, its own record if it has one and otherwise its parent's.
inline uint32_t layoutNodeStyle(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.style != kPrLayoutStyleNone)
        return n.style;
    return n.parent != kPrLayoutNodeNone ? layoutTreeNode(tree, n.parent).style : 0;
}

// Returns the hash by which LayoutTree::firstLineIndex finds `node`.
inline uint32_t layoutFirstLineHash(LayoutNodeIndex node) {
    return KeyIndex::hashOf(node);
}

// Returns the hash by which LayoutTree::highlightIndex finds `node`.
inline uint32_t layoutHighlightHash(LayoutNodeIndex node) {
    return KeyIndex::hashOf(node);
}

// Returns the highlights of the text `node`, or null if it has none.
const LayoutHighlights* PR_NULLABLE layoutTextHighlights(const LayoutTree* tree, LayoutNodeIndex node);

// Writes to `out` the indices of the ranges of `highlights` that reach the bytes from `start` to `end`, in paint order.
// The cost follows the number of ranges that begin before `end` and reach past the last range that ends before `start`.
void layoutHighlightsReaching(const LayoutHighlights* highlights, uint32_t start, uint32_t end, RawArray<uint32_t>& out);

// Sets the highlights of the text `node` (prLayoutNodeSetHighlights) on behalf of `frame`, or of no frame for 0.
void layoutTreeSetHighlights(LayoutTree* tree, LayoutNodeIndex node, const PrLayoutHighlight* PR_NULLABLE highlights,
    uint32_t count, uint32_t frame);

// Sets the carets (prLayoutTreeSetCarets) on behalf of `frame`, or of no frame for 0.
void layoutTreeSetCarets(LayoutTree* tree, const PrLayoutCaret* PR_NULLABLE carets, uint32_t count, uint32_t frame);

// Removes the highlights and the caret that a frame other than `frame` set.
void layoutTreeExpireFrameMarks(LayoutTree* tree, uint32_t frame);

// Returns the record of the element `node` for the first formatted line, or kPrLayoutStyleNone if it has none.
PrLayoutStyleIndex layoutFirstLineStyle(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the record that styles a node on the first formatted line of its block container. For an element, this is its
// record for that line if it has one. For a text node that takes its parent's record, this is its parent's record for
// that line. Otherwise it is the node's record (layoutNodeStyle).
uint32_t layoutNodeLineStyle(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the record that a fragment's node paints in: its record for the first formatted line if the fragment is on
// that line (LayoutFragmentFlag_FirstLine), and its own otherwise.
inline PrLayoutStyleIndex layoutFragmentStyle(const LayoutTree* tree, const LayoutFragment* fragment) {
    if ((fragment->flags & LayoutFragmentFlag_FirstLine) != 0)
        return layoutNodeLineStyle(tree, fragment->node);
    return layoutNodeStyle(tree, fragment->node);
}

} // namespace pr_impl

// C opaque types
struct PrLayoutTree : pr_impl::LayoutTree {};

PR_ASSUME_NONNULL_END
