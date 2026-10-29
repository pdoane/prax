// The SVG tree: what an svg element's content draws, built from its elements and their computed styles.
//
// SVG2 - https://svgwg.org/svg2-draft/render.html
//
// A tree holds a node for each element that renders, with its geometry, its resolved paints, its transform and its
// effects, and a resource for each paint server, marker, clipPath, mask and filter that a node references. Building
// reads the document and the styles, on the document's thread. Drawing reads only the tree, so it may run on another
// thread at a size that is known only then, such as the extent that layout gives an outermost svg element's box.
// Lengths that a viewport's size decides stay unresolved until the tree is drawn.
//
// A tree is updated in place as its document changes. Changes are noted as they happen (svgTreeNoteChange), and the
// next update builds again only the nodes and resources that they reach (svgTreeUpdate). Each element of the document
// that renders has a node of its own, which the tree finds by element, and a node keeps its index while it lives. The
// references that a build follows, through hrefs and URLs, are recorded with what depends on them, so that a change to
// what an ID names, or within the element that it names, reaches the nodes and resources that read it.

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/draw.h"
#include "pr/draw_filter.h"
#include "pr/draw_list.h"
#include "pr/hash_index.h"
#include "pr/path.h"
#include "pr/string_view.h"
#include "pr/svg_render.h"
#include "pr/text_buffer.h"

#include "css/src/css_filter.h"
#include "css/src/css_presentation_svg.h"
#include "css/src/css_selector.h"
#include "css/src/css_style.h"
#include "dom/src/dom_node_interfaces.h"
#include "svg/src/svg_parse.h"
#include "svg/src/svg_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct SvgCascade;
struct SvgImage;
struct SvgText;
struct SvgTextFonts;

// The index of a node of a tree, and the null index.
typedef uint32_t SvgNodeIndex;
constexpr SvgNodeIndex kSvgNodeNone = 0xffffffffu;

// The index of a resource of a tree, and the null index for a reference that names no resource that the tree draws.
typedef uint32_t SvgResourceIndex;
constexpr SvgResourceIndex kSvgResourceNone = 0xffffffffu;

// The kinds of node.
enum {
    SvgNodeKind_Group,          // children drawn in order: g, a, switch, use, and an element drawn in place
    SvgNodeKind_Viewport,       // children in a new viewport: an svg element, or a symbol that a use instantiates
    SvgNodeKind_Rect,
    SvgNodeKind_Ellipse,        // circle or ellipse
    SvgNodeKind_Line,
    SvgNodeKind_Path,           // path, polyline or polygon
    SvgNodeKind_Text,
    SvgNodeKind_Image,
    SvgNodeKind_ForeignObject,
    SvgNodeKind_Free,           // a slot of the tree's free list, in no node's children
};
typedef uint8_t SvgNodeKind;

// A fill or a stroke as a node paints it: a color, or a paint server with the color that paints if the server cannot.
struct SvgPaint {
    SvgPaintKind kind;
    bool fallback;              // Url: true if `color` paints in place of a server that cannot paint
    PrColor color;              // Color, or the fallback of Url
    SvgResourceIndex server;    // Url, or kSvgResourceNone if the reference names no paint server that the tree draws
};

// How a shape or a run of text is filled and stroked.
struct SvgPaints {
    SvgPaint fill;
    SvgPaint stroke;
    float fillOpacity;
    float strokeOpacity;

    // An index into SvgTree::strokes, or kArrayMissing for a paint that strokes nothing and whose pointer-events does
    // not read the stroke's shape.
    uint32_t strokeStyle;

    bool crisp;                 // shape-rendering asks for aliased edges
    bool nonScaling;            // vector-effect: non-scaling-stroke

    // The author's length of a shape's geometry in user units (SVG2/paths.html#PathLengthProperty), against which the
    // shape's dashes are measured, or below zero for none.
    float pathLength;

    SvgLayer order[3];          // the layers in the order that paint-order paints them
};

// A vertex of a shape's equivalent path, where a vertex marker is placed (SVG2/painting.html#VertexMarkerProperties).
struct SvgVertex {
    PrVec2 point;

    // The direction that orient="auto" turns a marker to, in radians from the positive x-axis
    // (SVG2/painting.html#RenderingMarkers).
    float angle;
};

// The vertex markers of a shape, each a resource or kSvgResourceNone.
struct SvgMarkers {
    SvgResourceIndex start;
    SvgResourceIndex mid;
    SvgResourceIndex end;

    // The unit of markerUnits="strokeWidth", whose percentage is of the normalized diagonal.
    SvgLengthPercentage strokeWidth;

    // The vertices of a path, a polyline or a polygon, whose positions the build knows. Other shapes' vertices depend
    // on lengths that resolve when the tree is drawn.
    Array<SvgVertex> vertices;
};

// The limits that min-width, max-width, min-height and max-height put on the used width and height of a rect, an image
// or a foreignObject (SVG2/geometry.html#Sizing), each of x and then y. A minimum of auto is zero. A maximum applies on
// each axis whose `bounded` is set, and none leaves it unset. A minimum wins over a maximum, as in CSS
// (CSS2/visudet.html#min-max-widths).
struct SvgSizeLimits {
    SvgLengthPercentage min[2];
    SvgLengthPercentage max[2];
    bool bounded[2];
};

// A rect's geometry (SVG2/shapes.html#RectElement). A corner radius that is auto takes the other's value.
struct SvgRectShape {
    SvgLengthPercentage x, y, width, height, rx, ry;
    bool rxAuto, ryAuto;
    SvgSizeLimits limits;
};

// A circle's or an ellipse's geometry. A radius that is auto takes the other's value.
struct SvgEllipseShape {
    SvgLengthPercentage cx, cy, rx, ry;
    bool rxAuto, ryAuto;
    bool circle;                // rx is the circle's r, a percentage of the viewport's normalized diagonal
};

// A line's geometry.
struct SvgLineShape {
    SvgLengthPercentage x1, y1, x2, y2;
};

// A viewport that a node establishes: its rectangle in the parent's viewport, and how its viewBox maps into it
// (SVG2/coords.html#EstablishingANewSVGViewport).
struct SvgViewportShape {
    SvgLengthPercentage x, y, width, height;
    PrRect viewBox;             // used if `hasViewBox` is set
    SvgAspect aspect;
    bool hasViewBox;
    bool clips;                 // overflow other than visible clips the content to the viewport
    bool fills;                 // the viewport is the whole of the parent's, as an outermost svg element's box is

    // The point of a symbol's content that is placed at the viewport's x and y, in the content's user space, on each
    // axis whose flag is set (SVG2/struct.html#SymbolAttributes). A percentage is of the content's user space. The
    // viewport moves so that the point lands there, and on an axis without a reference point, it stays where x and y
    // put it.
    SvgLengthPercentage refX;
    SvgLengthPercentage refY;
    bool hasRefX;
    bool hasRefY;

    // The size of the viewport in user units, as far as the build could tell it without the size that the tree is drawn
    // at. Text built in the viewport resolves its percentages against it.
    PrVec2 userExtent;
};

// An image that an image element shows: a raster image, or an SVG image, which is drawn as a vector. The holder holds a
// reference to it.
struct SvgPicture {
    const PrDrawImage* PR_NULLABLE raster;  // arranged as its orientation displays it
    const SvgImage* PR_NULLABLE svg;
};

// An image element's positioning rectangle and what it shows there (SVG2/embedded.html#Placement). A width or a height
// that is auto takes its size from the image's natural dimensions.
struct SvgImageShape {
    SvgLengthPercentage x, y, width, height;
    bool widthAuto, heightAuto;
    SvgSizeLimits limits;
    bool clips;                 // overflow other than visible clips the image to the positioning rectangle
    SvgAspect aspect;           // preserveAspectRatio
    PrLayoutObjectFit fit;
    SvgLengthPercentage position[2]; // object-position: offsets of the object in the room that the rectangle leaves it
    PrDrawSampling sampling;         // how a raster image is resampled, as image-rendering asks
    SvgPicture picture;              // none until the image has loaded, or if it does not load
};

// A foreignObject's positioning rectangle and the layout root of its content (SVG2/embedded.html#ForeignObjectElement).
// Its content is laid out by the presentation's layout tree in a containing block of the rectangle's size in user
// units, and is drawn in the rectangle.
struct SvgForeignShape {
    SvgLengthPercentage x, y, width, height;
    SvgSizeLimits limits;
    bool clips;                 // overflow other than visible clips the content to the positioning rectangle
    PrLayoutNode root;          // the layout root of the content, or kPrLayoutNodeNone if no presentation lays it out

    // The size of the rectangle in user units, as the build resolved it in the nearest viewport, which the content's
    // containing block takes.
    PrVec2 extent;
};

// What an element's clip-path gives (css-masking-1/#the-clip-path).
enum {
    SvgClipKind_None,
    SvgClipKind_Url,            // a clipPath element
    SvgClipKind_Shape,          // a basic shape in a reference box
    SvgClipKind_Box,            // the edges of a reference box
};
typedef uint8_t SvgClipKind;

// What a layer of an element's mask-image is (css-masking-1/#the-mask-image).
enum {
    SvgMaskLayerKind_None,      // none, a transparent black layer
    SvgMaskLayerKind_Error,     // a reference to nothing that masks, or an image that cannot be displayed: ignored
    SvgMaskLayerKind_Mask,      // a mask element
    SvgMaskLayerKind_Image,     // an image that a URL names, ignored until it has loaded
    SvgMaskLayerKind_Gradient,
};
typedef uint8_t SvgMaskLayerKind;

// A layer of an element's mask.
struct SvgMaskLayer {
    SvgMaskLayerKind kind;
    SvgResourceIndex mask;      // Mask: the mask element
    SvgPicture picture;         // Image: none until the image has loaded; the layer holds a reference

    // Gradient: the gradient with its colors resolved, and its stops, which the layer owns.
    PrLayoutGradient gradient;
    PrLayoutColorStop* PR_NULLABLE stops;
};

// An item of an element's filter list (filter-effects-1/#FilterProperty): a filter function, or the filter element that
// a URL names.
struct SvgFilterItem {
    CssFilterFunction function;
    SvgResourceIndex filter;    // Url: the filter element
};

// An element's clip path, mask layers, filters and blending (css-masking-1, filter-effects-1, compositing-1), as its
// computed values give them, with the elements that their URLs name resolved. A node that does none of these has none.
struct SvgEffects {
    // The computed values of the compositing group, from which drawing reads the basic shape and the reference boxes of
    // clip-path and how the mask layers are placed and combined. The effects hold a reference.
    const CssStyleValues* values;

    SvgClipKind clip;
    SvgResourceIndex clipPath;  // Url: the clipPath element

    // The mask layers in the order that mask-image lists them, the top layer first, or none if mask-image is none.
    Array<SvgMaskLayer> layers;

    // The items of the filter list, in order. The list is empty if filter is none, or if a URL of it names no filter
    // element, which ignores the whole list (filter-effects-1/#FilterProperty).
    Array<SvgFilterItem> filters;

    // How the element's group composites onto what lies beneath: mix-blend-mode as a blend mode, or plus-lighter as an
    // operator (compositing-1/#mix-blend-mode).
    PrDrawBlendMode blend;
    PrDrawComposite composite;

    bool isolated;              // isolation is isolate, or the transform is 3D (compositing-1/#csscompositingrules_SVG)
};

// A node of the tree.
struct SvgNode {
    SvgNodeKind kind;
    bool visible;               // visibility: an invisible node paints nothing, but its children may
    PrPathFillRule fillRule;
    PrPathFillRule clipRule;    // how the node's geometry fills the clip path of a clipPath that holds it
    SvgLineFlags lines;         // the text decorations that the element declares, which text inside it draws
    float opacity;              // the group opacity of the node and its children

    // The node is a use element, whose paints are the context paint of its shadow tree
    // (SVG2/painting.html#TermContextElement).
    bool context;

    bool linear;                // color-interpolation is linearRGB: the node composites in linear light
    SvgPointerEvents pointerEvents;
    SvgVectorEffectFlags vectorEffects;

    // The tree's structure. A parent's children are a list from `first` through `next`, and back through `prev`.
    SvgNodeIndex parent;
    SvgNodeIndex first;
    SvgNodeIndex last;
    SvgNodeIndex next;
    SvgNodeIndex prev;

    // The element that the node draws, which hit testing and geometry queries report. A node is its element's own if
    // the element is in the document and the node is the first that the element built. The tree finds such a node by
    // its element (SvgTree::elements). Every other node is part of the nearest node that is its element's own: the
    // nodes of a use element's shadow tree, of the glyphs of an SVG font, and a use element's second node.
    const PrDomElement* element;
    bool own;

    uint32_t built;             // the update that built the node (SvgTree::update)
    uint32_t firstReference;    // the references that the node's build followed, a list through SvgReference::next

    // The node's transform, applied to it and its children. A transform whose functions or origin are relative to the
    // reference box is resolved at drawing from `transformStyle`, an index into SvgTree::transforms; otherwise it is
    // `transform`, and `transformStyle` is kArrayMissing.
    PrAffine2 transform;
    uint32_t transformStyle;

    SvgPaints paints;
    uint32_t markers;           // an index into SvgTree::markers, or kArrayMissing for a node without vertex markers
    uint32_t effects;           // an index into SvgTree::effects, or kArrayMissing for a node without effects

    union {
        SvgRectShape rect;
        SvgEllipseShape ellipse;
        SvgLineShape line;
        SvgViewportShape viewport;
        SvgImageShape image;
        SvgForeignShape foreign;
        struct {
            PrPath* path;       // the tree owns it
            PrRect bounds;      // the path's tight bounds
        } path;
        SvgText* text;          // the tree owns it
    };
};

// A gradient (SVG2/pservers.html#Gradients), with the attributes that it takes from the gradients that it references.
// Its coordinates are fractions of the bounding box of what it paints, or lengths in the user space of what it paints.
// Its stops' offsets are clamped and nondecreasing, and their colors are multiplied by their stop-opacity. The stops
// mix with their colors apart from their alpha.
struct SvgGradient {
    bool radial;
    bool userSpace;             // gradientUnits is userSpaceOnUse
    bool hasFx;
    bool hasFy;
    PrDrawExtend extend;
    PrColorSpace space;         // the space that the stops mix in, which the gradient's color-interpolation gives
    PrAffine2 transform;        // gradientTransform

    SvgLengthPercentage x1, y1, x2, y2;        // linear
    SvgLengthPercentage cx, cy, r, fx, fy, fr; // radial

    // The stops, which the gradient owns.
    PrDrawColorStop* PR_NULLABLE stops;
    uint32_t stopCount;
};

// How a marker is turned at its vertex (SVG2/painting.html#OrientAttribute).
enum {
    SvgOrient_Angle,
    SvgOrient_Auto,             // along the path
    SvgOrient_AutoStartReverse, // along the path, and against it at the start
};
typedef uint8_t SvgOrient;

// A marker (SVG2/painting.html#MarkerElement): the viewport that its content is fitted to, how the viewport is placed
// at a vertex, and the content.
struct SvgMarker {
    SvgLengthPercentage width;  // markerWidth
    SvgLengthPercentage height; // markerHeight
    PrRect viewBox;             // used if `hasViewBox` is set
    SvgAspect aspect;
    bool hasViewBox;
    bool clips;                 // overflow other than visible clips the content to the viewport
    bool strokeUnits;           // markerUnits is strokeWidth, which scales the marker by the shape's stroke width
    SvgOrient orient;
    float angle;                // Angle: the turn from the shape's positive x-axis, in radians

    // The point of the content that is placed at the vertex, in the content's user space. A percentage is of the
    // viewBox's width or height, or of the viewport's without a viewBox.
    SvgLengthPercentage refX;
    SvgLengthPercentage refY;

    // A group of the marker's children, which no node holds, or kSvgNodeNone. The children inherit from the marker
    // element, not from the shape that the marker is drawn on.
    SvgNodeIndex content;
};

// A pattern (SVG2/pservers.html#Patterns), with the attributes and the content that it takes from the patterns that it
// references. Its tile is a rectangle in the user space of what it paints, or in fractions of the bounding box of what
// it paints, and repeats across the plane. The pattern's transform then maps the tiles into that user space.
struct SvgPattern {
    bool userSpace;             // patternUnits is userSpaceOnUse
    bool contentBoundingBox;    // patternContentUnits is objectBoundingBox, which a viewBox overrides
    bool hasViewBox;
    SvgAspect aspect;
    PrRect viewBox;             // used if `hasViewBox` is set
    SvgLengthPercentage x, y, width, height;
    PrAffine2 transform;        // patternTransform, the transform property of the pattern that gives it

    // A group of the content, which no node holds, or kSvgNodeNone. The content inherits from the pattern that the
    // paint references, not from what it paints, and is clipped to the tile.
    SvgNodeIndex content;
};

// A clipPath element (css-masking-1/#ClipPathElement): the children whose silhouettes are united into its clip path, in
// the user space of what it clips or in fractions of that element's bounding box.
struct SvgClipPath {
    bool boundingBox;           // clipPathUnits is objectBoundingBox

    // The clipPath's transform, which maps the content, in the bounding box's units if it takes them, into the user
    // space of what it clips.
    PrAffine2 transform;

    // A group of the children that contribute to the clip path, which no node holds, or kSvgNodeNone for a clipPath
    // with no style. The group takes the clipPath's own clip-path, which the children's silhouettes are clipped to, and
    // no other effect. Each child keeps its geometry and its clip-rule as a node, so that hit testing can tell whether
    // a point is inside the clip path without drawing it. A child that does not render, or is not a shape, a text
    // element or a use element that references one directly, has no node.
    SvgNodeIndex content;
};

// A mask element (css-masking-1/#MaskElement): its region, and its content, drawn in the user space of what it masks or
// in fractions of that element's bounding box.
struct SvgMask {
    bool userSpace;             // maskUnits is userSpaceOnUse
    bool contentBoundingBox;    // maskContentUnits is objectBoundingBox
    SvgLengthPercentage x, y, width, height;
    PrDrawMaskMode mode;        // mask-type, which a mask-mode of match-source uses
    PrColorSpace space;         // the space that luminance is computed in, which the mask's color-interpolation gives

    // A group of the children, which no node holds, or kSvgNodeNone for a mask with no style. The children inherit from
    // the mask, not from what it masks. The group takes the mask element's own clip-path and mask, and not its opacity,
    // which does not apply to a mask.
    SvgNodeIndex content;
};

// An input of a filter primitive: the index of an earlier primitive of its filter, or one of the sources that it reads
// besides their results (filter-effects-1/#element-attrdef-filter-primitive-in).
enum {
    SvgFilterInput_Graphic = -1,            // SourceGraphic
    SvgFilterInput_Alpha = -2,              // SourceAlpha
    SvgFilterInput_Background = -3,         // BackgroundImage
    SvgFilterInput_BackgroundAlpha = -4,    // BackgroundAlpha
    SvgFilterInput_Fill = -5,               // FillPaint
    SvgFilterInput_Stroke = -6,             // StrokePaint
};
typedef int32_t SvgFilterInput;

// The kinds of filter primitive (filter-effects-1/#FilterPrimitivesOverview), and what a primitive in error becomes.
enum {
    SvgPrimitiveKind_Blend,
    SvgPrimitiveKind_ColorMatrix,
    SvgPrimitiveKind_ComponentTransfer,
    SvgPrimitiveKind_Composite,         // feComposite with a Porter-Duff operator
    SvgPrimitiveKind_Arithmetic,        // feComposite with the arithmetic operator
    SvgPrimitiveKind_ConvolveMatrix,
    SvgPrimitiveKind_Lighting,          // feDiffuseLighting or feSpecularLighting
    SvgPrimitiveKind_DisplacementMap,
    SvgPrimitiveKind_DropShadow,
    SvgPrimitiveKind_Flood,
    SvgPrimitiveKind_GaussianBlur,
    SvgPrimitiveKind_Image,
    SvgPrimitiveKind_Merge,
    SvgPrimitiveKind_Morphology,
    SvgPrimitiveKind_Offset,
    SvgPrimitiveKind_Tile,
    SvgPrimitiveKind_Turbulence,
    SvgPrimitiveKind_PassThrough,       // its first input, unchanged
    SvgPrimitiveKind_Transparent,       // transparent black
};
typedef uint8_t SvgPrimitiveKind;

// The attributes of a filter primitive's subregion that it gives (filter-effects-1/#FilterPrimitiveSubRegion).
enum {
    SvgSubregionFlag_X = 1u << 0,
    SvgSubregionFlag_Y = 1u << 1,
    SvgSubregionFlag_Width = 1u << 2,
    SvgSubregionFlag_Height = 1u << 3,
};
typedef uint8_t SvgSubregionFlags;

// A transfer function of feComponentTransfer (filter-effects-1/#feComponentTransferElement). The values of a table are
// a range of its filter's numbers.
struct SvgTransfer {
    PrDrawTransferKind kind;
    float slope;
    float intercept;
    float amplitude;
    float exponent;
    float offset;
    uint32_t first;
    uint32_t count;
};

// A filter primitive, with its attributes and the properties of its style. Lengths and positions are in the primitive
// units of its filter: user units, or fractions of the bounding box of what the filter applies to. Its kind decides
// which member of the union it uses.
struct SvgPrimitive {
    SvgPrimitiveKind kind;
    PrColorSpace space;             // the space that color-interpolation-filters gives
    SvgFilterInput inputs[2];

    // The subregion: each attribute that `given` holds, and the default for the others.
    SvgLengthPercentage x, y, width, height;
    SvgSubregionFlags given;

    union {
        PrDrawBlendMode blend;                          // Blend
        PrAffine4 matrix;                               // ColorMatrix
        SvgTransfer transfer[PrDrawChannel_Count];      // ComponentTransfer
        PrDrawComposite composite;                      // Composite
        float k[4];                                     // Arithmetic

        // ConvolveMatrix: the kernel, `order.x` columns by `order.y` rows of the filter's numbers from `first`.
        struct {
            PrUVec2 order;
            PrUVec2 target;
            uint32_t first;
            float divisor;              // zero for the kernel's sum
            float bias;
            PrImageEdgeMode edge;
            bool preserveAlpha;
        } convolve;

        // Lighting: the light's angles are in radians, and its positions are in primitive units.
        struct {
            PrDrawLightingKind kind;
            float surfaceScale;
            float constant;             // the diffuse or specular constant
            float specularExponent;
            PrColor color;              // lighting-color, with straight alpha
            PrDrawLight light;
        } lighting;

        // DisplacementMap: the channels of the second input that displace each axis.
        struct {
            float scale;
            PrDrawChannel x;
            PrDrawChannel y;
        } displacement;

        // DropShadow: the color is flood-color times flood-opacity, with straight alpha.
        struct {
            PrVec2 offset;
            PrVec2 sigma;
            PrColor color;
        } shadow;

        PrColor flood;                  // Flood: flood-color times flood-opacity, with straight alpha

        // GaussianBlur: a deviation below zero is zero.
        struct {
            PrVec2 sigma;
            PrImageEdgeMode edge;
        } blur;

        // Image: an image, or an element of the filter's content group, drawn as a use element draws it.
        struct {
            SvgPicture picture;         // the primitive holds a reference
            SvgNodeIndex node;          // kSvgNodeNone for an image
            SvgAspect aspect;           // preserveAspectRatio
            PrDrawSampling sampling;
        } image;

        // Merge: `count` of the filter's merge inputs, from `first`.
        struct {
            uint32_t first;
            uint32_t count;
        } merge;

        struct {
            PrVec2 radius;
            PrDrawMorphologyOp op;
        } morphology;                   // Morphology

        PrVec2 offset;                     // Offset
        PrDrawFilterTurbulence turbulence; // Turbulence
    };
};

// A filter element (filter-effects-1/#FilterElement): its region, in the user space of what it applies to or in
// fractions of that element's bounding box, and its primitives. Only the primary filter primitive tree, which ends at
// the last primitive, contributes. A filter without primitives makes what it applies to invisible.
struct SvgFilter {
    bool userSpace;                 // filterUnits is userSpaceOnUse
    bool primitiveBoundingBox;      // primitiveUnits is objectBoundingBox
    SvgLengthPercentage x, y, width, height;

    // The primitives in order, the numbers of their tables and kernels, and the inputs of their merges, which the
    // filter owns.
    SvgPrimitive* PR_NULLABLE primitives;
    uint32_t primitiveCount;
    float* PR_NULLABLE numbers;
    SvgFilterInput* PR_NULLABLE merges;

    // A group of the elements that feImage primitives draw, which no node holds, or kSvgNodeNone. Each element inherits
    // as it does in the document.
    SvgNodeIndex content;
};

// The kinds of resource.
enum {
    SvgResourceKind_Gradient,
    SvgResourceKind_Marker,
    SvgResourceKind_Pattern,
    SvgResourceKind_ClipPath,
    SvgResourceKind_Mask,
    SvgResourceKind_Filter,
};
typedef uint8_t SvgResourceKind;

// An element that the nodes of a tree reference: a paint server, a marker, a clipPath, a mask or a filter. A resource
// is built the first time that a node references its element, and is shared by every node that references it after
// that. It holds the data of its kind alone, which resetting it frees.
struct SvgResource {
    const PrDomElement* PR_NULLABLE element;    // the resource holds a reference; null for a free slot
    uint32_t firstReference;                    // the references that its build followed (SvgReference::next)
    SvgResourceKind kind;
    union {
        SvgGradient gradient;
        SvgMarker marker;
        SvgPattern pattern;
        SvgClipPath clipPath;
        SvgMask mask;
        SvgFilter filter;
    };
};

// A reference that a build followed: the ID that it named, the element that the ID named then, and the node or the
// resource that read it. The dependent is built again if the ID comes to name another element. A dependent that read
// the element's subtree, as a use element's shadow tree copies it, is built again on a change at or under the element
// too. A reference into another document names an element of a resource document (dom_resource.h), which is built again
// once that document loads.
struct SvgReference {
    PrString id;                            // the null string for the root of another document
    PrString document;                      // the other document's absolute URL, or the null string for none
    const PrDomElement* PR_NULLABLE target; // the reference holds a reference to it

    // The root of the tree whose IDs the ID names an element of: the document, or a shadow root
    // (css-shadow-1/#css-tree-scoped-reference). The reference holds a reference to it.
    const PrDomNode* PR_NULLABLE scope;

    SvgNodeIndex node;                      // the node that depends on it, or kSvgNodeNone for a resource
    SvgResourceIndex resource;              // the resource that depends on it, or kSvgResourceNone for a node
    bool content;                           // the dependent read the target's subtree
    uint32_t next;                          // the dependent's next reference, or kArrayMissing
};

// The nodes of an svg element's content and the resources that they draw with. An array whose items keep their indices
// has a list of the slots that are free, which new items take first.
struct SvgTree {
    Array<SvgNode> nodes;               // by SvgNodeIndex; the first is the root
    Array<SvgNodeIndex> freeNodes;
    HashIndex elements;                 // the nodes that are their elements' own, by element
    Array<SvgStrokeStyle> strokes;      // owned by the paints of nodes and text (SvgPaints::strokeStyle)
    Array<uint32_t> freeStrokes;
    Array<SvgTransformStyle> transforms; // owned by nodes (SvgNode::transformStyle)
    Array<uint32_t> freeTransforms;
    Array<SvgMarkers> markers;          // owned by nodes (SvgNode::markers)
    Array<uint32_t> freeMarkers;
    Array<SvgEffects> effects;          // owned by nodes (SvgNode::effects)
    Array<uint32_t> freeEffects;
    Array<SvgResource> resources;       // by SvgResourceIndex
    Array<SvgResourceIndex> freeResources;
    Array<SvgReference> references;
    Array<uint32_t> freeReferences;

    // The cascades that style the elements of the other documents that references name, one for each document, which
    // the tree owns. Such a document does not change, so each style is computed once (svg_cascade.h).
    Array<SvgCascade*> cascades;

    // A filter of the elements whose subtrees references read: a bit for each such element's hash. A change outside
    // them reaches no dependent.
    uint64_t watched;

    // The resources that the last sweep kept. Once twice as many live, a sweep frees those that no node references, so
    // that the resources of elements that come and go do not accumulate.
    uint32_t sweptResources;

    uint32_t update;                    // the number of builds and updates so far
    bool readsSvgFonts;                 // text was set in the document's SVG fonts, which it found by family

    // True if a node blends with what lies beneath it, or a filter reads what lies beneath as its background. Each
    // build and update sets it. The root, an outermost svg element, then draws as an isolated group
    // (SVG2/render.html#ParentCompositing), so that neither reaches past the svg element.
    bool readsBackdrop;
};

// Where a build takes the computed styles of elements from: the presentation of the document, or the cascade of a
// document that has no presentation.
struct SvgStyleSource {
    // Returns the computed style of an element of the document, or null if it has none yet. The source holds the
    // reference.
    const CssComputedStyle* PR_NULLABLE (*PR_NONNULL style)(void* PR_NULLABLE context, const PrDomElement* element);

    // Returns the style of an element of a use element's shadow tree, which inherits from `parent`. The caller holds
    // one reference.
    const CssComputedStyle* PR_NONNULL (*PR_NONNULL styleUnder)(void* PR_NULLABLE context,
                                                                const PrDomElement* element,
                                                                const CssComputedStyle* PR_NULLABLE parent);

    // Returns the style of the ::first-line or ::first-letter `pseudo` of an element, which inherits from `parent`, or
    // null if no rule styles it. The caller holds one reference.
    const CssComputedStyle* PR_NULLABLE (*PR_NONNULL pseudoUnder)(void* PR_NULLABLE context,
                                                                  const PrDomElement* element, CssPseudo pseudo,
                                                                  const CssComputedStyle* PR_NULLABLE parent);
    void* PR_NULLABLE context;
};

// Where a build takes the images that image elements name: the loads of the presentation of the document, or the host's
// loader.
struct SvgImageSource {
    // Writes the image that `url`, the href of the image element `element`, names to `out`, with a reference for the
    // caller. Returns false if no image is available: the reference loads nothing, or has not loaded yet. A load that
    // completes is a change at the element (SvgChange_Image).
    bool (*PR_NULLABLE load)(void* PR_NULLABLE context, const PrDomElement* element, PrStringView url,
                             SvgPicture* out);
    void* PR_NULLABLE context;
};

// Where a build takes the layout roots of the content of foreignObject elements: the presentation of the document,
// which lays each out as a root of its layout tree.
struct SvgForeignSource {
    // Returns the layout root of the content of the foreignObject `element`, or kPrLayoutNodeNone if it has none.
    PrLayoutNode (*PR_NULLABLE root)(void* PR_NULLABLE context, const PrDomElement* element);
    void* PR_NULLABLE context;
};

// What a build reads besides the document: its styles, its images, the layout roots of foreignObject elements, what
// colors resolve for, and what text is set in. An update reads the same as the build that it updates.
struct SvgBuild {
    SvgStyleSource source;
    SvgImageSource images;
    SvgForeignSource foreign;
    CssColorEnvironment environment;
    const PrSvgDrawOptions* options;    // the host's fonts, shaper and loader

    // The viewport that the root's content is laid out in, for the text whose positions are percentages of it.
    PrVec2 viewport;
};

// What the root of a tree is.
enum {
    SvgRoot_Outermost,          // an svg element in a viewport that its width and height size, as SVG sizes them
    SvgRoot_Box,                // an outermost svg element whose content box is the viewport, as CSS sizes it, and
                                // whose box applies its opacity and effects
    SvgRoot_Image,              // the outermost svg element of an SVG image, whose size is the viewport, and which
                                // applies its own opacity and effects, since no box does
    SvgRoot_InPlace,            // any element, drawn in the user space of its parent
};
typedef uint8_t SvgRoot;

// Empties the tree and builds it from `element` and its descendants, drawn as `root` says. Returns the number of nodes
// built.
uint32_t svgTreeBuild(SvgTree* tree, const PrDomElement* element, SvgRoot root, const SvgBuild& build);

// Frees what the tree holds.
void svgTreeReset(SvgTree* tree);

// Releases what the tree holds of its document: the elements that its references and servers name, and the copies that
// are use elements' shadow trees. The tree still draws, on any thread, and frees no node of the document when it is
// reset. It can no longer be updated or compared, and its nodes' elements are not to be read.
void svgTreeDetach(SvgTree* tree);

// Returns the node that is `element`'s own, or kSvgNodeNone if the element has none in the tree.
SvgNodeIndex svgTreeNodeFromElement(const SvgTree* tree, const PrDomElement* element);

// Reads the geometry of a basic shape or a path from its element and computed style: a rect's, a circle's, an ellipse's
// or a line's into `node`, or the commands of a path, a polyline or a polygon into `commands`, and sets the node's
// kind. Returns false for any other element, and for a path, a polyline or a polygon with no commands.
bool svgReadShape(const PrDomElement* element, const CssComputedStyle* style, SvgNode* node,
                  Array<PrPathDataCommand>* commands);

// ---------------------------------------------------------------------------------------------------------------------
// Updates

// What an update does to a node or a resource, in the order that an update does it.
enum {
    SvgWork_Remove,             // frees the node and its descendants
    SvgWork_Resource,           // builds the resource again
    SvgWork_Refresh,            // reads the node's own values again, or builds the node again if they do not suffice
    SvgWork_Children,           // builds the children that the node lacks, and frees those whose elements left
    SvgWork_Rebuild,            // builds the node and its descendants again
};
typedef uint8_t SvgWork;

// What changed at an element in the SVG namespace, as a tree hears of it.
enum {
    SvgChange_Style,            // the element's computed style
    SvgChange_Attribute,        // an attribute of the element
    SvgChange_Children,         // a child was inserted into the element or removed from it
    SvgChange_Text,             // the data of a text node among the element's children
    SvgChange_Removing,         // the element is about to be removed from its parent
    SvgChange_Image,            // an image that the element names has loaded or failed to load
    SvgChange_Box,              // the element's box was made or erased, as a foreignObject's content box is
};
typedef uint8_t SvgChange;

// A node or a resource that an update brings up to date.
struct SvgDirt {
    SvgWork work;
    uint32_t index;             // an SvgNodeIndex, or an SvgResourceIndex for Resource
};

// The changes noted since a tree's last update. Noting reads the tree but does not change it, so a change may be noted
// while another thread draws the tree.
struct SvgChanges {
    bool all;                   // the tree is built again
    bool ids;                   // what the document's IDs name may have changed
    Array<SvgDirt> dirt;
};

// Notes a change at `element`: the work on the nodes and resources whose references read it, and on its own node or its
// nearest ancestor's if `under` says that it is under the element that built the tree. `name` is the attribute's local
// name for Attribute. Returns true if the tree needs an update.
bool svgTreeNoteChange(const SvgTree* tree, const PrDomElement* element, SvgChange change,
                       PrStringView name,
                       bool under, SvgChanges* changes);

// Notes that what the document's IDs name may have changed, as it may when an element with an ID enters or leaves the
// document. Returns true if the tree needs an update.
bool svgTreeNoteIds(const SvgTree* tree, SvgChanges* changes);

// What an update did.
struct SvgUpdate {
    bool changed;               // the drawing may differ
    uint32_t built;             // the number of nodes built
};

// Brings the tree that `element` built as SvgRoot_Box up to date with the changes, and empties them.
SvgUpdate svgTreeUpdate(SvgTree* tree, const PrDomElement* element, SvgChanges* changes, const SvgBuild& build);

// Frees what the changes hold.
void svgChangesReset(SvgChanges* changes);

// ---------------------------------------------------------------------------------------------------------------------
// Resources that no node references
//
// A tree that holds no node may hold resources that elements outside SVG content reference, as an HTML box's effects
// reference a clipPath, a mask or a filter by URL. Its user decides when a resource is no longer referenced.

// Returns the resource of `kind` that `url`, a fragment in an effect of `from`, names in the tree of `from` or in its
// document: built the first time that it is named, and kSvgResourceNone if the URL names no element of that kind.
SvgResourceIndex svgTreeResourceFor(SvgTree* tree, const PrDomElement* from, PrStringView url, SvgResourceKind kind,
                                    const SvgBuild& build);

// Brings the resources of a tree that holds no node up to date with the changes, and empties them: every resource if
// all changed, and otherwise each resource whose references reach a change, or whose IDs name other elements of
// `document` than they did. Returns true if any was built again.
bool svgTreeUpdateResources(SvgTree* tree, const PrDomNode* document, SvgChanges* changes, const SvgBuild& build);

// Frees a resource of a tree that holds no node.
void svgTreeFreeResource(SvgTree* tree, SvgResourceIndex index);

// Returns true if the tree draws as `expected` does. Otherwise writes the first difference to `difference`, naming the
// element whose node differs. Only what the roots reach is compared, so slots and indices may differ.
bool svgTreeSame(const SvgTree* tree, const SvgTree* expected, TextBuffer* difference);

// ---------------------------------------------------------------------------------------------------------------------
// What building shares with text

// The state of a build. Text reads it to style its elements and resolve its paints.
struct SvgBuilder {
    SvgTree* tree;
    const SvgBuild* build;
    const PrDomNode* document;          // the document whose elements the build's style source styles
    uint32_t built;                     // the number of nodes built
    SvgTextFonts* PR_NULLABLE fonts;    // created when text first needs a font

    // The size of the nearest viewport in user units, where the build can tell it without the size that the tree is
    // drawn at. Text's percentages are of it.
    PrVec2 viewport;

    // What the references that the build follows depend on: the node or the resource being built. A node is the nearest
    // that is its element's own.
    SvgNodeIndex owner;
    SvgResourceIndex ownerResource;
};

// Returns true if the node that the build adds for an element is the element's own: the element is in the document, not
// in a use element's shadow tree, and is built where it renders, not as the content of a resource.
inline bool svgBuildsOwn(const SvgBuilder& b, bool instance) {
    return !instance && b.ownerResource == kSvgResourceNone;
}

// Adds `node` under `parent` as its last child, and returns its index. `own` says whether the node is its element's
// own, in which case the references that the build follows from here depend on it.
SvgNodeIndex svgBuildNode(SvgBuilder& b, const SvgNode& node, SvgNodeIndex parent, bool own);

// Returns how the style of `element` fills and strokes, with the servers that its URLs name in the element's document
// resolved. The paints own a stroke, which the node or the text that holds them frees (svgTreeFreeStroke).
SvgPaints svgBuildPaints(SvgBuilder& b, const CssComputedStyle* style, const PrDomElement* element);

// Reads the clip path, the mask layers, the filters and the blending that `style` gives the node at `index`, with the
// elements that their URLs name in the node's document resolved.
void svgBuildEffects(SvgBuilder& b, SvgNodeIndex index, const CssComputedStyle* style);

// Adds a group that no node holds, for the content of the resource whose element is `e`, and returns its index.
SvgNodeIndex svgBuildContentGroup(SvgBuilder& b, const PrDomElement* e);

// Builds `e`, an element of the document, under `parent`, with the style that it has in the document, and returns the
// node that it added, or kSvgNodeNone if it adds none.
SvgNodeIndex svgBuildElement(SvgBuilder& b, const PrDomElement* e, SvgNodeIndex parent);

// Frees the stroke that `paints` own, if any.
void svgTreeFreeStroke(SvgTree* tree, const SvgPaints& paints);

// Returns true if two paints paint the same, each with the strokes and servers of its own tree. Otherwise writes what
// differs to `difference`.
bool svgPaintsSame(const SvgTree* tree, const SvgPaints& paints, const SvgTree* expectedTree, const SvgPaints& expected,
                   TextBuffer* difference);

// Returns true if two paths have the same verbs and points.
bool svgPathSame(const PrPath* path, const PrPath* expected);

// Appends to `out` the vertex of each of `count` commands of path data, with the direction that orient="auto" turns a
// marker to there (SVG2/painting.html#RenderingMarkers): along the path at the ends of an open subpath, and half way
// between the directions into and out of the vertex elsewhere.
void svgMarkerVertices(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count, Array<SvgVertex>* out);

// Returns the element that `url` names, or null, and records the reference for what the build is building. A fragment
// names an element of the tree of `from`, or of its document if the tree is a shadow tree that has no element of that
// ID. A URL of another document names an element of that document by its fragment, or its root without one, once the
// document has loaded (SVG2/linking.html#processingURL). `content` is true if what is built reads the element's
// subtree, and not only which element the URL names.
const PrDomElement* PR_NULLABLE svgBuildReference(SvgBuilder& b, const PrDomElement* from, PrStringView url,
                                                  bool content);

// Returns the computed style of `e`, as svgBuildElement styles an element of the document and svgBuildInstanceChildren
// an element of a shadow tree that inherits from `parent`. An element of another document, which a reference names, is
// styled by a cascade of its own document. The tree or the source holds the reference that svgBuildStyle returns, and
// the caller holds the one that svgBuildStyleUnder returns.
const CssComputedStyle* PR_NULLABLE svgBuildStyle(SvgBuilder& b, const PrDomElement* e);
const CssComputedStyle* svgBuildStyleUnder(SvgBuilder& b, const PrDomElement* e,
                                           const CssComputedStyle* PR_NULLABLE parent);

// Returns the style of the ::first-line or ::first-letter `pseudo` of `e`, inheriting from `parent`, or null if no rule
// styles it (SvgStyleSource::pseudoUnder). The caller holds the reference.
const CssComputedStyle* PR_NULLABLE svgBuildPseudoStyleUnder(SvgBuilder& b, const PrDomElement* e, CssPseudo pseudo,
                                                             const CssComputedStyle* PR_NULLABLE parent);

// Returns the element that the href or xlink:href of `from` names, as svgBuildReference does.
const PrDomElement* PR_NULLABLE svgBuildHref(SvgBuilder& b, const PrDomElement* from, bool content);

// Parses a length in the units that SVG gives an attribute: a number in user units, a length with a CSS unit, or a
// percentage. A font-relative unit is of the style's font, or of the initial font size without a style. Returns false
// if `text` is not a length.
bool svgParseLength(PrStringView text, const CssComputedStyle* PR_NULLABLE style, SvgLengthPercentage* out);

// Builds the children of `element` as the elements of a shadow tree, which inherit from `style`, under `parent`.
void svgBuildInstanceChildren(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style,
                              SvgNodeIndex parent);

// Returns the length of an attribute, such as a line's x1 or a gradient's cx, in the units that SVG gives it: a number
// in user units, a length with a CSS unit, or a percentage. A font-relative unit is of the style's font. Returns
// `fallback` if the attribute is missing or does not parse.
SvgLengthPercentage svgAttributeLength(const PrDomElement* element, PrStringView name, SvgLengthPercentage fallback,
                                       const CssComputedStyle* PR_NULLABLE style);

// Returns the element's attribute `name` in no namespace, without surrounding white space, or the null view.
PrStringView svgAttribute(const PrDomElement* element, PrStringView name);

// Reads the limits that a style's min-width, max-width, min-height and max-height put on a size.
SvgSizeLimits svgStyleSizeLimits(const CssComputedStyle* style);

// Returns `extent` within `limits`, whose percentages are of `viewport`.
PrVec2 svgLimitExtent(const SvgSizeLimits& limits, PrVec2 extent, PrVec2 viewport);

// Returns true if `element` is in the SVG namespace and has `tag`.
bool svgIs(const PrDomElement* element, PrDomTag tag);

// Returns the element's href or xlink:href, or the null view if it has neither.
PrStringView svgHref(const PrDomElement* element);

// Returns the URL of the image that an image or feImage element shows, as its href gives it, or the null view if it
// shows none: if it has no href, or if it is an feImage whose href references an element of its document
// (filter-effects-1/#feImageElement).
PrStringView svgImageSource(const PrDomElement* element);

// Returns the element of `document` whose ID is `id`. SVG 2 has no xml:id.
const PrDomElement* PR_NULLABLE svgFindById(const PrDomNode* document, PrStringView id);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
