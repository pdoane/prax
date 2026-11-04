// The paint walk: a tree's fragments visited in painting order, which recording and hit testing share.
//
// CSS2 - https://drafts.csswg.org/css2/
// css-overflow-3 - https://drafts.csswg.org/css-overflow-3/
// css-position-4 - https://drafts.csswg.org/css-position-4/
// css-transforms-1 - https://drafts.csswg.org/css-transforms-1/
//
// A stacking context paints in phases (CSS2/#elaborate-stacking-contexts, css-position-4/#paint-a-stacking-context):
// its own background and border, then the boxes of its list below level 0, then the backgrounds and borders of the
// block boxes in its flow, then its floats, then their inline content, and then the boxes of its list at level 0 and
// above, each whole (layout_stacking.h). The phases of its content skip every box that paints whole, because a list
// holds those boxes. A box that paints whole and makes no stacking context paints only its own content's phases, and
// the boxes that paint whole in it belong to its context. A float paints as if it made a stacking context, as an
// inline-block does on its line, and so does a flex item, with its container's inline content.
//
// A box of a list paints where the last layout placed it (LayoutPlacement), in its own containers: the boxes that clip
// along its placements (layoutNodeContainer), which the walk enters from the containers it is in. A box out of flow
// thereby escapes the containers between its holder and its placeholder, and a fixed box that the viewport holds
// escapes the viewport's scrolling.
//
// A line's children are in tree order. A float's own fragment is a child of the line's parent, and the floats phase
// visits it.
//
// An inline box that paints whole, because it is positioned or is a stacking context by its opacity, paints from its
// list with its own phases inside it: the blocks in its rows, its floats and their floats, and its parts and content
// line by line (css-position-4/#paint-a-stacking-context). The phases of its container skip its pieces: its parts and
// content on lines, its rows, and its floats.

#pragma once

#include "pr/draw.h"
#include "pr/vec.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_stacking.h"
#include "layout/src/layout_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Phases

// What a phase of a box's painting visits (CSS2/#elaborate-stacking-contexts).
enum {
    LayoutPhase_Blocks,             // the backgrounds and borders of the block boxes in flow
    LayoutPhase_Floats,             // the floats, each as if it made a stacking context
    LayoutPhase_Inlines,            // their inline content
    LayoutPhase_Body,               // a box that paints whole, all of its phases; a cache key, not a pass
    LayoutPhase_Atomic,             // an inline-block in its line's inline content, or a float: its decoration, then
                                    // its block-level content, its floats and its inline content; a cache key, not a
                                    // pass
    LayoutPhase_ListBelow,          // the boxes of a stacking context's list below level 0; a cache key, not a pass
    LayoutPhase_List,               // the boxes of a stacking context's list at 0 and above; a cache key, not a pass
    LayoutPhase_Count,
};
typedef uint8_t LayoutPhase;

// Returns true if `phase` keys the runs of a stacking context's list.
inline bool layoutPhaseIsList(LayoutPhase phase) {
    return phase == LayoutPhase_ListBelow || phase == LayoutPhase_List;
}

// The maximum number of children in a run of a box's children. A box with more children records them in runs of this
// many (LayoutPaintRun). A change re-records the run that holds it and the box's recording, which embeds one recording
// for each run.
constexpr uint32_t kPaintRun = 32;

// ---------------------------------------------------------------------------------------------------------------------
// Geometry

// A box's transform, resolved for its reference box (css-transforms-1/#transform-box), with the perspective of its
// parent (css-transforms-2/#perspective-property).
struct LayoutResolvedTransform {
    PrMatrix4 matrix;               // relative to the origin
    PrLayoutVec origin;             // relative to the top left of the border box
    float originZ;                  // in pixels
    PrVec2 reference;               // the extent of the reference box, in pixels
    float perspective;              // the parent's distance to the eye in pixels, or 0 for none
    bool backfaceHidden;            // the box is hidden while its plane shows its back (prDMatrix4ShowsBack)

    // The parent's perspective origin, relative to the top left of the box's border box with the parent unscrolled, and
    // the parent if it is a scroll container whose offset moves the box against that origin, or kPrLayoutNodeNone.
    PrLayoutVec perspectiveOrigin;
    LayoutNodeIndex scroller;
};

// Returns the transform of the box `node`, whose fragment is `fragment`.
LayoutResolvedTransform layoutNodeTransform(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment);

// Returns the map from a box's plane into the plane that it paints on, its parent's, both with the box's border box's
// top left at the origin: T(p) * perspective(d) * T(-p) * T(o) * matrix * T(-o), where `matrix` stands for the resolved
// transform's own (a compositor may give another) and p is the perspective origin moved by `scroll`, the parent's
// scroll offset.
PrDMatrix4 layoutPlaneMap(const LayoutResolvedTransform& transform, const PrMatrix4& matrix, PrLayoutVec scroll);

// Returns the homography that `map` applies to the plane z = 0, in coordinates in which the box's top left is at
// `offset`.
PrDMatrix3 layoutPlaneHomography(const PrDMatrix4& map, PrLayoutVec offset);

// Returns the map that painting applies for the transform of the box `fragment` at `offset`, with its parent's
// perspective and scroll offset as the tree holds it, in coordinates in which the box's top left is at `offset`. Sets
// `*hidden` to true if the box hides its back and shows it.
PrDMatrix3 layoutBoxPlane(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset, bool* PR_NULLABLE hidden);

// Returns true if the box `fragment` paints whole, from its stacking context's list (layoutPaintsWholeAs).
bool layoutFragmentPaintsWhole(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns the box style of the element that `fragment` is a box fragment of, or null. The root's fragment returns null,
// because the root's overflow is the viewport's.
const PrLayoutBoxStyle* PR_NULLABLE layoutOverflowBox(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns the axes in which the box `fragment` clips its content (layoutClips).
LayoutClips layoutFragmentClips(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns the padding box of a box fragment at `offset`.
PrLayoutRect layoutPaddingBox(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset);

// Returns the viewport's scroll offset, within its range.
PrLayoutVec layoutViewportScroll(const LayoutTree* tree, const LayoutFragment* viewport);

// Returns true if the box `fragment` clips its content to its clip rectangle (PrLayoutBoxStyle::clipped).
bool layoutFragmentClipped(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns true if the box `fragment` is a scroll container (layoutIsScrollContainer), which clips its content in both
// axes and scrolls it.
bool layoutFragmentScrolls(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns the scroll offset of the scroll container `fragment`, within its range (layoutFragmentScrolls).
PrLayoutVec layoutFragmentScroll(const LayoutTree* tree, const LayoutFragment* fragment);

// Returns true if the inline box `node` paints whole (layoutInlinePaintsWhole).
inline bool layoutNodeInlinePaintsWhole(const LayoutTree* tree, LayoutNodeIndex node) {
    return layoutInlinePaintsWhole(tree->styles, layoutTreeNode(tree, node).style);
}

// Returns true if an inline box that paints whole, around `node` and inside its container, holds `node` among its
// pieces.
bool layoutInOwningInline(const LayoutTree* tree, LayoutNodeIndex node);

// Appends to `out` the containers that a node whose container is `container` (layoutNodeContainer) paints in, outermost
// first: the viewport's content, as kLayoutContainerViewport, and the boxes that clip or stick around the node. Appends
// none of them for kLayoutContainerOutside.
void layoutContainerChain(const LayoutTree* tree, LayoutNodeIndex container, RawArray<LayoutNodeIndex>& out);

// ---------------------------------------------------------------------------------------------------------------------
// Visitors

// Where the walk is among a block container's lines and rows: the container, its content's offset, and the line or row
// (LayoutPieceAt).
struct LayoutWalkPlace {
    const LayoutFragment* PR_NULLABLE container;
    PrLayoutVec base;
    LayoutPieceAt at;
};

// What a box that paints whole pushed for its effects.
enum {
    LayoutPushed_Transform = 1 << 0,
    LayoutPushed_Opacity = 1 << 1,
    LayoutPushed_Hidden = 1 << 2,   // nothing: the box shows the back that it hides, and its body is not visited
    LayoutPushed_Clip = 1 << 3,     // its clip path or clip rectangle
};
typedef uint8_t LayoutPushed;

// What the walk tells a visitor and asks of it. Each default does nothing or declines. A visitor derives from this
// struct and declares what it does, and the walk calls the visitor's own member if it has one. A visitor says which
// subtrees it enters, how far the content of each scroll container it enters is moved, which boxes with layers of their
// own it takes separately rather than visiting them, and where it replays its cached recording of a subtree or a run of
// children for a phase.
struct LayoutWalkVisitor {
    // Returns true if the walk enters the subtree of `fragment` at `offset`.
    bool enters(const LayoutFragment*, PrLayoutVec) const { return true; }

    // Called for a box's border box.
    void box(const LayoutFragment*, PrLayoutRect, bool, bool) {}

    // Called for an inline box's row around a block-level box inside it, which paints nothing: in the blocks phase,
    // beneath the block, and again in the inlines phase, among the inline box's parts on lines in content order.
    void around(const LayoutFragment*, PrLayoutRect, bool, LayoutPhase) {}

    // Called for a replaced box's content and for a text run.
    void replaced(const LayoutFragment*, PrLayoutVec, bool) {}
    void text(const LayoutFragment*, PrLayoutVec, bool) {}

    // Called for a table at `at` in the blocks phase (LayoutFragmentFlag_Table). gridBackgrounds is called before the
    // table's parts, for the backgrounds of its tracks and track groups behind its cells. gridBorders is called after
    // them, for the collapsed borders of its grid.
    void gridBackgrounds(const LayoutFragment*, PrLayoutVec) {}
    void gridBorders(const LayoutFragment*, PrLayoutVec) {}

    // Called for a math box at `at` in the inlines phase, after its children (LayoutFragmentFlag_Math), for what it
    // draws beside them (mathml-core/#stacking-contexts).
    void math(const LayoutFragment*, PrLayoutVec) {}

    // Called before the decoration of a table's row group, row or cell at `at` that paints whole, for the backgrounds
    // of the tracks and track groups behind its cells, which its table's gridBackgrounds leaves to it.
    void trackBackgrounds(const LayoutFragment*, PrLayoutVec) {}

    // Called for a scroll container's content, with its padding box and its scroll offset, and for a box that only
    // clips. Returns how far a scroll container's content moves.
    PrLayoutVec pushScroll(const LayoutFragment*, PrLayoutRect, PrLayoutVec) { return kLayoutVecZero; }
    void popScroll() {}
    void pushClip(const LayoutFragment*, PrLayoutRect) {}
    void popClip() {}

    // Called for the container of the sticky box `node`, which holds the box and what paints in it. Returns how far the
    // container moves its content back, as a scroll offset does: its sticky offset, negated (layoutNodeStickyShift).
    PrLayoutVec pushSticky(LayoutNodeIndex) { return kLayoutVecZero; }
    void popSticky() {}

    // Called for the viewport's content, scrolled by `scroll`. Returns how far it moves. The walk leaves it for a fixed
    // box that the viewport holds, and enters it again afterward.
    PrLayoutVec scrollViewport(PrLayoutVec) { return kLayoutVecZero; }
    void leaveViewport() {}
    void enterViewport() {}

    // Called for the list of the stacking context `context` that the walk paints from, as found as `version`, and for
    // where a box of a list paints: its fragment, or the fragment of an inline box's block container, at `at`.
    void listed(LayoutNodeIndex, uint32_t) {}
    void placed(LayoutNodeIndex, const LayoutFragment*, PrLayoutVec) {}

    // Called around a box that paints whole, for its effects. pushClipPath is called inside the box's transform, with
    // the box's fragment at `offset`, for its clip path or its clip rectangle (PrLayoutCompositingStyle).
    void pushTransform(const PrDMatrix3&) {}
    void popTransform() {}
    void pushOpacity(float) {}
    void popOpacity() {}
    void pushClipPath(const LayoutFragment*, PrLayoutVec) {}
    void popClipPath() {}

    // Called around the decoration of a box that its clip rectangle `rect` clips (PrLayoutBoxStyle::clipped). The box's
    // container clips its content.
    void pushBoxClip(PrLayoutRect) {}
    void popBoxClip() {}

    // Returns true if the visitor takes a box with a layer of its own separately, and the walk does not visit it. The
    // box has a fragment of its own, or is an inline box whose opening part is on the line at `place`.
    bool takesLayer(const LayoutFragment*, PrLayoutVec) { return false; }
    bool takesInlineLayer(LayoutNodeIndex, const LayoutWalkPlace&) { return false; }

    // Returns true if the visitor replays its cached recording of a subtree for a phase, and the walk does not visit
    // the subtree. Otherwise the walk enters the subtree, from where the visitor says, and leaves it.
    bool replay(const LayoutFragment*, PrLayoutVec, LayoutPhase) { return false; }
    PrLayoutVec enterSubtree(const LayoutFragment*, PrLayoutVec offset, LayoutPhase) { return offset; }
    void leaveSubtree() {}

    // The runs of a box's children: where a run that starts at a child ends, which the walk ends where the children it
    // visits end, and where the visitor replays what it cached of a run or enters the run as a subtree of its own. A
    // visitor with no runs takes all children as one run.
    uint32_t runEnd(const LayoutFragment* fragment, uint32_t) const { return fragment->childCount; }
    bool replayRun(const LayoutFragment*, uint32_t, uint32_t, PrLayoutVec, LayoutPhase) { return false; }
    bool enterRun(const LayoutFragment*, uint32_t, uint32_t, PrLayoutVec, LayoutPhase) { return false; }
    void leaveRun() {}

    // The runs of a stacking context's list: a run of the boxes of the list of `context` below level 0
    // (LayoutPhase_ListBelow) or at 0 and above (LayoutPhase_List), whose boxes are `boxes`, painted from the context's
    // reference box. That box is at `origin` in the containers the walk is in, and at `reference` in the viewport's
    // content as laid out. The visitor replays what it cached of a run, or enters the run as a subtree of its own. A
    // visitor with no runs takes each box alone.
    bool replayEntries(LayoutNodeIndex, LayoutPhase, const LayoutStackedBox*, uint32_t, PrLayoutVec, PrLayoutVec) {
        return false;
    }
    bool enterEntries(LayoutNodeIndex, LayoutPhase, const LayoutStackedBox*, uint32_t, PrLayoutVec, PrLayoutVec) {
        return false;
    }
    void leaveEntries() {}
};

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// A container the walk is in: a box that clips or scrolls its content, a sticky box, or the viewport's content, and how
// far the visitor said that its content moves.
struct LayoutWalkContainer {
    LayoutNodeIndex node;           // as layoutNodeContainer names it
    PrLayoutVec scroll;
};

// What the walk entered at a box's content: its content's offset, and whether it entered a container.
struct LayoutWalkEntered {
    PrLayoutVec base;
    bool container;
};

// Visits a tree's fragments in painting order, telling `Visitor` about each box, text run, container and effect.
template <typename Visitor>
struct LayoutWalk {
    const LayoutTree* tree;
    const LayoutStyles* styles;
    Visitor& visitor;
    RawArray<LayoutWalkContainer> containers; // entered, outermost first
    RawArray<LayoutNodeIndex> chain;          // scratch: the containers a box of a list paints in
    LayoutWalkPlace place;                    // where the walk is among a container's lines

    // Frees the walk's storage.
    void release() {
        containers.reset();
        chain.reset();
    }

    const LayoutTreeNode& nodeOf(const LayoutFragment* fragment) const { return layoutTreeNode(tree, fragment->node); }

    bool isVisible(LayoutNodeIndex node) const {
        return layoutColorStyle(styles, layoutNodeStyle(tree, node)).visibility == PrLayoutVisibility_Visible;
    }

    // Sets `*out` to the float that the anchor `anchor` on the line at `at` names, at its offset, and returns true.
    // Returns false if it names none.
    bool anchored(const LayoutFragment* anchor, const LayoutWalkPlace& at, LayoutChild* out) const {
        if (anchor->anchor.sibling == kLayoutAnchorNone)
            return false;
        const LayoutFragment* parent = at.container;
        PrLayoutVec base = at.base;
        uint32_t line = at.at.child;
        if (at.at.line != kLayoutPieceNone) {
            const LayoutChild& block = parent->children[at.at.child];
            parent = block.fragment;
            base = base + block.offset;
            line = at.at.line;
        }
        const LayoutChild& floated = parent->children[uint32_t(int32_t(line) + anchor->anchor.sibling)];
        *out = LayoutChild{floated.fragment, base + floated.offset};
        return true;
    }

    // --- Containers ---

    // Enters the container of a sticky box (layoutContainerIsSticky). Returns how far its content moves.
    PrLayoutVec pushSticky(LayoutNodeIndex container) {
        PrLayoutVec scroll = visitor.pushSticky(layoutContainerBox(container));
        containers.push(LayoutWalkContainer{container, scroll});
        return scroll;
    }

    // Enters the container `fragment` at `offset`, a box that clips its content: a scroll container, or a box that only
    // clips, whose clip is its padding box extended without limit in an axis that it does not clip, cut by its clip
    // rectangle if it has one. Returns how far its content moves.
    PrLayoutVec push(const LayoutFragment* fragment, PrLayoutVec offset) {
        PrLayoutVec scroll = kLayoutVecZero;
        PrLayoutRect padding = layoutPaddingBox(tree, fragment, offset);
        if (layoutFragmentScrolls(tree, fragment)) {
            scroll = visitor.pushScroll(fragment, padding, layoutFragmentScroll(tree, fragment));
        } else {
            LayoutClips clips = layoutFragmentClips(tree, fragment);
            if (!clips.x) {
                padding.left = layoutUnit(-kLayoutClipUnbounded.raw);
                padding.right = kLayoutClipUnbounded;
            }
            if (!clips.y) {
                padding.top = layoutUnit(-kLayoutClipUnbounded.raw);
                padding.bottom = kLayoutClipUnbounded;
            }
            if (layoutFragmentClipped(tree, fragment))
                padding = layoutRectIntersect(
                    padding, layoutClipRect(tree, fragment->node, layoutRectAt(offset, fragment->extent)));
            visitor.pushClip(fragment, padding);
        }
        containers.push(LayoutWalkContainer{fragment->node, scroll});
        return scroll;
    }

    // Leaves the innermost container.
    void pop() {
        LayoutWalkContainer last = containers.last();
        containers.pop();
        if (last.node == kLayoutContainerViewport)
            visitor.leaveViewport();
        else if (layoutContainerIsSticky(last.node))
            visitor.popSticky();
        else if (layoutFragmentScrolls(tree, layoutCachePlaced(tree->cache, last.node)))
            visitor.popScroll();
        else
            visitor.popClip();
    }

    // Enters the content of the box `fragment` at `offset`, and its container if it clips its content.
    LayoutWalkEntered enterContent(const LayoutFragment* fragment, PrLayoutVec offset) {
        const PrLayoutBoxStyle* box = layoutOverflowBox(tree, fragment);
        if (box == nullptr)
            return LayoutWalkEntered{offset, false};
        LayoutClips clips = layoutClips(*box);
        if (!clips.x && !clips.y && !box->clipped)
            return LayoutWalkEntered{offset, false};
        return LayoutWalkEntered{offset - push(fragment, offset), true};
    }

    void leaveContent(const LayoutWalkEntered& entered) {
        if (entered.container)
            pop();
    }

    // Moves the walk from the containers it is in to those of `to`, outermost first. Leaves those that are not among
    // them, and enters the rest, each placed from the reference box of a stacking context. That box is at `reference`
    // in the viewport's content as laid out, and at `origin` in the `base` containers, which the context's list started
    // in. Returns how far that moves what the context places, relative to where `origin` puts it, whatever containers
    // an earlier box of the list left the walk in.
    PrLayoutVec enterChain(const RawArray<LayoutNodeIndex>& to, const RawArray<LayoutWalkContainer>& base, PrLayoutVec origin,
            PrLayoutVec reference) {
        uint32_t kept = 0;
        while (kept < containers.count && kept < to.count && containers.data[kept].node == to.data[kept])
            ++kept;
        while (containers.count > kept)
            pop();
        // The content moves by the scroll of each base container that the chain leaves, back, and of each container
        // past the base that it keeps.
        uint32_t shared = 0;
        while (shared < base.count && shared < kept && base.data[shared].node == containers.data[shared].node)
            ++shared;
        PrLayoutVec moved = kLayoutVecZero;
        for (uint32_t i = shared; i < base.count; ++i)
            moved = moved + base.data[i].scroll;
        for (uint32_t i = shared; i < kept; ++i)
            moved = moved - containers.data[i].scroll;
        for (uint32_t i = kept; i < to.count; ++i) {
            LayoutNodeIndex node = to.data[i];
            if (node == kLayoutContainerViewport) {
                visitor.enterViewport();
                PrLayoutVec scroll = visitor.scrollViewport(layoutViewportScroll(tree, tree->fragment));
                containers.push(LayoutWalkContainer{node, scroll});
                moved = moved - scroll;
                continue;
            }
            if (layoutContainerIsSticky(node)) {
                moved = moved - pushSticky(node);
                continue;
            }
            const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
            moved = moved - push(fragment, origin + moved + layoutNodePosition(tree, node) - reference);
        }
        return moved;
    }

    // --- Lines ---

    // Visits a line's inline content from child `first` to `end`, except for the atomic inlines and inline boxes that
    // paint whole, whose own phases visit their parts and content on the line.
    void lineRange(const LayoutFragment* line, PrLayoutVec offset, uint32_t first, uint32_t end) {
        for (uint32_t i = first; i < end; ++i) {
            const LayoutChild& child = line->children[i];
            const LayoutFragment* c = child.fragment;
            PrLayoutVec at = offset + child.offset;
            if (layoutFragmentIsPoint(c))
                continue;
            bool visible = isVisible(c->node);
            if (c->kind == LayoutFragmentKind_Text) {
                visitor.text(c, at, visible);
                continue;
            }
            if (c->kind == LayoutFragmentKind_Inline) {
                if (layoutNodeInlinePaintsWhole(tree, c->node)) {
                    uint32_t parts = 0;
                    i = layoutGroupEnd(line, i, &parts) - 1;
                    continue;
                }
                visitor.box(c, layoutRectAt(at, c->extent), visible, false);
                continue;
            }
            if ((c->kind == LayoutFragmentKind_Replaced || c->kind == LayoutFragmentKind_Box) &&
                    layoutFragmentPaintsWhole(tree, c))
                continue;
            if (c->kind == LayoutFragmentKind_Box) {
                inlineBlock(c, at);
                continue;
            }
            visitor.box(c, layoutRectAt(at, c->extent), visible, false);
            if (c->kind == LayoutFragmentKind_Replaced)
                visitor.replaced(c, at, visible);
        }
    }

    // Visits the floats anchored on a line from child `first` to `end` that do not paint whole, each as if it made a
    // stacking context, with the line at `place`. Skips the floats of the inline boxes that paint whole.
    void lineFloats(const LayoutFragment* line, uint32_t first, uint32_t end) {
        for (uint32_t i = first; i < end; ++i) {
            const LayoutFragment* c = line->children[i].fragment;
            if (c->kind == LayoutFragmentKind_Inline) {
                if (layoutNodeInlinePaintsWhole(tree, c->node)) {
                    uint32_t parts = 0;
                    i = layoutGroupEnd(line, i, &parts) - 1;
                }
                continue;
            }
            LayoutChild floated{};
            if (c->kind != LayoutFragmentKind_FloatAnchor || !anchored(c, place, &floated) ||
                    layoutFragmentPaintsWhole(tree, floated.fragment) ||
                    !visitor.enters(floated.fragment, floated.offset))
                continue;
            atomic(floated.fragment, floated.offset);
        }
    }

    // Visits an inline-block at `at` that does not paint whole, as if it made a stacking context
    // (CSS2/#elaborate-stacking-contexts): its decoration, then its block-level content, then its inline content. A
    // flex item paints in the same way, and an anonymous flex item has no decoration.
    void inlineBlock(const LayoutFragment* fragment, PrLayoutVec at) {
        if (!visitor.enters(fragment, at) || visitor.replay(fragment, at, LayoutPhase_Atomic))
            return;
        PrLayoutVec from = visitor.enterSubtree(fragment, at, LayoutPhase_Atomic);
        if (fragment->node != kPrLayoutNodeNone)
            visitor.box(fragment, layoutRectAt(from, fragment->extent), isVisible(fragment->node), false);
        blocks(fragment, from);
        content(fragment, from, LayoutPhase_Floats);
        content(fragment, from, LayoutPhase_Inlines);
        visitor.leaveSubtree();
    }

    // Visits a float or a flex item at `at` that does not paint whole, as if it made a stacking context, in the same
    // way as inlineBlock.
    void atomic(const LayoutFragment* fragment, PrLayoutVec at) {
        if (fragment->kind == LayoutFragmentKind_Replaced) {
            visitor.box(fragment, layoutRectAt(at, fragment->extent), isVisible(fragment->node), false);
            visitor.replaced(fragment, at, isVisible(fragment->node));
            return;
        }
        inlineBlock(fragment, at);
    }

    // --- Stacking contexts ---

    // Visits the boxes of the list of the stacking context `context`, whose reference box is at `origin` in the
    // containers the walk is in. The reference box is the context's border box, or for an inline box its block
    // container's border box. Visits those below level 0 if `negative` is true, and otherwise those at 0 and above.
    // Each paints whole in its own containers, and the walk then returns to the containers it was in.
    void paintList(LayoutNodeIndex context, PrLayoutVec origin, bool negative) {
        LayoutStackedBoxes list = layoutStackingList(tree, context);
        if (negative)
            visitor.listed(context, list.version);
        uint32_t first = negative ? 0 : list.zero;
        uint32_t end = negative ? list.zero : list.count;
        if (first == end)
            return;
        RawArray<LayoutNodeIndex> around;
        RawArray<LayoutWalkContainer> base;
        for (uint32_t i = 0; i < containers.count; ++i) {
            around.push(containers.data[i].node);
            base.push(containers.data[i]);
        }
        PrLayoutVec reference = layoutNodePosition(tree, context);
        // A list of more than kPaintRun boxes paints in runs. Each run starts in the containers that the list's
        // painting starts in.
        bool runs = list.count > kPaintRun;
        LayoutPhase phase = negative ? LayoutPhase_ListBelow : LayoutPhase_List;
        for (uint32_t start = first; start < end;) {
            uint32_t stop = runs ? runStop(list.boxes, start, end) : end;
            const LayoutStackedBox* boxes = list.boxes + start;
            uint32_t count = stop - start;
            if (runs) {
                enterChain(around, base, origin, reference);
                if (visitor.replayEntries(context, phase, boxes, count, origin, reference)) {
                    start = stop;
                    continue;
                }
            }
            bool run = runs && visitor.enterEntries(context, phase, boxes, count, origin, reference);
            for (uint32_t i = start; i < stop; ++i)
                paintEntry(list.boxes[i].node, base, origin, reference);
            if (run)
                visitor.leaveEntries();
            start = stop;
        }
        enterChain(around, base, origin, reference);
        around.reset();
        base.reset();
    }

    // Returns the end, before `end`, of the run of a list's boxes that starts at `start`. A run ends after at least a
    // quarter of kPaintRun boxes at a box whose node's hash ends a run (one box in kPaintRun), or at twice kPaintRun
    // boxes. Inserting or removing a box therefore changes only the runs around it.
    static uint32_t runStop(const LayoutStackedBox* boxes, uint32_t start, uint32_t end) {
        for (uint32_t i = start; i < end; ++i) {
            uint32_t length = i + 1 - start;
            bool ends = length >= kPaintRun / 4 && (boxes[i].node * 0x9e3779b9u) % kPaintRun == 0;
            if (ends || length == 2 * kPaintRun)
                return i + 1;
        }
        return end;
    }

    // Visits the box `node` of a stacking context's list, whose reference box is at `origin` in the containers the list
    // is painted in and at `reference` in the viewport's content as laid out, whole in its own containers.
    void paintEntry(LayoutNodeIndex node, const RawArray<LayoutWalkContainer>& base, PrLayoutVec origin,
            PrLayoutVec reference) {
        bool inlineBox = layoutIsInlineBox(tree, node);
        // An inline box that starts in a piece of its container after the first, as a fragmentation break makes, has no
        // placement to paint it from.
        LayoutPlacement placement{};
        if (inlineBox) {
            placement = layoutCachePlacement(tree->cache, node);
            if (placement.parent == kPrLayoutNodeNone)
                return;
        }
        LayoutPositioned positioned = inlineBox ? layoutNodePositioned(tree, node) : layoutBoxPositioned(tree, node);
        chain.clear();
        layoutContainerChain(tree, positioned.container, chain);
        PrLayoutVec moved = enterChain(chain, base, origin, reference);
        PrLayoutVec at = origin + moved + positioned.position - reference;
        if (inlineBox) {
            const LayoutFragment* container = layoutCachePlaced(tree->cache, placement.parent);
            if (container == nullptr)
                return;
            visitor.placed(node, container, at);
            inlineWhole(node, LayoutWalkPlace{container, at, placement.at});
            return;
        }
        const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
        if (fragment == nullptr)
            return;
        visitor.placed(node, fragment, at);
        // A stacking context's area holds its own content, not the boxes of its list, which may escape its clips.
        bool holdsList = layoutNodeLeadsWhole(tree, node) && layoutIsStackingContext(tree, node);
        if (holdsList || visitor.enters(fragment, at))
            paintWhole(fragment, at);
        // Only a tree with roots of fragmentation contexts has other pieces.
        if (tree->stacking.fragmentationRoots.count != 0)
            paintPieces(node, at - positioned.position, holdsList);
    }

    // Paints the pieces of the box `node` that paints whole other than its first, after it and in the same containers.
    // `shift` moves a position in the viewport's content into the coordinates of the containers.
    PR_NOINLINE void paintPieces(LayoutNodeIndex node, PrLayoutVec shift, bool holdsList) {
        LayoutWholePieces pieces = layoutWholePiecesOf(tree, node);
        for (uint32_t i = 0; i < pieces.count; ++i) {
            const LayoutWholePiece& piece = pieces.pieces[i];
            PrLayoutVec pieceAt = shift + piece.position;
            if (holdsList || visitor.enters(piece.fragment, pieceAt))
                paintWhole(piece.fragment, pieceAt);
        }
    }

    // --- Inline boxes that paint whole ---

    // Visits the inline box `node` whole, with its opening part on the line at `at`: as a separate layer if the visitor
    // takes it, and otherwise with its opacity around its own phases.
    void inlineWhole(LayoutNodeIndex node, LayoutWalkPlace at) {
        if (visitor.takesInlineLayer(node, at))
            return;
        float opacity = layoutEffectStyle(styles, layoutTreeNode(tree, node).style).opacity;
        if (opacity < 1)
            visitor.pushOpacity(opacity);
        inlineBody(node, at);
        if (opacity < 1)
            visitor.popOpacity();
    }

    // Visits what the inline box `node` paints itself, with its opening part on the line at `at`
    // (css-position-4/#paint-a-stacking-context): the boxes of its list below 0 if it is a stacking context, the blocks
    // in its rows, its floats and their floats, its parts and content line by line, and the boxes of its list at 0 and
    // above.
    void inlineBody(LayoutNodeIndex node, LayoutWalkPlace at) {
        bool context = layoutIsStackingContext(tree, node);
        LayoutWalkPlace saved = place;
        bool visible = isVisible(node);
        if (context)
            paintList(node, at.base, true);
        const LayoutPhase phases[] = {LayoutPhase_Blocks, LayoutPhase_Floats, LayoutPhase_Inlines};
        for (LayoutPhase phase : phases) {
            LayoutPieces walked = layoutPiecesFrom(at.container, at.base, node, at.at);
            LayoutPiece piece{};
            while (layoutPiecesNext(walked, &piece)) {
                if (piece.kind == LayoutPieceKind_Row) {
                    around(piece.fragment, piece.offset, phase);
                    continue;
                }
                place = LayoutWalkPlace{at.container, at.base, piece.at};
                const LayoutFragment* line = piece.fragment;
                if (phase == LayoutPhase_Floats) {
                    lineFloats(line, piece.parts, piece.end);
                } else if (phase == LayoutPhase_Inlines) {
                    for (uint32_t k = piece.first; k < piece.parts; ++k) {
                        const LayoutChild& part = line->children[k];
                        visitor.box(part.fragment, layoutRectAt(piece.offset + part.offset, part.fragment->extent),
                            visible, false);
                    }
                    lineRange(line, piece.offset, piece.parts, piece.end);
                }
            }
        }
        place = saved;
        if (context)
            paintList(node, at.base, false);
    }

    // --- Boxes ---

    // Visits a box that paints whole: as a separate layer if the visitor takes it, otherwise from the visitor's cached
    // recording of it, and otherwise whole.
    void paintWhole(const LayoutFragment* fragment, PrLayoutVec at) {
        if (visitor.takesLayer(fragment, at) || visitor.replay(fragment, at, LayoutPhase_Body))
            return;
        PrLayoutVec from = visitor.enterSubtree(fragment, at, LayoutPhase_Body);
        whole(fragment, from, false);
        visitor.leaveSubtree();
    }

    // Visits the block-level content of a box at `offset`: a table's track backgrounds before its parts, and its
    // collapsed borders after them (css-tables-3/#drawing-cell-backgrounds). The collapsed borders belong to the table,
    // so a table that is not visible draws none.
    void blocks(const LayoutFragment* fragment, PrLayoutVec offset) {
        bool table = (fragment->flags & LayoutFragmentFlag_Table) != 0;
        if (table)
            visitor.gridBackgrounds(fragment, offset);
        content(fragment, offset, LayoutPhase_Blocks);
        if (table && (fragment->node == kPrLayoutNodeNone || isVisible(fragment->node)))
            visitor.gridBorders(fragment, offset);
    }

    // Visits the content of a box at `offset` for one phase.
    void content(const LayoutFragment* fragment, PrLayoutVec offset, LayoutPhase phase) {
        if (phase == LayoutPhase_Floats && !layoutFragmentHoldsFloats(fragment))
            return;
        LayoutWalkEntered entered = enterContent(fragment, offset);
        PrLayoutVec base = entered.base;
        // In a block container, each child's index is the walk's position among its lines (`place.at.child`). In an
        // anonymous block, it is the line within the enclosing container (`place.at.line`). Each child's index is
        // written through `at`, with one store.
        LayoutWalkPlace saved = place;
        uint32_t ignored = 0;
        uint32_t* at = &ignored;
        if (fragment->kind == LayoutFragmentKind_Box) {
            if (fragment->node == kPrLayoutNodeNone) {
                at = &place.at.line;
            } else {
                place = LayoutWalkPlace{fragment, base, LayoutPieceAt{0, kLayoutPieceNone}};
                at = &place.at.child;
            }
        }
        // Only a fragment with lines has floats anchored on them. A flex or grid container's children in flow are its
        // items.
        bool lines = (fragment->flags & LayoutFragmentFlag_Lines) != 0;
        bool items = fragment->kind == LayoutFragmentKind_Box && fragment->node != kPrLayoutNodeNone &&
            layoutChildrenAreItems(layoutTreeNode(tree, fragment->node).children);
        // The floats phase visits the runs that hold the children that are floats or that hold floats. The boxes out of
        // flow that a holder holds, which are its last children, paint from their stacking contexts' lists.
        uint32_t first = 0;
        uint32_t last = layoutFragmentHeldFirst(fragment);
        if (phase == LayoutPhase_Floats) {
            first = fragment->floatsFirst - fragment->floatsFirst % kPaintRun;
            last = fragment->floatsEnd;
        }
        for (uint32_t start = first; start < last;) {
            // A run ends where the phase's children end, before the boxes out of flow that a holder holds.
            uint32_t end = visitor.runEnd(fragment, start);
            if (end > last)
                end = last;
            if (visitor.replayRun(fragment, start, end, base, phase)) {
                start = end;
                continue;
            }
            bool run = visitor.enterRun(fragment, start, end, base, phase);
            for (uint32_t i = start; i < end; ++i) {
                *at = i;
                child(fragment->children[i], base, phase, lines, items);
            }
            if (run)
                visitor.leaveRun();
            start = end;
        }
        place = saved;
        if (phase == LayoutPhase_Inlines && (fragment->flags & LayoutFragmentFlag_Math) != 0 &&
                isVisible(fragment->node))
            visitor.math(fragment, base);
        leaveContent(entered);
    }

    // Visits one child of a box's content, whose content space is at `base`, for one phase. `lines` is true if the box
    // has floats anchored on its lines among its children, and `items` is true if it is a flex container.
    void child(const LayoutChild& child, PrLayoutVec base, LayoutPhase phase, bool lines, bool items) {
        const LayoutFragment* c = child.fragment;
        // The floats phase reaches only floats, through the boxes that hold them.
        if (phase == LayoutPhase_Floats && (c->flags & LayoutFragmentFlag_Float) == 0 && !layoutFragmentHoldsFloats(c))
            return;
        // A box out of flow, and therefore its placeholder, paints from a list, as does a box that paints whole.
        if ((c->flags & LayoutFragmentFlag_OutOfFlow) != 0 || c->kind == LayoutFragmentKind_Placeholder ||
                layoutFragmentPaintsWhole(tree, c))
            return;
        // A float anchored on a line is a piece of an inline box around it that paints whole.
        if (lines && (c->flags & LayoutFragmentFlag_Float) != 0 && layoutInOwningInline(tree, c->node))
            return;
        // A row is a piece of its inline box, which paints it if the box paints whole.
        if (c->kind == LayoutFragmentKind_Around && layoutNodeInlinePaintsWhole(tree, c->node))
            return;
        PrLayoutVec at = base + child.offset;
        // A flex item paints in the same way as an inline-block, with its container's inline content, in order-modified
        // document order, which is the order of its container's children (css-flexbox-2/#painting).
        if (items) {
            if (phase == LayoutPhase_Inlines)
                atomic(c, at);
            return;
        }
        if (!visitor.enters(c, at))
            return;
        if (c->kind == LayoutFragmentKind_Line) {
            if (phase == LayoutPhase_Inlines)
                lineRange(c, at, 0, c->childCount);
            return;
        }
        if ((c->flags & LayoutFragmentFlag_Float) != 0) {
            // A float paints in its stacking context's floats phase (CSS2/#floats).
            if (phase == LayoutPhase_Floats)
                atomic(c, at);
            return;
        }
        // A block-level replaced box paints its decoration with the blocks and its content with the inlines
        // (CSS2/#painting-order).
        if (c->kind == LayoutFragmentKind_Replaced) {
            if (phase == LayoutPhase_Blocks)
                visitor.box(c, layoutRectAt(at, c->extent), isVisible(c->node), false);
            else if (phase == LayoutPhase_Inlines)
                visitor.replaced(c, at, isVisible(c->node));
            return;
        }
        if (c->kind == LayoutFragmentKind_Around) {
            around(c, at, phase);
            return;
        }
        // A visitor can replay its cached recording of the subtree for this phase.
        if (visitor.replay(c, at, phase))
            return;
        // The visitor returns the offset from which it visits the subtree, in place of `at`.
        PrLayoutVec from = visitor.enterSubtree(c, at, phase);
        if (phase == LayoutPhase_Blocks && c->node != kPrLayoutNodeNone)
            visitor.box(c, layoutRectAt(from, c->extent), isVisible(c->node), false);
        if (phase == LayoutPhase_Blocks)
            blocks(c, from);
        else
            content(c, from, phase);
        visitor.leaveSubtree();
    }

    // Visits an inline box's row at `at` for one phase. A row paints nothing and is new with each layout of its
    // container, so it is not a subtree of its own: the walk passes through it to the block it holds, which may be one.
    // This is kept separate from child, which every fragment passes through, because rows are rare.
    PR_NOINLINE void around(const LayoutFragment* c, PrLayoutVec at, LayoutPhase phase) {
        if (phase == LayoutPhase_Blocks || phase == LayoutPhase_Inlines)
            visitor.around(c, layoutRectAt(at, c->extent), isVisible(c->node), phase);
        content(c, at, phase);
    }

    // Pushes the effects of a box at `offset` that paints whole, and returns what it pushed, or only
    // LayoutPushed_Hidden if the box shows the back that it hides.
    LayoutPushed pushEffects(const LayoutFragment* fragment, PrLayoutVec offset) {
        LayoutPushed pushed = 0;
        if (fragment->node == kPrLayoutNodeNone)
            return pushed;
        PrLayoutStyleIndex style = nodeOf(fragment).style;
        const LayoutStyleTraits& traits = layoutStyleTraits(styles, style);
        if (traits.transforms) {
            bool hidden = false;
            PrDMatrix3 plane = layoutBoxPlane(tree, fragment, offset, &hidden);
            if (hidden)
                return LayoutPushed_Hidden;
            visitor.pushTransform(plane);
            pushed |= LayoutPushed_Transform;
        }
        if (traits.composites && layoutStyle(styles, style).groups[PrLayoutGroup_Compositing] != 0) {
            const PrLayoutCompositingStyle& compositing = layoutCompositingStyle(styles, style);
            if (compositing.clipKind != PrLayoutClipKind_None) {
                visitor.pushClipPath(fragment, offset);
                pushed |= LayoutPushed_Clip;
            }
        }
        float opacity = traits.composites ? layoutEffectStyle(styles, style).opacity : 1;
        if (opacity < 1) {
            visitor.pushOpacity(opacity);
            pushed |= LayoutPushed_Opacity;
        }
        return pushed;
    }

    void popEffects(LayoutPushed pushed) {
        if ((pushed & LayoutPushed_Opacity) != 0)
            visitor.popOpacity();
        if ((pushed & LayoutPushed_Clip) != 0)
            visitor.popClipPath();
        if ((pushed & LayoutPushed_Transform) != 0)
            visitor.popTransform();
    }

    // Visits what a box that paints whole paints itself: its decoration; the boxes of its list below 0 if it is a
    // stacking context; its content in phases; and the boxes of its list at 0 and above if it is a stacking context.
    void body(const LayoutFragment* fragment, PrLayoutVec offset, bool root) {
        // A clip rectangle clips the box's own decoration too.
        bool clipped = fragment->node != kPrLayoutNodeNone && layoutFragmentClipped(tree, fragment);
        if (clipped)
            visitor.pushBoxClip(layoutClipRect(tree, fragment->node, layoutRectAt(offset, fragment->extent)));
        if (fragment->node != kPrLayoutNodeNone) {
            if (styles->wholeTableParts != 0 && layoutStyleTraits(styles, nodeOf(fragment).style).wholeTablePart)
                visitor.trackBackgrounds(fragment, offset);
            visitor.box(fragment, layoutRectAt(offset, fragment->extent), isVisible(fragment->node), root);
        }
        if (fragment->kind == LayoutFragmentKind_Replaced)
            visitor.replaced(fragment, offset, isVisible(fragment->node));
        if (clipped)
            visitor.popBoxClip();
        bool context = fragment->node != kPrLayoutNodeNone && layoutIsStackingContext(tree, fragment->node);
        if (context)
            paintList(fragment->node, offset, true);
        blocks(fragment, offset);
        content(fragment, offset, LayoutPhase_Floats);
        content(fragment, offset, LayoutPhase_Inlines);
        if (context)
            paintList(fragment->node, offset, false);
    }

    // Visits a box that paints whole: its effects around its body.
    void whole(const LayoutFragment* fragment, PrLayoutVec offset, bool root) {
        LayoutPushed pushed = pushEffects(fragment, offset);
        if ((pushed & LayoutPushed_Hidden) != 0)
            return;
        body(fragment, offset, root);
        popEffects(pushed);
    }

    // Visits the recording of a box with a layer of its own, at the origin and in the containers it paints in, which a
    // visitor that records it separately is already in. For a box, visits the body of `node`, whose fragment is
    // `fragment`, which is the root's if `root` is true. For an inline box `node`, visits its pieces from the line
    // `opening` of its block container's fragment `fragment`.
    void layer(LayoutNodeIndex node, const LayoutFragment* fragment, LayoutPieceAt opening, bool root) {
        chain.clear();
        layoutContainerChain(tree, layoutNodeContainer(tree, node), chain);
        for (uint32_t i = 0; i < chain.count; ++i)
            containers.push(LayoutWalkContainer{chain.data[i], kLayoutVecZero});
        if (node != fragment->node)
            inlineBody(node, LayoutWalkPlace{fragment, kLayoutVecZero, opening});
        else
            body(fragment, kLayoutVecZero, root);
    }

    // Visits the content of a root other than the viewport's, from the top left of its containing block, unscrolled.
    void otherRoot(const LayoutFragment* fragment) {
        if (fragment->childCount == 0)
            return;
        const LayoutChild& root = fragment->children[0];
        containers.push(LayoutWalkContainer{kLayoutContainerViewport, kLayoutVecZero});
        whole(root.fragment, root.offset, true);
    }

    // Visits the viewport's content, scrolled as the visitor says, and the root in it. The root's list holds the boxes
    // out of flow that the viewport holds.
    void viewport(const LayoutFragment* fragment) {
        if (fragment->childCount == 0)
            return;
        const LayoutChild& root = fragment->children[0];
        PrLayoutVec scroll = visitor.scrollViewport(layoutViewportScroll(tree, fragment));
        containers.push(LayoutWalkContainer{kLayoutContainerViewport, scroll});
        whole(root.fragment, root.offset - scroll, true);
    }
};

// Visits the tree's last layout with `visitor`, from the viewport.
template <typename Visitor>
void layoutWalk(const LayoutTree* tree, Visitor& visitor) {
    if (tree->fragment == nullptr)
        return;
    LayoutWalk<Visitor> w{.tree = tree, .styles = tree->styles, .visitor = visitor};
    w.viewport(tree->fragment);
    w.release();
}

// Visits the last layout of a root other than the viewport's, whose fragment is `fragment`, with `visitor`.
template <typename Visitor>
void layoutWalkRoot(const LayoutTree* tree, const LayoutFragment* fragment, Visitor& visitor) {
    LayoutWalk<Visitor> w{.tree = tree, .styles = tree->styles, .visitor = visitor};
    w.otherRoot(fragment);
    w.release();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
