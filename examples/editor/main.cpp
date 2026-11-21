// The editor example: a level editor of Studio Kit's widgets and a HUD library of its own, composed in one tree, in
// Studio's window (pr/studio_window.h).
//
// main() is the composition root: the editor's model, and the HUD over the window's context and fonts.

#include "pr/assert.h"
#include "pr/compress_select.h"
#include "pr/count_of.h"
#include "pr/font_bundle.h"
#include "pr/image_select.h"
#include "pr/print.h"
#include "pr/studio_app.h"

#include "examples/editor/editor_view.h"
#include "examples/editor/hud.h"

namespace {

using namespace editor;

EditorState& stateOf(void* context) {
    return *static_cast<EditorState*>(context);
}

} // namespace

int main() {
    prCrashHandler();
    static EditorState state;
    state.loadLevel();
    String title = format("Editor: {}", state.levelName);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Complete};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles))) == 0) {
        printError("editor: the font bundles among its resources could not be installed\n");
        prFontSetDestroy(fonts);
        return 1;
    }
    PrStudioWindowDesc desc{
        .title = title,
        .extent = {1440, 900},
        // The HUD lives as long as the context that its records belong to.
        .open = [](void* context, PrStudioWindow* window) {
            stateOf(context).hud = hudCreate(prStudioWindowUi(window), prStudioWindowFonts(window));
            return true;
        },
        .build = [](void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
            buildEditor(prStudioWindowKit(window), root, stateOf(context));
        },
        .close = [](void* context, PrStudioWindow*, PrStatus) { hudDestroy(stateOf(context).hud); },
        .context = &state,
    };
    // The program decodes every format that Prax has.
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = compress != nullptr ? prImageCodecSetCreateDefault(compress) : nullptr;
    PrStudioAppDesc app{.fonts = fonts, .compress = compress, .images = images, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        printError("editor: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
