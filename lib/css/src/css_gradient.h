// Gradients: a computed <gradient> as layout draws it.
//
// css-images-4 - https://drafts.csswg.org/css-images-4/#gradients

#pragma once

#include "pr/layout_style.h"
#include "pr/raw_array.h"

#include "css/src/css_style.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How a conversion makes the lengths of a computed value into layout lengths. A calculation of a length and a
// percentage is a compound, which a client of a layout tree interns there (prLayoutLengthIntern).
struct CssLayoutLengths {
    // Returns the layout length of `run`, a computed <length-percentage>.
    PrLayoutLength (*PR_NONNULL length)(void* PR_NULLABLE context, CssValueRun run);

    // Returns the layout length of one axis of a computed <position>, `run`, from the value at `index`. If that value
    // is an edge keyword, `offset` is the index of its offset, or kArrayMissing if it has none (cssPositionAxes).
    PrLayoutLength (*PR_NONNULL position)(void* PR_NULLABLE context, CssValueRun run, uint32_t index, uint32_t offset);

    void* PR_NULLABLE context;
};

// Writes the gradient of the computed <gradient> at `index` of `run` to `out`, and appends its stops to `stops`. The
// caller points `out->stops` at them. The colors resolve in `colors`, and are limited to `cap` times SDR white if `cap`
// is above zero.
void cssGradientFromValue(CssValueRun run, uint32_t index, const CssColorContext& colors, float cap,
    const CssLayoutLengths& lengths, RawArray<PrLayoutColorStop>& stops, PrLayoutGradient* out);

// Returns true if the computed <image> `run` is a gradient (css-images-4/#gradients).
bool cssIsGradient(CssValueRun run);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
