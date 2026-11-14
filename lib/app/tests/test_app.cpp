#include "pr/app.h"
#include "pr/app_backend.h"
#include "pr/array.h"
#include "pr/string.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

extern "C" int prTestAppCApi(void);

namespace pr_impl {

namespace {

// A fake application's window, which records the calls made to it.
struct FakeWindow : PrAppWindow {
    Array<String>* calls;
    PrApp* app;
    bool active;
};

// An application whose run calls the handler's start, event and shutdown once each, and which records the calls made to
// it and its window.
struct FakeApp : PrApp {
    Array<String> calls;
    FakeWindow window;
    bool running;
    Array<String> posted;
};

FakeApp& fakeOf(PrApp* app) {
    return *static_cast<FakeApp*>(app);
}

FakeWindow& windowOf(PrAppWindow* window) {
    return *static_cast<FakeWindow*>(window);
}

void call(PrAppWindow* window, const char* name) {
    windowOf(window).calls->push(name);
}

void createFakeWindow(FakeApp& fake) {
    FakeWindow& w = fake.window;
    w.calls = &fake.calls;
    w.app = &fake;
    w.destroyFn = [](PrAppWindow* window) { call(window, "destroy window"); };
    w.appFn = [](const PrAppWindow* window) { return static_cast<const FakeWindow*>(window)->app; };
    w.requestFrameFn = [](PrAppWindow* window) { call(window, "frame"); };
    w.setTitleFn = [](PrAppWindow* window, PrStringView) { call(window, "title"); };
    w.titleFn = [](const PrAppWindow*) -> PrStringView { return "Fake"; };
    w.requestAttentionFn = [](PrAppWindow* window) { call(window, "attention"); };
    w.surfaceFn = [](const PrAppWindow*, PrAppSurface*) -> PrStatus { return PrStatus_InvalidState; };
}

void createFake(FakeApp& fake) {
    fake.destroyFn = [](PrApp* app) { fakeOf(app).calls.push("destroy"); };
    fake.runFn = [](PrApp* app, PrAppHandler handler) {
        FakeApp& fake = fakeOf(app);
        fake.calls.push("run");
        fake.running = true;
        if (handler.start(handler.context, app)) {
            PrAppEvent wake{.kind = PrAppEventKind_Wake};
            handler.event(handler.context, app, &wake);
        }
        handler.shutdown(handler.context, app);
        fake.running = false;
    };
    fake.stopFn = [](PrApp* app) { fakeOf(app).calls.push("stop"); };
    fake.wakeFn = [](PrApp* app) { fakeOf(app).calls.push("wake"); };
    fake.openUrlFn = [](PrApp*, PrStringView) -> PrStatus { return PrStatus_Ok; };
    // A read of text answers at once, which a backend never does, and a read of an image never answers.
    fake.dataReadFn = [](PrApp* app, const PrAppDataRead* read) {
        if (read->kind == PrAppDataReadKind_Text) {
            fakeOf(app).calls.push("read text");
            reinterpret_cast<PrAppDataTextFn>(read->done)(read->context, PrStatus_Ok, "text");
        } else if (read->kind == PrAppDataReadKind_Image) {
            fakeOf(app).calls.push("read image");
        }
    };
    fake.cancelFn = [](PrApp* app, void* PR_NULLABLE) { fakeOf(app).calls.push("cancel"); };
    fake.dataWriteFn = [](PrApp* app, PrAppDataSource, const PrAppDataContent* content) -> PrStatus {
        fakeOf(app).calls.push(format("write {}", content->text));
        return PrStatus_Ok;
    };
    fake.windowCreateFn = [](PrApp* app, const PrAppWindowDesc*, PrAppWindow** out) -> PrStatus {
        FakeApp& fake = fakeOf(app);
        *out = nullptr;
        if (!fake.running)
            return PrStatus_InvalidState;
        *out = &fake.window;
        return PrStatus_Ok;
    };
    fake.displaysFn = [](const PrApp*, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) -> uint32_t {
        if (capacity != 0)
            out[0] = PrAppDisplayInfo{.id = 7, .display = prAppDisplayDefault(), .extent = {800, 600}, .main = true};
        return 1;
    };
    fake.displayInfoFn = [](const PrApp*, PrAppDisplayId id, PrAppDisplayInfo* out) {
        if (id != 7)
            return false;
        *out = PrAppDisplayInfo{.id = 7, .display = prAppDisplayDefault(), .extent = {800, 600}, .main = true};
        return true;
    };
    createFakeWindow(fake);
}

// A window handler whose callbacks do nothing.
PrAppWindowHandler idleHandler() {
    return PrAppWindowHandler{
        .open = [](void*, PrAppWindow*, const PrAppFrame*) { return true; },
        .frame = [](void*, PrAppWindow*, const PrAppFrame*) { return PrAppOutput{}; },
        .event = [](void*, PrAppWindow*, const PrAppEvent*) {},
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestAppCApi() == 0);
}

TEST_CASE("a run passes the handler to the backend, and a window created during it belongs to the application") {
    FakeApp fake{};
    createFake(fake);
    struct Run {
        Array<String> seen;
        PrAppWindow* window = nullptr;
    } run;
    prAppRun(&fake, {
        .context = &run,
        .start = [](void* context, PrApp* app) {
            Run& r = *static_cast<Run*>(context);
            r.seen.push("start");
            PrAppWindowDesc desc{.title = "Window", .extent = {100, 50}, .handler = idleHandler()};
            CHECK(prAppWindowCreate(app, &desc, &r.window) == PrStatus_Ok);
            CHECK(prAppWindowApp(r.window) == app);
            return true;
        },
        .event = [](void* context, PrApp*, const PrAppEvent* event) {
            if (event->kind == PrAppEventKind_Wake)
                static_cast<Run*>(context)->seen.push("wake");
        },
        .shutdown = [](void* context, PrApp*) { static_cast<Run*>(context)->seen.push("shutdown"); },
    });
    CHECK(run.seen == Array<String>({"start", "wake", "shutdown"}));
    CHECK(run.window == &fake.window);
}

TEST_CASE("a window is created only while its application runs") {
    FakeApp fake{};
    createFake(fake);
    PrAppWindowDesc desc{.handler = idleHandler()};
    PrAppWindow* window = &fake.window;
    CHECK(prAppWindowCreate(&fake, &desc, &window) == PrStatus_InvalidState);
    CHECK(window == nullptr);
}

TEST_CASE("the interface passes calls through") {
    FakeApp fake{};
    createFake(fake);
    PrApp* app = &fake;
    PrAppWindow* window = &fake.window;
    prAppWindowRequestFrame(window);
    prAppWake(app);
    prAppWindowSetTitle(window, "Title");
    prAppWindowRequestAttention(window);
    prAppWindowSetExtent(window, PrUVec2{10, 10});
    prAppWindowFocus(window);
    prAppStop(app);
    CHECK(prAppWindowTitle(window) == "Fake");
    PrAppSurface surface{};
    CHECK(prAppWindowSurface(window, &surface) == PrStatus_InvalidState);
    prAppWindowDestroy(window);
    prAppWindowDestroy(nullptr);
    prAppDestroy(app);
    prAppDestroy(nullptr);
    CHECK(fake.calls == Array<String>({"frame", "wake", "title", "attention", "stop", "destroy window", "destroy"}));
}

TEST_CASE("the clipboard's reads, cancels and writes reach the backend") {
    FakeApp fake{};
    createFake(fake);
    String read;
    prAppDataReadText(&fake, kPrAppDataSourceClipboard, [](void* context, PrStatus status, PrStringView text) {
        CHECK(status == PrStatus_Ok);
        *static_cast<String*>(context) = String(text);
    }, &read);
    prAppDataReadImage(&fake, kPrAppDataSourceClipboard, [](void*, PrStatus, PrPixelView) {}, nullptr);
    prAppCancel(&fake, &read);
    CHECK(prAppDataWriteText(&fake, kPrAppDataSourceClipboard, "copied") == PrStatus_Ok);
    CHECK(read == "text");
    CHECK(fake.calls == Array<String>({"read text", "read image", "cancel", "write copied"}));
}

TEST_CASE("an enumerator's name is the enumerator without its prefix, and a flag's name is that of one flag") {
    CHECK(prAppEventKindName(PrAppEventKind_PointerMove) == "PointerMove");
    CHECK(prAppEventKindName(PrAppEventKind_Sensor) == "Sensor");
    CHECK(prAppEventKindName(PrAppEventKind(PrAppEventKind_Sensor + 1)).isNull());
    CHECK(prAppCursorName(PrAppCursor_ZoomOut) == "ZoomOut");
    CHECK(prAppWindowStateName(PrAppWindowState_Maximized) == "Maximized");
    CHECK(prAppModifierName(PrAppModifierFlag_Alt) == "Alt");
    CHECK(prAppModifierName(PrAppModifierFlag_Alt | PrAppModifierFlag_Shift).isNull());
    CHECK(prAppModifierName(0).isNull());
    CHECK(prAppDataFormName(PrAppDataFormFlag_Payload) == "Payload");
}

TEST_CASE("the displays come from the backend, and a window without positions reports none and stays Normal") {
    FakeApp fake{};
    createFake(fake);
    CHECK(prAppDisplays(&fake, nullptr, 0) == 1);
    PrAppDisplayInfo info{};
    CHECK(prAppDisplays(&fake, &info, 1) == 1);
    CHECK(info.id == 7);
    CHECK(info.main);
    PrAppDisplayInfo found{};
    CHECK(prAppDisplayInfoFromId(&fake, 7, &found));
    CHECK(found.extent.x == 800);
    CHECK_FALSE(prAppDisplayInfoFromId(&fake, 3, &found));
    PrAppDisplayPosition position{.display = 3};
    CHECK_FALSE(prAppWindowPosition(&fake.window, &position));
    CHECK(position.display == kPrAppDisplayNone);
    CHECK_FALSE(prAppWindowPointToDisplay(&fake.window, PrVec2{1, 2}, &position));
    PrVec2 point{3, 4};
    CHECK_FALSE(prAppWindowPointFromDisplay(&fake.window, &position, &point));
    CHECK(point.x == 0);
    CHECK(point.y == 0);
    prAppWindowSetPosition(&fake.window, &position);
    prAppWindowSetState(&fake.window, PrAppWindowState_Fullscreen);
    CHECK(prAppWindowState(&fake.window) == PrAppWindowState_Normal);
}

TEST_CASE("only the clipboard and the selection take writes") {
    FakeApp fake{};
    createFake(fake);
    CHECK(prAppDataWriteText(&fake, kPrAppDataSourceNone, "lost") == PrStatus_InvalidData);
    CHECK(prAppDataWriteText(&fake, kPrAppDataSourceSelection, "selected") == PrStatus_Ok);
    CHECK(fake.calls == Array<String>({"write selected"}));
}

TEST_CASE("an application without notifications posts none, and a window that cannot tell is active") {
    FakeApp fake{};
    createFake(fake);
    PrAppNotificationDesc desc{.title = "Title", .body = "Body", .tag = 7};
    CHECK(prAppNotificationPost(&fake, &desc) == PrStatus_NotSupported);
    CHECK(prAppWindowIsActive(&fake.window));
}

TEST_CASE("a notification reaches the backend, and the backend says whether the window is active") {
    FakeApp fake{};
    createFake(fake);
    PrApp* app = &fake;
    PrAppWindow* window = &fake.window;
    window->isActiveFn = [](const PrAppWindow* window) { return static_cast<const FakeWindow*>(window)->active; };
    app->notificationPostFn = [](PrApp* app, const PrAppNotificationDesc* desc) -> PrStatus {
        fakeOf(app).posted.push(format("{}|{}|{}", desc->title, desc->body, desc->tag));
        return PrStatus_Ok;
    };
    CHECK_FALSE(prAppWindowIsActive(window));
    fake.window.active = true;
    CHECK(prAppWindowIsActive(window));
    PrAppNotificationDesc desc{.title = "Build", .body = "done", .tag = 42};
    CHECK(prAppNotificationPost(app, &desc) == PrStatus_Ok);
    PrAppNotificationDesc untitled{.body = "only a body"};
    CHECK(prAppNotificationPost(app, &untitled) == PrStatus_Ok);
    CHECK(fake.posted == Array<String>({"Build|done|42", "|only a body|0"}));
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

TEST_CASE("an event's text and position are those of the member that its kind uses") {
    PrAppEvent key{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'A', .text = "a"}};
    PrAppEvent paste{.kind = PrAppEventKind_Paste, .input = {.text = "pasted"}};
    PrAppEvent press{.kind = PrAppEventKind_PointerDown, .pointer = {.position = {1, 2}, .button = 1}};
    PrAppEvent wheel{.kind = PrAppEventKind_Scroll, .scroll = {.position = {3, 4}, .delta = {0, 5}}};
    PrAppEvent wake{.kind = PrAppEventKind_Wake};
    CHECK(prAppEventText(&key) == "a");
    CHECK(prAppEventText(&paste) == "pasted");
    CHECK(prAppEventText(&press) == "");
    prAppEventSetText(&key, "b");
    prAppEventSetText(&paste, "c");
    prAppEventSetText(&press, "ignored");
    CHECK(key.keyboard.text == "b");
    CHECK(key.keyboard.key == 'A');
    CHECK(paste.input.text == "c");
    CHECK(press.pointer.button == 1);

    PrVec2 at{9, 9};
    CHECK(prAppEventPosition(&press, &at));
    CHECK(at.x == 1);
    CHECK(prAppEventPosition(&wheel, &at));
    CHECK(at.y == 4);
    CHECK_FALSE(prAppEventPosition(&wake, &at));
    CHECK(at.x == 0);
    // Moving a scroll keeps its delta.
    prAppEventSetPosition(&wheel, PrVec2{6, 7});
    prAppEventSetPosition(&key, PrVec2{6, 7});
    CHECK(wheel.scroll.position.x == 6);
    CHECK(wheel.scroll.delta.y == 5);
    CHECK(key.keyboard.key == 'A');
}

TEST_CASE("an event's source is a drop's content or an open's files, which retain and release keep") {
    PrAppEvent drop{.kind = PrAppEventKind_Drop, .drag = {.drop = 7}};
    PrAppEvent open{.kind = PrAppEventKind_Open, .open = {.source = 8}};
    PrAppEvent over{.kind = PrAppEventKind_DragOver, .drag = {.drop = 9}};
    CHECK(prAppEventSource(&drop) == 7);
    CHECK(prAppEventSource(&open) == 8);
    CHECK(prAppEventSource(&over) == kPrAppDataSourceNone);

    // The backend sees the retains and releases of the sources.
    static Array<PrAppDataSource> retained;
    static Array<PrAppDataSource> released;
    PrApp app{};
    app.dataRetainFn = [](PrApp*, PrAppDataSource source) { retained.push(source); };
    app.dataReleaseFn = [](PrApp*, PrAppDataSource source) { released.push(source); };
    prAppEventRetain(&app, &drop);
    prAppEventRetain(&app, &open);
    prAppEventRelease(&app, &drop);
    CHECK(retained == Array<PrAppDataSource>({7, 8}));
    CHECK(released == Array<PrAppDataSource>({7}));
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

TEST_CASE("a caret in another window stays as it is when its output is placed, and goes with it when outputs merge") {
    FakeApp fake{};
    createFake(fake);
    PrAppOutput page{.hasTextInput = true, .textInput = {1, 2, 3, 4}, .textInputWindow = &fake.window};
    PrAppOutput placed = prAppOutputPlaced(page, PrVec2{10, 20}, 2);
    CHECK(placed.textInput.left == 1);
    CHECK(placed.textInput.bottom == 4);
    CHECK(placed.textInputWindow == &fake.window);
    CHECK(prAppOutputMerge(PrAppOutput{}, placed, false).textInputWindow == &fake.window);
}

TEST_CASE("merged outputs take the drop operation of the surface under the pointer") {
    PrAppOutput chrome{.dropOperation = PrAppDragOperation_None};
    PrAppOutput page{.dropOperation = PrAppDragOperation_Copy};
    CHECK(prAppOutputMerge(chrome, page, true).dropOperation == PrAppDragOperation_Copy);
    CHECK(prAppOutputMerge(chrome, page, false).dropOperation == PrAppDragOperation_None);
    CHECK(prAppOutputPlaced(page, PrVec2{10, 20}, 2).dropOperation == PrAppDragOperation_Copy);
}
