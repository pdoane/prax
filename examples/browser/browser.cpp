// The browser example: a web browser in a window, whose pages' scripts run, rendered through lib/layout and lib/draw.
//
// Usage: browser <url | path>
//
// The browser opens a URL, or a file at its file: URL, and navigates from there: a link that the user follows, or a
// script that sets the location, opens the document that it names, and Alt or Cmd with Left goes back in the window's
// session history, and with Right forward. The network is lib/net's sockets, with the platform's TLS, under lib/http
// and lib/fetch, and an HTTP cache on disk. Local storage is on disk too, where the platform keeps a user's data.
//
// main() is the composition root, and the browser runs on three threads:
//
//   - The window's thread takes the window's events and frames. A wheel scrolls the compositor at once, and the rest of
//     the input is queued for the page. Each frame ticks the page's thread and presents the compositor's last scene.
//   - The page's thread owns the page shown (pr/html_page.h), its scripts and its view (pr/html_view.h). It sleeps
//     until a task of the page is due, input arrives, the window ticks it, or the layout thread finishes a job.
//   - The page's agent's layout thread lays out and records the page in jobs of about a frame's work. As each job
//     finishes, the view commits its scene to the compositor, which asks the window for a frame.
//
// Every page that the window shows has a script engine and a page of its own, as each document has a Window of its own.
// A navigation makes the next page and navigates it (prHtmlPageNavigate). The page shown stays until the next one's
// response arrives, and the next one then takes its place.

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
#include "pr/net.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/thread.h"
#include "pr/tls.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace pr;

namespace {

// How long the page's thread runs the page's due tasks at a time, in milliseconds, so that a long load renders as it
// goes.
constexpr double kTaskBudget = 8;

// The approximate duration of each job of the layout thread, in milliseconds.
constexpr double kJobBudget = 10;

// The most that the HTTP cache holds.
constexpr uint64_t kCacheBytes = uint64_t(256) << 20;

// A document of the window: its script engine and its page, and the view that presents it once it is shown.
struct Document {
    PrJsHtmlEngine* engine = nullptr;
    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;
};

// Input that the window's thread queues for the page's thread, with a copy of its text.
struct QueuedEvent {
    PrAppEvent event;
    std::string text;
    PrHtmlViewEventFlags flags;     // what the window's thread did with the event
};

struct Browser {
    std::string address;            // the URL that the browser opens first

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

    // The page's thread's: the document shown, the one that a navigation is loading, and a navigation that a task of
    // the page asked for, which starts once the task returns.
    Document shown;
    Document next;
    std::string pendingNavigation;

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

    // The page's thread, and what wakes it. `mutex` guards the members from `ending` to `queued`.
    PrThread thread{};
    bool started = false;
    std::mutex mutex;
    std::condition_variable wake;
    bool ending = false;
    bool ticked = false;            // true if the window ticked a frame
    bool woken = false;             // true if a task was posted or a layout job finished
    PrAppFrame tick{};
    std::vector<QueuedEvent> queued;
};

// The clock of the pages and of the compositor: a monotonic clock in milliseconds.
double clock(void* PR_NULLABLE) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Asks the window's thread for a frame, from any thread. Only the window's thread may request one, so this wakes it.
void wantFrame(Browser& b) {
    if (b.app != nullptr && !b.frameWanted.exchange(true))
        prAppWake(b.app);
}

// Wakes the page's thread when a task is posted to the agent or the layout thread finishes its jobs.
void wakePage(void* PR_NULLABLE context) {
    Browser& b = *static_cast<Browser*>(context);
    std::lock_guard<std::mutex> lock(b.mutex);
    b.woken = true;
    b.wake.notify_one();
}

void printConsole(JSContext*, PrJsHtmlConsoleLevel, uint32_t depth, PrStringView text) {
    for (uint32_t i = 0; i < depth; ++i)
        fputs("  ", stderr);
    fwrite(text.data, 1, text.size, stderr);
    fputc('\n', stderr);
}

constexpr PrJsHtmlConsolePrinter kConsolePrinter = {printConsole};

// ---------------------------------------------------------------------------------------------------------------------
// The page's thread

// Asks for a navigation, from a task of the page.
void navigateHost(void* context, PrHtmlPage*, PrStringView url) {
    static_cast<Browser*>(context)->pendingNavigation.assign(url.data, url.size);
}

// Frees a document: the engine first, since its objects' finalizers reach the page's window.
void closeDocument(Document& d) {
    prHtmlViewDestroy(d.view);
    prJsHtmlEngineDestroy(d.engine);
    prHtmlPageDestroy(d.page);
    d = Document{};
}

// Starts a navigation to `url` in a new page, which replaces the one that a navigation before it was loading.
void navigate(Browser& b, const std::string& url) {
    closeDocument(b.next);
    PrJsHtmlBind* const binds[] = {prJsUrlBind, prJsCharsetBind, prJsXhrBind};
    PrJsHtmlEngineDesc engine{.binds = binds, .bindCount = uint32_t(countOf(binds)), .console = &kConsolePrinter};
    b.next.engine = prJsHtmlEngineCreate(&engine);
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &b;
    host.fetcher = b.fetcher;
    host.navigate = navigateHost;
    host.agent = b.agent;
    host.font = b.font;
    host.shaper = b.shaper;
    host.images = b.images;
    host.device = b.device;
    host.compress = b.compress;
    host.localStorage = b.localStorage;
    host.sessionStorage = b.sessionStorage;
    host.sessionHistory = b.sessionHistory;
    host.now = clock;
    PrHtmlHost scripted = prJsHtmlEngineHost(b.next.engine, &host, PrJsHtmlScripting_Enabled);
    b.next.page = prHtmlPageCreate(&scripted);
    if (prHtmlPageNavigate(b.next.page, url) != PrStatus_Ok)
        fprintf(stderr, "browser: %s is not a URL\n", url.c_str());
}

// Shows the next document in place of the one shown, once its response has arrived.
void showNext(Browser& b) {
    if (b.next.page == nullptr || prHtmlPageIsNavigating(b.next.page))
        return;
    closeDocument(b.shown);
    b.shown = b.next;
    b.next = Document{};
    PrHtmlViewDesc view{.page = b.shown.page, .compositor = b.compositor, .jobBudget = kJobBudget};
    b.shown.view = prHtmlViewCreate(&view);
    wantFrame(b);
}

// Runs the agent's due tasks, each followed by the microtasks of the pages' engines, for as long as the budget allows.
// Then commits the scene that the layout thread recorded, and starts the next job.
void runPage(Browser& b) {
    double deadline = clock(nullptr) + kTaskBudget;
    while (clock(nullptr) < deadline && prDomAgentNextTaskDelay(b.agent) == 0 && prDomAgentRunTask(b.agent)) {
        for (Document* d : {&b.shown, &b.next}) {
            if (d->engine != nullptr)
                prJsHtmlEngineCheckpoint(d->engine);
        }
    }
    showNext(b);
    if (b.shown.view != nullptr)
        prHtmlViewUpdate(b.shown.view);
    if (!b.pendingNavigation.empty()) {
        std::string url = std::move(b.pendingNavigation);
        b.pendingNavigation.clear();
        navigate(b, url);
    }
}

// Handles input that the window's thread queued. Alt or Cmd with Left goes back, and with Right forward, as a
// platform's browsers do. Everything else goes to the page's view.
void pageEvent(Browser& b, QueuedEvent& queued) {
    const PrAppEvent& e = queued.event;
    if (e.kind == PrAppEventKind_KeyDown && (e.key == PrAppKey_Left || e.key == PrAppKey_Right)
        && (e.modifiers & (PrAppModifierFlag_Alt | PrAppModifierFlag_Super)) != 0) {
        if (b.shown.page != nullptr)
            prHtmlPageTraverse(b.shown.page, e.key == PrAppKey_Left ? -1 : 1);
        return;
    }
    if (b.shown.view == nullptr)
        return;
    queued.event.text = PrStringView{queued.text.data(), queued.text.size()};
    prHtmlViewEvent(b.shown.view, &queued.event, queued.flags);
    prJsHtmlEngineCheckpoint(b.shown.engine);
}

// The page's thread's event loop: it sleeps until something wakes it, then handles the input, takes the window's frame,
// and runs the page.
void pageThread(Browser& b) {
    navigate(b, b.address);
    for (;;) {
        std::vector<QueuedEvent> input;
        PrAppFrame tick{};
        bool ticked = false;
        {
            std::unique_lock<std::mutex> lock(b.mutex);
            auto woken = [&] { return b.ending || b.ticked || b.woken || !b.queued.empty(); };
            double delay = prDomAgentNextTaskDelay(b.agent);
            if (delay < 0)
                b.wake.wait(lock, woken);
            else
                b.wake.wait_for(lock, std::chrono::duration<double, std::milli>(delay), woken);
            if (b.ending)
                break;
            input.swap(b.queued);
            ticked = b.ticked;
            tick = b.tick;
            b.ticked = false;
            b.woken = false;
        }
        for (QueuedEvent& queued : input)
            pageEvent(b, queued);
        if (ticked && b.shown.view != nullptr) {
            prHtmlViewSetViewport(b.shown.view, tick.extent);
            prHtmlViewSetDisplay(b.shown.view, &tick.display);
            prHtmlViewSetUserPreferences(b.shown.view, &tick.preferences);
        }
        runPage(b);
    }
    closeDocument(b.next);
    closeDocument(b.shown);
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
    PrString cacheRoot = prFileCacheDirectory();
    if (!prStringIsEmpty(&cacheRoot)) {
        std::string directory = std::string(prStringView(&cacheRoot).data, prStringView(&cacheRoot).size)
                                + "/Prax/Browser/Cache";
        PrStorageDiskDesc disk{.directory = directory, .limitBytes = kCacheBytes, .pool = b.io};
        prStorageDiskCreate(&disk, &b.cache);
    }
    prStringReset(&cacheRoot);
    if (b.cache == nullptr)
        b.cache = prStorageMemoryCreate(kCacheBytes, PrStorageEviction_LeastRecentlyUsed);
    PrString dataRoot = prFileDataDirectory();
    PrHtmlStorageProviderDesc local{.quotaBytes = kPrHtmlStorageQuotaBytes, .pool = b.io};
    std::string localDirectory;
    if (!prStringIsEmpty(&dataRoot)) {
        localDirectory = std::string(prStringView(&dataRoot).data, prStringView(&dataRoot).size)
                         + "/Prax/Browser/Local Storage";
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
    PrFetchNetworkDesc fetch{
        .net = b.net, .http = b.http, .compress = b.compress, .cache = b.cache};
    b.fetcher = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetch);

    b.sessionHistory = prHtmlSessionHistoryCreate();
    uint32_t cores = prThreadHardwareCount();
    b.workers = cores > 1 ? prTaskPoolCreate(PrTaskPoolBackend_Default, cores - 1) : nullptr;
    PrDomAgentOptions agentOptions = prDomAgentOptionsDefault();
    agentOptions.restylePool = b.workers;
    b.agent = prDomAgentCreate(&agentOptions);
    prDomAgentSetWake(b.agent, wakePage, &b);
    prDomAgentSetLayoutDone(b.agent, wakePage, &b);
}

// The loop's part in closing the network: the fetcher and HTTP run on it.
void closeOnLoop(void* PR_NULLABLE context) {
    Browser& b = *static_cast<Browser*>(context);
    prFetchNetworkDestroy(b.fetcher);
    prHttpDestroy(b.http);
    std::lock_guard<std::mutex> lock(b.mutex);
    b.fetcher = nullptr;
    b.wake.notify_one();
}

// Frees what every page shares, once the pages are gone. The cancels of their fetches run on the loop before the
// fetcher is destroyed there.
void closeShared(Browser& b) {
    prDomAgentDestroy(b.agent);
    prNetPost(b.net, closeOnLoop, &b);
    {
        std::unique_lock<std::mutex> lock(b.mutex);
        b.wake.wait(lock, [&] { return b.fetcher == nullptr; });
    }
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
    if (b.extent.x == 0 || b.extent.y == 0 || prGpuSwapChainAcquire(b.swapChain, b.extent, &drawable) != PrStatus_Ok
        || !prGpuCommandsBegin(b.device, nullptr, &commands))
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
    PrDrawTarget into{.extent = b.extent,
                      .format = PrPixelFormat(mapped ? PrPixelFormat_RgbaF16 : PrPixelFormat_Bgra8),
                      .commands = commands};
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
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment,
                                    PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard,
                                    PrGpuAccessFlag_ColorAttachment);
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

void start(void* context, PrApp* app) {
    Browser& b = *static_cast<Browser*>(context);
    b.app = app;
    PrAppSurface surface{};
    PrGpuSwapChainDesc swapChain{.intent = PrGpuSwapChainIntent_Hdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &b.device) != PrStatus_Ok
        || prAppSurface(app, &surface) != PrStatus_Ok
        || prGpuSwapChainCreate(b.device, &surface, &swapChain, &b.swapChain) != PrStatus_Ok
        || prDrawRendererCreate(PrDrawRendererBackend_Default, b.device, nullptr, &b.renderer) != PrStatus_Ok
        || prDrawDisplayCreate(b.device, &b.display) != PrStatus_Ok) {
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
    prThreadStart(&b.thread, [](void* PR_NULLABLE context) { pageThread(*static_cast<Browser*>(context)); }, &b);
    b.started = true;
    prAppRequestFrame(app);
}

// Each of the window's frames ticks the page's thread, then presents the compositor's last scene at the window's size.
void frame(void* context, PrApp* app, const PrAppFrame* f) {
    Browser& b = *static_cast<Browser*>(context);
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        b.tick = *f;
        b.ticked = true;
    }
    b.wake.notify_one();
    prGpuSwapChainWait(b.swapChain);
    b.extent = PrUVec2{uint32_t(std::lround(f->extent.x * f->display.pixelRatio)),
                       uint32_t(std::lround(f->extent.y * f->display.pixelRatio))};
    b.shownOn = f->display;
    prLayoutCompositorFrame(b.compositor);
    if (prLayoutCompositorWantsFrame(b.compositor))
        prAppRequestFrame(app);
}

// The window's thread scrolls the compositor at once under the pointer, unless the wheel waits for the page's
// listeners, and queues the input for the page.
void event(void* context, PrApp* app, const PrAppEvent* e) {
    Browser& b = *static_cast<Browser*>(context);
    PrHtmlViewEventFlags flags = 0;
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
            break;
        case PrAppEventKind_Scroll: {
            PrLayoutVec at = prLayoutVecFromPx(e->position);
            if (b.compositor != nullptr && !prLayoutCompositorWheelBlocks(b.compositor, at)) {
                prLayoutCompositorScrollAt(b.compositor, at,
                                           prLayoutVecFromPx(prAppEventScrollPixels(e, kPrAppScrollLineHeight)));
                flags = PrHtmlViewEventFlag_Scrolled;
                if (prLayoutCompositorWantsFrame(b.compositor))
                    prAppRequestFrame(app);
            }
            break;
        }
        default:
            break;
    }
    QueuedEvent queued{*e, std::string(e->text.data != nullptr ? e->text.data : "", e->text.size), flags};
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        b.queued.push_back(std::move(queued));
    }
    b.wake.notify_one();
}

void shutdown(void* context, PrApp*) {
    Browser& b = *static_cast<Browser*>(context);
    if (b.started) {
        {
            std::lock_guard<std::mutex> lock(b.mutex);
            b.ending = true;
        }
        b.wake.notify_one();
        prThreadJoin(&b.thread);
    }
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
    if (argc != 2) {
        fprintf(stderr, "usage: browser <url | path>\n");
        return 2;
    }
    static Browser b;
    // A path to a file opens the file at its file: URL. Anything else is a URL.
    b.address = argv[1];
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
