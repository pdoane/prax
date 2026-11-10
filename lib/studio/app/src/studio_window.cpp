// Studio's windows: the kit and what it draws with, the swap chain on the application's device, the frames that run the
// application's rounds and render them, and the windows of the portals that float over the application.

#include "pr/studio_window.h"

#include "pr/alloc.h"
#include "pr/kit_panel.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/time.h"

#include "studio/app/src/studio_app_shared.h"

namespace pr_impl {

namespace {

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

// Declares a round: a panel on the app background that fills the viewport, and the application's round under it.
void build(void* context, PrUi*, const PrUiRound*) {
    PrStudioWindow* w = static_cast<PrStudioWindow*>(context);
    // Over a backdrop, the window's panel leaves the canvas transparent, so that the backdrop shows.
    bool opaque = w->backdrop == PrAppWindowBackdrop_Opaque;
    PrKitPanelStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .background = PrKitBackground(opaque ? PrKitBackground_App : PrKitBackground_None),
    };
    PrLayoutNode window = prKitPanel(w->kit, kPrLayoutNodeNone, {}, style);
    if (w->desc.decoration != PrAppWindowDecoration_Custom) {
        w->desc.build(w->desc.context, w, window, w->extent);
        return;
    }
    // The application's title bar is at the top of the panel, and the application fills the rest. While the frame has
    // no title bar, as in fullscreen, the title bar's content stays, in a plain row (prKitTitleBar).
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
    PrAppSurface surface{};
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
// premultiplied. A swap chain that holds more than 8-bit sRGB on a device without the display shader shows nothing.
void render(StudioAppState* studio, StudioWindowTarget& target, PrDrawList* list, PrUVec2 extent, PrColor canvas,
        const PrAppDisplay& display) {
    PrGpuDevice* device = studio->device;
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(target.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(device, nullptr, &commands))
        return;
    PrPixelFormat direct = PrPixelFormat_Bgra8;
    bool mapped = mapsToDisplay(target, &direct);
    PrDrawDisplay* shader = mapped ? studioAppDisplay(studio) : nullptr;
    if (mapped && shader == nullptr) {
        prGpuCommandsSubmit(commands);
        return;
    }
    // If the swap chain changes to 8-bit sRGB, for example after a move to an SDR display, the half-float texture is
    // freed.
    if (!mapped && target.rendered.id != 0) {
        prGpuTextureDestroy(device, target.rendered);
        target.rendered = PrGpuTexture{};
    }
    PrGpuTexture into = mapped ? renderedFor(studio, target, extent) : drawable;
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
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuSwapChainInfo info = prGpuSwapChainInfo(target.swapChain);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(shader, encoder, into, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(target.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

// ---------------------------------------------------------------------------------------------------------------------
// Portals

// Returns the platform window that shows `viewport` of the window's context, or null if none does.
PrAppWindow* PR_NULLABLE windowOfViewport(const StudioWindowState* w, PrUiViewport viewport) {
    if (viewport == kPrUiViewportMain)
        return w->appWindow;
    for (uint32_t i = 0; i < w->portals.count; ++i) {
        if (w->portals.data[i]->info.viewport == viewport)
            return w->portals.data[i]->appWindow;
    }
    return nullptr;
}

// Returns the platform window of a portal of the window's context (PrUiHost).
PrAppWindow* PR_NULLABLE portalWindow(void* PR_NULLABLE context, PrUiViewport viewport) {
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

// Returns the extent of a portal's window, in px, for the portal's extent.
PrUVec2 portalExtentOf(PrVec2 extent) {
    uint32_t x = uint32_t(extent.x + 0.5f);
    uint32_t y = uint32_t(extent.y + 0.5f);
    return PrUVec2{x > 0 ? x : 1, y > 0 ? y : 1};
}

bool samePlacement(const PrAppPopupPlacement& a, const PrAppPopupPlacement& b) {
    return prMemEquals(&a.anchor, &b.anchor, sizeof a.anchor) && a.side == b.side && a.align == b.align &&
        a.adjust == b.adjust && a.offset.x == b.offset.x && a.offset.y == b.offset.y;
}

bool portalOpen(void*, PrAppWindow* window, const PrAppFrame*) {
    prAppWindowRequestFrame(window);
    return true;
}

// Draws the portal's root as the window's last paint drew it.
PrAppOutput portalFrame(void* context, PrAppWindow*, const PrAppFrame* f) {
    StudioWindowPortal* s = static_cast<StudioWindowPortal*>(context);
    StudioWindowState* w = s->window;
    // The portal's window shows the cursor of the window's last frame and answers drags as it does. The window's own
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

// Gives the event to the window's context in the portal's viewport, and asks the window for the frame that routes it.
// The portal closes when its widget's state closes it, so a request to close it is not taken.
void portalEvent(void* context, PrAppWindow*, const PrAppEvent* e) {
    StudioWindowPortal* s = static_cast<StudioWindowPortal*>(context);
    StudioWindowState* w = s->window;
    if (w->closing || e->kind == PrAppEventKind_CloseRequested)
        return;
    if (w->firstEvent.ticks == 0)
        w->firstEvent = prTimePerfNow();
    prUiPushEvent(w->ui, s->info.viewport, e);
    prAppWindowRequestFrame(w->appWindow);
}

// Closes the portal's window and frees it.
void destroyPortal(PrStudioWindow* w, uint32_t index) {
    StudioWindowPortal* s = w->portals.data[index];
    studioWindowTargetClose(w->studio, &s->target);
    prAppWindowDestroy(s->appWindow);
    prDrawListRelease(s->list);
    prMemoryFree(s);
    w->portals.erase(index);
}

// Returns the window of the portal of `viewport`, or null.
StudioWindowPortal* PR_NULLABLE portalOf(PrStudioWindow* w, PrUiViewport viewport) {
    for (uint32_t i = 0; i < w->portals.count; ++i) {
        if (w->portals.data[i]->info.viewport == viewport)
            return w->portals.data[i];
    }
    return nullptr;
}

// Opens a window for the portal that `info` reports, against its parent's window. Returns null if the platform opens
// none.
StudioWindowPortal* PR_NULLABLE openPortal(PrStudioWindow* w, const PrUiViewportInfo& info, PrAppWindow* parent) {
    StudioWindowPortal* s = allocZeroed<StudioWindowPortal>();
    s->window = w;
    s->info = info;
    s->list = prDrawListCreate();
    bool dialog = info.kind == PrUiPortalKind_Dialog;
    PrAppWindowDesc desc{
        .extent = portalExtentOf(info.extent),
        .role = PrAppWindowRole(dialog ? PrAppWindowRole_Dialog : PrAppWindowRole_Popup),
        .parent = parent,
        .modal = info.modal,
        .decoration = PrAppWindowDecoration_None,
        .pointerPassthrough = info.passthrough,
        .placement = PrAppWindowPlacement(dialog ? PrAppWindowPlacement_Center : PrAppWindowPlacement_Default),
        .popup = info.placement,
        .handler = {.context = s, .open = portalOpen, .frame = portalFrame, .event = portalEvent},
    };
    if (prAppWindowCreate(w->studio->app, &desc, &s->appWindow) != PrStatus_Ok) {
        prDrawListRelease(s->list);
        prMemoryFree(s);
        return nullptr;
    }
    w->portals.push(s);
    return s;
}

// Matches the portals' windows to what the context reported at the frame: closes the windows of the portals that are
// gone or that changed kind, and of those whose parent's window closes, opens a window for each new portal, and moves
// and resizes the others. A portal's window draws again if it is new, or if its root's drawing, the cursor or the drop
// answer changed.
void showPortals(PrStudioWindow* w) {
    uint32_t count = prUiViewports(w->ui, nullptr, 0);
    w->viewports.resize(count);
    prUiViewports(w->ui, w->viewports.data, count);
    // A portal comes after its parent, so that the parent's window is decided first.
    RawArray<PrUiViewport> kept;
    for (uint32_t i = 0; i < count; ++i) {
        const PrUiViewportInfo& info = w->viewports.data[i];
        StudioWindowPortal* s = portalOf(w, info.viewport);
        bool parentKept = info.parent == kPrUiViewportMain;
        for (uint32_t k = 0; k < kept.count && !parentKept; ++k)
            parentKept = kept.data[k] == info.parent;
        if (s != nullptr && parentKept && s->info.kind == info.kind && s->info.modal == info.modal &&
                s->info.passthrough == info.passthrough)
            kept.push(info.viewport);
    }
    for (uint32_t i = w->portals.count; i-- > 0;) {
        bool listed = false;
        for (uint32_t k = 0; k < kept.count && !listed; ++k)
            listed = kept.data[k] == w->portals.data[i]->info.viewport;
        if (!listed)
            destroyPortal(w, i);
    }
    kept.reset();
    for (uint32_t i = 0; i < count; ++i) {
        const PrUiViewportInfo& info = w->viewports.data[i];
        if (info.extent.x <= 0 || info.extent.y <= 0)
            continue;
        StudioWindowPortal* s = portalOf(w, info.viewport);
        if (s == nullptr) {
            PrAppWindow* parent = windowOfViewport(w, info.parent);
            s = parent != nullptr ? openPortal(w, info, parent) : nullptr;
            if (s != nullptr)
                prAppWindowRequestFrame(s->appWindow);
            continue;
        }
        PrUVec2 extent = portalExtentOf(info.extent);
        PrUVec2 was = portalExtentOf(s->info.extent);
        if (extent.x != was.x || extent.y != was.y)
            prAppWindowSetExtent(s->appWindow, extent);
        if (info.kind == PrUiPortalKind_Popup && !samePlacement(info.placement, s->info.placement))
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
    w->portals.reset();
    w->viewports.reset();
    prKitDestroy(w->kit);
    prUiDestroy(w->ui);
    prLayoutTreeDestroy(w->tree);
    prMemoryFree(w);
}

// Closes the window after the application's `close`, and frees it. The portals' windows close first.
void destroyWindow(PrStudioWindow* w) {
    if (w->desc.close != nullptr)
        w->desc.close(w->desc.context, w);
    while (w->portals.count != 0)
        destroyPortal(w, w->portals.count - 1);
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
        .portalExtent = workExtentOf(w),
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
    // pass clears to the canvas's color, so the list leaves the canvas unfilled. Taking the scene paints the roots of
    // the portals' viewports too, which their windows draw.
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
    showPortals(w);
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
    w->variant = PrKitThemeVariant_Dark;
    w->output.cursor = PrAppCursor_Arrow;
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
        .transparent = desc->transparent,
        .hidden = desc->hidden,
        .pointerPassthrough = desc->pointerPassthrough,
        .placement = desc->placement,
        .position = desc->position,
        .popup = desc->popup,
        .handler = {.context = w, .open = open, .frame = frame, .event = event},
    };
    // A window over a backdrop presents alpha, which costs the compositor a blend, so only a transparent window does.
    w->transparent = desc->transparent || desc->backdrop != PrAppWindowBackdrop_Opaque;
    PrStatus status = prAppWindowCreate(app->app, &windowDesc, &w->appWindow);
    if (status != PrStatus_Ok) {
        freeWindow(w);
        return status;
    }
    prUiSetWindow(w->ui, w->appWindow);
    // An application of several windows shows each portal of the window's context in a window of its own.
    if (prAppWindowModel(app->app) == PrAppWindowModel_Multiple) {
        PrUiHost host{.window = portalWindow, .context = w};
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
