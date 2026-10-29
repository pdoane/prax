// The grammars that SVG defines for its own attributes rather than taking from CSS.
//
// SVG2 - https://svgwg.org/svg2-draft/

#pragma once

#include "pr/array.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses the four numbers of a viewBox (SVG2/coords.html#ViewBoxAttribute). Returns false if they do not parse, or if
// the width or height is negative.
bool svgParseViewBox(PrStringView text, PrRect* out);

// Where preserveAspectRatio aligns the viewBox in the viewport along one axis
// (SVG2/coords.html#PreserveAspectRatioAttribute). None scales each axis on its own to fill the viewport.
enum {
    SvgAlign_None,
    SvgAlign_Min,
    SvgAlign_Mid,
    SvgAlign_Max,
};
typedef uint8_t SvgAlign;

// A preserveAspectRatio value. Its defaults are `xMidYMid meet`.
struct SvgAspect {
    SvgAlign x = SvgAlign_Mid;          // SvgAlign_None in both if the value is `none`
    SvgAlign y = SvgAlign_Mid;
    bool slice = false;                 // `slice` rather than `meet`
};

// Parses a preserveAspectRatio value. Returns false, leaving `*out` the default, if `text` does not parse.
bool svgParseAspect(PrStringView text, SvgAspect* out);

// Returns the transform that places `viewBox` in a viewport of `width` by `height` at the origin
// (SVG2/coords.html#ComputingAViewportsTransform).
PrAffine2 svgViewBoxTransform(PrRect viewBox, SvgAspect aspect, float width, float height);

// Parses a transform list in SVG's syntax (css-transforms-1/#svg-syntax): matrix, translate, scale, rotate, skewX and
// skewY, with angles in degrees, separated by white space or commas. Writes the transform that applies the functions
// from left to right, the first outermost, to `out`. Returns false, leaving `out` alone, if `text` does not parse.
bool svgParseTransform(PrStringView text, PrAffine2* out);

// Replaces `out` with the numbers of the list in `text`, which white space or commas separate, up to the first number
// that does not parse. Returns false if a number did not parse.
bool svgParseNumbers(PrStringView text, Array<float>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
