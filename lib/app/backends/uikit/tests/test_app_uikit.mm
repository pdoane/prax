#include "pr/app_uikit.h"
#include "pr/array.h"
#include "pr/string.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

namespace pr_impl {

namespace {

// A run's calls, and the window it made.
struct Run {
    Array<String> seen;
    PrThread waker{};
    bool waking = false;    // the waker was started
    uint32_t frames = 0;
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrAppWindowModel windowModel = PrAppWindowModel_Single;
    void (*begin)(Run& run) = nullptr;
    PrAppFrame last{};
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

// A window's handler whose first frame wakes the application from another thread, which stops the run.
PrAppWindowHandler handlerOf(Run& run) {
    return PrAppWindowHandler{
        .context = &run,
        .open = [](void* context, PrAppWindow*, const PrAppFrame* frame) {
            Run& r = runOf(context);
            r.seen.push("open");
            CHECK(frame->extent.x == 64.0f);
            CHECK(frame->delta == 0.0);
            return true;
        },
        .frame = [](void* context, PrAppWindow*, const PrAppFrame* frame) {
            Run& r = runOf(context);
            r.last = *frame;
            if (r.frames++ == 0) {
                r.waking = true;
                prThreadStart(&r.waker, [](void* PR_NULLABLE app) { prAppWake(static_cast<PrApp*>(app)); }, r.app);
            }
            return PrAppOutput{};
        },
        .event = [](void*, PrAppWindow*, const PrAppEvent*) {},
    };
}

// Runs a headless application that creates a window of 64 by 32 and calls `run.begin` in its start. The application
// stops on a Wake event.
void runHeadless(Run& run) {
    PrAppOptions options{.headless = true, .windowModel = run.windowModel};
    run.app = prAppUiKitCreate(&options);
    prAppRun(run.app, {
        .context = &run,
        .start = [](void* context, PrApp* app) {
            Run& r = runOf(context);
            r.seen.push("start");
            PrAppWindowDesc desc{.title = "iOS test", .extent = {64, 32}, .handler = handlerOf(r)};
            REQUIRE(prAppWindowCreate(app, &desc, &r.window) == PrStatus_Ok);
            if (r.begin != nullptr)
                r.begin(r);
            return true;
        },
        .event = [](void* context, PrApp* app, const PrAppEvent* event) {
            Run& r = runOf(context);
            if (event->kind != PrAppEventKind_Wake)
                return;
            r.seen.push("wake");
            prAppStop(app);
        },
        .shutdown = [](void* context, PrApp*) { runOf(context).seen.push("shutdown"); },
    });
    if (run.waking)
        prThreadJoin(&run.waker);
    prAppDestroy(run.app);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a headless run starts, opens and frames a window, wakes from another thread and shuts down") {
    Run run;
    run.begin = [](Run& r) {
        // The window opens before its first frame, after the start returns.
        CHECK(r.seen == Array<String>({"start"}));
        CHECK(prAppWindowTitle(r.window) == "iOS test");
    };
    runHeadless(run);
    CHECK(run.seen == Array<String>({"start", "open", "wake", "shutdown"}));
    CHECK(run.frames >= 1);
    CHECK(run.last.extent.y == 32.0f);
    CHECK(run.last.display.pixelRatio >= 1.0f);
}

TEST_CASE("a window's surface is a view whose layer is a Metal layer") {
    Run run;
    run.begin = [](Run& r) {
        PrSurface surface{};
        REQUIRE(prAppWindowSurface(r.window, &surface) == PrStatus_Ok);
        CHECK(surface.kind == PrSurfaceKind_UiView);
        UIView* view = (__bridge UIView*)surface.handle;
        CHECK([view.layer isKindOfClass:CAMetalLayer.class]);
        CHECK(view.bounds.size.width == 64);
    };
    runHeadless(run);
}

TEST_CASE("a headless Single application refuses a second window, and a Desktop one creates it but no popup") {
    Run single;
    single.begin = [](Run& r) {
        CHECK(prAppWindowModel(r.app) == PrAppWindowModel_Single);
        PrAppWindowDesc desc{.extent = {32, 32}, .handler = handlerOf(r)};
        PrAppWindow* second = nullptr;
        CHECK(prAppWindowCreate(r.app, &desc, &second) == PrStatus_NotSupported);
        CHECK(second == nullptr);
    };
    runHeadless(single);
    Run multiple;
    multiple.windowModel = PrAppWindowModel_Desktop;
    multiple.begin = [](Run& r) {
        // A Desktop application has Multiple windows, which have no popups.
        CHECK(prAppWindowModel(r.app) == PrAppWindowModel_Multiple);
        PrAppWindowDesc desc{.extent = {32, 32}, .handler = handlerOf(r)};
        PrAppWindow* second = nullptr;
        CHECK(prAppWindowCreate(r.app, &desc, &second) == PrStatus_Ok);
        PrAppWindowDesc popup{
            .extent = {32, 32},
            .role = PrAppWindowRole_Popup,
            .parent = r.window,
            .handler = handlerOf(r),
        };
        PrAppWindow* third = nullptr;
        CHECK(prAppWindowCreate(r.app, &popup, &third) == PrStatus_NotSupported);
        CHECK(third == nullptr);
        prAppWindowDestroy(second);
    };
    runHeadless(multiple);
}

TEST_CASE("a window is created only while its application runs, and a headless one fills the display") {
    PrAppOptions options{.headless = true};
    PrApp* app = prAppUiKitCreate(&options);
    Run run;
    PrAppWindowDesc desc{.extent = {64, 32}, .handler = handlerOf(run)};
    PrAppWindow* window = nullptr;
    CHECK(prAppWindowCreate(app, &desc, &window) == PrStatus_InvalidState);
    CHECK(window == nullptr);
    prAppDestroy(app);

    Run headless;
    headless.begin = [](Run& r) {
        PrAppDisplayPosition position{};
        CHECK_FALSE(prAppWindowPosition(r.window, &position));
        CHECK(prAppWindowState(r.window) == PrAppWindowState_Fullscreen);
    };
    runHeadless(headless);
}
