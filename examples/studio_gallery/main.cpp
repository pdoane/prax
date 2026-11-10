// The Studio Kit gallery: every widget of Studio Kit, live, in Studio's window (pr/studio_window.h).
//
// main() is the composition root: the gallery's model, whose choice of theme the window's kit draws in.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/font_bundle.h"
#include "pr/print.h"
#include "pr/studio_app.h"

#include "examples/studio_gallery/gallery_view.h"

namespace {

using namespace gallery;

GalleryState& stateOf(void* context) {
    return *static_cast<GalleryState*>(context);
}

// Gives the kit the palette of the theme that the gallery chose, or keeps the user's preferred color scheme's.
void paletteOf(void* context, PrStudioWindow*, const PrAppUserPreferences*, PrKitPalette* palette) {
    switch (stateOf(context).theme) {
        case GalleryTheme_Dark: *palette = prKitPaletteStudio(PrKitThemeVariant_Dark); break;
        case GalleryTheme_Light: *palette = prKitPaletteStudio(PrKitThemeVariant_Light); break;
        default: break;
    }
}

} // namespace

int main() {
    prCrashHandler();
    static GalleryState state;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Complete};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles)), PR_FONT_BUNDLE_DIR) == 0) {
        printError("studio_gallery: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        prFontSetDestroy(fonts);
        return 1;
    }
    PrStudioWindowDesc desc{
        .title = "Studio Kit Gallery",
        .extent = {1440, 900},
        .intent = PrGpuSwapChainIntent_Hdr,
        .palette = paletteOf,
        .build = [](void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
            buildGallery(prStudioWindowKit(window), root, stateOf(context));
        },
        .context = &state,
    };
    PrStudioAppDesc app{.fonts = fonts, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        printError("studio_gallery: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
