// Studio Kit's splits and tabbed panels the user rearranges.
//
// A split divides its box among panes by weight, with a band between each pair of panes that the user drags to move
// weight between them. A dock is a model and a view of it. The model is a tree, owned by the dock, of splits and tabbed
// leaves that hold the application's panels. The view is the widget that declares the model each round. The view stages
// the rearrangements that its input asks for, and the next build applies them before it declares anything, so that the
// model never changes under the traversal that reads it.

#pragma once

#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Splits

// The thickness in px of the band between two panes.
static const float kPrKitSplitBand = 5.0f;

// The content of a split: panes that divide its box by weight. A pane's share of the box, less its bands, is its weight
// divided by the total of the weights.
typedef struct PrKitSplit {
    PrUiKey key;
    uint32_t count;                 // the number of panes, at least one
    float* weights;                 // one per pane, which a drag of a band writes
} PrKitSplit;

// The style of a split: the axis of its panes, the smallest share of the total that a drag leaves to either pane beside
// its band, and its placement in its parent.
typedef struct PrKitSplitStyle {
    PrKitDirection direction;
    float minShare;
    PrKitPlace place;
} PrKitSplitStyle;

// Creates a split under `parent`, and returns its box. Each pane is a column that clips its children. A drag of a band
// in the current round moves weight between the panes beside it, measured against the split's box at the last layout,
// before the panes are sized.
PrLayoutNode prKitSplit(PrKit* kit, PrLayoutNode parent, PrKitSplit split, PrKitSplitStyle style);

// Returns the split's pane at `index`, where the caller creates the pane's content.
PrLayoutNode prKitSplitPane(const PrKit* kit, PrLayoutNode split, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Dock models

// A tree of splits and tabbed leaves that hold panels. The tree always has a root, and a primary leaf that remains when
// its last panel leaves.
typedef struct PrKitDock PrKitDock;

// A node of a dock's tree, or kPrKitDockNodeNone. A removed node's id may name a node made later. A query of a node
// that is not in the tree returns a default, and a change to one does nothing.
typedef uint32_t PrKitDockNode;
enum { kPrKitDockNodeNone = 0 };

// The application's identity for a panel, which stays the same as the panel moves. Zero means none.
typedef uint64_t PrKitDockPanelId;

// What a node of a dock is.
enum {
    PrKitDockKind_Leaf,             // a strip of tabs, one per panel, and the open panel's content
    PrKitDockKind_Split,            // two or more children along an axis, sized by weight
};
typedef uint8_t PrKitDockKind;

// The side of a node that a new leaf goes to.
enum {
    PrKitDockEdge_Left,
    PrKitDockEdge_Right,
    PrKitDockEdge_Top,
    PrKitDockEdge_Bottom,
};
typedef uint8_t PrKitDockEdge;

// The payload kind of a panel's tab while it is dragged (prUiDragging). The payload's value is the panel.
static const uint64_t kPrKitDockDragKind = 0x646f636b70616e6cull;

// Creates a dock of one empty leaf, which is its root and its primary leaf.
PrKitDock* prKitDockCreate(void);

// Destroys the dock.
void prKitDockDestroy(PrKitDock* PR_NULLABLE dock);

// Returns true if `node` is a node of the dock's tree.
bool prKitDockNodeIsValid(const PrKitDock* dock, PrKitDockNode node);

// Returns the outermost node, and the primary leaf.
PrKitDockNode prKitDockRoot(const PrKitDock* dock);
PrKitDockNode prKitDockPrimary(const PrKitDock* dock);

// Returns what a node is, or a leaf for none.
PrKitDockKind prKitDockNodeKind(const PrKitDock* dock, PrKitDockNode node);

// Returns the split that a node is a child of, or none for the root.
PrKitDockNode prKitDockNodeParent(const PrKitDock* dock, PrKitDockNode node);

// Returns the leaf that holds `panel`, or none.
PrKitDockNode prKitDockPanelLeaf(const PrKitDock* dock, PrKitDockPanelId panel);

// Return the number of tabs in a leaf, the panel of tab `index` (or zero past the last tab), and the index of the tab
// whose panel is open.
uint32_t prKitDockLeafTabCount(const PrKitDock* dock, PrKitDockNode leaf);
PrKitDockPanelId prKitDockLeafTab(const PrKitDock* dock, PrKitDockNode leaf, uint32_t index);
uint32_t prKitDockLeafActiveTab(const PrKitDock* dock, PrKitDockNode leaf);

// Return the number of children of a split, its child `index` in order along its axis (or none past the last child),
// and that child's weight as a share of one.
uint32_t prKitDockSplitChildCount(const PrKitDock* dock, PrKitDockNode split);
PrKitDockNode prKitDockSplitChild(const PrKitDock* dock, PrKitDockNode split, uint32_t index);
float prKitDockSplitWeight(const PrKitDock* dock, PrKitDockNode split, uint32_t index);

// Returns the axis along which a split lays out its children.
PrKitDirection prKitDockSplitDirection(const PrKitDock* dock, PrKitDockNode split);

// Appends `panel`, which the dock does not hold, to the tabs of `leaf`, and opens it.
void prKitDockAddPanel(PrKitDock* dock, PrKitDockNode leaf, PrKitDockPanelId panel);

// Adds `panel`, which the dock does not hold, in a new leaf at `edge` of `leaf`. The new leaf takes `share` of the room
// that `leaf` had, or half for 0. If the parent split runs along the same axis, the new leaf becomes one of its
// children. Returns the new leaf, or none if `leaf` is not a leaf.
PrKitDockNode prKitDockSplit(PrKitDock* dock, PrKitDockNode leaf, PrKitDockEdge edge, PrKitDockPanelId panel, float share);

// Removes the tab of `panel`. A leaf that this empties, other than the primary leaf, is removed, and a split left with
// one child is replaced by that child. Returns true if the dock held the panel.
bool prKitDockRemovePanel(PrKitDock* dock, PrKitDockPanelId panel);

// Moves `panel` to tab `at` of `leaf`, or to the end if `at` is past it, and opens it.
void prKitDockMoveToTab(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockNode leaf, uint32_t at);

// Moves `panel` into a new leaf at `edge` of `leaf`, taking half of its room. A leaf's only panel stays where it is.
void prKitDockMoveToSplit(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockNode leaf, PrKitDockEdge edge);

// Moves `panel` into a new leaf along `edge` of the whole dock. The dock's only panel stays where it is.
void prKitDockMoveToEdge(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockEdge edge);

// Sets the weights of a split's children, scaled to sum to one. `count` must be the split's child count. A total of
// zero gives each child an equal share.
void prKitDockSplitSetWeights(PrKitDock* dock, PrKitDockNode split, const float* weights, uint32_t count);

// Opens tab `index` of `leaf`. An index past the last tab changes nothing.
void prKitDockLeafSetActiveTab(PrKitDock* dock, PrKitDockNode leaf, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Dock views

// What a panel's tab shows. The title stays valid until the frame commits.
typedef struct PrKitDockTab {
    PrStringView title;
    bool dirty;                     // the panel has unsaved changes
} PrKitDockTab;

// Returns what `panel`'s tab shows.
typedef PrKitDockTab (*PrKitDockDescribeFn)(void* PR_NULLABLE context, PrKitDockPanelId panel);

// The content of a dock's view: the dock's model, and the function that says what each panel's tab shows.
typedef struct PrKitDockView {
    PrUiKey key;
    PrKitDock* dock;
    PrKitDockDescribeFn describe;
    void* PR_NULLABLE context;
} PrKitDockView;

// A leaf's open panel, as the last build declared it: the box that the panel's content goes in, which the application
// fills.
typedef struct PrKitDockPane {
    PrKitDockPanelId panel;
    PrKitDockNode leaf;
    PrLayoutNode content;           // a column that clips its children, keyed by the panel
} PrKitDockPane;

// Applies the rearrangements that the last build staged, then creates the dock under `parent`: a split for each split,
// and for each leaf its strip of tabs, titled by `describe`, and the content box of its open panel. A panel's content
// box keeps its node, and so its subtree, as the panel moves between leaves. The round's input stages a tab's close,
// and a drop of a dragged tab on another leaf's tabs or content, or near an edge of a leaf or of the dock. While a tab
// is in flight, the overlay previews its drop (prUiOverlay). The dock's nodes are keyed under the dock's own, so docks
// in one tree never collide. Returns the dock's box.
PrLayoutNode prKitDockBuild(PrKit* kit, PrLayoutNode parent, PrKitDockView view);

// Return the number of panes that the last build declared, one for each leaf with an open panel, and pane `index`, in
// tree order. The panes are valid until the dock's next build.
uint32_t prKitDockPaneCount(const PrKitDock* dock);
PrKitDockPane prKitDockPane(const PrKitDock* dock, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
