// Geometry queries: the boxes of a tree's last layout, from where layout placed them, and hit testing.

#include "layout/src/layout_query.h"

#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/text_buffer.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"

#include "layout/src/layout_cache.h"
#include "layout/src/layout_effects.h"
#include "layout/src/layout_shaping.h"
#include "layout/src/layout_sticky.h"
#include "layout/src/layout_text_position.h"
#include "layout/src/layout_walk.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Hit areas

// Drops the cached hit areas.
void clearHitBounds(LayoutQueries& queries) {
    for (uint32_t i = 0; i < queries.hitBounds.count; ++i)
        layoutFragmentRelease(queries.hitBounds.data[i].fragment);
    queries.hitBounds.clear();
    queries.hitIndex.clear();
}

PrLayoutRect placedHitBounds(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset);

// Returns the area in which a fragment's subtree can be hit, from its top-left corner: its own box, and the areas that
// its descendants reach, through the transforms of those that paint whole. A box that clips limits its content to
// itself in the axes it clips. The area is cached while the effects of its nodes are the ones it was found for, and
// while the tree holds the fragment, which is immutable.
PrLayoutRect hitBounds(const LayoutTree* tree, const LayoutFragment* fragment) {
    PrLayoutRect bounds = layoutRectAt(kLayoutVecZero, fragment->extent);
    LayoutClips clips = layoutFragmentClips(tree, fragment);
    bool clipped = layoutFragmentClipped(tree, fragment);
    if (fragment->childCount == 0 || (clips.x && clips.y && !clipped))
        return bounds;
    LayoutQueries& queries = tree->queries;
    if (queries.hitEffects != tree->effects) {
        clearHitBounds(queries);
        queries.hitEffects = tree->effects;
    }
    uint32_t hash = hashPointer(fragment, 0);
    uint32_t at = queries.hitIndex.find(hash, [&](uint32_t i) { return queries.hitBounds.data[i].fragment == fragment; });
    if (at != kHashMissing)
        return queries.hitBounds.data[at].bounds;
    PrLayoutRect own = bounds;
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        if (!layoutFragmentIsPoint(child.fragment))
            bounds = layoutRectUnion(bounds, placedHitBounds(tree, child.fragment, child.offset));
    }
    if (clips.x) {
        bounds.left = own.left;
        bounds.right = own.right;
    }
    if (clips.y) {
        bounds.top = own.top;
        bounds.bottom = own.bottom;
    }
    // A clip rectangle may reach past the box, so the box stays in its own bounds.
    if (clipped)
        bounds = layoutRectUnion(own, layoutRectIntersect(bounds, layoutClipRect(tree, fragment->node, own)));
    layoutFragmentRetain(fragment);
    queries.hitBounds.push(LayoutHitBound{fragment, bounds});
    queries.hitIndex.insert(hash, queries.hitBounds.count - 1);
    return bounds;
}

// Returns the area in which a fragment's subtree can be hit, at `offset` in its parent. If the fragment paints whole,
// the area passes through its own transform, which its parent's walk applies around it.
PrLayoutRect placedHitBounds(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset) {
    PrLayoutRect bounds = layoutRectMoved(hitBounds(tree, fragment), offset);
    if (fragment->kind != LayoutFragmentKind_Box || !layoutFragmentPaintsWhole(tree, fragment))
        return bounds;
    PrLayoutStyleIndex style = layoutTreeNode(tree, fragment->node).style;
    if (!layoutStyleTraits(tree->styles, style).transforms)
        return bounds;
    PrDMatrix3 plane = layoutBoxPlane(tree, fragment, offset, nullptr);
    PrDAffine2 affine{};
    if (prDAffine2FromDMatrix3(&plane, &affine))
        return layoutRectMapped(affine, bounds);
    return layoutRectProjected(plane, bounds);
}

// ---------------------------------------------------------------------------------------------------------------------
// Visitors

// Finds the nodes painted over a point, in painting order.
struct HitTester : LayoutWalkVisitor {
    const LayoutTree* tree;
    Array<PrLayoutVec> points;      // the point in each transform's space, the current last

    // Whether each clip and transform, with those around it, excludes the point: a clip that does not hold it, or a
    // transform whose inverse does not reach it.
    Array<bool> outside;
    Array<uint32_t>* out;

    PrLayoutVec point() const { return points.data[points.count - 1]; }
    bool excluded() const { return outside.count != 0 && outside.data[outside.count - 1]; }

    // Enters only a subtree that can be hit at the point. A box that paints whole in it is hit from its stacking
    // context's list, in its own containers.
    bool enters(const LayoutFragment* fragment, PrLayoutVec offset) const {
        return !excluded() && layoutRectContains(placedHitBounds(tree, fragment, offset), point());
    }

    // A box that takes no pointer is not hit (css-ui-4/#pointer-events-control).
    void test(LayoutNodeIndex node, PrLayoutRect rect, bool visible) {
        bool takes = layoutColorStyle(tree->styles, layoutNodeStyle(tree, node)).pointerEvents !=
            PrLayoutPointerEvents_None;
        if (visible && takes && !excluded() && layoutRectContains(rect, point()))
            out->push(node);
    }

    // A column or column group is never hit. Its cells are hit, and the table between them. A box is hit only within
    // the curves of its corners (css-backgrounds-3/#corner-clipping).
    void box(const LayoutFragment* fragment, PrLayoutRect rect, bool visible, bool) {
        PrLayoutStyleIndex style = layoutTreeNode(tree, fragment->node).style;
        PrLayoutDisplay display = layoutBoxStyle(tree->styles, style).display;
        if (display == PrLayoutDisplay_TableColumn || display == PrLayoutDisplay_TableColumnGroup)
            return;
        if (!excluded() && layoutBorderContains(tree->styles, style, rect, point()))
            test(fragment->node, rect, visible);
    }

    // An inline box's row is hit under the block it holds.
    void around(const LayoutFragment* fragment, PrLayoutRect rect, bool visible, LayoutPhase phase) {
        if (phase == LayoutPhase_Blocks)
            test(fragment->node, rect, visible);
    }

    // A break has no extent along its line, and only gives a caret a place.
    void text(const LayoutFragment* fragment, PrLayoutVec offset, bool visible) {
        if ((fragment->flags & LayoutFragmentFlag_Break) == 0)
            test(fragment->node, layoutRectAt(offset, fragment->extent), visible);
    }

    void pushClip(const LayoutFragment* fragment, PrLayoutRect rect) {
        bool inside = layoutRectContains(rect, point());
        PrCorner corners[PrRectCorner_Count];
        if (inside && layoutClipCorners(tree, fragment, rect, corners)) {
            PrCornerRect shape{.rect = prLayoutRectToPx(rect)};
            memCopy(shape.corners, corners, sizeof(corners));
            inside = prCornerRectContains(shape, PrVec2{prLayoutUnitToPx(point().x), prLayoutUnitToPx(point().y)});
        }
        outside.push(excluded() || !inside);
    }
    void popClip() { outside.pop(); }

    // A clip path clips where a box is hit (css-masking-1/#clipping-paths).
    void pushClipPath(const LayoutFragment* fragment, PrLayoutVec offset) {
        PrLayoutVec at = point() - offset;
        bool inside = excluded() || layoutEffectsContain(tree, fragment->node, fragment, PrVec2{prLayoutUnitToPx(at.x),
            prLayoutUnitToPx(at.y)});
        outside.push(excluded() || !inside);
    }
    void popClipPath() { outside.pop(); }

    void pushBoxClip(PrLayoutRect rect) { outside.push(excluded() || !layoutRectContains(rect, point())); }
    void popBoxClip() { outside.pop(); }

    PrLayoutVec pushScroll(const LayoutFragment* fragment, PrLayoutRect padding, PrLayoutVec scroll) {
        if (layoutFragmentClipped(tree, fragment)) {
            const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, layoutTreeNode(tree, fragment->node).style);
            PrLayoutVec corner{padding.left - box.border[PrRectSide_Left], padding.top - box.border[PrRectSide_Top]};
            padding = layoutRectIntersect(padding,
                layoutClipRect(tree, fragment->node, layoutRectAt(corner, fragment->extent)));
        }
        pushClip(fragment, padding);
        return scroll;
    }
    void popScroll() { popClip(); }
    PrLayoutVec scrollViewport(PrLayoutVec scroll) { return scroll; }

    PrLayoutVec pushSticky(LayoutNodeIndex node) { return kLayoutVecZero - layoutNodeStickyShift(tree, node); }

    // A point that the inverse of a transform does not reach, behind the eye or through a collapse, hits nothing in the
    // transform's space.
    void pushTransform(const PrDMatrix3& transform) {
        PrDAffine2 affine{};
        PrDAffine2 inverse{};
        PrDMatrix3 projective{};
        PrLayoutVec mapped = point();
        bool reached = false;
        if (prDAffine2FromDMatrix3(&transform, &affine)) {
            reached = prDAffine2Invert(affine, &inverse);
            if (reached)
                mapped = layoutVecMapped(inverse, point());
        } else {
            reached = prDMatrix3Invert(&transform, &projective) && layoutVecProjected(projective, point(), &mapped);
        }
        points.push(mapped);
        outside.push(excluded() || !reached);
    }
    void popTransform() {
        points.pop();
        outside.pop();
    }
};

// Writes up to `capacity` of the nodes that a hit test found, in painting order, to `out`, topmost first and each once,
// and returns their number.
uint32_t topmostOnce(const LayoutTree* tree, const Array<uint32_t>& hits, PrLayoutNode* PR_NULLABLE out,
        uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t i = hits.count; i-- != 0;) {
        bool seen = false;
        for (uint32_t k = hits.count; k-- != i + 1 && !seen;)
            seen = hits.data[k] == hits.data[i];
        if (seen)
            continue;
        if (count < capacity)
            out[count] = layoutNodeName(tree, hits.data[i]);
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// Returns true if the node is in the tree's last layout: neither it nor any of its ancestors is absent from it
// (layoutIsAbsent), up to the root.
bool laidOut(const LayoutTree* tree, LayoutNodeIndex node) {
    if (node >= tree->nodes.count || !tree->nodes.data[node].live || tree->fragment == nullptr)
        return false;
    LayoutNodeIndex top = node;
    for (LayoutNodeIndex at = node; at != kPrLayoutNodeNone; at = layoutTreeNode(tree, at).parent) {
        if (layoutIsAbsent(tree, at))
            return false;
        top = at;
    }
    if (top != tree->root && !layoutTreeIsLaidOutRoot(tree, top))
        return false;
    // A node that a frame made since the last layout keeps the placement of the node that held its slot before, which
    // can name a box that is gone or is elsewhere. A placement holds only if each box that it passes through is an
    // ancestor of the box before it, as a containing block always is.
    for (LayoutNodeIndex at = node;;) {
        LayoutNodeIndex parent = layoutCachePlacement(tree->cache, at).parent;
        if (parent == kPrLayoutNodeNone)
            return true;
        LayoutNodeIndex above = layoutTreeNode(tree, at).parent;
        while (above != kPrLayoutNodeNone && above != parent)
            above = layoutTreeNode(tree, above).parent;
        if (above == kPrLayoutNodeNone)
            return false;
        at = parent;
    }
}

// The transform from a space to the coordinates that a query answers in, composed from the boxes that place it. The
// transform is affine until a box's transform with perspective joins it, and projective from then on.
struct Mapping {
    PrDAffine2 transform;
    PrDMatrix3 projective;          // used if `projected` is set
    bool projected;
    bool painted;                   // true for viewport coordinates, through transforms and scroll offsets
    bool sticky;                    // true if sticky boxes are at their sticky offsets

    // If set, the mapping is kept as stages instead of one composition: each projective transform is a stage of its
    // own, and the affine transforms between them compose into the stages around them. A point that maps back through
    // the stages one at a time is checked against each plane's eye, which one composed matrix cannot do.
    Array<PrDMatrix3>* PR_NULLABLE stages;
};

// Appends the composition that `m` holds to its stages, and starts a new one.
void flush(Mapping& m) {
    m.stages->push(m.projected ? m.projective : prDMatrix3FromDAffine2(m.transform));
    m.transform = prDAffine2Identity();
    m.projected = false;
}

// Appends `transform` to `m`.
void append(Mapping& m, const PrDMatrix3& transform) {
    PrDAffine2 affine{};
    if (!m.projected && prDAffine2FromDMatrix3(&transform, &affine)) {
        m.transform = prDAffine2Concat(m.transform, affine);
        return;
    }
    if (m.stages != nullptr) {
        flush(m);
        m.stages->push(transform);
        return;
    }
    if (!m.projected)
        m.projective = prDMatrix3FromDAffine2(m.transform);
    m.projective = prDMatrix3Concat(&m.projective, &transform);
    m.projected = true;
}

// Appends a move by `offset` to `m`.
void moveBy(Mapping& m, PrLayoutVec offset) {
    if (m.projected) {
        PrDMatrix3 move = prDMatrix3FromDAffine2(layoutTranslation(offset));
        m.projective = prDMatrix3Concat(&m.projective, &move);
    } else
        m.transform = prDAffine2Concat(m.transform, layoutTranslation(offset));
}

// Appends to `m` the mapping from the content of the box `node`, or from the viewport's content for kPrLayoutNodeNone,
// to the query's coordinates: its scroll offset if it is a scroll container, then the mapping from its border box.
void fromContent(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m);

// Returns how far the sticky boxes that placements pass over move `node` from where its placement puts it: the node
// itself if it sticks, and the sticky inline boxes around it (layoutStickyInlineAround).
PrLayoutVec stickyMoves(const LayoutTree* tree, LayoutNodeIndex node) {
    PrLayoutVec moved = kLayoutVecZero;
    if (layoutTreeNode(tree, node).kind != PrLayoutNodeKind_Text && layoutNodeSticks(tree, node))
        moved = layoutNodeStickyShift(tree, node);
    for (LayoutNodeIndex at = layoutStickyInlineAround(tree, node); at != kPrLayoutNodeNone;
            at = layoutStickyInlineAround(tree, at))
        moved = moved + layoutNodeStickyShift(tree, at);
    return moved;
}

// Appends to `m` the mapping from the content of the block container of the inline box or text `node`, whose placement
// names it as `parent`, with the moves of the sticky boxes that its placement passes over.
void fromPieces(const LayoutTree* tree, LayoutNodeIndex node, LayoutNodeIndex parent, Mapping& m) {
    if (m.sticky)
        moveBy(m, stickyMoves(tree, node));
    fromContent(tree, parent, m);
}

// Appends to `m` the mapping from the border box of the box `node` to the query's coordinates: its transform, its
// position in its parent's content with the moves of the sticky boxes that its placement passes over, and onward from
// there. The root and the absolutely positioned boxes that the viewport holds are in the viewport's scrolled content,
// and the fixed boxes that it holds are in the viewport itself.
void fromBox(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m) {
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
    if (m.painted && fragment != nullptr && layoutFragmentPaintsWhole(tree, fragment)) {
        PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
        if (layoutStyleTraits(tree->styles, style).transforms)
            append(m, layoutBoxPlane(tree, fragment, kLayoutVecZero, nullptr));
    }
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    moveBy(m, placement.offset);
    if (m.sticky)
        moveBy(m, stickyMoves(tree, node));
    if (placement.parent != kPrLayoutNodeNone) {
        fromContent(tree, placement.parent, m);
        return;
    }
    // The viewport scrolls its root's content, and no other root's.
    bool root = layoutTreeNode(tree, node).parent == kPrLayoutNodeNone;
    if (m.painted && layoutNodeRoot(tree, node) == tree->root && (root || !layoutIsFixed(tree, node)))
        moveBy(m, kLayoutVecZero - layoutViewportScroll(tree, tree->fragment));
}

void fromContent(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m) {
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
    if (m.painted && layoutFragmentScrolls(tree, fragment))
        moveBy(m, kLayoutVecZero - layoutFragmentScroll(tree, fragment));
    fromBox(tree, node, m);
}

// Returns `rect` mapped by `m`.
PrLayoutRect mapped(const Mapping& m, PrLayoutRect rect) {
    return m.projected ? layoutRectProjected(m.projective, rect) : layoutRectMapped(m.transform, rect);
}

// A text fragment of a text node, at `offset` in its block container's content, and the line box that holds it.
struct PlacedText {
    const LayoutFragment* fragment;
    PrLayoutVec offset;
    PrLayoutRect line;
    const LayoutFragment* lineFragment;
};

// Calls `fn` with each text fragment of the text `node`, in content order and in its block container's content. Its
// first fragment is on the line `at` of its block container `container`. Its fragments are on that line and the
// following lines of the same run of lines.
template <typename Fn>
void eachText(LayoutNodeIndex node, const LayoutFragment* container, LayoutPieceAt at, Fn fn) {
    auto scan = [&](const LayoutFragment* line, PrLayoutVec offset) {
        PrLayoutRect lineRect = layoutRectAt(offset, line->extent);
        for (uint32_t k = 0; k < line->childCount; ++k) {
            const LayoutChild& child = line->children[k];
            if (child.fragment->kind == LayoutFragmentKind_Text && child.fragment->node == node)
                fn(PlacedText{child.fragment, offset + child.offset, lineRect, line});
        }
    };
    // A text that the last layout gave no fragment keeps a placement from an older layout, which may name a line that
    // no longer exists.
    if (at.child >= container->childCount)
        return;
    if (at.line != kLayoutPieceNone) {
        const LayoutChild& block = container->children[at.child];
        for (uint32_t k = at.line; k < block.fragment->childCount; ++k) {
            const LayoutChild& line = block.fragment->children[k];
            if (line.fragment->kind == LayoutFragmentKind_Line)
                scan(line.fragment, block.offset + line.offset);
        }
        return;
    }
    for (uint32_t i = at.child; i < container->childCount; ++i) {
        const LayoutChild& line = container->children[i];
        if (line.fragment->kind == LayoutFragmentKind_Line)
            scan(line.fragment, line.offset);
        else if (line.fragment->kind == LayoutFragmentKind_Around ||
                (line.fragment->kind == LayoutFragmentKind_Box && line.fragment->node == kPrLayoutNodeNone))
            return;
    }
}

// Appends the line rectangles of the text `node`, in content order and in its block container's content. A break has no
// extent along its line, and only gives a caret a place.
void textBoxes(LayoutNodeIndex node, const LayoutFragment* container, LayoutPieceAt at, Array<PrLayoutRect>& out) {
    eachText(node, container, at, [&](const PlacedText& piece) {
        if ((piece.fragment->flags & LayoutFragmentFlag_Break) == 0)
            out.push(layoutRectAt(piece.offset, piece.fragment->extent));
    });
}

// Appends the boxes of the inline box `node`, which opens on the line `at` of its block container `container`, in
// content order and in its container's content: its parts on lines, and its rows around the blocks inside it.
void inlineBoxes(LayoutNodeIndex node, const LayoutFragment* container, LayoutPieceAt at, Array<PrLayoutRect>& out) {
    LayoutPieces pieces = layoutPiecesFrom(container, kLayoutVecZero, node, at);
    LayoutPiece piece{};
    while (layoutPiecesNext(pieces, &piece)) {
        if (piece.kind == LayoutPieceKind_Row) {
            out.push(layoutRectAt(piece.offset, piece.fragment->extent));
            continue;
        }
        for (uint32_t k = piece.first; k < piece.parts; ++k) {
            const LayoutChild& part = piece.fragment->children[k];
            out.push(layoutRectAt(piece.offset + part.offset, part.fragment->extent));
        }
    }
}

// Appends the boxes of `node` in the last layout to `out`, in content order and unmapped, and appends to `m` the
// mapping from the space they are in to the query's coordinates. A box element's box is its border box at the origin of
// its own space. The pieces of an inline box or a text are in the content of their block container.
void boxesIn(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m, Array<PrLayoutRect>& out) {
    if (!laidOut(tree, node))
        return;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if (n.kind == PrLayoutNodeKind_LineBreak)
        return;
    bool pieces = n.kind == PrLayoutNodeKind_Text || layoutIsInlineBox(tree, node);
    if (!pieces) {
        const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
        if (fragment == nullptr)
            return;
        fromBox(tree, node, m);
        out.push(layoutRectAt(kLayoutVecZero, fragment->extent));
        return;
    }
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    if (placement.parent == kPrLayoutNodeNone)
        return;
    const LayoutFragment* container = layoutCachePlaced(tree->cache, placement.parent);
    fromPieces(tree, node, placement.parent, m);
    if (n.kind == PrLayoutNodeKind_Text)
        textBoxes(node, container, placement.at, out);
    else
        inlineBoxes(node, container, placement.at, out);
}

// Appends the boxes of `node` in the last layout to `out`, in `coordinates` and in content order: where the last layout
// placed the node, mapped through the boxes that place it.
void boxesOf(const LayoutTree* tree, PrLayoutCoordinates coordinates, LayoutNodeIndex node, Array<PrLayoutRect>& out) {
    Mapping m{
        .transform = prDAffine2Identity(),
        .painted = coordinates == PrLayoutCoordinates_Viewport,
        .sticky = coordinates != PrLayoutCoordinates_Layout,
    };
    uint32_t first = out.count;
    boxesIn(tree, node, m, out);
    for (uint32_t i = first; i < out.count; ++i)
        out.data[i] = mapped(m, out.data[i]);
}

// Calls `fn` with each text fragment of the text `node` in the last layout, in content order and in its block
// container's content, and appends to `m` the mapping from that content to the query's coordinates. Returns false if
// the node is not text that the last layout placed.
template <typename Fn>
bool textPieces(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m, Fn fn) {
    if (!laidOut(tree, node) || layoutTreeNode(tree, node).kind != PrLayoutNodeKind_Text)
        return false;
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    if (placement.parent == kPrLayoutNodeNone)
        return false;
    fromPieces(tree, node, placement.parent, m);
    eachText(node, layoutCachePlaced(tree->cache, placement.parent), placement.at, fn);
    return true;
}

// Returns how far `offset` is from the bytes of the text fragment, in bytes, doubled, plus one if the fragment holds
// the offset only at the end that `affinity` does not prefer. The least is the fragment that a caret at the offset
// stands in.
uint32_t caretDistance(const LayoutFragment* fragment, uint32_t offset, PrLayoutTextAffinity affinity) {
    uint32_t start = layoutTextStart(fragment);
    uint32_t end = fragment->text.end;
    if (offset < start)
        return 2 * (start - offset);
    if (offset > end)
        return 2 * (offset - end);
    bool preferred = affinity == PrLayoutTextAffinity_Downstream ? offset < end : offset > start;
    return preferred ? 0 : 1;
}

// Returns the fragment of the box of the element `node` from the last layout, or null if `node` is an inline box, text,
// an element that generates no box, or an erased node. Every box of an element except an inline box has a fragment of
// its own, placed with it. A placement outlives its box if the index, or an ancestor, no longer generates one.
const LayoutFragment* PR_NULLABLE placedBoxOf(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    if (!prLayoutNodeIsValid(tree, node) || t->fragment == nullptr)
        return nullptr;
    LayoutNodeIndex index = uint32_t(node);
    const LayoutTreeNode& n = layoutTreeNode(t, index);
    bool inlineBox = layoutKindIsElement(n.kind) &&
        prLayoutDisplayIsInlineBox(layoutBoxStyle(t->styles, n.style).display);
    if (n.kind == PrLayoutNodeKind_Text || n.kind == PrLayoutNodeKind_LineBreak || inlineBox)
        return nullptr;
    uint32_t top = index;
    for (uint32_t at = index; at != kPrLayoutNodeNone; at = layoutTreeNode(t, at).parent) {
        if (layoutIsAbsent(t, at))
            return nullptr;
        top = at;
    }
    if (top != t->root && !layoutTreeIsLaidOutRoot(t, top))
        return nullptr;
    return layoutCachePlaced(t->cache, index);
}

// Fills the index of each text node's item from the shapings of the tree's contexts.
void indexRenderedItems(LayoutTree* tree) {
    LayoutQueries& queries = tree->queries;
    queries.rendered.clear();
    queries.renderedLayouts = tree->layouts;
    for (uint32_t i = 0; i < tree->shapings.count; ++i) {
        const LayoutInlineShaping* shaping = tree->shapings.data[i];
        if (shaping == nullptr)
            continue;
        for (uint32_t k = 0; k < shaping->items.count; ++k) {
            const LayoutInlineItem& item = shaping->items.data[k];
            if (item.kind != LayoutInlineItemKind_Text)
                continue;
            while (queries.rendered.count <= item.node)
                queries.rendered.push(LayoutRenderedItem{});
            queries.rendered.data[item.node] = LayoutRenderedItem{shaping, k};
        }
    }
}

// Returns true if nothing that a line holds comes before byte `byte` of item `at` on its line: the segment starts
// there, or a forced break comes before it.
bool startsLine(const LayoutInlineShaping& shaping, uint32_t at, uint32_t byte) {
    if (byte > shaping.items.data[at].start)
        return shaping.text.data[byte - 1] == '\n';
    for (uint32_t i = at; i-- != 0;) {
        const LayoutInlineItem& item = shaping.items.data[i];
        switch (item.kind) {
            case LayoutInlineItemKind_Text:
                if (item.end > item.start)
                    return shaping.text.data[item.end - 1] == '\n';
                break;
            case LayoutInlineItemKind_Atomic: return false;
            case LayoutInlineItemKind_Break:
            case LayoutInlineItemKind_Block: return true;
            default: break;
        }
    }
    return true;
}

// Returns true if nothing that a line holds comes at byte `byte` or after it, past item `at`: the segment ends there,
// or a forced break comes next.
bool endsLine(const LayoutInlineShaping& shaping, uint32_t at, uint32_t byte) {
    if (byte < shaping.items.data[at].end)
        return shaping.text.data[byte] == '\n';
    for (uint32_t i = at + 1; i < shaping.items.count; ++i) {
        const LayoutInlineItem& item = shaping.items.data[i];
        switch (item.kind) {
            case LayoutInlineItemKind_Text:
                if (item.end > item.start)
                    return shaping.text.data[item.start] == '\n';
                break;
            case LayoutInlineItemKind_Atomic: return false;
            case LayoutInlineItemKind_Break:
            case LayoutInlineItemKind_Block: return true;
            default: break;
        }
    }
    return true;
}

// What the rendered text steps have produced. A run of required line breaks waits until text follows it, so that none
// stands at the start or the end of the result.
struct RenderedWalk {
    LayoutTree* tree;
    PrLayoutRenderedTextDesc desc;
    TextBuffer out;
    uint32_t breaks;
};

void renderedAppend(RenderedWalk& walk, PrStringView text) {
    if (text.size == 0)
        return;
    if (textView(&walk.out).size != 0) {
        for (uint32_t i = 0; i < walk.breaks; ++i)
            textAppendChar(&walk.out, '\n');
    }
    walk.breaks = 0;
    textAppend(&walk.out, text);
}

void renderedRequireBreaks(RenderedWalk& walk, uint32_t breaks) {
    walk.breaks = breaks > walk.breaks ? breaks : walk.breaks;
}

void renderedAppendTextNode(RenderedWalk& walk, LayoutNodeIndex node) {
    const LayoutQueries& queries = walk.tree->queries;
    if (node >= queries.rendered.count || queries.rendered.data[node].shaping == nullptr)
        return;
    const LayoutRenderedItem& found = queries.rendered.data[node];
    const LayoutInlineShaping& shaping = *found.shaping;
    const LayoutInlineItem& item = shaping.items.data[found.item];
    uint32_t start = item.start;
    uint32_t end = item.end;
    if (start < end && (shaping.spaces.data[start] & PrTextSpaceFlag_Collapsible) != 0 &&
            startsLine(shaping, found.item, start))
        ++start;
    if (start < end && (shaping.spaces.data[end - 1] & PrTextSpaceFlag_Collapsible) != 0 &&
            endsLine(shaping, found.item, end))
        --end;
    renderedAppend(walk, PrStringView{shaping.text.data + start, end - start});
}

PrLayoutDisplay renderedDisplay(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    return layoutKindIsElement(n.kind) ? layoutBoxStyle(tree->styles, n.style).display : PrLayoutDisplay_Inline;
}

bool isTableRowGroup(PrLayoutDisplay display) {
    return display == PrLayoutDisplay_TableRowGroup || display == PrLayoutDisplay_TableHeaderGroup ||
        display == PrLayoutDisplay_TableFooterGroup;
}

// Returns true if a sibling after `node` has the display `display`.
bool hasLaterSibling(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutDisplay display) {
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).next; at != kPrLayoutNodeNone; at =
            layoutTreeNode(tree, at).next) {
        if (renderedDisplay(tree, at) == display)
            return true;
    }
    return false;
}

// Returns true if a row follows `row` in its table, in its row group or in a later one.
bool hasLaterRow(const LayoutTree* tree, LayoutNodeIndex row) {
    if (hasLaterSibling(tree, row, PrLayoutDisplay_TableRow))
        return true;
    LayoutNodeIndex group = layoutTreeNode(tree, row).parent;
    if (group == kPrLayoutNodeNone || !isTableRowGroup(renderedDisplay(tree, group)))
        return false;
    for (LayoutNodeIndex at = layoutTreeNode(tree, group).next; at != kPrLayoutNodeNone;
            at = layoutTreeNode(tree, at).next) {
        PrLayoutDisplay display = renderedDisplay(tree, at);
        if (display == PrLayoutDisplay_TableRow)
            return true;
        if (isTableRowGroup(display) && layoutTreeNode(tree, at).first != kPrLayoutNodeNone)
            return true;
    }
    return false;
}

// Returns true if the box of an element is block-level or a table caption, or is out of flow, which blockifies it.
bool rendersAsBlock(const LayoutTree* tree, LayoutNodeIndex node) {
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, n.style);
    if (n.positioned || box.floating != PrLayoutFloat_None || box.display == PrLayoutDisplay_TableCaption)
        return true;
    return !prLayoutDisplayIsInlineLevel(box.display) && !prLayoutDisplayIsLayoutInternal(box.display);
}

void renderedCollectChildren(RenderedWalk& walk, LayoutNodeIndex node);

// html/#rendered-text-collection-steps, for a node that the tree holds. A node without a box in the tree, as display:
// none gives, is not rendered, and display: contents gives its children to its parent.
void renderedCollect(RenderedWalk& walk, LayoutNodeIndex node) {
    LayoutTree* tree = walk.tree;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    if ((n.rendered & PrLayoutRenderedFlag_Skipped) != 0 ||
            (walk.desc.skips != nullptr && walk.desc.skips(walk.desc.context, layoutNodeName(tree, node))))
        return;
    LayoutNodeIndex parent = n.parent;
    bool visible = parent == kPrLayoutNodeNone ||
        layoutColorStyle(tree->styles, layoutTreeNode(tree, parent).style).visibility == PrLayoutVisibility_Visible;
    switch (n.kind) {
        case PrLayoutNodeKind_Text:
            if (visible)
                renderedAppendTextNode(walk, node);
            return;
        case PrLayoutNodeKind_LineBreak:
            if (visible)
                renderedAppend(walk, "\n");
            return;
        case PrLayoutNodeKind_Replaced: {
            uint32_t breaks = rendersAsBlock(tree, node) ? 1 : 0;
            renderedRequireBreaks(walk, breaks);
            const LayoutReplaced& replaced = layoutReplaced(tree, node);
            if (replaced.text != nullptr) {
                PrString text = replaced.text(replaced.textContext);
                renderedAppend(walk, prStringView(&text));
                prStringReset(&text);
            }
            renderedRequireBreaks(walk, breaks);
            return;
        }
        default:
            break;
    }
    PrLayoutDisplay display = layoutBoxStyle(tree->styles, n.style).display;
    if (display == PrLayoutDisplay_None || display == PrLayoutDisplay_Marker)
        return;
    if (layoutColorStyle(tree->styles, n.style).visibility != PrLayoutVisibility_Visible) {
        renderedCollectChildren(walk, node);
        return;
    }
    uint32_t breaks = 0;
    if ((n.rendered & PrLayoutRenderedFlag_Paragraph) != 0)
        breaks = 2;
    else if ((n.rendered & PrLayoutRenderedFlag_Inline) == 0 && rendersAsBlock(tree, node))
        breaks = 1;
    renderedRequireBreaks(walk, breaks);
    renderedCollectChildren(walk, node);
    if (display == PrLayoutDisplay_TableCell && hasLaterSibling(tree, node, PrLayoutDisplay_TableCell))
        renderedAppend(walk, "\t");
    else if (display == PrLayoutDisplay_TableRow && hasLaterRow(tree, node))
        renderedAppend(walk, "\n");
    renderedRequireBreaks(walk, breaks);
}

void renderedCollectChildren(RenderedWalk& walk, LayoutNodeIndex node) {
    if ((layoutTreeNode(walk.tree, node).rendered & PrLayoutRenderedFlag_ChildrenSkipped) != 0)
        return;
    for (LayoutNodeIndex child = layoutTreeNode(walk.tree, node).first; child != kPrLayoutNodeNone;
            child = layoutTreeNode(walk.tree, child).next)
        renderedCollect(walk, child);
}

bool caretPx(PrLayoutTree* tree, PrLayoutNode text, uint32_t offset, PrLayoutTextAffinity affinity, PrRect* out) {
    PrLayoutRect rect{};
    if (!prLayoutNodeTextCaret(tree, text, offset, affinity, PrLayoutCoordinates_Viewport, &rect))
        return false;
    *out = prLayoutRectToPx(rect);
    return true;
}

// Returns the node after `at` in the preorder of the subtree of `top`, or kPrLayoutNodeNone at its end.
LayoutNodeIndex subtreeNext(const LayoutTree* tree, LayoutNodeIndex at, LayoutNodeIndex top) {
    const LayoutTreeNode& n = layoutTreeNode(tree, at);
    if (n.first != kPrLayoutNodeNone)
        return n.first;
    while (at != top && layoutTreeNode(tree, at).next == kPrLayoutNodeNone)
        at = layoutTreeNode(tree, at).parent;
    return at == top ? LayoutNodeIndex(kPrLayoutNodeNone) : layoutTreeNode(tree, at).next;
}

// Returns the node before `at` in the preorder of the subtree of `top`, or kPrLayoutNodeNone at its start.
LayoutNodeIndex subtreePrevious(const LayoutTree* tree, LayoutNodeIndex at, LayoutNodeIndex top) {
    if (at == top)
        return kPrLayoutNodeNone;
    LayoutNodeIndex before = layoutTreeNode(tree, at).previous;
    if (before == kPrLayoutNodeNone)
        return layoutTreeNode(tree, at).parent;
    while (layoutTreeNode(tree, before).last != kPrLayoutNodeNone)
        before = layoutTreeNode(tree, before).last;
    return before;
}

// A text fragment on a line of a query of lines, and where its line-left edge stands along the line.
struct LinePiece {
    const LayoutFragment* fragment;
    LayoutNodeIndex node;
    uint32_t line;                  // by the order of the query's lines
    float along;
};

// The identity of a line box in a query of lines: a fragment can be placed more than once.
struct LineKey {
    const LayoutFragment* fragment;
    PrLayoutRect rect;
    LayoutNodeIndex container;
};

// Returns where the line-left edge of `rect`, the box of a fragment in layout coordinates, stands along its line, with
// `along` growing in the line-right direction.
float lineLeftAlong(const LayoutTree* tree, const LayoutFragment* fragment, const PrLayoutRect& rect) {
    if (!layoutStyleTraits(tree->styles, layoutFragmentStyle(tree, fragment)).vertical)
        return prLayoutUnitToPx(rect.left);
    if (fragment->turn == LayoutTextTurn_CounterClockwise)
        return -prLayoutUnitToPx(rect.bottom);
    return prLayoutUnitToPx(rect.top);
}

// Returns true if the fragment has a stop at the boundary `at`: a cluster inside a run of collapsed white space is not
// a place of its own, since a caret's move there would not show.
bool isLineStop(const LayoutFragment* fragment, PrStringView text, uint32_t at) {
    if (at >= fragment->text.end || at >= text.size)
        return true;
    char c = text.data[at];
    if (c != ' ' && c != '\t' && c != '\n')
        return true;
    const LayoutCaretStop* stops = layoutFragmentTextStops(fragment);
    for (uint32_t i = 0; i < fragment->text.stopCount; ++i) {
        if (stops[i].byte == at)
            return true;
    }
    return false;
}

// How near two stops stand, in px, to be one place: fragments are each placed in layout units.
constexpr float kLineStopSamePlace = 0.05f;

// Appends the stops of the fragment of `piece`, in order of `along`. A stop at a position that the last stop has, at
// the same place, is one stop, which stands with the text after it.
void appendLineStops(const PrLayoutTree* tree, const LinePiece& piece, Array<PrLayoutLineStop>& out) {
    const LayoutFragment* fragment = piece.fragment;
    PrLayoutNode name = layoutNodeName(tree, piece.node);
    PrStringView all = prLayoutNodeText(tree, name);
    uint32_t start = layoutTextStart(fragment);
    uint32_t end = fragment->text.end < all.size ? fragment->text.end : uint32_t(all.size);
    bool lone = (fragment->flags & LayoutFragmentFlag_Break) != 0;
    uint32_t first = out.count;
    PrStringView view{all.data, end};
    for (uint32_t b = start;;) {
        if (lone || isLineStop(fragment, all, b)) {
            PrLayoutTextAffinity affinity = PrLayoutTextAffinity_Downstream;
            if (!lone && b == end && b > start)
                affinity = PrLayoutTextAffinity_Upstream;
            float along = piece.along + layoutTextCaretAlong(tree, fragment, b);
            out.push(PrLayoutLineStop{{name, b}, affinity, along});
        }
        if (lone || b >= end)
            break;
        b = uint32_t(prUnicodeGraphemeNext(view, b));
    }
    if ((fragment->flags & LayoutFragmentFlag_Reversed) != 0) {
        for (uint32_t i = first, j = out.count; i + 1 < j; ++i) {
            --j;
            PrLayoutLineStop kept = out.data[i];
            out.data[i] = out.data[j];
            out.data[j] = kept;
        }
    }
    if (first != 0 && first < out.count) {
        const PrLayoutLineStop& before = out.data[first - 1];
        PrLayoutLineStop& after = out.data[first];
        // One place has one `along`, though each fragment is placed in layout units.
        if (absF32(before.along - after.along) < kLineStopSamePlace)
            after.along = before.along;
        if (before.position.node == after.position.node && before.position.offset == after.position.offset &&
                before.along == after.along) {
            out.data[first - 1].affinity = PrLayoutTextAffinity_Downstream;
            for (uint32_t i = first + 1; i < out.count; ++i)
                out.data[i - 1] = out.data[i];
            --out.count;
        }
    }
}

// Returns the direction of the paragraph around `position` under unicode-bidi: plaintext, which its first strong
// character decides (css-writing-modes-4/#unicode-bidi), or `fallback` if it has none.
PrTextDirection plaintextDirection(const PrLayoutTree* tree, PrLayoutTextPosition position, PrTextDirection fallback) {
    PrStringView text = prLayoutNodeText(tree, position.node);
    uint32_t start = position.offset < text.size ? position.offset : uint32_t(text.size);
    while (start > 0 && text.data[start - 1] != '\n')
        --start;
    uint32_t end = start;
    while (end < text.size && text.data[end] != '\n')
        ++end;
    switch (prUnicodeBidiFirstStrong(PrStringView{text.data + start, end - start})) {
        case PrUnicodeBidiStrong_Left: return PrTextDirection_Ltr;
        case PrUnicodeBidiStrong_Right: return PrTextDirection_Rtl;
        default: return fallback;
    }
}

// Returns the text node next to `text` in the subtree of `root`, walking by `step`. Writes whether nothing that
// separates lines lies between them to `contiguous`.
template <typename Step>
PrLayoutNode adjacentText(const PrLayoutTree* tree, PrLayoutNode root, PrLayoutNode text, bool* contiguous, Step step) {
    *contiguous = false;
    if (!prLayoutNodeIsValid(tree, root) || !prLayoutNodeIsValid(tree, text))
        return kPrLayoutNodeNone;
    LayoutNodeIndex top = uint32_t(root);
    LayoutNodeIndex from = uint32_t(text);
    bool separated = false;
    for (LayoutNodeIndex at = step(tree, from, top); at != kPrLayoutNodeNone; at = step(tree, at, top)) {
        PrLayoutNodeKind kind = layoutTreeNode(tree, at).kind;
        if (kind == PrLayoutNodeKind_Text) {
            if (!laidOut(tree, at) || layoutCachePlacement(tree->cache, at).parent == kPrLayoutNodeNone)
                continue;
            *contiguous = !separated &&
                layoutCachePlacement(tree->cache, at).parent == layoutCachePlacement(tree->cache, from).parent;
            return layoutNodeName(tree, at);
        }
        if (kind == PrLayoutNodeKind_LineBreak || !layoutIsInlineBox(tree, at))
            separated = true;
    }
    return kPrLayoutNodeNone;
}

} // namespace

bool layoutNodeLaidOut(const LayoutTree* tree, LayoutNodeIndex node) {
    return laidOut(tree, node);
}

const LayoutFragment* PR_NULLABLE layoutCaretFragment(const LayoutTree* tree, LayoutNodeIndex node, uint32_t offset,
        PrLayoutTextAffinity affinity) {
    Mapping m{.transform = prDAffine2Identity()};
    const LayoutFragment* best = nullptr;
    uint32_t nearest = UINT32_MAX;
    textPieces(tree, node, m, [&](const PlacedText& piece) {
        uint32_t distance = caretDistance(piece.fragment, offset, affinity);
        if (distance < nearest) {
            nearest = distance;
            best = piece.fragment;
        }
    });
    return best;
}

void layoutQueriesReset(LayoutQueries& queries) {
    queries.rendered.reset();
    queries.boxes.reset();
    clearHitBounds(queries);
    queries.hitBounds.reset();
    queries.hitIndex.reset();
}

void layoutQueriesRetire(LayoutTree* tree) {
    LayoutQueries& queries = tree->queries;
    for (uint32_t i = 0; i < tree->cache.retired.count; ++i) {
        const LayoutFragment* fragment = tree->cache.retired.data[i];
        uint32_t hash = hashPointer(fragment, 0);
        uint32_t at =
            queries.hitIndex.find(hash, [&](uint32_t k) { return queries.hitBounds.data[k].fragment == fragment; });
        if (at == kHashMissing)
            continue;
        layoutFragmentRelease(fragment);
        queries.hitIndex.erase(hash, at);
        uint32_t last = queries.hitBounds.count - 1;
        if (at != last) {
            const LayoutHitBound& moved = queries.hitBounds.data[last];
            queries.hitIndex.relocate(hashPointer(moved.fragment, 0), last, at);
            queries.hitBounds.data[at] = moved;
        }
        queries.hitBounds.pop();
    }
}

bool layoutQueriesVerify(LayoutTree* tree, uint32_t* node) {
    if (tree->fragment == nullptr)
        return true;
    // The placements that publishing the final tree from scratch gives.
    LayoutCache fresh{};
    layoutCachePlace(fresh, kPrLayoutNodeNone, tree->fragment);
    for (uint32_t i = 0; i < tree->roots.count; ++i)
        layoutCachePlace(fresh, kPrLayoutNodeNone, tree->roots.data[i].fragment);
    bool same = true;
    for (LayoutNodeIndex at = 0; at < tree->nodes.count && same; ++at) {
        if (tree->nodes.data[at].live && layoutTreeNode(tree, at).kind == PrLayoutNodeKind_LineBreak)
            continue;
        if (!laidOut(tree, at)) {
            // A node not in the final tree is placed by none of its fragments.
            same = layoutCachePlaced(fresh, at) == nullptr;
        } else {
            LayoutPlacement kept = layoutCachePlacement(tree->cache, at);
            LayoutPlacement found = layoutCachePlacement(fresh, at);
            // A text that the last layout gave no fragment is placed by no fragment, and keeps an older layout's
            // placement.
            bool unplaced = layoutTreeNode(tree, at).kind == PrLayoutNodeKind_Text && found.parent == kPrLayoutNodeNone;
            // A box in the final tree is placed with the fragment the tree holds of it.
            bool box = layoutTreeNode(tree, at).kind != PrLayoutNodeKind_Text && !layoutIsInlineBox(tree, at);
            same = unplaced ||
                (kept.parent == found.parent && memEquals(&kept.offset, &found.offset, sizeof(PrLayoutVec)) &&
                    kept.fragment == found.fragment && (!box || found.fragment != nullptr));
        }
        if (!same)
            *node = at;
    }
    layoutCacheReset(fresh);
    return same;
}

} // namespace pr_impl

using namespace pr_impl;

uint32_t prLayoutNodeBoxes(PrLayoutTree* tree, PrLayoutNode node, PrLayoutCoordinates coordinates,
        PrLayoutRect* PR_NULLABLE out, uint32_t capacity) {
    Array<PrLayoutRect>& boxes = tree->queries.boxes;
    boxes.clear();
    if (prLayoutNodeIsValid(tree, node))
        boxesOf(tree, coordinates, uint32_t(node), boxes);
    for (uint32_t i = 0; i < boxes.count && i < capacity; ++i)
        out[i] = boxes.data[i];
    return boxes.count;
}

PrLayoutRect prLayoutNodeBounds(PrLayoutTree* tree, PrLayoutNode node, PrLayoutCoordinates coordinates) {
    Array<PrLayoutRect>& boxes = tree->queries.boxes;
    boxes.clear();
    if (prLayoutNodeIsValid(tree, node))
        boxesOf(tree, coordinates, uint32_t(node), boxes);
    if (boxes.count == 0)
        return PrLayoutRect{};
    PrLayoutRect bounds = boxes.data[0];
    for (uint32_t i = 1; i < boxes.count; ++i)
        bounds = layoutRectUnion(bounds, boxes.data[i]);
    return bounds;
}

PrLayoutRect prLayoutNodeReplacedContent(PrLayoutTree* tree, PrLayoutNode node) {
    if (!prLayoutNodeIsValid(tree, node))
        return PrLayoutRect{};
    uint32_t index = layoutNodeIndex(tree, node);
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, index);
    if (fragment == nullptr || fragment->kind != LayoutFragmentKind_Replaced)
        return PrLayoutRect{};
    return fragment->replaced.content;
}

PrLayoutRect prLayoutNodeShownBounds(PrLayoutTree* tree, PrLayoutNode node) {
    PrLayoutRect shown = prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport);
    auto empty = [](PrLayoutRect r) { return r.right <= r.left || r.bottom <= r.top; };
    if (empty(shown))
        return PrLayoutRect{};
    // Each box that the node's placements pass through clips them, in the axes in which it clips, to its padding box.
    for (LayoutNodeIndex at = layoutCachePlacement(tree->cache, uint32_t(node)).parent; at != kPrLayoutNodeNone;
            at = layoutCachePlacement(tree->cache, at).parent) {
        const LayoutFragment* fragment = layoutCachePlaced(tree->cache, at);
        if (fragment == nullptr)
            break;
        LayoutClips clips = layoutFragmentClips(tree, fragment);
        bool clipped = layoutFragmentClipped(tree, fragment);
        if (!clips.x && !clips.y && !clipped)
            continue;
        Mapping m{.transform = prDAffine2Identity(), .painted = true, .sticky = true};
        fromBox(tree, at, m);
        if (clipped) {
            PrLayoutRect rect = mapped(m, layoutClipRect(tree, at, layoutRectAt(kLayoutVecZero, fragment->extent)));
            shown = layoutRectIntersect(shown, rect);
            if (empty(shown))
                return PrLayoutRect{};
        }
        PrLayoutRect clip = mapped(m, layoutPaddingBox(tree, fragment, kLayoutVecZero));
        if (clips.x) {
            shown.left = layoutUnitMax(shown.left, clip.left);
            shown.right = layoutUnitMin(shown.right, clip.right);
        }
        if (clips.y) {
            shown.top = layoutUnitMax(shown.top, clip.top);
            shown.bottom = layoutUnitMin(shown.bottom, clip.bottom);
        }
    }
    PrLayoutRect viewport = layoutRectAt(kLayoutVecZero, tree->viewport);
    shown.left = layoutUnitMax(shown.left, viewport.left);
    shown.top = layoutUnitMax(shown.top, viewport.top);
    shown.right = layoutUnitMin(shown.right, viewport.right);
    shown.bottom = layoutUnitMin(shown.bottom, viewport.bottom);
    return empty(shown) ? PrLayoutRect{} : shown;
}

bool prLayoutNodeLocalPoint(PrLayoutTree* tree, PrLayoutNode node, PrLayoutVec point, PrLayoutVec* out) {
    if (!prLayoutNodeIsValid(tree, node))
        return false;
    Array<PrLayoutRect>& boxes = tree->queries.boxes;
    Array<PrDMatrix3> stages;
    boxes.clear();
    Mapping m{.transform = prDAffine2Identity(), .painted = true, .sticky = true, .stages = &stages};
    boxesIn(tree, uint32_t(node), m, boxes);
    bool reached = boxes.count != 0;
    PrDVec2 at{prLayoutUnitToPxDouble(point.x), prLayoutUnitToPxDouble(point.y)};
    if (reached) {
        flush(m);
        // Back through each stage, from the viewport toward the box. A projective stage's inverse gives w the sign that
        // the stage gives the point it maps, so a point that a stage puts behind the eye fails to map back.
        for (uint32_t i = stages.count; i-- != 0 && reached;) {
            PrDMatrix3 inverse{};
            reached = prDMatrix3Invert(&stages.data[i], &inverse) && prDMatrix3MapPoint(&inverse, at, &at);
        }
    }
    if (reached) {
        PrLayoutRect first = boxes.data[0];
        *out = PrLayoutVec{
            prLayoutUnitFromPxRoundDouble(at.x - prLayoutUnitToPxDouble(first.left)),
            prLayoutUnitFromPxRoundDouble(at.y - prLayoutUnitToPxDouble(first.top)),
        };
    }
    stages.reset();
    return reached;
}

bool prLayoutNodeMapFromBox(PrLayoutTree* tree, PrLayoutNode node, PrDVec2* PR_NULLABLE points, uint32_t count) {
    if (!prLayoutNodeIsValid(tree, node))
        return false;
    Array<PrLayoutRect>& boxes = tree->queries.boxes;
    Array<PrDMatrix3> stages;
    boxes.clear();
    Mapping m{.transform = prDAffine2Identity(), .painted = true, .sticky = true, .stages = &stages};
    boxesIn(tree, uint32_t(node), m, boxes);
    bool reached = boxes.count != 0;
    if (reached) {
        flush(m);
        PrLayoutRect first = boxes.data[0];
        PrDVec2 origin{prLayoutUnitToPxDouble(first.left), prLayoutUnitToPxDouble(first.top)};
        for (uint32_t k = 0; k < count && reached; ++k) {
            PrDVec2 at{points[k].x + origin.x, points[k].y + origin.y};
            // From the box toward the viewport, the stages in the order that they were composed.
            for (uint32_t i = 0; i < stages.count && reached; ++i)
                reached = prDMatrix3MapPoint(&stages.data[i], at, &at);
            points[k] = at;
        }
    }
    stages.reset();
    return reached;
}

PrLayoutVec prLayoutNodeContentExtent(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    const LayoutFragment* fragment = placedBoxOf(tree, node);
    if (fragment == nullptr)
        return PrLayoutVec{};
    LayoutNodeIndex index = uint32_t(node);
    if (fragment->kind == LayoutFragmentKind_Replaced)
        return PrLayoutVec{layoutRectWidth(fragment->replaced.content), layoutRectHeight(fragment->replaced.content)};
    // Padding on every side resolves against the containing block's inline size (css-box-4/#padding-physical).
    const PrLayoutBoxStyle& box = layoutBoxStyle(t->styles, layoutTreeNode(t, index).style);
    PrLayoutUnit base = layoutUnitMax(kLayoutUnitZero, fragment->box.containingInline);
    PrLayoutUnit edges[PrRectSide_Count];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        edges[side] = box.border[side] + layoutUnitMax(kLayoutUnitZero, layoutResolve(t->styles, box.padding[side], base));
    return PrLayoutVec{
        layoutUnitMax(kLayoutUnitZero, fragment->extent.x - edges[PrRectSide_Left] - edges[PrRectSide_Right]),
        layoutUnitMax(kLayoutUnitZero, fragment->extent.y - edges[PrRectSide_Top] - edges[PrRectSide_Bottom]),
    };
}

PrLayoutUnit prLayoutNodeContainingInlineSize(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutFragment* fragment = placedBoxOf(tree, node);
    if (fragment == nullptr || fragment->kind != LayoutFragmentKind_Box)
        return layoutUnit(-1);
    return layoutUnitMax(kLayoutUnitZero, fragment->box.containingInline);
}

uint32_t prLayoutNodeGridTrackSizes(const PrLayoutTree* tree, PrLayoutNode node, PrLayoutGridAxis axis,
        PrLayoutUnit* PR_NULLABLE out, uint32_t capacity) {
    const LayoutFragment* fragment = placedBoxOf(tree, node);
    if (fragment == nullptr || (fragment->flags & LayoutFragmentFlag_Grid) == 0)
        return 0;
    LayoutGridAxisTracks tracks = layoutFragmentGridTracks(fragment, axis);
    for (uint32_t t = 0; t < tracks.count && t < capacity; ++t)
        out[t] = tracks.sizes[t];
    return tracks.count;
}

uint32_t prLayoutNodeGridLineNames(const PrLayoutTree* tree, PrLayoutNode node, PrLayoutGridAxis axis,
        PrLayoutGridLineName* PR_NULLABLE out, uint32_t capacity) {
    const LayoutTree* t = tree;
    const LayoutFragment* fragment = placedBoxOf(tree, node);
    if (fragment == nullptr || (fragment->flags & LayoutFragmentFlag_Grid) == 0)
        return 0;
    LayoutGridAxisTracks tracks = layoutFragmentGridTracks(fragment, axis);
    const PrLayoutGridStyle& grid = layoutGridStyle(t->styles, layoutTreeNode(t, uint32_t(node)).style);
    PrLayoutGridTracksIndex index = axis == PrLayoutGridAxis_Columns ? grid.templateColumns : grid.templateRows;
    const LayoutGridTracksEntry& list = layoutGridTracksAt(&t->styles->grids, index);
    // The names come in the order of their lines: those before the automatic repetition and at its start, those of each
    // repetition, which give an inner line the names that end one repetition before those that start the next, and
    // those at the repetition's end and after it.
    uint32_t count = 0;
    auto add = [&](uint32_t line, PrLayoutGridName name) {
        if (count < capacity)
            out[count] = PrLayoutGridLineName{uint32_t(int32_t(line) + tracks.explicitStart), name};
        ++count;
    };
    uint32_t repetitions = list.repeat != PrLayoutGridRepeat_None ? tracks.repetitions : 0;
    uint32_t shift = repetitions != 0 ? list.repeatCount * (repetitions - 1) : 0;
    for (uint32_t i = 0; i < list.nameCount; ++i) {
        if (repetitions == 0 || list.names[i].line <= list.repeatStart)
            add(list.names[i].line, list.names[i].name);
    }
    for (uint32_t r = 0; r < repetitions; ++r) {
        for (uint32_t i = 0; i < list.repeatNameCount; ++i)
            add(list.repeatStart + r * list.repeatCount + list.repeatNames[i].line, list.repeatNames[i].name);
    }
    for (uint32_t i = 0; i < list.nameCount && repetitions != 0; ++i) {
        if (list.names[i].line > list.repeatStart)
            add(list.names[i].line + shift, list.names[i].name);
    }
    return count;
}

PrStringView prLayoutGridNameText(const PrLayoutTree* tree, PrLayoutGridName name) {
    const LayoutTree* t = tree;
    if (name == kPrLayoutGridNameNone || name > t->styles->grids.names.atoms.count)
        return PrStringView{};
    return atomTableText(&t->styles->grids.names, name - 1);
}

PrLayoutUnit prLayoutNodeBorderWidth(const PrLayoutTree* tree, PrLayoutNode node, PrRectSide side) {
    const LayoutTree* t = tree;
    if (!prLayoutNodeIsValid(tree, node))
        return kLayoutUnitZero;
    const LayoutTreeNode& n = layoutTreeNode(t, layoutNodeIndex(t, node));
    if (n.kind == PrLayoutNodeKind_Text)
        return kLayoutUnitZero;
    return layoutBoxStyle(t->styles, n.style).border[side];
}

bool prLayoutNodeTextCaret(PrLayoutTree* tree, PrLayoutNode text, uint32_t offset, PrLayoutTextAffinity affinity,
        PrLayoutCoordinates coordinates, PrLayoutRect* out) {
    if (!prLayoutNodeIsValid(tree, text))
        return false;
    Mapping m{
        .transform = prDAffine2Identity(),
        .painted = coordinates == PrLayoutCoordinates_Viewport,
        .sticky = coordinates != PrLayoutCoordinates_Layout,
    };
    PlacedText best{};
    uint32_t nearest = UINT32_MAX;
    textPieces(tree, uint32_t(text), m, [&](const PlacedText& piece) {
        uint32_t distance = caretDistance(piece.fragment, offset, affinity);
        if (distance < nearest) {
            nearest = distance;
            best = piece;
        }
    });
    if (best.fragment == nullptr)
        return false;
    float along = layoutTextCaretAlong(tree, best.fragment, offset);
    *out = mapped(m, layoutTextSpanRect(tree, best.fragment, best.offset, along, along));
    return true;
}

uint32_t prLayoutNodeTextRangeBoxes(PrLayoutTree* tree, PrLayoutNode text, uint32_t start, uint32_t end,
        PrLayoutCoordinates coordinates, PrLayoutRect* PR_NULLABLE out, uint32_t capacity) {
    Array<PrLayoutRect>& boxes = tree->queries.boxes;
    boxes.clear();
    if (!prLayoutNodeIsValid(tree, text))
        return 0;
    Mapping m{
        .transform = prDAffine2Identity(),
        .painted = coordinates == PrLayoutCoordinates_Viewport,
        .sticky = coordinates != PrLayoutCoordinates_Layout,
    };
    textPieces(tree, uint32_t(text), m, [&](const PlacedText& piece) {
        const LayoutFragment* fragment = piece.fragment;
        uint32_t from = layoutTextStart(fragment);
        uint32_t to = fragment->text.end;
        from = start > from ? start : from;
        to = end < to ? end : to;
        if (from >= to || (fragment->flags & LayoutFragmentFlag_Break) != 0)
            return;
        boxes.push(layoutTextSpanRect(tree, fragment, piece.offset, layoutTextCaretAlong(tree, fragment, from),
            layoutTextCaretAlong(tree, fragment, to)));
    });
    for (uint32_t i = 0; i < boxes.count; ++i)
        boxes.data[i] = mapped(m, boxes.data[i]);
    for (uint32_t i = 0; i < boxes.count && i < capacity; ++i)
        out[i] = boxes.data[i];
    return boxes.count;
}

PrString prLayoutNodeRenderedText(PrLayoutTree* tree, PrLayoutNode root, const PrLayoutRenderedTextDesc* PR_NULLABLE desc) {
    if (!prLayoutNodeIsValid(tree, root))
        return prStringEmpty();
    if (tree->queries.renderedLayouts != tree->layouts || tree->queries.rendered.count == 0)
        indexRenderedItems(tree);
    RenderedWalk walk{.tree = tree, .desc = desc != nullptr ? *desc : PrLayoutRenderedTextDesc{}};
    renderedCollectChildren(walk, layoutNodeIndex(tree, root));
    return textTake(&walk.out);
}

bool prLayoutTextPositionFromPoint(PrLayoutTree* tree, PrLayoutNode root, PrLayoutVec point, PrLayoutTextPosition* out) {
    if (!prLayoutNodeIsValid(tree, root))
        return false;
    // The nearest piece so far: how far the point is from its line across the line, and from the piece along it.
    double nearestAcross = kInfinityF64;
    double nearestAlong = kInfinityF64;
    bool found = false;
    Array<PlacedText> pieces;
    Array<PrDMatrix3> stages;
    LayoutNodeIndex top = uint32_t(root);
    for (LayoutNodeIndex at = top; at != kPrLayoutNodeNone;) {
        pieces.clear();
        stages.clear();
        Mapping m{.transform = prDAffine2Identity(), .painted = true, .sticky = true, .stages = &stages};
        bool placed = textPieces(tree, at, m, [&](const PlacedText& piece) { pieces.push(piece); });
        PrDVec2 p{prLayoutUnitToPxDouble(point.x), prLayoutUnitToPxDouble(point.y)};
        bool reached = placed && pieces.count != 0;
        if (reached) {
            flush(m);
            for (uint32_t i = stages.count; i-- != 0 && reached;) {
                PrDMatrix3 inverse{};
                reached = prDMatrix3Invert(&stages.data[i], &inverse) && prDMatrix3MapPoint(&inverse, p, &p);
            }
        }
        for (uint32_t i = 0; reached && i < pieces.count; ++i) {
            const PlacedText& piece = pieces.data[i];
            const LayoutFragment* fragment = piece.fragment;
            bool vertical = layoutStyleTraits(tree->styles, layoutFragmentStyle(tree, fragment)).vertical;
            PrLayoutRect box = layoutRectAt(piece.offset, fragment->extent);
            auto outside = [](double v, PrLayoutUnit lo, PrLayoutUnit hi) {
                double low = prLayoutUnitToPxDouble(lo);
                double high = prLayoutUnitToPxDouble(hi);
                return v < low ? low - v : v > high ? v - high : 0.0;
            };
            double across;
            if (vertical)
                across = outside(p.x, piece.line.left, piece.line.right);
            else
                across = outside(p.y, piece.line.top, piece.line.bottom);
            double along = vertical ? outside(p.y, box.top, box.bottom) : outside(p.x, box.left, box.right);
            if (across > nearestAcross || (across == nearestAcross && along >= nearestAlong))
                continue;
            nearestAcross = across;
            nearestAlong = along;
            double into;
            if (!vertical)
                into = p.x - prLayoutUnitToPxDouble(box.left);
            else if (fragment->turn == LayoutTextTurn_CounterClockwise)
                into = prLayoutUnitToPxDouble(box.bottom) - p.y;
            else
                into = p.y - prLayoutUnitToPxDouble(box.top);
            *out = PrLayoutTextPosition{layoutNodeName(tree, at), layoutTextOffsetAt(tree, fragment, float(into))};
            found = true;
        }
        at = subtreeNext(tree, at, top);
    }
    pieces.reset();
    stages.reset();
    return found;
}

bool prLayoutTextOffsetNear(PrLayoutTree* tree, PrLayoutNode text, PrLayoutVec point, uint32_t* offset,
        PrLayoutTextAffinity* affinity) {
    PrLayoutTextPosition position{};
    if (!prLayoutTextPositionFromPoint(tree, text, point, &position))
        return false;
    *offset = position.offset;
    *affinity = PrLayoutTextAffinity_Downstream;
    PrRect down{};
    PrRect up{};
    if (caretPx(tree, text, position.offset, PrLayoutTextAffinity_Downstream, &down) &&
            caretPx(tree, text, position.offset, PrLayoutTextAffinity_Upstream, &up) && down.top != up.top) {
        float y = prLayoutUnitToPx(point.y);
        float toDown = (down.top + down.bottom) / 2 - y;
        float toUp = (up.top + up.bottom) / 2 - y;
        if (toUp * toUp < toDown * toDown)
            *affinity = PrLayoutTextAffinity_Upstream;
    }
    return true;
}

void prLayoutNodeLineInfo(const PrLayoutTree* tree, PrLayoutNode text, uint32_t offset, PrLayoutLineInfo* info) {
    *info = PrLayoutLineInfo{};
    if (!prLayoutNodeIsValid(tree, text))
        return;
    LayoutNodeIndex at = layoutTreeNode(tree, uint32_t(text)).parent;
    while (at != kPrLayoutNodeNone && layoutIsInlineBox(tree, at))
        at = layoutTreeNode(tree, at).parent;
    if (at == kPrLayoutNodeNone)
        return;
    PrLayoutStyleIndex style = layoutTreeNode(tree, at).style;
    const PrLayoutFontStyle& font = layoutFontStyle(tree->styles, style);
    info->direction = font.direction;
    info->writingMode = font.writingMode;
    if (layoutBoxStyle(tree->styles, style).unicodeBidi == PrTextBidi_Plaintext)
        info->direction = plaintextDirection(tree, PrLayoutTextPosition{text, offset}, font.direction);
}

uint32_t prLayoutLineStops(PrLayoutTree* tree, PrLayoutNode root, PrLayoutTextPosition position,
        PrLayoutTextAffinity affinity, PrLayoutLineStep step, PrLayoutLineInfo* PR_NULLABLE info,
        PrLayoutLineStop* PR_NULLABLE out, uint32_t capacity) {
    if (!prLayoutNodeIsValid(tree, root) || !prLayoutNodeIsValid(tree, position.node))
        return 0;
    if ((layoutTreeNode(tree, uint32_t(position.node)).dirty & LayoutDirtyFlag_Self) != 0)
        return 0;
    Array<LinePiece> pieces;
    Array<LineKey> lines;
    Array<PrLayoutLineStop> stops;
    uint32_t held = UINT32_MAX;
    uint32_t nearest = UINT32_MAX;
    LayoutNodeIndex wanted = uint32_t(position.node);
    LayoutNodeIndex top = uint32_t(root);
    for (LayoutNodeIndex at = top; at != kPrLayoutNodeNone; at = subtreeNext(tree, at, top)) {
        Mapping m{.transform = prDAffine2Identity()};
        bool firstOfNode = true;
        textPieces(tree, at, m, [&](const PlacedText& piece) {
            LineKey key{piece.lineFragment, mapped(m, piece.line), layoutCachePlacement(tree->cache, at).parent};
            auto same = [&](const LineKey& k) {
                return k.fragment == key.fragment && k.container == key.container && k.rect.left == key.rect.left &&
                    k.rect.top == key.rect.top;
            };
            // Only the first fragment of a node can join a line that is not the last, as text after an inline-block
            // does.
            uint32_t line = lines.count;
            if (lines.count != 0 && same(lines.data[lines.count - 1]))
                line = lines.count - 1;
            for (uint32_t i = lines.count; firstOfNode && line == lines.count && i-- != 0;) {
                if (same(lines.data[i]))
                    line = i;
            }
            firstOfNode = false;
            if (line == lines.count)
                lines.push(key);
            PrLayoutRect rect = mapped(m, layoutRectAt(piece.offset, piece.fragment->extent));
            pieces.push(LinePiece{piece.fragment, at, line, lineLeftAlong(tree, piece.fragment, rect)});
            if (at == wanted) {
                uint32_t distance = caretDistance(piece.fragment, position.offset, affinity);
                if (distance < nearest) {
                    nearest = distance;
                    held = line;
                }
            }
        });
    }
    uint32_t target = held;
    if (held != UINT32_MAX && step == PrLayoutLineStep_Next)
        target = held + 1 < lines.count ? held + 1 : UINT32_MAX;
    else if (held != UINT32_MAX && step == PrLayoutLineStep_Previous)
        target = held > 0 ? held - 1 : UINT32_MAX;
    if (target != UINT32_MAX) {
        Array<LinePiece> onLine;
        Array<LayoutNodeIndex> order;       // the nodes of the line, in content order
        for (uint32_t i = 0; i < pieces.count; ++i) {
            if (pieces.data[i].line != target)
                continue;
            onLine.push(pieces.data[i]);
            if (order.count == 0 || order.data[order.count - 1] != pieces.data[i].node)
                order.push(pieces.data[i].node);
        }
        // A line's children are in tree order, each at its visual place. A fragment with no extent comes first of those
        // that start at one place, so that the text it ends comes after it on a line that runs leftward.
        auto length = [&](const LinePiece& piece) { return layoutTextLength(tree, piece.fragment); };
        for (uint32_t i = 1; i < onLine.count; ++i) {
            LinePiece piece = onLine.data[i];
            uint32_t j = i;
            for (; j > 0; --j) {
                const LinePiece& before = onLine.data[j - 1];
                if (before.along < piece.along || (before.along == piece.along && length(before) <= length(piece)))
                    break;
                onLine.data[j] = before;
            }
            onLine.data[j] = piece;
        }
        for (uint32_t i = 0; i < onLine.count; ++i)
            appendLineStops(tree, onLine.data[i], stops);
        onLine.reset();
        // The first and last stops in the order of the text, each standing with the text of its line.
        uint64_t lowest = UINT64_MAX;
        uint64_t highest = 0;
        uint32_t startStop = 0;
        uint32_t endStop = 0;
        for (uint32_t i = 0; i < stops.count; ++i) {
            uint32_t rank = 0;
            while (rank + 1 < order.count && layoutNodeName(tree, order.data[rank]) != stops.data[i].position.node)
                ++rank;
            uint64_t key = (uint64_t(rank) << 32) | stops.data[i].position.offset;
            if (key < lowest || (key == lowest && stops.data[i].affinity == PrLayoutTextAffinity_Downstream)) {
                lowest = key;
                startStop = i;
            }
            if (key > highest || (key == highest && stops.data[i].affinity == PrLayoutTextAffinity_Upstream)) {
                highest = key;
                endStop = i;
            }
        }
        order.reset();
        if (info != nullptr) {
            PrLayoutStyleIndex style = layoutTreeNode(tree, lines.data[target].container).style;
            const PrLayoutFontStyle& font = layoutFontStyle(tree->styles, style);
            *info = PrLayoutLineInfo{
                .direction = font.direction,
                .writingMode = font.writingMode,
                .startStop = startStop,
                .endStop = endStop,
            };
            if (layoutBoxStyle(tree->styles, style).unicodeBidi == PrTextBidi_Plaintext && stops.count != 0)
                info->direction = plaintextDirection(tree, stops.data[0].position, font.direction);
        }
    }
    for (uint32_t i = 0; i < stops.count && i < capacity; ++i)
        out[i] = stops.data[i];
    uint32_t count = stops.count;
    pieces.reset();
    lines.reset();
    stops.reset();
    return count;
}

PrLayoutNode prLayoutNodeTextNext(const PrLayoutTree* tree, PrLayoutNode root, PrLayoutNode text, bool* contiguous) {
    return adjacentText(tree, root, text, contiguous, subtreeNext);
}

PrLayoutNode prLayoutNodeTextPrevious(const PrLayoutTree* tree, PrLayoutNode root, PrLayoutNode text, bool* contiguous) {
    return adjacentText(tree, root, text, contiguous, subtreePrevious);
}

PrLayoutNode prLayoutNodeRevealStep(PrLayoutTree* tree, PrLayoutNode from, PrLayoutRect* rect, PrLayoutVec* offset) {
    if (!prLayoutNodeIsValid(tree, from))
        return kPrLayoutNodeNone;
    for (PrLayoutNode at = prLayoutNodeParent(tree, from); at != kPrLayoutNodeNone; at = prLayoutNodeParent(tree, at)) {
        PrLayoutScrollInfo info{};
        if (!prLayoutNodeScrollInfo(tree, at, &info) || !info.scrollContainer)
            continue;
        // The scrollport in viewport coordinates: the container's border box less its borders.
        PrLayoutRect port = prLayoutNodeBounds(tree, at, PrLayoutCoordinates_Viewport);
        port.left.raw += prLayoutNodeBorderWidth(tree, at, PrRectSide_Left).raw;
        port.top.raw += prLayoutNodeBorderWidth(tree, at, PrRectSide_Top).raw;
        port.right.raw = port.left.raw + info.portExtent.x.raw;
        port.bottom.raw = port.top.raw + info.portExtent.y.raw;
        auto shift = [](int32_t lo, int32_t hi, int32_t portLo, int32_t portHi) -> int32_t {
            if (lo < portLo)
                return lo - portLo;
            if (hi > portHi)
                return hi - portHi < lo - portLo ? hi - portHi : lo - portLo;
            return 0;
        };
        int32_t dx = shift(rect->left.raw, rect->right.raw, port.left.raw, port.right.raw);
        int32_t dy = shift(rect->top.raw, rect->bottom.raw, port.top.raw, port.bottom.raw);
        if (dx == 0 && dy == 0)
            continue;
        auto clamp = [](int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; };
        *offset = PrLayoutVec{
            PrLayoutUnit{clamp(info.offset.x.raw + dx, info.range.left.raw, info.range.right.raw)},
            PrLayoutUnit{clamp(info.offset.y.raw + dy, info.range.top.raw, info.range.bottom.raw)},
        };
        // The rectangle moves with the content that the container scrolled, for the containers around it.
        int32_t movedX = offset->x.raw - info.offset.x.raw;
        int32_t movedY = offset->y.raw - info.offset.y.raw;
        rect->left.raw -= movedX;
        rect->right.raw -= movedX;
        rect->top.raw -= movedY;
        rect->bottom.raw -= movedY;
        return at;
    }
    return kPrLayoutNodeNone;
}

bool prLayoutNodeHoldsAbsolute(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutHoldsAbsolute(tree, layoutNodeIndex(tree, node));
}

bool prLayoutNodeHoldsFixed(const PrLayoutTree* tree, PrLayoutNode node) {
    return layoutHoldsFixed(tree, layoutNodeIndex(tree, node));
}

uint32_t prLayoutTreeHitTest(PrLayoutTree* tree, PrLayoutVec point, PrLayoutNode* PR_NULLABLE out, uint32_t capacity) {
    Array<uint32_t> hits;
    HitTester tester{};
    tester.tree = tree;
    tester.points.push(point);
    tester.out = &hits;
    layoutWalk(tree, tester);
    tester.points.reset();
    tester.outside.reset();
    uint32_t count = topmostOnce(tree, hits, out, capacity);
    hits.reset();
    return count;
}

uint32_t prLayoutNodeHitTest(PrLayoutTree* tree, PrLayoutNode root, PrLayoutVec point,
        PrLayoutNode* PR_NULLABLE out, uint32_t capacity) {
    LayoutNodeIndex index = layoutNodeIndex(tree, root);
    const LayoutFragment* fragment = nullptr;
    for (uint32_t i = 0; i < tree->roots.count && fragment == nullptr; ++i) {
        if (tree->roots.data[i].node == index)
            fragment = tree->roots.data[i].fragment;
    }
    if (fragment == nullptr || fragment->childCount == 0)
        return 0;
    Array<uint32_t> hits;
    HitTester tester{};
    tester.tree = tree;
    tester.points.push(point);
    tester.out = &hits;
    layoutWalkRoot(tree, fragment, tester);
    tester.points.reset();
    tester.outside.reset();
    uint32_t count = topmostOnce(tree, hits, out, capacity);
    hits.reset();
    return count;
}

bool prLayoutNodeScrollInfo(PrLayoutTree* tree, PrLayoutNode element, PrLayoutScrollInfo* out) {
    LayoutTree* t = tree;
    uint32_t index = layoutNodeIndex(t, element);
    const LayoutFragment* viewport = t->fragment;
    if (index == kPrLayoutNodeNone) {
        PrLayoutScrollInfo info{};
        info.portExtent = viewport != nullptr ? viewport->extent : t->viewport;
        info.content = viewport != nullptr ? viewport->overflow : layoutRectAt(kLayoutVecZero, t->viewport);
        info.range = layoutScrollRange(info.portExtent, info.content);
        info.offset = layoutScrollClamp(t->viewportScroll, info.range);
        PrLayoutBoxStyle overflow;
        if (t->root != kPrLayoutNodeNone)
            overflow = layoutBoxStyle(t->styles, layoutTreeNode(t, t->root).style);
        else
            overflow = prLayoutBoxStyleDefault();
        // A root that clips an axis makes that axis of the viewport hidden (css-overflow-3/#overflow-propagation).
        bool x = overflow.overflowX != PrLayoutOverflow_Hidden && overflow.overflowX != PrLayoutOverflow_Clip;
        bool y = overflow.overflowY != PrLayoutOverflow_Hidden && overflow.overflowY != PrLayoutOverflow_Clip;
        info.userAxes = PrLayoutScrollFlags((x ? PrLayoutScrollFlag_X : 0) | (y ? PrLayoutScrollFlag_Y : 0));
        PrLayoutBoxStyle scrolled = overflow;
        scrolled.overflowX = x ? PrLayoutOverflow_Scroll : PrLayoutOverflow_Hidden;
        scrolled.overflowY = y ? PrLayoutOverflow_Scroll : PrLayoutOverflow_Hidden;
        info.chainAxes = layoutChainAxes(scrolled);
        info.scrollContainer = true;
        *out = info;
        return true;
    }
    const LayoutTreeNode& n = layoutTreeNode(t, index);
    const LayoutFragment* fragment = n.kind != PrLayoutNodeKind_Text ? layoutCachePlaced(t->cache, index) : nullptr;
    if (fragment == nullptr)
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(t->styles, n.style);
    PrLayoutScrollInfo info{};
    PrLayoutUnit width = fragment->extent.x - box.border[PrRectSide_Left] - box.border[PrRectSide_Right];
    PrLayoutUnit height = fragment->extent.y - box.border[PrRectSide_Top] - box.border[PrRectSide_Bottom];
    info.portExtent = PrLayoutVec{layoutUnitMax(kLayoutUnitZero, width), layoutUnitMax(kLayoutUnitZero, height)};
    // A replaced element's content is not scrolled (css-overflow-3/#overflow-control).
    info.scrollContainer = n.parent != kPrLayoutNodeNone && layoutKindIsElement(n.kind) && layoutIsScrollContainer(box);
    if (info.scrollContainer) {
        info.content = layoutScrollContent(fragment, box);
        info.range = layoutScrollRange(info.portExtent, info.content);
        info.offset = layoutScrollClamp(n.scroll, info.range);
        info.userAxes = layoutUserAxes(box);
        info.chainAxes = layoutChainAxes(box);
    } else {
        info.content = layoutScrollingArea(t, fragment, box, layoutOwnMode(t, index));
    }
    *out = info;
    return true;
}

PrLayoutNode prLayoutNodeScrollContainer(const PrLayoutTree* tree, PrLayoutNode node) {
    const LayoutTree* t = tree;
    LayoutNodeIndex at = layoutNodeIndex(t, node);
    while (at != kPrLayoutNodeNone) {
        LayoutNodeIndex around = layoutTreeNode(t, at).parent;
        if (layoutIsOutOfFlow(t, at))
            layoutNodeHolder(t, at, &around);
        at = around;
        if (at == kPrLayoutNodeNone || layoutTreeNode(t, at).parent == kPrLayoutNodeNone)
            continue;
        const LayoutTreeNode& n = layoutTreeNode(t, at);
        if (layoutKindIsElement(n.kind) && layoutIsScrollContainer(layoutBoxStyle(t->styles, n.style)))
            return layoutNodeName(t, at);
    }
    return kPrLayoutNodeNone;
}
