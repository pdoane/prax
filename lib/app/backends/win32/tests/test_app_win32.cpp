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
    bool started = false;

    void start(Function<void()> fn) {
        run = move(fn);
        started = true;
        prThreadStart(&thread, [](void* PR_NULLABLE context) { static_cast<Helper*>(context)->run(); }, this);
    }

    // Waits for the thread, if one was started.
    void join() {
        if (started)
            prThreadJoin(&thread);
        started = false;
    }
};

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
        case PrAppEventKind_PointerCancel:
            seen.button = event->pointer.button;
            break;
        default:
            break;
    }
    return seen;
}

// A run of a headless application with one window of 64 by 32 px: what the run saw, the window's events and the
// application's Wake, in order, and whether an event arrived while the start callback ran. The run calls `begin` in its
// start, after the window is made, `frame` with each of the window's frames, and `event` with each of the window's
// events after it is recorded. A Wake stops the run.
struct Run {
    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    Array<String> seen;
    Array<Seen> events;
    Helper helper;
    uint32_t frames = 0;
    bool starting = false;
    bool nested = false;
    UINT dpi = 96;
    PrVec2 resized{};
    PrVec2 expected{};
    void (*begin)(Run& run) = nullptr;
    void (*frame)(Run& run, const PrAppFrame* frame) = nullptr;

    // The window's frame, and the title bar regions that its frames return.
    PrAppWindowDecoration decoration = PrAppWindowDecoration_System;
    Array<PrAppTitleBarRegion> regions;
    void (*event)(Run& run, const PrAppEvent* event) = nullptr;

    // The drag that the window's first frame starts, or null, and the operation of each drag that ended.
    const PrAppDragDesc* drag = nullptr;
    Array<PrAppDragOperation> ended;
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

HWND windowOf(PrAppWindow* window) {
    PrAppSurface surface{};
    REQUIRE(prAppWindowSurface(window, &surface) == PrStatus_Ok);
    REQUIRE(surface.kind == PrAppSurfaceKind_Hwnd);
    return static_cast<HWND>(surface.handle);
}

void runHeadless(Run& run) {
    PrAppOptions options{.headless = true};
    run.app = prAppWin32Create(&options);
    prAppRun(run.app, {
        .context = &run,
        .start = [](void* context, PrApp* app) {
            Run& r = runOf(context);
            r.seen.push("start");
            PrAppWindowDesc desc{
                .title = "Win32 test",
                .extent = {64, 32},
                .decoration = r.decoration,
                .handler = {
                    .context = &r,
                    .open = [](void* context, PrAppWindow*, const PrAppFrame*) {
                        runOf(context).seen.push("open");
                        return true;
                    },
                    .frame = [](void* context, PrAppWindow* window, const PrAppFrame* frame) {
                        Run& r = runOf(context);
                        if (r.frame != nullptr)
                            r.frame(r, frame);
                        r.frames++;
                        if (r.frames == 1 && r.drag != nullptr)
                            prAppWindowDragStart(window, r.drag);
                        return PrAppOutput{
                            .wantsFrame = r.frames == 1 && !r.regions.isEmpty(),
                            .titleBarRegions = r.regions.data(),
                            .titleBarRegionCount = r.regions.count(),
                        };
                    },
                    .event = [](void* context, PrAppWindow*, const PrAppEvent* event) {
                        Run& r = runOf(context);
                        r.nested = r.nested || r.starting;
                        r.events.push(seenOf(event));
                        if (event->kind == PrAppEventKind_DragEnd)
                            r.ended.push(event->drag.operation);
                        if (r.event != nullptr)
                            r.event(r, event);
                    },
                },
            };
            REQUIRE(prAppWindowCreate(app, &desc, &r.window) == PrStatus_Ok);
            if (r.begin != nullptr)
                r.begin(r);
            return true;
        },
        .event = [](void* context, PrApp* app, const PrAppEvent* event) {
            Run& r = runOf(context);
            r.nested = r.nested || r.starting;
            r.events.push(seenOf(event));
            if (event->kind == PrAppEventKind_Wake) {
                r.seen.push("wake");
                prAppStop(app);
            }
        },
        .shutdown = [](void* context, PrApp*) { runOf(context).seen.push("shutdown"); },
    });
    run.helper.join();
    prAppDestroy(run.app);
}

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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a headless run starts, opens and frames a window, wakes from another thread and shuts down") {
    Run run;
    run.begin = [](Run& r) {
        HWND window = windowOf(r.window);
        CHECK(IsWindow(window));
        CHECK(!IsWindowVisible(window));
        RECT client;
        REQUIRE(GetClientRect(window, &client));
        UINT dpi = GetDpiForWindow(window);
        CHECK(client.right == MulDiv(64, int(dpi), 96));
        CHECK(client.bottom == MulDiv(32, int(dpi), 96));
    };
    run.frame = [](Run& r, const PrAppFrame* frame) {
        if (r.frames != 0)
            return;
        CHECK(frame->extent.x == doctest::Approx(64.0f).epsilon(0.02));
        CHECK(frame->extent.y == doctest::Approx(32.0f).epsilon(0.02));
        CHECK(frame->delta == 0.0);
        CHECK(frame->time > 0.0);
        CHECK(frame->display.pixelRatio >= 1.0f);
        CHECK(frame->display.headroom >= 1.0f);
        r.helper.start([app = r.app] { prAppWake(app); });
    };
    runHeadless(run);
    CHECK(run.seen == Array<String>({"start", "open", "wake", "shutdown"}));
}

TEST_CASE("a window is created only while its application runs") {
    PrAppOptions options{.headless = true};
    PrApp* app = prAppWin32Create(&options);
    PrAppWindowDesc desc{
        .extent = {64, 32},
        .handler = {
            .open = [](void*, PrAppWindow*, const PrAppFrame*) { return true; },
            .frame = [](void*, PrAppWindow*, const PrAppFrame*) { return PrAppOutput{}; },
            .event = [](void*, PrAppWindow*, const PrAppEvent*) {},
        },
    };
    PrAppWindow* window = nullptr;
    CHECK(prAppWindowCreate(app, &desc, &window) == PrStatus_InvalidState);
    CHECK(window == nullptr);
    prAppDestroy(app);
}

TEST_CASE("a resize runs a frame at the new extent without a request") {
    Run run;
    run.frame = [](Run& r, const PrAppFrame* frame) {
        if (r.frames == 0) {
            // Another thread resizes the window, which the window's thread handles once this frame returns.
            HWND window = windowOf(r.window);
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
            return;
        }
        r.resized = frame->extent;
        CHECK(frame->delta >= 0.0);
        prAppStop(r.app);
    };
    runHeadless(run);
    CHECK(run.frames == 2);
    CHECK(run.resized.x == doctest::Approx(run.expected.x));
    CHECK(run.resized.y == doctest::Approx(run.expected.y));
}

TEST_CASE("keys, text, pointers and wheels translate to events") {
    Run run;
    run.begin = [](Run& r) {
        // No key is down and no lock is on, whatever the keyboard's state.
        BYTE state[256] = {};
        SetKeyboardState(state);
        HWND window = windowOf(r.window);
        r.dpi = GetDpiForWindow(window);
        int dpi = int(r.dpi);
        POINT screen{MulDiv(30, dpi, 96), MulDiv(16, dpi, 96)};
        ClientToScreen(window, &screen);
        // Messages sent from another thread reach the window procedure in order, past TranslateMessage, so each
        // character is sent as WM_CHAR would deliver it.
        r.helper.start([app = r.app, window, dpi, screen] {
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
    };
    runHeadless(run);

    const Array<Seen>& e = run.events;
    REQUIRE(e.count() == 16);
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
    CHECK(e[15].kind == PrAppEventKind_Wake);
}

TEST_CASE("input that a callback's own message loop dispatches arrives after the callback, in order") {
    Run run;
    run.begin = [](Run& r) {
        BYTE state[256] = {};
        SetKeyboardState(state);
        HWND window = windowOf(r.window);
        // A system call that the callback makes may run a message loop of its own, as this one does.
        r.starting = true;
        PostMessageW(window, WM_KEYDOWN, 'A', keyFlags(0x1E, false, false, false));
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(4, 4));
        PostMessageW(window, WM_LBUTTONUP, 0, pointAt(4, 4));
        prAppWake(r.app);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        r.starting = false;
    };
    runHeadless(run);
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
    Run run;
    run.begin = [](Run& r) {
        HWND window = windowOf(r.window);
        r.helper.start([app = r.app, window] {
            SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, pointAt(6, 8));
            prAppWake(app);
        });
    };
    run.event = [](Run&, const PrAppEvent* event) {
        // Another window takes the capture, as a system dialog would, and the button's release goes to it.
        if (event->kind == PrAppEventKind_PointerDown)
            ReleaseCapture();
    };
    runHeadless(run);
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
    Run run;
    run.begin = [](Run& r) {
        BYTE state[256] = {};
        SetKeyboardState(state);
        HWND window = windowOf(r.window);
        r.helper.start([app = r.app, window] {
            SendMessageW(window, WM_KEYDOWN, VK_SHIFT, keyFlags(0x2A, false, false, false));
            SendMessageW(window, WM_KEYDOWN, VK_SHIFT, keyFlags(0x36, false, false, false));
            SendMessageW(window, WM_KEYUP, VK_SHIFT, keyFlags(0x2A, false, false, true));
            SendMessageW(window, WM_KILLFOCUS, 0, 0);
            SendMessageW(window, WM_KILLFOCUS, 0, 0);
            prAppWake(app);
        });
    };
    runHeadless(run);
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

TEST_CASE("closing a window asks its program, which closes it or stops") {
    Run run;
    run.begin = [](Run& r) { PostMessageW(windowOf(r.window), WM_CLOSE, 0, 0); };
    run.event = [](Run& r, const PrAppEvent* event) {
        if (event->kind != PrAppEventKind_CloseRequested)
            return;
        CHECK(IsWindow(windowOf(r.window)));
        prAppStop(r.app);
    };
    runHeadless(run);
    REQUIRE(!run.events.isEmpty());
    CHECK(run.events.last().kind == PrAppEventKind_CloseRequested);
    CHECK(run.seen.last() == "shutdown");
}

TEST_CASE("a window destroyed during a callback receives none of the events that waited for it") {
    Run run;
    run.begin = [](Run& r) {
        HWND window = windowOf(r.window);
        // The start callback is running, so the press waits for it to return.
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(4, 4));
        prAppWindowDestroy(r.window);
        r.window = nullptr;
        CHECK(!IsWindow(window));
        prAppWake(r.app);
    };
    runHeadless(run);
    REQUIRE(run.events.count() == 1);
    CHECK(run.events[0].kind == PrAppEventKind_Wake);
}

TEST_CASE("a clipboard read answers after the callback that started it, and a cancelled read never answers") {
    Run run;
    run.begin = [](Run& r) {
        prAppDataReadText(r.app, kPrAppDataSourceClipboard, [](void* context, PrStatus status, PrStringView) {
            CHECK(status == PrStatus_Ok);
            runOf(context).seen.push("read");
        }, &r);
        static int cancelled = 0;
        prAppDataReadImage(r.app, kPrAppDataSourceClipboard, [](void*, PrStatus, PrPixelView) { FAIL("a cancelled read answered"); },
            &cancelled);
        prAppCancel(r.app, &cancelled);
        r.seen.push("started");
        // The wake comes after the read's answer, since the message window receives both in order.
        prAppWake(r.app);
    };
    runHeadless(run);
    Array<String> order;
    for (const String& call : run.seen) {
        if (call != "open")
            order.push(call);
    }
    CHECK(order == Array<String>({"start", "started", "read", "wake", "shutdown"}));
}

TEST_CASE("the displays have distinct ids, the main display comes first, and a headless window has no position") {
    PrAppOptions options{.headless = true};
    PrApp* app = prAppWin32Create(&options);
    uint32_t count = prAppDisplays(app, nullptr, 0);
    REQUIRE(count != 0);
    Array<PrAppDisplayInfo> displays(count);
    CHECK(prAppDisplays(app, displays.data(), count) == count);
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(displays[i].id != kPrAppDisplayNone);
        CHECK(displays[i].main == (i == 0));
        CHECK(displays[i].display.pixelRatio > 0);
        CHECK(displays[i].workArea.left >= 0);
        CHECK(displays[i].workArea.top >= 0);
        CHECK(displays[i].workArea.right <= displays[i].extent.x + 0.5f);
        CHECK(displays[i].workArea.bottom <= displays[i].extent.y + 0.5f);
        for (uint32_t j = 0; j < i; ++j)
            CHECK(displays[j].id != displays[i].id);
    }
    prAppDestroy(app);
    Run run;
    run.begin = [](Run& r) {
        PrAppDisplayPosition position{};
        CHECK_FALSE(prAppWindowPosition(r.window, &position));
        CHECK_FALSE(prAppWindowPointToDisplay(r.window, PrVec2{1, 2}, &position));
        PrVec2 point{};
        CHECK_FALSE(prAppWindowPointFromDisplay(r.window, &position, &point));
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("a window's extent stays within its limits, and a window of one extent cannot be resized") {
    Run run;
    run.begin = [](Run& r) {
        HWND window = windowOf(r.window);
        auto client = [&] {
            RECT rect{};
            GetClientRect(window, &rect);
            float scale = 96.0f / float(GetDpiForWindow(window));
            return PrVec2{float(rect.right) * scale, float(rect.bottom) * scale};
        };
        CHECK((GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME) != 0);
        prAppWindowSetExtentLimits(r.window, PrUVec2{80, 40}, PrUVec2{0, 0});
        CHECK(client().x == doctest::Approx(80).epsilon(0.02));
        CHECK(client().y == doctest::Approx(40).epsilon(0.02));
        prAppWindowSetExtent(r.window, PrUVec2{50, 300});
        CHECK(client().x == doctest::Approx(80).epsilon(0.02));
        CHECK(client().y == doctest::Approx(300).epsilon(0.02));
        prAppWindowSetExtentLimits(r.window, PrUVec2{100, 100}, PrUVec2{100, 100});
        CHECK(client().x == doctest::Approx(100).epsilon(0.02));
        CHECK(client().y == doctest::Approx(100).epsilon(0.02));
        CHECK((GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME) == 0);
        // Without limits, the window can be resized again.
        prAppWindowSetExtentLimits(r.window, PrUVec2{0, 0}, PrUVec2{0, 0});
        CHECK((GetWindowLongPtrW(window, GWL_STYLE) & WS_THICKFRAME) != 0);
        prAppWindowSetExtent(r.window, PrUVec2{64, 32});
        // A headless window takes no state but Normal.
        prAppWindowSetState(r.window, PrAppWindowState_Maximized);
        CHECK(prAppWindowState(r.window) == PrAppWindowState_Normal);
        prAppWake(r.app);
    };
    runHeadless(run);
}

TEST_CASE("a custom title bar is the program's to draw, and its regions answer the system's hit tests") {
    Run run;
    run.decoration = PrAppWindowDecoration_Custom;
    const PrAppTitleBarRegion regions[] = {
        {.rect = {0, 0, 64, 20}, .kind = PrAppTitleBarRegionKind_Drag},
        {.rect = {44, 0, 64, 20}, .kind = PrAppTitleBarRegionKind_Close},
    };
    run.regions = Array<PrAppTitleBarRegion>(regions);
    run.frame = [](Run& r, const PrAppFrame* frame) {
        CHECK(frame->titleBar.height > 0);
        CHECK(frame->titleBar.flags == PrAppTitleBarFlag_ProgramButtons);
        // The second frame finds the regions of the first.
        if (r.frames != 1)
            return;
        HWND window = windowOf(r.window);
        auto hitAt = [&](int x, int y) {
            float scale = float(GetDpiForWindow(window)) / 96.0f;
            POINT at{int(float(x) * scale), int(float(y) * scale)};
            ClientToScreen(window, &at);
            return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(at.x, at.y));
        };
        CHECK(hitAt(20, 12) == HTCAPTION);
        CHECK(hitAt(54, 12) == HTCLOSE);
        CHECK(hitAt(20, 28) == HTCLIENT);
        prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(run.frames >= 2);
}

TEST_CASE("a drag that a frame starts without the primary button down ends at once with no operation") {
    Run run;
    PrAppDragDesc desc{
        .content = {.forms = PrAppDataFormFlag_Text, .text = "dragged"},
        .operations = PrAppDragOperationFlag_Copy,
    };
    run.drag = &desc;
    run.event = [](Run& r, const PrAppEvent* event) {
        if (event->kind == PrAppEventKind_DragEnd)
            prAppWake(r.app);
    };
    runHeadless(run);
    CHECK(run.ended == Array<PrAppDragOperation>({PrAppDragOperation_None}));
}

TEST_CASE("text that is not a URL does not open") {
    PrAppOptions options{.headless = true};
    PrApp* app = prAppWin32Create(&options);
    CHECK(prAppOpenUrl(app, "not a url") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "C:\\Windows\\notepad.exe") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "") == PrStatus_Syntax);
    CHECK(prAppOpenUrl(app, "https://example.com/a b") == PrStatus_Syntax);
    prAppDestroy(app);
}
