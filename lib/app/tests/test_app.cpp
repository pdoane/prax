#include "pr/app.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestAppCApi(void);

namespace pr_impl {

namespace {

// An application that runs by calling each handler function once, and records the calls made to it.
struct FakeApp {
    PrApp record;
    std::vector<std::string> calls;
    double frameTime;
};

// A fake application that also records the notifications posted to it and reports whether its window is active.
struct FakeNotifier {
    FakeApp fake;
    bool active;
    std::vector<std::string> posted;
};

FakeNotifier& notifierOf(PrApp* app) {
    return *reinterpret_cast<FakeNotifier*>(app);
}

FakeApp& fakeOf(PrApp* app) {
    return *reinterpret_cast<FakeApp*>(app);
}

void createFake(FakeApp& fake) {
    fake.record.destroy = [](PrApp* app) { fakeOf(app).calls.push_back("destroy"); };
    fake.record.run = [](PrApp* app, const PrAppHandler* handler) {
        fakeOf(app).calls.push_back("run");
        PrAppFrame frame{.extent = {100, 50}, .display = {.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1}};
        handler->start(handler->context, app, &frame);
        prAppDeliverFrame(app, handler, &frame);
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyDown;
        event.keyboard.key = 'A';
        handler->event(handler->context, app, &event);
        handler->shutdown(handler->context, app);
    };
    fake.record.stop = [](PrApp* app) { fakeOf(app).calls.push_back("stop"); };
    fake.record.wake = [](PrApp* app) { fakeOf(app).calls.push_back("wake"); };
    fake.record.requestFrame = [](PrApp* app) { fakeOf(app).calls.push_back("frame"); };
    fake.record.requestFrameAt = [](PrApp* app, double time) { fakeOf(app).frameTime = time; };
    fake.record.setCursor = [](PrApp* app, PrAppCursor) { fakeOf(app).calls.push_back("cursor"); };
    fake.record.setTitle = [](PrApp* app, PrStringView) { fakeOf(app).calls.push_back("title"); };
    fake.record.requestAttention = [](PrApp* app) { fakeOf(app).calls.push_back("attention"); };
    fake.record.surface = [](PrApp*, PrAppSurface*) -> PrStatus { return PrStatus_InvalidState; };
    fake.record.openUrl = [](PrApp*, PrStringView) -> PrStatus { return PrStatus_Ok; };
    fake.record.requestClipboardText = [](PrApp* app) { fakeOf(app).calls.push_back("clipboard"); };
    fake.record.setClipboardText = [](PrApp*, PrStringView) -> PrStatus { return PrStatus_Ok; };
    fake.record.requestClipboardImage = [](PrApp* app) { fakeOf(app).calls.push_back("clipboard image"); };
    fake.record.setClipboardImage = [](PrApp*, PrPixelView) -> PrStatus { return PrStatus_Ok; };
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
            return PrAppOutput{};
        },
        [](void* context, PrApp*, const PrAppEvent* event) {
            std::vector<std::string>& seen = *static_cast<std::vector<std::string>*>(context);
            seen.push_back("key " + std::string(1, char(event->keyboard.key)));
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
    prAppSetTitle(app, "Title");
    prAppRequestAttention(app);
    prAppRequestClipboardText(app);
    prAppRequestClipboardImage(app);
    prAppStop(app);
    PrAppSurface surface{};
    CHECK(prAppSurface(app, &surface) == PrStatus_InvalidState);
    prAppDestroy(app);
    prAppDestroy(nullptr);
    CHECK(fake.calls == std::vector<std::string>{
        "frame", "wake", "title", "attention", "clipboard", "clipboard image", "stop", "destroy",
    });
}

TEST_CASE("an application without notifications posts none, and one that cannot tell its window is active") {
    FakeApp fake{};
    createFake(fake);
    PrAppNotificationDesc desc{.title = "Title", .body = "Body", .tag = 7};
    CHECK(prAppNotificationPost(&fake.record, &desc) == PrStatus_NotSupported);
    CHECK(prAppIsActive(&fake.record));
}

TEST_CASE("a notification reaches the backend, and the backend says whether the window is active") {
    FakeNotifier notifier{};
    createFake(notifier.fake);
    PrApp* app = &notifier.fake.record;
    app->isActive = [](PrApp* app) { return notifierOf(app).active; };
    app->notificationPost = [](PrApp* app, const PrAppNotificationDesc* desc) -> PrStatus {
        notifierOf(app).posted.push_back(std::string(desc->title.data, desc->title.size) + "|" +
            std::string(desc->body.data, desc->body.size) + "|" + std::to_string(desc->tag));
        return PrStatus_Ok;
    };
    CHECK_FALSE(prAppIsActive(app));
    notifier.active = true;
    CHECK(prAppIsActive(app));
    PrAppNotificationDesc desc{.title = "Build", .body = "done", .tag = 42};
    CHECK(prAppNotificationPost(app, &desc) == PrStatus_Ok);
    PrAppNotificationDesc untitled{.body = "only a body"};
    CHECK(prAppNotificationPost(app, &untitled) == PrStatus_Ok);
    CHECK(notifier.posted == std::vector<std::string>{"Build|done|42", "|only a body|0"});
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
    PrAppEvent pixels{.kind = PrAppEventKind_Scroll, .scroll = {.delta = PrVec2{3, -4}}};
    PrVec2 resolved = prAppEventScrollPixels(&pixels, 20);
    CHECK(resolved.x == 3.0f);
    CHECK(resolved.y == -4.0f);
    PrAppEvent lines{.kind = PrAppEventKind_Scroll, .scroll = {.delta = PrVec2{1, -2}, .unit = PrAppScrollUnit_Line}};
    resolved = prAppEventScrollPixels(&lines, 20);
    CHECK(resolved.x == 20.0f);
    CHECK(resolved.y == -40.0f);
}

TEST_CASE("a frame's output reaches the window") {
    FakeApp fake{};
    createFake(fake);
    PrAppHandler handler{};
    handler.frame = [](void*, PrApp*, const PrAppFrame*) {
        return PrAppOutput{.cursor = PrAppCursor_Hand, .wantsFrame = true};
    };
    PrAppFrame frame{};
    prAppDeliverFrame(&fake.record, &handler, &frame);
    CHECK(fake.calls == std::vector<std::string>{"cursor", "frame"});
}

TEST_CASE("a frame's time reaches the window, requests no frame now, and the next frame's output replaces it") {
    FakeApp fake{};
    createFake(fake);
    PrAppHandler handler{};
    handler.frame = [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{.frameTime = 12.5}; };
    PrAppFrame frame{};
    prAppDeliverFrame(&fake.record, &handler, &frame);
    CHECK(fake.calls == std::vector<std::string>{"cursor"});
    CHECK(fake.frameTime == 12.5);
    handler.frame = [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; };
    prAppDeliverFrame(&fake.record, &handler, &frame);
    CHECK(fake.frameTime == 0);
}

TEST_CASE("two surfaces' outputs take the earlier frame time") {
    PrAppOutput none{};
    PrAppOutput early{.frameTime = 1};
    PrAppOutput late{.frameTime = 2};
    CHECK(prAppOutputMerge(early, late, false).frameTime == 1);
    CHECK(prAppOutputMerge(late, early, false).frameTime == 1);
    CHECK(prAppOutputMerge(none, late, false).frameTime == 2);
    CHECK(prAppOutputMerge(late, none, false).frameTime == 2);
    CHECK(prAppOutputMerge(none, none, false).frameTime == 0);
}

TEST_CASE("a surface's output is placed in the window, and two surfaces' outputs merge") {
    PrAppOutput page{.cursor = PrAppCursor_Hand, .hasTextInput = true, .textInput = {1, 2, 3, 4}};
    PrAppOutput placed = prAppOutputPlaced(page, PrVec2{10, 20}, 2);
    CHECK(placed.textInput.left == 12);
    CHECK(placed.textInput.top == 24);
    CHECK(placed.textInput.right == 16);
    CHECK(placed.textInput.bottom == 28);

    PrAppOutput chrome{.cursor = PrAppCursor_Arrow, .wantsFrame = true};
    PrAppOutput overPage = prAppOutputMerge(chrome, placed, true);
    CHECK(overPage.cursor == PrAppCursor_Hand);
    CHECK(overPage.wantsFrame);
    CHECK(overPage.hasTextInput);
    CHECK(overPage.textInput.left == 12);
    CHECK(prAppOutputMerge(chrome, placed, false).cursor == PrAppCursor_Arrow);
    CHECK(!prAppOutputMerge(chrome, PrAppOutput{}, false).hasTextInput);
}
