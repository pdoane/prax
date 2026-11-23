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
// Characters are painted in order, so where cells overlap, the last that holds the point is the one on top.
uint32_t characterAt(const SvgText* text, PrVec2 point) {
    const SvgTextCharacter* characters = svgTextCharacters(text);
    uint32_t count = svgTextCharacterCount(text);
    for (uint32_t i = count; i-- > 0;) {
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
    PrVec2 point;                         // in the current user space
    RawArray<CssPresentationSvgHit> hits; // in painting order
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
    // A container or a text whose pointer-events is bounding-box is hit over its bounding box where none of its content
    // or characters are.
    bool container =
        node.kind == SvgNodeKind_Group || node.kind == SvgNodeKind_Viewport || node.kind == SvgNodeKind_Text;
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

// Returns `box`, in the user space of the node `index`, cut to the bounds of the node's clip, or `box` if the node has
// no clip. The bounds of a clipPath are those of its children, each cut by its own clip, and cut by the clipPath's own
// clip-path (css-masking-1/#ClipPathElement).
PrRect clipCut(SvgDrawer& d, SvgNodeIndex index, PrRect box);

// Returns the bounds of the node `index` with what `parts` adds, in its user space, cut by its clip
// (SVG2/coords.html#BoundingBoxes). A group's bounds are its children's, each cut by its own clip. So are the bounds of
// a use element, whose clip is on a node of its own inside its x and y.
PrRect clippedBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts) {
    const SvgNode& node = d.tree->nodes.data[index];
    if (node.kind != SvgNodeKind_Group)
        return clipCut(d, index, svgEffectBounds(d, index, parts));
    PrRect box{};
    bool any = false;
    for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = d.tree->nodes.data[child].next) {
        PrRect bounds = clippedBounds(d, child, parts);
        if (prRectWidth(bounds) <= 0 && prRectHeight(bounds) <= 0)
            continue;
        bounds = prAffine2MapRect(svgNodeTransform(d, d.tree->nodes.data[child]), bounds);
        box = any ? prRectUnion(box, bounds) : bounds;
        any = true;
    }
    return clipCut(d, index, box);
}

PrRect clipCut(SvgDrawer& d, SvgNodeIndex index, PrRect box) {
    const SvgNode& node = d.tree->nodes.data[index];
    const SvgEffects* effects = node.effects != kArrayMissing ? &d.tree->effects.data[node.effects] : nullptr;
    if (effects == nullptr || effects->clip == SvgClipKind_None)
        return box;
    PrRect clip{};
    if (effects->clip == SvgClipKind_Url) {
        const SvgClipPath& clipPath = d.tree->resources.data[effects->clipPath].clipPath;
        PrRect bounds = svgEffectBounds(d, index, 0);
        PrAffine2 space;
        if (clipPath.boundingBox)
            space = prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(bounds), prRectHeight(bounds)}),
                prAffine2Translate(PrVec2{bounds.left, bounds.top}));
        else
            space = prAffine2Identity();
        if (clipPath.content != kSvgNodeNone)
            clip = prAffine2MapRect(prAffine2Concat(svgOriginTransformMatrix(clipPath.transform, d.viewport), space),
                clippedBounds(d, clipPath.content, 0));
    } else {
        PrPath* path = prPathCreate();
        PrPathFillRule rule = PrPathFillRule_NonZero;
        svgClipShapePath(d, index, *effects, path, &rule);
        clip = prPathTightBounds(path);
        prPathDestroy(path);
    }
    PrRect cut = prRectIntersect(box, clip);
    return prRectWidth(cut) > 0 || prRectHeight(cut) > 0 ? cut : PrRect{};
}

// Areas over which nodes are hit, in the space that the tree is drawn in, as polylines filled by the nonzero rule.

// Appends `rect`, mapped through `transform`, to `out` as a closed contour.
void addRect(PrPathPolyline* out, PrRect rect, PrAffine2 transform) {
    prPathPolylineMoveTo(out, prAffine2MapPoint(transform, PrVec2{rect.left, rect.top}));
    prPathPolylineLineTo(out, prAffine2MapPoint(transform, PrVec2{rect.right, rect.top}));
    prPathPolylineLineTo(out, prAffine2MapPoint(transform, PrVec2{rect.right, rect.bottom}));
    prPathPolylineLineTo(out, prAffine2MapPoint(transform, PrVec2{rect.left, rect.bottom}));
    prPathPolylineClose(out);
}

// Appends the area that `path` fills by `rule`, mapped through `transform`, to `out`. The area is resolved into
// contours that each wind once, so that the nonzero rule fills it as `rule` does.
void addFill(PrPathPolyline* out, const PrPath* path, PrAffine2 transform, PrPathFillRule rule) {
    PrPathPolyline flat{};
    PrPathPolyline none{};
    prPathFlatten(path, transform, kStrokeTolerance, PrPathFlattening_Inside, &flat);
    prPathPolylineCombine(&flat, rule, &none, PrPathFillRule_NonZero, PrPathPolylineOp_Union, out);
    prPathPolylineReset(&flat);
}

// Cuts `area` to the part that `clip` also covers.
void cutArea(PrPathPolyline* area, const PrPathPolyline& clip) {
    PrPathPolyline kept{};
    prPathPolylineCombine(area, PrPathFillRule_NonZero, &clip, PrPathFillRule_NonZero, PrPathPolylineOp_Intersect, &kept);
    prPathPolylineReset(area);
    *area = kept;
}

// Appends the area of a shape node's stroke, whose user space `toTree` maps into the space that the tree is drawn in,
// to `out`.
void addStroke(const SvgDrawer& d, const SvgNode& node, PrAffine2 toTree, PrPathPolyline* out) {
    float length = node.kind == SvgNodeKind_Line || node.paints.pathLength >= 0 ? svgShapeLength(d, node) : -1;
    float dashes[kSvgMaxDashes];
    PrDrawStroke stroke{};
    if (!svgStrokeShapeFor(d, node.paints, length, dashes, &stroke))
        return;
    PrPath* path = prPathCreate();
    svgShapePath(d, node, path);
    PrPathPolyline outline{};
    // A non-scaling stroke's width is in the space that the tree is drawn in (SVG2/painting.html#NonScalingStroke).
    if (node.paints.nonScaling) {
        PrPath* mapped = prPathCreate();
        prPathAddPath(mapped, path, toTree);
        prPathStrokeOutline(mapped, &stroke.shape, kStrokeTolerance, &outline);
        prPathPolylineAddPolyline(out, &outline, prAffine2Identity());
        prPathDestroy(mapped);
    } else {
        prPathStrokeOutline(path, &stroke.shape, kStrokeTolerance, &outline);
        prPathPolylineAddPolyline(out, &outline, toTree);
    }
    prPathPolylineReset(&outline);
    prPathDestroy(path);
}

// Appends the cell of each character of a text node that `inside` accepts, mapped through `toTree`, to `out`. A hidden
// character has no cell.
template <typename Fn>
void addCells(const SvgText* text, PrAffine2 toTree, PrPathPolyline* out, Fn inside) {
    const SvgTextCharacter* characters = svgTextCharacters(text);
    for (uint32_t i = 0; i < svgTextCharacterCount(text); ++i) {
        const SvgTextCharacter& c = characters[i];
        if (c.hidden || !inside(i))
            continue;
        float radians = c.direction * (kPrF32Pi / 180);
        PrVec2 along{prF32Cos(radians), prF32Sin(radians)};
        PrVec2 across{-along.y, along.x};
        PrVec2 top{c.position.x - across.x * c.ascent, c.position.y - across.y * c.ascent};
        PrVec2 bottom{c.position.x + across.x * c.descent, c.position.y + across.y * c.descent};
        PrVec2 step{along.x * c.advance, along.y * c.advance};
        prPathPolylineMoveTo(out, prAffine2MapPoint(toTree, top));
        prPathPolylineLineTo(out, prAffine2MapPoint(toTree, PrVec2{top.x + step.x, top.y + step.y}));
        prPathPolylineLineTo(out, prAffine2MapPoint(toTree, PrVec2{bottom.x + step.x, bottom.y + step.y}));
        prPathPolylineLineTo(out, prAffine2MapPoint(toTree, bottom));
        prPathPolylineClose(out);
    }
}

// Appends the area over which the characters of `element` in a text node are hit, as hitText decides for each, to
// `out`.
void addTextArea(const SvgNode& node, const PrDomElement* element, PrAffine2 toTree, PrPathPolyline* out) {
    const SvgTextElement* elements = svgTextElements(node.text);
    uint32_t count = svgTextElementCount(node.text);
    const SvgTextElement* own = nullptr;
    for (uint32_t i = 0; i < count && own == nullptr; ++i) {
        if (elements[i].element == element)
            own = &elements[i];
    }
    if (own == nullptr)
        return;
    addCells(node.text, toTree, out, [&](uint32_t character) {
        if (character < own->start || character >= own->end)
            return false;
        // The innermost element whose characters include this one decides whether it is hit.
        const SvgTextElement* found = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            if (character >= elements[i].start && character < elements[i].end)
                found = &elements[i];
        }
        SvgPointerEvents events = found->pointerEvents;
        return events != SvgPointerEvents_None && (!readsVisibility(events) || found->visible) &&
            (!readsPaint(events) || found->painted);
    });
}

// Appends the area over which a shape node is hit, as shapeHit decides at each point, to `out`.
void addShapeArea(const SvgDrawer& d, const SvgNode& node, PrAffine2 toTree, PrPathPolyline* out) {
    SvgPointerEvents events = node.pointerEvents;
    if (events == SvgPointerEvents_None)
        return;
    if (events == SvgPointerEvents_BoundingBox) {
        addRect(out, svgGeometryBounds(d, node), toTree);
        return;
    }
    if (readsVisibility(events) && !node.visible)
        return;
    bool painted = readsPaint(events);
    if (node.kind != SvgNodeKind_Line && readsFill(events) && (!painted || paints(d, node.paints.fill))) {
        PrPath* path = prPathCreate();
        svgShapePath(d, node, path);
        addFill(out, path, toTree, node.fillRule);
        prPathDestroy(path);
    }
    bool stroke = painted || svgPointerReadsStroke(events);
    if (stroke && (!painted || paints(d, node.paints.stroke)))
        addStroke(d, node, toTree, out);
}

void addSilhouette(SvgDrawer& d, SvgNodeIndex index, PrAffine2 parentToTree, PrPathPolyline* out);

// Appends the area of the clip of the node `index`, whose user space `toTree` maps into the space that the tree is
// drawn in, to `out`: the silhouette of its clipPath, or its basic shape (css-masking-1/#clipping-paths). Returns false
// if the node has no clip, or if its clipPath references itself, which clips nothing. An empty area clips everything.
bool addClip(SvgDrawer& d, SvgNodeIndex index, PrAffine2 toTree, PrPathPolyline* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    if (node.effects == kArrayMissing)
        return false;
    const SvgEffects& effects = d.tree->effects.data[node.effects];
    if (effects.clip == SvgClipKind_None)
        return false;
    if (effects.clip == SvgClipKind_Url) {
        if (svgEffectCycles(d, effects.clipPath))
            return false;
        SvgDrawer inner = svgEffectDrawer(d, effects.clipPath, d.list);
        const SvgClipPath& clip = inner.tree->resources.data[effects.clipPath].clipPath;
        PrRect box = svgEffectBounds(d, index, 0);
        // A clip path in objectBoundingBox units on a box with no width or height clips everything away.
        if (clip.content == kSvgNodeNone || (clip.boundingBox && !(prRectWidth(box) > 0 && prRectHeight(box) > 0)))
            return true;
        PrAffine2 space = prAffine2Identity();
        if (clip.boundingBox)
            space = prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(box), prRectHeight(box)}),
                prAffine2Translate(PrVec2{box.left, box.top}));
        space = prAffine2Concat(svgOriginTransformMatrix(clip.transform, d.viewport), space);
        addSilhouette(inner, clip.content, prAffine2Concat(space, toTree), out);
        return true;
    }
    PrPath* path = prPathCreate();
    PrPathFillRule rule = PrPathFillRule_NonZero;
    svgClipShapePath(d, index, effects, path, &rule);
    if (prPathVerbCount(path) != 0)
        addFill(out, path, toTree, rule);
    prPathDestroy(path);
    return true;
}

// Appends the silhouette of the node `index` of a clipPath's content, whose parent's user space `parentToTree` maps
// into the space that the tree is drawn in, to `out`, as silhouetteHolds decides at each point.
void addSilhouette(SvgDrawer& d, SvgNodeIndex index, PrAffine2 parentToTree, PrPathPolyline* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrAffine2 toTree = prAffine2Concat(svgNodeTransform(d, node), parentToTree);
    PrPathPolyline area{};
    switch (node.kind) {
        case SvgNodeKind_Group:
            for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = d.tree->nodes.data[child].next)
                addSilhouette(d, child, toTree, &area);
            break;
        case SvgNodeKind_Text:
            if (node.visible)
                addCells(node.text, toTree, &area, [](uint32_t) { return true; });
            break;
        case SvgNodeKind_Rect:
        case SvgNodeKind_Ellipse:
        case SvgNodeKind_Path:
            if (node.visible) {
                PrPath* path = prPathCreate();
                svgShapePath(d, node, path);
                addFill(&area, path, toTree, node.clipRule);
                prPathDestroy(path);
            }
            break;
        default: break;
    }
    PrPathPolyline clip{};
    if (addClip(d, index, toTree, &clip))
        cutArea(&area, clip);
    prPathPolylineAddPolyline(out, &area, prAffine2Identity());
    prPathPolylineReset(&clip);
    prPathPolylineReset(&area);
}

// Appends the area over which the node `index` itself is hit, whose user space `toTree` maps into the space that the
// tree is drawn in, to `out`, without its clip. `element` is the text content element that a text node's area is of.
void addOwnArea(SvgDrawer& d, SvgNodeIndex index, const PrDomElement* element, PrAffine2 toTree, PrPathPolyline* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    switch (node.kind) {
        case SvgNodeKind_Group:
        case SvgNodeKind_Viewport:
        case SvgNodeKind_Free: break;
        case SvgNodeKind_Text:
            // A text whose pointer-events is bounding-box is hit over its whole bounding box.
            if (node.pointerEvents == SvgPointerEvents_BoundingBox)
                addRect(out, svgEffectBounds(d, index, 0), toTree);
            else
                addTextArea(node, element, toTree, out);
            break;
        case SvgNodeKind_Image:
        case SvgNodeKind_ForeignObject: {
            SvgPointerEvents events = node.pointerEvents;
            if (events == SvgPointerEvents_None || (readsVisibility(events) && !node.visible))
                break;
            PrRect rect = node.kind == SvgNodeKind_Image ? svgImageRect(d.viewport, node.image) : svgGeometryBounds(d, node);
            if (prRectWidth(rect) > 0 && prRectHeight(rect) > 0)
                addRect(out, rect, toTree);
            break;
        }
        default: addShapeArea(d, node, toTree, out); break;
    }
}

// Writes the nodes from the root to `index` to `out`, the root first.
void pathTo(const SvgTree* tree, SvgNodeIndex index, RawArray<SvgNodeIndex>* out) {
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

void svgNodeHitArea(const SvgTree* tree, PrVec2 viewport, SvgNodeIndex index, const PrDomElement* element,
        PrPathPolyline* out) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, viewport, nullptr, &tiles);
    RawArray<SvgNodeIndex> path;
    pathTo(tree, index, &path);
    bool reached = path.count != 0 && path.data[0] == 0;
    // The part of the tree's space that the viewports and clip paths around the node leave, valid once `clipped` is
    // set.
    PrPathPolyline clip{};
    bool clipped = false;
    auto cutBy = [&](PrPathPolyline* area) {
        if (clipped) {
            cutArea(&clip, *area);
        } else {
            prPathPolylineAddPolyline(&clip, area, prAffine2Identity());
            clipped = true;
        }
        prPathPolylineReset(area);
    };
    // A use element's paints are the context paint of its shadow tree, which decides what it paints.
    SvgContext context{};
    for (uint32_t i = 0; reached && i < path.count; ++i) {
        const SvgNode& node = tree->nodes.data[path.data[i]];
        PrAffine2 local = svgNodeTransform(d, node);
        if (node.kind == SvgNodeKind_Viewport) {
            SvgViewportShape v = svgViewportFromNode(d, path.data[i]);
            PrRect area{};
            PrAffine2 place{};
            reached = svgPlaceViewport(d, v, &area, &place);
            if (reached && v.clips) {
                PrPathPolyline box{};
                addRect(&box, area, prAffine2Concat(local, d.space));
                cutBy(&box);
            }
            local = prAffine2Concat(place, local);
            d.viewport = svgViewportContentExtent(v, area);
        }
        d.space = prAffine2Concat(local, d.space);
        PrPathPolyline own{};
        if (reached && addClip(d, path.data[i], d.space, &own))
            cutBy(&own);
        if (node.kind == SvgNodeKind_Group && node.context) {
            context = SvgContext{.fill = {.paint = node.paints.fill}, .stroke = {.paint = node.paints.stroke}};
            d.context = &context;
        }
    }
    if (reached) {
        PrPathPolyline area{};
        addOwnArea(d, index, element, d.space, &area);
        if (clipped)
            cutArea(&area, clip);
        prPathPolylineAddPolyline(out, &area, prAffine2Identity());
        prPathPolylineReset(&area);
    }
    prPathPolylineReset(&clip);
    path.reset();
    queryDrawerEnd(d);
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
    space = prAffine2Concat(svgOriginTransformMatrix(clip.transform, viewport), space);
    SvgDrawer inner = svgEffectDrawer(d, index, d.list);
    PrVec2 local{};
    bool holds = inside(space, point, &local) && silhouetteHolds(inner, clip.content, local);
    queryDrawerEnd(d);
    return holds;
}

bool svgNodePlace(const SvgTree* tree, PrVec2 viewport, SvgNodeIndex index, SvgPlace* out) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, viewport, nullptr, &tiles);
    RawArray<SvgNodeIndex> path;
    pathTo(tree, index, &path);
    bool drawn = path.count != 0 && path.data[0] == 0;
    // The transform of each node on the path from its user space to its parent's, and the nearest viewport around the
    // node.
    RawArray<PrAffine2> locals;
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
            // A viewport of no area draws nothing, but what is inside it still has geometry, which is placed at the
            // viewport's corner, without the scale of a view box that it cannot hold.
            if (!svgPlaceViewport(d, v, &area, &place))
                place = prAffine2Translate(PrVec2{area.left, area.top});
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
        out->content = d.viewport;
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
    if (options.clipped && (options.fill || parts != 0))
        box = clippedBounds(d, index, parts);
    else if (options.fill || parts != 0)
        box = svgEffectBounds(d, index, parts);
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

bool svgShapePointAtLength(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, float distance, PrVec2* out) {
    SvgTiles tiles{};
    SvgDrawer d = queryDrawer(tree, place.viewport, nullptr, &tiles);
    PrPath* path = prPathCreate();
    svgShapePath(d, tree->nodes.data[index], path);
    PrPathMeasure* measure = prPathMeasureCreate();
    prPathMeasureSetPath(measure, path, prAffine2Identity(), kStrokeTolerance);
    bool found = prPathVerbCount(path) != 0;
    if (prPathMeasureLength(measure) > 0)
        *out = prPathMeasurePosition(measure, distance).point;
    else
        *out = found ? prPathPoints(path)[0] : PrVec2{0, 0};
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
    queryDrawerEnd(d);
    return found;
}

} // namespace pr_impl
