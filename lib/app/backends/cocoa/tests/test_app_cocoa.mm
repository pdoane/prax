#include "pr/app_cocoa.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#import <AppKit/AppKit.h>

namespace pr_impl {

namespace {

struct Run {
    std::vector<std::string> seen;
    std::thread waker;
    uint32_t frames = 0;
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

// A run's events in order, and whether one arrived while the start callback ran.
struct Input {
    std::vector<PrAppEvent> events;
    bool starting = false;
    bool nested = false;
};

Input& inputOf(void* context) {
    return *static_cast<Input*>(context);
}

NSView* viewOf(PrApp* app) {
    PrAppSurface surface{};
    REQUIRE(prAppSurface(app, &surface) == PrStatus_Ok);
    return (__bridge NSView*)surface.handle;
}

// A press of the A key, as AppKit would dispatch it to the window.
NSEvent* keyA(NSWindow* window, NSEventType type) {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO
        keyCode:0];
}

NSEvent* leftMouse(NSWindow* window, NSEventType type) {
    return [NSEvent mouseEventWithType:type location:NSMakePoint(10, 10) modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
}

// The events of a run, without Text, which the input method types as it will.
std::vector<PrAppEventKind> kindsOf(const std::vector<PrAppEvent>& events) {
    std::vector<PrAppEventKind> kinds;
    for (const PrAppEvent& event : events) {
        if (event.kind != PrAppEventKind_Text)
            kinds.push_back(event.kind);
    }
    return kinds;
}

struct Browse {
    PrApp* app = nullptr;
    PrAppBrowser* browser = nullptr;
    PrStatus loaded = PrStatus_NotFound;
    std::string result;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> center;    // BGRA
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a headless run starts, frames, wakes from another thread and shuts down") {
    Run run;
    PrAppDesc appDesc{.title = "Cocoa test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            runOf(context).seen.push_back("start");
            PrAppSurface surface{};
            CHECK(prAppSurface(app, &surface) == PrStatus_Ok);
            CHECK(surface.kind == PrAppSurfaceKind_NsView);
            NSView* view = (__bridge NSView*)surface.handle;
            CHECK(view.bounds.size.width == 64);
        },
        [](void* context, PrApp* app, const PrAppFrame* frame) {
            Run& r = runOf(context);
            if (r.frames++ != 0)
                return;
            r.seen.push_back("frame");
            CHECK(frame->extent.x == 64.0f);
            CHECK(frame->extent.y == 32.0f);
            CHECK(frame->delta == 0.0);
            r.waker = std::thread([app] { prAppWake(app); });
        },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            if (event->kind != PrAppEventKind_Wake)
                return;
            runOf(context).seen.push_back("wake");
            prAppStop(app);
        },
        [](void* context, PrApp*) { runOf(context).seen.push_back("shutdown"); },
    };
    prAppRun(app, &handler);
    run.waker.join();
    prAppDestroy(app);
    CHECK(run.seen == std::vector<std::string>{"start", "frame", "wake", "shutdown"});
}

TEST_CASE("a browser loads a page, runs a script in it and captures it at two pixels per CSS pixel") {
    Browse browse;
    PrAppDesc appDesc{.title = "Browser test", .extent = {64, 32}, .headless = true};
    browse.app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &browse,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Browse& b = *static_cast<Browse*>(context);
            PrAppBrowserDesc desc{.viewport = PrUVec2{40, 20}};
            REQUIRE(prAppBrowserCreate(app, &desc, &b.browser) == PrStatus_Ok);
            // Load the page from a file, which is how the viewer loads documents.
            NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"pr-browser.svg"];
            [@"<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'><rect width='40' "
                @"height='20' fill='#ff0000'/></svg>" writeToFile:path atomically:YES
                encoding:NSUTF8StringEncoding error:nil];
            NSString* url = [NSURL fileURLWithPath:path].absoluteString;
            prAppBrowserLoad(b.browser, url.UTF8String, [](void* context, PrStatus status) {
                Browse& b = *static_cast<Browse*>(context);
                b.loaded = status;
                prAppBrowserEvaluate(b.browser, "document.documentElement.getAttribute('width')",
                    [](void* context, PrStatus, PrStringView json) {
                        Browse& b = *static_cast<Browse*>(context);
                        b.result.assign(json.data != nullptr ? json.data : "", json.size);
                        prAppBrowserCapture(b.browser, PrRect{0, 0, 40, 20}, PrUVec2{80, 40},
                            [](void* context, PrStatus status, PrPixelView pixels) {
                                Browse& b = *static_cast<Browse*>(context);
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
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) {
            Browse& b = *static_cast<Browse*>(context);
            prAppBrowserDestroy(b.browser);
            b.browser = nullptr;
        },
    };
    prAppRun(browse.app, &handler);
    prAppDestroy(browse.app);
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"40\"");
    CHECK(browse.width == 80);
    CHECK(browse.height == 40);
    REQUIRE(browse.center.size() == 4);
    CHECK(browse.center[2] > 240);      // red
    CHECK(browse.center[1] < 16);
    CHECK(browse.center[0] < 16);
}

TEST_CASE("a page from a file reads the files beside it, and a promise answers once it settles") {
    Browse browse;
    PrAppDesc appDesc{.title = "Browser test", .extent = {64, 32}, .headless = true};
    browse.app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &browse,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Browse& b = *static_cast<Browse*>(context);
            PrAppBrowserDesc desc{.viewport = PrUVec2{40, 20}};
            REQUIRE(prAppBrowserCreate(app, &desc, &b.browser) == PrStatus_Ok);
            NSString* directory = NSTemporaryDirectory();
            NSString* page = [directory stringByAppendingPathComponent:@"pr-browser-read.html"];
            [@"<!doctype html><title>read</title>" writeToFile:page atomically:YES encoding:NSUTF8StringEncoding
                error:nil];
            [@"beside" writeToFile:[directory stringByAppendingPathComponent:@"pr-browser-read.txt"] atomically:YES
                encoding:NSUTF8StringEncoding error:nil];
            NSString* url = [NSURL fileURLWithPath:page].absoluteString;
            prAppBrowserLoad(b.browser, url.UTF8String, [](void* context, PrStatus status) {
                Browse& b = *static_cast<Browse*>(context);
                b.loaded = status;
                prAppBrowserEvaluate(b.browser,
                    "var request = new XMLHttpRequest(); request.open('GET', 'pr-browser-read.txt', false);"
                        " request.send(); new Promise(resolve => setTimeout(() => resolve(request.responseText),"
                        " 10))", [](void* context, PrStatus, PrStringView json) {
                            Browse& b = *static_cast<Browse*>(context);
                            b.result.assign(json.data != nullptr ? json.data : "", json.size);
                            prAppStop(b.app);
                        }, &b);
            }, &b);
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) {
            Browse& b = *static_cast<Browse*>(context);
            prAppBrowserDestroy(b.browser);
            b.browser = nullptr;
        },
    };
    prAppRun(browse.app, &handler);
    prAppDestroy(browse.app);
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.result == "\"beside\"");
}

TEST_CASE("input that a callback's own run loop dispatches arrives after the callback, in order") {
    Input input;
    PrAppDesc appDesc{.title = "Cocoa test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &input,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Input& in = inputOf(context);
            NSView* view = viewOf(app);
            // A modal panel that the callback opens runs a run loop of its own, which dispatches input and the main
            // queue's blocks, as this one does.
            in.starting = true;
            [view keyDown:keyA(view.window, NSEventTypeKeyDown)];
            [view mouseDown:leftMouse(view.window, NSEventTypeLeftMouseDown)];
            [view mouseUp:leftMouse(view.window, NSEventTypeLeftMouseUp)];
            prAppWake(app);
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            in.starting = false;
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Input& in = inputOf(context);
            in.nested = in.nested || in.starting;
            in.events.push_back(*event);
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(!input.nested);
    CHECK(kindsOf(input.events) == std::vector<PrAppEventKind>{
        PrAppEventKind_KeyDown, PrAppEventKind_PointerDown,
        PrAppEventKind_PointerUp, PrAppEventKind_Wake,
    });
}

TEST_CASE("a window that stops being the key window releases the buttons and keys that are down") {
    Input input;
    PrAppDesc appDesc{.title = "Cocoa test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &input,
        [](void*, PrApp* app, const PrAppFrame*) {
            NSView* view = viewOf(app);
            [view keyDown:keyA(view.window, NSEventTypeKeyDown)];
            [view mouseDown:leftMouse(view.window, NSEventTypeLeftMouseDown)];
            // Another window becomes the key window, and the button's release goes to it, or arrives late.
            [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification
                object:view.window];
            [view mouseUp:leftMouse(view.window, NSEventTypeLeftMouseUp)];
            prAppWake(app);
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void* context, PrApp* app, const PrAppEvent* event) {
            inputOf(context).events.push_back(*event);
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(kindsOf(input.events) == std::vector<PrAppEventKind>{
        PrAppEventKind_KeyDown, PrAppEventKind_PointerDown,
        PrAppEventKind_PointerUp, PrAppEventKind_KeyUp,
        PrAppEventKind_Wake,
    });
    for (const PrAppEvent& event : input.events) {
        if (event.kind == PrAppEventKind_KeyUp)
            CHECK(event.code == PrAppKeyCode_KeyA);
        if (event.kind == PrAppEventKind_PointerUp)
            CHECK(event.button == 0);
    }
}
