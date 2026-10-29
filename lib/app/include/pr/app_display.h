// Displays: what a screen that content is shown on can show.

#pragma once

#include "pr/base.h"
#include "pr/color.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a display shows: its pixel density, its brightest color and its gamut. Layout, color resolution and rendering
// each take the whole display, so that a property added here reaches all of them.
typedef struct PrAppDisplay {
    float pixelRatio;           // the display's pixels per logical pixel, which what is drawn for it targets

    // The brightest color that the display shows, as a multiple of SDR white in linear light: now, and the most that it
    // reaches as its brightness changes. A display that shows SDR alone has a headroom of 1.
    float headroom;
    float potentialHeadroom;

    PrColorSpace gamut;         // the RGB space whose primaries are nearest the display's: Srgb, DisplayP3 or Rec2020
} PrAppDisplay;

// An SDR display of sRGB's gamut at one pixel per logical pixel.
PrAppDisplay prAppDisplayDefault(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
