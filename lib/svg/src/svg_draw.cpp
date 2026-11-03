// The SVG tree: drawing it into a draw list.

#include "svg/src/svg_draw.h"

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/path_measure.h"
#include "pr/path_polyline.h"

#include "css/src/css_animation.h"
#include "css/src/css_shape.h"
#include "svg/src/svg_filter.h"
#include "svg/src/svg_image.h"
#include "svg/src/svg_text.h"

namespace pr_impl {

namespace {

void drawNode(SvgDrawer& d, SvgNodeIndex index);
void drawWithEffects(SvgDrawer& d, SvgNodeIndex index);

void drawChildren(SvgDrawer& d, const SvgNode& node) {
    for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = d.tree->nodes.data[child].next)
        drawNode(d, child);
}

// Pushes `transform` onto the list, from the user space that it establishes to the current one, and makes it the
// current user space. Returns the user space that it replaces, which popTransform restores.
PrAffine2 pushTransform(SvgDrawer& d, PrAffine2 transform) {
    PrAffine2 saved = d.space;
    prDrawListPushTransformAffine2(d.list, transform);
    d.space = prAffine2Concat(transform, d.space);
    return saved;
}

void popTransform(SvgDrawer& d, PrAffine2 saved) {
    prDrawListPopTransform(d.list);
    d.space = saved;
}

// Returns a length in a viewport of `viewport`.
float resolveIn(PrVec2 viewport, SvgLengthPercentage length, SvgAxis axis) {
    float base;
    if (axis == SvgAxis_X)
        base = viewport.x;
    else if (axis == SvgAxis_Y)
        base = viewport.y;
    else
        base = prF32Sqrt((viewport.x * viewport.x + viewport.y * viewport.y) / 2);
    return svgLengthResolve(length, base);
}

float resolve(const SvgDrawer& d, SvgLengthPercentage length, SvgAxis axis) {
    return resolveIn(d.viewport, length, axis);
}

// Returns the radii of a rect's corners or of an ellipse: a radius that is auto takes the other's value, and both auto
// are zero (SVG2/geometry.html#RX).
PrVec2 radiiOf(PrVec2 viewport, SvgLengthPercentage rx, SvgLengthPercentage ry, bool rxAuto, bool ryAuto) {
    float x = resolveIn(viewport, rx, SvgAxis_X);
    float y = resolveIn(viewport, ry, SvgAxis_Y);
    if (rxAuto && ryAuto)
        return PrVec2{0, 0};
    return PrVec2{rxAuto ? y : x, ryAuto ? x : y};
}

PrRect rectOf(PrVec2 viewport, const SvgRectShape& r) {
    PrVec2 origin{resolveIn(viewport, r.x, SvgAxis_X), resolveIn(viewport, r.y, SvgAxis_Y)};
    PrVec2 size{resolveIn(viewport, r.width, SvgAxis_X), resolveIn(viewport, r.height, SvgAxis_Y)};
    return prRectAt(origin, svgLimitExtent(r.limits, size, viewport));
}

// Returns a foreignObject's positioning rectangle (SVG2/embedded.html#Placement).
PrRect foreignRectOf(PrVec2 viewport, const SvgForeignShape& f) {
    PrVec2 origin{resolveIn(viewport, f.x, SvgAxis_X), resolveIn(viewport, f.y, SvgAxis_Y)};
    PrVec2 size{resolveIn(viewport, f.width, SvgAxis_X), resolveIn(viewport, f.height, SvgAxis_Y)};
    return prRectAt(origin, svgLimitExtent(f.limits, size, viewport));
}

// Returns the radii of a rect's corners as it is drawn: clamped to half its width and height, and zero unless both are
// greater than zero (SVG2/shapes.html#RectElement).
PrVec2 cornerRadii(PrVec2 viewport, const SvgRectShape& r, PrRect rect) {
    PrVec2 radii = radiiOf(viewport, r.rx, r.ry, r.rxAuto, r.ryAuto);
    float rx = prF32Clamp(radii.x, 0, prRectWidth(rect) / 2);
    float ry = prF32Clamp(radii.y, 0, prRectHeight(rect) / 2);
    return rx > 0 && ry > 0 ? PrVec2{rx, ry} : PrVec2{0, 0};
}

// How far the lines that a length is measured along may lie from the curves of a shape, in user units.
constexpr float kLengthTolerance = 0.01f;

// Returns the length of `path` in user units.
float lengthOf(const PrPath* path) {
    PrPathMeasure* measure = prPathMeasureCreate();
    prPathMeasureSetPath(measure, path, prAffine2Identity(), kLengthTolerance);
    float length = prPathMeasureLength(measure);
    prPathMeasureDestroy(measure);
    return length;
}

// Returns the radii of an ellipse or a circle.
PrVec2 ellipseRadii(PrVec2 viewport, const SvgEllipseShape& s) {
    if (s.circle) {
        float r = resolveIn(viewport, s.rx, SvgAxis_Other);
        return PrVec2{r, r};
    }
    return radiiOf(viewport, s.rx, s.ry, s.rxAuto, s.ryAuto);
}

// The default object size, which an image with no natural size of its own takes (SVG2/embedded.html#Placement).
constexpr PrVec2 kDefaultObjectSize{300, 150};

// The natural dimensions of what an image element shows: a width or a height below zero if it has none, and a ratio of
// width over height, or zero for none.
PrSvgNaturalSize naturalOf(const SvgPicture& picture) {
    if (picture.raster != nullptr) {
        PrUVec2 extent = prDrawImageExtent(picture.raster);
        float ratio = extent.y != 0 ? float(extent.x) / float(extent.y) : 0;
        return PrSvgNaturalSize{float(extent.x), float(extent.y), ratio};
    }
    return picture.svg != nullptr ? picture.svg->natural : PrSvgNaturalSize{-1, -1, 0};
}

// Returns the size that the CSS default sizing algorithm gives an object of `natural` dimensions in no constraint, with
// the default object size where they leave a dimension open (css-images-3/#default-sizing).
PrVec2 defaultSizeOf(PrSvgNaturalSize natural) {
    bool hasWidth = natural.width >= 0;
    bool hasHeight = natural.height >= 0;
    if (hasWidth && hasHeight)
        return PrVec2{natural.width, natural.height};
    if (natural.ratio > 0) {
        if (hasWidth)
            return PrVec2{natural.width, natural.width / natural.ratio};
        if (hasHeight)
            return PrVec2{natural.height * natural.ratio, natural.height};
        // The largest rectangle of the ratio that the default object size contains.
        float width = prF32Min(kDefaultObjectSize.x, kDefaultObjectSize.y * natural.ratio);
        return PrVec2{width, width / natural.ratio};
    }
    return PrVec2{hasWidth ? natural.width : kDefaultObjectSize.x, hasHeight ? natural.height : kDefaultObjectSize.y};
}

// Returns an image element's positioning rectangle (SVG2/embedded.html#Placement). A width or a height that is auto is
// sized from the natural dimensions of what the image shows, by the default sizing algorithm with the other dimension
// as a constraint. The min and max sizes limit a dimension that is given before the other is sized from it, and limit
// the result.
PrRect imageRectOf(PrVec2 viewport, const SvgImageShape& image) {
    PrVec2 origin{resolveIn(viewport, image.x, SvgAxis_X), resolveIn(viewport, image.y, SvgAxis_Y)};
    PrVec2 size = svgLimitExtent(image.limits, PrVec2{
        resolveIn(viewport, image.width, SvgAxis_X),
        resolveIn(viewport, image.height, SvgAxis_Y),
    }, viewport);
    if (image.widthAuto || image.heightAuto) {
        PrSvgNaturalSize natural = naturalOf(image.picture);
        PrVec2 fallback = defaultSizeOf(natural);
        if (image.widthAuto && image.heightAuto)
            size = fallback;
        else if (image.widthAuto)
            size.x = natural.ratio > 0 ? size.y * natural.ratio : fallback.x;
        else
            size.y = natural.ratio > 0 ? size.x / natural.ratio : fallback.y;
        size = svgLimitExtent(image.limits, size, viewport);
    }
    return prRectAt(origin, size);
}

// Returns the bounds of a node's own geometry, which a fill-box transform and a bounding-box paint server are relative
// to (SVG2/coords.html#ObjectBoundingBox). A node without geometry of its own has empty bounds.
PrRect geometryBounds(const SvgDrawer& d, const SvgNode& node) {
    switch (node.kind) {
        case SvgNodeKind_Rect: return rectOf(d.viewport, node.rect);
        case SvgNodeKind_Ellipse: {
            PrVec2 center{resolve(d, node.ellipse.cx, SvgAxis_X), resolve(d, node.ellipse.cy, SvgAxis_Y)};
            PrVec2 radii = ellipseRadii(d.viewport, node.ellipse);
            return PrRect{center.x - radii.x, center.y - radii.y, center.x + radii.x, center.y + radii.y};
        }
        case SvgNodeKind_Line: {
            PrVec2 from{resolve(d, node.line.x1, SvgAxis_X), resolve(d, node.line.y1, SvgAxis_Y)};
            PrVec2 to{resolve(d, node.line.x2, SvgAxis_X), resolve(d, node.line.y2, SvgAxis_Y)};
            return PrRect{prF32Min(from.x, to.x), prF32Min(from.y, to.y), prF32Max(from.x, to.x), prF32Max(from.y, to.y)};
        }
        case SvgNodeKind_Path: return node.path.bounds;
        case SvgNodeKind_Text: return svgTextBounds(node.text);
        case SvgNodeKind_Image: return imageRectOf(d.viewport, node.image);
        case SvgNodeKind_ForeignObject: return foreignRectOf(d.viewport, node.foreign);
        default: return PrRect{};
    }
}

// Returns `transform`, a node's transform in the current user space, constrained by the node's vector effects
// (SVG2/coords.html#VectorEffectsCalculation). The host space is the space that the tree is drawn in, the viewport
// coordinate system of the outermost svg element. The node's user space keeps the host's scale with non-scaling-size,
// whose CTM is divided by the square root of its determinant, and loses rotation and skew with non-rotation, whose CTM
// is a uniform scale by that root. Its origin stays where the CTM puts it, unless fixed-position places it at the
// transform's translation in the host. A non-scaling-size whose CTM has no determinant is ignored.
PrAffine2 constrained(const SvgDrawer& d, const SvgNode& node, PrAffine2 transform) {
    SvgVectorEffectFlags flags = node.vectorEffects;
    PrAffine2 ctm = prAffine2Concat(transform, d.space);
    PrAffine2 parent{};
    float root = prF32Sqrt(prF32Abs(ctm.a * ctm.d - ctm.b * ctm.c));
    if (!prAffine2Invert(d.space, &parent) || ((flags & SvgVectorEffectFlag_NonScalingSize) != 0 && !(root > 0)))
        return transform;
    PrAffine2 host = ctm;
    bool size = (flags & SvgVectorEffectFlag_NonScalingSize) != 0;
    bool rotation = (flags & SvgVectorEffectFlag_NonRotation) != 0;
    if (size && rotation) {
        host.a = host.d = 1;
        host.b = host.c = 0;
    } else if (size) {
        host.a /= root;
        host.b /= root;
        host.c /= root;
        host.d /= root;
    } else if (rotation) {
        host.a = host.d = root;
        host.b = host.c = 0;
    }
    if ((flags & SvgVectorEffectFlag_FixedPosition) != 0) {
        host.e = transform.e;
        host.f = transform.f;
    }
    return prAffine2Concat(host, parent);
}

// Returns the node's transform from its user space to the current one, which `d.space` maps to the space that the tree
// is drawn in. A transform relative to the reference box resolves against the view box, the nearest viewport at its
// origin, or against the node's geometry for fill-box and stroke-box (css-transforms-1/#transform-box). Vector effects
// then constrain it.
PrAffine2 transformOf(const SvgDrawer& d, const SvgNode& node) {
    PrAffine2 transform = node.transform;
    if (node.transformStyle != kArrayMissing) {
        const SvgTransformStyle& style = d.tree->transforms.data[node.transformStyle];
        bool geometry = style.box == PrCssKeyword_FillBox || style.box == PrCssKeyword_StrokeBox ||
            style.box == PrCssKeyword_ContentBox || style.box == PrCssKeyword_BorderBox;
        PrRect box = geometry ? geometryBounds(d, node) : PrRect{0, 0, d.viewport.x, d.viewport.y};
        transform = svgTransformMatrix(style, box);
    }
    return node.vectorEffects != 0 ? constrained(d, node, transform) : transform;
}

// Writes the rectangle of a viewport in its parent's user space to `area`, and the transform from the user space of its
// content to its parent's to `place` (SVG2/coords.html#ComputingAViewportsTransform). Returns false if the viewport
// renders nothing: it has no area, or its viewBox has no width or height (SVG2/coords.html#ViewBoxAttribute).
bool placeViewport(const SvgDrawer& d, const SvgViewportShape& v, PrRect* area, PrAffine2* place) {
    if (v.fills)
        *area = PrRect{0, 0, d.viewport.x, d.viewport.y};
    else
        *area = prRectAt(PrVec2{resolve(d, v.x, SvgAxis_X), resolve(d, v.y, SvgAxis_Y)},
            PrVec2{resolve(d, v.width, SvgAxis_X), resolve(d, v.height, SvgAxis_Y)});
    float width = prRectWidth(*area);
    float height = prRectHeight(*area);
    if (!(width > 0) || !(height > 0))
        return false;
    if (v.hasViewBox && (!(v.viewBox.right > v.viewBox.left) || !(v.viewBox.bottom > v.viewBox.top)))
        return false;
    *place = prAffine2Translate(PrVec2{area->left, area->top});
    if (v.hasViewBox)
        *place = prAffine2Concat(svgViewBoxTransform(v.viewBox, v.aspect, width, height), *place);
    if (v.hasRefX || v.hasRefY) {
        PrVec2 extent = v.hasViewBox ? PrVec2{prRectWidth(v.viewBox), prRectHeight(v.viewBox)} : PrVec2{width, height};
        PrVec2 reference = prAffine2MapPoint(*place, PrVec2{
            svgLengthResolve(v.refX, extent.x),
            svgLengthResolve(v.refY, extent.y),
        });
        PrVec2 shift{v.hasRefX ? area->left - reference.x : 0, v.hasRefY ? area->top - reference.y : 0};
        *place = prAffine2Concat(*place, prAffine2Translate(shift));
        *area = PrRect{area->left + shift.x, area->top + shift.y, area->right + shift.x, area->bottom + shift.y};
    }
    return true;
}

// Returns the size of the user space of a viewport's content, which its content's percentages are of.
PrVec2 contentExtent(const SvgViewportShape& v, PrRect area) {
    if (v.hasViewBox)
        return PrVec2{prRectWidth(v.viewBox), prRectHeight(v.viewBox)};
    return PrVec2{prRectWidth(area), prRectHeight(area)};
}

PrRect strokeGeometryBounds(const SvgDrawer& d, const SvgNode& node);

// Returns true if the node has geometry of its own, whose bounds count toward a container's even if they have no area,
// as a horizontal line's do (SVG2/coords.html#BoundingBoxes).
bool hasGeometry(const SvgNode& node) {
    switch (node.kind) {
        case SvgNodeKind_Rect:
        case SvgNodeKind_Ellipse:
        case SvgNodeKind_Line:
        case SvgNodeKind_Path:
        case SvgNodeKind_Text:
        case SvgNodeKind_Image:
        case SvgNodeKind_ForeignObject: return true;
        default: return false;
    }
}

// Returns the bounds of `rect` mapped by `transform`. A rectangle without area, such as a line's bounds, maps as its
// corners do.
PrRect mapBounds(PrAffine2 transform, PrRect rect) {
    PrVec2 corners[4] = {
        {rect.left, rect.top}, {rect.right, rect.top}, {rect.right, rect.bottom},
        {rect.left, rect.bottom},
    };
    PrVec2 first = prAffine2MapPoint(transform, corners[0]);
    PrRect bounds{first.x, first.y, first.x, first.y};
    for (PrVec2 corner : corners) {
        PrVec2 at = prAffine2MapPoint(transform, corner);
        bounds = PrRect{
            prF32Min(bounds.left, at.x), prF32Min(bounds.top, at.y), prF32Max(bounds.right, at.x),
            prF32Max(bounds.bottom, at.y),
        };
    }
    return bounds;
}

// Joins `rect` into `bounds`, which hold nothing until `any` is set.
void join(PrRect* bounds, bool* any, PrRect rect) {
    if (*any)
        *bounds = PrRect{
            prF32Min(bounds->left, rect.left), prF32Min(bounds->top, rect.top), prF32Max(bounds->right, rect.right),
            prF32Max(bounds->bottom, rect.bottom),
        };
    else
        *bounds = rect;
    *any = true;
}

void markerBounds(SvgDrawer& d, const SvgNode& node, PrRect* bounds, bool* any);

// Writes the bounds of the geometry of a node and its descendants to `out`, with what `parts` adds, in the space inside
// the node's transform. Returns false if neither the node nor a descendant has geometry.
bool nodeBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts, PrRect* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrRect bounds{};
    bool any = false;
    if (hasGeometry(node)) {
        bool stroke = (parts & SvgBoundsFlag_Stroke) != 0;
        join(&bounds, &any, stroke ? strokeGeometryBounds(d, node) : geometryBounds(d, node));
        if ((parts & SvgBoundsFlag_Markers) != 0)
            markerBounds(d, node, &bounds, &any);
    }
    PrAffine2 inner = prAffine2Identity();
    PrVec2 saved = d.viewport;
    PrAffine2 savedSpace = d.space;
    bool placed = true;
    if (node.kind == SvgNodeKind_Viewport) {
        PrRect area{};
        placed = placeViewport(d, node.viewport, &area, &inner);
        if (placed)
            d.viewport = contentExtent(node.viewport, area);
    }
    // The children's vector effects read the space that they are drawn in.
    d.space = prAffine2Concat(inner, savedSpace);
    for (SvgNodeIndex child = node.first; placed && child != kSvgNodeNone; child = d.tree->nodes.data[child].next) {
        PrAffine2 local = transformOf(d, d.tree->nodes.data[child]);
        PrAffine2 space = d.space;
        d.space = prAffine2Concat(local, space);
        PrRect inside{};
        bool found = nodeBounds(d, child, parts, &inside);
        d.space = space;
        if (found)
            join(&bounds, &any, mapBounds(prAffine2Concat(local, inner), inside));
    }
    d.space = savedSpace;
    d.viewport = saved;
    *out = bounds;
    return any;
}

// Returns the bounds of the geometry of a node and its descendants in the node's user space, which is inside its own
// transform: the bounding box of a use element that context paint maps onto. Without geometry, the bounds are empty.
PrRect boundsOf(SvgDrawer& d, SvgNodeIndex index) {
    PrRect bounds{};
    nodeBounds(d, index, 0, &bounds);
    return bounds;
}

// Returns the layer of context paint that a paint of a context element gives, in the current user space and onto `box`.
// Context paint in a context element is the paint of its own context element, or none without one.
SvgContextPaint contextPaintOf(const SvgDrawer& d, const SvgPaint& paint, PrRect box) {
    if (paint.kind == SvgPaintKind_ContextFill || paint.kind == SvgPaintKind_ContextStroke) {
        if (d.context != nullptr)
            return paint.kind == SvgPaintKind_ContextFill ? d.context->fill : d.context->stroke;
        return SvgContextPaint{.paint = {.kind = SvgPaintKind_None, .server = kSvgResourceNone}};
    }
    return SvgContextPaint{.paint = paint, .space = d.space, .box = box};
}

// Returns true if a paint server draws the paint, which then maps onto the bounding box of its geometry.
bool usesServer(const SvgPaint& paint) {
    return paint.kind == SvgPaintKind_Url && paint.server != kSvgResourceNone;
}

// ---------------------------------------------------------------------------------------------------------------------
// Markers

// Writes how the marker `index` is placed at a vertex (SVG2/painting.html#RenderingMarkers): the transform from its
// viewport's space, placed at the vertex and turned by its orient, to the shape's user space to `place`, the transform
// of its viewBox into its viewport to `fit`, and the size of its viewport and of its content's user space.
// `strokeWidth` is the unit of markerUnits="strokeWidth", and `start` says whether the marker is the start's, which
// auto-start-reverse turns around. Returns false if the marker draws nothing.
bool placeMarker(const SvgDrawer& d, SvgResourceIndex index, SvgVertex vertex, float strokeWidth, bool start,
        PrAffine2* place, PrAffine2* fit, PrVec2* viewport, PrVec2* extent) {
    if (index == kSvgResourceNone)
        return false;
    const SvgMarker& m = d.tree->resources.data[index].marker;
    float width = resolve(d, m.width, SvgAxis_X);
    float height = resolve(d, m.height, SvgAxis_Y);
    if (m.content == kSvgNodeNone || !(width > 0) || !(height > 0))
        return false;
    if (m.hasViewBox && (!(m.viewBox.right > m.viewBox.left) || !(m.viewBox.bottom > m.viewBox.top)))
        return false;
    float angle = m.angle;
    if (m.orient != SvgOrient_Angle)
        angle = vertex.angle + (m.orient == SvgOrient_AutoStartReverse && start ? kPrF32Pi : 0);
    float scale = m.strokeUnits ? strokeWidth : 1;
    *fit = m.hasViewBox ? svgViewBoxTransform(m.viewBox, m.aspect, width, height) : prAffine2Identity();
    *extent = m.hasViewBox ? PrVec2{prRectWidth(m.viewBox), prRectHeight(m.viewBox)} : PrVec2{width, height};
    *viewport = PrVec2{width, height};
    // The reference point, in the content's user space, maps to a point of the viewport, which is placed at the vertex.
    PrVec2 reference = prAffine2MapPoint(*fit, PrVec2{
        svgLengthResolve(m.refX, extent->x),
        svgLengthResolve(m.refY, extent->y),
    });
    *place = prAffine2Concat(prAffine2Translate(-reference), prAffine2Scale(PrVec2{scale, scale}));
    *place = prAffine2Concat(prAffine2Concat(*place, prAffine2Rotate(angle)), prAffine2Translate(vertex.point));
    return true;
}

// Draws a marker at a vertex, as placeMarker places it.
void drawMarker(SvgDrawer& d, SvgResourceIndex index, SvgVertex vertex, float strokeWidth, bool start) {
    if (d.markerDepth == kSvgMaxMarkerDepth)
        return;
    for (uint32_t i = 0; i < d.markerDepth; ++i) {
        if (d.markers[i] == index)
            return;
    }
    PrAffine2 place{};
    PrAffine2 fit{};
    PrVec2 area{};
    PrVec2 extent{};
    if (!placeMarker(d, index, vertex, strokeWidth, start, &place, &fit, &area, &extent))
        return;
    const SvgMarker& m = d.tree->resources.data[index].marker;
    PrAffine2 outer = pushTransform(d, place);
    if (m.clips)
        prDrawListPushClipRect(d.list, PrRect{0, 0, area.x, area.y}, PrDrawEdges_Antialiased);
    PrAffine2 viewport = pushTransform(d, fit);
    PrVec2 saved = d.viewport;
    d.viewport = extent;
    d.markers[d.markerDepth++] = index;
    drawNode(d, m.content);
    --d.markerDepth;
    d.viewport = saved;
    popTransform(d, viewport);
    if (m.clips)
        prDrawListPopClip(d.list);
    popTransform(d, outer);
}

// The vertices that a shape's markers are placed at, and the width of its stroke, which markerUnits="strokeWidth"
// scales them by.
struct MarkerVertices {
    Array<SvgVertex> found;     // the vertices of a shape whose vertices the build did not record
    const SvgVertex* PR_NULLABLE vertices;
    uint32_t count;
    float strokeWidth;
};

// Writes the vertices of a shape's markers to `out`, which the caller resets
// (SVG2/painting.html#VertexMarkerProperties).
void markerVerticesOf(const SvgDrawer& d, const SvgNode& node, MarkerVertices* out) {
    const SvgMarkers& markers = d.tree->markers.data[node.markers];
    out->vertices = markers.vertices.data;
    out->count = markers.vertices.count;
    if (node.kind != SvgNodeKind_Path) {
        Array<PrPathDataCommand> commands;
        svgShapeCommands(node, d.viewport, &commands);
        svgMarkerVertices(commands.data, commands.count, &out->found);
        // A rect's or an ellipse's path ends with a closepath of no length, after the command that returns to the
        // start. Its markers are not placed by counting commands: the last vertex is the first, once
        // (SVG2/shapes.html#Introduction, SVG2/painting.html#VertexMarkerProperties). The closepath's vertex stays,
        // with the direction of a closed subpath's end.
        bool closed = commands.count != 0 && commands.data[commands.count - 1].letter == 'Z';
        if (closed && out->found.count >= 2)
            out->found.erase(out->found.count - 2);
        commands.reset();
        out->vertices = out->found.data;
        out->count = out->found.count;
    }
    // The stroke's width in user units. A stroke that does not scale is as wide in the host's space, the space that the
    // tree is drawn in, whatever the user space's scale (SVG2/painting.html#NonScalingStroke).
    out->strokeWidth = svgLengthResolve(markers.strokeWidth, svgPercentBase(d, SvgAxis_Other));
    if (node.paints.nonScaling) {
        float host = prF32Sqrt(prF32Abs(d.space.a * d.space.d - d.space.b * d.space.c));
        out->strokeWidth = host > 0 ? out->strokeWidth / host : out->strokeWidth;
    }
}

// Draws a shape's vertex markers, from the first vertex to the last (SVG2/painting.html#Markers). The shape is their
// context element. `box` is the shape's geometry bounds.
void drawMarkers(SvgDrawer& d, const SvgNode& node, PrRect box) {
    if (node.markers == kArrayMissing)
        return;
    const SvgMarkers& markers = d.tree->markers.data[node.markers];
    MarkerVertices v{};
    markerVerticesOf(d, node, &v);
    if (v.count != 0) {
        SvgContext context{
            .fill = contextPaintOf(d, node.paints.fill, box),
            .stroke = contextPaintOf(d, node.paints.stroke, box),
        };
        const SvgContext* outer = d.context;
        d.context = &context;
        drawMarker(d, markers.start, v.vertices[0], v.strokeWidth, true);
        for (uint32_t i = 1; i + 1 < v.count; ++i)
            drawMarker(d, markers.mid, v.vertices[i], v.strokeWidth, false);
        drawMarker(d, markers.end, v.vertices[v.count - 1], v.strokeWidth, false);
        d.context = outer;
    }
    v.found.reset();
}

// Joins the bounds of a shape's markers into `bounds`: the bounds of each marker's content, with its strokes and
// markers, in the shape's user space (SVG2/coords.html#BoundingBoxes). A marker inside its own content adds nothing.
void markerBounds(SvgDrawer& d, const SvgNode& node, PrRect* bounds, bool* any) {
    if (node.markers == kArrayMissing || d.markerDepth == kSvgMaxMarkerDepth)
        return;
    const SvgMarkers& markers = d.tree->markers.data[node.markers];
    MarkerVertices v{};
    markerVerticesOf(d, node, &v);
    for (uint32_t i = 0; i < v.count; ++i) {
        SvgResourceIndex index = i == 0 ? markers.start : i + 1 == v.count ? markers.end : markers.mid;
        bool nested = false;
        for (uint32_t k = 0; k < d.markerDepth; ++k)
            nested = nested || d.markers[k] == index;
        PrAffine2 place{};
        PrAffine2 fit{};
        PrVec2 area{};
        PrVec2 extent{};
        if (nested || !placeMarker(d, index, v.vertices[i], v.strokeWidth, i == 0, &place, &fit, &area, &extent))
            continue;
        PrAffine2 content = prAffine2Concat(fit, place);
        PrAffine2 savedSpace = d.space;
        PrVec2 saved = d.viewport;
        d.space = prAffine2Concat(content, savedSpace);
        d.viewport = extent;
        d.markers[d.markerDepth++] = index;
        PrRect inside{};
        bool found = nodeBounds(d, d.tree->resources.data[index].marker.content,
            SvgBoundsFlag_Stroke | SvgBoundsFlag_Markers, &inside);
        --d.markerDepth;
        d.viewport = saved;
        d.space = savedSpace;
        if (found)
            join(bounds, any, mapBounds(content, inside));
    }
    v.found.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

void drawRect(SvgDrawer& d, const SvgNode& node) {
    PrRect rect = rectOf(d.viewport, node.rect);
    if (!(prRectWidth(rect) > 0) || !(prRectHeight(rect) > 0))
        return;
    PrVec2 radii = cornerRadii(d.viewport, node.rect, rect);
    PrCorner round = prCornerRound(radii);
    PrCornerRect shape{rect, {round, round, round, round}};
    bool rounded = radii.x > 0;
    float length = -1;
    if (node.paints.pathLength >= 0) {
        PrPath* outline = prPathCreate();
        prPathAddCornerRect(outline, shape);
        length = lengthOf(outline);
        prPathDestroy(outline);
    }
    svgPaintShape(d, node.paints, true, rect, length, [&](PrDrawPaint paint) {
        if (rounded)
            prDrawListCornerRect(d.list, shape, paint);
        else
            prDrawListRect(d.list, rect, paint);
    }, [&] { drawMarkers(d, node, rect); });
}

void drawEllipse(SvgDrawer& d, const SvgNode& node) {
    PrVec2 center{resolve(d, node.ellipse.cx, SvgAxis_X), resolve(d, node.ellipse.cy, SvgAxis_Y)};
    PrVec2 radii = ellipseRadii(d.viewport, node.ellipse);
    if (!(radii.x > 0) || !(radii.y > 0))
        return;
    PrRect box{center.x - radii.x, center.y - radii.y, center.x + radii.x, center.y + radii.y};
    float length = -1;
    if (node.paints.pathLength >= 0) {
        PrPath* outline = prPathCreate();
        prPathAddEllipse(outline, center, radii, 0);
        length = lengthOf(outline);
        prPathDestroy(outline);
    }
    svgPaintShape(d, node.paints, true, box, length,
        [&](PrDrawPaint paint) { prDrawListEllipse(d.list, center, radii, 0, paint); }, [&] { drawMarkers(d, node, box); });
}

void drawLine(SvgDrawer& d, const SvgNode& node) {
    PrVec2 from{resolve(d, node.line.x1, SvgAxis_X), resolve(d, node.line.y1, SvgAxis_Y)};
    PrVec2 to{resolve(d, node.line.x2, SvgAxis_X), resolve(d, node.line.y2, SvgAxis_Y)};
    PrRect box = geometryBounds(d, node);
    float length = prF32Sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
    svgPaintShape(d, node.paints, false, box, length, [&](PrDrawPaint paint) { prDrawListLine(d.list, from, to, paint); },
        [&] { drawMarkers(d, node, box); });
}

void drawPath(SvgDrawer& d, const SvgNode& node) {
    float length = node.paints.pathLength >= 0 ? lengthOf(node.path.path) : -1;
    svgPaintShape(d, node.paints, true, node.path.bounds, length, [&](PrDrawPaint paint) {
        prDrawListPath(d.list, node.path.path, d.silhouette ? node.clipRule : node.fillRule, paint);
    }, [&] { drawMarkers(d, node, node.path.bounds); });
}

// Returns the concrete object rectangle of an image in its positioning rectangle `box`: the object sized by object-fit
// from its natural dimensions, and placed by object-position (css-images-3/#the-object-fit). An object without a
// natural ratio, or a dimension that it lacks, takes the box's.
PrRect concreteRectOf(const SvgImageShape& image, PrSvgNaturalSize natural, PrRect box) {
    PrVec2 room{prRectWidth(box), prRectHeight(box)};
    PrVec2 size = room;
    PrVec2 own{natural.width >= 0 ? natural.width : room.x, natural.height >= 0 ? natural.height : room.y};
    auto fitted = [&](bool cover) {
        if (!(natural.ratio > 0))
            return room;
        bool wide = room.x / room.y > natural.ratio;
        return wide != cover ? PrVec2{room.y * natural.ratio, room.y} : PrVec2{room.x, room.x / natural.ratio};
    };
    switch (image.fit) {
        case PrLayoutObjectFit_Contain: size = fitted(false); break;
        case PrLayoutObjectFit_Cover: size = fitted(true); break;
        case PrLayoutObjectFit_None: size = own; break;
        case PrLayoutObjectFit_ScaleDown: {
            PrVec2 contained = fitted(false);
            size = contained.x < own.x ? contained : own;
            break;
        }
        default: return box;
    }
    PrVec2 at{
        box.left + svgLengthResolve(image.position[0], room.x - size.x),
        box.top + svgLengthResolve(image.position[1], room.y - size.y),
    };
    return prRectAt(at, size);
}

// Draws an image element (SVG2/embedded.html#ImageElement). object-fit and object-position give the concrete object
// rectangle in the positioning rectangle. preserveAspectRatio then fits the image's natural ratio into it, which gives
// the image-rendering rectangle, unless the image has no natural ratio or object-fit already kept it. A raster image
// covers that rectangle, and an SVG image takes it as its viewport. The image is clipped to the positioning rectangle
// unless its overflow is visible. A positioning rectangle with no width or height draws nothing.
void drawImage(SvgDrawer& d, const SvgImageShape& image) {
    if (image.picture.raster == nullptr && image.picture.svg == nullptr)
        return;
    PrRect box = imageRectOf(d.viewport, image);
    if (!(prRectWidth(box) > 0) || !(prRectHeight(box) > 0))
        return;
    PrSvgNaturalSize natural = naturalOf(image.picture);
    PrRect area = concreteRectOf(image, natural, box);
    if (image.fit == PrLayoutObjectFit_Fill && natural.ratio > 0) {
        float height = 1 / natural.ratio;
        PrAffine2 fit =
            svgViewBoxTransform(PrRect{0, 0, 1, height}, image.aspect, prRectWidth(area), prRectHeight(area));
        area = prRectAt(PrVec2{area.left + fit.e, area.top + fit.f}, PrVec2{fit.a, height * fit.d});
    }
    if (!(prRectWidth(area) > 0) || !(prRectHeight(area) > 0))
        return;
    if (image.clips)
        prDrawListPushClipRect(d.list, box, PrDrawEdges_Antialiased);
    if (image.picture.raster != nullptr) {
        PrUVec2 texels = prDrawImageExtent(image.picture.raster);
        PrDrawBrush brush = prDrawBrushImage(
            prDrawSourceImage(image.picture.raster, PrRect{0, 0, float(texels.x), float(texels.y)}), area,
            image.sampling);
        prDrawListRect(d.list, area, {.brush = &brush});
    } else {
        svgImageDrawRendering(image.picture.svg, area, d.list);
    }
    if (image.clips)
        prDrawListPopClip(d.list);
}

// Draws a foreignObject's content, its layout root, which the host lays out in a containing block of the positioning
// rectangle's size and draws from its top left (SVG2/embedded.html#ForeignObjectElement). The content is clipped to the
// rectangle unless its overflow is visible. A rectangle with no width or height draws nothing.
void drawForeign(SvgDrawer& d, const SvgNode& node) {
    const SvgForeignShape& foreign = node.foreign;
    if (foreign.root == kPrLayoutNodeNone || d.host.drawRoot == nullptr)
        return;
    PrRect box = foreignRectOf(d.viewport, foreign);
    if (!(prRectWidth(box) > 0) || !(prRectHeight(box) > 0))
        return;
    if (foreign.clips)
        prDrawListPushClipRect(d.list, box, PrDrawEdges_Antialiased);
    PrAffine2 outer = pushTransform(d, prAffine2Translate(PrVec2{box.left, box.top}));
    d.host.drawRoot(d.host.context, foreign.root, d.list);
    popTransform(d, outer);
    if (foreign.clips)
        prDrawListPopClip(d.list);
}

// Draws a node that establishes a viewport: its children, with its viewBox mapped into the viewport, clipped to the
// viewport unless its overflow is visible (SVG2/coords.html#ComputingAViewportsTransform). Its clip path, mask and
// opacity apply in the user space of its content, as its bounding box is.
void drawViewport(SvgDrawer& d, SvgNodeIndex index) {
    SvgViewportShape v = svgViewportFromNode(d, index);
    PrRect area{};
    PrAffine2 place{};
    if (!placeViewport(d, v, &area, &place))
        return;
    // The view's transform moves the root as its transform property would, about the root's center, which the user
    // agent's sheet makes the root's transform-origin.
    const SvgRootView* view = d.host.view;
    bool turned = index == 0 && view != nullptr && view->hasTransform;
    PrAffine2 unturned{};
    if (turned) {
        PrVec2 center{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        unturned = pushTransform(d, prAffine2Concat(prAffine2Concat(prAffine2Translate(-center), view->transform),
            prAffine2Translate(center)));
    }
    if (v.clips)
        prDrawListPushClipRect(d.list, area, PrDrawEdges_Antialiased);
    PrAffine2 outer = pushTransform(d, place);
    PrVec2 saved = d.viewport;
    d.viewport = contentExtent(v, area);
    drawWithEffects(d, index);
    d.viewport = saved;
    popTransform(d, outer);
    if (v.clips)
        prDrawListPopClip(d.list);
    if (turned)
        popTransform(d, unturned);
}

// Draws a use element's node, whose paints are the context paint of its shadow tree
// (SVG2/painting.html#TermContextElement). A paint server of the use element maps onto the bounding box of its content.
void drawContext(SvgDrawer& d, SvgNodeIndex index) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrRect box{};
    if (usesServer(node.paints.fill) || usesServer(node.paints.stroke))
        box = boundsOf(d, index);
    SvgContext context{
        .fill = contextPaintOf(d, node.paints.fill, box),
        .stroke = contextPaintOf(d, node.paints.stroke, box),
    };
    const SvgContext* outer = d.context;
    d.context = &context;
    drawChildren(d, node);
    d.context = outer;
}

// ---------------------------------------------------------------------------------------------------------------------
// Clipping and masking

// Returns the bounds of a node's own geometry and its stroke shape (SVG2/coords.html#TermStrokeBoundingBox). A stroke's
// dashes do not change them. Text's stroke is taken as half its width on every side.
PrRect strokeGeometryBounds(const SvgDrawer& d, const SvgNode& node) {
    PrRect fill = geometryBounds(d, node);
    float dashes[kSvgMaxDashes];
    PrDrawStroke stroke{};
    if (!svgStrokeFor(d, node.paints, -1, dashes, &stroke))
        return fill;
    stroke.shape.dashes = nullptr;
    stroke.shape.dashCount = 0;
    if (node.kind == SvgNodeKind_Text) {
        float half = stroke.shape.width / 2;
        return PrRect{fill.left - half, fill.top - half, fill.right + half, fill.bottom + half};
    }
    PrPath* path = prPathCreate();
    if (node.kind == SvgNodeKind_Path) {
        prPathAddPath(path, node.path.path, prAffine2Identity());
    } else {
        Array<PrPathDataCommand> commands;
        svgShapeCommands(node, d.viewport, &commands);
        prPathAppendCommands(path, commands.data, commands.count);
        commands.reset();
    }
    PrRect bounds = fill;
    if (prPathVerbCount(path) != 0) {
        PrPathPolyline outline{};
        prPathStrokeOutline(path, &stroke.shape, kLengthTolerance, &outline);
        if (outline.pointCount != 0)
            bounds = prRectUnion(fill, prPathPolylineBounds(&outline));
        prPathPolylineReset(&outline);
    }
    prPathDestroy(path);
    return bounds;
}

// Returns the bounds of a node and its descendants with what `parts` adds, in the user space that its clip path and
// mask apply in: its own, or its content's for a node that establishes a viewport, whose children it bounds. The object
// bounding box adds nothing, and the stroke bounding box the strokes.
PrRect effectBoundsOf(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrRect bounds{};
    if (node.kind != SvgNodeKind_Viewport) {
        nodeBounds(d, index, parts, &bounds);
        return bounds;
    }
    bool any = false;
    for (SvgNodeIndex child = node.first; child != kSvgNodeNone; child = d.tree->nodes.data[child].next) {
        PrRect inner{};
        if (nodeBounds(d, child, parts, &inner))
            join(&bounds, &any, mapBounds(transformOf(d, d.tree->nodes.data[child]), inner));
    }
    return bounds;
}

// Returns the reference box that a box keyword of clip-path or a mask layer names on a node, in the user space that its
// effects apply in (css-masking-1/#typedef-geometry-box). An element without a CSS box uses fill-box for content-box
// and padding-box, and stroke-box for border-box and margin-box. view-box is the nearest viewport, from the origin of
// its user space.
PrRect referenceBoxOf(SvgDrawer& d, SvgNodeIndex index, PrCssKeyword box) {
    switch (box) {
        case PrCssKeyword_FillBox:
        case PrCssKeyword_ContentBox:
        case PrCssKeyword_PaddingBox: return effectBoundsOf(d, index, 0);
        case PrCssKeyword_ViewBox: return PrRect{0, 0, d.viewport.x, d.viewport.y};
        default: return effectBoundsOf(d, index, SvgBoundsFlag_Stroke);
    }
}

// Returns the box keyword among the components of a computed value, or `fallback` if it has none.
PrCssKeyword boxKeywordOf(CssValueRun run, PrCssKeyword fallback) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Keyword)
            continue;
        switch (cssValueKeyword(&run.values[i])) {
            case PrCssKeyword_ContentBox:
            case PrCssKeyword_PaddingBox:
            case PrCssKeyword_BorderBox:
            case PrCssKeyword_MarginBox:
            case PrCssKeyword_HalfBorderBox:
            case PrCssKeyword_FillBox:
            case PrCssKeyword_StrokeBox:
            case PrCssKeyword_ViewBox:
            case PrCssKeyword_NoClip: return cssValueKeyword(&run.values[i]);
            default: break;
        }
    }
    return fallback;
}

// Returns the computed value of a property of the compositing group from the values that effects hold.
CssValueRun maskingValue(const SvgEffects& masking, PrCssProperty property) {
    return cssStyleValue(masking.values, cssStyleSlot(property).index);
}

// Returns the transform from the unit square onto `box`, which content in objectBoundingBox units is drawn in.
PrAffine2 unitOnto(PrRect box) {
    return prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(box), prRectHeight(box)}),
        prAffine2Translate(PrVec2{box.left, box.top}));
}

// Returns true if a clipPath, a mask or a filter cannot apply where it is drawn: inside its own content, or too deep in
// others'. Such a reference is in error, and is ignored.
bool effectCycles(const SvgDrawer& d, SvgResourceIndex index) {
    if (d.effectDepth == kSvgMaxEffectDepth)
        return true;
    for (uint32_t i = 0; i < d.effectDepth; ++i) {
        if (d.effects[i] == index)
            return true;
    }
    return false;
}

// Returns a drawer that records the content of the clipPath, mask or filter `index` into `list`, in the drawer's user
// space. The content has no context element (SVG2/painting.html#context-paint).
SvgDrawer effectDrawer(const SvgDrawer& d, SvgResourceIndex index, PrDrawList* list) {
    SvgDrawer inner = d;
    inner.list = list;
    inner.context = nullptr;
    inner.effects[inner.effectDepth++] = index;
    return inner;
}

// Returns `rect`, grown to whole units of the space that the tree is drawn in. A list source is rendered into a layer
// whose texels start at its rect, so a rect on whole units keeps the layer's texels on the target's pixels, and an edge
// of the content that lies on a pixel's edge stays sharp.
PrRect alignedArea(const SvgDrawer& d, PrRect rect) {
    PrAffine2 fromTree{};
    if (!prAffine2Invert(d.space, &fromTree))
        return rect;
    PrRect tree = prAffine2MapRect(d.space, rect);
    tree = PrRect{prF32Floor(tree.left), prF32Floor(tree.top), prF32Ceil(tree.right), prF32Ceil(tree.bottom)};
    return prRectUnion(rect, prAffine2MapRect(fromTree, tree));
}

// Returns a mask of what `list` draws over `rect`, read in `mode`, which is zero outside `rect`
// (css-masking-1/#MaskValues). What the list draws outside `rect` is not read.
PrDrawMask maskOfList(const SvgDrawer& d, PrDrawList* list, PrRect rect, PrDrawMaskMode mode, PrColorSpace space) {
    return prDrawMaskFromList(list, alignedArea(d, rect), mode, space);
}

// What an element's clip path and mask layers pushed onto the list around its content, which endMasking pops.
struct Masked {
    uint32_t clips;                 // the number of clips pushed
    bool coverage;                  // a group masked by the coverage of a clip path was pushed
    bool masks;                     // `mask` applies to the group of the element's content
    PrDrawList* PR_NULLABLE values; // the list whose values `mask` reads, which `Masked` holds a reference to

    // The mask, set if `masks` is. It is large, and most nodes draw without one, so it is not initialized otherwise.
    PrDrawMask mask;
};

// Returns the outline that a clipPath's content gives as one path, if the content is one shape with no clip path of its
// own inside a group with none either: the case that a clip path of the list draws exactly. `space` maps the content's
// user space into the drawer's. Returns false if the clip path needs its coverage drawn.
bool clipOutline(SvgDrawer& d, SvgNodeIndex content, PrAffine2 space, PrPath* out, PrPathFillRule* rule) {
    const SvgNode& group = d.tree->nodes.data[content];
    if (group.effects != kArrayMissing || group.first == kSvgNodeNone || group.first != group.last)
        return false;
    const SvgNode& child = d.tree->nodes.data[group.first];
    bool shape = child.kind == SvgNodeKind_Rect || child.kind == SvgNodeKind_Ellipse || child.kind == SvgNodeKind_Path;
    if (!shape || !child.visible || child.effects != kArrayMissing)
        return false;
    PrAffine2 transform = prAffine2Concat(transformOf(d, child), prAffine2Concat(transformOf(d, group), space));
    if (child.kind == SvgNodeKind_Path) {
        prPathAddPath(out, child.path.path, transform);
    } else {
        Array<PrPathDataCommand> commands;
        svgShapeCommands(child, d.viewport, &commands);
        PrPath* path = prPathCreate();
        prPathAppendCommands(path, commands.data, commands.count);
        prPathAddPath(out, path, transform);
        prPathDestroy(path);
        commands.reset();
    }
    *rule = child.clipRule;
    return true;
}

// Pushes the clip path of the clipPath `index` around the content of an element whose object bounding box is `box`
// (css-masking-1/#ClipPathElement). A clip path of one shape is a clip of the list. Any other is the coverage of its
// children's silhouettes, drawn into a list that masks a group. Returns false if the clip path clips everything away.
bool pushClipPath(SvgDrawer& d, SvgResourceIndex index, PrRect box, Masked* out) {
    const SvgClipPath& clip = d.tree->resources.data[index].clipPath;
    if (clip.content == kSvgNodeNone)
        return false;
    // A clip path in objectBoundingBox units on a box with no width or height clips everything away.
    if (clip.boundingBox && prRectIsEmpty(box))
        return false;
    // The clipPath's transform maps the content, in the bounding box's units if it takes them, into user space.
    PrAffine2 space = prAffine2Concat(clip.boundingBox ? unitOnto(box) : prAffine2Identity(), clip.transform);
    PrPath* outline = prPathCreate();
    PrPathFillRule rule = PrPathFillRule_NonZero;
    if (clipOutline(d, clip.content, space, outline, &rule)) {
        bool any = prPathVerbCount(outline) != 0;
        if (any) {
            prDrawListPushClipPath(d.list, outline, rule, PrDrawEdges_Antialiased);
            ++out->clips;
        }
        prPathDestroy(outline);
        return any;
    }
    prPathDestroy(outline);
    PrDrawList* coverage = prDrawListCreate();
    SvgDrawer inner = effectDrawer(d, index, coverage);
    inner.silhouette = true;
    PrAffine2 outer = pushTransform(inner, space);
    drawNode(inner, clip.content);
    popTransform(inner, outer);
    PrRect bounds = prDrawListBounds(coverage);
    bool any = !prRectIsEmpty(bounds);
    if (any) {
        PrDrawMask mask = maskOfList(d, coverage, bounds, PrDrawMaskMode_Alpha, PrColorSpace_Srgb);
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        prDrawListPushGroup(d.list, &group);
        out->coverage = true;
    }
    prDrawListRelease(coverage);
    return any;
}

// Pushes the clip path that the masking of the node `index` gives around its content (css-masking-1/#the-clip-path): a
// basic shape in its reference box, the reference box itself, or a clipPath. A reference to a clipPath that is drawing
// its own content is ignored. Returns false if the clip path clips everything away.
bool pushClip(SvgDrawer& d, SvgNodeIndex index, const SvgEffects& masking, Masked* out) {
    switch (masking.clip) {
        case SvgClipKind_Url:
            if (effectCycles(d, masking.clipPath))
                return true;
            return pushClipPath(d, masking.clipPath, effectBoundsOf(d, index, 0), out);
        case SvgClipKind_Box: {
            PrRect box = referenceBoxOf(
                d, index, boxKeywordOf(maskingValue(masking, PrCssProperty_ClipPath), PrCssKeyword_BorderBox));
            prDrawListPushClipRect(d.list, box, PrDrawEdges_Antialiased);
            ++out->clips;
            return !prRectIsEmpty(box);
        }
        case SvgClipKind_Shape: {
            PrPath* path = prPathCreate();
            PrPathFillRule rule = PrPathFillRule_NonZero;
            svgClipShapePath(d, index, masking, path, &rule);
            bool any = prPathVerbCount(path) != 0;
            if (any) {
                prDrawListPushClipPath(d.list, path, rule, PrDrawEdges_Antialiased);
                ++out->clips;
            }
            prPathDestroy(path);
            return any;
        }
        default:
            return true;
    }
}

// A mask layer as drawn: the list whose values mask, over its area, and how its values are read. A transparent layer
// has no list.
struct LayerMask {
    PrDrawList* PR_NULLABLE list;   // the layer holds a reference
    PrRect area;
    PrDrawMaskMode mode;
    PrColorSpace space;
};

// What drawing a mask layer gives.
enum {
    LayerResult_Drawn,          // the layer masks with its values
    LayerResult_Ignored,        // the layer is a reference in error, which does not mask
    LayerResult_Hides,          // the element draws nothing, as a mask region with no width or height asks
};
typedef uint8_t LayerResult;

// Draws the content of the mask element `index`, for an element whose object bounding box is `box`, into a list of its
// own (css-masking-1/#MaskElement). The content is clipped to the mask's region.
LayerResult maskElementLayer(SvgDrawer& d, SvgResourceIndex index, PrRect box, LayerMask* out) {
    if (effectCycles(d, index))
        return LayerResult_Ignored;
    const SvgMask& m = d.tree->resources.data[index].mask;
    out->mode = m.mode;
    out->space = m.space;
    // A region in objectBoundingBox units, or content in them, on a box with no width or height draws nothing.
    if ((!m.userSpace || m.contentBoundingBox) && prRectIsEmpty(box))
        return LayerResult_Hides;
    auto place = [&](SvgLengthPercentage length, SvgAxis axis, float origin, float extent) {
        return m.userSpace ? resolve(d, length, axis) : origin + (length.px + length.percent / 100) * extent;
    };
    PrVec2 size{prRectWidth(box), prRectHeight(box)};
    PrVec2 at{place(m.x, SvgAxis_X, box.left, size.x), place(m.y, SvgAxis_Y, box.top, size.y)};
    PrVec2 extent{place(m.width, SvgAxis_X, 0, size.x), place(m.height, SvgAxis_Y, 0, size.y)};
    // A width or a height of zero or less disables rendering of the element.
    if (!(extent.x > 0) || !(extent.y > 0))
        return LayerResult_Hides;
    out->area = prRectAt(at, extent);
    if (m.content == kSvgNodeNone)
        return LayerResult_Drawn;
    out->list = prDrawListCreate();
    SvgDrawer inner = effectDrawer(d, index, out->list);
    prDrawListPushClipRect(inner.list, out->area, PrDrawEdges_Antialiased);
    PrAffine2 outer = pushTransform(inner, m.contentBoundingBox ? unitOnto(box) : prAffine2Identity());
    drawNode(inner, m.content);
    popTransform(inner, outer);
    prDrawListPopClip(inner.list);
    return LayerResult_Drawn;
}

// Returns the length of a computed <length-percentage> at `index` of `run`, with percentages of `base`.
float lengthIn(CssValueRun run, uint32_t index, float base) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Percentage)
        return float(value->number) * base / 100;
    double px = 0;
    return cssResolveLength(run, index, double(base), &px) ? float(px) : 0;
}

// Returns the size of an image layer in a positioning area of `area` (css-backgrounds-3/#background-size), for an image
// of `natural` dimensions, before round repetition adjusts it. An image without natural dimensions, such as a gradient,
// takes the area's in each axis that the size leaves auto.
PrVec2 layerSizeOf(CssValueRun size, PrSvgNaturalSize natural, PrVec2 area) {
    bool hasWidth = natural.width >= 0;
    bool hasHeight = natural.height >= 0;
    float ratio = hasWidth && hasHeight && natural.height > 0 ? natural.width / natural.height : natural.ratio;
    PrCssKeyword keyword = PrCssKeyword_Count;
    if (size.count != 0 && size.values[0].kind == PrCssValueKind_Keyword)
        keyword = cssValueKeyword(size.values);
    if (keyword == PrCssKeyword_Cover || keyword == PrCssKeyword_Contain) {
        if (!(ratio > 0))
            return area;
        bool cover = keyword == PrCssKeyword_Cover;
        bool widthFirst = cover ? area.x / area.y > ratio : area.x / area.y <= ratio;
        return widthFirst ? PrVec2{area.x, area.x / ratio} : PrVec2{area.y * ratio, area.y};
    }
    uint32_t second = size.count != 0 ? size.values[0].next : 0;
    bool autoX = size.count == 0 || cssValueIsKeyword(size.values, PrCssKeyword_Auto);
    bool autoY = second >= size.count || cssValueIsKeyword(&size.values[second], PrCssKeyword_Auto);
    float x = autoX ? 0 : prF32Max(lengthIn(size, 0, area.x), 0);
    float y = autoY ? 0 : prF32Max(lengthIn(size, second, area.y), 0);
    if (autoX && autoY) {
        // The default sizing algorithm (css-images-3/#default-sizing), with the area as the default object size.
        if (hasWidth && hasHeight)
            return PrVec2{natural.width, natural.height};
        if (hasWidth)
            return PrVec2{natural.width, ratio > 0 ? natural.width / ratio : area.y};
        if (hasHeight)
            return PrVec2{ratio > 0 ? natural.height * ratio : area.x, natural.height};
        if (ratio > 0)
            return area.x / area.y > ratio ? PrVec2{area.y * ratio, area.y} : PrVec2{area.x, area.x / ratio};
        return area;
    }
    if (autoX)
        x = ratio > 0 ? y * ratio : hasWidth ? natural.width : area.x;
    if (autoY)
        y = ratio > 0 ? x / ratio : hasHeight ? natural.height : area.y;
    return PrVec2{x, y};
}

// How an image layer repeats along one axis (css-backgrounds-3/#background-repeat).
PrCssKeyword repeatOf(CssValueRun repeat, bool vertical) {
    if (repeat.count == 0)
        return PrCssKeyword_Repeat;
    PrCssKeyword first = cssValueKeyword(repeat.values);
    if (first == PrCssKeyword_RepeatX)
        return vertical ? PrCssKeyword_NoRepeat : PrCssKeyword_Repeat;
    if (first == PrCssKeyword_RepeatY)
        return vertical ? PrCssKeyword_Repeat : PrCssKeyword_NoRepeat;
    uint32_t second = repeat.values[0].next;
    return vertical && second < repeat.count ? cssValueKeyword(&repeat.values[second]) : first;
}

// Returns the reference box that a box keyword of mask-origin or mask-clip names on a node. An element without a CSS
// box positions its mask layers in its fill-box for content-box, padding-box and border-box, and clips them to its
// fill-box for content-box and padding-box and to its stroke-box for border-box (css-masking-1/#the-mask-origin,
// css-masking-1/#the-mask-clip).
PrRect layerBoxOf(SvgDrawer& d, SvgNodeIndex index, PrCssKeyword box, bool origin) {
    if (origin && box == PrCssKeyword_BorderBox)
        return referenceBoxOf(d, index, PrCssKeyword_FillBox);
    return referenceBoxOf(d, index, box);
}

// Draws a mask layer of an image or a gradient into a list of its own, sized by mask-size and placed by mask-position
// in the box of mask-origin, repeated by mask-repeat, and painted within the box of mask-clip
// (css-masking-1/#positioned-masks). An image that has not loaded is ignored.
LayerResult imageLayer(SvgDrawer& d, SvgNodeIndex index, const SvgEffects& masking, uint32_t item, LayerMask* out) {
    const SvgMaskLayer& layer = masking.layers.data[item];
    out->mode = PrDrawMaskMode_Alpha;
    out->space = PrColorSpace_Srgb;
    auto itemOf = [&](PrCssProperty property) {
        CssValueRun run = maskingValue(masking, property);
        CssListItems items = cssListItems(run);
        return items.count != 0 ? cssListItem(run, items, item) : CssValueRun{};
    };
    PrRect area = layerBoxOf(d, index, boxKeywordOf(itemOf(PrCssProperty_MaskOrigin), PrCssKeyword_BorderBox), true);
    PrCssKeyword clip = boxKeywordOf(itemOf(PrCssProperty_MaskClip), PrCssKeyword_BorderBox);
    // A layer that is not clipped paints over all that the element draws, which its stroke box bounds.
    PrRect painted;
    if (clip == PrCssKeyword_NoClip)
        painted = prRectUnion(area, referenceBoxOf(d, index, PrCssKeyword_StrokeBox));
    else
        painted = layerBoxOf(d, index, clip, false);
    PrVec2 extent{prRectWidth(area), prRectHeight(area)};
    out->area = painted;
    if (prRectIsEmpty(painted) || !(extent.x > 0) || !(extent.y > 0))
        return LayerResult_Drawn;
    PrSvgNaturalSize natural{-1, -1, 0};
    if (layer.kind == SvgMaskLayerKind_Image && layer.picture.raster != nullptr) {
        PrUVec2 texels = prDrawImageExtent(layer.picture.raster);
        natural = PrSvgNaturalSize{
            float(texels.x), float(texels.y), texels.y != 0 ? float(texels.x) / float(texels.y) : 0,
        };
    } else if (layer.kind == SvgMaskLayerKind_Image && layer.picture.svg != nullptr) {
        natural = layer.picture.svg->natural;
    } else if (layer.kind == SvgMaskLayerKind_Image) {
        return LayerResult_Ignored;
    }
    CssValueRun sizeItem = itemOf(PrCssProperty_MaskSize);
    PrVec2 size = layerSizeOf(sizeItem, natural, extent);
    CssValueRun repeat = itemOf(PrCssProperty_MaskRepeat);
    PrCssKeyword repeats[2] = {repeatOf(repeat, false), repeatOf(repeat, true)};
    // Round scales the image so that whole copies fill the area. If only one axis rounds and the other is auto, the
    // other keeps the image's ratio.
    bool autoY = sizeItem.count == 0 || sizeItem.values[0].next >= sizeItem.count ||
        cssValueIsKeyword(&sizeItem.values[sizeItem.values[0].next], PrCssKeyword_Auto);
    bool autoX = sizeItem.count == 0 || cssValueIsKeyword(sizeItem.values, PrCssKeyword_Auto);
    for (uint32_t axis = 0; axis < 2; ++axis) {
        float& own = axis == 0 ? size.x : size.y;
        float& other = axis == 0 ? size.y : size.x;
        float whole = axis == 0 ? extent.x : extent.y;
        if (repeats[axis] != PrCssKeyword_Round || !(own > 0))
            continue;
        float rounded = whole / prF32Max(prF32Round(whole / own), 1);
        bool otherRounds = repeats[1 - axis] == PrCssKeyword_Round;
        if (!otherRounds && (axis == 0 ? autoY : autoX))
            other *= rounded / own;
        own = rounded;
    }
    if (!(size.x > 0) || !(size.y > 0))
        return LayerResult_Drawn;
    // The position: a percentage places the point that far across the image at the point that far across the area.
    SvgLengthPercentage position[2] = {SvgLengthPercentage{}, SvgLengthPercentage{}};
    CssValueRun place = itemOf(PrCssProperty_MaskPosition);
    if (place.count != 0) {
        uint32_t second = place.values[0].next;
        const CssValueNode* x = &place.values[0];
        if (x->kind == PrCssValueKind_Percentage)
            position[0] = SvgLengthPercentage{0, float(x->number)};
        else
            position[0] = SvgLengthPercentage{lengthIn(place, 0, 0), lengthIn(place, 0, 100) - lengthIn(place, 0, 0)};
        if (second < place.count) {
            const CssValueNode* y = &place.values[second];
            if (y->kind == PrCssValueKind_Percentage)
                position[1] = SvgLengthPercentage{0, float(y->number)};
            else
                position[1] = SvgLengthPercentage{
                    lengthIn(place, second, 0),
                    lengthIn(place, second, 100) - lengthIn(place, second, 0),
                };
        }
    }
    PrVec2 at{
        area.left + svgLengthResolve(position[0], extent.x - size.x),
        area.top + svgLengthResolve(position[1], extent.y - size.y),
    };
    PrVec2 spacing{};
    for (uint32_t axis = 0; axis < 2; ++axis) {
        if (repeats[axis] != PrCssKeyword_Space)
            continue;
        // As many whole copies as fit, the first and last against the area's edges. Fewer than two copies are placed as
        // one, by the position.
        float whole = axis == 0 ? extent.x : extent.y;
        float own = axis == 0 ? size.x : size.y;
        float copies = prF32Floor(whole / own);
        if (copies >= 2) {
            (axis == 0 ? at.x : at.y) = axis == 0 ? area.left : area.top;
            (axis == 0 ? spacing.x : spacing.y) = (whole - copies * own) / (copies - 1);
        } else {
            repeats[axis] = PrCssKeyword_NoRepeat;
        }
    }
    PrRect tile = prRectAt(at, size);
    // A raster image is sampled as it is. A gradient and an SVG image are drawn at the tile's size.
    PrDrawList* source = nullptr;
    PrDrawBrush brush{};
    if (layer.kind == SvgMaskLayerKind_Image && layer.picture.raster != nullptr) {
        PrUVec2 texels = prDrawImageExtent(layer.picture.raster);
        PrRect rect{0, 0, float(texels.x), float(texels.y)};
        brush = prDrawBrushImage(prDrawSourceImage(layer.picture.raster, rect), tile, PrDrawSampling_Linear);
    } else {
        source = prDrawListCreate();
        PrRect own{0, 0, size.x, size.y};
        if (layer.kind == SvgMaskLayerKind_Gradient)
            prLayoutGradientDraw(&layer.gradient, own, own, source);
        else
            svgImageDrawRendering(layer.picture.svg, own, source);
        brush = prDrawBrushImage(prDrawSourceList(source, own), tile, PrDrawSampling_Linear);
    }
    brush.image.tiles.spacing = spacing;
    brush.image.tiles.x = repeats[0] == PrCssKeyword_NoRepeat ? PrDrawExtend_None : PrDrawExtend_Repeat;
    brush.image.tiles.y = repeats[1] == PrCssKeyword_NoRepeat ? PrDrawExtend_None : PrDrawExtend_Repeat;
    brush.image.tiles.flags = PrDrawTileFlag_Clamped;
    out->list = prDrawListCreate();
    prDrawListRect(out->list, painted, PrDrawPaint{.brush = &brush});
    prDrawListRelease(source);
    return LayerResult_Drawn;
}

// Writes the mask that the mask layers of the node `index` give to `out` (css-masking-1/#layering). Each layer's values
// become alpha by its mask-mode, and the layers composite from the bottom up by their mask-composite, as an isolated
// group. A none layer is transparent black. A layer that references nothing that masks, or whose image cannot be
// displayed or has not loaded, is ignored. Returns false if the element draws nothing.
bool maskOf(SvgDrawer& d, SvgNodeIndex index, const SvgEffects& masking, Masked* out) {
    CssValueRun modes = maskingValue(masking, PrCssProperty_MaskMode);
    CssValueRun composites = maskingValue(masking, PrCssProperty_MaskComposite);
    CssListItems modeItems = cssListItems(modes);
    CssListItems compositeItems = cssListItems(composites);
    PrRect box = effectBoundsOf(d, index, 0);
    Array<LayerMask> layers;
    Array<PrDrawComposite> operators;
    bool hides = false;
    for (uint32_t i = 0; i < masking.layers.count && !hides; ++i) {
        const SvgMaskLayer& layer = masking.layers.data[i];
        LayerMask drawn{.mode = PrDrawMaskMode_Alpha, .space = PrColorSpace_Srgb};
        LayerResult result = LayerResult_Drawn;
        if (layer.kind == SvgMaskLayerKind_Mask)
            result = maskElementLayer(d, layer.mask, box, &drawn);
        else if (layer.kind == SvgMaskLayerKind_Image || layer.kind == SvgMaskLayerKind_Gradient)
            result = imageLayer(d, index, masking, i, &drawn);
        else if (layer.kind == SvgMaskLayerKind_Error)
            result = LayerResult_Ignored;
        if (result == LayerResult_Ignored)
            continue;
        hides = result == LayerResult_Hides;
        // mask-mode overrides a mask element's mask-type, and an image is an alpha mask unless it asks for luminance
        // (css-masking-1/#the-mask-mode).
        PrCssKeyword mode = PrCssKeyword_MatchSource;
        if (modeItems.count != 0)
            mode = cssValueKeyword(cssListItem(modes, modeItems, i).values);
        if (mode == PrCssKeyword_Alpha)
            drawn.mode = PrDrawMaskMode_Alpha;
        else if (mode == PrCssKeyword_Luminance)
            drawn.mode = PrDrawMaskMode_Luminance;
        layers.push(drawn);
        if (compositeItems.count != 0)
            operators.push(cssMaskCompositeOperator(cssValueKeyword(cssListItem(composites, compositeItems,
                i).values)));
        else
            operators.push(PrDrawComposite_SourceOver);
    }
    bool draws = !hides;
    if (draws && layers.count == 0) {
        // Every layer was ignored, which masks nothing.
        out->masks = false;
    } else if (draws && layers.count == 1) {
        LayerMask& only = layers.data[0];
        draws = only.list != nullptr && !prRectIsEmpty(only.area);
        if (draws) {
            out->mask = maskOfList(d, only.list, only.area, only.mode, only.space);
            out->masks = true;
            out->values = only.list;
            prDrawListRetain(only.list);
        }
    } else if (draws) {
        // The layers composite in a list of their own, from the bottom layer up. The operator of the bottom layer is
        // ignored, since nothing lies below it.
        PrRect area{};
        for (uint32_t i = 0; i < layers.count; ++i)
            area = layers.data[i].list != nullptr ? prRectUnion(area, layers.data[i].area) : area;
        draws = !prRectIsEmpty(area);
        PrDrawList* combined = draws ? prDrawListCreate() : nullptr;
        PrDrawPaint white{.color = prColor16FromColor(PrColor{1, 1, 1, 1})};
        for (uint32_t k = layers.count; draws && k-- > 0;) {
            const LayerMask& layer = layers.data[k];
            PrDrawComposite composite = k + 1 == layers.count ? PrDrawComposite_SourceOver : operators.data[k];
            if (layer.list == nullptr) {
                // A transparent layer leaves what lies below it, or clears it.
                if (composite == PrDrawComposite_SourceIn || composite == PrDrawComposite_SourceOut)
                    prDrawListRect(combined, area, PrDrawPaint{
                        .color = white.color,
                        .composite = PrDrawComposite_Clear,
                    });
                continue;
            }
            PrDrawMask mask = maskOfList(d, layer.list, layer.area, layer.mode, layer.space);
            PrDrawGroup group = prDrawGroupDefault();
            group.bounds = area;
            group.composite = composite;
            group.mask = &mask;
            prDrawListPushGroup(combined, &group);
            prDrawListRect(combined, area, white);
            prDrawListPopGroup(combined);
        }
        if (draws) {
            out->mask = maskOfList(d, combined, area, PrDrawMaskMode_Alpha, PrColorSpace_Srgb);
            out->masks = true;
            out->values = combined;
        } else {
            prDrawListRelease(combined);
        }
    }
    for (uint32_t i = 0; i < layers.count; ++i)
        prDrawListRelease(layers.data[i].list);
    layers.reset();
    operators.reset();
    return draws;
}

// Pops what beginMasking pushed.
void endMasking(SvgDrawer& d, const Masked& masked) {
    prDrawListRelease(masked.values);
    if (masked.coverage)
        prDrawListPopGroup(d.list);
    for (uint32_t i = 0; i < masked.clips; ++i)
        prDrawListPopClip(d.list);
}

// Pushes the clip path of the node `index` around its content, and writes its mask, which the group of its content
// takes, to `out`. Returns false if the node draws nothing, after popping what it pushed. In the drawing of a clip
// path's silhouettes, only clip paths apply (css-masking-1/#ClipPathElement).
bool beginMasking(SvgDrawer& d, SvgNodeIndex index, Masked* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    if (node.effects == kArrayMissing)
        return true;
    const SvgEffects& masking = d.tree->effects.data[node.effects];
    bool draws = pushClip(d, index, masking, out);
    if (draws && !d.silhouette && masking.layers.count != 0)
        draws = maskOf(d, index, masking, out);
    if (!draws)
        endMasking(d, *out);
    return draws;
}

// Draws a node's own content, without its transform and effects.
void drawBody(SvgDrawer& d, SvgNodeIndex index) {
    const SvgNode& node = d.tree->nodes.data[index];
    switch (node.kind) {
        case SvgNodeKind_Group:
            if (node.context)
                drawContext(d, index);
            else
                drawChildren(d, node);
            break;
        case SvgNodeKind_Viewport: drawChildren(d, node); break;
        case SvgNodeKind_Text:
            svgDrawText(d, node);
            drawChildren(d, node);
            break;
        default:
            if (!node.visible)
                break;
            if (node.kind == SvgNodeKind_Rect)
                drawRect(d, node);
            else if (node.kind == SvgNodeKind_Ellipse)
                drawEllipse(d, node);
            else if (node.kind == SvgNodeKind_Line)
                drawLine(d, node);
            else if (node.kind == SvgNodeKind_Path)
                drawPath(d, node);
            else if (node.kind == SvgNodeKind_Image && !d.silhouette)
                drawImage(d, node.image);
            else if (node.kind == SvgNodeKind_ForeignObject && !d.silhouette)
                drawForeign(d, node);
            break;
    }
}

// Draws a node's content with its effects, in the order of filter, clip path, mask and opacity
// (css-masking-1/#placement, filter-effects-1/#placement). The clip path is pushed around the group whose filter, mask
// and opacity apply to the content, in that order, and which blends with what lies beneath by mix-blend-mode
// (compositing-1/#mix-blend-mode). The silhouettes of a clip path take only clip paths.
void drawWithEffects(SvgDrawer& d, SvgNodeIndex index) {
    SvgFilterGraph graph{};
    SvgFiltered filtered = d.silhouette ? SvgFiltered_None : svgFilterFromNode(d, index, &graph);
    if (filtered == SvgFiltered_Hidden)
        return;
    Masked masked;
    masked.clips = 0;
    masked.coverage = false;
    masked.masks = false;
    masked.values = nullptr;
    if (!beginMasking(d, index, &masked)) {
        prDrawFilterRelease(graph.filter);
        return;
    }
    const SvgNode& node = d.tree->nodes.data[index];
    const SvgEffects* effects = node.effects != kArrayMissing && !d.silhouette ? &d.tree->effects.data[node.effects] : nullptr;
    // A node that paints and asks for linearRGB composites in linear light (SVG2/painting.html#ColorInterpolation). A
    // group's children composite by their own values. The silhouettes of a clip path have no opacity.
    bool paints = node.kind != SvgNodeKind_Group && node.kind != SvgNodeKind_Viewport;
    bool linear = !d.silhouette && node.linear && (paints || node.opacity < 1);
    // Blending and isolation make a group of the node (compositing-1/#csscompositingrules_SVG). A node that establishes
    // a stacking context is an isolated group (SVG2/render.html#EstablishingStackingContex), which matters only to what
    // blends with what lies beneath it: an inner viewport that clips, and a node with a clip path or a mask.
    bool stacks = d.tree->readsBackdrop && !d.silhouette &&
        ((node.kind == SvgNodeKind_Viewport && node.viewport.clips && index != 0) ||
            (effects != nullptr && (effects->clip != SvgClipKind_None || effects->layers.count != 0)));
    bool composites = stacks || (effects != nullptr && (effects->blend != PrDrawBlendMode_Normal ||
        effects->composite != PrDrawComposite_SourceOver || effects->isolated));
    bool grouped =
        !d.silhouette && (node.opacity < 1 || linear || masked.masks || graph.filter != nullptr || composites);
    if (grouped) {
        PrDrawGroup group = prDrawGroupDefault();
        group.opacity = node.opacity;
        group.space = linear ? PrColorSpace_SrgbLinear : PrColorSpace_Srgb;
        group.mask = masked.masks ? &masked.mask : nullptr;
        group.filter = graph.filter;
        group.bounds = graph.region;
        if (effects != nullptr) {
            group.blendMode = effects->blend;
            group.composite = effects->composite;
            group.isolated = effects->isolated;
        }
        group.isolated = group.isolated || stacks;
        prDrawListPushGroup(d.list, &group);
    }
    drawBody(d, index);
    if (grouped)
        prDrawListPopGroup(d.list);
    endMasking(d, masked);
    prDrawFilterRelease(graph.filter);
}

void drawNode(SvgDrawer& d, SvgNodeIndex index) {
    const SvgNode& node = d.tree->nodes.data[index];
    PrAffine2 transform = transformOf(d, node);
    bool transformed = !prAffine2IsIdentity(transform);
    PrAffine2 outer = d.space;
    if (transformed)
        outer = pushTransform(d, transform);
    if (node.kind == SvgNodeKind_Viewport)
        drawViewport(d, index);
    else
        drawWithEffects(d, index);
    if (transformed)
        popTransform(d, outer);
}

// ---------------------------------------------------------------------------------------------------------------------
// Paints

// Returns the coordinate of a gradient in the space of its brush: a fraction of the bounding box, which a percentage is
// a hundredth of, or a length in user space, which a percentage is of the viewport along `axis`.
float gradientCoordinate(const SvgDrawer& d, const SvgGradient& g, SvgLengthPercentage length, SvgAxis axis) {
    return g.userSpace ? resolve(d, length, axis) : length.px + length.percent / 100;
}

// Sets `base` to paint with the gradient at `opacity`, or with a color if the gradient paints one. Returns false if it
// paints nothing.
bool gradientPaint(const SvgDrawer& d, const SvgGradient& g, float opacity, PrRect box, PrDrawPaint* base, PrDrawBrush* brush) {
    if (g.stopCount == 0)
        return false;
    const PrDrawColorStop* stops = g.stops;
    PrColor last = prColorFromColor16(stops[g.stopCount - 1].color);
    last.a *= opacity;
    if (g.stopCount == 1) {
        PrColor only = prColorFromColor16(stops[0].color);
        only.a *= opacity;
        base->color = only;
        return true;
    }
    PrDrawBrush b{};
    if (!g.radial) {
        PrVec2 start{gradientCoordinate(d, g, g.x1, SvgAxis_X), gradientCoordinate(d, g, g.y1, SvgAxis_Y)};
        PrVec2 end{gradientCoordinate(d, g, g.x2, SvgAxis_X), gradientCoordinate(d, g, g.y2, SvgAxis_Y)};
        // A gradient of no length paints its last stop (SVG2/pservers.html#LinearGradientNotes).
        if (start.x == end.x && start.y == end.y) {
            base->color = last;
            return true;
        }
        b = prDrawBrushLinear(start, end, stops, g.stopCount);
    } else {
        PrVec2 center{gradientCoordinate(d, g, g.cx, SvgAxis_X), gradientCoordinate(d, g, g.cy, SvgAxis_Y)};
        float radius = gradientCoordinate(d, g, g.r, SvgAxis_Other);
        PrVec2 focal{gradientCoordinate(d, g, g.fx, SvgAxis_X), gradientCoordinate(d, g, g.fy, SvgAxis_Y)};
        // A gradient of no radius paints its last stop (SVG2/pservers.html#RadialGradientNotes).
        if (!(radius > 0)) {
            base->color = last;
            return true;
        }
        // A start circle that holds the end circle draws no gradient (SVG2/pservers.html#RadialGradientNotes), though
        // the cone of Canvas's gradient that both circles touch would cover it.
        float focalRadius = gradientCoordinate(d, g, g.fr, SvgAxis_Other);
        PrVec2 apart = center - focal;
        if (focalRadius >= radius + prF32Sqrt(apart.x * apart.x + apart.y * apart.y))
            return false;
        b = prDrawBrushRadial(focal, focalRadius, center, radius, stops, g.stopCount);
    }
    // A gradient in objectBoundingBox units maps onto `box`, which SVG decides, rather than onto the bounds of the
    // command that it paints: text's runs share their text element's box (SVG2/text.html#TextElement).
    b.space = PrDrawBrushSpace_User;
    b.extend = g.extend;
    b.interpolationSpace = g.space;
    // SVG mixes a gradient's stops with their alpha apart, so that a stop toward transparent black mixes toward black
    // (SVG2/pservers.html#StopColorProperty).
    b.interpolationAlpha = PrDrawAlpha_Straight;
    b.transform = g.transform;
    if (!g.userSpace) {
        PrAffine2 unit = prAffine2Concat(prAffine2Scale(PrVec2{prRectWidth(box), prRectHeight(box)}),
            prAffine2Translate(PrVec2{box.left, box.top}));
        b.transform = prAffine2Concat(g.transform, unit);
    }
    b.opacity = opacity;
    *brush = b;
    base->brush = brush;
    return true;
}

// Returns true if a pattern cannot paint where it is drawn: inside its own tile, or too deep in other patterns' tiles.
bool patternCycles(const SvgDrawer& d, SvgResourceIndex index) {
    if (d.patternDepth == kSvgMaxPatternDepth)
        return true;
    for (uint32_t i = 0; i < d.patternDepth; ++i) {
        if (d.patterns[i] == index)
            return true;
    }
    return false;
}

// Returns a list of a pattern's tile: its content in a space `content` maps it into, clipped to a tile of `extent` from
// the origin, which `space` maps into the space that the tree is drawn in. The drawing holds the list. A tile is
// clipped whatever the pattern's overflow (SVG2/pservers.html#PatternNotes).
PrDrawList* tileOf(const SvgDrawer& d, SvgResourceIndex index, PrAffine2 content, PrAffine2 space, PrVec2 extent) {
    SvgTile key{.pattern = index, .content = content, .space = space, .extent = extent, .viewport = d.viewport};
    // A tile inside another's depends on the patterns around it, so only an outermost tile is shared.
    if (d.patternDepth == 0) {
        for (uint32_t i = 0; i < d.tiles->tiles.count; ++i) {
            const SvgTile& tile = d.tiles->tiles.data[i];
            if (tile.pattern == key.pattern && prMemEquals(&tile.content, &key.content, sizeof(PrAffine2)) &&
                    prMemEquals(&tile.space, &key.space, sizeof(PrAffine2)) &&
                    prMemEquals(&tile.extent, &key.extent, sizeof(PrVec2)) &&
                    prMemEquals(&tile.viewport, &key.viewport, sizeof(PrVec2)))
                return tile.list;
        }
    }
    const SvgPattern& p = d.tree->resources.data[index].pattern;
    SvgDrawer inner = d;
    inner.list = prDrawListCreate();
    inner.space = space;
    // The content of a tile has no context element around it (SVG2/painting.html#context-paint).
    inner.context = nullptr;
    inner.patterns[inner.patternDepth++] = index;
    prDrawListPushClipRect(inner.list, PrRect{0, 0, extent.x, extent.y}, PrDrawEdges_Antialiased);
    PrAffine2 outer = pushTransform(inner, content);
    drawNode(inner, p.content);
    popTransform(inner, outer);
    prDrawListPopClip(inner.list);
    key.list = inner.list;
    d.tiles->tiles.push(key);
    return inner.list;
}

// Sets `base` to paint with the pattern at `opacity`. Returns false if it paints nothing: its tile has no area, or it
// has no content.
bool patternPaint(const SvgDrawer& d, SvgResourceIndex index, float opacity, PrRect box, PrDrawPaint* base, PrDrawBrush* brush) {
    const SvgPattern& p = d.tree->resources.data[index].pattern;
    PrVec2 size{prRectWidth(box), prRectHeight(box)};
    if (p.content == kSvgNodeNone)
        return false;
    // The tile in user space: lengths there, or fractions of the bounding box, which a percentage is a hundredth of.
    auto place = [&](SvgLengthPercentage length, SvgAxis axis, float origin, float extent) {
        return p.userSpace ? resolve(d, length, axis) : origin + (length.px + length.percent / 100) * extent;
    };
    PrVec2 at{place(p.x, SvgAxis_X, box.left, size.x), place(p.y, SvgAxis_Y, box.top, size.y)};
    PrVec2 extent{place(p.width, SvgAxis_X, 0, size.x), place(p.height, SvgAxis_Y, 0, size.y)};
    if (!(extent.x > 0) || !(extent.y > 0))
        return false;
    // The content's user space: the viewBox fitted to the tile, or else the tile's own space, scaled by the bounding
    // box for objectBoundingBox content (SVG2/pservers.html#PatternElementPatternContentUnitsAttribute).
    PrAffine2 content = prAffine2Identity();
    if (p.hasViewBox) {
        if (!(p.viewBox.right > p.viewBox.left) || !(p.viewBox.bottom > p.viewBox.top))
            return false;
        content = svgViewBoxTransform(p.viewBox, p.aspect, extent.x, extent.y);
    } else if (p.contentBoundingBox) {
        content = prAffine2Scale(size);
    }
    // The tiles lie in user space, and the pattern's transform then applies there. SVG 2 has the transform apply inside
    // the bounding box's units, which would shear the tiles of a box that is not square while the content in user units
    // stays square.
    PrAffine2 tile = prAffine2Concat(prAffine2Translate(at), p.transform);
    PrDrawList* list = tileOf(d, index, content, prAffine2Concat(tile, d.space), extent);
    PrRect rect{0, 0, extent.x, extent.y};
    PrDrawBrush b = prDrawBrushImage(prDrawSourceList(list, rect), rect, PrDrawSampling_Linear);
    b.image.tiles.x = PrDrawExtend_Repeat;
    b.image.tiles.y = PrDrawExtend_Repeat;
    b.transform = tile;
    b.opacity = opacity;
    *brush = b;
    base->brush = brush;
    return true;
}

// Returns true if a paint server's transform cannot be inverted, which displays nothing of it
// (css-transforms-1/#transform-function-lists).
bool collapses(const SvgResource& server) {
    PrAffine2 transform = server.kind == SvgResourceKind_Pattern ? server.pattern.transform : server.gradient.transform;
    PrAffine2 inverse{};
    return !prAffine2Invert(transform, &inverse);
}

// Returns true if a paint server maps a part of itself onto the bounding box of what it paints.
bool readsBox(const SvgResource& server) {
    if (server.kind == SvgResourceKind_Pattern)
        return !server.pattern.userSpace || (server.pattern.contentBoundingBox && !server.pattern.hasViewBox);
    return !server.gradient.userSpace;
}

// Sets `base` to paint with a paint that is not context paint, as svgPaintFor does.
bool paintWith(const SvgDrawer& d, const SvgPaint& paint, float opacity, PrRect box, PrDrawPaint* base, PrDrawBrush* brush) {
    base->brush = nullptr;
    PrColor color = paint.color;
    switch (paint.kind) {
        case SvgPaintKind_Color:
            break;
        case SvgPaintKind_Url: {
            const SvgResource* server = paint.server != kSvgResourceNone ? &d.tree->resources.data[paint.server] : nullptr;
            // A pattern inside its own tile is a reference in error, which paints the fallback.
            if (server != nullptr && server->kind == SvgResourceKind_Pattern && patternCycles(d, paint.server))
                server = nullptr;
            // A server in objectBoundingBox units is ignored on geometry whose bounding box has no width or height
            // (SVG2/coords.html#ObjectBoundingBoxUnits). Its paint takes the fallback, as a reference in error does.
            if (server != nullptr && readsBox(*server) && (!(prRectWidth(box) > 0) || !(prRectHeight(box) > 0)))
                server = nullptr;
            // A transform that collapses the plane displays nothing of the server, which paints the fallback.
            if (server != nullptr && collapses(*server))
                server = nullptr;
            if (server != nullptr && server->kind == SvgResourceKind_Pattern)
                return patternPaint(d, paint.server, opacity, box, base, brush);
            if (server != nullptr)
                return gradientPaint(d, server->gradient, opacity, box, base, brush);
            // A reference to no server that paints takes its fallback, which may be none.
            if (!paint.fallback)
                return false;
            break;
        }
        default:
            return false;
    }
    color.a *= opacity;
    base->color = color;
    return true;
}

} // namespace

void svgTreeDraw(const SvgTree* tree, PrVec2 viewport, const SvgDrawHost* PR_NULLABLE host, PrDrawList* list) {
    if (tree->nodes.count == 0)
        return;
    SvgTiles tiles{};
    SvgDrawer d{
        .tree = tree,
        .list = list,
        .viewport = viewport,
        .scratch = prPathCreate(),
        .space = prAffine2Identity(),
        .host = host != nullptr ? *host : SvgDrawHost{},
        .tiles = &tiles,
    };
    // An svg element is an isolated group (SVG2/render.html#ParentCompositing), which matters only to what blends with
    // what lies beneath it.
    bool isolates = tree->readsBackdrop && tree->nodes.data[0].kind == SvgNodeKind_Viewport;
    if (isolates) {
        PrDrawGroup group = prDrawGroupDefault();
        group.isolated = true;
        prDrawListPushGroup(list, &group);
    }
    drawNode(d, 0);
    if (isolates)
        prDrawListPopGroup(list);
    prPathDestroy(d.scratch);
    for (uint32_t i = 0; i < tiles.tiles.count; ++i)
        prDrawListRelease(tiles.tiles.data[i].list);
    tiles.tiles.reset();
}

void svgDrawNode(SvgDrawer& d, SvgNodeIndex index) {
    drawNode(d, index);
}

bool svgDrawClipCoverage(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrDrawList* list) {
    const SvgClipPath& clip = tree->resources.data[index].clipPath;
    if (clip.content == kSvgNodeNone || (clip.boundingBox && prRectIsEmpty(box)))
        return false;
    SvgTiles tiles{};
    SvgDrawer d{
        .tree = tree,
        .list = list,
        .viewport = viewport,
        .scratch = prPathCreate(),
        .space = prAffine2Identity(),
        .tiles = &tiles,
    };
    SvgDrawer inner = effectDrawer(d, index, list);
    inner.silhouette = true;
    PrAffine2 outer = pushTransform(inner, prAffine2Concat(clip.boundingBox ? unitOnto(box) : prAffine2Identity(),
        clip.transform));
    drawNode(inner, clip.content);
    popTransform(inner, outer);
    prPathDestroy(d.scratch);
    for (uint32_t i = 0; i < tiles.tiles.count; ++i)
        prDrawListRelease(tiles.tiles.data[i].list);
    tiles.tiles.reset();
    return !prRectIsEmpty(prDrawListBounds(list));
}

bool svgDrawMaskContent(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrDrawList* list,
        PrRect* area, PrDrawMaskMode* mode, PrColorSpace* space) {
    SvgTiles tiles{};
    SvgDrawer d{
        .tree = tree,
        .list = list,
        .viewport = viewport,
        .scratch = prPathCreate(),
        .space = prAffine2Identity(),
        .tiles = &tiles,
    };
    LayerMask layer{.mode = PrDrawMaskMode_Luminance, .space = PrColorSpace_Srgb};
    LayerResult result = maskElementLayer(d, index, box, &layer);
    if (layer.list != nullptr) {
        prDrawListEmbed(list, layer.list, PrDVec2{0, 0});
        prDrawListRelease(layer.list);
    }
    prPathDestroy(d.scratch);
    for (uint32_t i = 0; i < tiles.tiles.count; ++i)
        prDrawListRelease(tiles.tiles.data[i].list);
    tiles.tiles.reset();
    *area = layer.area;
    *mode = layer.mode;
    *space = layer.space;
    return result != LayerResult_Hides;
}

void svgDrawImage(SvgDrawer& d, const SvgImageShape& image) {
    drawImage(d, image);
}

PrRect svgEffectBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts) {
    return effectBoundsOf(d, index, parts);
}

bool svgEffectCycles(const SvgDrawer& d, SvgResourceIndex index) {
    return effectCycles(d, index);
}

SvgDrawer svgEffectDrawer(const SvgDrawer& d, SvgResourceIndex index, PrDrawList* list) {
    return effectDrawer(d, index, list);
}

void svgShapeCommands(const SvgNode& node, PrVec2 viewport, Array<PrPathDataCommand>* out) {
    auto add = [&](char letter, float a, float b) {
        out->push(PrPathDataCommand{.letter = letter, .arguments = {a, b}});
    };
    auto arc = [&](PrVec2 radii, float x, float y) {
        out->push(PrPathDataCommand{.letter = 'A', .arguments = {radii.x, radii.y, 0, 0, 1, x, y}});
    };
    switch (node.kind) {
        case SvgNodeKind_Rect: {
            PrRect r = rectOf(viewport, node.rect);
            if (!(prRectWidth(r) > 0) || !(prRectHeight(r) > 0))
                return;
            PrVec2 radii = cornerRadii(viewport, node.rect, r);
            bool round = radii.x > 0;
            add('M', r.left + radii.x, r.top);
            add('H', r.right - radii.x, 0);
            if (round)
                arc(radii, r.right, r.top + radii.y);
            add('V', r.bottom - radii.y, 0);
            if (round)
                arc(radii, r.right - radii.x, r.bottom);
            add('H', r.left + radii.x, 0);
            if (round)
                arc(radii, r.left, r.bottom - radii.y);
            add('V', r.top + radii.y, 0);
            if (round)
                arc(radii, r.left + radii.x, r.top);
            add('Z', 0, 0);
            break;
        }
        case SvgNodeKind_Ellipse: {
            PrVec2 center{
                resolveIn(viewport, node.ellipse.cx, SvgAxis_X),
                resolveIn(viewport, node.ellipse.cy, SvgAxis_Y),
            };
            PrVec2 radii = ellipseRadii(viewport, node.ellipse);
            if (!(radii.x > 0) || !(radii.y > 0))
                return;
            add('M', center.x + radii.x, center.y);
            arc(radii, center.x, center.y + radii.y);
            arc(radii, center.x - radii.x, center.y);
            arc(radii, center.x, center.y - radii.y);
            arc(radii, center.x + radii.x, center.y);
            add('Z', 0, 0);
            break;
        }
        case SvgNodeKind_Line:
            add('M', resolveIn(viewport, node.line.x1, SvgAxis_X), resolveIn(viewport, node.line.y1, SvgAxis_Y));
            add('L', resolveIn(viewport, node.line.x2, SvgAxis_X), resolveIn(viewport, node.line.y2, SvgAxis_Y));
            break;
        default:
            break;
    }
}

float svgPercentBase(const SvgDrawer& d, SvgAxis axis) {
    if (axis == SvgAxis_X)
        return d.viewport.x;
    if (axis == SvgAxis_Y)
        return d.viewport.y;
    return prF32Sqrt((d.viewport.x * d.viewport.x + d.viewport.y * d.viewport.y) / 2);
}

bool svgPaintFor(const SvgDrawer& d, const SvgPaint& paint, float opacity, PrRect box, PrDrawPaint* base, PrDrawBrush* brush) {
    if (paint.kind != SvgPaintKind_ContextFill && paint.kind != SvgPaintKind_ContextStroke)
        return paintWith(d, paint, opacity, box, base, brush);
    // Context paint with no context element paints nothing (SVG2/painting.html#context-paint).
    if (d.context == nullptr)
        return false;
    const SvgContextPaint& layer = paint.kind == SvgPaintKind_ContextFill ? d.context->fill : d.context->stroke;
    if (!paintWith(d, layer.paint, opacity, layer.box, base, brush))
        return false;
    if (base->brush == nullptr)
        return true;
    // A server paints in the context element's user space, onto its bounding box, which paintWith mapped it onto.
    PrAffine2 fromTree{};
    if (!prAffine2Invert(d.space, &fromTree))
        return false;
    brush->transform = prAffine2Concat(prAffine2Concat(brush->transform, layer.space), fromTree);
    return true;
}

bool svgStrokeFor(const SvgDrawer& d, const SvgPaints& paints, float length, float* dashes, PrDrawStroke* out) {
    return paints.stroke.kind != SvgPaintKind_None && svgStrokeShapeFor(d, paints, length, dashes, out);
}

bool svgStrokeShapeFor(const SvgDrawer& d, const SvgPaints& paints, float length, float* dashes, PrDrawStroke* out) {
    if (paints.strokeStyle == kArrayMissing)
        return false;
    const SvgStrokeStyle& style = d.tree->strokes.data[paints.strokeStyle];
    float diagonal = svgPercentBase(d, SvgAxis_Other);
    float width = svgLengthResolve(style.width, diagonal);
    if (!(width > 0))
        return false;
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.miterLimit = style.miterLimit;
    stroke.shape.join = style.join;
    stroke.shape.startCap = style.cap;
    stroke.shape.endCap = style.cap;
    stroke.shape.dashCap = style.cap;
    stroke.nonScaling = paints.nonScaling;
    // The dashes and their offset are measured in the author's length of the geometry: they scale by the geometry's
    // length over path-length (SVG2/paths.html#PathLengthAttribute). A path-length of zero scales by infinity, which
    // leaves a zero at zero and takes any other length past the end of the geometry.
    float scale = length >= 0 && paints.pathLength >= 0 ? length / paints.pathLength : 1;
    float beyond = length + width;
    auto scaled = [&](float value) {
        if (value == 0 || scale == 1)
            return value;
        float result = value * scale;
        return prF32IsFinite(result) ? result : value < 0 ? -beyond : beyond;
    };
    // Dashes that sum to zero stroke without dashes (SVG2/painting.html#StrokeDasharrayProperty).
    float total = 0;
    for (uint32_t i = 0; i < style.dashCount; ++i) {
        dashes[i] = scaled(prF32Max(svgLengthResolve(style.dashes[i], diagonal), 0));
        total += dashes[i];
    }
    if (total > 0) {
        stroke.shape.dashes = dashes;
        stroke.shape.dashCount = style.dashCount;
        stroke.shape.dashOffset = scaled(svgLengthResolve(style.dashOffset, diagonal));
    }
    *out = stroke;
    return true;
}

PrAffine2 svgNodeTransform(const SvgDrawer& d, const SvgNode& node) {
    return transformOf(d, node);
}

SvgViewportShape svgViewportFromNode(const SvgDrawer& d, SvgNodeIndex index) {
    SvgViewportShape v = d.tree->nodes.data[index].viewport;
    // The host's view replaces the root's values. The root is the tree's first node.
    const SvgRootView* view = d.host.view;
    if (view != nullptr && index == 0) {
        v.hasViewBox = view->hasViewBox;
        v.viewBox = view->viewBox;
        v.aspect = view->aspect;
        v.clips = view->clips;
    }
    return v;
}

bool svgPlaceViewport(const SvgDrawer& d, const SvgViewportShape& v, PrRect* area, PrAffine2* place) {
    return placeViewport(d, v, area, place);
}

PrVec2 svgViewportContentExtent(const SvgViewportShape& v, PrRect area) {
    return contentExtent(v, area);
}

PrRect svgGeometryBounds(const SvgDrawer& d, const SvgNode& node) {
    return geometryBounds(d, node);
}

PrRect svgNodeBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts) {
    PrRect bounds{};
    nodeBounds(d, index, parts, &bounds);
    return bounds;
}

void svgShapePath(const SvgDrawer& d, const SvgNode& node, PrPath* out) {
    if (node.kind == SvgNodeKind_Path) {
        prPathAddPath(out, node.path.path, prAffine2Identity());
        return;
    }
    Array<PrPathDataCommand> commands;
    svgShapeCommands(node, d.viewport, &commands);
    prPathAppendCommands(out, commands.data, commands.count);
    commands.reset();
}

float svgShapeLength(const SvgDrawer& d, const SvgNode& node) {
    PrPath* path = prPathCreate();
    svgShapePath(d, node, path);
    float length = lengthOf(path);
    prPathDestroy(path);
    return length;
}

bool svgClipShapePath(SvgDrawer& d, SvgNodeIndex index, const SvgEffects& effects, PrPath* out, PrPathFillRule* rule) {
    CssValueRun value = maskingValue(effects, PrCssProperty_ClipPath);
    PrRect box = referenceBoxOf(d, index, boxKeywordOf(value, PrCssKeyword_BorderBox));
    *rule = PrPathFillRule_NonZero;
    if (effects.clip == SvgClipKind_Box) {
        prPathAddRect(out, box);
        return true;
    }
    if (effects.clip != SvgClipKind_Shape)
        return false;
    // clip-rule does not apply to a basic shape, whose own fill rule does (css-masking-1/#the-clip-rule).
    for (uint32_t i = 0; i < value.count; i += value.values[i].next) {
        if (cssIsBasicShape(value, i))
            cssShapePath(value, i, box, out, rule);
    }
    return true;
}

PrRect svgImageRect(PrVec2 viewport, const SvgImageShape& image) {
    return imageRectOf(viewport, image);
}

} // namespace pr_impl
