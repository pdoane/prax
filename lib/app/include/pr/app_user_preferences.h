// User preferences: the user's settings for how content is shown.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A color scheme: light backgrounds with dark text, or the reverse.
enum {
    PrAppColorScheme_Light,
    PrAppColorScheme_Dark,
};
typedef uint8_t PrAppColorScheme;

// Returns the name of a color scheme, as in "Dark", or the null view if `scheme` is not one.
PrStringView prAppColorSchemeName(PrAppColorScheme scheme);

// The contrast that the user requests. Custom means that the user chose a palette of their own, which is neither more
// nor less contrast.
enum {
    PrAppContrast_NoPreference,
    PrAppContrast_More,
    PrAppContrast_Less,
    PrAppContrast_Custom,
};
typedef uint8_t PrAppContrast;

// Returns the name of a contrast preference, as in "NoPreference", or the null view if `contrast` is not one.
PrStringView prAppContrastName(PrAppContrast contrast);

// The user's settings for how content is shown.
//
// `accentColor` is the color that the user chose for controls and selections. It is transparent if the user chose none,
// and the program then uses its own. `textScale` is the factor by which the user enlarges text independently of the
// rest of the interface, such as 1.5 for 150%. It is separate from the display's scale (PrAppDisplay::pixelRatio),
// which applies to everything.
typedef struct PrAppUserPreferences {
    PrAppColorScheme colorScheme;
    PrAppContrast contrast;
    bool reducedMotion;
    bool reducedTransparency;   // fewer translucent layers
    PrColor16 accentColor;
    float textScale;
} PrAppUserPreferences;

// Returns the preferences of a user who changed no settings: a light scheme, no accent color, and a text scale of 1.
PrAppUserPreferences prAppUserPreferencesDefault(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
