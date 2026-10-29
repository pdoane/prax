// Color Values
//
// css-color-4 - https://drafts.csswg.org/css-color-4/
//
// A color function written with its components folds into a PrCssValueKind_Color whose children are its channels as
// written. A channel's meaning depends on the color's space. The functions here read a channel in the space's units.

#pragma once

#include "pr/color.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How a channel is read in its space's units. A number is multiplied by `number` and a percentage by `percent`, then
// clamped to [`min`, `max`]. If `hue` is set, the channel is in degrees and normalized to [0, 360)
// (css-color-4/#hue-syntax).
struct CssColorChannelSyntax {
    double number;
    double percent;
    double min;
    double max;
    bool hue;
};

// How an alpha channel is read: an <alpha-value> is clamped to [0, 1] (css-color-4/#alpha-syntax).
extern const CssColorChannelSyntax kCssColorAlpha;

// Returns how the three channels before the alpha are read for a color in `space`. If `legacy` is set, returns the
// channels of rgb() and rgba() (css-color-4/#rgb-functions).
const CssColorChannelSyntax* cssColorChannels(PrColorSpace space, bool legacy);

// The channels of a PrCssValueKind_Color value. Each array has one entry for each channel, alpha last.
struct CssColorChannels {
    PrColorSpace space;
    bool legacy;                                    // written in a legacy form (CssValueAuxFlag_ColorLegacy)
    uint32_t at[4];                                 // position of each channel in the run
    const CssColorChannelSyntax* PR_NONNULL meaning[4];
    uint32_t missing;                               // a bit for each channel written `none`
    uint32_t calculated;                            // a bit for each channel that a math function computes
};

// Returns the channels of the color at `index` of `run`.
CssColorChannels cssColorChannelsFromValue(CssValueRun run, uint32_t index);

// Returns the value of a channel written as `value`, in the units of its color space. `value` is a number, a percentage
// or an angle.
double cssColorChannelValue(const CssValueNode* value, const CssColorChannelSyntax& channel);

// Returns the color space that a color function's keyword names, or that a <predefined-rgb> or <xyz-space> keyword
// names. `xyz` is xyz-d65 (css-color-4/#valdef-color-xyz). Returns PrColorSpace_Count for a keyword that names none.
PrColorSpace cssColorSpaceFromKeyword(PrCssKeyword keyword);

// Returns true if a function named `keyword` is a <color> other than one named by a color space.
bool cssIsColorFunction(PrCssKeyword keyword);

// Returns true if the colors among `values` in [first, end) are all legacy colors: named colors, hex colors and the
// sRGB functions (css-color-4/#legacy-color-syntax), none of them with a missing component. Colors that are all legacy
// interpolate in sRGB by default (css-color-4/#interpolation-space).
bool cssColorsAreLegacy(const CssValueNode* values, uint32_t first, uint32_t end);

// A color as it resolves before it is converted for display: its space, its channels in the units that PrColorSpace
// gives, its alpha, and a bit for each missing component, alpha's last (css-color-4/#missing).
struct CssColor {
    PrColorSpace space;
    double c[3];
    double alpha;
    uint32_t missing;
};

// Adds a color as a Color written with its components in its own space, a missing component as `none`.
void cssValuePoolAddColorComponents(CssValuePool* pool, const CssColor& color);

// Adds an sRGB color as a Color written with rgb(). Its channels are the numbers that rgb() takes, followed by its
// alpha.
void cssValuePoolAddColor(CssValuePool* pool, PrColor8 color);

// Adds a color in sRGB as a Color written with rgb(), its channels at full precision and outside rgb()'s range if they
// fall there. A missing component is zero.
void cssValuePoolAddLegacyColor(CssValuePool* pool, const CssColor& color);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
