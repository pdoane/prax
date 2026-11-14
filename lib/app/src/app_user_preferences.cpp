// User preferences.

#include "pr/app_user_preferences.h"

#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

// The names of the enumerations, in the order of their enumerators.
const char* const kColorSchemes[] = {"Light", "Dark"};
static_assert(countOf(kColorSchemes) == PrAppColorScheme_Dark + 1, "a name for each color scheme");

const char* const kContrasts[] = {"NoPreference", "More", "Less", "Custom"};
static_assert(countOf(kContrasts) == PrAppContrast_Custom + 1, "a name for each contrast preference");

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppColorSchemeName(PrAppColorScheme scheme) {
    return appNameOf(kColorSchemes, scheme);
}

PrStringView prAppContrastName(PrAppContrast contrast) {
    return appNameOf(kContrasts, contrast);
}

PrAppUserPreferences prAppUserPreferencesDefault(void) {
    return PrAppUserPreferences{.colorScheme = PrAppColorScheme_Light, .textScale = 1};
}
