#include "pr/app.h"
#include "pr/app_win32.h"
#include "pr/webview_webview2.h"

#include "tools/harness/test.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A view's run: what it loaded, what its scripts answered and what it captured.
struct Browse {
    PrApp* app = nullptr;
    PrWebView* view = nullptr;
    PrWebViewDesc desc{.viewport = PrUVec2{40, 20}};
    void (*begin)(Browse& b) = nullptr;
    bool supported = true;
    PrStatus loaded = PrStatus_InvalidState;
    PrStatus status = PrStatus_InvalidState;
    PrStatus thrown = PrStatus_InvalidState;
    bool null = false;              // the last script's result was the null view
    std::string result;
    std::string message;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> center;    // BGRA
    uint32_t processes = 0;
    bool stale = false;             // a destroyed view called back
};

Browse& browseOf(void* context) {
    return *static_cast<Browse*>(context);
}

// Runs a headless application that creates a view as `b.desc` describes and calls `b.begin` with it. A step stops the
// application to end the run. Without a WebView2 runtime, the run ends at once.
void runBrowser(Browse& b) {
    PrAppDesc appDesc{.title = "WebView2 test", .extent = {64, 32}, .headless = true};
    b.app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &b,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Browse& b = browseOf(context);
            PrStatus created = prWebViewWebView2Create(&b.desc, &b.view);
            if (created == PrStatus_NotSupported) {
                b.supported = false;
                prAppStop(app);
                return;
            }
            REQUIRE(created == PrStatus_Ok);
            b.begin(b);
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) {
            Browse& b = browseOf(context);
            prWebViewDestroy(b.view);
            b.view = nullptr;
        },
    };
    prAppRun(b.app, &handler);
    prAppDestroy(b.app);
    if (!b.supported)
        MESSAGE("no WebView2 runtime; skipped");
}

// Writes `contents` to `name` in the temporary folder, and returns its file URL.
std::string fileUrl(const char* name, const char* contents) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    FILE* file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fputs(contents, file);
    std::fclose(file);
    return "file:///" + path.generic_string();
}

// Evaluates `script`, records its answer, and stops the application.
void answerAndStop(Browse& b, const char* script) {
    prWebViewEvaluate(b.view, script, [](void* context, PrStatus status, PrStringView json) {
        Browse& b = browseOf(context);
        b.status = status;
        b.null = json.data == nullptr;
        b.result.assign(json.data != nullptr ? json.data : "", json.size);
        prAppStop(b.app);
    }, &b);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a view loads a page, runs a script in it and captures it at two pixels per CSS pixel") {
    Browse browse;
    browse.desc.pixelRatio = 2;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
            "<rect width='40' height='20' fill='#ff0000'/></svg>");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            b.processes = prWebViewProcesses(b.view, nullptr, 0);
            prWebViewEvaluate(b.view, "document.documentElement.getAttribute('width')",
                [](void* context, PrStatus status, PrStringView json) {
                    Browse& b = browseOf(context);
                    b.status = status;
                    b.result.assign(json.data != nullptr ? json.data : "", json.size);
                    prWebViewCapture(b.view, PrRect{0, 0, 40, 20}, PrUVec2{80, 40},
                        [](void* context, PrStatus status, PrPixelView pixels) {
                            Browse& b = browseOf(context);
                            if (status == PrStatus_Ok) {
                                b.width = pixels.extent.x;
                                b.height = pixels.extent.y;
                                const uint8_t* at = pixels.data + pixels.stride * 20 + 40 * 4;
                                b.center.assign(at, at + 4);
                            }
                            prAppStop(b.app);
                        }, &b);
                }, &b);
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.status == PrStatus_Ok);
    CHECK(browse.result == "\"40\"");
    CHECK(browse.processes > 0);
    CHECK(browse.width == 80);
    CHECK(browse.height == 40);
    REQUIRE(browse.center.size() == 4);
    CHECK(browse.center[2] > 240);      // red
    CHECK(browse.center[1] < 16);
    CHECK(browse.center[0] < 16);
    CHECK(browse.center[3] == 255);
}

TEST_CASE("a page from a file reads the files beside it, and a promise answers once it settles") {
    Browse browse;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview-read.html", "<!doctype html><title>read</title>");
        fileUrl("pr-webview-read.txt", "beside");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            answerAndStop(b, "var request = new XMLHttpRequest(); request.open('GET', 'pr-view-read.txt', false);"
                " request.send(); new Promise(resolve => setTimeout(() => resolve(request.responseText), 10))");
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"beside\"");
}

TEST_CASE("a script's error gives its message, and a script with no result answers the null view") {
    Browse browse;
    browse.begin = [](Browse& b) {
        prWebViewEvaluate(b.view, "throw new Error('no such thing')", [](void* context, PrStatus status, PrStringView json) {
            Browse& b = browseOf(context);
            b.thrown = status;
            b.message.assign(json.data != nullptr ? json.data : "", json.size);
            answerAndStop(b, "var declared = 1;");
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.thrown == PrStatus_InvalidData);
    CHECK(browse.message == "no such thing");
    CHECK(browse.status == PrStatus_Ok);
    CHECK(browse.null);
}

TEST_CASE("a view renders at the pixel ratio that it is created with, and a capture scales to the size asked for") {
    Browse browse;
    browse.desc.pixelRatio = 3;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview-ratio.html", "<!doctype html><style>body { margin: 0; background: #0000ff; }</style>");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prWebViewCapture(b.view, PrRect{10, 5, 30, 15}, PrUVec2{10, 5},
                [](void* context, PrStatus status, PrPixelView pixels) {
                    Browse& b = browseOf(context);
                    if (status == PrStatus_Ok) {
                        b.width = pixels.extent.x;
                        b.height = pixels.extent.y;
                        const uint8_t* at = pixels.data + pixels.stride * 2 + 5 * 4;
                        b.center.assign(at, at + 4);
                    }
                    answerAndStop(b, "[devicePixelRatio, innerWidth, innerHeight]");
                }, &b);
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "[3,40,20]");
    CHECK(browse.width == 10);
    CHECK(browse.height == 5);
    REQUIRE(browse.center.size() == 4);
    CHECK(browse.center[0] > 240);      // blue
    CHECK(browse.center[2] < 16);
}

TEST_CASE("a view prints its page to a PDF") {
    Browse browse;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview-print.html", "<!doctype html><p>printed");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            std::string path = (std::filesystem::temp_directory_path() / "pr-webview-print.pdf").string();
            std::remove(path.c_str());
            PrStatus printing = prWebViewPrint(b.view, path, PrVec2{384, 288}, [](void* context, PrStatus status) {
                Browse& b = browseOf(context);
                b.status = status;
                prAppStop(b.app);
            }, &b);
            REQUIRE(printing == PrStatus_Ok);
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.status == PrStatus_Ok);
    std::string path = (std::filesystem::temp_directory_path() / "pr-webview-print.pdf").string();
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    char head[5] = {};
    CHECK(std::fread(head, 1, 4, file) == 4);
    std::fclose(file);
    CHECK(std::string(head) == "%PDF");
}

TEST_CASE("a capture shows the viewport where the page has scrolled to") {
    Browse browse;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview-scroll.html", "<!doctype html><style>body { margin: 0; } div { height: 50px; }</style>"
            "<div style='background: #ff0000'></div><div style='background: #0000ff'></div>");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prWebViewEvaluate(b.view, "scrollTo(0, 50); scrollY", [](void* context, PrStatus, PrStringView json) {
                Browse& b = browseOf(context);
                b.result.assign(json.data != nullptr ? json.data : "", json.size);
                prWebViewCapture(b.view, PrRect{0, 0, 40, 20}, PrUVec2{40, 20},
                    [](void* context, PrStatus status, PrPixelView pixels) {
                        Browse& b = browseOf(context);
                        if (status == PrStatus_Ok) {
                            const uint8_t* at = pixels.data + pixels.stride * 10 + 20 * 4;
                            b.center.assign(at, at + 4);
                        }
                        prAppStop(b.app);
                    }, &b);
            }, &b);
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.result == "50");
    REQUIRE(browse.center.size() == 4);
    CHECK(browse.center[0] > 240);      // blue
    CHECK(browse.center[2] < 16);
}

TEST_CASE("a page shown in the window keeps its viewport, captures as it does hidden, and hides again") {
    Browse browse;
    browse.begin = [](Browse& b) {
        std::string url = fileUrl("pr-webview-shown.html", "<!doctype html><style>body { margin: 0; background: #00ff00; }</style>");
        PrAppSurface host{};
        REQUIRE(prAppSurface(b.app, &host) == PrStatus_Ok);
        prWebViewShow(b.view, &host, PrRect{8, 4, 48, 24}, PrRect{0, 0, 80, 40});
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prWebViewEvaluate(b.view, "[innerWidth, innerHeight]", [](void* context, PrStatus, PrStringView json) {
                Browse& b = browseOf(context);
                b.message.assign(json.data != nullptr ? json.data : "", json.size);
                prWebViewCapture(b.view, PrRect{0, 0, 40, 20}, PrUVec2{40, 20},
                    [](void* context, PrStatus status, PrPixelView pixels) {
                        Browse& b = browseOf(context);
                        if (status == PrStatus_Ok) {
                            const uint8_t* at = pixels.data + pixels.stride * 10 + 20 * 4;
                            b.center.assign(at, at + 4);
                        }
                        prWebViewHide(b.view);
                        answerAndStop(b, "[innerWidth, innerHeight]");
                    }, &b);
            }, &b);
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.message == "[40,20]");
    CHECK(browse.result == "[40,20]");
    REQUIRE(browse.center.size() == 4);
    CHECK(browse.center[1] > 240);      // green
    CHECK(browse.center[0] < 16);
}

TEST_CASE("a view destroyed while its calls are pending calls none of them") {
    Browse browse;
    browse.begin = [](Browse& b) {
        prWebViewLoad(b.view, "about:blank", [](void* context, PrStatus) { browseOf(context).stale = true; }, &b);
        prWebViewEvaluate(b.view, "1", [](void* context, PrStatus, PrStringView) { browseOf(context).stale = true; }, &b);
        prWebViewDestroy(b.view);
        // A second view runs while the engine finishes with the first, and ends the run once its page loads.
        PrWebViewDesc desc{.viewport = PrUVec2{40, 20}};
        REQUIRE(prWebViewWebView2Create(&desc, &b.view) == PrStatus_Ok);
        prWebViewLoad(b.view, "about:blank", [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            answerAndStop(b, "'second'");
        }, &b);
    };
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(!browse.stale);
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"second\"");
}
