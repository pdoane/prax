// The grammars that SVG defines for its own attributes rather than taking from CSS.
//
// SVG2 - https://svgwg.org/svg2-draft/

#pragma once

#include "pr/geom.h"
#include "pr/path.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/svg_types.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses the four numbers of a viewBox (SVG2/coords.html#ViewBoxAttribute). Returns false if they do not parse, or if
// the width or height is negative.
bool svgParseViewBox(PrStringView text, PrRect* out);

// The value of a missing or invalid preserveAspectRatio: `xMidYMid meet`.
constexpr PrSvgPreserveAspectRatio kSvgAspectDefault = {PrSvgAlign_XMidYMid, PrSvgMeetOrSlice_Meet};

// Returns true if two values of preserveAspectRatio are the same.
inline bool svgSameAspect(PrSvgPreserveAspectRatio a, PrSvgPreserveAspectRatio b) {
    return a.align == b.align && a.meetOrSlice == b.meetOrSlice;
}

// Parses a preserveAspectRatio value (SVG2/coords.html#PreserveAspectRatioAttribute): an optional defer, which SVG 2
// dropped, an alignment, and an optional meet or slice. Returns false, writing the default, if `text` does not parse.
bool svgParseAspect(PrStringView text, PrSvgPreserveAspectRatio* out);

// Returns the transform that places `viewBox` in a viewport of `width` by `height` at the origin
// (SVG2/coords.html#ComputingAViewportsTransform).
PrAffine2 svgViewBoxTransform(PrRect viewBox, PrSvgPreserveAspectRatio aspect, float width, float height);

// Parses a transform list in SVG's syntax (css-transforms-1/#svg-syntax): matrix, translate, scale, rotate, skewX and
// skewY, with angles in degrees, separated by white space or commas. Writes the transform that applies the functions
// from left to right, the first outermost, to `out`. Returns false, leaving `out` alone, if `text` does not parse.
bool svgParseTransform(PrStringView text, PrAffine2* out);

// Replaces `out` with the numbers of the list in `text`, which white space or commas separate, up to the first number
// that does not parse. Returns false if a number did not parse.
bool svgParseNumbers(PrStringView text, RawArray<float>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
