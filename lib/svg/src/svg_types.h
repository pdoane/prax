// The values of SVG's attributes, as lib/svg reads and writes them inside the engine (pr/svg_types.h)
//
// SVG2 - https://svgwg.org/svg2-draft/types.html

#pragma once

#include "pr/css.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/svg_types.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Return true if a length, or an angle, takes the unit.
bool svgIsLengthUnit(PrCssUnit unit);
bool svgIsAngleUnit(PrCssUnit unit);

// Returns the unit that `text` spells after a number, without regard to ASCII case: PrCssUnit_Number for none, a
// percent sign's, or a unit of CSS, or PrCssUnit_Unknown.
PrCssUnit svgUnitOf(PrStringView text);

// Return the unit that `text` spells after a length's number, or an angle's, or PrCssUnit_Unknown if it spells none
// that the value takes. The empty text spells PrCssUnit_Number: user units, or degrees.
PrCssUnit svgLengthUnitOf(PrStringView text);
PrCssUnit svgAngleUnitOf(PrStringView text);

// Returns the spelling of a unit after a number: none for PrCssUnit_Number, and a percent sign for a percentage.
PrStringView svgUnitName(PrCssUnit unit);

// Returns the user units in one of a length's `unit`, with percentages of `basis.percent` and font-relative units of
// its font size, or 0 for a unit that a length does not take.
double svgLengthUnitScale(PrCssUnit unit, PrSvgLengthBasis basis);

// Returns the degrees in one of an angle's `unit`, or 0 for a unit that an angle does not take.
double svgAngleUnitDegrees(PrCssUnit unit);

// Appends the functions of a transform list in SVG's syntax to `out`. Returns false if `text` does not parse, having
// appended the functions before the error.
bool svgParseTransformList(PrStringView text, RawArray<PrSvgTransform>* out);

// Appends a transform function as SVG writes it: its name and the arguments that it is written with.
void svgAppendTransform(PrTextBuffer* out, const PrSvgTransform& transform);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
