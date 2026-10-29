// Animation values: easing, the lists of the animation properties, and interpolating computed values.
//
// css-easing-2 - https://drafts.csswg.org/css-easing-2/
// css-values-4 - https://drafts.csswg.org/css-values-4/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#pragma once

#include "pr/color.h"
#include "pr/layout_animation.h"

#include "css/src/css_style.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The items of a comma-separated list value, given as the index in the run of the first value of each item.
struct CssListItems {
    uint32_t starts[16];
    uint32_t count;                 // at most countOf(starts); items past that limit are left out
};

// Returns the items of `run`, a comma-separated list.
CssListItems cssListItems(CssValueRun run);

// Returns item `index` of a list. The list repeats as many times as it takes to reach `index`
// (css-animations-1/#animation-name).
CssValueRun cssListItem(CssValueRun run, const CssListItems& items, uint32_t index);

// Returns a <time> in milliseconds, or `fallback` if the value is not a <time>.
double cssTimeMilliseconds(CssValueRun run, double fallback);

// Returns the easing function of an <easing-function> value (css-easing-2/#easing-functions). Returns linear for a
// function that the engine does not run, such as linear() with stops.
PrLayoutEasing cssEasing(CssValueRun easing);

// Returns the transform functions of an element's computed translate, rotate, scale and transform, in the order in
// which css-transforms-2/#ctm composes them (css-transforms-2/#transform-functions). The functions are in the engine's
// terms: lengths in px, a percentage as a fraction of the reference box, and angles in radians. Each value may be
// `none`, or empty for none.
PrLayoutTransformList cssTransformFunctions(CssValueRun translate, CssValueRun rotate, CssValueRun scale,
                                            CssValueRun transform);

// Returns true if the computed values `from` and `to` of `property` differ and interpolate other than discretely. A
// transition between them requires both (css-transitions-1/#starting).
bool cssInterpolable(PrCssProperty property, CssValueRun from, CssValueRun to, const CssColorContext& colors);

// Appends the computed value of `property` at `progress` between the computed values `from` and `to`
// (css-values-4/#interpolation).
//
// - Values of the same shape interpolate their numbers. A <length-percentage> that is a length on one side and a
//   percentage or a calculation on the other interpolates as a calculation.
// - Colors interpolate premultiplied: in sRGB if both are legacy colors, and in Oklab otherwise.
// - A transform list interpolates with `none`, which stands for the identity of the other list's functions.
// - Ratios interpolate by their logarithms, unless either ratio is degenerate.
// - Filter lists interpolate function by function if their functions pair by name. The shorter list is padded with
//   the initial values of the other list's functions, and none stands for the empty list.
// - Basic shapes interpolate argument by argument if their kinds and reference boxes agree. An inset() interpolates
//   with every inset and radius written out. A circle or ellipse sized by an extent keyword interpolates discretely.
// - A property of one to four sides, and a clip's rect(), interpolate side by side.
//
// Any other pair interpolates discretely, as does every pair of a property whose animation type is discrete
// (kCssPropertyDiscrete): the result is `from` below a progress of one half, and `to` from one half on
// (web-animations-1/#discrete). Colors resolve in `colors`, the element's.
void cssInterpolate(CssValuePool* out, PrCssProperty property, CssValueRun from, CssValueRun to, double progress,
                    const CssColorContext& colors);

// How a keyframe's value combines with the value under the animation (web-animations-1/#effect-composition).
enum {
    CssComposite_Replace,
    CssComposite_Add,
    CssComposite_Accumulate,
};
typedef uint8_t CssComposite;

// Returns the operation that an item of a computed animation-composition names
// (css-animations-2/#animation-composition).
CssComposite cssCompositeFromValue(CssValueRun item);

// Appends the computed value of `property` that `value` gives over `underlying` by `composite`
// (css-values-4/#combining-values). Addition sums numbers, lengths and percentages, adds colors premultiplied in the
// space that they interpolate in, appends a transform list to the underlying list, and composes individual transforms:
// translations add, scales multiply, and rotations compose. A filter list's addition appends it to the underlying list
// (filter-effects-1/#addition). Accumulation differs for scales, transform lists and filter lists. The factors of
// scales add less one. The functions of transform lists accumulate argument by argument if the lists have the same
// shape. The functions of filter lists that pair by name sum argument by argument, one-based for a function that does
// nothing at an amount of one (filter-effects-1/#accumulation). Any other pair, and every pair of a discrete property,
// gives `value`, as replace does. Colors resolve in `colors`.
void cssComposite(CssValuePool* out, PrCssProperty property, CssValueRun underlying, CssValueRun value,
                  CssComposite composite, const CssColorContext& colors);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
