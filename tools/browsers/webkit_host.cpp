// webkit_host: runs the system's WebKit, as Safari uses it, in a browser of lib/app's Cocoa backend, the same browser
// as the viewer's, so that tools/browsers/browsers.py can compare the engine with it and the probes and stills can
// capture it. The application is headless: no window of it is seen and it never takes focus.
//
// It loads the URL that it is given, over a data store that keeps nothing, so that no profile, tab or extension of the
// developer's Safari runs beside it. It runs until it is stopped. It prints the processes that WebKit runs the page in
// (`web <pid>`, `gpu <pid>` and `network <pid>`, for those that WebKit names), one per line as each starts, so that the
// runner counts their time on cores with the host's own. WebKit launches them as services, not as its children.
//
// With --screenshot, it instead writes the page as it is displayed to a PNG once the page has loaded and its fonts are
// ready, and exits. --size sets the viewport in CSS pixels, 1024x768 by default. --pixel-ratio sets the pixels per CSS
// pixel that WebKit renders at, in place of the display's, and the screenshot has that many pixels per CSS pixel.
//
// With --pdf, it instead prints the page to a PDF once the page has loaded and its fonts are ready, and exits. The
// paper has the size that the document's `@page` rule gives, read from its style sheets, or US Letter if no rule gives
// one, with no margins of the paper's own.
//
// --appearance dark shows the page for a user who prefers a dark color scheme, and --appearance light for one who
// prefers a light one, which is the default.
//
// Usage: webkit_host [--screenshot <png> | --pdf <pdf>] [--size WxH] [--pixel-ratio <pixels per px>]
//                    [--appearance light|dark] <url>

#include "pr/app.h"
#include "pr/app_cocoa.h"
#include "pr/assert.h"
#include "pr/string_ops.h"

#include "tools/harness/harness_png.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

// How often the host looks for the processes that WebKit has started.
constexpr std::chrono::milliseconds kProcessPoll{5};

// Waits for the page's fonts, which load after the load event, and for two frames after them. Resolves to the device
// pixels per CSS pixel, and the size that the first `@page` rule without a selector gives in CSS px, or null.
constexpr const char* kReady = "(async () => {  await document.fonts.ready;"
    "  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));"
    "  for (const sheet of document.styleSheets)    for (const rule of sheet.cssRules)"
    "      if (rule instanceof CSSPageRule && rule.selectorText === '') {"
    "        const size = /^([\\d.]+)px ([\\d.]+)px$/.exec(rule.style.getPropertyValue('size'));"
    "        if (size) return [devicePixelRatio, parseFloat(size[1]), parseFloat(size[2])];      }"
    "  return [devicePixelRatio, 0, 0];})()";

struct Host {
    PrApp* app = nullptr;
    PrAppBrowser* browser = nullptr;
    std::string url;
    std::string screenshot;         // where to write the page, or empty
    std::string pdf;                // where to print the page, or empty. With neither, the host runs until stopped
    PrUVec2 size{1024, 768};
    float pixelRatio = 0;
    PrAppUserPreferences preferences{};
    int status = 0;

    // The process last printed for each role, and the thread that wakes the application to look again. WebKit may move
    // the page to another content process as its navigation commits, so the host looks until the page has loaded.
    int32_t named[3] = {};
    std::atomic<bool> polling{true};
    std::thread poller;
};

void stop(Host& h, int status) {
    h.status = status;
    prAppStop(h.app);
}

// Prints each process that WebKit runs the page in and that has not been printed for its role.
void nameProcesses(Host& h) {
    PrAppBrowserProcess processes[3];
    uint32_t count = prAppBrowserProcesses(h.browser, processes, 3);
    static const char* const kRoles[] = {"web", "gpu", "network"};
    for (uint32_t i = 0; i < count && i < 3; ++i) {
        PrAppBrowserProcessRole role = processes[i].role;
        if (role >= 3 || h.named[role] == processes[i].id)
            continue;
        h.named[role] = processes[i].id;
        std::printf("%s %d\n", kRoles[role], processes[i].id);
        std::fflush(stdout);
    }
}

void captured(void* context, PrStatus status, PrPixelView pixels) {
    Host& h = *static_cast<Host*>(context);
    bool ok = status == PrStatus_Ok;
    if (ok) {
        std::vector<uint8_t> rgba(size_t(pixels.extent.x) * pixels.extent.y * 4);
        harnessStraightRgba(pixels, rgba.data());
        ok =
            harnessWritePng(h.screenshot.c_str(), PrPixelView{rgba.data(), size_t(pixels.extent.x) * 4, pixels.extent});
    }
    if (!ok)
        std::fprintf(stderr, "webkit_host: no screenshot of %s\n", h.url.c_str());
    stop(h, ok ? 0 : 1);
}

void printed(void* context, PrStatus status) {
    Host& h = *static_cast<Host*>(context);
    if (status != PrStatus_Ok)
        std::fprintf(stderr, "webkit_host: the print of %s did not save\n", h.url.c_str());
    stop(h, status == PrStatus_Ok ? 0 : 1);
}

void ready(void* context, PrStatus status, PrStringView json) {
    Host& h = *static_cast<Host*>(context);
    std::string text(json.data != nullptr ? json.data : "", json.size);
    double ratio = 0;
    double paperWidth = 0;
    double paperHeight = 0;
    if (status != PrStatus_Ok || std::sscanf(text.c_str(), "[%lf,%lf,%lf]", &ratio, &paperWidth, &paperHeight) != 3 ||
            !(ratio > 0)) {
        std::fprintf(stderr, "webkit_host: %s did not become ready: %s\n", h.url.c_str(), text.c_str());
        stop(h, 1);
        return;
    }
    if (!h.pdf.empty()) {
        PrVec2 paper{float(paperWidth), float(paperHeight)};
        if (prAppBrowserPrint(h.browser, PrStringView{h.pdf.data(), h.pdf.size()}, paper, printed, &h) != PrStatus_Ok) {
            std::fprintf(stderr, "webkit_host: this browser does not print\n");
            stop(h, 1);
        }
        return;
    }
    PrUVec2 pixels{uint32_t(double(h.size.x) * ratio + 0.5), uint32_t(double(h.size.y) * ratio + 0.5)};
    prAppBrowserCapture(h.browser, PrRect{0, 0, float(h.size.x), float(h.size.y)}, pixels, captured, &h);
}

void loaded(void* context, PrStatus status) {
    Host& h = *static_cast<Host*>(context);
    nameProcesses(h);
    h.polling = false;
    if (h.screenshot.empty() && h.pdf.empty())
        return;
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "webkit_host: %s did not load\n", h.url.c_str());
        stop(h, 1);
        return;
    }
    prAppBrowserEvaluate(h.browser, kReady, ready, &h);
}

int usage() {
    std::fprintf(stderr, "usage: webkit_host [--screenshot <png> | --pdf <pdf>] [--size WxH] "
        "[--pixel-ratio <pixels per px>] [--appearance light|dark] <url>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    Host h;
    int i = 1;
    for (; i + 1 < argc; i += 2) {
        const char* value = argv[i + 1];
        if (std::strcmp(argv[i], "--screenshot") == 0) {
            h.screenshot = value;
        } else if (std::strcmp(argv[i], "--pdf") == 0) {
            h.pdf = value;
        } else if (std::strcmp(argv[i], "--pixel-ratio") == 0) {
            if (std::sscanf(value, "%f", &h.pixelRatio) != 1 || !(h.pixelRatio > 0))
                return usage();
        } else if (std::strcmp(argv[i], "--appearance") == 0) {
            if (std::strcmp(value, "dark") != 0 && std::strcmp(value, "light") != 0)
                return usage();
            h.preferences.colorScheme =
                std::strcmp(value, "dark") == 0 ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
        } else if (std::strcmp(argv[i], "--size") != 0 ||
                std::sscanf(value, "%ux%u", &h.size.x, &h.size.y) != 2 || h.size.x == 0 || h.size.y == 0) {
            return usage();
        }
    }
    if (i != argc - 1 || (!h.screenshot.empty() && !h.pdf.empty()))
        return usage();
    h.url = argv[argc - 1];
    PrAppDesc appDesc{.title = "WebKit host", .extent = {64, 64}, .headless = true};
    h.app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &h,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Host& h = *static_cast<Host*>(context);
            PrAppBrowserDesc desc{.viewport = h.size, .pixelRatio = h.pixelRatio, .preferences = &h.preferences};
            if (prAppBrowserCreate(app, &desc, &h.browser) != PrStatus_Ok) {
                std::fprintf(stderr, "webkit_host: no browser\n");
                stop(h, 1);
                return;
            }
            // The processes start once the page begins to load, so the host looks for them until it has them all.
            h.poller = std::thread([&h, app] {
                while (h.polling) {
                    std::this_thread::sleep_for(kProcessPoll);
                    prAppWake(app);
                }
            });
            prAppBrowserLoad(h.browser, PrStringView{h.url.data(), h.url.size()}, loaded, &h);
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void* context, PrApp*, const PrAppEvent* event) {
            Host& h = *static_cast<Host*>(context);
            if (event->kind == PrAppEventKind_Wake && h.browser != nullptr && h.polling)
                nameProcesses(h);
        },
        [](void* context, PrApp*) {
            Host& h = *static_cast<Host*>(context);
            h.polling = false;
            if (h.poller.joinable())
                h.poller.join();
            prAppBrowserDestroy(h.browser);
            h.browser = nullptr;
        },
    };
    prAppRun(h.app, &handler);
    prAppDestroy(h.app);
    return h.status;
}
