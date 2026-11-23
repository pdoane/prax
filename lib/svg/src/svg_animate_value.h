// Animation values: how the animation elements read, interpolate and compose the values of an attribute
//
// svg-animations - https://svgwg.org/specs/animations/
//
// An animation's values are read once, as the type of the animated attribute reads them, into an animation function
// that samples them. The type decides how two values combine: numbers and lengths component by component, colors in
// sRGB, path data command by command, and a transform function by its arguments. A value that does not parse as its
// type, or a type that does not interpolate, animates discretely. What a sample gives an attribute is a value, which is
// written as text only for a reader that takes text.

#pragma once

#include "pr/css_properties.h"
#include "pr/dom.h"
#include "pr/path_data.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/svg_types.h"
#include "pr/text_buffer.h"
#include "pr/vec.h"

#include "css/src/css_color_value.h"
#include "svg/src/svg_animate_names.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How an animation moves between its values (svg-animations/#CalcModeAttribute).
enum {
    SvgCalcMode_Discrete,
    SvgCalcMode_Linear,
    SvgCalcMode_Paced,
    SvgCalcMode_Spline,
};
typedef uint8_t SvgCalcMode;

// What the units of an animated value resolve against.
struct SvgAnimateContext {
    double width;                           // the size of the viewport that percentages measure
    double height;
    double fontSize;                        // the font size that em and ex measure
    SvgAxis axis;
    const PrDomElement* PR_NULLABLE target; // resolves currentcolor

    // The CSS property that the attribute is a presentation attribute of, whose inherit is the target's parent's value,
    // or PrCssProperty_Count.
    PrCssProperty property;
};

// A value of an animated attribute, as its type reads it. Its components keep the units that they are written in, which
// resolve against a context as they combine.
struct SvgAnimateValue {
    RawArray<double> numbers;               // the components as written
    RawArray<PrCssUnit> units;              // of each component: PrCssUnit_Number for a number alone
    RawArray<PrPathDataCommand> path;       // in absolute coordinates
    PrRawString text;                       // as written, or null for a value that a combination made
    RawArray<PrSvgTransform> before;        // the transform functions before the one that an animateTransform animates
    CssColor color;                         // in sRGB, which colors interpolate in, each channel in [0, 1]
    PrSvgValueType type;
    PrSvgTransformType transform;           // the function that a transform animates, or 0 for none
    bool hasColor;
    bool valid;

    // Reading the value read its context beyond its units: a color that the target resolves, a parent's inherited
    // value, or a math function that the viewport resolves. Such a value is read again for each sample.
    bool contextual;
};

// Frees a value, and copies one into another.
void svgAnimateValueReset(SvgAnimateValue* value);
void svgAnimateValueCopy(SvgAnimateValue* out, const SvgAnimateValue& from);

// Returns true if two values are the same value.
bool svgAnimateValueEqual(const SvgAnimateValue& a, const SvgAnimateValue& b);

// Reads a value of `type` from `text`. A value that does not parse as the type is not valid.
void svgAnimateValueRead(const SvgAnimateContext& context, PrSvgValueType type, PrStringView text, SvgAnimateValue* out);

// Appends the text of a value: the text that it was read from if nothing combined it, and its components otherwise.
void svgAnimateValueWrite(const SvgAnimateValue& value, PrTextBuffer* out);

// Appends the functions of a transform value to `out`: those before the function that it animates, and that function.
void svgAnimateTransforms(const SvgAnimateValue& value, RawArray<PrSvgTransform>* out);

// The value attributes of an animation element (svg-animations/#ValueAttributes). An absent attribute is the null view,
// which differs from an empty one.
struct SvgAnimateSpec {
    PrStringView values;
    PrStringView from;
    PrStringView to;
    PrStringView by;
    PrStringView keyTimes;
    PrStringView keySplines;
    PrStringView calcMode;
    PrStringView additive;
    PrStringView accumulate;
    PrStringView transformType;     // an animateTransform's type, or the null view for another element
    PrSvgValueType type;
    bool set;                       // a set element, which takes its to value for the whole simple duration
};

// What an animation function does with its values.
enum {
    SvgAnimateFlag_Additive = 1 << 0,     // adds its value to the underlying value
    SvgAnimateFlag_Accumulate = 1 << 1,   // each repeat builds on the last value of the one before
    SvgAnimateFlag_ToAnimation = 1 << 2,  // the first value is the underlying value, which a sample fills in
    SvgAnimateFlag_Set = 1 << 3,          // a set, which takes its to value for the whole simple duration

    // An additive animateTransform, which appends its function to the underlying list.
    SvgAnimateFlag_Appends = 1 << 4,

    // The function reads values that depend on its context: colors of currentcolor, or paced values whose spacing
    // depends on units that the context resolves. It is compiled again for each sample.
    SvgAnimateFlag_Contextual = 1 << 5,
};
typedef uint8_t SvgAnimateFlags;

// An animation function: its values, with a key time for each, and the splines between them.
struct SvgAnimateFunction {
    RawArray<SvgAnimateValue> values;
    RawArray<double> keyTimes;
    RawArray<double> keySplines;    // four for each interval, in spline mode
    PrSvgValueType type;
    SvgCalcMode calcMode;
    PrSvgTransformType transform;   // the function that an animateTransform animates, or 0 for another element
    SvgAnimateFlags flags;
};

// Builds the animation function of `spec`. Returns false if the animation is in error.
bool svgAnimateCompile(const SvgAnimateContext& context, const SvgAnimateSpec& spec, SvgAnimateFunction* out);
void svgAnimateFunctionReset(SvgAnimateFunction* function);

// Writes to `out` the value that the function takes at `progress` through repeat `repeat` of its simple duration,
// composed over `underlying`, the value that the animations below it give the attribute, or the attribute's own value
// (svg-animations/#AdditiveAttribute, svg-animations/#AccumulateAttribute). An underlying value of another type is read
// again as the function's. Returns false, writing nothing, if the animation is in error.
bool svgAnimateSample(const SvgAnimateContext& context, const SvgAnimateFunction& function,
    const SvgAnimateValue& underlying, double progress, uint32_t repeat, SvgAnimateValue* out);

// Appends to `out` the items of a list separated by semicolons, trimmed, without an empty last item
// (svg-animations/#ValuesAttribute).
void svgAnimateSplit(PrStringView text, RawArray<PrStringView>* out);

// Appends to `out` the numbers of `text`, separated by white space or commas. Returns false if anything else is there.
bool svgAnimateNumbers(PrStringView text, RawArray<double>* out);

// Appends to `out` the key times of `text`, one for each of `values` values. They start at 0, never decrease, and end
// at 1 unless the animation is discrete (svg-animations/#KeyTimesAttribute). Returns false if they do not.
bool svgAnimateKeyTimes(PrStringView text, uint32_t values, bool discrete, RawArray<double>* out);

// Appends to `out` the four control coordinates of each of `intervals` splines, each within [0, 1]
// (svg-animations/#KeySplinesAttribute). Returns false if `text` does not give them.
bool svgAnimateKeySplines(PrStringView text, uint32_t intervals, RawArray<double>* out);

// Returns the output of the cubic Bézier easing whose control points are `spline` for the input `x`.
double svgAnimateSpline(const double* spline, double x);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
