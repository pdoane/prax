// The SVG DOM's lengths.
//
// SVG2 - https://svgwg.org/svg2-draft/types.html#InterfaceSVGLength

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/svg.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// SVGLength (SVG2/types.html#InterfaceSVGLength) and SVGAnimatedLength (SVG2/types.html#InterfaceSVGAnimatedLength)
//
// A length reflects an attribute of an element, or is detached. With no animation running, an animated value is the
// base value, and is read only.

// The type of a length's value, with the values of SVGLength's constants.
enum {
    PrSvgLengthType_Unknown,            // another unit, or a value that is not a number times a unit
    PrSvgLengthType_Number,
    PrSvgLengthType_Percentage,
    PrSvgLengthType_Ems,
    PrSvgLengthType_Exs,
    PrSvgLengthType_Px,
    PrSvgLengthType_Cm,
    PrSvgLengthType_Mm,
    PrSvgLengthType_In,
    PrSvgLengthType_Pt,
    PrSvgLengthType_Pc,
};
typedef uint16_t PrSvgLengthType;

// A length of an attribute, or a detached one.
typedef struct PrSvgLength PrSvgLength;

// The length attributes of an element, for the SVGAnimatedLength members of its interface.
typedef struct PrSvgAnimatedLength PrSvgAnimatedLength;

// Creates a detached length of 0 (SVG2/struct.html#__svg__SVGSVGElement__createSVGLength).
PrSvgLength* prSvgLengthCreate(void);

// Frees the length.
void prSvgLengthDestroy(PrSvgLength* PR_NULLABLE length);

// Returns the attribute `name` of the element as an animated length, or null if its interface has no such member: x, y,
// width and height of svg, rect, image, use, foreignObject, pattern, mask, filter and of filter primitives; cx, cy and
// r of circle; cx, cy, rx and ry of ellipse; rx and ry of rect; x1, y1, x2 and y2 of line; and the attributes of
// gradients and markers that are lengths. The caller frees it with prSvgAnimatedLengthDestroy.
PrSvgAnimatedLength* PR_NULLABLE prSvgAnimatedLengthCreate(PrSvgElement* element, PrStringView name);

// Frees the animated length.
void prSvgAnimatedLengthDestroy(PrSvgAnimatedLength* PR_NULLABLE length);

// Returns the length that reflects the attribute's base value, or its animated value, which is read only. The caller
// frees it with prSvgLengthDestroy.
PrSvgLength* prSvgAnimatedLengthBaseVal(const PrSvgAnimatedLength* animated);
PrSvgLength* prSvgAnimatedLengthAnimVal(const PrSvgAnimatedLength* animated);

// Returns the type of the length's value (SVG2/types.html#__svg__SVGLength__unitType).
PrSvgLengthType prSvgLengthUnitType(const PrSvgLength* length);

// Returns the length in user units, with percentages of its element's viewport along the attribute's direction and
// font-relative units of its element's font, or of 100 and the initial font size if it is detached
// (SVG2/types.html#__svg__SVGLength__value). The value of a length whose element no tree draws resolves percentages
// against 0.
float prSvgLengthValue(const PrSvgLength* svgLength);

// Returns the number before the unit, or 0 for a value of another kind
// (SVG2/types.html#__svg__SVGLength__valueInSpecifiedUnits).
float prSvgLengthValueInSpecifiedUnits(const PrSvgLength* length);

// Returns the value as a string (SVG2/types.html#__svg__SVGLength__valueAsString). The caller releases it.
PrString prSvgLengthValueAsString(const PrSvgLength* length);

// Sets the value in user units as a number, the number before the unit, or the value from a string, and writes a
// reflected attribute again. Returns ReadOnly for an animated value, and Syntax for a string that is not a length.
PrStatus prSvgLengthSetValue(PrSvgLength* length, float value);
PrStatus prSvgLengthSetValueInSpecifiedUnits(PrSvgLength* length, float value);
PrStatus prSvgLengthSetValueAsString(PrSvgLength* length, PrStringView text);

// Sets the value to `value` in `type`, or converts it to `type` and keeps its value in user units
// (SVG2/types.html#__svg__SVGLength__newValueSpecifiedUnits). Returns ReadOnly for an animated value, and NotSupported
// for Unknown or a type that is not one.
PrStatus prSvgLengthNewValueSpecifiedUnits(PrSvgLength* length, PrSvgLengthType type, float value);
PrStatus prSvgLengthConvertToSpecifiedUnits(PrSvgLength* length, PrSvgLengthType type);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
