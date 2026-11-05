// Studio's window (window.h): the kit and what it draws with, the device and the swap chain, and the frames that run
// the application's rounds and render them.

#include "pr/studio_window.h"

#include "pr/alloc.h"
#include "pr/draw_display.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font_bundle.h"
#include "pr/gpu.h"
#include "pr/kit_surface.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/raw_array.h"
#include "pr/text.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// A window while it runs: the application's description, its kit and what the kit draws with, and the device that the
// frames render on.
struct WindowRun {
    PrStudioWindow* handle;
    PrStudioWindowDesc desc;
    PrStatus status;                // how the run ended
    PrVec2 extent;                  // of the viewport, in px

    double lastFrame;               // when the last frame ended
    double firstEvent;              // when the oldest event that no frame has taken arrived, or zero

    PrFontSet* fonts;
    PrTextShaper* shaper;
    PrLayoutTree* tree;
    PrUi* ui;
    PrKit* kit;
    PrKitThemeVariant variant;              // the variant that the kit's theme is in
    const PrLayoutScene* PR_NULLABLE scene; // the scene of the last frame, which its list plays
    PrDrawList* list;
    RawArray<PrLayoutSceneScroll> scrolls;

    PrApp* PR_NULLABLE app;
    PrGpuDevice* PR_NULLABLE device;
    PrGpuSwapChain* PR_NULLABLE swapChain;
    PrDrawRenderer* PR_NULLABLE renderer;
    PrDrawFrame* PR_NULLABLE frame;

    // Where the swap chain holds more than 8-bit sRGB: the texture of half floats that the list renders into, and the
    // shader that maps it to the display.
    PrGpuTexture rendered;
    PrDrawDisplay* PR_NULLABLE display;
};

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

// Declares a round: a surface on the app's ground that fills the viewport, and the application's round under it.
void build(void* context, PrUi*, const PrUiRound*) {
    WindowRun& w = *static_cast<WindowRun*>(context);
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .ground = PrKitGround_App,
    };
    PrLayoutNode root = prKitSurface(w.kit, kPrLayoutNodeNone, {}, style);
    w.desc.build(w.desc.context, w.handle, root, w.extent);
}

// Frees what the window made on its device, and the device.
void closeDevice(WindowRun& w) {
    if (w.device != nullptr)
        prGpuTextureDestroy(w.device, w.rendered);
    prDrawDisplayDestroy(w.display);
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.rendered = PrGpuTexture{};
    w.display = nullptr;
    w.frame = nullptr;
    w.renderer = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
}

// Makes the platform's device, a swap chain over the window for HDR, and the renderer. Returns false if the window has
// no device to render with.
bool openDevice(WindowRun& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok ||
            prDrawDisplayCreate(w.device, &w.display) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

// Returns true if the swap chain holds more than 8-bit sRGB, which the list then renders into a texture of its own for.
bool mapsToDisplay(const WindowRun& w) {
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(w.swapChain);
    return info.format != PrGpuFormat_Bgra8 || info.space != PrColorSpace_Srgb;
}

// Returns the texture of half floats that a frame of `extent` renders into, made again when the window grows past it.
PrGpuTexture renderedFor(WindowRun& w, PrUVec2 extent) {
    PrUVec2 held = w.rendered.id != 0 ? prGpuTextureExtent(w.device, w.rendered) : PrUVec2{};
    if (w.rendered.id == 0 || extent.x > held.x || extent.y > held.y) {
        prGpuTextureDestroy(w.device, w.rendered);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        w.rendered = PrGpuTexture{};
        prGpuTextureCreate(w.device, &desc, &w.rendered);
    }
    return w.rendered;
}

// Renders the list into the swap chain's texture at `extent` target pixels and presents it, clearing to the canvas.
void render(WindowRun& w, PrUVec2 extent, PrColor canvas, const PrAppDisplay& display) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    bool mapped = mapsToDisplay(w);
    PrGpuTexture target = mapped ? renderedFor(w, extent) : drawable;
    PrPixelFormat format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8);
    PrDrawTarget into{.extent = extent, .format = format, .commands = commands};
    prDrawFramePrepare(w.frame, w.list, &into);
    // The half floats rest in the accesses of shader reads between frames, and the drawable holds nothing yet.
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_ShaderRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, canvas);
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(w.swapChain);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(w.display, encoder, target, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    WindowRun& w = *static_cast<WindowRun*>(context);
    w.app = app;
    prUiSetApp(w.ui, app);
    if (!openDevice(w)) {
        w.status = PrStatus_NotSupported;
        prAppStop(app);
        return;
    }
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp*, const PrAppFrame* f) {
    WindowRun& w = *static_cast<WindowRun*>(context);
    double started = prTimeMonotonicMilliseconds();
    // A lost device is made again, with everything on it. The frame draws from the model, as every frame does.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    // Waiting first lets the frame read the latest input, and then wait for nothing once it is built.
    prGpuSwapChainWait(w.swapChain);

    PrKitThemeVariant variant = variantOf(f->preferences.colorScheme);
    if (w.desc.theme != nullptr)
        variant = w.desc.theme(w.desc.context, w.handle, &f->preferences);
    if (variant != w.variant) {
        w.variant = variant;
        PrKitTheme theme = prKitThemeStudio(variant, w.fonts);
        prKitSetTheme(w.kit, &theme);
    }
    if (w.desc.beginFrame != nullptr)
        w.desc.beginFrame(w.desc.context, w.handle, f);
    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    w.extent = f->extent;
    double waited = prTimeMonotonicMilliseconds();
    uint32_t rounds = prUiFrame(w.ui, &desc, build, &w);
    if (w.desc.laidOut != nullptr && w.desc.laidOut(w.desc.context, w.handle))
        prUiRequestFrame(w.ui);
    double built = prTimeMonotonicMilliseconds();

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // pass clears to the canvas's color, so the list leaves the canvas unfilled.
    const PrLayoutScene* scene = prLayoutTreeScene(w.tree);
    prLayoutSceneRelease(w.scene);
    w.scene = scene;
    w.scrolls.resize(prLayoutSceneScrolls(w.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(w.tree, scene, w.scrolls.data, w.scrolls.count);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = w.scrolls.data;
    options.scrollCount = w.scrolls.count;
    options.canvasFilled = true;
    double sceneTaken = prTimeMonotonicMilliseconds();
    float ratio = f->display.pixelRatio;
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, w.list, &options);
    prDrawListPopTransform(w.list);
    PrColor canvas = prColorFromColor16(prLayoutSceneCanvasColor(scene));

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    double played = prTimeMonotonicMilliseconds();
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a}, f->display);
    if (w.desc.drawn != nullptr)
        w.desc.drawn(w.desc.context, w.handle, w.list, extent, canvas);
    double end = prTimeMonotonicMilliseconds();
    if (w.desc.trace != nullptr) {
        PrStudioWindowFrameTimes times{
            .extent = f->extent,
            .rounds = rounds,
            .total = end - started,
            .wait = waited - started,
            .build = built - waited,
            .scene = sceneTaken - built,
            .play = played - sceneTaken,
            .render = end - played,
            .gap = w.lastFrame != 0 ? started - w.lastFrame : 0,
            .inputWait = w.firstEvent != 0 ? started - w.firstEvent : 0,
        };
        w.desc.trace(w.desc.context, &times);
    }
    w.lastFrame = end;
    w.firstEvent = 0;
    return prUiOutput(w.ui);
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    WindowRun& w = *static_cast<WindowRun*>(context);
    if (w.firstEvent == 0)
        w.firstEvent = prTimeMonotonicMilliseconds();
    if (w.desc.event == nullptr || !w.desc.event(w.desc.context, w.handle, e))
        prUiPushEvent(w.ui, e);
    prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<WindowRun*>(context));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrStudioWindow : WindowRun {};

PrStatus prStudioWindowRun(const PrStudioWindowDesc* desc) {
    PrStudioWindow* w = allocZeroed<PrStudioWindow>();
    w->handle = w;
    w->desc = *desc;
    w->variant = PrKitThemeVariant_Dark;
    w->fonts = prFontSetCreate(nullptr);
    if (prFontBundleInstall(w->fonts, PrFontBundle_Noto, desc->fontDirectory) == 0) {
        prFontSetDestroy(w->fonts);
        prMemoryFree(w);
        return PrStatus_NotFound;
    }
    w->shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = w->shaper};
    w->tree = prLayoutTreeCreate(&treeDesc);
    w->ui = prUiCreate(w->tree);
    PrKitTheme theme = prKitThemeStudio(w->variant, w->fonts);
    w->kit = prKitCreate(w->ui, &theme);
    w->list = prDrawListCreate();

    PrAppDesc appDesc{.title = desc->title, .extent = desc->extent};
    w->app = prAppCreate(PrAppBackend_Default, &appDesc);
    w->status = PrStatus_Ok;
    bool opened = desc->open == nullptr || desc->open(desc->context, w);
    if (opened) {
        PrAppHandler handler{w, start, frame, event, shutdown};
        prAppRun(w->app, &handler);
    } else {
        w->status = PrStatus_InvalidState;
    }
    if (desc->close != nullptr)
        desc->close(desc->context, w);
    PrStatus status = w->status;
    prAppDestroy(w->app);
    prDrawListRelease(w->list);
    prLayoutSceneRelease(w->scene);
    w->scrolls.reset();
    prKitDestroy(w->kit);
    prUiDestroy(w->ui);
    prLayoutTreeDestroy(w->tree);
    prTextShaperDestroy(w->shaper);
    prFontSetDestroy(w->fonts);
    prMemoryFree(w);
    return status;
}

PrApp* prStudioWindowApp(const PrStudioWindow* window) {
    return window->app;
}

PrKit* prStudioWindowKit(const PrStudioWindow* window) {
    return window->kit;
}

PrUi* prStudioWindowUi(const PrStudioWindow* window) {
    return window->ui;
}

PrFontSet* prStudioWindowFonts(const PrStudioWindow* window) {
    return window->fonts;
}

PrTextShaper* prStudioWindowShaper(const PrStudioWindow* window) {
    return window->shaper;
}
