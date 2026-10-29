// Studio Kit's splits, and tabbed panels that the user rearranges.

#include "pr/kit_dock.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/kit_button.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

// One split or leaf of a dock's tree. A removed node keeps its arrays' storage for the next node that takes its id.
struct DockNode {
    Array<PrKitDockNode> children;  // Split: its children, in order along its axis
    Array<float> weights;           // Split: its children's shares, by child index, summing to one
    Array<PrKitDockPanelId> tabs;   // Leaf: its panels, in the order of its tab strip
    PrKitDockNode parent;
    uint32_t active;                // a leaf's open tab
    PrKitDockKind kind;
    PrKitDirection direction;       // a split's axis
    bool alive;
};

// What a staged rearrangement does.
enum : uint8_t {
    DockOpKind_Remove,                  // removes `panel`
    DockOpKind_ToTab,                   // moves `panel` to tab `index` of `leaf`
    DockOpKind_ToSplit,                 // moves `panel` to a new leaf at `edge` of `leaf`
    DockOpKind_ToEdge,                  // moves `panel` to a new leaf at `edge` of the dock
};
typedef uint8_t DockOpKind;

// A rearrangement that a build staged for the next build to apply.
struct DockOp {
    PrKitDockPanelId panel;
    PrKitDockNode leaf;
    uint32_t index;
    DockOpKind kind;
    PrKitDockEdge edge;
};

// A dock's tree, the rearrangements staged against it, and what its last build declared.
struct Dock {
    Array<DockNode> nodes;          // by PrKitDockNode; the first entry stands for none and is never alive
    Array<PrKitDockNode> spare;     // removed nodes, whose ids new nodes take
    Array<DockOp> staged;
    Array<PrKitDockPane> panes;     // the open panels that the last build declared, in tree order
    Array<PrLayoutNode> scratch;    // the build's nodes for the splits and strips that it is inside, innermost last
    PrKitDockNode root;
    PrKitDockNode primary;
};

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Splits

// The smallest share of a split that a dock's band leaves to either pane beside it.
constexpr float kDockMinShare = 0.04f;

// The state a band is drawn in.
enum : uint8_t {
    BandState_Rest,
    BandState_Hover,
    BandState_Press,
};
typedef uint8_t BandState;

// A band between two panes of a split along `direction`: a strip kPrKitSplitBand px across, which centers its rule.
PR_NO_PADDING_BEGIN
struct BandBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitDirection direction;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        (direction == PrKitDirection_Row ? box.width : box.height) = prLayoutLengthPx(kPrKitSplitBand);
    }
};
struct BandFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    PrKitDirection direction;

    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.direction = direction == PrKitDirection_Row ? PrLayoutFlexDirection_Row : PrLayoutFlexDirection_Column;
        flex.justifyContent = PrLayoutAlignment_Center;
        flex.shrink = 0;
    }
};

// The line the eye follows between the panes, one px across, stretched across the band by its alignment.
struct BandRuleBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitDirection direction;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        (direction == PrKitDirection_Row ? box.width : box.height) = prLayoutLengthPx(1);
    }
};
PR_NO_PADDING_END

// Creates the band between two panes of a split along `direction`.
PrLayoutNode splitBand(Kit& kit, PrLayoutNode box, PrKitDirection direction) {
    PrLayoutNode band = prUiElement(kit.ui, box, 0, 0, PrUiFlag_Pressable);
    bool enabled = (prUiFlags(kit.ui, band) & PrUiFlag_Disabled) == 0;
    BandState state = prUiIsPressed(kit.ui, band, 0)                ? BandState_Press
                    : enabled && prUiIsHovered(kit.ui, band) ? BandState_Hover
                                                           : BandState_Rest;
    const PrKitPalette& p = kit.theme.palette;
    KitColors wash{.background = state == BandState_Rest ? PrColor{0, 0, 0, 0} : p.accentWeak};
    prLayoutTreeFrameSetStyle(kit.tree, band,
                              kitStyle(kit, BandBox{.direction = direction}, BandFlex{.direction = direction}, wash));
    KitColors line{.background = state == BandState_Press ? p.accent : p.line1};
    prLayoutTreeFrameElement(kit.tree, band, 0, kitStyle(kit, BandRuleBox{.direction = direction}, KitFixed{}, line));
    if (enabled)
        prUiSetCursor(kit.ui, band, direction == PrKitDirection_Row ? PrAppCursor_ResizeX : PrAppCursor_ResizeY);
    return band;
}

// Moves weight between `weights[index]` and the next weight by `motion` px of the `free` px that the weights divide.
// Each weight keeps at least `minShare` of their total.
void moveWeights(float* weights, uint32_t count, uint32_t index, float minShare, float free, float motion) {
    if (index + 1 >= count || free <= 1 || motion == 0)
        return;
    float total = 0;
    for (uint32_t i = 0; i < count; ++i)
        total += weights[i];
    if (total <= 0)
        return;
    float lowest = minShare * total;
    float applied = motion / free * total;
    if (weights[index] + applied < lowest)
        applied = lowest - weights[index];
    if (weights[index + 1] - applied < lowest)
        applied = weights[index + 1] - lowest;
    weights[index] += applied;
    weights[index + 1] -= applied;
}

// A pane: a column that clips its children, sized by its share of the split alone. The share moves with the weights,
// and only this group with it.
PR_NO_PADDING_BEGIN
struct PaneFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    float grow;

    void make(const Kit& kit, PrLayoutFlexStyle& flex) const {
        kitContainerFlex(PrKitBoxStyle{}).make(kit, flex);
        flex.grow = grow;
        flex.shrink = 1;
        flex.basis = prLayoutLengthPx(0);
    }
};
PR_NO_PADDING_END

// Returns the record of a pane that grows by `grow`.
PrLayoutStyleIndex paneStyle(Kit& kit, float grow) {
    return kitStyle(kit, KitContainerBox{.style = {.overflow = PrKitOverflow_Clip}}, PaneFlex{.grow = grow});
}

// Returns the motion of `drag` in the round along `box`'s own x axis if `across` is set, or its y axis otherwise, so
// that a band follows the pointer under a transform of the box. Returns zero if either end of the motion does not reach
// the box's plane.
float motionIn(const Kit& kit, PrLayoutNode box, const PrUiDrag& drag, bool across) {
    PrVec2 from{};
    PrVec2 to{};
    PrVec2 start{drag.position.x - drag.delta.x, drag.position.y - drag.delta.y};
    if (!prUiLocalPoint(kit.ui, box, start, &from) || !prUiLocalPoint(kit.ui, box, drag.position, &to))
        return 0;
    return across ? to.x - from.x : to.y - from.y;
}

// Creates `count` panes under a split's `box`, with a band between each pair. Applies the round's drag of each band to
// `weights`, then sizes the panes by the weights. Writes the panes to `panes`.
void splitPanes(Kit& kit, PrLayoutNode box, PrKitDirection direction, float* weights, uint32_t count, float minShare,
                PrLayoutNode* panes) {
    PrLayoutTree* tree = kit.tree;
    PrVec2 size = kitExtent(kit, box);
    bool across = direction == PrKitDirection_Row;
    float extent = across ? size.x : size.y;
    float free = extent - float(count - 1) * kPrKitSplitBand;
    for (uint32_t i = 0; i < count; ++i) {
        panes[i] = prLayoutTreeFrameElement(tree, box, 0, 0);
        if (i + 1 == count)
            break;
        PrLayoutNode band = splitBand(kit, box, direction);
        PrUiDrag drag{};
        if (prUiDrag(kit.ui, band, 0, 0, &drag) && !drag.cancelled)
            moveWeights(weights, count, i, minShare, free, motionIn(kit, box, drag, across));
    }
    // Grow factors that sum to the pane count, so that the panes fill the box regardless of the weights' total.
    float total = 0;
    for (uint32_t i = 0; i < count; ++i)
        total += weights[i] > 0 ? weights[i] : 0;
    for (uint32_t i = 0; i < count; ++i) {
        float weight = weights[i] > 0 ? weights[i] : 0;
        float grow = total > 0 ? weight / total * float(count) : 1;
        prLayoutTreeFrameSetStyle(tree, panes[i], paneStyle(kit, grow));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Models

bool isAlive(const Dock& dock, PrKitDockNode node) {
    return node != kPrKitDockNodeNone && node < dock.nodes.count && dock.nodes.data[node].alive;
}

bool isLeaf(const Dock& dock, PrKitDockNode node) {
    return isAlive(dock, node) && dock.nodes.data[node].kind == PrKitDockKind_Leaf;
}

bool isSplit(const Dock& dock, PrKitDockNode node) {
    return isAlive(dock, node) && dock.nodes.data[node].kind == PrKitDockKind_Split;
}

DockNode& nodeAt(Dock& dock, PrKitDockNode node) {
    PR_ASSERT(isAlive(dock, node));
    return dock.nodes.data[node];
}

// Makes a node of `kind` with no parent. The node table may move.
PrKitDockNode makeNode(Dock& dock, PrKitDockKind kind) {
    if (dock.nodes.count == 0)
        dock.nodes.push(DockNode{});
    PrKitDockNode id = kPrKitDockNodeNone;
    if (dock.spare.count != 0) {
        id = dock.spare.last();
        dock.spare.pop();
    } else {
        id = dock.nodes.count;
        dock.nodes.push(DockNode{});
    }
    DockNode& node = dock.nodes.data[id];
    node.children.clear();
    node.weights.clear();
    node.tabs.clear();
    node.parent = kPrKitDockNodeNone;
    node.active = false;
    node.kind = kind;
    node.direction = PrKitDirection_Row;
    node.alive = true;
    return id;
}

void freeNode(Dock& dock, PrKitDockNode id) {
    DockNode& node = nodeAt(dock, id);
    node.alive = false;
    dock.spare.push(id);
}

uint32_t indexInParent(Dock& dock, PrKitDockNode node) {
    PrKitDockNode parent = nodeAt(dock, node).parent;
    uint32_t at = nodeAt(dock, parent).children.find(node);
    PR_ASSERT(at != kArrayMissing);
    return at;
}

// Scales a split's weights to sum to one, or gives each an equal share if they sum to zero.
void normalizeWeights(DockNode& split) {
    float total = 0;
    for (uint32_t i = 0; i < split.weights.count; ++i)
        total += split.weights.data[i];
    for (uint32_t i = 0; i < split.weights.count; ++i)
        split.weights.data[i] = total > 0 ? split.weights.data[i] / total : 1.0f / float(split.weights.count);
}

// Puts `replacement` in the place that `node` holds in its parent, or at the root.
void replaceChild(Dock& dock, PrKitDockNode node, PrKitDockNode replacement) {
    PrKitDockNode parent = nodeAt(dock, node).parent;
    nodeAt(dock, replacement).parent = parent;
    if (parent == kPrKitDockNodeNone)
        dock.root = replacement;
    else
        nodeAt(dock, parent).children.data[indexInParent(dock, node)] = replacement;
}

// Splices a split into a parent along the same axis, scaling its children's weights into the share that it had.
void flattenSplit(Dock& dock, PrKitDockNode split) {
    PrKitDockNode parentId = nodeAt(dock, split).parent;
    if (parentId == kPrKitDockNodeNone || nodeAt(dock, parentId).direction != nodeAt(dock, split).direction)
        return;
    uint32_t at = indexInParent(dock, split);
    DockNode& parent = nodeAt(dock, parentId);
    DockNode& inner = nodeAt(dock, split);
    float slot = parent.weights.data[at];
    parent.children.erase(at);
    parent.weights.erase(at);
    for (uint32_t i = 0; i < inner.children.count; ++i) {
        nodeAt(dock, inner.children.data[i]).parent = parentId;
        parent.children.insert(at + i, inner.children.data[i]);
        parent.weights.insert(at + i, slot * inner.weights.data[i]);
    }
    freeNode(dock, split);
}

// Removes an emptied leaf and gives its share to the sibling before it. If this leaves a split with one child, the
// split is replaced by that child.
void collapseLeaf(Dock& dock, PrKitDockNode leaf) {
    PrKitDockNode parentId = nodeAt(dock, leaf).parent;
    if (parentId == kPrKitDockNodeNone)
        return;
    uint32_t at = indexInParent(dock, leaf);
    DockNode& parent = nodeAt(dock, parentId);
    float share = parent.weights.data[at];
    parent.children.erase(at);
    parent.weights.erase(at);
    parent.weights.data[at > 0 ? at - 1 : 0] += share;
    normalizeWeights(parent);
    freeNode(dock, leaf);
    if (parent.children.count != 1)
        return;
    PrKitDockNode survivor = parent.children.data[0];
    replaceChild(dock, parentId, survivor);
    freeNode(dock, parentId);
    if (nodeAt(dock, survivor).kind == PrKitDockKind_Split)
        flattenSplit(dock, survivor);
}

PrKitDockNode findPanel(const Dock& dock, PrKitDockPanelId panel) {
    if (panel == 0)
        return kPrKitDockNodeNone;
    for (uint32_t i = 1; i < dock.nodes.count; ++i) {
        if (isLeaf(dock, i) && dock.nodes.data[i].tabs.find(panel) != kArrayMissing)
            return i;
    }
    return kPrKitDockNodeNone;
}

void insertTab(Dock& dock, PrKitDockNode leaf, uint32_t at, PrKitDockPanelId panel) {
    DockNode& node = nodeAt(dock, leaf);
    at = at > node.tabs.count ? node.tabs.count : at;
    node.tabs.insert(at, panel);
    node.active = at;
}

// Removes tab `index` of `leaf`. The open panel stays open. If the open tab is the one removed, the tab before it
// opens.
void removeTab(Dock& dock, PrKitDockNode leaf, uint32_t index) {
    DockNode& node = nodeAt(dock, leaf);
    node.tabs.erase(index);
    if (index < node.active)
        --node.active;
    if (node.active >= node.tabs.count && node.active > 0)
        node.active = node.tabs.count - 1;
}

// Removes `panel` from its leaf. If that empties the leaf, also removes the leaf, unless it is the primary leaf or
// `keep`.
void detachPanel(Dock& dock, PrKitDockPanelId panel, PrKitDockNode keep) {
    PrKitDockNode source = findPanel(dock, panel);
    if (source == kPrKitDockNodeNone)
        return;
    removeTab(dock, source, nodeAt(dock, source).tabs.find(panel));
    if (source != keep && source != dock.primary && nodeAt(dock, source).tabs.count == 0)
        collapseLeaf(dock, source);
}

PrKitDirection directionOf(PrKitDockEdge edge) {
    return edge == PrKitDockEdge_Left || edge == PrKitDockEdge_Right ? PrKitDirection_Row : PrKitDirection_Column;
}

bool isAfter(PrKitDockEdge edge) {
    return edge == PrKitDockEdge_Right || edge == PrKitDockEdge_Bottom;
}

void addPanel(Dock& dock, PrKitDockNode leaf, PrKitDockPanelId panel) {
    PR_ASSERT(panel != 0 && findPanel(dock, panel) == kPrKitDockNodeNone);
    if (!isLeaf(dock, leaf))
        return;
    insertTab(dock, leaf, kArrayMissing, panel);
}

PrKitDockNode splitLeaf(Dock& dock, PrKitDockNode leaf, PrKitDockEdge edge, PrKitDockPanelId panel, float share) {
    if (!isLeaf(dock, leaf))
        return kPrKitDockNodeNone;
    float part = share > 0 && share < 1 ? share : 0.5f;
    PrKitDirection direction = directionOf(edge);
    bool after = isAfter(edge);
    PrKitDockNode made = makeNode(dock, PrKitDockKind_Leaf);
    addPanel(dock, made, panel);
    PrKitDockNode parentId = nodeAt(dock, leaf).parent;
    if (parentId != kPrKitDockNodeNone && nodeAt(dock, parentId).direction == direction) {
        uint32_t at = indexInParent(dock, leaf);
        DockNode& parent = nodeAt(dock, parentId);
        float weight = parent.weights.data[at];
        parent.weights.data[at] = weight * (1 - part);
        uint32_t to = after ? at + 1 : at;
        parent.children.insert(to, made);
        parent.weights.insert(to, weight * part);
        nodeAt(dock, made).parent = parentId;
        return made;
    }
    PrKitDockNode splitId = makeNode(dock, PrKitDockKind_Split);
    replaceChild(dock, leaf, splitId);
    DockNode& split = nodeAt(dock, splitId);
    split.direction = direction;
    split.children.push(after ? leaf : made);
    split.children.push(after ? made : leaf);
    split.weights.push(after ? 1 - part : part);
    split.weights.push(after ? part : 1 - part);
    nodeAt(dock, leaf).parent = splitId;
    nodeAt(dock, made).parent = splitId;
    return made;
}

bool removePanel(Dock& dock, PrKitDockPanelId panel) {
    PrKitDockNode leaf = findPanel(dock, panel);
    if (leaf == kPrKitDockNodeNone)
        return false;
    detachPanel(dock, panel, kPrKitDockNodeNone);
    return true;
}

void moveToTab(Dock& dock, PrKitDockPanelId panel, PrKitDockNode leaf, uint32_t at) {
    PrKitDockNode source = findPanel(dock, panel);
    if (!isLeaf(dock, leaf) || source == kPrKitDockNodeNone)
        return;
    uint32_t count = nodeAt(dock, leaf).tabs.count;
    at = at > count ? count : at;
    if (source == leaf) {
        // `at` indexes the strip as it was before the tab left it.
        uint32_t from = nodeAt(dock, leaf).tabs.find(panel);
        uint32_t to = at > from ? at - 1 : at;
        removeTab(dock, leaf, from);
        insertTab(dock, leaf, to, panel);
        return;
    }
    detachPanel(dock, panel, leaf);
    insertTab(dock, leaf, at, panel);
}

void moveToSplit(Dock& dock, PrKitDockPanelId panel, PrKitDockNode leaf, PrKitDockEdge edge) {
    PrKitDockNode source = findPanel(dock, panel);
    if (!isLeaf(dock, leaf) || source == kPrKitDockNodeNone
        || (source == leaf && nodeAt(dock, leaf).tabs.count <= 1))
        return;
    detachPanel(dock, panel, leaf);
    splitLeaf(dock, leaf, edge, panel, 0.5f);
}

void moveToEdge(Dock& dock, PrKitDockPanelId panel, PrKitDockEdge edge) {
    PrKitDockNode source = findPanel(dock, panel);
    if (source == kPrKitDockNodeNone || (source == dock.root && nodeAt(dock, source).tabs.count <= 1))
        return;
    detachPanel(dock, panel, kPrKitDockNodeNone);
    PrKitDirection direction = directionOf(edge);
    bool after = isAfter(edge);
    PrKitDockNode made = makeNode(dock, PrKitDockKind_Leaf);
    addPanel(dock, made, panel);
    PrKitDockNode rootId = dock.root;
    if (nodeAt(dock, rootId).kind == PrKitDockKind_Split && nodeAt(dock, rootId).direction == direction) {
        // A leaf along the root's own axis takes the panes' average share.
        DockNode& root = nodeAt(dock, rootId);
        uint32_t count = root.children.count;
        uint32_t at = after ? count : 0;
        root.children.insert(at, made);
        root.weights.insert(at, 1.0f / float(count));
        nodeAt(dock, made).parent = rootId;
        normalizeWeights(root);
        return;
    }
    PrKitDockNode splitId = makeNode(dock, PrKitDockKind_Split);
    DockNode& split = nodeAt(dock, splitId);
    split.direction = direction;
    split.children.push(after ? rootId : made);
    split.children.push(after ? made : rootId);
    split.weights.push(0.5f);
    split.weights.push(0.5f);
    nodeAt(dock, rootId).parent = splitId;
    nodeAt(dock, made).parent = splitId;
    dock.root = splitId;
}

void applyOp(Dock& dock, const DockOp& op) {
    switch (op.kind) {
        case DockOpKind_Remove:
            removePanel(dock, op.panel);
            break;
        case DockOpKind_ToTab:
            moveToTab(dock, op.panel, op.leaf, op.index);
            break;
        case DockOpKind_ToSplit:
            moveToSplit(dock, op.panel, op.leaf, op.edge);
            break;
        case DockOpKind_ToEdge:
            moveToEdge(dock, op.panel, op.edge);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Views

// The fraction of a leaf's box, from each side toward its middle, in which a drop splits the leaf rather than joining
// it.
constexpr float kEdgeFraction = 0.30f;

// The distance in px from the dock's own side within which a drop takes a whole side of the dock.
constexpr float kDockMargin = 36.0f;

// The distance in px that a press on a tab moves before it carries the panel.
constexpr float kDragThreshold = 4.0f;

// The width in px of the mark that shows where a drop on a strip inserts.
constexpr float kMarkWidth = 2.0f;

// The offset in px of a dragged tab's chip from the pointer.
constexpr float kGhostOffset = 12.0f;

// The salts of the keys of a dock's nodes, combined with the dock's own key.
enum : uint64_t {
    DockKey_Tab = 1,
    DockKey_Content,
    DockKey_Preview,
    DockKey_Mark,
    DockKey_Ghost,
};
typedef uint64_t DockKey;

// A dock's tab: a row that clips its title, with a rule at its end.
struct DockTabBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.minWidth = prLayoutLengthPx(0);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        kitSetPadding(box, PrKitEdges{0, kPrKitSpace2, 0, kPrKitSpace4});
        box.border[PrRectSide_Right] = prLayoutUnitFromPx(1);
    }
};
struct DockTabFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace2);
    }
};

// A box in the overlay at `left` and `top`, which follows the pointer: `width` by `height` px if `sized`, bordered if
// `bordered`, and a padded row if `padded`.
PR_NO_PADDING_BEGIN
struct DockFloatBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float left;
    float top;
    float width;
    float height;
    bool sized;
    bool bordered;
    bool padded;
    uint8_t _pad;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = padded ? PrLayoutDisplay_Flex : PrLayoutDisplay_Block;
        if (padded)
            kitSetPadding(box, prKitEdgesXY(kPrKitSpace1, kPrKitSpace3));
        if (bordered)
            kitSetBorderWidth(box, 1);
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
        if (sized) {
            box.boxSizing = PrLayoutBoxSizing_BorderBox;
            box.width = prLayoutLengthPx(width);
            box.height = prLayoutLengthPx(height);
        }
    }
};
PR_NO_PADDING_END

// What a drop over a leaf does.
enum : uint8_t {
    DropKind_Tab,                   // joins the leaf's tabs
    DropKind_Split,                 // splits the leaf at an edge
    DropKind_DockEdge,              // takes a side of the dock
};
typedef uint8_t DropKind;

struct Drop {
    DropKind kind;
    PrKitDockEdge edge;
};

// The inputs of one build, and what its leaves found to preview.
struct DockBuild {
    PrKit* kit;
    Dock* dock;
    PrKitDockDescribeFn describe;
    void* PR_NULLABLE context;
    uint64_t key;
    PrRect area;                    // the dock's box at the last layout
    PrVec2 pointer;
    bool dragging;                  // true if a tab is in flight
    bool preview;
    PrRect previewRect;
    bool mark;
    PrRect markRect;
};

PrRect boundsOf(Kit& kit, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(kit.tree, node, PrLayoutCoordinates_Viewport));
}

uint64_t keyOf(const DockBuild& b, DockKey salt, uint64_t item) {
    return hashMixKeys(hashMixKeys(b.key, salt), item);
}

// Resolves a drop at `pointer` over a leaf's `content`, in the dock's `area`. Near the dock's border, the drop takes a
// side of the dock. In the leaf's outer band, it takes an edge of the leaf. In the leaf's middle, it joins the tabs.
Drop resolveDrop(PrRect content, PrRect area, PrVec2 pointer) {
    float distances[] = {pointer.x - area.left, area.right - pointer.x, pointer.y - area.top,
                         area.bottom - pointer.y};
    static const PrKitDockEdge kEdges[] = {PrKitDockEdge_Left, PrKitDockEdge_Right, PrKitDockEdge_Top,
                                           PrKitDockEdge_Bottom};
    float nearest = kDockMargin;
    Drop drop{.kind = DropKind_Tab, .edge = PrKitDockEdge_Left};
    for (uint32_t i = 0; i < 4; ++i) {
        if (distances[i] >= 0 && distances[i] < nearest) {
            nearest = distances[i];
            drop = Drop{.kind = DropKind_DockEdge, .edge = kEdges[i]};
        }
    }
    if (drop.kind == DropKind_DockEdge)
        return drop;
    float width = content.right - content.left;
    float height = content.bottom - content.top;
    if (width <= 0 || height <= 0)
        return drop;
    float fractions[] = {(pointer.x - content.left) / width, (content.right - pointer.x) / width,
                         (pointer.y - content.top) / height, (content.bottom - pointer.y) / height};
    nearest = kEdgeFraction;
    for (uint32_t i = 0; i < 4; ++i) {
        if (fractions[i] < nearest) {
            nearest = fractions[i];
            drop = Drop{.kind = DropKind_Split, .edge = kEdges[i]};
        }
    }
    return drop;
}

// Returns the half of `rect` at `edge`.
PrRect halfAt(PrRect rect, PrKitDockEdge edge) {
    float midX = (rect.left + rect.right) / 2;
    float midY = (rect.top + rect.bottom) / 2;
    switch (edge) {
        case PrKitDockEdge_Left:
            rect.right = midX;
            break;
        case PrKitDockEdge_Right:
            rect.left = midX;
            break;
        case PrKitDockEdge_Top:
            rect.bottom = midY;
            break;
        default:
            rect.top = midY;
            break;
    }
    return rect;
}

void stage(DockBuild& b, const DockOp& op) {
    b.dock->staged.push(op);
    prUiRequestFrame(b.kit->ui);
}

// Returns the panel that a payload carries, or zero for a payload that is not a tab.
PrKitDockPanelId panelOf(const PrUiPayload* PR_NULLABLE payload) {
    return payload != nullptr && payload->kind == kPrKitDockDragKind ? payload->value : 0;
}

void buildStrip(DockBuild& b, PrLayoutNode column, PrKitDockNode leaf) {
    PrKit* kit = b.kit;
    PrUi* ui = kit->ui;
    PrKitSurfaceStyle surface{};
    surface.ground = PrKitGround_Header;
    surface.rules = PrKitRuleFlag_Bottom;
    surface.box.direction = PrKitDirection_Row;
    surface.box.overflow = PrKitOverflow_Clip;
    surface.box.place.height = prKitPx(kPrKitHeightSm);
    surface.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode strip = prKitSurface(kit, column, {}, surface);

    // The tabs are created before any of them is styled, so that a click opens its tab before the strip shows which tab
    // is open.
    DockNode& node = b.dock->nodes.data[leaf];
    uint32_t count = node.tabs.count;
    uint32_t base = b.dock->scratch.count;
    for (uint32_t i = 0; i < count; ++i) {
        PrKitDockPanelId panel = node.tabs.data[i];
        PrLayoutNode tab = prUiElement(ui, strip, keyOf(b, DockKey_Tab, panel), 0, PrUiFlag_Pressable);
        b.dock->scratch.push(tab);
        if (prUiClicked(ui, tab, 0))
            node.active = i;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PrKitDockPanelId panel = node.tabs.data[i];
        PrLayoutNode tab = b.dock->scratch.data[base + i];
        bool enabled = (prUiFlags(ui, tab) & PrUiFlag_Disabled) == 0;
        bool active = i == node.active;
        bool hover = enabled && prUiIsHovered(ui, tab);
        const PrKitPalette& p = kit->theme.palette;
        // The title is the tab's own text, set in its record's font and color.
        KitColors fill{.background = active ? p.bg1 : hover ? p.bg3 : PrColor{0, 0, 0, 0}, .border = p.line1};
        KitInk ink{.color = active ? p.text1 : hover ? p.text2 : p.text3};
        prLayoutTreeFrameSetStyle(kit->tree, tab,
                                  kitStyle(*kit, DockTabBox{}, DockTabFlex{}, fill,
                                           KitFont{.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Nowrap}, ink));
        if (enabled && prUiIsPressed(ui, tab, 0))
            prUiDragStart(ui, tab, kDragThreshold, kPrKitDockDragKind, panel);

        PrKitDockTab info = b.describe(b.context, panel);
        prLayoutTreeFrameText(kit->tree, tab, 0, info.title);
        if (info.dirty) {
            PrKitTextStyle dirty{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Warning, .wrap = PrKitWrap_Nowrap};
            prKitLabel(kit, tab, {.text = "*"}, dirty);
        }
        PrKitButton close{.icon = PrKitIconKind_Close};
        PrKitButtonStyle closeStyle{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Xs};
        PrLayoutNode closer = prKitButton(kit, tab, close, closeStyle);
        if (prKitActivated(kit, closer))
            stage(b, DockOp{.panel = panel, .kind = DockOpKind_Remove});

        // A drop on a tab inserts on the side of the tab's middle that the pointer is on.
        bool over = prUiIsDropTarget(ui, tab);
        PrRect rect = boundsOf(*kit, tab);
        bool after = b.pointer.x > (rect.left + rect.right) / 2;
        PrKitDockPanelId dropped = panelOf(prUiDropped(ui, tab));
        if (dropped != 0)
            stage(b, DockOp{.panel = dropped, .leaf = leaf, .index = after ? i + 1 : i, .kind = DockOpKind_ToTab});
        if (over && b.dragging) {
            float x = after ? rect.right - kMarkWidth : rect.left;
            b.mark = true;
            b.markRect = PrRect{x, rect.top, x + kMarkWidth, rect.bottom};
        }
    }
    b.dock->scratch.resize(base);

    // A drop on the strip past its tabs appends.
    prUiIsDropTarget(ui, strip);
    PrKitDockPanelId appended = panelOf(prUiDropped(ui, strip));
    if (appended != 0)
        stage(b, DockOp{.panel = appended, .leaf = leaf, .index = kArrayMissing, .kind = DockOpKind_ToTab});
}

void buildLeaf(DockBuild& b, PrLayoutNode parent, PrKitDockNode leaf) {
    PrKit* kit = b.kit;
    PrKitSurfaceStyle surface{};
    surface.ground = PrKitGround_Panel;
    surface.box.overflow = PrKitOverflow_Clip;
    surface.box.place.flex = PrKitFlex_Fill;
    PrLayoutNode column = prKitSurface(kit, parent, {}, surface);
    if (b.dock->nodes.data[leaf].tabs.count != 0)
        buildStrip(b, column, leaf);

    const DockNode& node = b.dock->nodes.data[leaf];
    PrKitDockPanelId panel = node.tabs.count != 0 ? node.tabs.data[node.active] : 0;
    PrKitBoxStyle box{};
    box.overflow = PrKitOverflow_Clip;
    box.place.flex = PrKitFlex_Fill;
    if (panel == 0) {
        // A leaf left with no panel holds its room and says so.
        box.justify = PrKitJustify_Center;
        box.align = PrKitAlign_Center;
    }
    PrLayoutNode content = prKitBox(kit, column, {.key = panel != 0 ? keyOf(b, DockKey_Content, panel) : 0}, box);
    if (panel != 0) {
        b.dock->panes.push(PrKitDockPane{.panel = panel, .leaf = leaf, .content = content});
    } else {
        PrKitTextStyle faint{.tone = PrKitTone_Faint, .wrap = PrKitWrap_Nowrap};
        prKitLabel(kit, content, {.text = "No panel"}, faint);
    }

    bool over = prUiIsDropTarget(kit->ui, content);
    PrKitDockPanelId dropped = panelOf(prUiDropped(kit->ui, content));
    if (dropped == 0 && !(over && b.dragging))
        return;
    PrRect rect = boundsOf(*kit, content);
    Drop drop = resolveDrop(rect, b.area, b.pointer);
    if (dropped != 0) {
        DockOp op{.panel = dropped, .leaf = leaf, .index = kArrayMissing, .edge = drop.edge};
        op.kind = drop.kind == DropKind_DockEdge ? DockOpKind_ToEdge
                  : drop.kind == DropKind_Split  ? DockOpKind_ToSplit
                                                 : DockOpKind_ToTab;
        stage(b, op);
    }
    if (over && b.dragging) {
        b.preview = true;
        b.previewRect = drop.kind == DropKind_DockEdge ? halfAt(b.area, drop.edge)
                        : drop.kind == DropKind_Split  ? halfAt(rect, drop.edge)
                                                       : rect;
    }
}

void buildNode(DockBuild& b, PrLayoutNode parent, PrKitDockNode id) {
    DockNode& node = b.dock->nodes.data[id];
    if (node.kind == PrKitDockKind_Leaf) {
        buildLeaf(b, parent, id);
        return;
    }
    PrKitBoxStyle box{};
    box.direction = node.direction;
    box.overflow = PrKitOverflow_Clip;
    box.place.flex = PrKitFlex_Fill;
    PrLayoutNode split = prKitBox(b.kit, parent, {}, box);
    uint32_t count = node.children.count;
    Array<PrLayoutNode>& scratch = b.dock->scratch;
    uint32_t base = scratch.count;
    scratch.resize(base + count);
    splitPanes(*b.kit, split, node.direction, node.weights.data, count, kDockMinShare, scratch.data + base);
    for (uint32_t i = 0; i < count; ++i)
        buildNode(b, scratch.data[base + i], node.children.data[i]);
    scratch.resize(base);
}

// Creates the overlay's boxes for a tab in flight: the area that its drop would fill, where it would insert, and its
// title.
void buildFloating(DockBuild& b, PrKitDockPanelId dragged) {
    Kit& kit = *b.kit;
    PrLayoutNode overlay = prUiOverlay(kit.ui);
    const PrKitPalette& p = kit.theme.palette;
    auto box = [](PrRect rect, bool sized) {
        return DockFloatBox{.left = rect.left,
                            .top = rect.top,
                            .width = rect.right - rect.left,
                            .height = rect.bottom - rect.top,
                            .sized = sized};
    };
    if (b.preview) {
        PrColor wash = p.accent;
        wash.a = 0.25f;
        DockFloatBox area = box(b.previewRect, true);
        area.bordered = true;
        prUiElement(kit.ui, overlay, keyOf(b, DockKey_Preview, 0),
                    kitStyle(kit, area, KitColors{.background = wash, .border = p.accent}),
                    PrUiFlag_SubtreeTransparent);
    }
    if (b.mark) {
        prUiElement(kit.ui, overlay, keyOf(b, DockKey_Mark, 0),
                    kitStyle(kit, box(b.markRect, true), KitColors{.background = p.accent}),
                    PrUiFlag_SubtreeTransparent);
    }
    DockFloatBox title = box(PrRect{b.pointer.x + kGhostOffset, b.pointer.y + kGhostOffset, 0, 0}, false);
    title.bordered = true;
    title.padded = true;
    PrLayoutStyleIndex record = kitStyle(kit, title, KitColors{.background = p.bg3, .border = p.line2},
                                         KitFont{.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Nowrap},
                                         KitInk{.color = p.text1});
    PrLayoutNode ghost =
        prUiElement(kit.ui, overlay, keyOf(b, DockKey_Ghost, 0), record, PrUiFlag_SubtreeTransparent);
    prLayoutTreeFrameText(kit.tree, ghost, 0, b.describe(b.context, dragged).title);
}

// The context that a split provides for the current round: its panes, which follow the header in the context's storage.
struct SplitRound {
    uint32_t count;
    uint32_t _pad;

    PrLayoutNode* panes() {
        return reinterpret_cast<PrLayoutNode*>(this + 1);
    }
    const PrLayoutNode* panes() const {
        return reinterpret_cast<const PrLayoutNode*>(this + 1);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrKitDock : Dock {};

PrLayoutNode prKitSplit(PrKit* kit, PrLayoutNode parent, PrKitSplit split, PrKitSplitStyle style) {
    PR_ASSERT(split.count != 0);
    PrKitBoxStyle box{.direction = style.direction, .overflow = PrKitOverflow_Clip, .place = style.place};
    PrLayoutNode node = prKitBox(kit, parent, {.key = split.key}, box);
    // The panes, which the split provides to the queries after it.
    uint32_t size = uint32_t(sizeof(SplitRound) + sizeof(PrLayoutNode) * split.count);
    SplitRound& round = *static_cast<SplitRound*>(prUiProvide(kit->ui, node, &kKitOwner, KitContext_Split, size));
    round.count = split.count;
    splitPanes(*kit, node, style.direction, split.weights, split.count, style.minShare, round.panes());
    return node;
}

PrLayoutNode prKitSplitPane(const PrKit* kit, PrLayoutNode split, uint32_t index) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const void* found = prUiContext(kit->ui, split, &kKitOwner, KitContext_Split, &provider);
    const SplitRound* round = provider == split ? static_cast<const SplitRound*>(found) : nullptr;
    PR_ASSERT(round != nullptr && index < round->count);
    return round->panes()[index];
}

PrKitDock* prKitDockCreate(void) {
    PrKitDock* dock = allocZeroed<PrKitDock>();
    dock->root = makeNode(*dock, PrKitDockKind_Leaf);
    dock->primary = dock->root;
    return dock;
}

void prKitDockDestroy(PrKitDock* PR_NULLABLE dock) {
    if (dock == nullptr)
        return;
    for (uint32_t i = 0; i < dock->nodes.count; ++i) {
        DockNode& node = dock->nodes.data[i];
        node.children.reset();
        node.weights.reset();
        node.tabs.reset();
    }
    dock->nodes.reset();
    dock->spare.reset();
    dock->staged.reset();
    dock->panes.reset();
    dock->scratch.reset();
    prMemoryFree(dock);
}

bool prKitDockNodeIsValid(const PrKitDock* dock, PrKitDockNode node) {
    return isAlive(*dock, node);
}

PrKitDockNode prKitDockRoot(const PrKitDock* dock) {
    return dock->root;
}

PrKitDockNode prKitDockPrimary(const PrKitDock* dock) {
    return dock->primary;
}

PrKitDockKind prKitDockNodeKind(const PrKitDock* dock, PrKitDockNode node) {
    return isAlive(*dock, node) ? dock->nodes.data[node].kind : PrKitDockKind_Leaf;
}

PrKitDockNode prKitDockNodeParent(const PrKitDock* dock, PrKitDockNode node) {
    return isAlive(*dock, node) ? dock->nodes.data[node].parent : kPrKitDockNodeNone;
}

PrKitDockNode prKitDockPanelLeaf(const PrKitDock* dock, PrKitDockPanelId panel) {
    return findPanel(*dock, panel);
}

uint32_t prKitDockLeafTabCount(const PrKitDock* dock, PrKitDockNode leaf) {
    return isLeaf(*dock, leaf) ? dock->nodes.data[leaf].tabs.count : 0;
}

PrKitDockPanelId prKitDockLeafTab(const PrKitDock* dock, PrKitDockNode leaf, uint32_t index) {
    if (!isLeaf(*dock, leaf) || index >= dock->nodes.data[leaf].tabs.count)
        return 0;
    return dock->nodes.data[leaf].tabs.data[index];
}

uint32_t prKitDockLeafActiveTab(const PrKitDock* dock, PrKitDockNode leaf) {
    return isLeaf(*dock, leaf) ? dock->nodes.data[leaf].active : 0;
}

uint32_t prKitDockSplitChildCount(const PrKitDock* dock, PrKitDockNode split) {
    return isSplit(*dock, split) ? dock->nodes.data[split].children.count : 0;
}

PrKitDockNode prKitDockSplitChild(const PrKitDock* dock, PrKitDockNode split, uint32_t index) {
    if (!isSplit(*dock, split) || index >= dock->nodes.data[split].children.count)
        return kPrKitDockNodeNone;
    return dock->nodes.data[split].children.data[index];
}

float prKitDockSplitWeight(const PrKitDock* dock, PrKitDockNode split, uint32_t index) {
    if (!isSplit(*dock, split) || index >= dock->nodes.data[split].weights.count)
        return 0;
    return dock->nodes.data[split].weights.data[index];
}

PrKitDirection prKitDockSplitDirection(const PrKitDock* dock, PrKitDockNode split) {
    return isSplit(*dock, split) ? dock->nodes.data[split].direction : PrKitDirection_Row;
}

void prKitDockAddPanel(PrKitDock* dock, PrKitDockNode leaf, PrKitDockPanelId panel) {
    addPanel(*dock, leaf, panel);
}

PrKitDockNode prKitDockSplit(PrKitDock* dock, PrKitDockNode leaf, PrKitDockEdge edge, PrKitDockPanelId panel,
                             float share) {
    return splitLeaf(*dock, leaf, edge, panel, share);
}

bool prKitDockRemovePanel(PrKitDock* dock, PrKitDockPanelId panel) {
    return removePanel(*dock, panel);
}

void prKitDockMoveToTab(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockNode leaf, uint32_t at) {
    moveToTab(*dock, panel, leaf, at);
}

void prKitDockMoveToSplit(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockNode leaf, PrKitDockEdge edge) {
    moveToSplit(*dock, panel, leaf, edge);
}

void prKitDockMoveToEdge(PrKitDock* dock, PrKitDockPanelId panel, PrKitDockEdge edge) {
    moveToEdge(*dock, panel, edge);
}

void prKitDockSplitSetWeights(PrKitDock* dock, PrKitDockNode split, const float* weights, uint32_t count) {
    if (!isSplit(*dock, split))
        return;
    DockNode& node = nodeAt(*dock, split);
    PR_ASSERT(count == node.children.count);
    for (uint32_t i = 0; i < count; ++i)
        node.weights.data[i] = weights[i] > 0 ? weights[i] : 0;
    normalizeWeights(node);
}

void prKitDockLeafSetActiveTab(PrKitDock* dock, PrKitDockNode leaf, uint32_t index) {
    if (isLeaf(*dock, leaf) && index < nodeAt(*dock, leaf).tabs.count)
        nodeAt(*dock, leaf).active = index;
}

PrLayoutNode prKitDockBuild(PrKit* kit, PrLayoutNode parent, PrKitDockView view) {
    PrKitDock* dock = view.dock;
    PrKitDockDescribeFn describe = view.describe;
    void* context = view.context;
    uint64_t key = view.key;
    // The staged rearrangements are applied in order. One whose panel or leaf an earlier one removed does nothing.
    for (uint32_t i = 0; i < dock->staged.count; ++i)
        applyOp(*dock, dock->staged.data[i]);
    dock->staged.clear();
    dock->panes.clear();

    PrKitBoxStyle box{};
    box.overflow = PrKitOverflow_Clip;
    box.place.flex = PrKitFlex_Fill;
    PrLayoutNode area = prKitBox(kit, parent, {.key = key}, box);

    PrKitDockPanelId dragged = panelOf(prUiDragging(kit->ui));
    // The dock's nodes are keyed from its area's node, which is unique in the tree.
    DockBuild b{.kit = kit, .dock = dock, .describe = describe, .context = context, .key = area};
    b.area = boundsOf(*kit, area);
    b.dragging = dragged != 0;
    prUiPointer(kit->ui, &b.pointer);
    buildNode(b, area, dock->root);
    if (b.dragging)
        buildFloating(b, dragged);
    return area;
}

uint32_t prKitDockPaneCount(const PrKitDock* dock) {
    return dock->panes.count;
}

PrKitDockPane prKitDockPane(const PrKitDock* dock, uint32_t index) {
    PR_ASSERT(index < dock->panes.count);
    return dock->panes.data[index];
}
