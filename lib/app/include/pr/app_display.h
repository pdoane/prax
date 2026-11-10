// Displays: the properties that rendering targets, the connected displays, positions on them, and the position of the
// mouse pointer.

#pragma once

#include "pr/app_application.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The properties of a display that layout, color resolution and rendering depend on. They take the whole struct, so
// that a property added here reaches all of them.
typedef struct PrAppDisplay {
    float pixelRatio;           // device pixels per logical pixel

    // The brightest color that the display can show, as a multiple of SDR white in linear light. `headroom` is the
    // current value, and `potentialHeadroom` is the maximum as the display's brightness changes. An SDR display has a
    // headroom of 1.
    float headroom;
    float potentialHeadroom;

    PrColorSpace gamut;         // the RGB space closest to the display's primaries: Srgb, DisplayP3 or Rec2020
} PrAppDisplay;

// Returns an SDR display with the sRGB gamut and one device pixel per logical pixel.
PrAppDisplay prAppDisplayDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Connected displays

// The identity of a display. It stays the same while the display is connected and, if the platform can identify the
// display, across reconnections. A program can save it with a window's position.
typedef uint64_t PrAppDisplayId;

// The id that identifies no display.
enum { kPrAppDisplayNone = 0 };

// The rotation of a display. Each orientation has a matching flag in PrAppOrientationFlags.
enum {
    PrAppOrientation_Landscape,
    PrAppOrientation_Portrait,
    PrAppOrientation_LandscapeFlipped,
    PrAppOrientation_PortraitFlipped,
};
typedef uint8_t PrAppOrientation;

// Returns the name of an orientation, as in "PortraitFlipped", or the null view if `orientation` is not one.
PrStringView prAppOrientationName(PrAppOrientation orientation);

// A set of orientations.
enum {
    PrAppOrientationFlag_Landscape = 1 << PrAppOrientation_Landscape,
    PrAppOrientationFlag_Portrait = 1 << PrAppOrientation_Portrait,
    PrAppOrientationFlag_LandscapeFlipped = 1 << PrAppOrientation_LandscapeFlipped,
    PrAppOrientationFlag_PortraitFlipped = 1 << PrAppOrientation_PortraitFlipped,
    PrAppOrientationFlag_All = 0xf,
};
typedef uint8_t PrAppOrientationFlags;

// A connected display. `extent` and `workArea` are in the display's logical pixels, from its top left. The work area
// excludes the space that the system reserves, such as the menu bar, the Dock or the taskbar. `name` is the name that
// the system shows for the display, and it is valid until the next DisplaysChanged event. `refreshRate` is the
// display's maximum refresh rate, in hertz.
typedef struct PrAppDisplayInfo {
    PrAppDisplayId id;
    PrStringView name;
    PrAppDisplay display;
    PrVec2 extent;
    PrRect workArea;
    float refreshRate;
    PrAppOrientation orientation;
    bool main;                  // true for the display with the menu bar or the primary taskbar
} PrAppDisplayInfo;

// A point on a display, in the display's logical pixels from its top left. Displays can have different scales, so no
// single coordinate space covers them all, and a position names its display.
typedef struct PrAppDisplayPosition {
    PrAppDisplayId display;
    PrVec2 position;
} PrAppDisplayPosition;

// Writes up to `capacity` connected displays to `out`, with the main display first, and returns the number of displays.
// A DisplaysChanged event follows each change. On the web, the only display is the page's screen.
uint32_t prAppDisplays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity);

// Writes the connected display whose id is `id` to `out`, and returns true. Returns false if no display has it.
bool prAppDisplayInfoFromId(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out);

// Writes the position of the mouse pointer on the screen to `out`, and returns true. Returns false if the platform does
// not report it, as in a headless application. On the web, it is where a window last saw the pointer, and false until
// one has.
bool prAppPointerPosition(const PrApp* app, PrAppDisplayPosition* out);

// Sets the orientations that the application's windows allow when the user rotates the device. Platforms whose displays
// do not rotate ignore it.
void prAppSetOrientations(PrApp* app, PrAppOrientationFlags orientations);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
