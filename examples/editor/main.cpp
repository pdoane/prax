// The editor example: a level editor of Studio Kit's widgets and a HUD library of its own, composed in one tree, in a
// window.
//
// main() is the composition root. The editor chooses each piece that runs it: the platform's window, the Noto fonts
// shaped by HarfBuzz, lib/ui's context over a layout tree with the kit and the HUD on top, and lib/draw's fragment
// renderer on the platform's GPU. Each frame runs the context's rounds, plays the tree's scene into a draw list, and
// renders the list into the window's swap chain, in SDR, since the editor shows nothing past sRGB.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "examples/editor/editor_view.h"
#include "examples/editor/hud.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace {

using namespace editor;

// The editor in a window: its model, its UI, and the device that it renders on.
struct Window {
    EditorState state;

    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PrAppColorScheme scheme = PrAppColorScheme_Dark; // the scheme of the kit's theme
    const PrLayoutScene* scene = nullptr;            // the scene of the last frame, which its list plays
    PrDrawList* list = nullptr;
    std::vector<PrLayoutSceneScroll> scrolls;

    PrApp* app = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
};

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

// Declares a round: a surface on the app's ground that fills the viewport, and the editor under it.
void build(void* context, PrUi*, const PrUiRound*) {
    Window& w = *static_cast<Window*>(context);
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .ground = PrKitGround_App,
    };
    buildEditor(w.kit, prKitSurface(w.kit, kPrLayoutNodeNone, {}, style), w.state);
}

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr)
        prGpuDeviceWaitIdle(w.device);
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.frame = nullptr;
    w.renderer = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
}

// Makes the platform's device, a swap chain over the window for SDR, and the renderer. Returns false if the window has
// no device to render with.
bool openDevice(Window& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

// Renders the list into the swap chain's texture at `extent` target pixels, clearing it to the canvas, and presents it.
void render(Window& w, PrUVec2 extent, PrColor canvas) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrDrawTarget into{.extent = extent, .format = PrPixelFormat_Bgra8, .commands = commands};
    prDrawFramePrepare(w.frame, w.list, &into);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(drawable, PrGpuLoad_Clear, canvas);
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    prUiSetApp(w.ui, app);
    if (!openDevice(w)) {
        fprintf(stderr, "editor: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    prAppRequestFrame(app);
}

void frame(void* context, PrApp* app, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // A lost device is made again, with everything on it. The frame draws from the model, as every frame does.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return;
    // Waiting first lets the frame read the latest input, and then wait for nothing once it is built.
    prGpuSwapChainWait(w.swapChain);

    // The kit's theme follows the user's preferred color scheme.
    PrAppColorScheme scheme = f->preferences.colorScheme;
    if (scheme != w.scheme) {
        w.scheme = scheme;
        PrKitTheme theme = prKitThemeStudio(variantOf(scheme), w.fonts);
        prKitSetTheme(w.kit, &theme);
    }
    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    prUiFrame(w.ui, &desc, build, &w);

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // pass clears to the canvas's color, so the list leaves the canvas unfilled.
    const PrLayoutScene* scene = prLayoutTreeScene(w.tree);
    prLayoutSceneRelease(w.scene);
    w.scene = scene;
    w.scrolls.resize(prLayoutSceneScrolls(w.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(w.tree, scene, w.scrolls.data(), uint32_t(w.scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = w.scrolls.data();
    options.scrollCount = uint32_t(w.scrolls.size());
    options.canvasFilled = true;
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, w.list, &options);
    prDrawListPopTransform(w.list);
    PrColor canvas = prColorFromColor16(prLayoutSceneCanvasColor(scene));

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a});
    prAppSetCursor(app, prUiCursor(w.ui));
    if (prUiWantsFrame(w.ui))
        prAppRequestFrame(app);
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    prUiPushEvent(w.ui, e);
    prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main() {
    prCrashHandler();
    static Window w;
    w.fonts = prFontSetCreate(nullptr);
    if (prFontBundleInstall(w.fonts, PrFontBundle_Noto, PR_FONT_BUNDLE_DIR) == 0) {
        fprintf(stderr, "editor: the fonts under %s could not be installed\n", PR_FONT_BUNDLE_DIR);
        return 1;
    }
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = w.shaper};
    w.tree = prLayoutTreeCreate(&treeDesc);
    w.ui = prUiCreate(w.tree);
    PrKitTheme theme = prKitThemeStudio(variantOf(w.scheme), w.fonts);
    w.kit = prKitCreate(w.ui, &theme);
    w.list = prDrawListCreate();
    // The HUD lives as long as the context that its records belong to.
    w.state.hud = hudCreate(w.ui, w.fonts);
    w.state.loadLevel();

    std::string title = "Editor: " + w.state.levelName;
    PrAppDesc desc{.title = title, .extent = {1440, 900}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&w, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);

    prDrawListRelease(w.list);
    prLayoutSceneRelease(w.scene);
    hudDestroy(w.state.hud);
    prKitDestroy(w.kit);
    prUiDestroy(w.ui);
    prLayoutTreeDestroy(w.tree);
    prTextShaperDestroy(w.shaper);
    prFontSetDestroy(w.fonts);
    return 0;
}
