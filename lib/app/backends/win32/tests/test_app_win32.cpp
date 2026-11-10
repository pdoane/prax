#include "pr/app_win32.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pr_impl {

namespace {

struct Run {
    std::vector<std::string> seen;
    std::thread helper;
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
    std::string text;
};

struct Input {
    PrApp* app = nullptr;
    std::vector<Seen> events;
    std::thread sender;
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

// What a run saw: its events in order, and whether one arrived while the start callback ran.
struct Nesting {
    std::vector<Seen> events;
    bool starting = false;
    bool nested = false;
    std::thread sender;
};

Nesting& nestingOf(void* context) {
    return *static_cast<Nesting*>(context);
}

Seen seenOf(const PrAppEvent* event) {
    Seen seen{};
    seen.kind = event->kind;
    PrStringView text = prAppEventText(event);
    seen.text = std::string(text.data != nullptr ? text.data : "", text.size);
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
            runOf(context).seen.push_back("start");
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
            r.seen.push_back("frame");
            CHECK(frame->extent.x == doctest::Approx(64.0f).epsilon(0.02));
            CHECK(frame->extent.y == doctest::Approx(32.0f).epsilon(0.02));
            CHECK(frame->delta == 0.0);
            CHECK(frame->time > 0.0);
            CHECK(frame->display.pixelRatio >= 1.0f);
            CHECK(frame->display.headroom >= 1.0f);
            r.helper = std::thread([app] { prAppWake(app); });
            return PrAppOutput{};
        },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            if (event->kind != PrAppEventKind_Wake)
                return;
            runOf(context).seen.push_back("wake");
            prAppStop(app);
        },
        [](void* context, PrApp* app) {
            runOf(context).seen.push_back("shutdown");
            PrAppSurface surface{};
            CHECK(prAppSurface(app, &surface) == PrStatus_Ok);
        },
    };
    prAppRun(app, &handler);
    run.helper.join();
    PrAppSurface surface{};
    CHECK(prAppSurface(app, &surface) == PrStatus_InvalidState);
    prAppDestroy(app);
    CHECK(run.seen == std::vector<std::string>{"start", "frame", "wake", "shutdown"});
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
                r.helper = std::thread([window, outer] {
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
            in.sender = std::thread([app, window, dpi, screen] {
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
            in.events.push_back(seenOf(event));
        },
        [](void*, PrApp*) {},
    };
    prAppRun(input.app, &handler);
    input.sender.join();
    prAppDestroy(input.app);

    const std::vector<Seen>& e = input.events;
    REQUIRE(e.size() == 15);
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
    CHECK(e[4].text.empty());
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
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    UINT chars = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    // A notch toward the user scrolls down, and half a notch to the right scrolls half as far right.
    CHECK(e[13].kind == PrAppEventKind_Scroll);
    CHECK(e[13].scrollUnit == PrAppScrollUnit_Line);
    CHECK(e[13].delta.x == 0.0f);
    if (lines != WHEEL_PAGESCROLL)
        CHECK(e[13].delta.y == doctest::Approx(float(lines)));
    CHECK(e[13].position.x == doctest::Approx(30.0f).epsilon(0.05));
    CHECK(e[14].kind == PrAppEventKind_Scroll);
    CHECK(e[14].delta.y == 0.0f);
    if (chars != WHEEL_PAGESCROLL)
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
            n.events.push_back(seenOf(event));
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(!run.nested);
    const std::vector<Seen>& e = run.events;
    REQUIRE(e.size() == 4);
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
            n.sender = std::thread([app, window] {
                SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, pointAt(6, 8));
                prAppWake(app);
            });
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void* context, PrApp* app, const PrAppEvent* event) {
            Nesting& n = nestingOf(context);
            n.events.push_back(seenOf(event));
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
    const std::vector<Seen>& e = run.events;
    REQUIRE(e.size() == 3);
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
            n.sender = std::thread([app, window] {
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
            n.events.push_back(seenOf(event));
            if (event->kind == PrAppEventKind_Wake)
                prAppStop(app);
        },
        [](void*, PrApp*) {},
    };
    prAppRun(app, &handler);
    run.sender.join();
    prAppDestroy(app);
    const std::vector<Seen>& e = run.events;
    // The left Shift was released by the keyboard, and the right Shift by the loss of focus.
    REQUIRE(e.size() == 5);
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
            runOf(context).seen.push_back("start");
            PostMessageW(windowOf(app), WM_CLOSE, 0, 0);
        },
        [](void*, PrApp*, const PrAppFrame*) { return PrAppOutput{}; },
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) { runOf(context).seen.push_back("shutdown"); },
    };
    prAppRun(app, &handler);
    prAppDestroy(app);
    CHECK(run.seen == std::vector<std::string>{"start", "shutdown"});
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
