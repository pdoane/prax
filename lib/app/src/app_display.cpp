// Displays.

#include "pr/app_display.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

// The names of the enumerations, in the order of their enumerators.
const char* const kOrientations[] = {"Landscape", "Portrait", "LandscapeFlipped", "PortraitFlipped"};
static_assert(countOf(kOrientations) == PrAppOrientation_PortraitFlipped + 1, "a name for each orientation");

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAppDisplay prAppDisplayDefault(void) {
    return PrAppDisplay{.pixelRatio = 1, .headroom = 1, .potentialHeadroom = 1, .gamut = PrColorSpace_Srgb};
}

PrStringView prAppOrientationName(PrAppOrientation orientation) {
    return appNameOf(kOrientations, orientation);
}

uint32_t prAppDisplays(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    PR_ASSERT(out != nullptr || capacity == 0);
    return app->displaysFn(app, out, capacity);
}

bool prAppDisplayInfoFromId(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out) {
    *out = PrAppDisplayInfo{};
    return app->displayInfoFn(app, id, out);
}

bool prAppPointerPosition(const PrApp* app, PrAppDisplayPosition* out) {
    *out = PrAppDisplayPosition{};
    return app->pointerPositionFn != nullptr && app->pointerPositionFn(app, out);
}

void prAppSetOrientations(PrApp* app, PrAppOrientationFlags orientations) {
    PR_ASSERT(orientations != 0 && (orientations & ~PrAppOrientationFlag_All) == 0);
    if (app->setOrientationsFn != nullptr)
        app->setOrientationsFn(app, orientations);
}
