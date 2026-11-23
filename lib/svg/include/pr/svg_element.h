// SVG elements: the interfaces of the elements in the SVG namespace
//
// SVG2 - https://svgwg.org/svg2-draft/
// svg-animations - https://svgwg.org/specs/animations/

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An element in the SVG namespace (SVG2/#InterfaceSVGElement).
typedef struct PrSvgElement PrSvgElement;

// ---------------------------------------------------------------------------------------------------------------------
// SVGElement : Element (SVG2)

// An SVGElement as each interface it inherits from.
PR_HANDLE_UPCAST(SvgElement, DomElement, Element)
PR_HANDLE_UPCAST(SvgElement, DomNode, Node)
PR_HANDLE_UPCAST(SvgElement, DomEventTarget, EventTarget)

// Returns `object` as an SVGElement, or null if it is not one.
PrSvgElement* PR_NULLABLE prSvgElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the nearest ancestor that is an svg element in the element's SVG document fragment, or null if there is none
// (SVG2/types.html#__svg__SVGElement__ownerSVGElement). The fragment ends at the first ancestor that is not an SVG
// element, and at a foreignObject, whose content is rendered in a context of its own
// (SVG2/embedded.html#ForeignObjectElement).
PrSvgElement* PR_NULLABLE prSvgElementOwnerSvgElement(PrSvgElement* svgElement);

// The magnification and panning transform of an outermost svg element, which scales its content by `currentScale` and
// then moves it by `currentTranslate` (SVG2/struct.html#__svg__SVGSVGElement__currentScale). The transform is the
// document's, so it draws only for an svg element that is the root of its document. Any element that is not outermost
// has a scale of 1 and no translation, and setting either does nothing.
float prSvgElementCurrentScale(const PrSvgElement* svg);
void prSvgElementSetCurrentScale(PrSvgElement* svg, float scale);
PrVec2 prSvgElementCurrentTranslate(const PrSvgElement* svg);
void prSvgElementSetCurrentTranslate(PrSvgElement* svg, PrVec2 translate);

// The interfaces of SVG elements (SVG2/idl.html), each the most specific that describes some element. An element that
// SVG does not define is an SVGElement. The abstract interfaces that these derive from, such as SVGGraphicsElement,
// SVGGeometryElement and SVGTextContentElement, are no element's most specific.
enum {
    PrSvgInterface_Element,              // SVGElement
    PrSvgInterface_Svg,                  // SVGSVGElement
    PrSvgInterface_G,                    // SVGGElement
    PrSvgInterface_Defs,                 // SVGDefsElement
    PrSvgInterface_Desc,                 // SVGDescElement
    PrSvgInterface_Metadata,             // SVGMetadataElement
    PrSvgInterface_Title,                // SVGTitleElement
    PrSvgInterface_Symbol,               // SVGSymbolElement
    PrSvgInterface_Use,                  // SVGUseElement
    PrSvgInterface_Switch,               // SVGSwitchElement
    PrSvgInterface_Style,                // SVGStyleElement
    PrSvgInterface_Path,                 // SVGPathElement
    PrSvgInterface_Rect,                 // SVGRectElement
    PrSvgInterface_Circle,               // SVGCircleElement
    PrSvgInterface_Ellipse,              // SVGEllipseElement
    PrSvgInterface_Line,                 // SVGLineElement
    PrSvgInterface_Polyline,             // SVGPolylineElement
    PrSvgInterface_Polygon,              // SVGPolygonElement
    PrSvgInterface_Text,                 // SVGTextElement
    PrSvgInterface_TSpan,                // SVGTSpanElement
    PrSvgInterface_TextPath,             // SVGTextPathElement
    PrSvgInterface_Image,                // SVGImageElement
    PrSvgInterface_ForeignObject,        // SVGForeignObjectElement
    PrSvgInterface_Marker,               // SVGMarkerElement
    PrSvgInterface_LinearGradient,       // SVGLinearGradientElement
    PrSvgInterface_RadialGradient,       // SVGRadialGradientElement
    PrSvgInterface_Stop,                 // SVGStopElement
    PrSvgInterface_Pattern,              // SVGPatternElement
    PrSvgInterface_Script,               // SVGScriptElement
    PrSvgInterface_A,                    // SVGAElement
    PrSvgInterface_View,                 // SVGViewElement
    PrSvgInterface_Filter,               // SVGFilterElement
    PrSvgInterface_FeBlend,              // SVGFEBlendElement
    PrSvgInterface_FeColorMatrix,        // SVGFEColorMatrixElement
    PrSvgInterface_FeComponentTransfer,  // SVGFEComponentTransferElement
    PrSvgInterface_FeComposite,          // SVGFECompositeElement
    PrSvgInterface_FeConvolveMatrix,     // SVGFEConvolveMatrixElement
    PrSvgInterface_FeDiffuseLighting,    // SVGFEDiffuseLightingElement
    PrSvgInterface_FeDisplacementMap,    // SVGFEDisplacementMapElement
    PrSvgInterface_FeDistantLight,       // SVGFEDistantLightElement
    PrSvgInterface_FeDropShadow,         // SVGFEDropShadowElement
    PrSvgInterface_FeFlood,              // SVGFEFloodElement
    PrSvgInterface_FeFuncA,              // SVGFEFuncAElement
    PrSvgInterface_FeFuncB,              // SVGFEFuncBElement
    PrSvgInterface_FeFuncG,              // SVGFEFuncGElement
    PrSvgInterface_FeFuncR,              // SVGFEFuncRElement
    PrSvgInterface_FeGaussianBlur,       // SVGFEGaussianBlurElement
    PrSvgInterface_FeImage,              // SVGFEImageElement
    PrSvgInterface_FeMerge,              // SVGFEMergeElement
    PrSvgInterface_FeMergeNode,          // SVGFEMergeNodeElement
    PrSvgInterface_FeMorphology,         // SVGFEMorphologyElement
    PrSvgInterface_FeOffset,             // SVGFEOffsetElement
    PrSvgInterface_FePointLight,         // SVGFEPointLightElement
    PrSvgInterface_FeSpecularLighting,   // SVGFESpecularLightingElement
    PrSvgInterface_FeSpotLight,          // SVGFESpotLightElement
    PrSvgInterface_FeTile,               // SVGFETileElement
    PrSvgInterface_FeTurbulence,         // SVGFETurbulenceElement
    PrSvgInterface_ClipPath,             // SVGClipPathElement
    PrSvgInterface_Mask,                 // SVGMaskElement
    PrSvgInterface_Animate,              // SVGAnimateElement
    PrSvgInterface_Set,                  // SVGSetElement
    PrSvgInterface_AnimateMotion,        // SVGAnimateMotionElement
    PrSvgInterface_AnimateTransform,     // SVGAnimateTransformElement
    PrSvgInterface_MPath,                // SVGMPathElement
};
typedef uint8_t PrSvgInterface;

// Returns the most specific interface of the element.
PrSvgInterface prSvgElementInterface(const PrSvgElement* element);

// Returns true if the interface derives from SVGGeometryElement: the interface of a shape.
bool prSvgInterfaceIsGeometry(PrSvgInterface interface);

// ---------------------------------------------------------------------------------------------------------------------
// SVGGraphicsElement : SVGElement (SVG2/types.html#InterfaceSVGGraphicsElement)

// The queries of SVGGraphicsElement measure the element as it is drawn: by the presentation of its document, brought up
// to date first, or as a document without a presentation draws its outermost svg element at its natural size. An
// element that no tree draws, as one that does not render, measures as empty.

// What getBBox includes (SVG2/coords.html#BoundingBoxes).
typedef struct PrSvgBoundingBoxOptions {
    bool fill;
    bool stroke;
    bool markers;
    bool clipped;
} PrSvgBoundingBoxOptions;

// Returns options of fill alone, as getBBox() with no options measures.
PrSvgBoundingBoxOptions prSvgBoundingBoxOptionsDefault(void);

// Returns the element's bounding box in its user space (SVG2/types.html#__svg__SVGGraphicsElement__getBBox), or an
// empty rectangle at the origin if no tree draws it. Null options mean the defaults.
PrDomRect prSvgGraphicsElementGetBBox(PrSvgElement* element, const PrSvgBoundingBoxOptions* PR_NULLABLE options);

// Writes the transform from the element's user space to the coordinates that its nearest viewport is placed in
// (SVG2/types.html#__svg__SVGGraphicsElement__getCTM), or for an outermost svg element, to its viewport's, to `out`.
// Returns false if no tree draws the element.
bool prSvgGraphicsElementGetCtm(PrSvgElement* element, PrDomMatrix* out);

// Writes the transform from the element's user space to viewport coordinates, through the transforms of its box and its
// ancestors' (SVG2/types.html#__svg__SVGGraphicsElement__getScreenCTM), to `out`: the affine transform that maps the
// user space's origin and unit vectors where they are drawn. Returns false if no presentation draws the element.
bool prSvgGraphicsElementGetScreenCtm(PrSvgElement* element, PrDomMatrix* out);

// ---------------------------------------------------------------------------------------------------------------------
// SVGGeometryElement : SVGGraphicsElement (SVG2/types.html#InterfaceSVGGeometryElement)
//
// A geometry element is a path or a basic shape. It is measured by its equivalent path in its user space, as
// SVGGraphicsElement's queries measure it. An element that no tree draws, such as one that is not displayed or not in
// the document, is measured as if it were drawn in its parent's user space.

// Writes whether `point`, in the element's user space, is inside its interior by its fill-rule, or inside the shape of
// its stroke, with its width, joins, caps, dashes and vector-effect, to `out`. A point whose coordinates are not finite
// is in neither.
PrStatus prSvgGeometryElementIsPointInFill(PrSvgElement* element, PrDomPoint point, bool* out);
PrStatus prSvgGeometryElementIsPointInStroke(PrSvgElement* element, PrDomPoint point, bool* out);

// Returns the length of the element's equivalent path in user units. An empty path has a length of 0.
float prSvgGeometryElementTotalLength(PrSvgElement* element);

// Writes the point at `distance` along the element's equivalent path, clamped to its ends, to `out`. Returns
// InvalidState if the path is empty.
PrStatus prSvgGeometryElementGetPointAtLength(PrSvgElement* element, float distance, PrDomPoint* out);

// ---------------------------------------------------------------------------------------------------------------------
// SVGTextContentElement : SVGGraphicsElement (SVG2/text.html#InterfaceSVGTextContentElement)
//
// A text content element is a text, tspan or textPath element. Its methods measure its addressable characters as the
// text element that holds it is laid out: by the presentation of the document, brought up to date first, or as a
// document without a presentation draws at its natural size. A character is a UTF-16 code unit, and the characters of a
// text that does not render, or of an element that is not in a text, are none.

// Returns the number of characters.
uint32_t prSvgTextContentElementGetNumberOfChars(PrSvgElement* element);

// Returns the sum of the characters' advances, with letter and word spacing.
float prSvgTextContentElementGetComputedTextLength(PrSvgElement* element);

// Writes the sum of the advances of `count` characters from `first`, or of those up to the last character, to `out`.
// Returns OutOfRange if `first` is not a character's index.
PrStatus prSvgTextContentElementGetSubStringLength(PrSvgElement* element, uint32_t first, uint32_t count, float* out);

// Writes the point on the baseline where a character's glyph starts, or where it ends, one advance from the start in
// the glyph's direction, to `out`. Returns OutOfRange if `index` is not a character's.
PrStatus prSvgTextContentElementGetStartPositionOfChar(PrSvgElement* element, uint32_t index, PrDomPoint* out);
PrStatus prSvgTextContentElementGetEndPositionOfChar(PrSvgElement* element, uint32_t index, PrDomPoint* out);

// Writes the bounds of a character's cell to `out`: its advance along the baseline, by the ascent and descent of its
// font, turned by its rotation. Returns OutOfRange if `index` is not a character's.
PrStatus prSvgTextContentElementGetExtentOfChar(PrSvgElement* element, uint32_t index, PrDomRect* out);

// Writes a character's rotation in degrees to `out`. Returns OutOfRange if `index` is not a character's.
PrStatus prSvgTextContentElementGetRotationOfChar(PrSvgElement* element, uint32_t index, float* out);

// Returns the index of the first character whose cell holds `point`, which is in the text's user space, or -1 if none
// does.
int32_t prSvgTextContentElementGetCharNumAtPosition(PrSvgElement* element, PrDomPoint point);

// Selects the `count` characters from `index` (SVG2/text.html#__svg__SVGTextContentElement__selectSubString), or those
// to the end of the element's characters, as the document's selection. The characters are counted in UTF-16 code units
// of the element's text, in tree order. Returns OutOfRange, and selects nothing, if `index` is past the last character.
PrStatus prSvgTextContentElementSelectSubString(PrSvgElement* element, uint32_t index, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement : SVGGraphicsElement, the time container (svg-animations/#InterfaceSVGSVGElement)
//
// Each outermost svg element is the time container of the animation elements inside it. Its time, in seconds, starts at
// zero when the document's animations first update after it appears. These calls act on the container of the outermost
// svg element that holds `svgElement`.

// Pauses the container at its current time, or runs it again from that time. A paused container's time stays put.
void prSvgSvgElementPauseAnimations(PrSvgElement* svgElement);
void prSvgSvgElementUnpauseAnimations(PrSvgElement* svgElement);

// Returns true if the container is paused.
bool prSvgSvgElementAnimationsPaused(PrSvgElement* svgElement);

// Returns the container's current time.
float prSvgSvgElementGetCurrentTime(PrSvgElement* svgElement);

// Seeks the container to `seconds`, a negative time being zero, and samples its animations there at once, so that the
// values that they give read back at the new time. A seek queues no repeat event.
void prSvgSvgElementSetCurrentTime(PrSvgElement* svgElement, float seconds);

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement's hit queries (SVG2/struct.html#__svg__SVGSVGElement__getIntersectionList)
//
// A rectangle is in the user space of `svgElement`. A graphics element hits it by the area over which pointer events
// hit the element, cut by its clips: the area overlaps the rectangle, or for an enclosure, lies within it. The query
// descends into svg and g elements, and into the shadow tree of a use element, which hits the rectangle in place of its
// instances if any of them hits it. An element that no tree draws hits no rectangle.

// Returns true if every graphics element that `element` is or holds through svg and g elements hits `rect`, and there
// is at least one (checkIntersection, checkEnclosure).
bool prSvgSvgElementCheckHit(PrSvgElement* svgElement, PrSvgElement* element, PrDomRect rect, bool enclosure);

// Makes `out` a snapshot of the elements of `svgElement` that hit `rect`, in tree order, restricted to descendants of
// `reference` if it is not null (getIntersectionList, getEnclosureList).
void prSvgSvgElementGetHitList(PrSvgElement* svgElement, PrDomRect rect, PrSvgElement* PR_NULLABLE reference,
    bool enclosure, PrDomNodeList* out);

// ---------------------------------------------------------------------------------------------------------------------
// SVGAnimationElement : SVGElement (svg-animations/#InterfaceSVGAnimationElement)
//
// An animate, set, animateMotion or animateTransform element. Its times are in seconds of its time container.

// Returns the element that the animation animates, or null if it has none that is an SVG element
// (svg-animations/#__svg__SVGAnimationElement__targetElement).
PrSvgElement* PR_NULLABLE prSvgAnimationElementTargetElement(PrSvgElement* animation);

// Writes the begin time of the animation's current interval to `out`. Returns InvalidState if it has none.
PrStatus prSvgAnimationElementGetStartTime(PrSvgElement* animation, float* out);

// Returns the current time of the animation's container.
float prSvgAnimationElementGetCurrentTime(PrSvgElement* animation);

// Writes the animation's simple duration to `out`. Returns NotSupported if it is indefinite.
PrStatus prSvgAnimationElementGetSimpleDuration(PrSvgElement* animation, float* out);

// Adds a begin or an end time `offset` seconds after the container's current time, as beginElementAt and endElementAt
// do, and samples the animations at once.
void prSvgAnimationElementBeginElementAt(PrSvgElement* animation, float offset);
void prSvgAnimationElementEndElementAt(PrSvgElement* animation, float offset);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
