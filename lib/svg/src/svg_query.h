// Queries of the SVG tree: hit testing, and where a node is drawn.
//
// SVG2 - https://svgwg.org/svg2-draft/interact.html#PointerEventsProp
// SVG2 - https://svgwg.org/svg2-draft/types.html#InterfaceSVGGraphicsElement

#pragma once

#include "pr/svg.h"

#include "css/src/css_presentation_svg.h"
#include "svg/src/svg_draw.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Hit tests `point` in the tree drawn in a viewport of `viewport`, with the root's view replaced by `view` if it is not
// null (SVG2/interact.html#PointerEventsProp). The point is in the space that the tree is drawn in. A node is hit over
// its fill, its stroke or its bounding box as its pointer-events says. Text is hit by its characters' cells, and an
// image and a foreignObject by their positioning rectangles. A clip path clips where content is hit. Masks, filters,
// opacity and markers do not change it. Writes up to `capacity` hits to `out`, topmost first, and returns their number.
uint32_t svgTreeHitTest(const SvgTree* tree, PrVec2 viewport, const SvgRootView* PR_NULLABLE view, PrVec2 point,
    CssPresentationSvgHit* PR_NULLABLE out, uint32_t capacity);

// Returns true if `point` is inside the clip path of the clipPath `index`, which clips an element whose object bounding
// box is `box`, with percentages of `viewport`. The point and the box are in the element's user space. The clip path is
// the union of its children's silhouettes, each clipped by its own clip-path, and the clipPath's own clip-path clips
// the union (css-masking-1/#ClipPathElement).
bool svgClipContains(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport, PrVec2 point);

// Where a node is drawn in a tree drawn in a viewport, and the transforms that SVGGraphicsElement reports.
struct SvgPlace {
    PrAffine2 toTree;           // from the node's user space to the space that the tree is drawn in

    // From the node's user space to the coordinates that its nearest viewport is placed in
    // (SVG2/types.html#__svg__SVGGraphicsElement__getCTM), or for the root, to the space that the tree is drawn in.
    PrAffine2 ctm;

    PrVec2 viewport;            // the viewport that the node's own percentages are of
};

// Writes where the node `index` is drawn in the tree drawn in a viewport of `viewport` to `out`. A node's user space is
// the space inside its transform, and for a node that establishes a viewport, the space of its content. Returns false
// if the node is not drawn, as inside a viewport of no area.
bool svgNodePlace(const SvgTree* tree, PrVec2 viewport, SvgNodeIndex index, SvgPlace* out);

// Returns the bounding box of the node `index` and its descendants in its user space, as `options` asks: the object
// bounding box, with the stroke bounding box if `stroke` is set and the bounds of the markers if `markers` is set, cut
// by the bounds of the node's clip path if `clipped` is set. `place` is where the node is drawn (svgNodePlace).
PrRect svgNodeBox(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrSvgBoundingBoxOptions options);

// Returns true if `point`, in the user space of a shape node, is in its interior by its fill-rule, or in its stroke's
// shape, with the stroke's width, joins, caps and dashes (SVG2/types.html#InterfaceSVGGeometryElement). A shape that
// paints no stroke and whose pointer-events reads none has no stroke shape. `place` is where the node is drawn.
bool svgShapeFillContains(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrVec2 point);
bool svgShapeStrokeContains(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, PrVec2 point);

// Returns the length of the equivalent path of a shape node in user units, and the point at `distance` along it,
// clamped to its ends (SVG2/types.html#__svg__SVGGeometryElement__getPointAtLength). `place` is where the node is
// drawn.
float svgShapeTotalLength(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index);
PrVec2 svgShapePointAtLength(const SvgTree* tree, const SvgPlace& place, SvgNodeIndex index, float distance);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
