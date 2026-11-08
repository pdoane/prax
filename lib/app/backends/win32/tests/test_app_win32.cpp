#include "pr/app_win32.h"
#include "pr/array.h"
#include "pr/file.h"
#include "pr/function.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pr_impl {

namespace {

// A thread that runs a function, which a test starts from a callback and joins once the run ends. It must not move
// while the thread runs.
struct Helper {
    PrThread thread{};
    Function<void()> run;

    void start(Function<void()> fn) {
        run = move(fn);
        prThreadStart(&thread, [](void* PR_NULLABLE context) { static_cast<Helper*>(context)->run(); }, this);
    }

    void join() { prThreadJoin(&thread); }
};

struct Run {
    Array<String> seen;
    Helper helper;
    uint32_t frames = 0;
    PrVec2 resized{};
    PrVec2 expected{};
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

HWND windowOf(PrApp* app) {
    PrAppSurface surface{};
    REQUIRE(prAppSurface(app, &surface) == PrStatus_Ok);
    REQUIRE(surface.kind == PrAppSurfaceKind_Hwnd);
    return static_cast<HWND>(surface.handle);
}

// A copy of an event, which outlives its delivery.
struct Seen {
    PrAppEventKind kind;
    PrAppKey key;
    PrAppKeyCode code;
    bool repeat;
    uint8_t button;
    PrAppScrollUnit scrollUnit;
    PrVec2 position;
    PrVec2 delta;
    String text;
};

struct Input {
    PrApp* app = nullptr;
    Array<Seen> events;
    Helper sender;
    UINT dpi = 96;
};

// The lParam of a key message with `scan`, in set 1, and the flags of WM_KEYDOWN's documentation.
LPARAM keyFlags(UINT scan, bool extended, bool repeat, bool up) {
    LPARAM flags = 1 | LPARAM(scan) << 16;
    if (extended)
        flags |= LPARAM(1) << 24;
    if (repeat || up)
        flags |= LPARAM(1) << 30;
    if (up)
        flags |= LPARAM(1) << 31;
    return flags;
}

LPARAM pointAt(int x, int y) {
    return LPARAM(uint32_t(uint16_t(x)) | uint32_t(uint16_t(y)) << 16);
}

// A browser's run: what it loaded, what its scripts answered and what it captured.
struct Browse {
    PrApp* app = nullptr;
    PrAppBrowser* browser = nullptr;
    PrAppBrowserDesc desc{.viewport = PrUVec2{40, 20}};
    void (*begin)(Browse& b) = nullptr;
    bool supported = true;
    PrStatus loaded = PrStatus_InvalidState;
    PrStatus status = PrStatus_InvalidState;
    PrStatus thrown = PrStatus_InvalidState;
    bool null = false;              // the last script's result was the null view
    String result;
    String message;
    uint32_t width = 0;
    uint32_t height = 0;
    Array<uint8_t> center;    // BGRA
    uint32_t processes = 0;
    bool stale = false;             // a destroyed browser called back
};

Browse& browseOf(void* context) {
    return *static_cast<Browse*>(context);
}

// Runs a headless application that creates a browser as `b.desc` describes and calls `b.begin` with it. A step stops
// the application to end the run. Without a WebView2 runtime, the run ends at once.
void runBrowser(Browse& b) {
    PrAppDesc appDesc{.title = "Browser test", .extent = {64, 32}, .headless = true};
    b.app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &b,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Browse& b = browseOf(context);
            PrStatus created = prAppBrowserCreate(app, &b.desc, &b.browser);
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
            prAppBrowserDestroy(b.browser);
            b.browser = nullptr;
        },
    };
    prAppRun(b.app, &handler);
    prAppDestroy(b.app);
    if (!b.supported)
        MESSAGE("no WebView2 runtime; skipped");
}

// The path of `name` in the binary's folder (harness::testFolder).
String tempPath(PrStringView name) {
    return format("{}\\{}", harness::testFolder(), name);
}

// Writes `contents` to `name` in the temporary folder (tempPath), and returns its file URL.
String fileUrl(PrStringView name, PrStringView contents) {
    REQUIRE(prFileWrite(tempPath(name), prStringBytes(contents)));
    return format("file:///{}/{}", harness::testFolder(), name);
}

// Evaluates `script`, records its answer, and stops the application.
void answerAndStop(Browse& b, const char* script) {
    prAppBrowserEvaluate(b.browser, script, [](void* context, PrStatus status, PrStringView json) {
        Browse& b = browseOf(context);
        b.status = status;
        b.null = json.data == nullptr;
        b.result.assign(json.isNull() ? PrStringView("") : json);
        prAppStop(b.app);
    }, &b);
}

// What a run saw: its events in order, and whether one arrived while the start callback ran.
struct Nesting {
    Array<Seen> events;
    bool starting = false;
    bool nested = false;
    Helper sender;
};

Nesting& nestingOf(void* context) {
    return *static_cast<Nesting*>(context);
}

Seen seenOf(const PrAppEvent* event) {
    Seen seen{};
    seen.kind = event->kind;
    seen.text = String(prAppEventText(event));
    prAppEventPosition(event, &seen.position);
    switch (event->kind) {
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
            seen.key = event->keyboard.key;
            seen.code = event->keyboard.code;
            seen.repeat = event->keyboard.repeat;
            break;
        case PrAppEventKind_Scroll:
            seen.scrollUnit = event->scroll.unit;
            seen.delta = event->scroll.delta;
            break;
        case PrAppEventKind_PointerMove:
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
            seen.button = event->pointer.button;
            break;
        default:
            break;
    }
    return seen;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a headless run starts, frames, wakes from another thread and shuts down") {
    Run run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            runOf(context).seen.push("start");
            HWND window = windowOf(app);
            CHECK(IsWindow(window));
            CHECK(!IsWindowVisible(window));
            RECT client;
            REQUIRE(GetClientRect(window, &client));
            UINT dpi = GetDpiForWindow(window);
            CHECK(client.right == MulDiv(64, int(dpi), 96));
            CHECK(client.bottom == MulDiv(32, int(dpi), 96));
        },
        [](void* context, PrApp* app, const PrAppFrame* frame) {
            Run& r = runOf(context);
            if (r.frames++ != 0)
                return PrAppOutput{};
            r.seen.push("frame");
            CHECK(frame->extent.x == doctest::Approx(64.0f).epsilon(0.02));
            CHECK(frame->extent.y == doctest::Approx(32.0f).epsilon(0.02));
            CHECK(frame->delta == 0.0);
            CHECK(frame->time > 0.0);
            CHECK(frame->display.pixelRatio >= 1.0f);
            CHECK(frame->display.headroom >= 1.0f);
            r.helper.start([app] { prAppWake(app); });
            return PrAppOutput{};
        },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            if (event->kind != PrAppEventKind_Wake)
                return;
            runOf(context).seen.push("wake");
            prAppStop(app);
        },
        [](void* context, PrApp* app) {
            runOf(context).seen.push("shutdown");
            PrAppSurface surface{};
            CHECK(prAppSurface(app, &surface) == PrStatus_Ok);
        },
    };
    prAppRun(app, &handler);
    run.helper.join();
    PrAppSurface surface{};
    CHECK(prAppSurface(app, &surface) == PrStatus_InvalidState);
    prAppDestroy(app);
    CHECK(run.seen == Array<String>({"start", "frame", "wake", "shutdown"}));
}

TEST_CASE("a resize runs a frame at the new extent without a request") {
    Run run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void* context, PrApp* app, const PrAppFrame* frame) {
            Run& r = runOf(context);
            if (r.frames++ == 0) {
                // Another thread resizes the window, which the window's thread handles once this frame returns.
                HWND window = windowOf(app);
                UINT dpi = GetDpiForWindow(window);
                RECT outer{0, 0, MulDiv(100, int(dpi), 96), MulDiv(50, int(dpi), 96)};
                DWORD style = DWORD(GetWindowLongW(window, GWL_STYLE));
                AdjustWindowRectExForDpi(&outer, style, FALSE, 0, dpi);
                r.expected = PrVec2{
                    float(MulDiv(100, int(dpi), 96)) * 96.0f / float(dpi),
                    float(MulDiv(50, int(dpi), 96)) * 96.0f / float(dpi),
                };
                r.helper.start([window, outer] {
                    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                });
                return PrAppOutput{};
            }
            r.resized = frame->extent;
            CHECK(frame->delta >= 0.0);
            prAppStop(app);
            return PrAppOutput{};
        },
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    run.helper.join();
    prAppDestroy(app);
    CHECK(run.frames == 2);
    CHECK(run.resized.x == doctest::Approx(run.expected.x));
    CHECK(run.resized.y == doctest::Approx(run.expected.y));
}

TEST_CASE("keys, text, pointers and wheels translate to events") {
    Input input;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    input.app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &input,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Input& in = *static_cast<Input*>(context);
            // No key is down and no lock is on, whatever the keyboard's state.
            BYTE state[256] = {};
            SetKeyboardState(state);
            HWND window = windowOf(app);
            in.dpi = GetDpiForWindow(window);
            int dpi = int(in.dpi);
            POINT screen{MulDiv(30, dpi, 96), MulDiv(16, dpi, 96)};
            ClientToScreen(window, &screen);
            // Messages sent from another thread reach the window procedure in order, past TranslateMessage, so each
            // character is sent as WM_CHAR would deliver it.
            in.sender.start([app, window, dpi, screen] {
                SendMessageW(window, WM_KEYDOWN, 'A', keyFlags(0x1E, false, false, false));
                SendMessageW(window, WM_CHAR, 'a', keyFlags(0x1E, false, false, false));
                SendMessageW(window, WM_KEYDOWN, 'A', keyFlags(0x1E, false, true, false));
                SendMessageW(window, WM_KEYUP, 'A', keyFlags(0x1E, false, false, true));
                SendMessageW(window, WM_KEYDOWN, VK_LEFT, keyFlags(0x4B, true, false, false));
                SendMessageW(window, WM_KEYDOWN, VK_RETURN, keyFlags(0x1C, false, false, false));
                SendMessageW(window, WM_CHAR, '\r', keyFlags(0x1C, false, false, false));
                SendMessageW(window, WM_KEYDOWN, VK_SHIFT, keyFlags(0x36, false, false, false));
                SendMessageW(window, WM_CHAR, 0xD83D, 0);
                SendMessageW(window, WM_CHAR, 0xDE00, 0);
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(MulDiv(10, dpi, 96), MulDiv(6, dpi, 96)));
                SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, pointAt(MulDiv(20, dpi, 96), MulDiv(12, dpi, 96)));
                SendMessageW(window, WM_LBUTTONUP, 0, pointAt(MulDiv(20, dpi, 96), MulDiv(12, dpi, 96)));
                SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, pointAt(0, 0));
                SendMessageW(window, WM_RBUTTONUP, 0, pointAt(0, 0));
                SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, uint16_t(-WHEEL_DELTA)), pointAt(screen.x, screen.y));
                SendMessageW(window, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), pointAt(screen.x, screen.y));
                prAppWake(app);
            });
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Input& in = *static_cast<Input*>(context);
            if (event->kind == PrAppEventKind_Wake) {
                prAppStop(app);
                return;
            }
            in.events.push(seenOf(event));
        },
        [](void*, PrApp*) {},
    };
    prAppRun(input.app, &handler);
    input.sender.join();
    prAppDestroy(input.app);

    const Array<Seen>& e = input.events;
    REQUIRE(e.count() == 15);
    CHECK(e[0].kind == PrAppEventKind_KeyDown);
    CHECK(e[0].key == 'A');
    CHECK(e[0].code == PrAppKeyCode_KeyA);
    CHECK(e[0].text == "a");
    CHECK(!e[0].repeat);
    CHECK(e[1].kind == PrAppEventKind_Text);
    CHECK(e[1].text == "a");
    CHECK(e[2].kind == PrAppEventKind_KeyDown);
    CHECK(e[2].repeat);
    CHECK(e[3].kind == PrAppEventKind_KeyUp);
    CHECK(e[3].key == 'A');
    CHECK(e[3].text == "a");
    CHECK(e[4].kind == PrAppEventKind_KeyDown);
    CHECK(e[4].key == PrAppKey_Left);
    CHECK(e[4].code == PrAppKeyCode_ArrowLeft);
    CHECK(e[4].text.isEmpty());
    // Enter types a carriage return, which is no Text.
    CHECK(e[5].key == PrAppKey_Enter);
    CHECK(e[5].code == PrAppKeyCode_Enter);
    CHECK(e[6].kind == PrAppEventKind_KeyDown);
    CHECK(e[6].key == PrAppKey_Shift);
    CHECK(e[6].code == PrAppKeyCode_ShiftRight);
    // A character beyond the BMP arrives as two surrogates, and is one Text.
    CHECK(e[7].kind == PrAppEventKind_Text);
    CHECK(e[7].text == "\xF0\x9F\x98\x80");
    CHECK(e[8].kind == PrAppEventKind_PointerDown);
    CHECK(e[8].button == 0);
    CHECK(e[8].position.x == doctest::Approx(10.0f).epsilon(0.05));
    CHECK(e[8].position.y == doctest::Approx(6.0f).epsilon(0.1));
    CHECK(e[9].kind == PrAppEventKind_PointerMove);
    CHECK(e[9].position.x == doctest::Approx(20.0f).epsilon(0.05));
    CHECK(e[10].kind == PrAppEventKind_PointerUp);
    CHECK(e[11].kind == PrAppEventKind_PointerDown);
    CHECK(e[11].button == 1);
    CHECK(e[12].kind == PrAppEventKind_PointerUp);
    CHECK(e[12].button == 1);
    // WHEEL_PAGESCROLL, which winuser.h spells as UINT_MAX from <limits.h>, which this test does not include.
    UINT const pageScroll = UINT32_MAX;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    UINT chars = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    // A notch toward the user scrolls down, and half a notch to the right scrolls half as far right.
    CHECK(e[13].kind == PrAppEventKind_Scroll);
    CHECK(e[13].scrollUnit == PrAppScrollUnit_Line);
    CHECK(e[13].delta.x == 0.0f);
    if (lines != pageScroll)
        CHECK(e[13].delta.y == doctest::Approx(float(lines)));
    CHECK(e[13].position.x == doctest::Approx(30.0f).epsilon(0.05));
    CHECK(e[14].kind == PrAppEventKind_Scroll);
    CHECK(e[14].delta.y == 0.0f);
    if (chars != pageScroll)
        CHECK(e[14].delta.x == doctest::Approx(float(chars) / 2));
}

TEST_CASE("input that a callback's own message loop dispatches arrives after the callback, in order") {
    Nesting run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Nesting& n = nestingOf(context);
            BYTE state[256] = {};
            SetKeyboardState(state);
            HWND window = windowOf(app);
            // A system call that the callback makes may run a message loop of its own, as this one does.
            n.starting = true;
            PostMessageW(window, WM_KEYDOWN, 'A', keyFlags(0x1E, false, false, false));
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(4, 4));
            PostMessageW(window, WM_LBUTTONUP, 0, pointAt(4, 4));
            prAppWake(app);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            n.starting = false;
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Nesting& n = nestingOf(context);
            n.nested = n.nested || n.starting;
            n.events.push(seenOf(event));
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(!run.nested);
    const Array<Seen>& e = run.events;
    REQUIRE(e.count() == 4);
    CHECK(e[0].kind == PrAppEventKind_KeyDown);
    CHECK(e[0].text == "a");
    CHECK(e[1].kind == PrAppEventKind_PointerDown);
    CHECK(e[2].kind == PrAppEventKind_PointerUp);
    CHECK(e[3].kind == PrAppEventKind_Wake);
}

TEST_CASE("a window that loses the pointer's capture releases the buttons that are down") {
    Nesting run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Nesting& n = nestingOf(context);
            HWND window = windowOf(app);
            n.sender.start([app, window] {
                SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, pointAt(6, 8));
                prAppWake(app);
            });
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Nesting& n = nestingOf(context);
            n.events.push(seenOf(event));
            // Another window takes the capture, as a system dialog would, and the button's release goes to it.
            if (event->kind == PrAppEventKind_PointerDown)
                ReleaseCapture();
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    run.sender.join();
    prAppDestroy(app);
    const Array<Seen>& e = run.events;
    REQUIRE(e.count() == 3);
    CHECK(e[0].kind == PrAppEventKind_PointerDown);
    CHECK(e[1].kind == PrAppEventKind_PointerUp);
    CHECK(e[1].button == 1);
    CHECK(e[1].position.x == e[0].position.x);
    CHECK(e[1].position.y == e[0].position.y);
    CHECK(e[2].kind == PrAppEventKind_Wake);
}

TEST_CASE("a window that loses the keyboard releases the keys that are down, once") {
    Nesting run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            Nesting& n = nestingOf(context);
            BYTE state[256] = {};
            SetKeyboardState(state);
            HWND window = windowOf(app);
            n.sender.start([app, window] {
                SendMessageW(window, WM_KEYDOWN, VK_SHIFT, keyFlags(0x2A, false, false, false));
                SendMessageW(window, WM_KEYDOWN, VK_SHIFT, keyFlags(0x36, false, false, false));
                SendMessageW(window, WM_KEYUP, VK_SHIFT, keyFlags(0x2A, false, false, true));
                SendMessageW(window, WM_KILLFOCUS, 0, 0);
                SendMessageW(window, WM_KILLFOCUS, 0, 0);
                prAppWake(app);
            });
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Nesting& n = nestingOf(context);
            n.events.push(seenOf(event));
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    run.sender.join();
    prAppDestroy(app);
    const Array<Seen>& e = run.events;
    // The left Shift was released by the keyboard, and the right Shift by the loss of focus.
    REQUIRE(e.count() == 5);
    CHECK(e[2].kind == PrAppEventKind_KeyUp);
    CHECK(e[2].code == PrAppKeyCode_ShiftLeft);
    CHECK(e[3].kind == PrAppEventKind_KeyUp);
    CHECK(e[3].code == PrAppKeyCode_ShiftRight);
    CHECK(e[3].key == PrAppKey_Shift);
    CHECK(e[4].kind == PrAppEventKind_Wake);
}

TEST_CASE("closing the window stops the run") {
    Run run;
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    PrAppHandler handler{
        &run,
        [](void* context, PrApp* app, const PrAppFrame*) {
            runOf(context).seen.push("start");
            PostMessageW(windowOf(app), WM_CLOSE, 0, 0);
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) { runOf(context).seen.push("shutdown"); },
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(run.seen == Array<String>({"start", "shutdown"}));
}

TEST_CASE("text that is not a URL does not open") {
    PrAppDesc appDesc{.title = "Win32 test", .extent = {64, 32}, .headless = true};
    PrApp* app = prAppWin32Create(&appDesc);
    CHECK(prAppOpenUrl(app, "not a url") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "C:\\Windows\\notepad.exe") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "https://example.com/a b") == PrStatus_Syntax);
    prAppDestroy(app);
}

TEST_CASE("a browser loads a page, runs a script in it and captures it at two pixels per CSS pixel") {
    Browse browse;
    browse.desc.pixelRatio = 2;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
            "<rect width='40' height='20' fill='#ff0000'/></svg>");
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            b.processes = prAppBrowserProcesses(b.browser, nullptr, 0);
            prAppBrowserEvaluate(b.browser, "document.documentElement.getAttribute('width')",
                [](void* context, PrStatus status, PrStringView json) {
                    Browse& b = browseOf(context);
                    b.status = status;
                    b.result.assign(json.isNull() ? PrStringView("") : json);
                    prAppBrowserCapture(b.browser, PrRect{0, 0, 40, 20}, PrUVec2{80, 40},
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
    runBrowser(browse);
    if (!browse.supported)
        return;
    CHECK(browse.loaded == PrStatus_Ok);
    CHECK(browse.status == PrStatus_Ok);
    CHECK(browse.result == "\"40\"");
    CHECK(browse.processes > 0);
    CHECK(browse.width == 80);
    CHECK(browse.height == 40);
    REQUIRE(browse.center.count() == 4);
    CHECK(browse.center[2] > 240);      // red
    CHECK(browse.center[1] < 16);
    CHECK(browse.center[0] < 16);
    CHECK(browse.center[3] == 255);
}

TEST_CASE("a page from a file reads the files beside it, and a promise answers once it settles") {
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser-read.html", "<!doctype html><title>read</title>");
        fileUrl("pr-browser-read.txt", "beside");
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            answerAndStop(b, "var request = new XMLHttpRequest(); request.open('GET', 'pr-browser-read.txt', false);"
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
        prAppBrowserEvaluate(b.browser, "throw new Error('no such thing')",
            [](void* context, PrStatus status, PrStringView json) {
                Browse& b = browseOf(context);
                b.thrown = status;
                b.message.assign(json.isNull() ? PrStringView("") : json);
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

TEST_CASE("a browser renders at the pixel ratio that it is created with, and a capture scales to the size asked for") {
    Browse browse;
    browse.desc.pixelRatio = 3;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser-ratio.html", "<!doctype html><style>body { margin: 0; background: #0000ff; }</style>");
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prAppBrowserCapture(b.browser, PrRect{10, 5, 30, 15}, PrUVec2{10, 5},
                [](void* context, PrStatus status, PrPixelView pixels) {
                    Browse& b = browseOf(context);
                    if (status == PrStatus_Ok) {
                        b.width = pixels.extent.x;
                        b.height = pixels.extent.y;
                        const uint8_t* at = pixels.data + pixels.stride * 2 + 5 * 4;
                        b.center.clear();
                        b.center.append(at, 4);
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
    REQUIRE(browse.center.count() == 4);
    CHECK(browse.center[0] > 240);      // blue
    CHECK(browse.center[2] < 16);
}

TEST_CASE("a browser prints its page to a PDF") {
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser-print.html", "<!doctype html><p>printed");
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            String path = tempPath("pr-browser-print.pdf");
            prFileRemove(path);
            PrStatus printing = prAppBrowserPrint(b.browser, path, PrVec2{384, 288}, [](void* context, PrStatus status) {
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
    Array<uint8_t> pdf = harness::readFile(tempPath("pr-browser-print.pdf"));
    REQUIRE(pdf.count() >= 4);
    CHECK(PrStringView(reinterpret_cast<const char*>(pdf.data()), 4) == "%PDF");
}

TEST_CASE("a capture shows the viewport where the page has scrolled to") {
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser-scroll.html", "<!doctype html><style>body { margin: 0; } div { height: 50px; }</style>"
            "<div style='background: #ff0000'></div><div style='background: #0000ff'></div>");
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prAppBrowserEvaluate(b.browser, "scrollTo(0, 50); scrollY", [](void* context, PrStatus, PrStringView json) {
                Browse& b = browseOf(context);
                b.result.assign(json.isNull() ? PrStringView("") : json);
                prAppBrowserCapture(b.browser, PrRect{0, 0, 40, 20}, PrUVec2{40, 20},
                    [](void* context, PrStatus status, PrPixelView pixels) {
                        Browse& b = browseOf(context);
                        if (status == PrStatus_Ok) {
                            const uint8_t* at = pixels.data + pixels.stride * 10 + 20 * 4;
                            b.center.clear();
                            b.center.append(at, 4);
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
    REQUIRE(browse.center.count() == 4);
    CHECK(browse.center[0] > 240);      // blue
    CHECK(browse.center[2] < 16);
}

TEST_CASE("a page shown in the window keeps its viewport, captures as it does hidden, and hides again") {
    Browse browse;
    browse.begin = [](Browse& b) {
        String url = fileUrl("pr-browser-shown.html", "<!doctype html><style>body { margin: 0; background: #00ff00; }</style>");
        prAppBrowserShow(b.browser, PrRect{8, 4, 48, 24}, PrRect{0, 0, 80, 40});
        prAppBrowserLoad(b.browser, url, [](void* context, PrStatus status) {
            Browse& b = browseOf(context);
            b.loaded = status;
            prAppBrowserEvaluate(b.browser, "[innerWidth, innerHeight]", [](void* context, PrStatus, PrStringView json) {
                Browse& b = browseOf(context);
                b.message.assign(json.isNull() ? PrStringView("") : json);
                prAppBrowserCapture(b.browser, PrRect{0, 0, 40, 20}, PrUVec2{40, 20},
                    [](void* context, PrStatus status, PrPixelView pixels) {
                        Browse& b = browseOf(context);
                        if (status == PrStatus_Ok) {
                            const uint8_t* at = pixels.data + pixels.stride * 10 + 20 * 4;
                            b.center.clear();
                            b.center.append(at, 4);
                        }
                        prAppBrowserHide(b.browser);
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
    REQUIRE(browse.center.count() == 4);
    CHECK(browse.center[1] > 240);      // green
    CHECK(browse.center[0] < 16);
}

TEST_CASE("a browser destroyed while its calls are pending calls none of them") {
    Browse browse;
    browse.begin = [](Browse& b) {
        prAppBrowserLoad(b.browser, "about:blank", [](void* context, PrStatus) { browseOf(context).stale = true; }, &b);
        prAppBrowserEvaluate(b.browser, "1", [](void* context, PrStatus, PrStringView) { browseOf(context).stale = true; }, &b);
        prAppBrowserDestroy(b.browser);
        // A second browser runs while the engine finishes with the first, and ends the run once its page loads.
        PrAppBrowserDesc desc{.viewport = PrUVec2{40, 20}};
        REQUIRE(prAppBrowserCreate(b.app, &desc, &b.browser) == PrStatus_Ok);
        prAppBrowserLoad(b.browser, "about:blank", [](void* context, PrStatus status) {
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
