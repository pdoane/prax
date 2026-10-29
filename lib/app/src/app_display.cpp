// Displays: what a screen that content is shown on can show.

#include "pr/app_display.h"

PrAppDisplay prAppDisplayDefault(void) {
    return PrAppDisplay{.pixelRatio = 1, .headroom = 1, .potentialHeadroom = 1, .gamut = PrColorSpace_Srgb};
}
