// The page host (page_host.h). It implements lib/html's host callbacks, fetches pages and their resources as a browser
// does, answers the requests for captured URLs from their files, and binds the engine's JavaScript APIs in each page's
// script context.

#include "tools/shell/page_host.h"

#include "pr/ascii.h"
#include "pr/audio_memory.h"
#include "pr/base64.h"
#include "pr/callback.h"
#include "pr/charset_quickjs.h"
#include "pr/compress_builtin.h"
#include "pr/count_of.h"
#include "pr/css_presentation.h"
#include "pr/fetch.h"
#include "pr/fetch_intercept.h"
#include "pr/file.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/html_storage.h"
#include "pr/html_structure.h"
#include "pr/html_view.h"
#include "pr/http.h"
#include "pr/image_builtin.h"
#include "pr/json.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/mathml_quickjs.h"
#include "pr/mem.h"
#include "pr/net.h"
#include "pr/net_null.h"
#include "pr/quickjs_binding.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/string_ops.h"
#include "pr/svg_quickjs.h"
#include "pr/task.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/tls.h"
#include "pr/url.h"
#include "pr/url_percent.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

#include "tools/harness/harness.h"

#include "quickjs.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace pr;
using namespace harness;

namespace page_host {

// A resource of a captured page (addCapture): its file, or empty if the capture omitted it, and the Content-Type that
// it was served with, if known.
struct CapturedResource {
    std::filesystem::path file;
    std::string type;
};

struct HostState {
    bool timedOut = false;      // the run passed its timeout, so painting waits for no more of the network

    PrJsHtmlEngine* engine = nullptr;       // runs the pages' scripts, and the host's own outside any page
    PrDomAgent* agent = nullptr;            // the single event loop that every page runs on
    PrFontSet* fonts = nullptr;
    PrTextFont* font = nullptr;             // the default text font of every page
    PrTextShaper* shaper = nullptr;         // shapes the text of every page
    PrCompressCodec* compress = nullptr;
    PrImageCodec* images = nullptr;            // decodes the images of every page
    PrMediaCodec* PR_NULLABLE media = nullptr; // reads the media resources of every page, or null
    PrAudioMixer* PR_NULLABLE mixer = nullptr; // plays the audio of every page, or null

    // The device that presents the pages, or null if they are presented in memory.
    PrGpuDevice* PR_NULLABLE device = nullptr;

    float pageGap = 0;          // the space between the pages of a paint for print media, in CSS pixels

    // The pages' local storage and session storage, in memory, which a run's pages share as the pages of one tab do.
    // Each run starts with both empty, so that what a page stores does not depend on the pages that ran before it.
    PrHtmlStorageProvider* PR_NULLABLE localStorage = nullptr;
    PrHtmlStorageProvider* PR_NULLABLE sessionStorage = nullptr;

    // The network that the pages fetch over, which every run shares once the first page opens it. Its loop is a thread
    // of its own with sockets (useNetwork), and otherwise runs on the host's thread (`nullNet`). The host's fetcher
    // answers captured URLs, and passes any other request to the built-in fetcher.
    PrNet* PR_NULLABLE net = nullptr;
    bool nullNet = true;
    PrTls* PR_NULLABLE tls = nullptr;
    PrHttp* PR_NULLABLE http = nullptr;
    PrFetchNetwork* PR_NULLABLE builtin = nullptr;
    PrFetchNetwork* PR_NULLABLE fetcher = nullptr;

    // The built-in fetcher's HTTP cache, which the runs share as a browser under wptrunner keeps its cache from one
    // test to the next, or null without sockets. A server that marks its resources cacheable is asked for each once in
    // a run. The cache is in memory, or on disk in `cacheDirectory` (useDiskCache), where `cacheIo` writes it.
    PrStorage* PR_NULLABLE cache = nullptr;
    std::string cacheDirectory;
    PrTaskPool* PR_NULLABLE cacheIo = nullptr;

    // How the network is opened: with sockets or not, the rules that resolve hosts (addHostRule), and the DER
    // certificates that HTTPS trusts besides the system's (addTrustAnchor), with the views of them that HTTP holds.
    bool sockets = false;
    std::vector<std::pair<std::string, std::string>> hostRules;
    std::vector<std::string> anchors;
    std::vector<PrByteView> anchorViews;

    // What the pages loaded next are served and run with: the captured resources, and the scripts that run in each
    // window they open, before its document's own scripts. The loop reads the captures under `servedLock`.
    std::mutex servedLock;
    std::map<std::string, CapturedResource> captured;   // by serialized URL (addCapture)
    std::vector<std::string> preludes;

    // The script that waits for a captured page to be ready (setCaptureWait), or empty.
    std::string captureWait;

    // The virtual clock that the pages run on, in milliseconds, which advances only when they wait. Each run starts it
    // at kVirtualStart. `now` points to it if the clock is virtual, and is null if the pages run on the system's clock.
    double virtualTime = 0;
    double* PR_NULLABLE now = nullptr;

    // The memory output that renders in step with the virtual clock (paceAudio), or null.
    PrAudioOutput* PR_NULLABLE pacedAudio = nullptr;

    // The wall clock that the pages' dates read with a virtual clock (wallNow). It is the virtual clock's time since
    // the run started plus the system's time since the virtual clock last moved, so that a script that waits on dates
    // in a loop ends, and it never goes back. Each is in milliseconds: the wall time at which the run started, the
    // system's monotonic time at which the virtual clock last moved, and the last wall time read.
    double wallStart = 0;
    double movedAt = 0;
    double lastWall = 0;
    bool allFrames = false;                     // run every animation frame, rather than the observed ones

    bool verify = false;                        // check each page's presentation after its rendering updates
    bool scripts = true;                        // run the pages' own scripts
    PrAppUserPreferences preferences{};         // the user's preferences that the pages load with
    PrVec2 viewport = {800, 600};               // the viewport that the pages load in, in CSS pixels
    uint32_t restyleThreads = 0;                // as in PrDomAgentOptions::restyleThreads
    PrTaskPool* workers = nullptr;              // the threads that share the agent's work with the loop's thread

    // The wake of the host's thread when the loop posts a task to the agent, which a wait for the network sleeps on.
    std::mutex wakeLock;
    std::condition_variable wake;
    bool woken = false;
    uint64_t posts = 0;         // the number of tasks posted to the agent so far, under `wakeLock`

    // True while settle waits for fetches that a rendering update started after every page stopped the run.
    bool awaitingRender = false;

    std::vector<std::unique_ptr<Page>> pages;   // every page, including the pages of iframes
};

// An event of the page's input (sendInput), with a copy of its text.
struct InputEvent {
    PrAppEvent event;
    std::string text;
};

// Input that a page sent, which the host dispatches as a task, and the function that resolves the promise that
// sendInput returned once it has.
struct PendingInput {
    std::vector<InputEvent> events;
    JSValue resolve;
};

struct Page {
    JSContext* PR_NULLABLE ctx; // the page's realm, once its browsing context is opened
    std::string name;           // what load was given, or the name of the page whose iframe holds the page
    std::string url;            // the URL that the page loads at
    HostState* host;
    PrHtmlPage* htmlPage = nullptr;  // null once destroyed
    bool ok = true;
    bool finished = false;      // the page said it is done (stopRun), and nothing that it waits for matters
    bool requested = false;     // loaded by PageHost::load, rather than an iframe's
    bool verifyFailed = false;  // the page's presentation failed a check (PageHost::verifyRendering)
    Capture capture = Capture_None;

    // The view that presents the page, made when it is first painted or sent input, and the input that the page sent
    // and the host has yet to dispatch.
    PrHtmlView* PR_NULLABLE view = nullptr;
    std::vector<PendingInput> input = {};
    PrVec2 viewport = {800, 600};   // the view's viewport, the host's until a paint gives another

    // True while a print or a paint for print media has left the view laid out in pages over `pageBox`, `pageGap`
    // apart.
    bool paged = false;
    PrLayoutPageBox pageBox{};
    float pageGap = 0;
};

namespace {

JSValue print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    for (int i = 0; i < argc; ++i) {
        const char* text = JS_ToCString(ctx, argv[i]);
        if (text == nullptr)
            return JS_EXCEPTION;
        fputs(i != 0 ? " " : "", stdout);
        fputs(text, stdout);
        JS_FreeCString(ctx, text);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

// The console's printer. Writes each message on a line of stderr, indented by its group's depth.
void printConsole(JSContext*, PrJsHtmlConsoleLevel, uint32_t depth, PrStringView text) {
    for (uint32_t i = 0; i < depth; ++i)
        fputs("  ", stderr);
    fwrite(text.data, 1, text.size, stderr);
    fputc('\n', stderr);
}

constexpr PrJsHtmlConsolePrinter kConsolePrinter = {printConsole};

// Runs the script file in `ctx`. Returns false, saying why on stderr, if the file cannot be read, or if the script
// throws outside a window, which reports an author's exception to itself.
bool runFile(JSContext* ctx, const char* path, bool window = false) {
    PrTextBuffer text = {};
    bool ok = harnessReadFile(&text, path);
    if (ok)
        ok = prJsHtmlEngineRun(ctx, prTextBufferView(&text), path, 1, 1) || window;
    else
        fprintf(stderr, "cannot read %s\n", path);
    prTextBufferReset(&text);
    return ok;
}

// The virtual time that a document's commit takes, in milliseconds, which puts its scripts after its time origin.
constexpr double kCommitDuration = 1;

// The virtual clock's time when a run starts, in milliseconds. It is the same in every run, so that the times that
// pages read, and the order of the timers and frames that fall due together, never depend on when the run started: the
// sum of a time and a delay rounds by the time's magnitude.
constexpr double kVirtualStart = 0;

// Advances the virtual clock by `delay` milliseconds.
void moveClock(HostState& host, double delay) {
    *host.now += delay;
    host.movedAt = prTimeMonotonicMilliseconds();
    if (host.pacedAudio != nullptr)
        prAudioMemoryAdvance(host.pacedAudio, *host.now);
}

void committedHtml(void* context, PrHtmlPage*) {
    moveClock(*static_cast<Page*>(context)->host, kCommitDuration);
}

// Returns the virtual clock's time.
double virtualNow(void* context) {
    return *static_cast<Page*>(context)->host->now;
}

// Returns the wall clock's time on a virtual clock (HostState::wallStart).
double virtualWallNow(void* context) {
    HostState& host = *static_cast<Page*>(context)->host;
    double wall = host.wallStart + (*host.now - kVirtualStart) + (prTimeMonotonicMilliseconds() - host.movedAt);
    host.lastWall = wall > host.lastWall ? wall : host.lastWall;
    return host.lastWall;
}

bool sleepMilliseconds(double delay) {
    prThreadSleep(delay);
    return true;
}

// The most times that a paint paints again with the responses to the fetches that its paint before started.
constexpr uint32_t kMaxPaintPasses = 4;

// The longest that a paint waits for the network to answer the fetches that it started, in milliseconds of the system's
// time.
constexpr double kPaintNetworkWait = 10000;

// The most bytes that the HTTP cache keeps.
constexpr uint64_t kCacheBytes = 64 * 1024 * 1024;

// Waits `delay` milliseconds. On the system's clock it sleeps. On a virtual clock it returns at once and advances the
// clock by `delay`.
bool waitMilliseconds(HostState& host, double delay) {
    if (host.now == nullptr)
        return sleepMilliseconds(delay);
    moveClock(host, delay);
    return true;
}

// Returns the MIME type that WPT's server serves a file as, by its extension, or the empty string for an extension that
// the shell does not know.
const char* contentTypeOf(const std::filesystem::path& file) {
    struct ContentType {
        const char* extension;
        const char* type;
    };
    constexpr ContentType kTypes[] = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".xhtml", "application/xhtml+xml"},
        {".xht", "application/xhtml+xml"},
        {".svg", "image/svg+xml"},
        {".md", "text/markdown"},
        {".markdown", "text/markdown"},
        {".xml", "application/xml"},
        {".css", "text/css"},
        {".js", "text/javascript"},
        {".mjs", "text/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".jpg", "image/jpeg"},
    };
    std::string extension = file.extension().string();
    for (const ContentType& entry : kTypes) {
        if (extension == entry.extension)
            return entry.type;
    }
    return "";
}

// Answers a request of the host's fetcher, on the loop: a captured resource from its file, with the type that it was
// served with, and any other request by passing it to the built-in fetcher. A captured response is basic, with no CORS
// check, as the built-in fetcher answers a file: URL, whatever the request's mode. A resource that the capture omitted,
// or whose file does not load, is a network error.
void answerResource(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    HostState& s = *static_cast<HostState*>(context);
    CapturedResource resource;
    {
        std::lock_guard<std::mutex> lock(s.servedLock);
        std::map<std::string, CapturedResource>::const_iterator captured =
            s.captured.find(std::string(request->desc.url.data, request->desc.url.size));
        if (captured == s.captured.end()) {
            prFetchInterceptRequestPass(request);
            return;
        }
        resource = captured->second;
    }
    PrTextBuffer text = {};
    if (resource.file.empty() || !harnessReadFile(&text, resource.file.string().c_str())) {
        prTextBufferReset(&text);
        prFetchInterceptRequestFail(request, PrNetError_Refused);
        return;
    }
    std::string type = resource.type.empty() ? contentTypeOf(resource.file) : resource.type;
    PrFetchHeader header{"Content-Type", type};
    PrStringView body = prTextBufferView(&text);
    PrFetchInterceptResponse response{
        .status = 200,
        .headers = &header,
        .headerCount = type.empty() ? 0u : 1u,
        .body = prStringBytes(body),
    };
    prFetchInterceptRequestRespond(request, &response);
    prTextBufferReset(&text);
}

// Runs the network's loop on the host's thread, if it runs there.
void runNetwork(HostState& s) {
    if (s.net != nullptr && s.nullNet)
        prNetNullRun(s.net);
}

// Wakes the host's thread when a task is posted to the agent, as the loop does with each response.
void wakeHost(void* PR_NULLABLE context) {
    HostState& s = *static_cast<HostState*>(context);
    std::lock_guard<std::mutex> lock(s.wakeLock);
    s.woken = true;
    ++s.posts;
    s.wake.notify_all();
}

// Returns the number of tasks posted to the agent so far.
uint64_t postsOf(HostState& s) {
    std::lock_guard<std::mutex> lock(s.wakeLock);
    return s.posts;
}

// Sleeps until the loop posts a task to the agent, or until `deadline` in milliseconds of the system's time, or for
// ever if `deadline` is negative. Returns false if the deadline passed first.
bool waitForWake(HostState& s, double deadline) {
    std::unique_lock<std::mutex> lock(s.wakeLock);
    if (deadline < 0) {
        s.wake.wait(lock, [&] { return s.woken; });
    } else {
        double left = deadline - prTimeMonotonicMilliseconds();
        s.wake.wait_for(lock, std::chrono::duration<double, std::milli>(left > 0 ? left : 0), [&] { return s.woken; });
    }
    bool woken = s.woken;
    s.woken = false;
    return woken;
}

// Returns true while a page's document or a resource of a page is being fetched.
bool fetching(const HostState& s) {
    for (const std::unique_ptr<Page>& page : s.pages) {
        PrHtmlPage* context = page->htmlPage;
        if (context != nullptr && prHtmlPageIsFetching(context))
            return true;
    }
    return false;
}

// Returns true if the pages wait for a server to answer: the network's loop runs apart from the host's thread, and a
// fetch is in flight. The virtual clock stands while they wait, so that a server answers before any timer is due.
bool waitsForNetwork(const HostState& s) {
    return !s.nullNet && fetching(s);
}

// The engine's wait for what a module waits on: the network while a page fetches, with the pages' clock standing, and
// otherwise a timer, on the pages' clock. A network that runs on the host's thread answers as it runs.
void waitForModule(void* context, double delay) {
    HostState& s = *static_cast<HostState*>(context);
    if (!fetching(s))
        waitMilliseconds(s, delay);
    else if (s.nullNet)
        runNetwork(s);
    else
        waitForWake(s, -1);
}

// Returns the path's segments, each percent-encoded as a URL's path segment, each after a '/'. A Windows drive, such as
// C:, stays as a file URL spells it (rfc 8089).
std::string encodedPath(const std::filesystem::path& relative) {
    std::string path;
    for (const std::filesystem::path& segment : relative) {
        std::string text = segment.string();
        if (segment == relative.root_directory())
            continue;
        if (relative.has_root_name() && segment == relative.root_name()) {
            path += '/' + text;
            continue;
        }
        PrStringView input{text.data(), text.size()};
        PrUrlPercentEncodeSet set = PrUrlPercentEncodeSet_Component;
        std::string encoded(prUrlPercentEncode(input, set, nullptr, 0), '\0');
        prUrlPercentEncode(input, set, encoded.data(), encoded.size());
        path += '/' + encoded;
    }
    return path;
}

// Returns the file: URL of the page at `path`. A path that starts with a Windows drive is absolute, including under
// Node.js on Windows, where a WebAssembly program's paths are the host's but std::filesystem knows no drives.
std::string urlOfPath(const std::filesystem::path& path) {
    std::string text = path.generic_string();
    if (text.size() >= 3 && prAsciiIsLetter(text[0]) && text[1] == ':' && text[2] == '/')
        return "file:///" + text.substr(0, 2) + encodedPath(std::filesystem::path(text.substr(2)).lexically_normal());
    return "file://" + encodedPath(std::filesystem::absolute(path).lexically_normal());
}

// Returns true if `location` is an absolute URL rather than a path. A scheme of one letter is a drive's.
bool isUrl(const std::string& location) {
    PrUrl* url = nullptr;
    bool parsed = prUrlCreate(location, nullptr, PrCharset_Utf8, &url) == PrStatus_Ok;
    bool absolute = parsed && prUrlScheme(url).size > 1;
    prUrlDestroy(url);
    return absolute;
}

PrHtmlHost startPage(HostState& host, const std::string& name, const std::string& url);

// Starts an iframe's browsing context, under the name of the page that holds it. The engine gives it a realm of its
// own.
void startFrame(void* context, PrHtmlPage*, PrHtmlHost* child) {
    Page* run = static_cast<Page*>(context);
    *child = startPage(*run->host, run->name, std::string());
}

void closedHtml(void* context, PrHtmlPage*) {
    static_cast<Page*>(context)->htmlPage = nullptr;
}

// Reports how a requested page's load ended. A failure fails the page, and so does an XML document that is not
// well-formed, which holds the parsererror element that explains why.
void loadedHtml(void* context, PrHtmlPage* htmlPage, PrStatus status) {
    Page* run = static_cast<Page*>(context);
    if (!run->requested)
        return;
    if (status != PrStatus_Ok) {
        PrStringView name = prStatusName(status);
        fprintf(stderr, "%s: HTML load failed: %.*s\n", run->name.c_str(), int(name.size), name.data);
        run->ok = false;
    }
    PrDomElement* element = prDomDocumentElement(prHtmlWindowDocument(prHtmlPageWindow(htmlPage)));
    if (element != nullptr && prDomElementNamespace(element) == PrDomNamespace_Other) {
        if (prDomElementLocalName(element) == "parsererror") {
            PrRawString message = prDomNodeTextContent(prDomElementAsNode(element));
            PrStringView view = prRawStringView(&message);
            fprintf(stderr, "%s: %.*s\n", run->name.c_str(), int(view.size), view.data);
            prRawStringReset(&message);
            run->ok = false;
        }
    }
    // A captured page runs the capture's wait once it has loaded, which stops the run (stopRun()) once the page is
    // ready.
    const std::string& wait = run->host->captureWait;
    if (run->capture == Capture_Wait && !wait.empty() && run->ok && run->ctx != nullptr) {
        std::string waiter = "(function () {\n" + wait + "\n}).call(window, location.href, stopRun);";
        run->ok = prJsHtmlEngineRun(run->ctx, waiter, "capture-wait", 1, 1) && run->ok;
    }
}

// Checks the page's presentation, which is up to date, and writes its first failure to stderr.
void verifyRender(Page* run, PrCssPresentation* presentation) {
    if (run->verifyFailed)
        return;
    PrRawString report{};
    if (!prCssPresentationVerify(presentation, &report)) {
        run->verifyFailed = true;
        PrStringView what = prRawStringView(&report);
        fprintf(stderr, "%s: presentation verify: %.*s\n", run->name.c_str(), int(what.size), what.data);
    }
    prRawStringReset(&report);
}

// Checks the layout of the page's printed pages (prLayoutTreeVerify), which is up to date, apart from the checks of its
// presentation that its rendering updates make. Writes the first failure to stderr.
void verifyPrint(Page* run, PrCssPresentation* presentation) {
    PrLayoutTreeDifference difference{};
    if (!prLayoutTreeVerify(prCssPresentationTree(presentation), &difference)) {
        run->verifyFailed = true;
        PrStringView what = prRawStringView(&difference.what);
        fprintf(stderr, "%s: print verify: %.*s\n", run->name.c_str(), int(what.size), what.data);
    }
    prRawStringReset(&difference.what);
}

void renderedHtml(void* context, PrHtmlPage* htmlPage) {
    if (PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage))
        verifyRender(static_cast<Page*>(context), presentation);
}

// Opens the network, and the fetchers over it, if no page has yet. On the build's network, HTTP resolves hosts by the
// host's rules and trusts its anchors.
void openNetwork(HostState& s) {
    if (s.net != nullptr)
        return;
    if (s.sockets) {
        s.net = prNetCreate(PrNetBackend_Default);
        s.nullNet = false;
        s.tls = prTlsCreate(PrTlsBackend_Default, s.net);
        std::vector<PrHttpHostRule> rules;
        for (const auto& [host, target] : s.hostRules)
            rules.push_back({.host = host, .target = target});
        s.anchorViews.clear();
        for (const std::string& anchor : s.anchors)
            s.anchorViews.push_back(prStringBytes(anchor));
        PrHttpDesc http{
            .net = s.net,
            .tls = s.tls,
            .anchors = s.anchorViews.data(),
            .anchorCount = uint32_t(s.anchorViews.size()),
            .hostRules = rules.data(),
            .hostRuleCount = uint32_t(rules.size()),
        };
        s.http = prHttpCreate(PrHttpBackend_Default, &http);
        if (!s.cacheDirectory.empty()) {
            s.cacheIo = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
            PrStorageDiskDesc disk{.directory = s.cacheDirectory, .limitBytes = kCacheBytes, .pool = s.cacheIo};
            if (prStorageDiskCreate(&disk, &s.cache) != PrStatus_Ok)
                fprintf(stderr, "cannot open the cache in %s\n", s.cacheDirectory.c_str());
        }
        if (s.cache == nullptr)
            s.cache = prStorageMemoryCreate(kCacheBytes, PrStorageEviction_LeastRecentlyUsed);
    }
    if (s.net == nullptr) {
        s.net = prNetNullCreate();
        s.nullNet = true;
    }
    PrFetchNetworkDesc builtin{.net = s.net, .http = s.http, .compress = s.compress, .cache = s.cache};
    s.builtin = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &builtin);
    PrFetchInterceptDesc fetcher{.net = s.net, .request = answerResource, .context = &s, .next = s.builtin};
    s.fetcher = prFetchInterceptCreate(&fetcher);
}

// Frees the network and its fetchers. The browsing contexts are gone, and the cancels of their fetches were posted
// before this.
void closeNetwork(HostState& s) {
    if (s.net == nullptr)
        return;
    auto destroyOnLoop = [&] {
        prFetchNetworkDestroy(s.fetcher);
        prFetchNetworkDestroy(s.builtin);
        prHttpDestroy(s.http);
    };
    prNetPostBlocking(s.net, PR_SCOPED_CALLBACK(destroyOnLoop));
    prTlsDestroy(s.tls);
    prNetDestroy(s.net);
    prStorageDestroy(s.cache);
    s.cache = nullptr;
    prTaskPoolDestroy(s.cacheIo);
    s.cacheIo = nullptr;
    s.fetcher = nullptr;
    s.builtin = nullptr;
    s.http = nullptr;
    s.tls = nullptr;
    s.net = nullptr;
}

// Starts a page, and returns the host of the browsing context that it runs in, before the engine runs its scripts.
PrHtmlHost startPage(HostState& state, const std::string& name, const std::string& url) {
    openNetwork(state);
    state.pages.push_back(std::make_unique<Page>(Page{nullptr, name, url, &state}));
    Page* run = state.pages.back().get();
    run->viewport = state.viewport;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = run;
    host.agent = state.agent;
    host.font = state.font;
    host.shaper = state.shaper;
    host.images = state.images;
    host.media = state.media;
    host.mixer = state.mixer;
    host.device = state.device;
    host.compress = state.compress;
    host.fetcher = state.fetcher;
    host.localStorage = state.localStorage;
    host.sessionStorage = state.sessionStorage;
    // On a virtual clock, which no one watches unless the host shows animations, a page runs only the animation frames
    // that are observed.
    if (state.now != nullptr) {
        host.now = virtualNow;
        host.wallNow = virtualWallNow;
        host.documentCommitted = committedHtml;
        host.animationFrames = state.allFrames ? PrHtmlAnimationFrames_All : PrHtmlAnimationFrames_Observed;
    }
    host.frame = startFrame;
    host.closed = closedHtml;
    host.loaded = loadedHtml;
    if (state.verify)
        host.rendered = renderedHtml;
    return host;
}

// readFile(path): returns the text of the file, or null if it cannot be read.
JSValue jsReadFile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1)
        return JS_NULL;

    const char* path = JS_ToCString(ctx, argv[0]);
    if (path == nullptr)
        return JS_EXCEPTION;

    PrTextBuffer text = {};
    bool read = harnessReadFile(&text, path);
    JS_FreeCString(ctx, path);
    JSValue value = read ? jsNewStringView(ctx, prTextBufferView(&text)) : JS_NULL;
    prTextBufferReset(&text);
    return value;
}

// counters(): returns what the process has spent so far, as harnessCounters counts it: {instructions, cycles,
// performanceInstructions, performanceCycles, energy, time}. A benchmark measures with it if it is defined.
JSValue jsCounters(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    HarnessCounters counters = harnessCounters();
    JSValue value = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, value, "instructions", JS_NewFloat64(ctx, counters.instructions));
    JS_SetPropertyStr(ctx, value, "cycles", JS_NewFloat64(ctx, counters.cycles));
    JS_SetPropertyStr(ctx, value, "performanceInstructions", JS_NewFloat64(ctx, counters.performanceInstructions));
    JS_SetPropertyStr(ctx, value, "performanceCycles", JS_NewFloat64(ctx, counters.performanceCycles));
    JS_SetPropertyStr(ctx, value, "energy", JS_NewFloat64(ctx, counters.energy));
    JS_SetPropertyStr(ctx, value, "time", JS_NewFloat64(ctx, counters.millis));
    JS_SetPropertyStr(ctx, value, "cpu", JS_NewFloat64(ctx, counters.cpu));
    return value;
}

// stopRun(): marks the page as done, and the run stops waiting for its timers, frames and animations.
JSValue jsStopRun(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (Page* run = static_cast<Page*>(prJsHtmlEngineHostContext(ctx)))
        run->finished = true;
    return JS_UNDEFINED;
}

// Returns the named key that has the name of UI Events `name`, or else the printable key of its single ASCII character,
// or PrAppKey_Unknown.
PrAppKey keyNamed(const std::string& name) {
    for (uint32_t key = PrAppKey_Left; key <= PrAppKey_F20; ++key) {
        if (prAppKeyName(PrAppKey(key)) == name)
            return PrAppKey(key);
    }
    if (name.size() == 1 && uint8_t(name[0]) < 128)
        return PrAppKey(prAsciiUpper(name[0]));
    return PrAppKey_Unknown;
}

// Returns the code that has the name of UI Events `name`, or PrAppKeyCode_Unidentified.
PrAppKeyCode codeNamed(const std::string& name) {
    for (uint32_t code = 0; code < PrAppKeyCode_Count; ++code) {
        if (prAppKeyCodeName(PrAppKeyCode(code)) == name)
            return PrAppKeyCode(code);
    }
    return PrAppKeyCode_Unidentified;
}

// Reads one event of sendInput's list into `out`. Returns false if `value` is not an object of a known type.
bool readInputEvent(JSContext* ctx, JSValueConst value, InputEvent* out) {
    auto string = [&](const char* name) {
        JSValue field = JS_GetPropertyStr(ctx, value, name);
        std::string result;
        if (JS_IsString(field)) {
            const char* chars = JS_ToCString(ctx, field);
            result = chars != nullptr ? chars : "";
            JS_FreeCString(ctx, chars);
        }
        JS_FreeValue(ctx, field);
        return result;
    };
    auto number = [&](const char* name) {
        JSValue field = JS_GetPropertyStr(ctx, value, name);
        double result = 0;
        if (JS_IsNumber(field))
            JS_ToFloat64(ctx, &result, field);
        JS_FreeValue(ctx, field);
        return result;
    };
    struct Kind {
        const char* name;
        PrAppEventKind kind;
    };
    constexpr Kind kKinds[] = {
        {"pointerMove", PrAppEventKind_PointerMove}, {"pointerDown", PrAppEventKind_PointerDown},
        {"pointerUp", PrAppEventKind_PointerUp},     {"scroll", PrAppEventKind_Scroll},
        {"keyDown", PrAppEventKind_KeyDown},         {"keyUp", PrAppEventKind_KeyUp},
        {"text", PrAppEventKind_Text},
    };
    std::string type = string("type");
    const Kind* kind = nullptr;
    for (const Kind& candidate : kKinds) {
        if (type == candidate.name)
            kind = &candidate;
    }
    if (kind == nullptr)
        return false;
    PrAppEvent& event = out->event;
    event = PrAppEvent{};
    event.kind = kind->kind;
    event.time = prTimeMonotonicMilliseconds() / 1000;
    event.position = PrVec2{float(number("x")), float(number("y"))};
    event.delta = PrVec2{float(number("deltaX")), float(number("deltaY"))};
    event.button = uint8_t(number("button"));
    event.modifiers = PrAppModifierFlags(number("modifiers"));
    event.key = keyNamed(string("key"));
    event.code = codeNamed(string("code"));
    out->text = string("text");
    return true;
}

// sendInput(events): queues the page's input, a list of events, each an object with a type (pointerMove, pointerDown,
// pointerUp, scroll, keyDown, keyUp or text) and the fields of PrAppEvent that the type uses: x, y, button, deltaX,
// deltaY, modifiers as PrAppModifierFlags, key as a key value of UI Events, code as a code of UI Events, and text. The
// host dispatches it through the page's view as a task, as a user's input arrives, and resolves the promise returned.
JSValue jsSendInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Page* run = static_cast<Page*>(prJsHtmlEngineHostContext(ctx));
    if (argc < 1 || run == nullptr)
        return JS_ThrowTypeError(ctx, "sendInput takes a list of events in a page");
    PendingInput pending;
    int64_t length = 0;
    JSValue count = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToInt64(ctx, &length, count);
    JS_FreeValue(ctx, count);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[0], uint32_t(i));
        InputEvent event;
        bool read = readInputEvent(ctx, item, &event);
        JS_FreeValue(ctx, item);
        if (!read)
            return JS_ThrowTypeError(ctx, "sendInput: event %lld has no known type", static_cast<long long>(i));
        pending.events.push_back(std::move(event));
    }
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    pending.resolve = functions[0];
    JS_FreeValue(ctx, functions[1]);
    run->input.push_back(std::move(pending));
    return promise;
}

// Gives a realm the shell's functions. A page's realm also runs the preludes, once its global is its window.
void realmOpened(void* context, JSContext* ctx, PrHtmlPage* PR_NULLABLE htmlPage) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, JsCallScoped<print>::call, "print", 1));
    JS_SetPropertyStr(ctx, global, "readFile", JS_NewCFunction(ctx, JsCallScoped<jsReadFile>::call, "readFile", 1));
    JS_SetPropertyStr(ctx, global, "stopRun", JS_NewCFunction(ctx, JsCallScoped<jsStopRun>::call, "stopRun", 0));
    JS_SetPropertyStr(ctx, global, "sendInput", JS_NewCFunction(ctx, JsCallScoped<jsSendInput>::call, "sendInput", 1));
    JS_SetPropertyStr(ctx, global, "counters", JS_NewCFunction(ctx, JsCallScoped<jsCounters>::call, "counters", 0));
    JS_FreeValue(ctx, global);
    Page* run = static_cast<Page*>(prJsHtmlEngineHostContext(ctx));
    if (run == nullptr)
        return;
    run->ctx = ctx;
    run->htmlPage = htmlPage;
    for (const std::string& prelude : static_cast<HostState*>(context)->preludes)
        run->ok = runFile(ctx, prelude.c_str(), true) && run->ok;
}

// Starts what pages run on: the script runtime, the event loop, and the clock. The pages' dates start at the system's
// time, as pages expect. The network outlives the run.
void startRun(HostState& s) {
    PrJsHtmlBind* const binds[] = {prJsSvgBind, prJsMathMlBind, prJsUrlBind, prJsCharsetBind, prJsXhrBind};
    PrJsHtmlEngineDesc engine = {
        .binds = binds,
        .bindCount = uint32_t(countOf(binds)),
        .console = &kConsolePrinter,
        .context = &s,
        .realmOpened = realmOpened,
        .wait = waitForModule,
    };
    s.engine = prJsHtmlEngineCreate(&engine);
    uint32_t threads = s.restyleThreads != 0 ? s.restyleThreads : prThreadHardwareCount();
    if (threads > 1)
        s.workers = prTaskPoolCreate(PrTaskPoolBackend_Default, threads - 1);
    PrDomAgentOptions options = prDomAgentOptionsDefault();
    options.restyleThreads = s.restyleThreads;
    options.restylePool = s.workers;
    s.agent = prDomAgentCreate(&options);
    prDomAgentSetWake(s.agent, wakeHost, &s);
    s.virtualTime = kVirtualStart;
    // The last run's pages are gone, and with them the sources that ran the output, so its clock may start over.
    if (s.pacedAudio != nullptr)
        prAudioMemoryAdvance(s.pacedAudio, s.virtualTime);
    s.movedAt = prTimeMonotonicMilliseconds();
    s.wallStart = prTimeWallMilliseconds();
    s.lastWall = s.wallStart;
    s.awaitingRender = false;
    PrHtmlStorageProviderDesc storage{.quotaBytes = kPrHtmlStorageQuotaBytes};
    s.localStorage = prHtmlStorageProviderCreate(&storage);
    s.sessionStorage = prHtmlStorageProviderCreate(&storage);
}

// Closes every page and frees what startRun started. A page that is destroyed cancels its fetches, so that the loop
// posts nothing more to it. The engine is destroyed before the pages, because the finalizers of its objects reach their
// windows.
void stopRun(HostState& s) {
    for (const std::unique_ptr<Page>& page : s.pages) {
        for (const PendingInput& pending : page->input)
            JS_FreeValue(page->ctx, pending.resolve);
        page->input.clear();
    }
    prJsHtmlEngineDestroy(s.engine);
    s.engine = nullptr;
    for (const std::unique_ptr<Page>& page : s.pages) {
        prHtmlViewDestroy(page->view);
        prHtmlPageDestroy(page->htmlPage);
    }
    prHtmlStorageProviderDestroy(s.localStorage);
    prHtmlStorageProviderDestroy(s.sessionStorage);
    s.localStorage = nullptr;
    s.sessionStorage = nullptr;
    // The loop runs the cancels of the fetches that the browsing contexts dropped.
    runNetwork(s);
    s.pages.clear();
    prDomAgentSetWake(s.agent, nullptr, nullptr);
    prDomAgentDestroy(s.agent);
    s.agent = nullptr;
    prTaskPoolDestroy(s.workers);
    s.workers = nullptr;
}

// Returns the view that presents the page, making it the first time.
PrHtmlView* viewOf(Page* page) {
    if (page->view == nullptr) {
        PrHtmlViewDesc desc{.page = page->htmlPage};
        page->view = prHtmlViewCreate(&desc);
        prHtmlViewSetCaretHidden(page->view, true);
        prHtmlViewSetViewport(page->view, page->viewport);
    }
    return page->view;
}

// Dispatches one event of a host's input through the page's view, and runs the jobs that it queued. Brings the page's
// layout up to date first, as a browser does before it hit tests an event, and after a move, hovers what is under the
// pointer.
void dispatchEvent(HostState& s, Page* page, const PrAppEvent& event) {
    if (page->htmlPage == nullptr)
        return;
    PrHtmlView* view = viewOf(page);
    if (PrCssPresentation* presentation = prHtmlPagePresentation(page->htmlPage))
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(page->viewport));
    prHtmlViewEvent(view, &event, 0);
    if (event.kind == PrAppEventKind_PointerMove) {
        prHtmlViewUpdate(view);
        prHtmlViewWait(view);
    }
    prJsHtmlEngineCheckpoint(s.engine);
}

// Dispatches the input that the pages sent through their views, as a task, and resolves each sendInput's promise.
// Returns true if there was input.
bool dispatchInput(HostState& s) {
    bool any = false;
    for (size_t at = 0; at < s.pages.size(); ++at) {
        Page* page = s.pages[at].get();
        while (!page->input.empty()) {
            any = true;
            PendingInput pending = std::move(page->input.front());
            page->input.erase(page->input.begin());
            for (InputEvent& input : pending.events) {
                input.event.text = input.text;
                dispatchEvent(s, page, input.event);
            }
            JS_FreeValue(page->ctx, JS_Call(page->ctx, pending.resolve, JS_UNDEFINED, 0, nullptr));
            JS_FreeValue(page->ctx, pending.resolve);
            prJsHtmlEngineCheckpoint(s.engine);
        }
    }
    return any;
}

// Serves `file` with `type`, which may be empty, for the URL `location`, or a network error if `file` is empty. Does
// nothing if `location` is not a URL.
void addCaptured(HostState& s, PrStringView location, const std::filesystem::path& file, PrStringView type) {
    PrUrl* url = nullptr;
    if (location.data == nullptr || prUrlCreate(location, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return;
    PrRawString serialized = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    PrStringView href = prRawStringView(&serialized);
    std::lock_guard<std::mutex> lock(s.servedLock);
    s.captured[std::string(href.data, href.size)] = {
        file,
        std::string(type.data != nullptr ? type.data : "", type.size),
    };
    prRawStringReset(&serialized);
    prUrlDestroy(url);
}

// Brings each page's presentation up to date at its viewport, and returns true if that started a fetch, which a server
// is to answer, or whose response the host has already posted. An update computes the styles that no update has yet,
// which start the fetches of the images that they name. A document whose load ended before its first rendering update,
// as an XML document's may, has its images fetched only then. Writes to `started` whether a fetch now waits for a
// server, where none waited before.
bool renderFetches(HostState& s, bool* started) {
    uint64_t posts = postsOf(s);
    bool network = waitsForNetwork(s);
    for (const std::unique_ptr<Page>& page : s.pages) {
        PrCssPresentation* presentation = page->htmlPage != nullptr ? prHtmlPagePresentation(page->htmlPage) : nullptr;
        if (presentation != nullptr)
            prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(page->viewport));
    }
    runNetwork(s);
    *started = !network && waitsForNetwork(s);
    return *started || postsOf(s) != posts;
}

// Reads the DER certificate of the PEM file at `path`, its first one, into `out`. Returns false if it cannot.
bool readPemCertificate(const std::filesystem::path& path, std::string* out) {
    PrTextBuffer text = {};
    bool ok = harnessReadFile(&text, path.string().c_str());
    std::string pem = ok ? std::string(prTextBufferView(&text).data, prTextBufferView(&text).size) : std::string();
    prTextBufferReset(&text);
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    size_t start = pem.find(begin);
    size_t end = pem.find("-----END CERTIFICATE-----");
    if (start == std::string::npos || end == std::string::npos || end < start)
        return false;
    PrStringView body{pem.data() + start + begin.size(), end - start - begin.size()};
    out->assign(prBase64DecodedCapacity(body.size), '\0');
    size_t size = 0;
    if (!prBase64Decode(body, out->data(), out->size(), &size))
        return false;
    out->resize(size);
    return true;
}

// Returns the file of page `number`, from 1, of a document written to `path` as a file for each page
// (PageHost::documentFiles).
std::filesystem::path pageFile(const std::filesystem::path& path, uint32_t number) {
    std::filesystem::path file = path;
    file.replace_filename(path.stem().string() + "-" + std::to_string(number) + path.extension().string());
    return file;
}

} // namespace

PageHost::PageHost(PrTextShaper* shaper, PrMediaCodec* PR_NULLABLE media, PrAudioMixer* PR_NULLABLE mixer) :
        state_(std::make_unique<HostState>()) {
    HostState& s = *state_;
    s.fonts = prFontSetCreate(nullptr);
    // The PDF standard fonts, and the Complete bundle for the characters that they lack.
    const PrFontBundle bundles[] = {PrFontBundle_Standard14, PrFontBundle_Complete};
    prFontBundleInstall(s.fonts, bundles, 2, PR_FONT_BUNDLE_DIR);
    PrTextFontDesc defaultFontDesc{.set = s.fonts, .style = prFontStyleDefault()};
    s.font = prTextFontCreate(&defaultFontDesc);
    s.shaper = shaper;
    s.compress = prCompressBuiltinCreate();
    s.images = prImageBuiltinCreate(s.compress);
    s.media = media;
    s.mixer = mixer;
    startRun(s);
}

PageHost::~PageHost() {
    HostState& s = *state_;
    stopRun(s);
    closeNetwork(s);
    prTextFontRelease(s.font);
    prImageCodecDestroy(s.images);
    prCompressCodecDestroy(s.compress);
    prFontSetDestroy(s.fonts);
}

void PageHost::reset() {
    HostState& s = *state_;
    stopRun(s);
    s.timedOut = false;
    startRun(s);
}

bool PageHost::addCapture(const std::filesystem::path& capture, std::string* pageUrl, const std::filesystem::path& page) {
    PrTextBuffer text = {};
    PrJson* json = prJsonCreate();
    bool ok = harnessReadFile(&text, capture.string().c_str()) && prJsonParse(json, prTextBufferView(&text), nullptr);
    if (!ok) {
        fprintf(stderr, "%s: not a capture\n", capture.string().c_str());
        prTextBufferReset(&text);
        prJsonDestroy(json);
        return false;
    }
    PrStringView url = prJsonString(json, prJsonMember(json, 0, "url"));
    *pageUrl = std::string(url.data != nullptr ? url.data : "", url.size);
    // The page itself is served from its file, with the type that it was served with.
    std::filesystem::path html = !page.empty() ? page : std::filesystem::path(capture).replace_extension(".html");
    addCaptured(*state_, url, html, prJsonString(json, prJsonMember(json, 0, "type")));
    uint32_t resources = prJsonMember(json, 0, "resources");
    for (uint32_t i = 0;; ++i) {
        uint32_t entry = prJsonElement(json, resources, i);
        if (entry == kPrJsonMissing)
            break;
        PrStringView file = prJsonString(json, prJsonMember(json, entry, "file"));
        if (file.data != nullptr)
            addCaptured(*state_, prJsonString(json, prJsonMember(json, entry, "url")),
                capture.parent_path() / std::string(file.data, file.size), prJsonString(json, prJsonMember(json, entry, "type")));
    }
    // What the capture omitted fails, rather than reaching the network.
    uint32_t omitted = prJsonMember(json, 0, "omitted");
    for (uint32_t i = 0;; ++i) {
        uint32_t entry = prJsonElement(json, omitted, i);
        if (entry == kPrJsonMissing)
            break;
        addCaptured(*state_, prJsonString(json, prJsonMember(json, entry, "url")), {}, PrStringView{nullptr, 0});
    }
    prTextBufferReset(&text);
    prJsonDestroy(json);
    return true;
}

void PageHost::addPrelude(const std::filesystem::path& script) {
    state_->preludes.push_back(script.string());
}

bool PageHost::addFont(const std::filesystem::path& file) {
    std::string path = file.string();
    const PrFont* font = nullptr;
    if (prFontCreateFromFile(path, 0, nullptr, &font) != PrStatus_Ok) {
        fprintf(stderr, "cannot load the font %s\n", path.c_str());
        return false;
    }
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(state_->fonts, &descriptor, font);
    prFontRelease(font);
    return true;
}

PrFontSet* PageHost::fonts() {
    return state_->fonts;
}

void PageHost::setHinting(PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter) {
    prFontSetSetHinting(state_->fonts, hinting, hinter);
}

void PageHost::setDevice(PrGpuDevice* PR_NULLABLE device) {
    state_->device = device;
}

void PageHost::setPageGap(float gap) {
    state_->pageGap = gap;
}

void PageHost::useVirtualTime() {
    state_->now = &state_->virtualTime;
}

void PageHost::paceAudio(PrAudioOutput* output) {
    state_->pacedAudio = output;
    prAudioMemoryAdvance(output, state_->virtualTime);
}

void PageHost::runAllAnimationFrames() {
    state_->allFrames = true;
}

double PageHost::time() const {
    return state_->now != nullptr ? *state_->now : prTimeMonotonicMilliseconds();
}

void PageHost::advance(double until) {
    HostState& s = *state_;
    if (s.now == nullptr)
        return;
    for (;;) {
        if (!prJsHtmlEngineCheckpoint(s.engine))
            return;
        runNetwork(s);
        double delay = prDomAgentNextTaskDelay(s.agent);
        // The clock stands while a server answers.
        if (delay != 0 && waitsForNetwork(s)) {
            waitForWake(s, -1);
            continue;
        }
        if (delay < 0 || *s.now + delay > until) {
            if (*s.now >= until)
                return;
            moveClock(s, until - *s.now);
            continue;
        }
        moveClock(s, delay);
        if (!prDomAgentRun(s.agent, 0))
            return;
    }
}

bool PageHost::useNetwork() {
    if (state_->net != nullptr) {
        fprintf(stderr, "the network is chosen before any page loads\n");
        return false;
    }
    if (!prNetBackendIsBuilt(PrNetBackend_Default) || !prTlsBackendIsBuilt(PrTlsBackend_Default)) {
        fprintf(stderr, "this build has no network\n");
        return false;
    }
    state_->sockets = true;
    return true;
}

bool PageHost::useDiskCache(const std::filesystem::path& directory) {
    if (!useNetwork())
        return false;
    state_->cacheDirectory = directory.string();
    return true;
}

void PageHost::addHostRule(const std::string& host, const std::string& target) {
    state_->hostRules.emplace_back(host, target);
}

bool PageHost::addTrustAnchor(const std::filesystem::path& pem) {
    std::string anchor;
    if (!readPemCertificate(pem, &anchor)) {
        fprintf(stderr, "cannot read the certificate %s\n", pem.string().c_str());
        return false;
    }
    state_->anchors.push_back(std::move(anchor));
    return true;
}

bool PageHost::setCaptureWait(const std::filesystem::path& script) {
    PrTextBuffer text = {};
    bool ok = harnessReadFile(&text, script.string().c_str());
    if (ok)
        state_->captureWait.assign(prTextBufferView(&text).data, prTextBufferView(&text).size);
    else
        fprintf(stderr, "cannot read %s\n", script.string().c_str());
    prTextBufferReset(&text);
    return ok;
}

JSContext* PageHost::scriptContext() {
    return prJsHtmlEngineContext(state_->engine);
}

bool PageHost::runFile(const char* path) {
    return page_host::runFile(scriptContext(), path) && prJsHtmlEngineCheckpoint(state_->engine);
}

Page* PR_NULLABLE PageHost::load(const std::string& location, Capture capture) {
    HostState& s = *state_;
    std::string url = location;
    if (!isUrl(location)) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(location, error)) {
            fprintf(stderr, "cannot read %s\n", location.c_str());
            return nullptr;
        }
        url = urlOfPath(location);
    }
    PrHtmlHost host = startPage(s, location, url);
    Page& run = *static_cast<Page*>(host.context);
    PrJsHtmlScripting scripting = s.scripts ? PrJsHtmlScripting_Enabled : PrJsHtmlScripting_Disabled;
    PrHtmlHost scripted = prJsHtmlEngineHost(s.engine, &host, scripting);
    PrHtmlPage* htmlPage = prHtmlPageCreate(&scripted);
    prHtmlPageSetUserPreferences(htmlPage, &s.preferences);
    prHtmlPageSetViewport(htmlPage, run.viewport);
    run.requested = true;
    run.capture = capture;
    if (!run.ok)
        return &run;
    PrStatus navigated = prHtmlPageNavigate(htmlPage, url);
    if (navigated != PrStatus_Ok) {
        PrStringView name = prStatusName(navigated);
        fprintf(stderr, "%s: cannot load %s: %.*s\n", run.name.c_str(), url.c_str(), int(name.size), name.data);
        run.ok = false;
    }
    run.ok = prJsHtmlEngineCheckpoint(s.engine) && run.ok;
    return &run;
}

void PageHost::disableScripts() {
    state_->scripts = false;
}

void PageHost::setUserPreferences(const PrAppUserPreferences& preferences) {
    state_->preferences = preferences;
}

PrAppUserPreferences PageHost::userPreferences() const {
    return state_->preferences;
}

void PageHost::setViewport(PrVec2 viewport) {
    state_->viewport = viewport;
}

void PageHost::setRestyleThreads(uint32_t threads) {
    HostState& s = *state_;
    s.restyleThreads = threads;
    stopRun(s);
    startRun(s);
}

void PageHost::verifyRendering() {
    state_->verify = true;
}

uint32_t PageHost::verify(PrVec2 viewport) {
    uint32_t failed = 0;
    for (const std::unique_ptr<Page>& page : state_->pages) {
        if (state_->verify && page->htmlPage != nullptr) {
            if (PrCssPresentation* presentation = prHtmlPagePresentation(page->htmlPage)) {
                // Painting brings the presentation up to date, as a frame does.
                PrDrawList* list = prDrawListCreate();
                paint(page.get(), Media_Screen, viewport, prAppDisplayDefault(), state_->preferences, list);
                prDrawListRelease(list);
                verifyRender(page.get(), presentation);
            }
        }
        failed += page->verifyFailed ? 1 : 0;
    }
    return failed;
}

bool PageHost::pageOk(const Page* page) {
    return page->ok;
}

const std::string& PageHost::pageName(const Page* page) {
    return page->name;
}

PrHtmlPage* PR_NULLABLE PageHost::htmlPage(const Page* page) {
    return page->htmlPage;
}

Settled PageHost::settle(double timeout) {
    HostState& s = *state_;
    double deadline = timeout >= 0 ? prTimeMonotonicMilliseconds() + timeout : -1;
    for (;;) {
        if (deadline >= 0 && prTimeMonotonicMilliseconds() >= deadline)
            return Settled_TimedOut;
        if (!prJsHtmlEngineCheckpoint(s.engine))
            return Settled_Failed;
        runNetwork(s);
        // A run stops once each page that it loaded is done, whatever an iframe or a page's own work waits for.
        bool requested = false;
        bool finished = true;
        for (const std::unique_ptr<Page>& page : s.pages) {
            requested |= page->requested;
            finished &= !page->requested || page->finished;
        }
        bool done = requested && finished;
        // A page's input comes first, as a user's input does when a page waits for it.
        if (!done && dispatchInput(s))
            continue;
        // The pages, and the frames in them, run on the single agent's event loop. While a server answers a fetch, the
        // clock stands, unless every page is done and the fetch is not one that a rendering update started.
        double delay = prDomAgentNextTaskDelay(s.agent);
        bool network = waitsForNetwork(s);
        if (!network)
            s.awaitingRender = false;
        if (delay != 0 && network && (!done || s.awaitingRender)) {
            waitForWake(s, deadline);
            continue;
        }
        // Pages in which only animations want frames have settled, once their presentations fetch nothing more.
        if (done || prDomAgentIsIdle(s.agent) || delay < 0) {
            bool started = false;
            if (!renderFetches(s, &started))
                return Settled_Idle;
            s.awaitingRender = started;
            continue;
        }
        if (delay > 0) {
            // A browser has a rendering opportunity before a wait ends, so the presentations come up to date before the
            // clock moves, and the fetches that the update starts are answered while it stands.
            bool started = false;
            if (renderFetches(s, &started)) {
                s.awaitingRender = started;
                continue;
            }
            if (!waitMilliseconds(s, delay))
                return Settled_Failed;
        }
        prDomAgentRun(s.agent, 0);
    }
}

void PageHost::stopWaiting() {
    state_->timedOut = true;
}

void PageHost::input(Page* page, const PrAppEvent& event) {
    dispatchEvent(*state_, page, event);
}

PrAppOutput PageHost::output(Page* page, double time) {
    if (page->htmlPage == nullptr)
        return PrAppOutput{};
    prHtmlViewSetTime(viewOf(page), time);
    return prHtmlViewOutput(viewOf(page));
}

void PageHost::setInteractive(Page* page, PrApp* app) {
    if (page->htmlPage == nullptr)
        return;
    prHtmlViewSetApp(viewOf(page), app);
    prHtmlViewSetCaretHidden(viewOf(page), false);
}

bool PageHost::finished(const Page* page) {
    if (page->htmlPage == nullptr)
        return true;
    PrDomElement* root = prDomDocumentElement(prHtmlWindowDocument(prHtmlPageWindow(page->htmlPage)));
    if (root == nullptr)
        return true;
    PrDomTokenList* classes = prDomTokenListCreate(root, "class");
    bool waiting = prDomTokenListContains(classes, "reftest-wait");
    prDomTokenListDestroy(classes);
    return !waiting;
}

bool PageHost::signalRendered(PrVec2 viewport) {
    bool fired = false;
    for (const std::unique_ptr<Page>& page : state_->pages) {
        if (finished(page.get()))
            continue;
        PrDrawList* list = prDrawListCreate();
        paint(page.get(), Media_Screen, viewport, prAppDisplayDefault(), state_->preferences, list);
        prDrawListRelease(list);
        PrDomElement* root = prDomDocumentElement(prHtmlWindowDocument(prHtmlPageWindow(page->htmlPage)));
        PrDomEventInit init = prDomEventInitDefault();
        init.bubbles = true;
        PrDomEvent* event = prDomEventCreate("TestRendered", &init);
        prDomEventTargetDispatchEvent(prDomNodeAsEventTarget(prDomElementAsNode(root)), event, nullptr);
        prDomEventRelease(event);
        fired = true;
    }
    return fired;
}

bool PageHost::paint(Page* page, Media media, PrVec2 viewport, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences, PrDrawList* list, PrDrawStructure* PR_NULLABLE structure) {
    if (page->htmlPage == nullptr || prHtmlPagePresentation(page->htmlPage) == nullptr)
        return false;
    PrHtmlView* view = viewOf(page);
    // Paginating again lays the whole document out again, so a view that stays paged in the same box is left as it is.
    bool paged = media == Media_Print;
    PrLayoutPageBox paper = prLayoutPageBoxDefault();
    float gap = state_->pageGap;
    if (paged != page->paged ||
            (paged && (!prMemEquals(&page->pageBox, &paper, sizeof(PrLayoutPageBox)) || page->pageGap != gap))) {
        prHtmlViewSetPaged(view, paged ? &paper : nullptr, gap);
        page->paged = paged;
        page->pageBox = paper;
        page->pageGap = gap;
    }
    page->viewport = viewport;
    prHtmlViewSetViewport(view, viewport);
    prHtmlViewSetDisplay(view, &display);
    prHtmlViewSetUserPreferences(view, &preferences);
    if (paged)
        prepareWithFetches(view, [](PrHtmlView* printed) { prHtmlViewPageCount(printed); });
    else
        prepareWithFetches(view, prHtmlViewPrepare);
    // The list is the caller's, with what the caller has recorded in it, so the document is recorded in it once.
    prHtmlViewSetMarking(view, structure != nullptr ? PrLayoutMarking_Content : PrLayoutMarking_None);
    prHtmlViewPaint(view, list);
    prHtmlViewSetMarking(view, PrLayoutMarking_None);
    if (state_->verify && paged)
        verifyPrint(page, prHtmlPagePresentation(page->htmlPage));
    else if (state_->verify)
        verifyRender(page, prHtmlPagePresentation(page->htmlPage));
    if (structure != nullptr)
        prHtmlStructureAppend(page->htmlPage, structure);
    return true;
}

bool PageHost::print(Page* page, const PrLayoutPageBox& initial, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences, std::vector<PrintedPage>& pages,
        PrDrawStructure* PR_NULLABLE structure, PrintTimings* PR_NULLABLE timings) {
    if (page->htmlPage == nullptr || prHtmlPagePresentation(page->htmlPage) == nullptr)
        return false;
    double started = prTimeMonotonicMilliseconds();
    PrHtmlView* view = viewOf(page);
    prHtmlViewSetDisplay(view, &display);
    prHtmlViewSetUserPreferences(view, &preferences);
    // Paginating again lays the whole document out again, so a view that stays paged in the same box is left as it is.
    if (!page->paged || !prMemEquals(&page->pageBox, &initial, sizeof(PrLayoutPageBox)))
        prHtmlViewSetPaged(view, &initial, page->pageGap);
    page->paged = true;
    page->pageBox = initial;
    prepareWithFetches(view, [](PrHtmlView* paged) { prHtmlViewPageCount(paged); });
    uint32_t count = prHtmlViewPageCount(view);
    double paginated = prTimeMonotonicMilliseconds();
    prHtmlViewSetMarking(view, structure != nullptr ? PrLayoutMarking_Content : PrLayoutMarking_None);
    for (uint32_t i = 0; i < count; ++i) {
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaintPage(view, i, list);
        PrLayoutPage printed = prHtmlViewPage(view, i);
        PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&printed.box);
        pages.push_back(PrintedPage{
            .list = list,
            .extent = prLayoutVecToPx(sheet.extent),
            .bleed = prLayoutRectToPx(sheet.bleed),
            .trim = prLayoutRectToPx(sheet.trim),
        });
    }
    double painted = prTimeMonotonicMilliseconds();
    prHtmlViewSetMarking(view, PrLayoutMarking_None);
    if (state_->verify)
        verifyPrint(page, prHtmlPagePresentation(page->htmlPage));
    if (structure != nullptr)
        prHtmlStructureAppend(page->htmlPage, structure);
    if (timings != nullptr) {
        *timings = PrintTimings{
            .paginate = paginated - started,
            .paint = painted - paginated,
            .structure = prTimeMonotonicMilliseconds() - painted,
        };
    }
    return true;
}

bool PageHost::writeDocument(Page* page, Media media, const PrLayoutPageBox& pageBox, PrVec2 viewport,
        PrDrawWriterBackend backend, const std::filesystem::path& path, DocumentTimings* PR_NULLABLE timings) {
    std::vector<PrintedPage> pages;
    PrintTimings phases{};
    double started = prTimeMonotonicMilliseconds();
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);
    // The paint marks the content, which gives its text runs their text, and a format that tags its content joins the
    // marks to the structure.
    bool ok = false;
    if (media == Media_Print) {
        ok = print(page, pageBox, prAppDisplayDefault(), userPreferences(), pages, structure, &phases);
    } else {
        PrDrawList* list = prDrawListCreate();
        pages.push_back(PrintedPage{.list = list, .extent = viewport});
        ok = paint(page, Media_Screen, viewport, prAppDisplayDefault(), userPreferences(), list, structure);
    }
    if (!ok)
        fprintf(stderr, "%s: no rendering to write\n", pageName(page).c_str());
    PrDrawWriterOptions options = prDrawWriterOptionsDefault();
    options.compress = state_->compress;
    options.images = state_->images;
    options.tasks = state_->workers;
    PrDrawWriter* writer = nullptr;
    ok = ok && prDrawWriterCreate(backend, &options, &writer) == PrStatus_Ok;
    if (ok) {
        std::vector<std::string> outputs;
        std::string title = pageTitle(page);
        PrDrawDocumentDesc desc{
            .write = [](void* PR_NULLABLE context, uint32_t output, PrByteView data) {
                std::vector<std::string>& written = *static_cast<std::vector<std::string>*>(context);
                if (output >= written.size())
                    written.resize(output + 1);
                written[output].append(reinterpret_cast<const char*>(data.data), data.size);
            },
            .context = &outputs,
            .structure = structure,
            .title = title,
            .language = prDrawStructureNodeDesc(structure, kPrDrawStructureRoot).language,
        };
        std::vector<PrDrawPage> written;
        for (const PrintedPage& printed : pages)
            written.push_back(PrDrawPage{
                .list = printed.list,
                .extent = printed.extent,
                .bleed = printed.bleed,
                .trim = printed.trim,
            });
        double writing = prTimeMonotonicMilliseconds();
        prDrawWriterWrite(writer, &desc, written.data(), uint32_t(written.size()));
        double filing = prTimeMonotonicMilliseconds();
        // The files of an earlier write go first, so that what remains is this write's.
        std::error_code error;
        for (const std::filesystem::path& file : documentFiles(path))
            std::filesystem::remove(file, error);
        size_t bytes = 0;
        for (size_t i = 0; i < outputs.size() && ok; ++i) {
            std::filesystem::path file = outputs.size() > 1 ? pageFile(path, uint32_t(i + 1)) : path;
            const std::string& output = outputs[i];
            ok = prFileWrite(file.string(), prStringBytes(output));
            bytes += output.size();
        }
        if (timings != nullptr) {
            *timings = DocumentTimings{
                .pages = uint32_t(pages.size()),
                .bytes = bytes,
                .print = phases,
                .other = writing - started - phases.paginate - phases.paint - phases.structure,
                .write = filing - writing,
                .file = prTimeMonotonicMilliseconds() - filing,
            };
        }
        if (!ok)
            fprintf(stderr, "%s: cannot write %s\n", pageName(page).c_str(), path.string().c_str());
    }
    prDrawWriterDestroy(writer);
    prDrawStructureDestroy(structure);
    for (const PrintedPage& printed : pages)
        prDrawListRelease(printed.list);
    return ok;
}

void PageHost::prepareWithFetches(PrHtmlView* view, void (*prepare)(PrHtmlView* view)) {
    prepare(view);
    // Preparing a document that computes a style for the first time starts the fetches of the images that the style
    // names. The responses are used in tasks, which run before the document is prepared again with them. A page whose
    // styles were computed before its load ended had its load wait for them instead.
    HostState& s = *state_;
    double deadline = prTimeMonotonicMilliseconds() + (s.timedOut ? 0 : kPaintNetworkWait);
    for (uint32_t pass = 0; pass < kMaxPaintPasses; ++pass) {
        // Every response that the server is to send arrives before the document is prepared again.
        bool ran = false;
        for (;;) {
            runNetwork(s);
            while (prDomAgentNextTaskDelay(s.agent) == 0 && prDomAgentRun(s.agent, 0))
                ran = true;
            if (!waitsForNetwork(s) || !waitForWake(s, deadline))
                break;
        }
        if (!ran)
            break;
        prepare(view);
    }
}

std::vector<std::filesystem::path> PageHost::documentFiles(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error))
        return {path};
    std::vector<std::filesystem::path> files;
    for (uint32_t number = 1;; ++number) {
        std::filesystem::path file = pageFile(path, number);
        if (!std::filesystem::exists(file, error))
            break;
        files.push_back(file);
    }
    return files;
}

PrTaskPool* PR_NULLABLE PageHost::workers() const {
    return state_->workers;
}

std::string PageHost::pageTitle(Page* page) {
    if (page->htmlPage == nullptr)
        return {};
    PrRawString title = prDomDocumentTitle(prHtmlWindowDocument(prHtmlPageWindow(page->htmlPage)));
    PrStringView view = prRawStringView(&title);
    std::string result(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&title);
    return result;
}

} // namespace page_host
