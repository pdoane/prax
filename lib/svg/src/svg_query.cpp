// Queries of the SVG tree: hit testing, and where a node is drawn.

#include "svg/src/svg_query.h"

#include "pr/float.h"
#include "pr/path_measure.h"
#include "pr/path_polyline.h"
#include "pr/path_stroke.h"

#include "svg/src/svg_text.h"

namespace pr_impl {

namespace {

// How far a stroke's outline may lie from its curves when a point is tested against it, in the units that it is made
// in.
constexpr float kStrokeTolerance = 0.01f;

// Returns a drawer for a query, which records nothing. It owns a scratch path and a list, which queryDrawerEnd frees.
SvgDrawer queryDrawer(const SvgTree* tree, PrVec2 viewport, const SvgRootView* PR_NULLABLE view, SvgTiles* tiles) {
    SvgDrawer d{
        .tree = tree,
        .list = prDrawListCreate(),
        .viewport = viewport,
        .scratch = prPathCreate(),
        .space = prAffine2Identity(),
        .tiles = tiles,
    };
    d.host.view = view;
    return d;
}

void queryDrawerEnd(SvgDrawer& d) {
    prPathDestroy(d.scratch);
    prDrawListRelease(d.list);
    d.tiles->tiles.reset();
}

bool rectHolds(PrRect rect, PrVec2 point) {
    return point.x >= rect.left && point.x <= rect.right && point.y >= rect.top && point.y <= rect.bottom;
}

// Returns `point`, in the space outside `transform`, mapped into the space inside it. Returns false if the transform
// cannot be inverted, which collapses what it holds.
bool inside(PrAffine2 transform, PrVec2 point, PrVec2* out) {
    PrAffine2 inverse{};
    if (!prAffine2Invert(transform, &inverse))
        return false;
    *out = prAffine2MapPoint(inverse, point);
    return true;
}

// Returns true if a paint paints something: its actual value is not none (SVG2/interact.html#PointerEventsProp). A
// reference to no paint server paints its fallback, and context paint paints what the context element's does.
bool paints(const SvgDrawer& d, const SvgPaint& paint) {
    switch (paint.kind) {
        case SvgPaintKind_None: return false;
        case SvgPaintKind_Url: return paint.server != kSvgResourceNone || paint.fallback;
        case SvgPaintKind_ContextFill:
            return d.context != nullptr && paints(SvgDrawer{.tree = d.tree}, d.context->fill.paint);
        case SvgPaintKind_ContextStroke:
            return d.context != nullptr && paints(SvgDrawer{.tree = d.tree}, d.context->stroke.paint);
        default: return true;
    }
}

// Returns true if `point`, in a shape node's user space, is inside its stroke's shape.
bool strokeHolds(const SvgDrawer& d, const SvgNode& node, PrVec2 point) {
    float length = node.kind == SvgNodeKind_Line || node.paints.pathLength >= 0 ? svgShapeLength(d, node) : -1;
    float dashes[kSvgMaxDashes];
    PrDrawStroke stroke{};
    if (!svgStrokeShapeFor(d, node.paints, length, dashes, &stroke))
        return false;
    PrPath* path = prPathCreate();
    svgShapePath(d, node, path);
    // A non-scaling stroke's width is in the space that the tree is drawn in (SVG2/painting.html#NonScalingStroke).
    if (node.paints.nonScaling) {
        PrPath* mapped = prPathCreate();
        prPathAddPath(mapped, path, d.space);
        prPathDestroy(path);
        path = mapped;
        point = prAffine2MapPoint(d.space, point);
    }
    PrPathPolyline outline{};
    prPathStrokeOutline(path, &stroke.shape, kStrokeTolerance, &outline);
    PrPath* area = prPathCreate();
    prPathAddPolyline(area, &outline, prAffine2Identity());
    bool holds = prPathVerbCount(area) != 0 && prPathContains(area, point, PrPathFillRule_NonZero);
    prPathDestroy(area);
    prPathPolylineReset(&outline);
    prPathDestroy(path);
    return holds;
}

// Returns true if `point`, in a shape node's user space, is inside its interior by `rule`. A line has none.
bool fillHolds(const SvgDrawer& d, const SvgNode& node, PrVec2 point, PrPathFillRule rule) {
    if (node.kind == SvgNodeKind_Line)
        return false;
    PrPath* path = prPathCreate();
    svgShapePath(d, node, path);
    bool holds = prPathVerbCount(path) != 0 && prPathContains(path, point, rule);
    prPathDestroy(path);
    return holds;
}

// Returns true if pointer events over an element with `events` need it to be visible.
bool readsVisibility(SvgPointerEvents events) {
    return events == SvgPointerEvents_VisiblePainted || events == SvgPointerEvents_VisibleFill ||
        events == SvgPointerEvents_VisibleStroke || events == SvgPointerEvents_Visible;
}

// Returns true if pointer events over an element with `events` need what is under the pointer to be painted.
bool readsPaint(SvgPointerEvents events) {
    return events == SvgPointerEvents_VisiblePainted || events == SvgPointerEvents_Painted;
}

// Returns true if an element with `events` is hit over its interior.
bool readsFill(SvgPointerEvents events) {
    return events != SvgPointerEvents_VisibleStroke && events != SvgPointerEvents_Stroke;
}

// Returns true if a shape node is hit at `point`, in its user space (SVG2/interact.html#PointerEventsProp).
bool shapeHit(const SvgDrawer& d, const SvgNode& node, PrVec2 point) {
    SvgPointerEvents events = node.pointerEvents;
    if (events == SvgPointerEvents_None)
        return false;
    if (events == SvgPointerEvents_BoundingBox)
        return rectHolds(svgGeometryBounds(d, node), point);
    if (readsVisibility(events) && !node.visible)
        return false;
    bool painted = readsPaint(events);
    if (readsFill(events) && (!painted || paints(d, node.paints.fill)) && fillHolds(d, node, point, node.fillRule))
        return true;
    bool stroke = painted || svgPointerReadsStroke(events);
    return stroke && (!painted || paints(d, node.paints.stroke)) && strokeHolds(d, node, point);
}

// Returns true if a node that is hit over its whole rectangle, an image or a foreignObject, is hit at `point`.
bool rectangleHit(const SvgNode& node, PrRect rect, PrVec2 point) {
    SvgPointerEvents events = node.pointerEvents;
    if (events == SvgPointerEvents_None || (readsVisibility(events) && !node.visible))
        return false;
    return rectHolds(rect, point);
}

// Returns the index of the character of `text` whose cell holds `point`, in the text's user space, or kArrayMissing.
uint32_t characterAt(const SvgText* text, PrVec2 point) {
    const SvgTextCharacter* characters = svgTextCharacters(text);
    uint32_t count = svgTextCharacterCount(text);
    for (uint32_t i = 0; i < count; ++i) {
        const SvgTextCharacter& c = characters[i];
        if (c.hidden)
            continue;
        float radians = c.direction * (kPrF32Pi / 180);
        PrVec2 along{prF32Cos(radians), prF32Sin(radians)};
        PrVec2 across{-along.y, along.x};
        float x = point.x - c.position.x;
        float y = point.y - c.position.y;
        float a = x * along.x + y * along.y;
        float b = x * across.x + y * across.y;
        if (a >= 0 && a <= c.advance && b >= -c.ascent && b <= c.descent)
            return i;
    }
    return kArrayMissing;
}

// The state of a hit test.
struct Hitter {
    SvgDrawer d;
    PrVec2 point;                      // in the current user space
    Array<CssPresentationSvgHit> hits; // in painting order
};

void hitNode(Hitter& h, SvgNodeIndex index);

// Returns true if the node's clip path holds the point, in the user space that its effects apply in, or if it has none
// (css-masking-1/#clipping-paths).
bool clipHolds(SvgDrawer& d, SvgNodeIndex index, PrVec2 point) {
    const SvgNode& node = d.tree->nodes.data[index];
    if (node.effects == kArrayMissing)
        return true;
    const SvgEffects& effects = d.tree->effects.data[node.effects];
    if (effects.clip == SvgClipKind_None)
        return true;
    if (effects.clip == SvgClipKind_Url) {
        if (svgEffectCycles(d, effects.clipPath))
            return true;
        SvgDrawer inner = svgEffectDrawer(d, effects.clipPath, d.list);
        return svgClipContains(inner.tree, effects.clipPath, svgEffectBounds(d, index, 0), d.viewport, point);
    }
    PrPath* path = prPathCreate();
    PrPathFillRule rule = PrPathFillRule_NonZero;
    svgClipShapePath(d, index, effects, path, &rule);
    bool holds = prPathVerbCount(path) != 0 && prPathContains(path, point, rule);
    prPathDestroy(path);
    return holds;
}

// Adds a hit of `element`.
void addHit(Hitter& h, const PrDomElement* element) {
    h.hits.push(CssPresentationSvgHit{.element = element, .root = kPrLayoutNodeNone});
}

void hitChildren(Hitter& h, const SvgNode& node) {
    for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = h.d.tree->nodes.data[child].next)
        hitNode(h, child);
}

// Hit tests a text node's characters, at the point in its user space. The element hit is the innermost whose characters
// include the one under the point, and that element's pointer-events, visibility and paints decide whether the text is
// hit there.
void hitText(Hitter& h, const SvgNode& node) {
    uint32_t character = characterAt(node.text, h.point);
    if (character == kArrayMissing)
        return;
    // The text element comes first, then its descendants in tree order, so the innermost is the last that holds it.
    const SvgTextElement* elements = svgTextElements(node.text);
    const SvgTextElement* found = nullptr;
    for (uint32_t i = 0; i < svgTextElementCount(node.text); ++i) {
        if (character >= elements[i].start && character < elements[i].end)
            found = &elements[i];
    }
    if (found == nullptr)
        return;
    SvgPointerEvents events = found->pointerEvents;
    if (events == SvgPointerEvents_None || (readsVisibility(events) && !found->visible) ||
            (readsPaint(events) && !found->painted))
        return;
    addHit(h, found->element);
}

// Hit tests a node's own content, at the point in its user space.
void hitBody(Hitter& h, SvgNodeIndex index) {
    const SvgNode& node = h.d.tree->nodes.data[index];
    uint32_t before = h.hits.count;
    switch (node.kind) {
        case SvgNodeKind_Group: {
            if (!node.context) {
                hitChildren(h, node);
                break;
            }
            // A use element's paints are the context paint of its shadow tree, which decides what it paints.
            SvgContext context{.fill = {.paint = node.paints.fill}, .stroke = {.paint = node.paints.stroke}};
            const SvgContext* outer = h.d.context;
            h.d.context = &context;
            hitChildren(h, node);
            h.d.context = outer;
            break;
        }
        case SvgNodeKind_Viewport: hitChildren(h, node); break;
        case SvgNodeKind_Text: hitText(h, node); break;
        case SvgNodeKind_Image:
            if (rectangleHit(node, svgImageRect(h.d.viewport, node.image), h.point))
                addHit(h, node.element);
            break;
        case SvgNodeKind_ForeignObject: {
            PrRect rect = svgGeometryBounds(h.d, node);
            if (!(prRectWidth(rect) > 0) || !(prRectHeight(rect) > 0) || !rectangleHit(node, rect, h.point))
                break;
            h.hits.push(CssPresentationSvgHit{
                .element = node.element,
                .root = node.foreign.root,
                .rootPoint = PrVec2{h.point.x - rect.left, h.point.y - rect.top},
            });
            break;
        }
        case SvgNodeKind_Free: break;
        default:
            if (shapeHit(h.d, node, h.point))
                addHit(h, node.element);
            break;
    }
    // A container whose pointer-events is bounding-box is hit over its bounding box where none of its content is.
    bool container = node.kind == SvgNodeKind_Group || node.kind == SvgNodeKind_Viewport;
    if (container && node.pointerEvents == SvgPointerEvents_BoundingBox && index != 0 && h.hits.count == before &&
            rectHolds(svgEffectBounds(h.d, index, 0), h.point))
        addHit(h, node.element);
}

void hitNode(Hitter& h, SvgNodeIndex index) {
    SvgDrawer& d = h.d;
    const SvgNode& node = d.tree->nodes.data[index];
    PrVec2 saved = h.point;
    PrAffine2 savedSpace = d.space;
    PrVec2 savedViewport = d.viewport;
    PrAffine2 transform = svgNodeTransform(d, node);
    bool reached = inside(transform, h.point, &h.point);
    d.space = prAffine2Concat(transform, d.space);
    if (reached && node.kind == SvgNodeKind_Viewport) {
        SvgViewportShape v = svgViewportFromNode(d, index);
        PrRect area{};
        PrAffine2 place{};
        reached = svgPlaceViewport(d, v, &area, &place) && (!v.clips || rectHolds(area, h.point)) &&
            inside(place, h.point, &h.point);
        if (reached) {
            d.space = prAffine2Concat(place, d.space);
            d.viewport = svgViewportContentExtent(v, area);
        }
    }
    if (reached && clipHolds(d, index, h.point))
        hitBody(h, index);
    h.point = saved;
    d.space = savedSpace;
    d.viewport = savedViewport;
}

// Returns true if the silhouette of the node `index` of a clipPath's content holds `point`, in the node's parent's user
// space: the geometry of a shape or the cells of a text by its clip-rule, or a group's children's, each clipped by its
// own clip path. A node that is not visible adds nothing.
bool silhouetteHolds(SvgDrawer& d, SvgNodeIndex index, PrVec2 point) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrVec2 local{};
    if (!inside(svgNodeTransform(d, node), point, &local) || !clipHolds(d, index, local))
        return false;
    switch (node.kind) {
        case SvgNodeKind_Group:
            for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = d.tree->nodes.data[child].next) {
                if (silhouetteHolds(d, child, local))
                    return true;
            }
            return false;
        case SvgNodeKind_Text: return node.visible && characterAt(node.text, local) != kArrayMissing;
        case SvgNodeKind_Rect:
        case SvgNodeKind_Ellipse:
        case SvgNodeKind_Path: return node.visible && fillHolds(d, node, local, node.clipRule);
        default: return false;
    }
}

// Writes the nodes from the root to `index` to `out`, the root first.
void pathTo(const SvgTree* tree, SvgNodeIndex index, Array<SvgNodeIndex>* out) {
    for (SvgNodeIndex at = index; at != kSvgNodeNone; at = tree->nodes.data[at].parent)
        out->push(at);
    for (uint32_t i = 0; i < out->count / 2; ++i) {
        SvgNodeIndex swapped = out->data[i];
        out->data[i] = out->data[out->count - 1 - i];
        out->data[out->count - 1 - i] = swapped;
    }
}

} // namespace

uint32_t svgTreeHitTest(const SvgTree* tree, PrVec2 viewport, const SvgRootView* PR_NULLABLE view, PrVec2 point,
        CssPresentationSvgHit* PR_NULLABLE out, uint32_t capacity) {
    if (tree->nodes.count == 0)
        return 0;
    SvgTiles tiles{};
    Hitter h{.d = queryDrawer(tree, viewport, view, &tiles), .point = point};
    hitNode(h, 0);
    queryDrawerEnd(h.d);
    uint32_t count = h.hits.count;
    for (uint32_t i = 0; i < count && i < capacity; ++i)
        out[i] = h.hits.data[count - 1 - i];
    h.hits.reset();
    return count;
}

bool svgClipContains(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrVec2 point) {
    const SvgClipPath& clip = tree->resources.data[index].clipPath;
    // A clip path in objectBoundingBox units on a box with no width or height clips everything away.
    if (clip.content == kSvgNodeNone || (clip.boundingBox && !(prRectWidth(box) > 0 && prRectHeight(box) > 0)))
        return false;
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, viewport, nullptr, &tiles);
    PrAffine2 space;
    if (clip.boundingBox)
        space = prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(box), prRectHeight(box)}),
            prAffine2Translate(PrVec2{box.left, box.top}));
    else
        space = prAffine2Identity();
    space = prAffine2Concat(clip.transform, space);
    SvgDrawer inner = svgEffectDrawer(d, index, d.list);
    PrVec2 local{};
    bool holds = inside(space, point, &local) && silhouetteHolds(inner, clip.content, local);
    queryDrawerEnd(d);
    return holds;
}

bool svgNodePlace(const SvgTree* tree, PrVec2 viewport, SvgNodeIndex index, SvgPlace* out) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, viewport, nullptr, &tiles);
    Array<SvgNodeIndex> path;
    pathTo(tree, index, &path);
    bool drawn = path.count != 0 && path.data[0] == 0;
    // The transform of each node on the path from its user space to its parent's, and the nearest viewport around the
    // node.
    Array<PrAffine2> locals;
    uint32_t nearest = 0;
    for (uint32_t i = 0; drawn && i < path.count; ++i) {
        const SvgNode& node = tree->nodes.data[path.data[i]];
        if (i + 1 == path.count)
            out->viewport = d.viewport;
        PrAffine2 local = svgNodeTransform(d, node);
        if (node.kind == SvgNodeKind_Viewport) {
            SvgViewportShape v = svgViewportFromNode(d, path.data[i]);
            PrRect area{};
            PrAffine2 place{};
            drawn = svgPlaceViewport(d, v, &area, &place);
            local = prAffine2Concat(place, local);
            d.viewport = svgViewportContentExtent(v, area);
            if (i + 1 < path.count)
                nearest = i;
        }
        locals.push(local);
        d.space = prAffine2Concat(local, d.space);
    }
    if (drawn) {
        // The CTM maps up to the coordinates that the nearest viewport is placed in, through that viewport's own
        // placement. The root's is its own placement.
        PrAffine2 ctm = prAffine2Identity();
        uint32_t last = path.count - 1;
        uint32_t first = last == 0 ? 0 : nearest;
        for (uint32_t i = last + 1; i-- != first;)
            ctm = prAffine2Concat(ctm, locals.data[i]);
        out->toTree = d.space;
        out->ctm = ctm;
    }
    locals.reset();
    path.reset();
    queryDrawerEnd(d);
    return drawn;
}

PrRect svgNodeBox(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrSvgBoundingBoxOptions options) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    d.space = place.toTree;
    PrRect box{};
    SvgBoundsFlags parts = SvgBoundsFlags((options.stroke ? SvgBoundsFlag_Stroke : 0) |
        (options.markers ? SvgBoundsFlag_Markers : 0));
    if (options.fill || parts != 0)
        box = svgEffectBounds(d, index, parts);
    const SvgNode& node = tree->nodes.data[index];
    const SvgEffects* effects = node.effects != kArrayMissing ? &tree->effects.data[node.effects] : nullptr;
    if (options.clipped && effects != nullptr && effects->clip != SvgClipKind_None) {
        PrRect clip{};
        if (effects->clip == SvgClipKind_Url) {
            const SvgClipPath& clipPath = tree->resources.data[effects->clipPath].clipPath;
            PrRect bounds = svgEffectBounds(d, index, 0);
            PrAffine2 space;
            if (clipPath.boundingBox)
                space = prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(bounds), prRectHeight(bounds)}),
                    prAffine2Translate(PrVec2{bounds.left, bounds.top}));
            else
                space = prAffine2Identity();
            if (clipPath.content != kSvgNodeNone)
                clip = prAffine2MapRect(prAffine2Concat(clipPath.transform, space), svgNodeBounds(d, clipPath.content, 0));
        } else {
            PrPath* path = prPathCreate();
            PrPathFillRule rule = PrPathFillRule_NonZero;
            svgClipShapePath(d, index, *effects, path, &rule);
            clip = prPathTightBounds(path);
            prPathDestroy(path);
        }
        PrRect cut = prRectIntersect(box, clip);
        box = prRectWidth(cut) > 0 || prRectHeight(cut) > 0 ? cut : PrRect{};
    }
    queryDrawerEnd(d);
    return box;
}

bool svgShapeFillContains(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrVec2 point) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    const SvgNode& node = tree->nodes.data[index];
    bool holds = fillHolds(d, node, point, node.fillRule);
    queryDrawerEnd(d);
    return holds;
}

bool svgShapeStrokeContains(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrVec2 point) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    d.space = place.toTree;
    bool holds = strokeHolds(d, tree->nodes.data[index], point);
    queryDrawerEnd(d);
    return holds;
}

float svgShapeTotalLength(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    float length = svgShapeLength(d, tree->nodes.data[index]);
    queryDrawerEnd(d);
    return length;
}

PrVec2 svgShapePointAtLength(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, float distance) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    PrPath* path = prPathCreate();
    svgShapePath(d, tree->nodes.data[index], path);
    PrPathMeasure* measure = prPathMeasureCreate();
    prPathMeasureSetPath(measure, path, prAffine2Identity(), kStrokeTolerance);
    PrVec2 point;
    if (prPathMeasureLength(measure) > 0)
        point = prPathMeasurePosition(measure, distance).point;
    else if (prPathVerbCount(path) != 0)
        point = prPathPoints(path)[0];
    else
        point = PrVec2{0, 0};
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
    queryDrawerEnd(d);
    return point;
}

} // namespace pr_impl
