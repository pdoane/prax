// Alchemy: a studio for generating images with a ComfyUI server, in a window.
//
// main() is the composition root. Alchemy chooses each piece that runs it: the platform's window, the Noto fonts shaped
// by the platform's default shaper, lib/ui's context with Studio Kit on top, lib/draw's fragment renderer on the
// platform's GPU, the platform's network with HTTP and WebSockets over it, a task pool for decoding images, and the
// image and compression codecs. Each frame takes the server's events and the decoded images into the state, runs the
// context's rounds, and renders the tree's scene into the window's swap chain.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/http.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/net.h"
#include "pr/text.h"
#include "pr/tls.h"
#include "pr/ui.h"
#include "pr/websocket.h"

#include "examples/alchemy/commands.h"
#include "examples/alchemy/text.h"
#include "examples/alchemy/view.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

namespace {

using namespace alchemy;

// Alchemy in a window: its state and interface, the services that the state uses, and the device that it renders on.
struct Window {
    AppState state;
    Shell shell{&state};
    Commands* commands = nullptr;
    View view;

    PrNet* net = nullptr;
    PrTls* PR_NULLABLE tls = nullptr;
    PrHttp* http = nullptr;
    PrWebSocketNetwork* websockets = nullptr;

    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PrAppColorScheme scheme = PrAppColorScheme_Dark;
    const PrLayoutScene* scene = nullptr;
    PrDrawList* list = nullptr;
    std::vector<PrLayoutSceneScroll> scrolls;

    // The application, which the network's loop and the task pool wake when they deliver results.
    std::atomic<PrApp*> app{nullptr};
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    double lastTime = 0;

    // The file that $ALCHEMY_CAPTURE names, which a frame drawn $ALCHEMY_CAPTURE_SECONDS after the start is written to
    // as a PNG, rendered on the CPU, before the application quits. It lets a script see the interface without a screen.
    const char* PR_NULLABLE capturePath = nullptr;
    double captureSeconds = 5;
    double startTime = -1;
};

// Renders the list on the CPU into a PNG file at `path`, at `extent` target pixels on the `canvas` color.
bool capture(Window& w, const char* path, PrUVec2 extent, PrColor canvas) {
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrDrawFrame* cpuFrame = prDrawFrameCreate(cpu);
    std::vector<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = uint8_t(canvas.r * 255.0f + 0.5f);
        pixels[i + 1] = uint8_t(canvas.g * 255.0f + 0.5f);
        pixels[i + 2] = uint8_t(canvas.b * 255.0f + 0.5f);
        pixels[i + 3] = 255;
    }
    PrDrawTarget target{.extent = extent, .format = PrPixelFormat_Rgba8};
    bool written = false;
    if (prDrawFramePrepare(cpuFrame, w.list, &target) == PrStatus_Ok) {
        prDrawFrameRenderPixels(cpuFrame, PrPixels{pixels.data(), extent.x * 4, extent}, PrPixelFormat_Rgba8);
        PrPixelView view{pixels.data(), extent.x * 4, extent};
        size_t size = 0;
        prImageCodecEncode(w.state.codec, "image/png", view, nullptr, 0, &size);
        std::vector<uint8_t> png(size);
        if (prImageCodecEncode(w.state.codec, "image/png", view, png.data(), png.size(), &size) == PrStatus_Ok)
            written = prFileWrite(path, PrByteView{png.data(), size});
    }
    prDrawFrameDestroy(cpuFrame);
    prDrawRendererDestroy(cpu);
    return written;
}

void wake(Window& w) {
    if (PrApp* app = w.app.load())
        prAppWake(app);
}

PrKitThemeVariant variantOf(PrAppColorScheme scheme) {
    return scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
}

void build(void* context, PrUi*, const PrUiRound*) {
    Window& w = *static_cast<Window*>(context);
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .ground = PrKitGround_App,
    };
    PrLayoutNode root = prKitSurface(w.kit, kPrLayoutNodeNone, {}, style);
    buildView(w.kit, root, w.view, *w.commands);
}

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

bool openDevice(Window& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app.load(), &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

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
    w.app.store(app);
    w.shell.app = app;
    prUiSetApp(w.ui, app);
    if (!openDevice(w)) {
        fprintf(stderr, "alchemy: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    connectServer(w.state);
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp* app, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    prGpuSwapChainWait(w.swapChain);

    PrAppColorScheme scheme = f->preferences.colorScheme;
    if (scheme != w.scheme) {
        w.scheme = scheme;
        PrKitTheme theme = prKitThemeStudio(variantOf(scheme), w.fonts);
        prKitSetTheme(w.kit, &theme);
    }

    // The state takes what arrived since the last frame before the build reads it.
    double dt = w.lastTime != 0 ? f->time - w.lastTime : 0;
    w.lastTime = f->time;
    pumpEvents(w.state, dt);
    imageCachePump(w.state.images);
    applyDimensions(w.state);
    noteVisit(w.state);

    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    prUiFrame(w.ui, &desc, build, &w);

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
    if (w.startTime < 0)
        w.startTime = f->time;
    if (w.capturePath != nullptr && f->time - w.startTime >= w.captureSeconds && extent.x != 0) {
        if (!capture(w, w.capturePath, extent, canvas))
            fprintf(stderr, "alchemy: could not write %s\n", w.capturePath);
        prAppStop(app);
        return {};
    }
    PrAppOutput output = prUiOutput(w.ui);
    // A capture waits for its moment, which no input announces.
    if (w.capturePath != nullptr)
        output.wantsFrame = true;
    // A run in flight moves its progress and its preview, which the server's events deliver with a wake.
    return output;
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
        fprintf(stderr, "alchemy: the fonts under %s could not be installed\n", PR_FONT_BUNDLE_DIR);
        return 1;
    }
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = w.shaper};
    w.tree = prLayoutTreeCreate(&treeDesc);
    w.ui = prUiCreate(w.tree);
    PrKitTheme theme = prKitThemeStudio(variantOf(w.scheme), w.fonts);
    w.kit = prKitCreate(w.ui, &theme);
    w.list = prDrawListCreate();

    // The services of the state.
    w.net = prNetCreate(PrNetBackend_Default);
    w.tls = prTlsCreate(PrTlsBackend_Default, w.net);
    PrHttpDesc httpDesc{.net = w.net, .tls = w.tls};
    w.http = prHttpCreate(PrHttpBackend_Default, &httpDesc);
    PrWebSocketNetworkDesc socketDesc{.net = w.net, .tls = w.tls};
    w.websockets = prWebSocketNetworkCreate(PrWebSocketNetworkBackend_Default, &socketDesc);
    w.state.compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    w.state.codec = prImageCodecCreate(PrImageCodecBackend_Default, w.state.compress);
    w.state.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 4);
    w.state.images = imageCacheCreate(w.state.pool, w.state.codec, [] { wake(w); });
    w.state.client = comfyClientCreate({
        .net = w.net,
        .http = w.http,
        .websockets = w.websockets,
        .wake = [] { wake(w); },
    });
    if (const char* server = getenv("ALCHEMY_SERVER"))
        w.state.server = server;
    w.capturePath = getenv("ALCHEMY_CAPTURE");
    if (const char* seconds = getenv("ALCHEMY_CAPTURE_SECONDS"))
        w.captureSeconds = atof(seconds);
    galleryOpen(w.state.gallery, galleryDirectory());

    w.shell.ui = w.ui;
    w.commands = commandsCreate(&w.shell);
    viewInit(w.view);

    PrAppDesc desc{.title = "Alchemy", .extent = {1500, 940}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&w, start, frame, event, shutdown};
    prAppRun(app, &handler);
    w.app.store(nullptr);
    prAppDestroy(app);

    // The services go in the reverse of their making: the client lets go of the loop, the sources on the loop go on it,
    // and the pool finishes its decodes and surveys before the state that they write to.
    comfyClientDestroy(w.state.client);
    prNetPostBlocking(w.net, [](void* PR_NULLABLE context) {
        Window& window = *static_cast<Window*>(context);
        prWebSocketNetworkDestroy(window.websockets);
        prHttpDestroy(window.http);
    }, &w);
    prTlsDestroy(w.tls);
    prNetDestroy(w.net);
    imageCacheDestroy(w.state.images);
    stateReset(w.state);
    prTaskPoolDestroy(w.state.pool);
    prImageCodecDestroy(w.state.codec);
    prCompressCodecDestroy(w.state.compress);

    viewReset(w.view);
    commandsDestroy(w.commands);
    prDrawListRelease(w.list);
    prLayoutSceneRelease(w.scene);
    prKitDestroy(w.kit);
    prUiDestroy(w.ui);
    prLayoutTreeDestroy(w.tree);
    prTextShaperDestroy(w.shaper);
    prFontSetDestroy(w.fonts);
    return 0;
}
