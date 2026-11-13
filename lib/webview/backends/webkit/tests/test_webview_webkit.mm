#include "pr/app.h"
#include "pr/app_cocoa.h"
#include "pr/apple_string.h"
#include "pr/array.h"
#include "pr/string.h"
#include "pr/webview_webkit.h"

#include "tools/harness/test.h"

#import <AppKit/AppKit.h>

namespace pr_impl {

namespace {

// A view's run: what it loaded, what its scripts answered and what it captured.
struct Browse {
    PrApp* app = nullptr;
    PrWebView* view = nullptr;
    PrStatus loaded = PrStatus_NotFound;
    String result;
    uint32_t width = 0;
    uint32_t height = 0;
    Array<uint8_t> center;    // BGRA
    void (*begin)(Browse& b) = nullptr;
};

Browse& browseOf(void* context) {
    return *static_cast<Browse*>(context);
}

// Returns the file URL of a file in the temporary directory that holds `text`.
String fileUrl(NSString* name, NSString* text) {
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
    return String::adopt(prAppleNSStringToRawString([NSURL fileURLWithPath:path].absoluteString));
}

// Runs a headless application that creates a view and calls `b.begin` with it. A step stops the application.
void runBrowse(Browse& b) {
    PrAppDesc appDesc{.title = "WebKit test", .extent = {64, 32}, .headless = true};
    b.app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &b,
        [](void* context, PrApp*, const PrAppFrame*) {
            Browse& b = browseOf(context);
            PrWebViewDesc desc{.viewport = PrUVec2{40, 20}};
            REQUIRE(prWebViewWebKitCreate(&desc, &b.view) == PrStatus_Ok);
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
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a view loads a page, runs a script in it and captures it at two pixels per CSS pixel") {
    // A capture waits for the screen's frames, which stop while the display is off.
    if (CGDisplayIsAsleep(CGMainDisplayID()))
        return;
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl(@"pr-webview.svg", @"<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
            @"<rect width='40' height='20' fill='#ff0000'/></svg>");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prWebViewEvaluate(b.view, "document.documentElement.getAttribute('width')",
                [](void* context, PrStatus, PrStringView json) {
                    Browse& b = browseOf(context);
                    b.result.assign(json.isNull() ? PrStringView("") : json);
                    prWebViewCapture(b.view, PrRect{0, 0, 40, 20}, PrUVec2{80, 40},
                        [](void* context, PrStatus status, PrPixelView pixels) {
                            Browse& b = browseOf(context);
                            if (status == PrStatus_Ok) {
                                b.width = pixels.extent.x;
                                b.height = pixels.extent.y;
                                const uint8_t* at = pixels.data + pixels.stride * 20 + 40 * 4;
                                b.center.clear();
                                b.center.append(at, 4);
                            }
                            prAppStop(b.app);
                        }, &b);
                }, &b);
        }, &b);
    };
    runBrowse(browse);
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"40\"");
    CHECK(browse.width == 80);
    CHECK(browse.height == 40);
    REQUIRE(browse.center.count() == 4);
    CHECK(browse.center[2] > 240);      // red
    CHECK(browse.center[1] < 16);
    CHECK(browse.center[0] < 16);
}

TEST_CASE("a page from a file reads the files beside it, and a promise answers once it settles") {
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl(@"pr-webview-read.html", @"<!doctype html><title>read</title>");
        fileUrl(@"pr-webview-read.txt", @"beside");
        prWebViewLoad(b.view, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prWebViewEvaluate(b.view, "var request = new XMLHttpRequest(); request.open('GET', 'pr-webview-read.txt', false);"
                " request.send(); new Promise(resolve => setTimeout(() => resolve(request.responseText), 10))",
                [](void* context, PrStatus, PrStringView json) {
                    Browse& b = browseOf(context);
                    b.result.assign(json.isNull() ? PrStringView("") : json);
                    prAppStop(b.app);
                }, &b);
        }, &b);
    };
    runBrowse(browse);
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"beside\"");
}

TEST_CASE("a page shows over a window's view, and moves back to its own window when hidden") {
    Browse browse;
    browse.begin = [](Browse& b) {
        PrAppSurface surface{};
        REQUIRE(prAppSurface(b.app, &surface) == PrStatus_Ok);
        NSView* host = (__bridge NSView*)surface.handle;
        prWebViewShow(b.view, &surface, PrRect{0, 0, 32, 16}, PrRect{0, 0, 32, 16});
        b.width = uint32_t(host.subviews.count);
        prWebViewHide(b.view);
        b.height = uint32_t(host.subviews.count);
        prAppStop(b.app);
    };
    runBrowse(browse);
    CHECK(browse.width == 1);
    CHECK(browse.height == 0);
}
