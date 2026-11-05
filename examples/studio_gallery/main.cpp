// The Studio Kit gallery: every widget of Studio Kit, live, in Studio's window (pr/studio_window.h).
//
// main() is the composition root: the gallery's model, whose choice of theme the window's kit draws in.

#include "pr/assert.h"
#include "pr/print.h"
#include "pr/studio_window.h"

#include "examples/studio_gallery/gallery_view.h"

namespace {

using namespace gallery;

GalleryState& stateOf(void* context) {
    return *static_cast<GalleryState*>(context);
}

// Returns the variant of the theme that the gallery chose: its own, or the user's preferred color scheme's.
PrKitThemeVariant themeOf(void* context, PrStudioWindow*, const PrAppUserPreferences* preferences) {
    switch (stateOf(context).theme) {
        case GalleryTheme_Dark: return PrKitThemeVariant_Dark;
        case GalleryTheme_Light: return PrKitThemeVariant_Light;
        default: break;
    }
    return preferences->colorScheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

} // namespace

int main() {
    prCrashHandler();
    static GalleryState state;
    PrStudioWindowDesc desc{
        .title = "Studio Kit Gallery",
        .extent = {1440, 900},
        .fontDirectory = PR_FONT_BUNDLE_DIR,
        .theme = themeOf,
        .build = [](void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
            buildGallery(prStudioWindowKit(window), root, stateOf(context));
        },
        .context = &state,
    };
    PrStatus status = prStudioWindowRun(&desc);
    if (status == PrStatus_NotFound)
        printError("studio_gallery: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
    else if (status == PrStatus_NotSupported)
        printError("studio_gallery: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
