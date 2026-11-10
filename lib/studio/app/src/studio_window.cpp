// Studio's windows: the kit and what it draws with, the swap chain on the application's device, and the frames that run
// the application's rounds and render them.

#include "pr/studio_window.h"

#include "pr/alloc.h"
#include "pr/kit_surface.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/time.h"

#include "studio/app/src/studio_app_shared.h"

namespace pr_impl {

namespace {

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

// Declares a round: a surface on the app background that fills the viewport, and the application's round under it.
void build(void* context, PrUi*, const PrUiRound*) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    // Over a backdrop, the window's surface leaves the canvas transparent, so that the backdrop shows.
    bool opaque = w->backdrop == PrAppWindowBackdrop_Opaque;
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .background = PrKitBackground(opaque ? PrKitBackground_App : PrKitBackground_None),
    };
    PrLayoutNode window = prKitSurface(w->kit, kPrLayoutNodeNone, {}, style);
    // A Custom title bar shows while the frame has one, which a fullscreen window does not.
    if (w->desc.decoration != PrAppWindowDecoration_Custom || w->titleBar.height == 0) {
        w->desc.build(w->desc.context, w, window, w->extent);
        return;
    }
    // The application's title bar is at the top of the surface, and the application fills the rest.
    PrKitTitleBar titleBar{
        .bar = w->titleBar,
        .maximized = prAppWindowState(w->appWindow) == PrAppWindowState_Maximized,
    };
    PrLayoutNode bar = prKitTitleBar(w->kit, window, &titleBar);
    if (w->desc.titleBar != nullptr)
        w->desc.titleBar(w->desc.context, w, bar);
    PrKitBoxStyle rest{.place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode root = prKitBox(w->kit, window, PrKitBox{}, rest);
    // With nothing focused, a key starts at the application's root, under the title bar, as it does without one.
    prUiSetKeyRoot(w->ui, root);
    w->desc.build(w->desc.context, w, root, w->extent);
}

// Makes the window's swap chain with the desc's intent, and its frame, on the application's device, which is made again
// first if it was lost. Returns false if there is no device to render with.
bool openSwapChain(PrStudioWindow* w) {
    PrStudioApp* app = w->studio;
    if ((app->device == nullptr || prGpuDeviceIsLost(app->device)) && !studioAppMakeDevice(app))
        return false;
    // A window over a backdrop presents alpha, which costs the compositor a blend, so that a window whose backdrop
    // changes to or from Opaque makes its swap chain again.
    bool transparent = w->backdrop != PrAppWindowBackdrop_Opaque;
    if (w->swapChain != nullptr && w->generation == app->generation && w->transparent == transparent)
        return true;
    studioWindowCloseDevice(w);
    PrAppSurface surface{};
    PrGpuSwapChainFlags flags = transparent ? PrGpuSwapChainFlags(PrGpuSwapChainFlag_Transparent) : 0;
    PrGpuSwapChainDesc desc{.intent = w->desc.intent, .flags = flags};
    w->transparent = transparent;
    if (prAppWindowSurface(w->appWindow, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(app->device, &surface, &desc, &w->swapChain) != PrStatus_Ok) {
        studioWindowCloseDevice(w);
        return false;
    }
    w->frame = prDrawFrameCreate(app->renderer);
    w->generation = app->generation;
    return true;
}

// Returns true if the swap chain holds more than 8-bit sRGB, which the list then renders into a texture of its own for.
// The swap chain chooses its format for the window's display, and chooses again if the display changes, so a frame asks
// after it acquires its texture.
bool mapsToDisplay(const PrStudioWindow* w) {
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(w->swapChain);
    return info.format != PrGpuFormat_Bgra8 || info.space != PrColorSpace_Srgb;
}

// Returns the texture of half floats that a frame of `extent` renders into, made again when the window grows past it.
PrGpuTexture renderedFor(PrStudioWindow* w, PrUVec2 extent) {
    PrGpuDevice* device = w->studio->device;
    PrUVec2 held = w->rendered.id != 0 ? prGpuTextureExtent(device, w->rendered) : PrUVec2{};
    if (w->rendered.id == 0 || extent.x > held.x || extent.y > held.y) {
        prGpuTextureDestroy(device, w->rendered);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        w->rendered = PrGpuTexture{};
        prGpuTextureCreate(device, &desc, &w->rendered);
    }
    return w->rendered;
}

// Renders the list into the swap chain's texture at `extent` target pixels and presents it, clearing to the canvas.
void render(PrStudioWindow* w, PrUVec2 extent, PrColor canvas, const PrAppDisplay& display) {
    PrGpuDevice* device = w->studio->device;
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w->swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(device, nullptr, &commands))
        return;
    bool mapped = mapsToDisplay(w);
    // If the swap chain changes to 8-bit sRGB, for example after a move to an SDR display, the half-float texture is
    // freed.
    if (!mapped && w->rendered.id != 0) {
        prGpuTextureDestroy(device, w->rendered);
        w->rendered = PrGpuTexture{};
    }
    PrGpuTexture target = mapped ? renderedFor(w, extent) : drawable;
    PrPixelFormat format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8);
    PrDrawTarget into{.extent = extent, .format = format, .commands = commands};
    prDrawFramePrepare(w->frame, w->list, &into);
    // Between frames, the half-float texture is left in the fragment-read state. The display shader reads it only in
    // the fragment stage, so no barrier waits on the vertex stage. The drawable holds nothing yet.
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_FragmentRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, canvas);
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(w->frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_FragmentRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(w->swapChain);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(w->studio->display, encoder, target, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w->swapChain, commands);
    prGpuCommandsSubmit(commands);
}

// Frees what the window made apart from its device and its platform window, and the window's state.
void freeWindow(PrStudioWindow* w) {
    prDrawListRelease(w->list);
    prLayoutSceneRelease(w->scene);
    w->scrolls.reset();
    prKitDestroy(w->kit);
    prUiDestroy(w->ui);
    prLayoutTreeDestroy(w->tree);
    prMemoryFree(w);
}

// Closes the window after the application's `close`, and frees it.
void destroyWindow(PrStudioWindow* w) {
    if (w->desc.close != nullptr)
        w->desc.close(w->desc.context, w);
    studioWindowCloseDevice(w);
    prAppWindowDestroy(w->appWindow);
    studioAppWindowClosed(w->studio, w);
    freeWindow(w);
}

// Ends one of the window's callbacks, and closes the window if the callback closed it. The callback uses the window no
// more after this.
void endCallback(PrStudioWindow* w) {
    if (--w->callbacks == 0 && w->closing)
        destroyWindow(w);
}

// Makes the swap chain as the window opens, and opens the application's window. Either failure closes the window. The
// window closes through its own close, which calls the application's, so it opens as the platform sees it either way.
bool open(void* context, PrAppWindow*, const PrAppFrame* first) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    ++w->callbacks;
    w->backdrop = first->backdrop;
    if (!openSwapChain(w)) {
        w->status = PrStatus_NotSupported;
        w->closing = true;
    } else if (w->desc.open != nullptr && !w->desc.open(w->desc.context, w)) {
        w->status = PrStatus_InvalidState;
        w->closing = true;
    } else {
        prAppWindowRequestFrame(w->appWindow);
    }
    endCallback(w);
    return true;
}

// Runs the frame's rounds and renders them, and returns what the ui reports.
PrAppOutput renderFrame(PrStudioWindow* w, const PrAppFrame* f) {
    PrTimePerf started = prTimePerfNow();
    w->backdrop = f->backdrop;
    // A lost device is made again, with everything on it. The frame draws from the model, as every frame does.
    if (!openSwapChain(w))
        return {};
    // Waiting first lets the frame read the latest input, and then wait for nothing once it is built.
    prGpuSwapChainWait(w->swapChain);

    PrKitThemeVariant variant = variantOf(f->preferences.colorScheme);
    if (w->desc.theme != nullptr)
        variant = w->desc.theme(w->desc.context, w, &f->preferences);
    if (variant != w->variant) {
        w->variant = variant;
        PrKitTheme theme = prKitThemeStudio(variant, w->studio->desc.fonts);
        prKitSetTheme(w->kit, &theme);
    }
    if (w->desc.beginFrame != nullptr)
        w->desc.beginFrame(w->desc.context, w, f);
    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    w->extent = f->extent;
    w->titleBar = f->titleBar;
    PrTimePerf waited = prTimePerfNow();
    uint32_t rounds = prUiFrame(w->ui, &desc, build, w);
    if (w->desc.laidOut != nullptr && w->desc.laidOut(w->desc.context, w))
        prUiRequestFrame(w->ui);
    PrTimePerf built = prTimePerfNow();

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // pass clears to the canvas's color, so the list leaves the canvas unfilled.
    const PrLayoutScene* scene = prLayoutTreeScene(w->tree);
    prLayoutSceneRelease(w->scene);
    w->scene = scene;
    w->scrolls.resize(prLayoutSceneScrolls(w->tree, scene, nullptr, 0));
    prLayoutSceneScrolls(w->tree, scene, w->scrolls.data, w->scrolls.count);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = w->scrolls.data;
    options.scrollCount = w->scrolls.count;
    options.canvasFilled = true;
    PrTimePerf sceneTaken = prTimePerfNow();
    float ratio = f->display.pixelRatio;
    prDrawListClear(w->list);
    prDrawListPushTransformAffine2(w->list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, w->list, &options);
    prDrawListPopTransform(w->list);
    PrColor canvas = prColorFromColor16(prLayoutSceneCanvasColor(scene));

    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    PrTimePerf played = prTimePerfNow();
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a}, f->display);
    if (w->desc.drawn != nullptr)
        w->desc.drawn(w->desc.context, w, w->list, extent, canvas);
    PrTimePerf end = prTimePerfNow();
    if (w->desc.trace != nullptr) {
        PrStudioWindowFrameTimes times{
            .extent = f->extent,
            .rounds = rounds,
            .total = prTimePerfMilliseconds(started, end),
            .wait = prTimePerfMilliseconds(started, waited),
            .build = prTimePerfMilliseconds(waited, built),
            .scene = prTimePerfMilliseconds(built, sceneTaken),
            .play = prTimePerfMilliseconds(sceneTaken, played),
            .render = prTimePerfMilliseconds(played, end),
            .gap = w->lastFrame.ticks != 0 ? prTimePerfMilliseconds(w->lastFrame, started) : 0,
            .inputWait = w->firstEvent.ticks != 0 ? prTimePerfMilliseconds(w->firstEvent, started) : 0,
        };
        w->desc.trace(w->desc.context, &times);
    }
    w->lastFrame = end;
    w->firstEvent = PrTimePerf{};
    return prUiOutput(w->ui);
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    ++w->callbacks;
    PrAppOutput output = w->closing ? PrAppOutput{} : renderFrame(w, f);
    endCallback(w);
    return output;
}

// Gives the event to the application, and otherwise to the window's ui. A request to close that the application does
// not take closes the window.
void event(void* context, PrAppWindow*, const PrAppEvent* e) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    ++w->callbacks;
    if (w->firstEvent.ticks == 0)
        w->firstEvent = prTimePerfNow();
    bool taken = w->desc.event != nullptr && w->desc.event(w->desc.context, w, e);
    if (!taken && e->kind == PrAppEventKind_CloseRequested)
        w->closing = true;
    else if (!taken)
        prUiPushEvent(w->ui, e);
    if (!w->closing)
        prAppWindowRequestFrame(w->appWindow);
    endCallback(w);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prStudioWindowCreate(PrStudioApp* app, const PrStudioWindowDesc* desc, PrStudioWindow* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrStudioWindow* w = allocZeroed<PrStudioWindow>();
    w->studio = app;
    w->desc = *desc;
    w->status = PrStatus_Ok;
    w->variant = PrKitThemeVariant_Dark;
    PrLayoutTreeDesc treeDesc{.shaper = app->shaper};
    w->tree = prLayoutTreeCreate(&treeDesc);
    w->ui = prUiCreate(w->tree);
    PrKitTheme theme = prKitThemeStudio(w->variant, app->desc.fonts);
    w->kit = prKitCreate(w->ui, &theme);
    w->list = prDrawListCreate();
    PrAppWindowDesc windowDesc{
        .title = desc->title,
        .extent = desc->extent,
        .minExtent = desc->minExtent,
        .maxExtent = desc->maxExtent,
        .role = desc->role,
        .parent = desc->parent != nullptr ? desc->parent->appWindow : nullptr,
        .modal = desc->modal,
        .decoration = desc->decoration,
        .backdrop = desc->backdrop,
        .hidden = desc->hidden,
        .pointerPassthrough = desc->pointerPassthrough,
        .placement = desc->placement,
        .position = desc->position,
        .popup = desc->popup,
        .handler = {.context = w, .open = open, .frame = frame, .event = event},
    };
    PrStatus status = prAppWindowCreate(app->app, &windowDesc, &w->appWindow);
    if (status != PrStatus_Ok) {
        freeWindow(w);
        return status;
    }
    prUiSetWindow(w->ui, w->appWindow);
    app->windows.push(w);
    *out = w;
    return PrStatus_Ok;
}

void prStudioWindowDestroy(PrStudioWindow* PR_NULLABLE window) {
    if (window == nullptr)
        return;
    if (window->callbacks != 0)
        window->closing = true;
    else
        destroyWindow(window);
}

PrStudioApp* prStudioWindowStudioApp(const PrStudioWindow* window) {
    return window->studio;
}

PrApp* prStudioWindowApp(const PrStudioWindow* window) {
    return window->studio->app;
}

PrAppWindow* prStudioWindowAppWindow(const PrStudioWindow* window) {
    return window->appWindow;
}

PrKit* prStudioWindowKit(const PrStudioWindow* window) {
    return window->kit;
}

PrUi* prStudioWindowUi(const PrStudioWindow* window) {
    return window->ui;
}

PrFontSet* prStudioWindowFonts(const PrStudioWindow* window) {
    return window->studio->desc.fonts;
}

PrTextShaper* prStudioWindowShaper(const PrStudioWindow* window) {
    return window->studio->shaper;
}
