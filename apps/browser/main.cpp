// The browser example: a web browser in a window, with tabs, whose pages' scripts run, rendered through lib/layout and
// lib/draw, inside chrome built with Studio Kit.
//
// Usage: browser [--cache <directory>] [--stats] [--trace] [<url | path | search>...]
//
// Each argument opens a tab, read as the address bar reads its text (addressResolve). With none, the browser opens a
// tab on the start page. The network is lib/net's sockets, with the platform's TLS, under lib/http and lib/fetch, and
// an HTTP cache on disk, where the platform keeps a user's caches or in the directory that --cache names, as pr_shell's
// --cache does. Local storage, the favorites and the history are on disk too, where the platform keeps a user's data.
// --stats prints, once a second, how many frames the window presented, the time that the window's thread and the GPU
// spent on each, and the work that the renderer prepared for the last. Each navigation that the address bar or an
// argument starts prints how long it took to settle: until its document is complete and the last scene shows all of it.
// --trace prints each viewport that the window asks for and the page sees, each layout job as it starts and ends, and
// how long the viewport's changes took to settle, with the jobs that they took and those laid out at a replaced size.
//
// main() is the composition root. Each tab is a page (pr/html_page.h), the navigable that loads each document, with its
// view (pr/html_view.h) and a compositor of its own. One script engine (pr/html_quickjs.h) runs the scripts of every
// page, on the agent that runs their event loop. The chrome is a layout tree of its own, which lib/ui and Studio Kit
// build each frame (chrome.cpp). A frame presents the active tab's compositor, and renders the page's list into the
// page's box and the chrome's list over the window, in one pass.

#include "pr/app.h"
#include "pr/assert.h"
#include "pr/callback.h"
#include "pr/charset_quickjs.h"
#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/fetch_intercept.h"
#include "pr/file.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/mathml_quickjs.h"
#include "pr/print.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/svg_quickjs.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

#include "apps/browser/address.h"
#include "apps/browser/browser.h"
#include "apps/browser/commands.h"
#include "apps/browser/icons.h"

using namespace browser;
using namespace pr;

namespace {

// The most that the HTTP cache holds.
constexpr uint64_t kCacheBytes = uint64_t(256) << 20;

void printConsole(JSContext*, PrJsHtmlConsoleLevel, uint32_t depth, PrStringView text) {
    for (uint32_t i = 0; i < depth; ++i)
        fputs("  ", stderr);
    fwrite(text.data, 1, text.size, stderr);
    fputc('\n', stderr);
}

constexpr PrJsHtmlConsolePrinter kConsolePrinter = {printConsole};

// Returns the directory `name` under `root`, a directory that the platform names, which this releases, or the empty
// string if the platform names none.
String userDirectory(PrRawString root, const char* name) {
    String directory;
    if (!prRawStringIsEmpty(&root))
        directory = format("{}/Prax/Browser/{}", prRawStringView(&root), name);
    prRawStringReset(&root);
    return directory;
}

// ---------------------------------------------------------------------------------------------------------------------
// What every page shares

// Answers a navigation to one of the browser's pages with an empty document of the page's title, which the chrome draws
// the page over, and passes every other request on to the network. Only a navigation that the user starts, which has no
// origin, reaches a page, so that no document can show the user's history in its tab or in a frame.
void answerBrowserPage(void* PR_NULLABLE, PrFetchInterceptRequest* request) {
    const PrFetchRequestDesc& desc = request->desc;
    if (!addressIsBrowserPage(desc.url)) {
        prFetchInterceptRequestPass(request);
        return;
    }
    if (desc.url != kAddressHistory || desc.origin.data != nullptr || desc.mode != PrFetchMode_Navigate) {
        prFetchInterceptRequestFail(request, PrNetError_Blocked);
        return;
    }
    constexpr PrStringView kDocument = "<!DOCTYPE html><title>History</title>";
    PrFetchHeader type{"Content-Type", "text/html;charset=utf-8"};
    PrFetchInterceptResponse response{
        .status = 200,
        .type = PrFetchResponseType_Basic,
        .headers = &type,
        .headerCount = 1,
        .body =prStringBytes(kDocument),
    };
    prFetchInterceptRequestRespond(request, &response);
}

// Opens text, images, media, storage, the network, the agent and the script engine, and reads the favorites and the
// history.
void openShared(Browser& b) {
    b.fonts = prFontSetCreate(nullptr);
    b.system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(b.fonts, b.system);
    b.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrTextFontDesc font{.set = b.fonts, .style = prFontStyleDefault()};
    b.font = prTextFontCreate(&font);
    b.compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    b.images = prImageCodecCreate(PrImageCodecBackend_Default, b.compress);
    b.media = prMediaCodecCreate(PrMediaCodecBackend_Default);
    // Without an output device, pages play without sound.
    b.audioOutput = prAudioOutputCreate(PrAudioOutputBackend_Default, PrAudioFormat{48000, 2});
    if (b.audioOutput != nullptr)
        b.mixer = prAudioMixerCreate(b.audioOutput);
    b.io = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);

    // The cache, local storage, the favorites and the history are on disk where the platform keeps a user's, so that
    // they outlive the run.
    String directory = b.cacheDirectory;
    if (directory.isEmpty())
        directory = userDirectory(prFileUserDirectory(PrFileUserDirectory_Cache), "Cache");
    if (!directory.isEmpty()) {
        PrStorageDiskDesc disk{.directory = directory, .limitBytes = kCacheBytes, .pool = b.io};
        if (prStorageDiskCreate(&disk, &b.cache) != PrStatus_Ok)
            printError("browser: cannot open the cache in {}\n", directory);
    }
    if (b.cache == nullptr)
        b.cache = prStorageMemoryCreate(kCacheBytes, PrStorageEviction_LeastRecentlyUsed);
    // A platform that names no data directory keeps local storage in memory.
    String localDirectory = userDirectory(prFileUserDirectory(PrFileUserDirectory_Data), "Local Storage");
    PrHtmlStorageProviderDesc local{.directory = localDirectory, .quotaBytes = kPrHtmlStorageQuotaBytes, .pool = b.io};
    b.localStorage = prHtmlStorageProviderCreate(&local);
    PrHtmlStorageProviderDesc session{.quotaBytes = kPrHtmlStorageQuotaBytes};
    b.sessionStorage = prHtmlStorageProviderCreate(&session);
    b.favoritesPath = userDirectory(prFileUserDirectory(PrFileUserDirectory_Data), "Favorites.json");
    if (b.favoritesPath.isEmpty() || !favoritesLoad(b.favoritesPath, &b.favorites))
        b.favorites = favoritesDefault();
    b.historyPath = userDirectory(prFileUserDirectory(PrFileUserDirectory_Data), "History.json");
    if (!b.historyPath.isEmpty())
        historyLoad(b.historyPath, &b.history);
    for (const Visit& visit : b.history)
        b.visitedUrls.add(visit.url);

    b.net = prNetCreate(PrNetBackend_Default);
    b.tls = prTlsCreate(PrTlsBackend_Default, b.net);
    PrHttpDesc http{.net = b.net, .tls = b.tls};
    b.http = prHttpCreate(PrHttpBackend_Default, &http);
    PrFetchNetworkDesc fetch{.net = b.net, .http = b.http, .compress = b.compress, .cache = b.cache};
    b.network = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetch);
    PrFetchInterceptDesc pages{.net = b.net, .request = answerBrowserPage, .next = b.network};
    b.fetcher = prFetchInterceptCreate(&pages);

    uint32_t cores = prThreadHardwareCount();
    b.workers = cores > 1 ? prTaskPoolCreate(PrTaskPoolBackend_Default, cores - 1) : nullptr;
    PrDomAgentOptions agentOptions = prDomAgentOptionsDefault();
    agentOptions.restylePool = b.workers;
    b.agent = prDomAgentCreate(&agentOptions);
    PrJsHtmlBind* const binds[] = {prJsSvgBind, prJsMathMlBind, prJsUrlBind, prJsCharsetBind, prJsXhrBind};
    PrJsHtmlEngineDesc engine{.binds = binds, .bindCount = uint32_t(countOf(binds)), .console = &kConsolePrinter};
    b.engine = prJsHtmlEngineCreate(&engine);
}

// Frees what every page shares, once the pages and the engine are gone. The fetchers and HTTP run on the network's
// loop, and are freed there, after the cancels of the pages' fetches. The fetcher of the browser's pages goes before
// the fetcher that it passes requests to.
void closeShared(Browser& b) {
    prDomAgentDestroy(b.agent);
    auto destroyOnLoop = [&] {
        prFetchNetworkDestroy(b.fetcher);
        prFetchNetworkDestroy(b.network);
        prHttpDestroy(b.http);
    };
    prNetPostBlocking(b.net, PR_SCOPED_CALLBACK(destroyOnLoop));
    prTlsDestroy(b.tls);
    prNetDestroy(b.net);
    prStorageDestroy(b.cache);
    prHtmlStorageProviderDestroy(b.localStorage);
    prHtmlStorageProviderDestroy(b.sessionStorage);
    prTaskPoolDestroy(b.workers);
    prTaskPoolDestroy(b.io);
    prImageCodecDestroy(b.images);
    prMediaCodecDestroy(b.media);
    prAudioMixerDestroy(b.mixer);
    prAudioOutputDestroy(b.audioOutput);
    prCompressCodecDestroy(b.compress);
    prTextFontRelease(b.font);
    prTextShaperDestroy(b.shaper);
}

// ---------------------------------------------------------------------------------------------------------------------
// The chrome

// Makes the chrome's layout tree, which has a shaper of its own because the pages shape on other threads.
void openChrome(Browser& b) {
    b.chromeShaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc tree{.shaper = b.chromeShaper};
    b.tree = prLayoutTreeCreate(&tree);
    b.ui = prUiCreate(b.tree);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Light, b.fonts);
    b.kit = prKitCreate(b.ui, &theme);
    b.icons = iconsCreate(b);
    commandsCreate(b);
    b.chromeList = prDrawListCreate();
}

void closeChrome(Browser& b) {
    commandsDestroy(b);
    prCacheDestroy(b.icons);
    prDrawListRelease(b.chromeList);
    prLayoutSceneRelease(b.scene);
    prKitDestroy(b.kit);
    prUiDestroy(b.ui);
    prLayoutTreeDestroy(b.tree);
    prTextShaperDestroy(b.chromeShaper);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Browser b;
    for (int at = 1; at < argc; ++at) {
        PrStringView argument = argv[at];
        if (argument == "--cache" && at + 1 < argc) {
            b.cacheDirectory = argv[++at];
        } else if (argument == "--stats") {
            b.stats = true;
        } else if (argument == "--trace") {
            b.trace = true;
        } else if (prStringStartsWith(argument, "--")) {
            printError("usage: browser [--cache <directory>] [--stats] [--trace] [<url | path | search>...]\n");
            return 2;
        } else {
            String url = addressResolve(argument);
            if (!url.isEmpty())
                b.addresses.push(url);
        }
    }
    openShared(b);
    openChrome(b);
    PrAppDesc desc{.title = "Prax", .extent = {1280, 860}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler = browserHandler(b);
    prAppRun(app, &handler);
    prAppDestroy(app);
    closeChrome(b);
    closeShared(b);
    prFontSetDestroy(b.fonts);
    prFontSystemDestroy(b.system);
    return 0;
}
