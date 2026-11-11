// Studio's windows: the kit and what it draws with, the swap chain on the application's device, the frames that run the
// application's rounds and render them, and the windows of the surfaces that float over the application.

#include "pr/studio_window.h"

#include "pr/alloc.h"
#include "pr/kit_surface.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/mem.h"
#include "pr/time.h"

#include "studio/app/src/studio_app_shared.h"

namespace pr_impl {

namespace {

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

// Declares a round: a surface on the app background that fills the viewport, and the application's round under it. The
// background reaches under the system's bars, and the surface pads its content to the safe area.
void build(void* context, PrUi*, const PrUiRound*) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    // Over a backdrop, the window's surface leaves the canvas transparent, so that the backdrop shows.
    bool opaque = w->backdrop == PrAppWindowBackdrop_Opaque;
    PrAppInsets safe = w->safeArea;
    PrKitSurfaceStyle style{
        .box = {
            .overflow = PrKitOverflow_Clip,
            .padding = {safe.top, safe.right, safe.bottom, safe.left},
            .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
        },
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

// Makes the target's swap chain for `appWindow` with `intent`, presenting alpha if `transparent`, and its frame, on the
// application's device, which is made again first if it was lost. Returns false if there is no device to render with.
bool openTarget(StudioAppState* app, StudioWindowTarget* target, PrAppWindow* appWindow, PrGpuSwapChainIntent intent,
        bool transparent) {
    if ((app->device == nullptr || prGpuDeviceIsLost(app->device)) && !studioAppMakeDevice(app))
        return false;
    // Presenting alpha costs the compositor a blend, so that a change to or from it makes the swap chain again.
    if (target->swapChain != nullptr && target->generation == app->generation && target->transparent == transparent)
        return true;
    studioWindowTargetClose(app, target);
    PrSurface surface{};
    PrGpuSwapChainFlags flags = transparent ? PrGpuSwapChainFlags(PrGpuSwapChainFlag_Transparent) : 0;
    PrGpuSwapChainDesc desc{.intent = intent, .flags = flags};
    target->transparent = transparent;
    if (prAppWindowSurface(appWindow, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(app->device, &surface, &desc, &target->swapChain) != PrStatus_Ok) {
        studioWindowTargetClose(app, target);
        return false;
    }
    target->frame = prDrawFrameCreate(app->renderer);
    target->generation = app->generation;
    return true;
}

// Makes the window's swap chain with the desc's intent. A window over a backdrop presents alpha.
bool openSwapChain(PrStudioWindow* w) {
    bool transparent = w->backdrop != PrAppWindowBackdrop_Opaque;
    return openTarget(w->studio, &w->target, w->appWindow, w->desc.intent, transparent);
}

// Returns true if the swap chain holds more than 8-bit sRGB, which the list then renders into a texture of its own for.
// Otherwise writes the drawable's pixel format to `direct`. The swap chain chooses its format for the window's display,
// and chooses again if the display changes, so a frame asks after it acquires its texture.
bool mapsToDisplay(const StudioWindowTarget& target, PrPixelFormat* direct) {
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(target.swapChain);
    return info.space != PrColorSpace_Srgb || !prGpuFormatToPixelFormat(info.format, direct) ||
        *direct == PrPixelFormat_RgbaF16;
}

// Returns the texture of half floats that a frame of `extent` renders into, made again when the window grows past it.
PrGpuTexture renderedFor(StudioAppState* app, StudioWindowTarget& target, PrUVec2 extent) {
    PrGpuDevice* device = app->device;
    PrUVec2 held = target.rendered.id != 0 ? prGpuTextureExtent(device, target.rendered) : PrUVec2{};
    if (target.rendered.id == 0 || extent.x > held.x || extent.y > held.y) {
        prGpuTextureDestroy(device, target.rendered);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        target.rendered = PrGpuTexture{};
        prGpuTextureCreate(device, &desc, &target.rendered);
    }
    return target.rendered;
}

// Renders `list` into the target's swap chain at `extent` target pixels and presents it, clearing to `canvas`, which is
// premultiplied.
void render(StudioAppState* studio, StudioWindowTarget& target, PrDrawList* list, PrUVec2 extent, PrColor canvas,
        const PrAppDisplay& display) {
    PrGpuDevice* device = studio->device;
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(target.swapChain, extent, &frame) != PrStatus_Ok ||
            !prGpuCommandsBegin(device, nullptr, &commands))
        return;
    PrPixelFormat direct = PrPixelFormat_Bgra8;
    bool mapped = mapsToDisplay(target, &direct);
    if (mapped && studio->display == nullptr && prDrawDisplayCreate(device, &studio->display) != PrStatus_Ok) {
        prGpuCommandsSubmit(commands);
        return;
    }
    // If the swap chain changes to 8-bit sRGB, for example after a move to an SDR display, the half-float texture is
    // freed.
    if (!mapped && target.rendered.id != 0) {
        prGpuTextureDestroy(device, target.rendered);
        target.rendered = PrGpuTexture{};
    }
    PrGpuTexture into = mapped ? renderedFor(studio, target, extent) : frame.color;
    PrPixelFormat format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : direct);
    PrDrawTarget drawn{.extent = extent, .format = format, .commands = commands};
    prDrawFramePrepare(target.frame, list, &drawn);
    // Between frames, the half-float texture is left in the fragment-read state. The display shader reads it only in
    // the fragment stage, so no barrier waits on the vertex stage. The drawable holds nothing yet.
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, into, all, mapped ? PrGpuAccessFlag_FragmentRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(into, PrGpuLoad_Clear, canvas);
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(target.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, into, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_FragmentRead);
        prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_Discard,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(target.swapChain);
        PrGpuPassDesc shown = prGpuPassDescColor(frame.color, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(studio->display, encoder, into, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, frame.color, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(target.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

// ---------------------------------------------------------------------------------------------------------------------
// Surfaces

// Returns the platform window that shows `viewport` of the window's context, or null if none does.
PrAppWindow* PR_NULLABLE windowOfViewport(const StudioWindowState* w, PrUiViewport viewport) {
    if (viewport == kPrUiViewportMain)
        return w->appWindow;
    for (uint32_t i = 0; i < w->surfaces.count; ++i) {
        if (w->surfaces.data[i]->info.viewport == viewport)
            return w->surfaces.data[i]->appWindow;
    }
    return nullptr;
}

// Returns the platform window of a surface of the window's context (PrUiHost).
PrAppWindow* PR_NULLABLE surfaceWindow(void* PR_NULLABLE context, PrUiViewport viewport) {
    return windowOfViewport(static_cast<const StudioWindowState*>(context), viewport);
}

// Returns the extent of the work area of the display that shows the window, in px, or zero if the platform does not
// report it.
PrVec2 workExtentOf(const PrStudioWindow* w) {
    PrAppDisplayPosition at{};
    PrAppDisplayInfo display{};
    if (!prAppWindowPosition(w->appWindow, &at) || !prAppDisplayInfoFromId(w->studio->app, at.display, &display))
        return PrVec2{};
    return PrVec2{display.workArea.right - display.workArea.left, display.workArea.bottom - display.workArea.top};
}

// Returns the extent of a surface's window, in px, for the surface's extent.
PrUVec2 surfaceExtentOf(PrVec2 extent) {
    uint32_t x = uint32_t(extent.x + 0.5f);
    uint32_t y = uint32_t(extent.y + 0.5f);
    return PrUVec2{x > 0 ? x : 1, y > 0 ? y : 1};
}

bool samePlacement(const PrAppPopupPlacement& a, const PrAppPopupPlacement& b) {
    return prMemEquals(&a.anchor, &b.anchor, sizeof a.anchor) && a.side == b.side && a.align == b.align &&
        a.adjust == b.adjust && a.offset.x == b.offset.x && a.offset.y == b.offset.y;
}

bool surfaceOpen(void*, PrAppWindow* window, const PrAppFrame*) {
    prAppWindowRequestFrame(window);
    return true;
}

// Draws the surface's root as the window's last paint drew it.
PrAppOutput surfaceFrame(void* context, PrAppWindow*, const PrAppFrame* f) {
    StudioWindowSurface* s = static_cast<StudioWindowSurface*>(context);
    StudioWindowState* w = s->window;
    // The surface's window shows the cursor of the window's last frame and answers drags as it does. The window's own
    // output holds the rest, the caret included.
    PrAppOutput output{.cursor = w->output.cursor, .dropOperation = w->output.dropOperation};
    s->cursor = output.cursor;
    s->dropOperation = output.dropOperation;
    if (w->closing || !openTarget(w->studio, &s->target, s->appWindow, w->desc.intent, false))
        return output;
    prGpuSwapChainWait(s->target.swapChain);
    s->drawn = prLayoutNodeDrawingRevision(w->tree, s->info.root);
    float ratio = f->display.pixelRatio;
    prDrawListClear(s->list);
    prDrawListPushTransformAffine2(s->list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutNodeDraw(w->tree, s->info.root, s->list);
    prDrawListPopTransform(s->list);
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w->studio, s->target, s->list, extent, PrColor{0, 0, 0, 1}, f->display);
    return output;
}

// Gives the event to the window's context in the surface's viewport, and asks the window for the frame that routes it.
// The surface closes when its widget's state closes it, so a request to close it is not taken.
void surfaceEvent(void* context, PrAppWindow*, const PrAppEvent* e) {
    StudioWindowSurface* s = static_cast<StudioWindowSurface*>(context);
    StudioWindowState* w = s->window;
    if (w->closing || e->kind == PrAppEventKind_CloseRequested)
        return;
    if (w->firstEvent.ticks == 0)
        w->firstEvent = prTimePerfNow();
    prUiPushEvent(w->ui, s->info.viewport, e);
    prAppWindowRequestFrame(w->appWindow);
}

// Closes the surface's window and frees it.
void destroySurface(PrStudioWindow* w, uint32_t index) {
    StudioWindowSurface* s = w->surfaces.data[index];
    studioWindowTargetClose(w->studio, &s->target);
    prAppWindowDestroy(s->appWindow);
    prDrawListRelease(s->list);
    prMemoryFree(s);
    w->surfaces.erase(index);
}

// Returns the window of the surface of `viewport`, or null.
StudioWindowSurface* PR_NULLABLE surfaceOf(PrStudioWindow* w, PrUiViewport viewport) {
    for (uint32_t i = 0; i < w->surfaces.count; ++i) {
        if (w->surfaces.data[i]->info.viewport == viewport)
            return w->surfaces.data[i];
    }
    return nullptr;
}

// Opens a window for the surface that `info` reports, against its parent's window. Returns null if the platform opens
// none.
StudioWindowSurface* PR_NULLABLE openSurface(PrStudioWindow* w, const PrUiViewportInfo& info, PrAppWindow* parent) {
    StudioWindowSurface* s = allocZeroed<StudioWindowSurface>();
    s->window = w;
    s->info = info;
    s->list = prDrawListCreate();
    bool dialog = info.kind == PrUiSurfaceKind_Dialog;
    PrAppWindowDesc desc{
        .extent = surfaceExtentOf(info.extent),
        .role = PrAppWindowRole(dialog ? PrAppWindowRole_Dialog : PrAppWindowRole_Popup),
        .parent = parent,
        .modal = info.modal,
        .decoration = PrAppWindowDecoration_None,
        .pointerPassthrough = info.passthrough,
        .placement = PrAppWindowPlacement(dialog ? PrAppWindowPlacement_Center : PrAppWindowPlacement_Default),
        .popup = info.placement,
        .handler = {.context = s, .open = surfaceOpen, .frame = surfaceFrame, .event = surfaceEvent},
    };
    if (prAppWindowCreate(w->studio->app, &desc, &s->appWindow) != PrStatus_Ok) {
        prDrawListRelease(s->list);
        prMemoryFree(s);
        return nullptr;
    }
    w->surfaces.push(s);
    return s;
}

// Matches the surfaces' windows to what the context reported at the frame: closes the windows of the surfaces that are
// gone or that changed kind, and of those whose parent's window closes, opens a window for each new surface, and moves
// and resizes the others. A surface's window draws again if it is new, or if its root's drawing, the cursor or the drop
// answer changed.
void showSurfaces(PrStudioWindow* w) {
    uint32_t count = prUiViewports(w->ui, nullptr, 0);
    w->viewports.resize(count);
    prUiViewports(w->ui, w->viewports.data, count);
    // A surface comes after its parent, so that the parent's window is decided first.
    RawArray<PrUiViewport> kept;
    for (uint32_t i = 0; i < count; ++i) {
        const PrUiViewportInfo& info = w->viewports.data[i];
        StudioWindowSurface* s = surfaceOf(w, info.viewport);
        bool parentKept = info.parent == kPrUiViewportMain;
        for (uint32_t k = 0; k < kept.count && !parentKept; ++k)
            parentKept = kept.data[k] == info.parent;
        if (s != nullptr && parentKept && s->info.kind == info.kind && s->info.modal == info.modal &&
                s->info.passthrough == info.passthrough)
            kept.push(info.viewport);
    }
    for (uint32_t i = w->surfaces.count; i-- > 0;) {
        bool listed = false;
        for (uint32_t k = 0; k < kept.count && !listed; ++k)
            listed = kept.data[k] == w->surfaces.data[i]->info.viewport;
        if (!listed)
            destroySurface(w, i);
    }
    kept.reset();
    for (uint32_t i = 0; i < count; ++i) {
        const PrUiViewportInfo& info = w->viewports.data[i];
        if (info.extent.x <= 0 || info.extent.y <= 0)
            continue;
        StudioWindowSurface* s = surfaceOf(w, info.viewport);
        if (s == nullptr) {
            PrAppWindow* parent = windowOfViewport(w, info.parent);
            s = parent != nullptr ? openSurface(w, info, parent) : nullptr;
            if (s != nullptr)
                prAppWindowRequestFrame(s->appWindow);
            continue;
        }
        PrUVec2 extent = surfaceExtentOf(info.extent);
        PrUVec2 was = surfaceExtentOf(s->info.extent);
        if (extent.x != was.x || extent.y != was.y)
            prAppWindowSetExtent(s->appWindow, extent);
        if (info.kind == PrUiSurfaceKind_Popup && !samePlacement(info.placement, s->info.placement))
            prAppWindowSetPopupPlacement(s->appWindow, &info.placement);
        s->info = info;
        bool changed = prLayoutNodeDrawingRevision(w->tree, info.root) != s->drawn ||
            w->output.cursor != s->cursor || w->output.dropOperation != s->dropOperation;
        if (changed)
            prAppWindowRequestFrame(s->appWindow);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Windows

// Frees what the window made apart from its device and its platform window, and the window's state.
void freeWindow(PrStudioWindow* w) {
    prDrawListRelease(w->list);
    prLayoutSceneRelease(w->scene);
    w->scrolls.reset();
    w->surfaces.reset();
    w->viewports.reset();
    prKitDestroy(w->kit);
    prUiDestroy(w->ui);
    prLayoutTreeDestroy(w->tree);
    prMemoryFree(w);
}

// Closes the window after the application's `close`, and frees it. The surfaces' windows close first.
void destroyWindow(PrStudioWindow* w) {
    if (w->desc.close != nullptr)
        w->desc.close(w->desc.context, w);
    while (w->surfaces.count != 0)
        destroySurface(w, w->surfaces.count - 1);
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
    prGpuSwapChainWait(w->target.swapChain);

    PrKitThemeVariant variant = variantOf(f->preferences.colorScheme);
    PrKitPalette palette = prKitPaletteStudio(variant);
    if (w->desc.palette != nullptr)
        w->desc.palette(w->desc.context, w, &f->preferences, &palette);
    if (!prMemEquals(&palette, &w->kit->theme.palette, sizeof palette)) {
        PrKitTheme theme = prKitThemeStudio(variant, w->studio->desc.fonts);
        theme.palette = palette;
        prKitSetTheme(w->kit, &theme);
    }
    if (w->desc.beginFrame != nullptr)
        w->desc.beginFrame(w->desc.context, w, f);
    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .surfaceExtent = workExtentOf(w),
        .display = f->display,
        .preferences = f->preferences,
    };
    w->extent = f->extent;
    w->titleBar = f->titleBar;
    w->safeArea = f->safeArea;
    PrTimePerf waited = prTimePerfNow();
    uint32_t rounds = prUiFrame(w->ui, &desc, build, w);
    if (w->desc.laidOut != nullptr && w->desc.laidOut(w->desc.context, w))
        prUiRequestFrame(w->ui);
    PrTimePerf built = prTimePerfNow();

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // pass clears to the canvas's color, so the list leaves the canvas unfilled. Taking the scene paints the roots of
    // the surfaces' viewports too, which their windows draw.
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
    if (extent.x != 0 && extent.y != 0) {
        PrColor clear{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a};
        render(w->studio, w->target, w->list, extent, clear, f->display);
    }
    if (w->desc.drawn != nullptr)
        w->desc.drawn(w->desc.context, w, w->list, extent, canvas);
    PrAppOutput output = prUiOutput(w->ui);
    w->output = output;
    showSurfaces(w);
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
    return output;
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
        prUiPushEvent(w->ui, kPrUiViewportMain, e);
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
    w->output.cursor = PrAppCursor_Arrow;
    PrLayoutTreeDesc treeDesc{.shaper = app->shaper};
    w->tree = prLayoutTreeCreate(&treeDesc);
    w->ui = prUiCreate(w->tree);
    // The first frame gives the kit its palette.
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, app->desc.fonts);
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
    // A Desktop application shows each surface of the window's context, such as a menu or a dialog, in a window of its
    // own. Any other draws them inside the window.
    if (prAppWindowModel(app->app) == PrAppWindowModel_Desktop) {
        PrUiHost host{.window = surfaceWindow, .context = w};
        prUiSetHost(w->ui, &host);
    }
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
