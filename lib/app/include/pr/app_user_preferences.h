// User preferences: what the user chose for how content is shown.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A color scheme: light backgrounds with dark text, or the opposite.
enum {
    PrAppColorScheme_Light,
    PrAppColorScheme_Dark,
};
typedef uint8_t PrAppColorScheme;

// How much contrast the user asks for. Custom is a palette of the user's own, which is neither more nor less contrast.
enum {
    PrAppContrast_NoPreference,
    PrAppContrast_More,
    PrAppContrast_Less,
    PrAppContrast_Custom,
};
typedef uint8_t PrAppContrast;

// The user's choices for how content is shown. The neutral value, a light scheme with no other preference, is all
// zeros.
typedef struct PrAppUserPreferences {
    PrAppColorScheme colorScheme;
    PrAppContrast contrast;
    bool reducedMotion;         // the user asks for less motion
    bool reducedTransparency;   // the user asks for fewer translucent layers
} PrAppUserPreferences;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
