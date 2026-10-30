#include "pr/app.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestAppCApi(void);

namespace pr_impl {

namespace {

// An application that runs by calling each handler function once, and records the calls made to it.
struct FakeApp {
    PrApp record;
    std::vector<std::string> calls;
};

FakeApp& fakeOf(PrApp* app) {
    return *reinterpret_cast<FakeApp*>(app);
}

void createFake(FakeApp& fake) {
    fake.record.destroy = [](PrApp* app) { fakeOf(app).calls.push_back("destroy"); };
    fake.record.run = [](PrApp* app, const PrAppHandler* handler) {
        fakeOf(app).calls.push_back("run");
        PrAppFrame frame{.extent = {100, 50}, .display = {.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1}};
        handler->start(handler->context, app, &frame);
        handler->frame(handler->context, app, &frame);
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyDown;
        event.key = 'A';
        handler->event(handler->context, app, &event);
        handler->shutdown(handler->context, app);
    };
    fake.record.stop = [](PrApp* app) { fakeOf(app).calls.push_back("stop"); };
    fake.record.wake = [](PrApp* app) { fakeOf(app).calls.push_back("wake"); };
    fake.record.requestFrame = [](PrApp* app) { fakeOf(app).calls.push_back("frame"); };
    fake.record.setCursor = [](PrApp* app, PrAppCursor) { fakeOf(app).calls.push_back("cursor"); };
    fake.record.surface = [](PrApp*, PrAppSurface*) -> PrStatus { return PrStatus_InvalidState; };
    fake.record.openUrl = [](PrApp*, PrStringView) -> PrStatus { return PrStatus_Ok; };
    fake.record.requestClipboardText = [](PrApp* app) { fakeOf(app).calls.push_back("clipboard"); };
    fake.record.setClipboardText = [](PrApp*, PrStringView) -> PrStatus { return PrStatus_Ok; };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestAppCApi() == 0);
}

TEST_CASE("a run delivers to the handler in order") {
    FakeApp fake{};
    createFake(fake);
    PrApp* app = &fake.record;
    std::vector<std::string> seen;
    PrAppHandler handler{
        &seen,
        [](void* context, PrApp*, const PrAppFrame*) {
            static_cast<std::vector<std::string>*>(context)->push_back("start");
        },
        [](void* context, PrApp*, const PrAppFrame* frame) {
            static_cast<std::vector<std::string>*>(context)->push_back(
                "frame " + std::to_string(int(frame->extent.x * frame->display.pixelRatio)));
        },
        [](void* context, PrApp*, const PrAppEvent* event) {
            static_cast<std::vector<std::string>*>(context)->push_back("key " + std::string(1, char(event->key)));
        },
        [](void* context, PrApp*) {
            static_cast<std::vector<std::string>*>(context)->push_back("shutdown");
        },
    };
    prAppRun(app, &handler);
    CHECK(seen == std::vector<std::string>{"start", "frame 200", "key A", "shutdown"});
}

TEST_CASE("the interface passes calls through") {
    FakeApp fake{};
    createFake(fake);
    PrApp* app = &fake.record;
    prAppRequestFrame(app);
    prAppWake(app);
    prAppSetCursor(app, PrAppCursor_Hand);
    prAppRequestClipboardText(app);
    prAppStop(app);
    PrAppSurface surface{};
    CHECK(prAppSurface(app, &surface) == PrStatus_InvalidState);
    PrAppBrowser* browser = nullptr;
    PrAppBrowserDesc desc{.viewport = PrUVec2{10, 10}};
    CHECK(prAppBrowserCreate(app, &desc, &browser) == PrStatus_NotSupported);
    CHECK(browser == nullptr);
    prAppDestroy(app);
    prAppDestroy(nullptr);
    CHECK(fake.calls == std::vector<std::string>{"frame", "wake", "cursor", "clipboard", "stop", "destroy"});
}

TEST_CASE("named keys and codes take the names of UI Events") {
    CHECK(prAppKeyName(PrAppKey_Left) == "ArrowLeft");
    CHECK(prAppKeyName(PrAppKey_Dead) == "Dead");
    CHECK(prAppKeyName(PrAppKey_F20) == "F20");
    CHECK(prAppKeyName('A') == "");
    CHECK(prAppKeyName(PrAppKey_Unknown) == "");
    CHECK(prAppKeyCodeName(PrAppKeyCode_Unidentified) == "Unidentified");
    CHECK(prAppKeyCodeName(PrAppKeyCode_KeyQ) == "KeyQ");
    CHECK(prAppKeyCodeName(PrAppKeyCode_NumpadEnter) == "NumpadEnter");
    CHECK(prAppKeyCodeName(PrAppKeyCode_Pause) == "Pause");
    CHECK(prAppKeyCodeName(PrAppKeyCode_Count) == "Unidentified");
}

TEST_CASE("a scroll resolves to pixels by its unit") {
    PrAppEvent pixels{.kind = PrAppEventKind_Scroll, .delta = PrVec2{3, -4}};
    PrVec2 resolved = prAppEventScrollPixels(&pixels, 20);
    CHECK(resolved.x == 3.0f);
    CHECK(resolved.y == -4.0f);
    PrAppEvent lines{.kind = PrAppEventKind_Scroll, .scrollUnit = PrAppScrollUnit_Line, .delta = PrVec2{1, -2}};
    resolved = prAppEventScrollPixels(&lines, 20);
    CHECK(resolved.x == 20.0f);
    CHECK(resolved.y == -40.0f);
}
