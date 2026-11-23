// The values of SVG's attributes: the lengths, angles, transforms, lists and other types that SVG elements read their
// attributes as, and the typed reads and writes of an element's attributes
//
// SVG2 - https://svgwg.org/svg2-draft/types.html

#pragma once

#include "pr/base.h"
#include "pr/css.h"
#include "pr/dom_geometry.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/svg_attribute.h"
#include "pr/svg_element.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Values

// A length: a number and a CSS unit (SVG2/types.html#InterfaceSVGLength). The unit is PrCssUnit_Number for user units,
// PrCssUnit_Percent, or any CSS length unit. It is PrCssUnit_Unknown for a CSS value that no unit expresses, such as
// var() or calc(), and the number is then 0.
typedef struct PrSvgLength {
    float value;
    PrCssUnit unit;
} PrSvgLength;

// What a length resolves against: the length of 100%, the font size for em and ex, and the element whose style resolves
// the other font-relative and viewport-relative units. With a null element, those units resolve to zero.
typedef struct PrSvgLengthBasis {
    float percent;
    float fontSize;
    const PrDomElement* PR_NULLABLE element;
} PrSvgLengthBasis;

// Returns the basis of a length that has no element: 100% is 100 user units, and the font size is the initial one.
PrSvgLengthBasis prSvgLengthBasisDefault(void);

// Returns the length in user units.
float prSvgLengthResolve(PrSvgLength length, PrSvgLengthBasis basis);

// Returns the length in `unit`, with the same length in user units, or a length of 0 if a unit of `unit` resolves to 0
// or is not a length's.
PrSvgLength prSvgLengthConvert(PrSvgLength length, PrCssUnit unit, PrSvgLengthBasis basis);

// Parses `text` as a length: a number, with a unit or a percent sign, around which white space is allowed. Returns
// false if it is not one.
bool prSvgLengthParse(PrStringView text, PrSvgLength* out);

// Returns the length as text (SVG2/types.html#__svg__SVGLength__valueAsString). The caller releases it.
PrRawString prSvgLengthSerialize(PrSvgLength length);

// Returns true if `text` is a CSS function, such as var() or calc(). A length holds one as a value that no unit
// expresses.
bool prSvgLengthIsCssFunction(PrStringView text);

// An angle: a number of a unit (SVG2/types.html#InterfaceSVGAngle). Its unit is PrCssUnit_Number for a number in
// degrees, or an angle unit.
typedef struct PrSvgAngle {
    float value;
    PrCssUnit unit;
} PrSvgAngle;

// Returns the angle in degrees.
float prSvgAngleDegrees(PrSvgAngle angle);

// Returns the angle in `unit`, or the angle itself if `unit` is not an angle's.
PrSvgAngle prSvgAngleConvert(PrSvgAngle angle, PrCssUnit unit);

// Parses `text` as an angle, or returns false if it is not one; and returns the angle as text. The caller releases the
// text.
bool prSvgAngleParse(PrStringView text, PrSvgAngle* out);
PrRawString prSvgAngleSerialize(PrSvgAngle angle);

// A function of a transform list (SVG2/coords.html#InterfaceSVGTransform), with the arguments that it is written with
// and those that it leaves out filled in: a, b, c, d, e and f of a matrix; tx and ty of a translate; sx and sy of a
// scale; the angle in degrees and the center of a rotate; and the angle in degrees of a skew.
typedef struct PrSvgTransform {
    PrSvgTransformType type;
    float values[6];
} PrSvgTransform;

// Returns the transform's matrix, which is 2D.
PrDomMatrix prSvgTransformMatrix(const PrSvgTransform* transform);

// Returns the angle of a rotate or a skew, or 0 for another function (SVG2/coords.html#__svg__SVGTransform__angle).
float prSvgTransformAngle(const PrSvgTransform* transform);

// Returns the matrix function of the 2D part of `matrix`.
PrSvgTransform prSvgTransformFromMatrix(PrDomMatrix matrix);

// The value of preserveAspectRatio (SVG2/coords.html#PreserveAspectRatioAttribute).
typedef struct PrSvgPreserveAspectRatio {
    PrSvgAlign align;
    PrSvgMeetOrSlice meetOrSlice;
} PrSvgPreserveAspectRatio;

// ---------------------------------------------------------------------------------------------------------------------
// The typed attributes of elements
//
// An element reads some of its attributes as values of a type: a rect's width as a length, a text's x as a list of
// lengths, a marker's orient as a keyword or an angle. A read of an attribute that is missing, or whose value does not
// parse as its type or is out of its range, gives the attribute's initial value. A write sets the attribute, in no
// namespace, to the value's text. A call on an attribute that the element does not read as that type reads nothing and
// returns false, or returns WrongType.
//
// The base value of an attribute is the element's own. Its animated value is what SVG's animations give it, or else its
// base value (SVG2/types.html#__svg__SVGAnimatedLength__animVal).

// The types that elements read attributes as.
enum {
    PrSvgValueType_None,                // the element does not read the attribute as a value
    PrSvgValueType_Boolean,             // the keyword true or false
    PrSvgValueType_Enumeration,         // a keyword of one of the enumerations of pr/svg_attribute.h
    PrSvgValueType_Integer,
    PrSvgValueType_IntegerPair,         // one or two integers, the second the same as the first if missing
    PrSvgValueType_Number,
    PrSvgValueType_NumberPair,          // one or two numbers, the second the same as the first if missing
    PrSvgValueType_String,
    PrSvgValueType_Length,
    PrSvgValueType_Orient,              // a marker's orient: a PrSvgOrient, and its angle
    PrSvgValueType_Rect,                // four numbers: x, y, width and height
    PrSvgValueType_PreserveAspectRatio,
    PrSvgValueType_NumberList,
    PrSvgValueType_LengthList,
    PrSvgValueType_TransformList,
    PrSvgValueType_PointList,
    PrSvgValueType_StringList,          // requiredExtensions, split by white space, and systemLanguage, by commas

    // The types of presentation attributes that are no element's typed attributes, which CSS reads as its properties'
    // values (pr/css.h), and which SVG's animations interpolate.
    PrSvgValueType_Color,
    PrSvgValueType_Path,                // path data
};
typedef uint8_t PrSvgValueType;

// Which value of an attribute a read gives.
enum {
    PrSvgLayer_Base,
    PrSvgLayer_Animated,
};
typedef uint8_t PrSvgLayer;

// Returns the type that the element reads the attribute as, or None.
PrSvgValueType prSvgElementAttributeType(const PrSvgElement* element, PrSvgAttribute attribute);

// Returns what the element's length attribute resolves against: the size of its viewport along the attribute's axis, or
// the diagonal for a length of neither axis, and its font size (SVG2/coords.html#Units). A length of an element that no
// tree draws resolves percentages against the viewport that its nearest drawn ancestor gives its children, or against 0
// if no ancestor is drawn.
PrSvgLengthBasis prSvgElementLengthBasis(const PrSvgElement* element, PrSvgAttribute attribute);

// Read and write a boolean, an enumeration, an integer, a number or a string. An enumeration's value is of the type of
// its attribute's keywords. A pair writes its second value only if it differs from the first.
bool prSvgElementGetBoolean(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, bool* out);
bool prSvgElementGetEnumeration(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, uint8_t* out);
bool prSvgElementGetInteger(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, int32_t* out);
bool prSvgElementGetIntegerPair(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    int32_t out[PR_NONNULL 2]);
bool prSvgElementGetNumber(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, float* out);
bool prSvgElementGetNumberPair(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    float out[PR_NONNULL 2]);
PrStatus prSvgElementSetBoolean(PrSvgElement* element, PrSvgAttribute attribute, bool value);
PrStatus prSvgElementSetInteger(PrSvgElement* element, PrSvgAttribute attribute, int32_t value);
PrStatus prSvgElementSetIntegerPair(PrSvgElement* element, PrSvgAttribute attribute, int32_t first, int32_t second);
PrStatus prSvgElementSetNumber(PrSvgElement* element, PrSvgAttribute attribute, float value);
PrStatus prSvgElementSetNumberPair(PrSvgElement* element, PrSvgAttribute attribute, float first, float second);

// Sets an enumeration to the keyword of `value`. Returns InvalidData, setting nothing, for a value that no keyword of
// the attribute spells. A marker's orient of PrSvgOrient_Angle keeps the angle that it has, or is 0.
PrStatus prSvgElementSetEnumeration(PrSvgElement* element, PrSvgAttribute attribute, uint8_t value);

// Returns a string attribute's value, or the empty string if it is missing. An href without a namespace is preferred,
// and the href in the XLink namespace is read if there is none (SVG2/linking.html#XLinkRefAttrs). The view lasts until
// the attribute changes, or for an animated value, until the animations sample again. Returns the null view if the
// element does not read the attribute as a string.
PrStringView prSvgElementGetString(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer);
PrStatus prSvgElementSetString(PrSvgElement* element, PrSvgAttribute attribute, PrStringView value);

// Read and write a length, and the angle of a marker's orient, which is 0 for a keyword.
bool prSvgElementGetLength(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgLength* out);
PrStatus prSvgElementSetLength(PrSvgElement* element, PrSvgAttribute attribute, PrSvgLength value);

// Read and write the text of a length attribute, as SVGLength.valueAsString does. The text of a CSS function is kept as
// it is written, and any other length is serialized. A write takes a length or a CSS function, and returns Syntax for
// any other text. The caller releases the text that a read returns.
PrRawString prSvgElementGetLengthText(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer);
PrStatus prSvgElementSetLengthText(PrSvgElement* element, PrSvgAttribute attribute, PrStringView text);
bool prSvgElementGetAngle(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgAngle* out);
PrStatus prSvgElementSetAngle(PrSvgElement* element, PrSvgAttribute attribute, PrSvgAngle value);

// Read and write a rectangle, such as a viewBox. A missing rectangle, or one that does not parse, reads as none: the
// read returns false.
bool prSvgElementGetRect(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrDomRect* out);
PrStatus prSvgElementSetRect(PrSvgElement* element, PrSvgAttribute attribute, PrDomRect value);

// Read and write preserveAspectRatio.
bool prSvgElementGetPreserveAspectRatio(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    PrSvgPreserveAspectRatio* out);
PrStatus prSvgElementSetPreserveAspectRatio(PrSvgElement* element, PrSvgAttribute attribute, PrSvgPreserveAspectRatio value);

// Write up to `capacity` items of a list to `items`, and return the number of items that the list has, which is 0 if
// the element does not read the attribute as that list. A list that does not parse is empty, except that a list of
// points keeps the points before its error.
uint32_t prSvgElementGetNumberList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    float* PR_NULLABLE items, uint32_t capacity);
uint32_t prSvgElementGetLengthList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    PrSvgLength* PR_NULLABLE items, uint32_t capacity);
uint32_t prSvgElementGetTransformList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    PrSvgTransform* PR_NULLABLE items, uint32_t capacity);
uint32_t prSvgElementGetPointList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
    PrDomPoint* PR_NULLABLE items, uint32_t capacity);

// A string list's items are views of the attribute, which last until it changes. A string list has no animated value.
uint32_t prSvgElementGetStringList(const PrSvgElement* element, PrSvgAttribute attribute,
    PrStringView* PR_NULLABLE items, uint32_t capacity);

// Set a list's attribute to `count` items.
PrStatus prSvgElementSetNumberList(PrSvgElement* element, PrSvgAttribute attribute, const float* PR_NULLABLE items,
    uint32_t count);
PrStatus prSvgElementSetLengthList(PrSvgElement* element, PrSvgAttribute attribute,
    const PrSvgLength* PR_NULLABLE items, uint32_t count);
PrStatus prSvgElementSetTransformList(PrSvgElement* element, PrSvgAttribute attribute,
    const PrSvgTransform* PR_NULLABLE items, uint32_t count);
PrStatus prSvgElementSetPointList(PrSvgElement* element, PrSvgAttribute attribute, const PrDomPoint* PR_NULLABLE items,
    uint32_t count);
PrStatus prSvgElementSetStringList(PrSvgElement* element, PrSvgAttribute attribute,
    const PrStringView* PR_NULLABLE items, uint32_t count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
