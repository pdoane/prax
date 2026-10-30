// Geometry queries: the boxes of a tree's last layout, from where layout placed them, and hit testing.

#include "layout/src/layout_query.h"

#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/sort.h"

#include "layout/src/layout_cache.h"
#include "layout/src/layout_effects.h"
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
    return top == tree->root || layoutTreeIsLaidOutRoot(tree, top);
}

// The transform from a space to the coordinates that a query answers in, composed from the boxes that place it. The
// transform is affine until a box's transform with perspective joins it, and projective from then on.
struct Mapping {
    PrDAffine2 transform;
    PrDMatrix3 projective;          // used if `projected` is set
    bool projected;
    bool painted;                   // true for viewport coordinates, through transforms and scroll offsets

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

// Appends to `m` the mapping from the border box of the box `node` to the query's coordinates: its transform, its
// position in its parent's content, and onward from there. The root and the absolutely positioned boxes that the
// viewport holds are in the viewport's scrolled content, and the fixed boxes that it holds are in the viewport itself.
void fromBox(const LayoutTree* tree, LayoutNodeIndex node, Mapping& m) {
    const LayoutFragment* fragment = layoutCachePlaced(tree->cache, node);
    if (m.painted && fragment != nullptr && layoutFragmentPaintsWhole(tree, fragment)) {
        PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
        if (layoutStyleTraits(tree->styles, style).transforms)
            append(m, layoutBoxPlane(tree, fragment, kLayoutVecZero, nullptr));
    }
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    moveBy(m, placement.offset);
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
                fn(PlacedText{child.fragment, offset + child.offset, lineRect});
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
    fromContent(tree, placement.parent, m);
    if (n.kind == PrLayoutNodeKind_Text)
        textBoxes(node, container, placement.at, out);
    else
        inlineBoxes(node, container, placement.at, out);
}

// Appends the boxes of `node` in the last layout to `out`, in `coordinates` and in content order: where the last layout
// placed the node, mapped through the boxes that place it.
void boxesOf(const LayoutTree* tree, PrLayoutCoordinates coordinates, LayoutNodeIndex node, Array<PrLayoutRect>& out) {
    Mapping m{.transform = prDAffine2Identity(), .painted = coordinates == PrLayoutCoordinates_Viewport};
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
    fromContent(tree, placement.parent, m);
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
        Mapping m{.transform = prDAffine2Identity(), .painted = true};
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
    Mapping m{.transform = prDAffine2Identity(), .painted = true, .stages = &stages};
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
    Mapping m{.transform = prDAffine2Identity(), .painted = true, .stages = &stages};
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
    Mapping m{.transform = prDAffine2Identity(), .painted = coordinates == PrLayoutCoordinates_Viewport};
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
    Mapping m{.transform = prDAffine2Identity(), .painted = coordinates == PrLayoutCoordinates_Viewport};
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
        Mapping m{.transform = prDAffine2Identity(), .painted = true, .stages = &stages};
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
        // The next node of the subtree in preorder.
        const LayoutTreeNode& n = layoutTreeNode(tree, at);
        if (n.first != kPrLayoutNodeNone) {
            at = n.first;
            continue;
        }
        while (at != top && layoutTreeNode(tree, at).next == kPrLayoutNodeNone)
            at = layoutTreeNode(tree, at).parent;
        at = at == top ? kPrLayoutNodeNone : layoutTreeNode(tree, at).next;
    }
    pieces.reset();
    stages.reset();
    return found;
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
