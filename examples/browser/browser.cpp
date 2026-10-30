// The browser example: a web browser in a window, whose pages' scripts run, rendered through lib/layout and lib/draw.
//
// Usage: browser [--cache <directory>] <url | path>
//
// The browser opens a URL, or a file at its file: URL, and navigates from there: a link that the user follows, or a
// script that sets the location, opens the document that it names, and Alt or Cmd with Left goes back in the window's
// session history, and with Right forward. The network is lib/net's sockets, with the platform's TLS, under lib/http
// and lib/fetch, and an HTTP cache on disk, where the platform keeps a user's caches or in the directory that --cache
// names, as pr_shell's --cache does. Local storage is on disk too, where the platform keeps a user's data.
//
// main() is the composition root: the window is one page (pr/html_page.h), the navigable that loads each document, with
// its script engine (pr/html_quickjs.h) and its view (pr/html_view.h). The browser runs on three threads:
//
//   - The window's thread takes the window's events and frames, and posts them to the view. A wheel scrolls the
//     compositor at once. Each frame presents the compositor's last scene.
//   - The page's thread runs the page's agent's event loop (prDomAgentRun), which the view's update ends.
//   - The agent's layout thread lays out and records the page in jobs of about a frame's work. As each job finishes,
//     the view commits its scene to the compositor, which asks the window for a frame.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/charset_quickjs.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/draw_display.h"
#include "pr/draw_renderer.h"
#include "pr/fetch.h"
#include "pr/file.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/html_storage.h"
#include "pr/html_view.h"
#include "pr/http.h"
#include "pr/image.h"
#include "pr/layout_compositor.h"
#include "pr/mathml_quickjs.h"
#include "pr/net.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/svg_quickjs.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/tls.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace pr;

namespace {

// How long the page's thread runs the page's due tasks at a time, in milliseconds, so that a long load renders as it
// goes.
constexpr double kTaskBudget = 8;

// The approximate duration of each job of the layout thread, in milliseconds.
constexpr double kJobBudget = 10;

// The most that the HTTP cache holds.
constexpr uint64_t kCacheBytes = uint64_t(256) << 20;

struct Browser {
    std::string address;            // the URL that the browser opens first
    std::string cacheDirectory;     // where the HTTP cache is kept, or empty for the platform's cache directory

    // What every page shares: text, images, storage, the network, the window's session history, and the agent whose
    // event loop runs the pages on the page's thread.
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = nullptr;
    PrCompressCodec* compress = nullptr;
    PrImageCodec* images = nullptr;
    PrTaskPool* io = nullptr;       // writes the files of the cache and of local storage
    PrTaskPool* workers = nullptr;  // the threads that share the agent's work with the page's thread, such as restyles
    PrStorage* cache = nullptr;
    PrHtmlStorageProvider* localStorage = nullptr;
    PrHtmlStorageProvider* sessionStorage = nullptr;
    PrNet* net = nullptr;
    PrTls* tls = nullptr;
    PrHttp* http = nullptr;
    PrFetchNetwork* fetcher = nullptr;
    PrHtmlSessionHistory* sessionHistory = nullptr;
    PrDomAgent* agent = nullptr;

    // The window's page, the engine of its scripts, and the view that presents it.
    PrJsHtmlEngine* engine = nullptr;
    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;

    // What the window's thread presents with: the compositor, which the page's thread commits to, and the device.
    PrApp* app = nullptr;
    PrLayoutCompositor* compositor = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrGpuTexture rendered{};        // the half floats that a frame renders into where the swap chain holds more
    PrDrawDisplay* display = nullptr;
    PrUVec2 extent{};               // the window's frame's extent in target pixels
    PrAppDisplay shownOn = prAppDisplayDefault();
    std::atomic<bool> frameWanted{false};

    // The page's thread.
    PrThread thread{};
    bool started = false;
};

// The clock of the compositor, which is the pages' clock: the system's monotonic clock.
double clock(void* PR_NULLABLE) {
    return prTimeMonotonicMilliseconds();
}

// Asks the window's thread for a frame, from any thread. Only the window's thread may request one, so this wakes it.
void wantFrame(Browser& b) {
    if (b.app != nullptr && !b.frameWanted.exchange(true))
        prAppWake(b.app);
}

void printConsole(JSContext*, PrJsHtmlConsoleLevel, uint32_t depth, PrStringView text) {
    for (uint32_t i = 0; i < depth; ++i)
        fputs("  ", stderr);
    fwrite(text.data, 1, text.size, stderr);
    fputc('\n', stderr);
}

constexpr PrJsHtmlConsolePrinter kConsolePrinter = {printConsole};

PrHtmlHost pageHost(Browser& b);

// Gives the page of an iframe the host that the window's page has.
void frameHost(void* context, PrHtmlPage*, PrHtmlHost* child) {
    *child = pageHost(*static_cast<Browser*>(context));
}

// Returns the host of the window's page, before its script engine takes it: what every page shares.
PrHtmlHost pageHost(Browser& b) {
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &b;
    host.fetcher = b.fetcher;
    host.frame = frameHost;
    host.agent = b.agent;
    host.font = b.font;
    host.shaper = b.shaper;
    host.images = b.images;
    host.device = b.device;
    host.compress = b.compress;
    host.localStorage = b.localStorage;
    host.sessionStorage = b.sessionStorage;
    host.sessionHistory = b.sessionHistory;
    return host;
}

// Opens the window's page in `window`, as the window first is, and navigates it to the browser's address.
void openPage(Browser& b, const PrAppFrame& window) {
    PrJsHtmlBind* const binds[] = {prJsSvgBind, prJsMathMlBind, prJsUrlBind, prJsCharsetBind, prJsXhrBind};
    PrJsHtmlEngineDesc engine{.binds = binds, .bindCount = uint32_t(countOf(binds)), .console = &kConsolePrinter};
    b.engine = prJsHtmlEngineCreate(&engine);
    PrHtmlHost host = pageHost(b);
    PrHtmlHost scripted = prJsHtmlEngineHost(b.engine, &host, PrJsHtmlScripting_Enabled);
    b.page = prHtmlPageCreate(&scripted);
    prHtmlPageSetViewport(b.page, window.extent);
    prHtmlPageSetDisplay(b.page, &window.display);
    prHtmlPageSetUserPreferences(b.page, &window.preferences);
    PrHtmlViewDesc view{.page = b.page, .compositor = b.compositor, .jobBudget = kJobBudget};
    b.view = prHtmlViewCreate(&view);
    if (prHtmlPageNavigate(b.page, b.address) != PrStatus_Ok)
        fprintf(stderr, "browser: %s is not a URL\n", b.address.c_str());
}

// The page's thread: the agent's event loop, until the window closes.
void pageThread(Browser& b) {
    while (prDomAgentWait(b.agent, -1))
        prDomAgentRun(b.agent, kTaskBudget);
}

// ---------------------------------------------------------------------------------------------------------------------
// What every page shares

// Opens text, images, storage, the network and the agent.
void openShared(Browser& b) {
    b.fonts = prFontSetCreate(nullptr);
    prFontBundleInstall(b.fonts, PrFontBundle_Noto, PR_FONT_BUNDLE_DIR);
    prFontSystemInstall(PrFontSystemBackend_Default, b.fonts);
    b.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrTextFontDesc font{.set = b.fonts, .style = prFontStyleDefault()};
    b.font = prTextFontCreate(&font);
    b.compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    b.images = prImageCodecCreate(PrImageCodecBackend_Default, b.compress);
    b.io = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);

    // The cache and local storage are on disk where the platform keeps a user's, so that they outlive the run.
    std::string directory = b.cacheDirectory;
    PrString cacheRoot = prFileCacheDirectory();
    if (directory.empty() && !prStringIsEmpty(&cacheRoot))
        directory = std::string(prStringView(&cacheRoot).data, prStringView(&cacheRoot).size) + "/Prax/Browser/Cache";
    prStringReset(&cacheRoot);
    if (!directory.empty()) {
        PrStorageDiskDesc disk{.directory = directory, .limitBytes = kCacheBytes, .pool = b.io};
        if (prStorageDiskCreate(&disk, &b.cache) != PrStatus_Ok)
            fprintf(stderr, "browser: cannot open the cache in %s\n", directory.c_str());
    }
    if (b.cache == nullptr)
        b.cache = prStorageMemoryCreate(kCacheBytes, PrStorageEviction_LeastRecentlyUsed);
    PrString dataRoot = prFileDataDirectory();
    PrHtmlStorageProviderDesc local{.quotaBytes = kPrHtmlStorageQuotaBytes, .pool = b.io};
    std::string localDirectory;
    if (!prStringIsEmpty(&dataRoot)) {
        localDirectory = std::string(prStringView(&dataRoot).data, prStringView(&dataRoot).size) + "/Prax/Browser/Local Storage";
        local.directory = localDirectory;
    }
    prStringReset(&dataRoot);
    b.localStorage = prHtmlStorageProviderCreate(&local);
    PrHtmlStorageProviderDesc session{.quotaBytes = kPrHtmlStorageQuotaBytes};
    b.sessionStorage = prHtmlStorageProviderCreate(&session);

    b.net = prNetCreate(PrNetBackend_Default);
    b.tls = prTlsCreate(PrTlsBackend_Default, b.net);
    PrHttpDesc http{.net = b.net, .tls = b.tls};
    b.http = prHttpCreate(PrHttpBackend_Default, &http);
    PrFetchNetworkDesc fetch{.net = b.net, .http = b.http, .compress = b.compress, .cache = b.cache};
    b.fetcher = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetch);

    b.sessionHistory = prHtmlSessionHistoryCreate();
    uint32_t cores = prThreadHardwareCount();
    b.workers = cores > 1 ? prTaskPoolCreate(PrTaskPoolBackend_Default, cores - 1) : nullptr;
    PrDomAgentOptions agentOptions = prDomAgentOptionsDefault();
    agentOptions.restylePool = b.workers;
    b.agent = prDomAgentCreate(&agentOptions);
}

// Frees what every page shares, once the pages are gone. The fetcher and HTTP run on the network's loop, and are freed
// there, after the cancels of the pages' fetches.
void closeShared(Browser& b) {
    prDomAgentDestroy(b.agent);
    prNetPostBlocking(b.net, [](void* PR_NULLABLE context) {
        Browser& browser = *static_cast<Browser*>(context);
        prFetchNetworkDestroy(browser.fetcher);
        prHttpDestroy(browser.http);
    }, &b);
    prTlsDestroy(b.tls);
    prNetDestroy(b.net);
    prStorageDestroy(b.cache);
    prHtmlSessionHistoryDestroy(b.sessionHistory);
    prHtmlStorageProviderDestroy(b.localStorage);
    prHtmlStorageProviderDestroy(b.sessionStorage);
    prTaskPoolDestroy(b.workers);
    prTaskPoolDestroy(b.io);
    prImageCodecDestroy(b.images);
    prCompressCodecDestroy(b.compress);
    prTextFontRelease(b.font);
    prTextShaperDestroy(b.shaper);
    prFontSetDestroy(b.fonts);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window's thread

// Presents a frame that the compositor made, on the window's thread. The pass clears to the frame's canvas color, which
// fills the canvas in place of the list. A swap chain that holds more than 8-bit sRGB takes the frame through the
// display shader.
void present(void* PR_NULLABLE context, PrDrawList* list, PrColor16 canvasColor) {
    Browser& b = *static_cast<Browser*>(context);
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (b.extent.x == 0 || b.extent.y == 0 || prGpuSwapChainAcquire(b.swapChain, b.extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(b.device, nullptr, &commands))
        return;
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
    PrDrawTarget into{
        .extent = b.extent,
        .format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8),
        .commands = commands,
    };
    prDrawFramePrepare(b.frame, list, &into);
    PrColor canvas = prColorFromColor16(canvasColor);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_ShaderRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(
        target, PrGpuLoad_Clear, PrColor{canvas.r * canvas.a, canvas.g * canvas.a, canvas.b * canvas.a, canvas.a});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(b.frame, encoder, PrIVec2{0, 0});
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
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame* window) {
    Browser& b = *static_cast<Browser*>(context);
    b.app = app;
    PrAppSurface surface{};
    PrGpuSwapChainDesc swapChain{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &b.device) != PrStatus_Ok ||
            prAppSurface(app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(b.device, &surface, &swapChain, &b.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, b.device, nullptr, &b.renderer) != PrStatus_Ok ||
            prDrawDisplayCreate(b.device, &b.display) != PrStatus_Ok) {
        fprintf(stderr, "browser: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    b.frame = prDrawFrameCreate(b.renderer);
    // The compositor has no thread of its own: the window's frames make its frames, when it wants them.
    PrLayoutCompositorDesc compositor{
        .context = &b,
        .now = clock,
        .present = present,
        .wantFrame = [](void* PR_NULLABLE context) { wantFrame(*static_cast<Browser*>(context)); },
        .canvasFilled = true,
    };
    b.compositor = prLayoutCompositorCreate(&compositor);
    openPage(b, *window);
    prThreadStart(&b.thread, [](void* PR_NULLABLE context) { pageThread(*static_cast<Browser*>(context)); }, &b);
    b.started = true;
    prAppRequestFrame(app);
}

// Each of the window's frames goes to the view, and the compositor presents its last scene at the window's size.
void frame(void* context, PrApp* app, const PrAppFrame* f) {
    Browser& b = *static_cast<Browser*>(context);
    prHtmlViewPostFrame(b.view, f);
    prGpuSwapChainWait(b.swapChain);
    b.extent = PrUVec2{
        uint32_t(std::lround(f->extent.x * f->display.pixelRatio)),
        uint32_t(std::lround(f->extent.y * f->display.pixelRatio)),
    };
    b.shownOn = f->display;
    prLayoutCompositorFrame(b.compositor);
    if (prLayoutCompositorWantsFrame(b.compositor))
        prAppRequestFrame(app);
}

// The window's input goes to the view. Alt or Cmd with Left goes back in the session history, and with Right forward,
// as a platform's browsers do, and Escape closes the window.
void event(void* context, PrApp* app, const PrAppEvent* e) {
    Browser& b = *static_cast<Browser*>(context);
    switch (e->kind) {
        case PrAppEventKind_Wake:
            if (b.frameWanted.exchange(false))
                prAppRequestFrame(app);
            return;
        case PrAppEventKind_Clipboard:
            return;
        case PrAppEventKind_KeyDown:
            if (e->key == PrAppKey_Escape) {
                prAppStop(app);
                return;
            }
            if ((e->key == PrAppKey_Left || e->key == PrAppKey_Right) &&
                    (e->modifiers & (PrAppModifierFlag_Alt | PrAppModifierFlag_Super)) != 0) {
                prHtmlViewPostTraverse(b.view, e->key == PrAppKey_Left ? -1 : 1);
                return;
            }
            break;
        default:
            break;
    }
    prHtmlViewPostEvent(b.view, e);
    if (prLayoutCompositorWantsFrame(b.compositor))
        prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    Browser& b = *static_cast<Browser*>(context);
    if (b.started) {
        prDomAgentStop(b.agent);
        prThreadJoin(&b.thread);
    }
    // The engine goes before the page, since its objects' finalizers reach the page's window.
    prHtmlViewDestroy(b.view);
    prJsHtmlEngineDestroy(b.engine);
    prHtmlPageDestroy(b.page);
    prLayoutCompositorDestroy(b.compositor);
    if (b.device != nullptr) {
        prGpuDeviceWaitIdle(b.device);
        prGpuTextureDestroy(b.device, b.rendered);
    }
    prDrawDisplayDestroy(b.display);
    prDrawFrameDestroy(b.frame);
    prDrawRendererDestroy(b.renderer);
    prGpuSwapChainDestroy(b.swapChain);
    prGpuDeviceDestroy(b.device);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Browser b;
    int at = 1;
    if (at + 1 < argc && std::string(argv[at]) == "--cache") {
        b.cacheDirectory = argv[at + 1];
        at += 2;
    }
    if (at + 1 != argc) {
        fprintf(stderr, "usage: browser [--cache <directory>] <url | path>\n");
        return 2;
    }
    // A path to a file opens the file at its file: URL. Anything else is a URL.
    b.address = argv[at];
    std::error_code error;
    if (std::filesystem::is_regular_file(b.address, error))
        b.address = "file://" + std::filesystem::absolute(b.address).lexically_normal().generic_string();
    openShared(b);
    std::string title = "Prax: " + b.address;
    PrAppDesc desc{.title = title, .extent = {1024, 768}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&b, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);
    closeShared(b);
    return 0;
}
