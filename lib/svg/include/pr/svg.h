// SVG elements
//
// SVG2 - https://svgwg.org/svg2-draft/

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"
#include "pr/status.h"
#include "pr/string_view.h"

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

// Returns the nearest ancestor that is an svg element, or null if there is none
// (SVG2/#__svg__SVGElement__ownerSVGElement).
PrSvgElement* PR_NULLABLE prSvgElementOwnerSvgElement(PrSvgElement* svgElement);

// The interfaces of SVG elements (SVG2/idl.html), each the most specific that describes some element. An interface
// derives from SVGGraphicsElement if a graphics element, a container that renders or a text content element has it, and
// from SVGElement otherwise. The shapes' interfaces derive from SVGGeometryElement, which no element has as its most
// specific.
enum {
    PrSvgInterface_Element,             // SVGElement
    PrSvgInterface_Graphics,            // SVGGraphicsElement
    PrSvgInterface_Rect,                // SVGRectElement
    PrSvgInterface_Circle,              // SVGCircleElement
    PrSvgInterface_Ellipse,             // SVGEllipseElement
    PrSvgInterface_Line,                // SVGLineElement
    PrSvgInterface_Path,                // SVGPathElement
    PrSvgInterface_Polyline,            // SVGPolylineElement
    PrSvgInterface_Polygon,             // SVGPolygonElement
    PrSvgInterface_Svg,                 // SVGSVGElement
    PrSvgInterface_TextContent,         // SVGTextContentElement
    PrSvgInterface_A,                   // SVGAElement
    PrSvgInterface_Use,                 // SVGUseElement
    PrSvgInterface_Switch,              // SVGSwitchElement
    PrSvgInterface_Symbol,              // SVGSymbolElement
    PrSvgInterface_Script,              // SVGScriptElement
    PrSvgInterface_View,                // SVGViewElement
    PrSvgInterface_Image,               // SVGImageElement
    PrSvgInterface_ForeignObject,       // SVGForeignObjectElement
    PrSvgInterface_Animate,             // SVGAnimateElement
    PrSvgInterface_Set,                 // SVGSetElement
    PrSvgInterface_AnimateMotion,       // SVGAnimateMotionElement
    PrSvgInterface_AnimateTransform,    // SVGAnimateTransformElement
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
// A geometry element is a path or a basic shape, and is measured by its equivalent path in its user space, as
// SVGGraphicsElement's queries measure it. A call returns InvalidState if no tree draws the element.

// Writes whether `point`, in the element's user space, is inside its interior by its fill-rule, or inside the shape of
// its stroke, with its width, joins, caps, dashes and vector-effect, to `out`. A point whose coordinates are not finite
// is in neither.
PrStatus prSvgGeometryElementIsPointInFill(PrSvgElement* element, PrDomPoint point, bool* out);
PrStatus prSvgGeometryElementIsPointInStroke(PrSvgElement* element, PrDomPoint point, bool* out);

// Writes the length of the element's equivalent path in user units to `out`.
PrStatus prSvgGeometryElementGetTotalLength(PrSvgElement* element, float* out);

// Writes the point at `distance` along the element's equivalent path, clamped to its ends, to `out`.
PrStatus prSvgGeometryElementGetPointAtLength(PrSvgElement* element, float distance, PrDomPoint* out);

// ---------------------------------------------------------------------------------------------------------------------
// SVGAnimatedString (SVG2/types.html#InterfaceSVGAnimatedString)

// A string attribute of an element, for the SVGAnimatedString members of its interface. Its animated value is its base
// value.
typedef struct PrSvgAnimatedString PrSvgAnimatedString;

// Returns the attribute `name` of the element as an animated string, or null if its interface has no such member: the
// href of a, use, image, script and the other elements that SVGURIReference describes, and the target of a. The caller
// frees it with prSvgAnimatedStringDestroy.
PrSvgAnimatedString* PR_NULLABLE prSvgAnimatedStringCreate(PrSvgElement* element, PrStringView name);

// Frees the animated string.
void prSvgAnimatedStringDestroy(PrSvgAnimatedString* PR_NULLABLE string);

// Returns the attribute's value, or the empty string if it is missing. An href without a namespace is preferred, and
// the href in the XLink namespace is read if there is none (SVG2/linking.html#XLinkRefAttrs).
PrStringView prSvgAnimatedStringBaseVal(const PrSvgAnimatedString* animated);

// Sets the attribute without a namespace.
void prSvgAnimatedStringSetBaseVal(PrSvgAnimatedString* animated, PrStringView value);

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

// Gives the document SVG: its elements' presentation attributes style them (SVG2/styling.html#PresentationAttributes).
// A document that the DOM creates from it, such as by DOMImplementation, has SVG too. Gives the document CSS first
// (prCssEnable), and MathML, which SVG content holds through foreignObject (prMathMlEnable). Does nothing for a
// document that has SVG.
void prSvgEnable(PrDomDocument* document);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
