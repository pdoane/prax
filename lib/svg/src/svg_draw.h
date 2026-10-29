// The SVG tree: drawing it into a draw list, and the paints that shapes and text share.
//
// SVG2 - https://svgwg.org/svg2-draft/painting.html

#pragma once

#include "pr/draw.h"
#include "pr/draw_list.h"

#include "svg/src/svg_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How the root of a tree that is an svg element places its content in its viewport, in place of the values of its own
// attributes and style: the view that a fragment names, or an image element's (SVG2/embedded.html#ImageElement).
struct SvgRootView {
    PrRect viewBox;             // used if `hasViewBox` is set
    SvgAspect aspect;
    bool hasViewBox;
    bool clips;                 // the content is clipped to the viewport

    // A transform of the view, applied after the viewBox about the viewport's center, if `hasTransform` is set: the
    // transform() of svgView() (SVG2/linking.html#SVGFragmentIdentifiersDefinitions).
    PrAffine2 transform;
    bool hasTransform;
};

// What the host that draws a tree gives the drawing.
struct SvgDrawHost {
    // How the root places its content in place of its own values, or null for its own.
    const SvgRootView* PR_NULLABLE view;

    // Draws the layout root `root` of a foreignObject's content into `list`, from the top left of its containing block,
    // or null to draw no such content.
    void (*PR_NULLABLE drawRoot)(void* PR_NULLABLE context, PrLayoutNode root, PrDrawList* list);
    void* PR_NULLABLE context;
};

// Draws the tree into `list` in a viewport of `viewport`, whose top left is the origin of the list's current transform.
// Percentages resolve against the viewport. A null host gives the drawing nothing. The list's state is the same after
// the call as before.
void svgTreeDraw(const SvgTree* tree, PrVec2 viewport, const SvgDrawHost* PR_NULLABLE host, PrDrawList* list);

// A paint of the context element, which context-fill or context-stroke paints with, and the user space and bounding box
// that its server is drawn in (SVG2/painting.html#context-paint).
struct SvgContextPaint {
    SvgPaint paint;
    PrAffine2 space;            // the context element's user space to the space that the tree is drawn in
    PrRect box;
};

// The paints of a context element: a use element, or the shape that a marker is drawn on.
struct SvgContext {
    SvgContextPaint fill;
    SvgContextPaint stroke;
};

// The most markers that are drawn inside one another's content.
constexpr uint32_t kSvgMaxMarkerDepth = 8;

// The most patterns that are drawn inside one another's content.
constexpr uint32_t kSvgMaxPatternDepth = 8;

// The most clipPaths, masks and filters that are drawn inside one another's content.
constexpr uint32_t kSvgMaxEffectDepth = 8;

// A pattern's tile, drawn into a list of its own, which an image brush repeats.
struct SvgTile {
    SvgResourceIndex pattern;
    PrAffine2 content;          // the content's user space to the tile's
    PrAffine2 space;            // the tile's space to the space that the tree is drawn in
    PrVec2 extent;              // the tile's size
    PrVec2 viewport;            // the viewport that the content's percentages are of
    PrDrawList* list;           // the drawing holds a reference
};

// The tiles of the patterns that a drawing paints with, which it holds until it ends. A tile that the same pattern
// draws again in the same spaces is shared, so that the backend renders it once.
struct SvgTiles {
    Array<SvgTile> tiles;
};

// The state of a drawing: the list, the viewport that percentages are of, and the context element.
struct SvgDrawer {
    const SvgTree* tree;
    PrDrawList* list;
    PrVec2 viewport;
    PrPath* scratch;
    PrAffine2 space;            // the current user space to the space that the tree is drawn in
    const SvgContext* PR_NULLABLE context;
    SvgDrawHost host;

    // The markers whose content is being drawn, outermost first. A marker is not drawn inside its own content.
    SvgResourceIndex markers[kSvgMaxMarkerDepth];
    uint32_t markerDepth;

    // The patterns whose tiles are being drawn, outermost first. A pattern does not paint inside its own tile, where a
    // reference to it is in error and paints its fallback.
    SvgResourceIndex patterns[kSvgMaxPatternDepth];
    uint32_t patternDepth;

    // The clipPaths, masks and filters whose content is being drawn, outermost first. A reference to one of them from
    // inside its own content is in error, and is ignored.
    SvgResourceIndex effects[kSvgMaxEffectDepth];
    uint32_t effectDepth;

    // True while the drawing records the silhouettes of a clip path's content: the geometry of shapes and text, filled
    // opaque by their clip-rule, without their paints, markers, opacity or masks (css-masking-1/#ClipPathElement).
    bool silhouette;

    SvgTiles* tiles;
};

// Draws the node `index` with its transform and its effects.
void svgDrawNode(SvgDrawer& d, SvgNodeIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Resources that an element without a node draws with
//
// An HTML box takes a clipPath, a mask or a filter that a URL names from a tree's resources (css-masking-1,
// filter-effects-1). Each draws in the box's user space, whose origin is its border box's top left. `box` is the box's
// object bounding box. Percentages in user space are of `viewport`.

// Records into `list` the coverage of the clipPath `index`: its children's silhouettes, opaque where they clip in.
// Returns false if the clip path clips everything away.
bool svgDrawClipCoverage(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrDrawList* list);

// Records into `list` the content of the mask `index`, clipped to its region, and writes the region and how the content
// masks: by luminance or alpha, as its mask-type says, in the space that its color-interpolation gives. Returns false
// if the mask hides what it masks, as a region of no area does.
bool svgDrawMaskContent(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrDrawList* list,
                        PrRect* area, PrDrawMaskMode* mode, PrColorSpace* space);

// Draws an image element's image in its positioning rectangle (SVG2/embedded.html#ImageElement).
void svgDrawImage(SvgDrawer& d, const SvgImageShape& image);

// What the bounds of a node hold besides the geometry of the node and its descendants (SVG2/coords.html#BoundingBoxes):
// their stroke shapes, and their markers with the markers' strokes.
enum {
    SvgBoundsFlag_Stroke = 1u << 0,
    SvgBoundsFlag_Markers = 1u << 1,
};
typedef uint8_t SvgBoundsFlags;

// Returns the bounds of a node and its descendants with what `parts` adds, in the user space that its effects apply in:
// its own, or its content's for a node that establishes a viewport. Without parts, they are the object bounding box,
// and with the strokes, the stroke bounding box.
PrRect svgEffectBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts);

// Returns true if a clipPath, a mask or a filter cannot apply where it is drawn: inside its own content, or too deep in
// others'. Such a reference is in error.
bool svgEffectCycles(const SvgDrawer& d, SvgResourceIndex index);

// Returns a drawer that records the content of the clipPath, mask or filter `index` into `list`, in the drawer's user
// space, with no context element.
SvgDrawer svgEffectDrawer(const SvgDrawer& d, SvgResourceIndex index, PrDrawList* list);

// Appends the commands of the equivalent path of a rect, a circle, an ellipse or a line node (SVG2/shapes.html), with
// its lengths resolved in a viewport of `viewport`. A rect or an ellipse that renders nothing has none.
void svgShapeCommands(const SvgNode& node, PrVec2 viewport, Array<PrPathDataCommand>* out);

// Returns the length that a percentage along `axis` resolves against in the drawer's viewport.
float svgPercentBase(const SvgDrawer& d, SvgAxis axis);

// Sets `base` to paint with `paint` at `opacity`: its color, or `brush` filled with the paint server's brush. `box` is
// the bounds of the geometry painted, which a bounding-box server maps onto. Context paint paints with the drawer's
// context element, in its user space and onto its bounding box. Returns false if the paint paints nothing.
bool svgPaintFor(const SvgDrawer& d, const SvgPaint& paint, float opacity, PrRect box, PrDrawPaint* base,
                 PrDrawBrush* brush);

// Writes the stroke that `paints` strokes with, its lengths resolved, to `out`. `dashes` holds kSvgMaxDashes floats,
// which `out` refers to. `length` is the length of the geometry in user units, which the paints' path-length scales the
// dashes by, or below zero for geometry that path-length does not apply to. Returns false if the paints stroke nothing.
bool svgStrokeFor(const SvgDrawer& d, const SvgPaints& paints, float length, float* dashes, PrDrawStroke* out);

// Writes the shape of the stroke that `paints` give, as svgStrokeFor does, whether or not the stroke paints, as a hit
// test over the stroke asks. Returns false if the paints give no stroke shape.
bool svgStrokeShapeFor(const SvgDrawer& d, const SvgPaints& paints, float length, float* dashes, PrDrawStroke* out);

// ---------------------------------------------------------------------------------------------------------------------
// Geometry that drawing and queries share
//
// A node's user space is the space inside its own transform, and for a node that establishes a viewport, the space of
// its content. The drawer's viewport is the one that the node's percentages are of.

// Returns the node's transform, from its user space to its parent's, as drawing resolves it.
PrAffine2 svgNodeTransform(const SvgDrawer& d, const SvgNode& node);

// Returns the viewport that the node `index`, which establishes one, places its content in: its own, or for the root,
// what the host's view puts in its place.
SvgViewportShape svgViewportFromNode(const SvgDrawer& d, SvgNodeIndex index);

// Writes the rectangle of a viewport in its parent's user space to `area`, and the transform from the user space of its
// content to its parent's to `place` (SVG2/coords.html#ComputingAViewportsTransform). Returns false if the viewport
// renders nothing.
bool svgPlaceViewport(const SvgDrawer& d, const SvgViewportShape& v, PrRect* area, PrAffine2* place);

// Returns the size of the user space of a viewport's content, whose rectangle is `area`, which its content's
// percentages are of.
PrVec2 svgViewportContentExtent(const SvgViewportShape& v, PrRect area);

// Returns the bounds of a node's own geometry (SVG2/coords.html#ObjectBoundingBox), or empty bounds for a node without
// geometry of its own.
PrRect svgGeometryBounds(const SvgDrawer& d, const SvgNode& node);

// Returns the bounds of a node and its descendants with what `parts` adds (SVG2/coords.html#BoundingBoxes), in the
// space inside the node's transform. For a node that establishes a viewport, that space is outside the viewport's
// placement.
PrRect svgNodeBounds(SvgDrawer& d, SvgNodeIndex index, SvgBoundsFlags parts);

// Appends the equivalent path of a path, rect, ellipse or line node to `out`, in the node's user space.
void svgShapePath(const SvgDrawer& d, const SvgNode& node, PrPath* out);

// Returns the length of the equivalent path of a path, rect, ellipse or line node, in user units.
float svgShapeLength(const SvgDrawer& d, const SvgNode& node);

// Appends the outline that a clip-path of a basic shape or a reference box gives the node `index` to `out`, in the user
// space that its effects apply in, and writes its fill rule. Returns false if `effects` clip by neither.
bool svgClipShapePath(SvgDrawer& d, SvgNodeIndex index, const SvgEffects& effects, PrPath* out, PrPathFillRule* rule);

// Returns an image element's positioning rectangle in a viewport of `viewport` (SVG2/embedded.html#Placement).
PrRect svgImageRect(PrVec2 viewport, const SvgImageShape& image);

// Paints the shape that `draw` records, with the fill if `fills` is set, the stroke, and the markers that `markers`
// draws, in the order of the paints' paint-order. A drawing of silhouettes records the fill alone, opaque. `box` is the
// shape's geometry bounds, which a bounding-box paint server maps onto. `length` is the length of the geometry, as
// svgStrokeFor takes it. `draw` records the shape with the paint it is given.
template <typename Draw, typename Markers>
void svgPaintShape(const SvgDrawer& d, const SvgPaints& paints, bool fills, PrRect box, float length, Draw draw,
                   Markers markers) {
    PrDrawEdges edges = paints.crisp ? PrDrawEdges_Aliased : PrDrawEdges_Antialiased;
    if (d.silhouette) {
        if (fills)
            draw(PrDrawPaint{.color = prColor16FromColor(PrColor{1, 1, 1, 1}), .edges = edges});
        return;
    }
    for (SvgLayer layer : paints.order) {
        PrDrawBrush brush{};
        if (layer == SvgLayer_Fill) {
            PrDrawPaint fill{.edges = edges};
            if (fills && svgPaintFor(d, paints.fill, paints.fillOpacity, box, &fill, &brush))
                draw(fill);
        } else if (layer == SvgLayer_Stroke) {
            float dashes[kSvgMaxDashes];
            PrDrawStroke stroke{};
            PrDrawPaint outline{.edges = edges};
            if (svgStrokeFor(d, paints, length, dashes, &stroke)
                && svgPaintFor(d, paints.stroke, paints.strokeOpacity, box, &outline, &brush)) {
                outline.stroke = &stroke;
                draw(outline);
            }
        } else {
            markers();
        }
    }
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
