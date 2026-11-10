// The browser's window: the chrome and the active tab's page presented together, and the window's input routed to them.
//
// A frame builds the chrome, posts the frame to the active tab's view at the extent of the page's box, and presents the
// tab's compositor. The compositor's present renders the page's list into the page's box and the chrome's list over the
// whole window, in one pass.

#include "apps/browser/browser.h"

#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/kit_theme.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/layout_unit.h"
#include "pr/min_max.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/time.h"

#include "apps/browser/chrome.h"
#include "apps/browser/commands.h"
#include "apps/browser/tab.h"

namespace browser {

using namespace pr;

namespace {

// The color that the page's box shows before its page has a frame to present.
constexpr PrColor kBlankCanvas{1, 1, 1, 1};

void build(void* context, PrUi*, const PrUiRound*) {
    chromeBuild(*static_cast<Browser*>(context));
}

// Plays the chrome's scene into its list in target pixels at `ratio`, each scroll container at the tree's offset for
// it. The chrome leaves the page's box empty.
void playChrome(Browser& b, float ratio) {
    const PrLayoutScene* scene = prLayoutTreeScene(b.tree);
    prLayoutSceneRelease(b.scene);
    b.scene = scene;
    b.scrolls.resize(prLayoutSceneScrolls(b.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(b.tree, scene, b.scrolls.data(), b.scrolls.count());
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = b.scrolls.data();
    options.scrollCount = b.scrolls.count();
    options.canvasFilled = true;
    prDrawListClear(b.chromeList);
    prDrawListPushTransformAffine2(b.chromeList, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, b.chromeList, &options);
    prDrawListPopTransform(b.chromeList);
}

// ---------------------------------------------------------------------------------------------------------------------
// Presenting

// Prints the frames' statistics if a second has passed since they were last printed, and starts counting again.
void printStats(Browser& b, double now) {
    if (b.statsSince == 0)
        b.statsSince = now;
    double elapsed = now - b.statsSince;
    if (elapsed < 1000)
        return;
    uint32_t frames = b.statsFrames;
    double gpu = double(b.statsGpu.exchange(0)) / 1000;
    const PrDrawStats& last = b.statsLast;
    printError("browser: {:.1f} fps, per frame {:.2f} ms on the window's thread ({:.2f} ms waiting for a drawable), "
        "{:.2f} ms on the GPU; last frame {} passes, {} layers, {} draws, {} primitives, {} path fills, {} KB "
        "uploaded\n", frames * 1000 / elapsed, b.statsCpu / frames, b.statsWait / frames, gpu / frames, last.passes,
        last.layers, last.draws, last.primitives, last.pathFills, last.uploadBytes >> 10);
    b.statsSince = now;
    b.statsFrames = 0;
    b.statsCpu = 0;
    b.statsWait = 0;
}

// Renders a frame and presents it: the page's list, if there is one, in the page's rectangle, and the chrome's list
// over the whole window. The pass clears to the page's canvas color, which fills the canvas in place of the page's
// list. The chrome's surfaces cover the rest of the window. A swap chain that holds more than 8-bit sRGB takes the
// frame through the display shader.
void render(Browser& b, PrDrawList* PR_NULLABLE page, PrColor canvas) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    double began = prTimeMonotonicMilliseconds();
    if (b.extent.x == 0 || b.extent.y == 0 || prGpuSwapChainAcquire(b.swapChain, b.extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(b.device, nullptr, &commands))
        return;
    double acquired = prTimeMonotonicMilliseconds();
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(b.swapChain);
    bool mapped = info.format != PrGpuFormat_Bgra8 || info.space != PrColorSpace_Srgb;
    if (mapped) {
        PrUVec2 held = b.rendered.id != 0 ? prGpuTextureExtent(b.device, b.rendered) : PrUVec2{};
        if (b.rendered.id == 0 || b.extent.x > held.x || b.extent.y > held.y) {
            prGpuTextureDestroy(b.device, b.rendered);
            PrGpuTextureDesc desc = prGpuTextureDesc2d(
                PrGpuFormat_Rgba16F, b.extent, PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
            b.rendered = PrGpuTexture{};
            prGpuTextureCreate(b.device, &desc, &b.rendered);
        }
    }
    PrGpuTexture target = mapped ? b.rendered : drawable;
    PrPixelFormat format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8);
    if (page != nullptr) {
        PrDrawTarget into{.extent = b.pageExtent, .format = format, .commands = commands};
        prDrawFramePrepare(b.pageFrame, page, &into);
    }
    PrDrawTarget window{.extent = b.extent, .format = format, .commands = commands};
    prDrawFramePrepare(b.chromeFrame, b.chromeList, &window);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_ShaderRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(
        target, PrGpuLoad_Clear, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    if (page != nullptr)
        prDrawFrameRender(b.pageFrame, encoder, b.pageOrigin);
    prDrawFrameRender(b.chromeFrame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = b.shownOn.headroom, .gamut = b.shownOn.gamut};
        prDrawDisplayDraw(b.display, encoder, target, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(b.swapChain, commands);
    if (b.stats) {
        prGpuCommandsOnDone(commands, [](void* PR_NULLABLE context, double seconds) {
            static_cast<Browser*>(context)->statsGpu.fetchAdd(uint64_t(seconds * 1e6));
        }, &b);
    }
    prGpuCommandsSubmit(commands);
    if (b.stats) {
        double now = prTimeMonotonicMilliseconds();
        ++b.statsFrames;
        b.statsCpu += now - began;
        b.statsWait += acquired - began;
        b.statsLast = prDrawFrameStats(page != nullptr ? b.pageFrame : b.chromeFrame);
        printStats(b, now);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

// Returns true if the pointer at `position` is over the page: in the page's box, with nothing of the chrome over it,
// such as a menu.
bool overPage(Browser& b, PrVec2 position) {
    const PrRect& page = b.chrome.page;
    if (b.chrome.slot == kPrLayoutNodeNone || position.x < page.left || position.x >= page.right ||
            position.y < page.top || position.y >= page.bottom)
        return false;
    PrLayoutNode top = kPrLayoutNodeNone;
    prLayoutTreeHitTest(b.tree, prLayoutVecFromPx(position), &top, 1);
    return top == b.chrome.slot;
}

// Returns the event as the tab's page sees it: its position in the page's viewport, and a scroll's delta in the page's
// CSS pixels, at the tab's zoom.
PrAppEvent toPage(const Browser& b, const Tab& tab, const PrAppEvent& e) {
    return prAppEventPlaced(&e, PrVec2{b.chrome.page.left, b.chrome.page.top}, tab.zoom);
}

// Returns true while the keyboard goes to the page: the active tab shows a page, and nothing of the chrome has focus.
bool pageHasKeys(Browser& b) {
    return b.chrome.slot != kPrLayoutNodeNone && prUiFocus(b.ui) == kPrLayoutNodeNone;
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

// Makes the device that the window renders with, the swap chain over the window and the renderer. Returns false if
// there is no device to render with.
bool openDevice(Browser& b) {
    PrAppWindow* window = b.window;
    PrAppSurface surface{};
    PrGpuSwapChainDesc swapChain{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &b.device) != PrStatus_Ok ||
            prAppWindowSurface(window, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(b.device, &surface, &swapChain, &b.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, b.device, nullptr, &b.renderer) != PrStatus_Ok ||
            prDrawDisplayCreate(b.device, &b.display) != PrStatus_Ok) {
        printError("browser: no GPU device to render with\n");
        return false;
    }
    b.pageFrame = prDrawFrameCreate(b.renderer);
    b.chromeFrame = prDrawFrameCreate(b.renderer);
    return true;
}

// Opens the tabs at the window's first frame, and starts the thread that runs them.
bool open(void* context, PrAppWindow* window, const PrAppFrame* first) {
    Browser& b = *static_cast<Browser*>(context);
    b.pageWindow = *first;
    for (const String& address : b.addresses)
        tabOpen(b, address, b.tabs.count());
    if (b.tabs.isEmpty()) {
        tabOpen(b, PrStringView{}, 0);
        b.chrome.focusAddress = true;
    }
    tabActivate(b, *tabAt(b, 0));
    prThreadStart(&b.thread, [](void* PR_NULLABLE context) { tabsRun(*static_cast<Browser*>(context)); }, &b);
    b.started = true;
    prAppWindowRequestFrame(window);
    return true;
}

// Each frame builds the chrome, posts the frame to the active tab's view at the extent of the page's box, and presents
// the tab's compositor, which renders the page with the chrome over it. A tab with no frame to present yet renders the
// chrome over a blank page.
PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Browser& b = *static_cast<Browser*>(context);
    prGpuSwapChainWait(b.swapChain);
    tabsRefresh(b);
    prCachePump(b.icons);
    if (f->preferences.colorScheme != b.scheme) {
        b.scheme = f->preferences.colorScheme;
        PrKitThemeVariant variant =
            b.scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
        PrKitTheme theme = prKitThemeStudio(variant, b.fonts);
        prKitSetTheme(b.kit, &theme);
    }
    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    prUiFrame(b.ui, &desc, build, &b);
    float ratio = f->display.pixelRatio;
    playChrome(b, ratio);
    b.extent = PrUVec2{uint32_t(prF32Round(f->extent.x * ratio)), uint32_t(prF32Round(f->extent.y * ratio))};
    b.shownOn = f->display;

    // The page's box, as this frame's layout placed it, in px and in target pixels within the window.
    PrRect page{};
    if (b.chrome.slot != kPrLayoutNodeNone)
        page = prLayoutRectToPx(prLayoutNodeBounds(b.tree, b.chrome.slot, PrLayoutCoordinates_Viewport));
    b.chrome.page = page;
    int32_t left = int32_t(prF32Round(page.left * ratio));
    int32_t top = int32_t(prF32Round(page.top * ratio));
    int32_t right = min(int32_t(prF32Round(page.right * ratio)), int32_t(b.extent.x));
    int32_t bottom = min(int32_t(prF32Round(page.bottom * ratio)), int32_t(b.extent.y));
    b.pageOrigin = PrIVec2{left, top};
    b.pageExtent = PrUVec2{uint32_t(max(right - left, 0)), uint32_t(max(bottom - top, 0))};

    Tab* tab = browserActiveTab(b);
    PrHtmlView* view = tab != nullptr ? tab->view.load() : nullptr;
    b.presented = false;
    if (view != nullptr && b.pageExtent.x != 0 && b.pageExtent.y != 0) {
        // The page sees the window at its zoom: a smaller viewport of larger CSS pixels.
        PrAppFrame shown = *f;
        shown.extent = PrVec2{(page.right - page.left) / tab->zoom, (page.bottom - page.top) / tab->zoom};
        shown.display.pixelRatio = ratio * tab->zoom;
        if (b.trace && (shown.extent.x != b.pageWindow.extent.x || shown.extent.y != b.pageWindow.extent.y))
            printError("[{:.3f}] window asks for viewport {:.0f}x{:.0f}\n", prTimeMonotonicMilliseconds() / 1000,
                shown.extent.x, shown.extent.y);
        b.pageWindow = shown;
        prHtmlViewPostFrame(view, &shown);
        prLayoutCompositorFrame(tab->compositor);
    }
    if (!b.presented)
        render(b, nullptr, kBlankCanvas);
    // The pointer shows the document's cursor wherever the page takes the pointer, and the chrome's elsewhere. The page
    // sees the window at its zoom.
    PrAppOutput output = prUiOutput(b.ui);
    if (view != nullptr) {
        bool pagePointer = b.pageButtons != 0 || overPage(b, b.pointer);
        PrVec2 origin{b.chrome.page.left, b.chrome.page.top};
        output = prAppOutputMerge(output, prAppOutputPlaced(tab->info.output, origin, tab->zoom), pagePointer);
    }
    if (tab != nullptr && prLayoutCompositorWantsFrame(tab->compositor))
        output.wantsFrame = true;
    return output;
}

// The window's input goes to the chrome, and to the active tab's page where the page has it. The pointer goes to the
// page over the page's box, and stays with the page while a button pressed there is held. The keyboard goes to the page
// while nothing of the chrome has focus. The chrome sees every move of the pointer, so that its hover follows the
// pointer out of it, and every press, so that a press in the page closes its menus. A key bound to one of the browser's
// commands runs it wherever focus is: through the chrome's scope while the chrome has the keyboard, and ahead of the
// page while the page has it.
void event(void* context, PrAppWindow* window, const PrAppEvent* e) {
    Browser& b = *static_cast<Browser*>(context);
    Tab* tab = browserActiveTab(b);
    PrHtmlView* view = tab != nullptr ? tab->view.load() : nullptr;
    bool toPageToo = false;
    switch (e->kind) {
        case PrAppEventKind_CloseRequested:
            prAppStop(b.app);
            return;
        case PrAppEventKind_KeyDown:
            if (view != nullptr && pageHasKeys(b) && commandsRunChord(b, *e)) {
                prAppWindowRequestFrame(window);
                return;
            }
            if (view != nullptr)
                prHtmlViewPostEvent(view, e);
            break;
        case PrAppEventKind_KeyUp:
        case PrAppEventKind_Text:
        case PrAppEventKind_Paste:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            if (view != nullptr && pageHasKeys(b)) {
                prHtmlViewPostEvent(view, e);
                return;
            }
            break;
        case PrAppEventKind_PointerMove:
            b.pointer = e->pointer.position;
            toPageToo = b.pageButtons != 0 || overPage(b, e->pointer.position);
            break;
        case PrAppEventKind_PointerDown:
            b.pointer = e->pointer.position;
            if (overPage(b, e->pointer.position)) {
                b.pageButtons |= 1u << e->pointer.button;
                b.chrome.releaseFocus = true;
                toPageToo = true;
            }
            break;
        case PrAppEventKind_PointerUp:
            b.pointer = e->pointer.position;
            toPageToo = (b.pageButtons & (1u << e->pointer.button)) != 0;
            b.pageButtons &= ~(1u << e->pointer.button);
            break;
        case PrAppEventKind_Scroll:
            if (view != nullptr && overPage(b, e->scroll.position)) {
                PrAppEvent moved = toPage(b, *tab, *e);
                prHtmlViewPostEvent(view, &moved);
                if (prLayoutCompositorWantsFrame(tab->compositor))
                    prAppWindowRequestFrame(window);
                return;
            }
            break;
        default:
            break;
    }
    if (toPageToo && view != nullptr) {
        PrAppEvent moved = toPage(b, *tab, *e);
        prHtmlViewPostEvent(view, &moved);
    }
    prUiPushEvent(b.ui, kPrUiViewportMain, e);
    prAppWindowRequestFrame(window);
}

// Opens the browser's window and its device. The window's close stops the application.
bool start(void* context, PrApp* app) {
    Browser& b = *static_cast<Browser*>(context);
    b.app = app;
    PrAppWindowDesc desc{
        .title = "Prax",
        .extent = {1280, 860},
        .handler = {.context = &b, .open = open, .frame = frame, .event = event},
    };
    if (prAppWindowCreate(app, &desc, &b.window) != PrStatus_Ok)
        return false;
    prUiSetWindow(b.ui, b.window);
    return openDevice(b);
}

// A wake from a tab's thread asks for a frame, if one asked for it since the last.
void appEvent(void* context, PrApp*, const PrAppEvent* e) {
    Browser& b = *static_cast<Browser*>(context);
    if (e->kind == PrAppEventKind_Wake && b.window != nullptr && b.frameWanted.exchange(false))
        prAppWindowRequestFrame(b.window);
}

void shutdown(void* context, PrApp*) {
    Browser& b = *static_cast<Browser*>(context);
    tabsShutdown(b);
    if (b.device != nullptr)
        prGpuTextureDestroy(b.device, b.rendered);
    prDrawDisplayDestroy(b.display);
    prDrawFrameDestroy(b.pageFrame);
    prDrawFrameDestroy(b.chromeFrame);
    prDrawRendererDestroy(b.renderer);
    prGpuSwapChainDestroy(b.swapChain);
    prGpuDeviceDestroy(b.device);
}

} // namespace

void browserWantFrame(Browser& b) {
    if (b.app != nullptr && !b.frameWanted.exchange(true))
        prAppWake(b.app);
}

Tab* PR_NULLABLE browserActiveTab(const Browser& b) {
    return tabFromId(prKitTabsSelected(b.tabStrip));
}

void browserPresent(Browser& b, PrDrawList* list, PrColor16 canvas) {
    b.presented = true;
    render(b, list, prColorFromColor16(canvas));
}

PrAppHandler browserHandler(Browser& b) {
    return PrAppHandler{.context = &b, .start = start, .event = appEvent, .shutdown = shutdown};
}

} // namespace browser
