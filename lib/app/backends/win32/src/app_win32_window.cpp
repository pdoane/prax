// The Win32 backend's windows: their frames, their placement, their look, and their window procedure.

#include "app/backends/win32/src/app_win32_window.h"

#include "app/backends/win32/src/app_win32_data.h"
#include "app/backends/win32/src/app_win32_display.h"
#include "app/backends/win32/src/app_win32_input.h"
#include "app/backends/win32/src/app_win32_menu.h"
#include "app/backends/win32/src/app_win32_shell.h"

namespace pr_impl {

namespace {

// The window's style. The title bar's minimize and maximize buttons set a least width that no window message lowers, so
// a window that is never shown has none, and opens at any extent.
DWORD styleOf(const Win32App* app) {
    return app->headless ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME : WS_OVERLAPPEDWINDOW;
}

// Returns true if the window's limits allow one extent, which leaves it no resize border and no maximize button.
bool isFixed(const Win32Window* window) {
    return window->maxExtent.x != 0 && window->maxExtent.y != 0 && window->minExtent.x == window->maxExtent.x &&
        window->minExtent.y == window->maxExtent.y;
}

// The style of one window: the application's, without resizing if its extent is fixed, or a popup while it is
// fullscreen or if it is an overlay.
DWORD windowStyle(const Win32Window* window) {
    if (window->fullscreen)
        return WS_POPUP | WS_VISIBLE;
    if (window->overlay)
        return WS_POPUP | DWORD(GetWindowLongPtrW(window->window, GWL_STYLE) & WS_VISIBLE);
    DWORD style = window->style;
    if (isFixed(window))
        style &= ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return style | (GetWindowLongPtrW(window->window, GWL_STYLE) & WS_VISIBLE);
}

// Returns `extent` within the window's limits.
PrUVec2 limitedExtent(const Win32Window* window, PrUVec2 extent) {
    PrUVec2 limited = extent;
    limited.x = limited.x > window->minExtent.x ? limited.x : window->minExtent.x;
    limited.y = limited.y > window->minExtent.y ? limited.y : window->minExtent.y;
    if (window->maxExtent.x != 0 && limited.x > window->maxExtent.x)
        limited.x = window->maxExtent.x;
    if (window->maxExtent.y != 0 && limited.y > window->maxExtent.y)
        limited.y = window->maxExtent.y;
    return limited;
}

// Returns the outer size of a window whose content is `extent` logical pixels.
POINT frameSizeOf(const Win32Window* window, PrUVec2 extent, UINT dpi) {
    RECT frame{0, 0, MulDiv(int(extent.x), int(dpi), 96), MulDiv(int(extent.y), int(dpi), 96)};
    AdjustWindowRectExForDpi(&frame, windowStyle(window), FALSE, 0, dpi);
    return POINT{frame.right - frame.left, frame.bottom - frame.top};
}

// Waits until the display's next refresh. The compositor reports the time of a refresh and its period, and a waitable
// timer sleeps to the next one. This holds the pace whether or not anything presents, unlike DwmFlush, which returns at
// once when the compositor has nothing to compose, and it needs no DXGI output of the window's display, as
// IDXGIOutput::WaitForVBlank does. The compositor runs at the refresh of the primary display.
void waitForRefresh(HANDLE PR_NULLABLE timer) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    int64_t period = frequency.QuadPart / 60;
    int64_t vblank = now.QuadPart;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod != 0) {
        period = int64_t(timing.qpcRefreshPeriod);
        vblank = int64_t(timing.qpcVBlank);
    }
    // The compositor reports a refresh that may be past or more than one period ahead, and the next refresh is the
    // first on its grid after now.
    int64_t ahead = (vblank - now.QuadPart) % period;
    if (ahead <= 0)
        ahead += period;
    // A due time below zero is relative, in units of 100 ns.
    int64_t units = ahead * 10000000 / frequency.QuadPart;
    LARGE_INTEGER due;
    due.QuadPart = -(units > 1 ? units : 1);
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(DWORD(units / 10000));
}

void releaseDisplayObjects(Win32Window* window) {
    if (window->output != nullptr)
        window->output->Release();
    if (window->factory != nullptr)
        window->factory->Release();
    window->output = nullptr;
    window->factory = nullptr;
    window->monitor = nullptr;
}

// Reads the headroom and gamut of the display that shows the window again, if the window moved to another display, the
// displays changed, or the last read is older than kDisplayRefreshSeconds. DXGI describes an output as it was when its
// factory was created, so a stale factory is replaced.
void refreshDisplay(Win32Window* window, double now) {
    HMONITOR monitor = MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST);
    if (monitor == window->monitor && now - window->displayRead < kDisplayRefreshSeconds)
        return;
    window->displayRead = now;
    if (window->factory != nullptr && !window->factory->IsCurrent())
        releaseDisplayObjects(window);
    if (monitor != window->monitor) {
        if (window->output != nullptr)
            window->output->Release();
        window->output = nullptr;
        if (window->factory == nullptr &&
                FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&window->factory))))
            window->factory = nullptr;
        if (window->factory != nullptr)
            window->output = win32OutputOf(window->factory, monitor);
        window->monitor = monitor;
    }
    window->display = win32DisplayOfOutput(window->output, monitor);
}

// Reads the user's preferences, and matches the window's title bar to the color scheme. A contrast theme is a palette
// of the user's, and its scheme is the one that its window background belongs to.
void refreshPreferences(Win32Window* window) {
    PrAppUserPreferences& preferences = window->preferences;
    preferences = prAppUserPreferencesDefault();
    if (win32PersonalizeValue(L"AppsUseLightTheme", 1) == 0)
        preferences.colorScheme = PrAppColorScheme_Dark;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        preferences.contrast = PrAppContrast_More;
        COLORREF background = GetSysColor(COLOR_WINDOW);
        uint32_t luma = 2126u * GetRValue(background) + 7152u * GetGValue(background) + 722u * GetBValue(background);
        preferences.colorScheme = luma < 10000u * 128u ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
    }
    BOOL animation = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0))
        preferences.reducedMotion = !animation;
    preferences.reducedTransparency = win32PersonalizeValue(L"EnableTransparency", 1) == 0;
    // The accent is an ABGR value of the DWM's settings. Windows always has one.
    DWORD accent = 0;
    DWORD size = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
            nullptr, &accent, &size) == ERROR_SUCCESS)
        preferences.accentColor = PrColor{
            float(accent & 0xFF) / 255.0f, float((accent >> 8) & 0xFF) / 255.0f,
            float((accent >> 16) & 0xFF) / 255.0f, 1,
        };
    // The text size of the Accessibility settings, as a percentage of the default.
    DWORD textScale = 100;
    size = sizeof(textScale);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
            nullptr, &textScale, &size) == ERROR_SUCCESS && textScale >= 100)
        preferences.textScale = float(textScale) / 100.0f;
    BOOL dark = preferences.colorScheme == PrAppColorScheme_Dark;
    DwmSetWindowAttribute(window->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// Arranges for a window's frame to run. A window that is hidden or minimized is not composed, so its frames are timed
// instead of paced to the display.
void scheduleFrame(Win32Window* window) {
    if (!IsWindowVisible(window->window) || IsIconic(window->window))
        SetTimer(window->window, kTimerFrame, kHiddenFrameMilliseconds, nullptr);
    else
        SetEvent(appOf(window)->pacerRequest);
}

// The height of the system's title bar at the window's scale, in physical pixels: the caption and the resize border
// above it.
int captionHeight(const Win32Window* window) {
    return GetSystemMetricsForDpi(SM_CYCAPTION, window->dpi) + GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) +
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
}

// A custom title bar. Windows draws no buttons in it, so the program draws them, in a band as tall as the system's own
// title bar.
PrAppTitleBar titleBarOf(const Win32Window* window) {
    if (!window->customTitleBar || window->fullscreen)
        return PrAppTitleBar{};
    PrAppTitleBar titleBar{};
    titleBar.height = float(captionHeight(window)) * 96.0f / float(window->dpi);
    titleBar.flags = PrAppTitleBarFlag_ProgramButtons;
    return titleBar;
}

// The window as a frame at `now` finds it, with the display refreshed.
PrAppFrame frameAt(Win32Window* window, double now) {
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = window->framed ? now - window->lastFrame : 0;
    frame.extent = win32ExtentOf(window);
    refreshDisplay(window, now);
    frame.display = window->display;
    frame.display.pixelRatio = float(window->dpi) / 96.0f;
    frame.preferences = window->preferences;
    frame.titleBar = titleBarOf(window);
    frame.backdrop = window->shownBackdrop;
    return frame;
}

void applyOutput(Win32Window* window, const PrAppOutput& output);

void runFrame(Win32Window* window) {
    Win32App* app = appOf(window);
    // A hidden window has no frames, but for the one that it shows with.
    if (!app->running || (!window->shown && !window->showing))
        return;
    if (app->inCallback) {
        // A callback that pumps messages, as some system calls do, cannot run a frame within it, so the frame waits.
        window->frameRequested = true;
        scheduleFrame(window);
        return;
    }
    window->frameRequested = false;
    uint64_t serial = window->serial;
    double now = win32SecondsNow();
    PrAppFrame frame = frameAt(window, now);
    window->framed = true;
    window->lastFrame = now;
    app->inCallback = true;
    PrAppOutput output{};
    bool open = appWindowDeliverFrame(window->delivery, window, &frame, &output);
    if (open)
        applyOutput(window, output);
    else
        win32WindowDestroy(window);
    win32EndCallback(app);
    if (open && win32IsOpen(app, window, serial))
        win32StartDrag(window);
}

// Answers WM_NCHITTEST for a custom title bar. The system's answer keeps the resize borders at the sides and the
// bottom, and the top border, which the content covers, is answered here. The rest of the content is the program's
// title bar regions.
LRESULT customHitTest(Win32Window* window, WPARAM wParam, LPARAM lParam) {
    LRESULT hit = DefWindowProcW(window->window, WM_NCHITTEST, wParam, lParam);
    if (hit != HTCLIENT)
        return hit;
    POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
    ScreenToClient(window->window, &at);
    RECT client{};
    GetClientRect(window->window, &client);
    int border =
        GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
    if (at.y < border && !IsZoomed(window->window) && !isFixed(window)) {
        if (at.x < border)
            return HTTOPLEFT;
        return at.x >= client.right - border ? HTTOPRIGHT : HTTOP;
    }
    switch (appTitleBarRegionAt(window->titleBar, win32PositionOf(window, at.x, at.y))) {
        case PrAppTitleBarRegionKind_Drag: return HTCAPTION;
        case PrAppTitleBarRegionKind_Minimize: return HTMINBUTTON;
        case PrAppTitleBarRegionKind_Maximize: return HTMAXBUTTON;
        case PrAppTitleBarRegionKind_Close: return HTCLOSE;
        default: return HTCLIENT;
    }
}

// Acts on the release of a custom title bar's button whose press it saw. The program draws the buttons, so the system's
// handling of a press, which draws the classic buttons, is skipped.
void customButtonReleased(Win32Window* window, LRESULT button) {
    LRESULT pressed = window->pressedButton;
    window->pressedButton = HTNOWHERE;
    if (button != pressed)
        return;
    switch (button) {
        case HTMINBUTTON: ShowWindow(window->window, SW_MINIMIZE); break;
        case HTMAXBUTTON: ShowWindow(window->window, IsZoomed(window->window) ? SW_RESTORE : SW_MAXIMIZE); break;
        case HTCLOSE: PostMessageW(window->window, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

// Where placeWindow puts a window that is shown: centered on its display's work area, or where it already is.
enum {
    Placement_Center,
    Placement_Keep,
};
typedef uint8_t Placement;

// Sizes the window's client area to its extent. A window that is shown is clamped to its display's work area, and
// centered on it if `placement` is Center.
void placeWindow(Win32Window* window, Placement placement) {
    Win32App* app = appOf(window);
    window->extent = limitedExtent(window, window->extent);
    POINT size = frameSizeOf(window, window->extent, window->dpi);
    int width = size.x;
    int height = size.y;
    int x = 0;
    int y = 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (app->headless || !GetMonitorInfoW(MonitorFromWindow(window->window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        flags |= SWP_NOMOVE;
    } else {
        RECT work = monitor.rcWork;
        width = width < work.right - work.left ? width : work.right - work.left;
        height = height < work.bottom - work.top ? height : work.bottom - work.top;
        x = work.left + (work.right - work.left - width) / 2;
        y = work.top + (work.bottom - work.top - height) / 2;
        if (placement == Placement_Keep)
            flags |= SWP_NOMOVE;
    }
    SetWindowPos(window->window, nullptr, x, y, width, height, flags);
}

PrApp* windowApp(const PrAppWindow* base) {
    return static_cast<const Win32Window*>(base)->app;
}

void requestFrameAt(PrAppWindow* base, double time) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (!appOf(window)->running)
        return;
    if (time == 0) {
        KillTimer(window->window, kTimerFrameAt);
        return;
    }
    double delay = time - win32SecondsNow();
    // The timer rounds up, so that the frame finds the time reached.
    UINT milliseconds = delay > 0 ? UINT(delay * 1000) + 1 : USER_TIMER_MINIMUM;
    SetTimer(window->window, kTimerFrameAt, milliseconds, nullptr);
}

void setCursor(PrAppWindow* base, PrAppCursor cursor) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (cursor == window->shape)
        return;
    window->shape = cursor;
    HCURSOR selected = win32CursorFor(cursor);
    window->cursor = selected;
    // WM_SETCURSOR applies the shape on the pointer's next move, and a pointer that rests over the window takes it now.
    POINT point;
    RECT client;
    if (GetCursorPos(&point) && WindowFromPoint(point) == window->window && ScreenToClient(window->window, &point) &&
            GetClientRect(window->window, &client) && PtInRect(&client, point))
        SetCursor(selected);
}

void setTextInputRect(Win32Window* window, PrRect rect) {
    window->textInputRect = rect;
    win32PlaceInputMethod(window);
}

// Returns the window whose latest output holds the text being edited in `window`: the window itself if its output has
// some there, or else a window that owns it whose output names it. Returns null if no output does.
const Win32Window* PR_NULLABLE textInputSource(Win32Window* window) {
    if (window->textInputWindow == window)
        return window;
    Win32App* app = appOf(window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->textInputWindow == window)
            return app->windows.data[i];
    }
    return nullptr;
}

// Applies the text being edited in the window: where the input method opens its windows, and whether the input method
// is set aside for a password.
void applyTextInput(Win32Window* window) {
    const Win32Window* source = textInputSource(window);
    if (source != nullptr)
        setTextInputRect(window, source->textInput);
    win32SetPasswordInput(window, source != nullptr && source->textInputKind == PrAppTextInputKind_Password);
}

// Withdraws the text input that `window`, which is being destroyed, names in another window, and the text input that
// other windows name in it.
void forgetTextInput(Win32Window* window) {
    PrAppWindow* named = window->textInputWindow;
    window->textInputWindow = nullptr;
    if (named != nullptr && named != window)
        applyTextInput(static_cast<Win32Window*>(named));
    Win32App* app = appOf(window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->textInputWindow == window)
            app->windows.data[i]->textInputWindow = nullptr;
    }
}

// Applies the output of the window's frame. The text input applies to the window that the output names, which may be
// another than the one before.
void applyOutput(Win32Window* window, const PrAppOutput& output) {
    setCursor(window, output.cursor);
    PrAppWindow* before = window->textInputWindow;
    PrAppWindow* named = output.textInputWindow != nullptr ? output.textInputWindow : window;
    window->textInputWindow = output.hasTextInput ? named : nullptr;
    window->textInputKind = output.textInputKind;
    window->textInput = output.textInput;
    applyTextInput(window);
    if (before != nullptr && before != window)
        applyTextInput(static_cast<Win32Window*>(before));
    PrAppWindow* now = window->textInputWindow;
    if (now != nullptr && now != window && now != before)
        applyTextInput(static_cast<Win32Window*>(now));
    if (output.wantsFrame)
        win32RequestFrame(window);
    requestFrameAt(window, output.frameTime);
    appTitleBarRegionsSet(window->titleBar, output);
    window->dropOperation = output.dropOperation;
}

PrStringView title(const PrAppWindow* base);

void setTitle(PrAppWindow* base, PrStringView title) {
    Win32Window* window = static_cast<Win32Window*>(base);
    RawArray<wchar_t> wide;
    windowsWiden(title, wide);
    prMemoryFree(window->title);
    window->title = wide.data;
    prRawStringReset(&window->titleText);
    window->titleText = prRawStringCopy(title);
    SetWindowTextW(window->window, window->title);
    win32RequestFrame(window);
}

PrStringView title(const PrAppWindow* base) {
    return prRawStringView(&static_cast<const Win32Window*>(base)->titleText);
}

void setExtent(PrAppWindow* base, PrUVec2 extent) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->extent = extent;
    placeWindow(window, Placement_Keep);
}

void setExtentLimits(PrAppWindow* base, PrUVec2 minExtent, PrUVec2 maxExtent) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->minExtent = minExtent;
    window->maxExtent = maxExtent;
    if (window->fullscreen)
        return;
    SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window)));
    SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PrVec2 now = win32ExtentOf(window);
    PrUVec2 current{uint32_t(now.x + 0.5f), uint32_t(now.y + 0.5f)};
    PrUVec2 limited = limitedExtent(window, current);
    if (limited.x != current.x || limited.y != current.y) {
        window->extent = limited;
        placeWindow(window, Placement_Keep);
    }
}

// Moves the window's content to `position` on `monitor`, in its logical pixels, keeping the window within the monitor's
// work area.
void placeOnMonitor(Win32Window* window, const Win32Monitor& monitor, PrVec2 position) {
    POINT size = frameSizeOf(window, window->extent, monitor.dpi);
    RECT frame{
        0, 0, MulDiv(int(window->extent.x), int(monitor.dpi), 96),
        MulDiv(int(window->extent.y), int(monitor.dpi), 96),
    };
    AdjustWindowRectExForDpi(&frame, windowStyle(window), FALSE, 0, monitor.dpi);
    // The frame's left and top are negative: the content's offset into the frame.
    int x = monitor.bounds.left + MulDiv(int(position.x), int(monitor.dpi), 96) + frame.left;
    int y = monitor.bounds.top + MulDiv(int(position.y), int(monitor.dpi), 96) + frame.top;
    x = x + size.x > monitor.work.right ? monitor.work.right - size.x : x;
    y = y + size.y > monitor.work.bottom ? monitor.work.bottom - size.y : y;
    x = x < monitor.work.left ? monitor.work.left : x;
    y = y < monitor.work.top ? monitor.work.top : y;
    SetWindowPos(window->window, nullptr, x, y, size.x, size.y, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool position(const PrAppWindow* base, PrAppDisplayPosition* out);

// Centers the window's content on the work area of `monitor`.
void centerOnMonitor(Win32Window* window, const Win32Monitor& monitor) {
    PrRect work = win32LogicalOn(monitor, monitor.work);
    PrVec2 position{
        work.left + (work.right - work.left - float(window->extent.x)) / 2,
        work.top + (work.bottom - work.top - float(window->extent.y)) / 2,
    };
    placeOnMonitor(window, monitor, position);
}

bool position(const PrAppWindow* base, PrAppDisplayPosition* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    Win32Monitor monitor{};
    POINT origin{0, 0};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin) ||
            !win32MonitorOf(MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    PrRect at = win32LogicalOn(monitor, RECT{origin.x, origin.y, origin.x, origin.y});
    *out = PrAppDisplayPosition{.display = monitor.id, .position = PrVec2{at.left, at.top}};
    return true;
}

// The window's logical pixels are at its own DPI, and a display's at the display's.
bool pointToDisplay(const PrAppWindow* base, PrVec2 point, PrAppDisplayPosition* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    POINT origin{0, 0};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin))
        return false;
    float scale = float(window->dpi) / USER_DEFAULT_SCREEN_DPI;
    float x = float(origin.x) + point.x * scale;
    float y = float(origin.y) + point.y * scale;
    Win32Monitor monitor{};
    POINT at{LONG(x), LONG(y)};
    if (!win32MonitorOf(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    float toMonitor = USER_DEFAULT_SCREEN_DPI / float(monitor.dpi);
    *out = PrAppDisplayPosition{
        .display = monitor.id,
        .position = PrVec2{(x - float(monitor.bounds.left)) * toMonitor, (y - float(monitor.bounds.top)) * toMonitor},
    };
    return true;
}

bool pointFromDisplay(const PrAppWindow* base, const PrAppDisplayPosition* position, PrVec2* out) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    POINT origin{0, 0};
    Win32Monitor monitor{};
    if (appOf(window)->headless || !ClientToScreen(window->window, &origin) || !win32MonitorWithId(position->display, &monitor))
        return false;
    float fromMonitor = float(monitor.dpi) / USER_DEFAULT_SCREEN_DPI;
    float x = float(monitor.bounds.left) + position->position.x * fromMonitor;
    float y = float(monitor.bounds.top) + position->position.y * fromMonitor;
    float toWindow = USER_DEFAULT_SCREEN_DPI / float(window->dpi);
    *out = PrVec2{(x - float(origin.x)) * toWindow, (y - float(origin.y)) * toWindow};
    return true;
}

void setPosition(PrAppWindow* base, const PrAppDisplayPosition* position) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless || window->fullscreen)
        return;
    if (IsZoomed(window->window) || IsIconic(window->window))
        ShowWindow(window->window, SW_RESTORE);
    Win32Monitor monitor{};
    if (win32MonitorWithId(position->display, &monitor)) {
        placeOnMonitor(window, monitor, position->position);
        return;
    }
    // A display that is gone gives the primary display, at the same position if the window fits there.
    if (!win32PrimaryMonitor(&monitor))
        return;
    PrRect work = win32LogicalOn(monitor, monitor.work);
    bool fits = position->position.x >= work.left && position->position.y >= work.top &&
        position->position.x + float(window->extent.x) <= work.right &&
        position->position.y + float(window->extent.y) <= work.bottom;
    if (fits)
        placeOnMonitor(window, monitor, position->position);
    else
        centerOnMonitor(window, monitor);
}

// Covers the window's display with its content, without a frame, or returns it to where it stood before.
void setFullscreen(Win32Window* window, bool fullscreen) {
    if (fullscreen) {
        window->restore.length = sizeof(window->restore);
        GetWindowPlacement(window->window, &window->restore);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST), &monitor);
        window->fullscreen = true;
        SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window)));
        RECT area = monitor.rcMonitor;
        SetWindowPos(window->window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        window->fullscreen = false;
        SetWindowLongPtrW(window->window, GWL_STYLE, LONG_PTR(windowStyle(window) | WS_VISIBLE));
        SetWindowPlacement(window->window, &window->restore);
        SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    win32RequestFrame(window);
}

bool isActive(const PrAppWindow* base) {
    return GetForegroundWindow() == static_cast<const Win32Window*>(base)->window;
}

// Focusing shows a hidden window, and the windows of a hidden application.
void focus(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    Win32App* app = appOf(window);
    window->shown = true;
    window->showing = false;
    if (app->headless)
        return;
    if (app->hidden)
        win32SetHidden(app, false);
    if (!IsWindowVisible(window->window))
        ShowWindow(window->window, SW_SHOW);
    win32BringForward(window);
}

void requestAttention(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (GetForegroundWindow() == window->window)
        return;
    // The taskbar button flashes until the window comes to the foreground.
    FLASHWINFO flash{.cbSize = sizeof(FLASHWINFO), .hwnd = window->window, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
    FlashWindowEx(&flash);
}

// Records the drag and posts its start to the window, whose procedure starts it once no callback runs.
void dragStart(PrAppWindow* base, const PrAppDragDesc* desc) {
    Win32Window* window = static_cast<Win32Window*>(base);
    appDragPendingSet(window->drag, *desc);
    PostMessageW(window->window, kMessageDragStart, 0, 0);
}

void setBlocking(Win32Window* window, bool blocking);

void setVisible(PrAppWindow* base, bool visible) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (window->shown == visible)
        return;
    window->shown = visible;
    window->hiddenByApp = false;
    if (appOf(window)->headless)
        return;
    if (visible) {
        window->showing = true;
        PostMessageW(window->window, kMessageShow, 0, 0);
        return;
    }
    window->showing = false;
    win32SetLock(window, false);
    setBlocking(window, false);
    ShowWindow(window->window, SW_HIDE);
}

bool isVisible(const PrAppWindow* base) {
    return static_cast<const Win32Window*>(base)->shown;
}

// Applies the window's opacity and passthrough. A layered window's alpha fades the whole window, frame and content, and
// the compositor blends it with a flip-model swap chain's content. The pointer passes through a layered window that is
// transparent to hit tests.
void applyLayering(Win32Window* window) {
    bool layered = window->opacity < 1 || window->passthrough;
    LONG_PTR style = GetWindowLongPtrW(window->window, GWL_EXSTYLE) & ~LONG_PTR(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    if (layered)
        style |= WS_EX_LAYERED;
    if (window->passthrough)
        style |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(window->window, GWL_EXSTYLE, style);
    if (layered)
        SetLayeredWindowAttributes(window->window, 0, BYTE(window->opacity * 255.0f + 0.5f), LWA_ALPHA);
}

void setPointerPassthrough(PrAppWindow* base, bool passthrough) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->passthrough = passthrough;
    applyLayering(window);
}

void setOpacity(PrAppWindow* base, float opacity) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->opacity = opacity > 0 ? (opacity < 1 ? opacity : 1) : 0;
    applyLayering(window);
}

// Shows the window's backdrop behind its content. The window has no surface of its own, so where the content's alpha is
// less than one, what is behind the window shows. Blur is DWM's Acrylic, which Windows 11 22H2 and later draw where
// DWM's frame reaches, so the frame covers the whole client area. A sheet's frame reaches one pixel into its client
// area, which gives it the frame's shadow.
void applyBackdrop(Win32Window* window) {
    PrAppWindowBackdrop shown = window->transparent ? window->backdrop : PrAppWindowBackdrop_Opaque;
    DWM_SYSTEMBACKDROP_TYPE system = shown == PrAppWindowBackdrop_Blur ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
    if (FAILED(DwmSetWindowAttribute(window->window, DWMWA_SYSTEMBACKDROP_TYPE, &system, sizeof(system))) &&
            shown == PrAppWindowBackdrop_Blur)
        shown = PrAppWindowBackdrop_Opaque;
    int extent = shown == PrAppWindowBackdrop_Blur ? -1 : (window->sheet ? 1 : 0);
    MARGINS margins{extent, extent, extent, extent};
    if (FAILED(DwmExtendFrameIntoClientArea(window->window, &margins)))
        shown = PrAppWindowBackdrop_Opaque;
    window->shownBackdrop = shown;
}

void setBackdrop(PrAppWindow* base, PrAppWindowBackdrop backdrop) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->backdrop = backdrop;
    applyBackdrop(window);
    win32RequestFrame(window);
}

void setAspectRatio(PrAppWindow* base, float ratio) {
    static_cast<Win32Window*>(base)->aspectRatio = ratio > 0 ? ratio : 0;
}

// Keeps the content's aspect ratio while the user drags an edge of the frame. `edge` is WM_SIZING's, and `rect` the
// frame in screen pixels, which this adjusts.
void keepAspectRatio(Win32Window* window, WPARAM edge, RECT* rect) {
    RECT border{};
    AdjustWindowRectExForDpi(&border, DWORD(GetWindowLongPtrW(window->window, GWL_STYLE)), FALSE,
        DWORD(GetWindowLongPtrW(window->window, GWL_EXSTYLE)), window->dpi);
    LONG borderWidth = border.right - border.left;
    LONG borderHeight = border.bottom - border.top;
    LONG width = rect->right - rect->left - borderWidth;
    LONG height = rect->bottom - rect->top - borderHeight;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
        LONG wanted = LONG(float(width) / window->aspectRatio + 0.5f);
        rect->bottom = rect->top + wanted + borderHeight;
        return;
    }
    LONG wanted = LONG(float(height) * window->aspectRatio + 0.5f);
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
        rect->left = rect->right - wanted - borderWidth;
    else
        rect->right = rect->left + wanted + borderWidth;
}

// Moves a popup to where its placement puts it against its parent, within the parent's work area.
void placePopup(Win32Window* window) {
    Win32Window* parent = window->parent;
    if (window->role != PrAppWindowRole_Popup || parent == nullptr || appOf(window)->headless)
        return;
    Win32Monitor monitor{};
    if (!win32MonitorOf(MonitorFromWindow(parent->window, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    float scale = float(parent->dpi) / 96.0f;
    POINT origin{0, 0};
    ClientToScreen(parent->window, &origin);
    const PrRect& a = window->popup.anchor;
    PrRect anchor{
        float(origin.x) + a.left * scale, float(origin.y) + a.top * scale, float(origin.x) + a.right * scale,
        float(origin.y) + a.bottom * scale,
    };
    PrAppPopupPlacement placement = window->popup;
    placement.offset = PrVec2{placement.offset.x * scale, placement.offset.y * scale};
    RECT client{};
    GetClientRect(window->window, &client);
    PrVec2 extent{float(client.right - client.left), float(client.bottom - client.top)};
    PrRect work{
        float(monitor.work.left), float(monitor.work.top), float(monitor.work.right),
        float(monitor.work.bottom),
    };
    bool rightToLeft = (GetWindowLongPtrW(parent->window, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    placement.anchor = anchor;
    PrRect placed = prAppPopupRect(&placement, extent, work,
        PrAppLayoutDirection(rightToLeft ? PrAppLayoutDirection_RightToLeft : PrAppLayoutDirection_LeftToRight));
    SetWindowPos(window->window, nullptr, int(placed.left), int(placed.top), int(placed.right - placed.left),
        int(placed.bottom - placed.top), SWP_NOZORDER | SWP_NOACTIVATE);
}

void setPopupPlacement(PrAppWindow* base, const PrAppPopupPlacement* placement) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->popup = *placement;
    placePopup(window);
}

// Asks each popup to close, but `except`, as for a press elsewhere or the application's deactivation.
void closePopups(Win32App* app, Win32Window* PR_NULLABLE except) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window == except || window->role != PrAppWindowRole_Popup || !IsWindowVisible(window->window))
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_CloseRequested;
        event.time = win32SecondsNow();
        win32Deliver(window, event);
    }
}

// Disables a modal dialog's parent while the dialog shows, or enables it again, and gives it back the keyboard. Returns
// the modal dialog of `window` that shows, or null.
Win32Window* PR_NULLABLE blockerOf(Win32Window* window) {
    Win32App* app = appOf(window);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* dialog = app->windows.data[i];
        if (dialog->parent == window && dialog->blocking)
            return dialog;
    }
    return nullptr;
}

// Places the window's dim over its visible frame, with the frame's rounded corners, just below the modal dialog that
// shows.
void placeDim(Win32Window* window) {
    Win32Window* dialog = blockerOf(window);
    if (window->dim == nullptr || dialog == nullptr)
        return;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window->window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        GetWindowRect(window->window, &frame);
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    int diameter = 2 * MulDiv(kCornerRadius, int(window->dpi), 96);
    bool square = IsZoomed(window->window) || window->fullscreen;
    SetWindowRgn(window->dim, square ? nullptr : CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter), TRUE);
    SetWindowPos(window->dim, dialog->window, frame.left, frame.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Disables a modal dialog's parent while the dialog shows, and dims it, or enables it again, and gives it back the
// keyboard. The dim is a translucent black window that the parent owns, which the pointer passes through.
void setBlocking(Win32Window* window, bool blocking) {
    Win32Window* parent = window->parent;
    if (!window->modal || parent == nullptr || window->blocking == blocking)
        return;
    window->blocking = blocking;
    EnableWindow(parent->window, blocking ? FALSE : TRUE);
    if (blocking && !appOf(window)->headless) {
        if (parent->dim == nullptr) {
            parent->dim = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                kDimClassName, L"", WS_POPUP, 0, 0, 0, 0, parent->window, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (parent->dim != nullptr)
                SetLayeredWindowAttributes(parent->dim, 0, kDimAlpha, LWA_ALPHA);
        }
        placeDim(parent);
    }
    if (!blocking) {
        if (parent->dim != nullptr && blockerOf(parent) == nullptr) {
            DestroyWindow(parent->dim);
            parent->dim = nullptr;
        }
        SetActiveWindow(parent->window);
    }
}

// Windows tells only that a window is hidden, minimized or cloaked on another desktop, and not that other windows cover
// it.
bool isOccluded(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (appOf(window)->headless)
        return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(window->window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked || IsIconic(window->window) || !IsWindowVisible(window->window);
}

void setKeepAbove(PrAppWindow* base, bool above) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (window->role != PrAppWindowRole_Normal)
        return;
    SetWindowPos(window->window, above ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void setIcon(PrAppWindow* base, PrPixelView pixels) {
    Win32Window* window = static_cast<Win32Window*>(base);
    HICON icon = win32IconOf(pixels);
    SendMessageW(window->window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(window->window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    if (window->icon != nullptr)
        DestroyIcon(window->icon);
    window->icon = icon;
}

void setProgress(PrAppWindow* base, PrAppProgressState state, float progress) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->progressState = state;
    window->progress = progress;
    win32ApplyTaskbar(window);
}

PrStatus surface(const PrAppWindow* base, PrAppSurface* out) {
    out->kind = PrAppSurfaceKind_Hwnd;
    out->handle = static_cast<const Win32Window*>(base)->window;
    return PrStatus_Ok;
}

// Places a new window as its description asks. By default, a window cascades from the last one that opened, and the
// first is centered on the primary display.
void placeNewWindow(Win32App* app, Win32Window* window, const PrAppWindowDesc* desc) {
    Win32Monitor monitor{};
    if (desc->placement == PrAppWindowPlacement_Position && win32MonitorWithId(desc->position.display, &monitor)) {
        placeOnMonitor(window, monitor, desc->position.position);
        return;
    }
    // A dialog is centered on its parent.
    PrAppDisplayPosition parentAt{};
    if (window->parent != nullptr && position(window->parent, &parentAt) &&
            win32MonitorWithId(parentAt.display, &monitor)) {
        PrUVec2 around = window->parent->extent;
        PrVec2 at{
            parentAt.position.x + (float(around.x) - float(window->extent.x)) / 2,
            parentAt.position.y + (float(around.y) - float(window->extent.y)) / 2,
        };
        placeOnMonitor(window, monitor, at);
        return;
    }
    if (desc->placement == PrAppWindowPlacement_Center) {
        HMONITOR active = nullptr;
        if (app->lastActive != nullptr)
            active = MonitorFromWindow(app->lastActive->window, MONITOR_DEFAULTTOPRIMARY);
        if ((active != nullptr && win32MonitorOf(active, &monitor)) || win32PrimaryMonitor(&monitor))
            centerOnMonitor(window, monitor);
        return;
    }
    PrAppDisplayPosition previous{};
    if (app->windows.count != 0 && position(app->windows.data[app->windows.count - 1], &previous) &&
            win32MonitorWithId(previous.display, &monitor)) {
        // The offset is the height of a title bar, so that the last window's title shows below the new window's.
        float step = float(GetSystemMetricsForDpi(SM_CYCAPTION, monitor.dpi)) * 96.0f / float(monitor.dpi) +
            float(GetSystemMetricsForDpi(SM_CYFRAME, monitor.dpi)) * 96.0f / float(monitor.dpi);
        placeOnMonitor(window, monitor, PrVec2{previous.position.x + step, previous.position.y + step});
    }
}

} // namespace

PrVec2 win32ExtentOf(Win32Window* window) {
    RECT client;
    if (!GetClientRect(window->window, &client))
        return PrVec2{0, 0};
    float scale = 96.0f / float(window->dpi);
    return PrVec2{float(client.right) * scale, float(client.bottom) * scale};
}

void win32Pace(void* PR_NULLABLE context) {
    Win32App* app = static_cast<Win32App*>(context);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    for (;;) {
        WaitForSingleObject(app->pacerRequest, INFINITE);
        if (app->pacerQuit.load())
            break;
        waitForRefresh(timer);
        if (app->pacerQuit.load())
            break;
        PostMessageW(app->messageWindow, kMessageFrame, 0, 0);
    }
    if (timer != nullptr)
        CloseHandle(timer);
}

bool win32IsOpen(const Win32App* app, const Win32Window* window, uint64_t serial) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window)
            return app->windows.data[i]->serial == serial;
    }
    return false;
}

void win32RunRequestedFrames(Win32App* app) {
    app->frameDue = false;
    RawArray<Win32Window*> due;
    RawArray<uint64_t> serials;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* window = app->windows.data[i];
        if (window->frameRequested && !window->inSizeMove) {
            due.push(window);
            serials.push(window->serial);
        }
    }
    for (uint32_t i = 0; i < due.count; ++i) {
        if (win32IsOpen(app, due.data[i], serials.data[i]))
            runFrame(due.data[i]);
    }
    due.reset();
    serials.reset();
}

bool win32AnyFrameRequested(const Win32App* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i]->frameRequested)
            return true;
    }
    return false;
}

void win32BringForward(Win32Window* window) {
    if (IsIconic(window->window))
        ShowWindow(window->window, SW_RESTORE);
    SetForegroundWindow(window->window);
}

LRESULT CALLBACK win32WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    Win32Window* window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (window == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    Win32App* app = appOf(window);
    if (message == app->taskbarButtonCreated && app->taskbarButtonCreated != 0) {
        win32ApplyTaskbar(window);
        return 0;
    }
    switch (message) {
        case WM_CLOSE: {
            // The program closes the window if it chooses to.
            PrAppEvent event{};
            event.kind = PrAppEventKind_CloseRequested;
            event.time = win32SecondsNow();
            win32Deliver(window, event);
            return 0;
        }
        case WM_ERASEBKGND:
            // The frames fill the window, and an erase would flash between them.
            return 1;
        case WM_SIZE:
            // A frame at each step of a resize presents at the window's new size before the compositor shows it. A
            // minimized window has no frame to run, and its next frame sees the new state. The dim follows first, since
            // the frame may destroy the window.
            placeDim(window);
            if (wParam != SIZE_MINIMIZED)
                runFrame(window);
            else
                win32RequestFrame(window);
            win32LifecycleMayChange(app);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            if (window->frameRequested && !window->inSizeMove)
                runFrame(window);
            return 0;
        }
        case WM_NCCALCSIZE:
            // With a custom title bar, the content covers the caption, and keeps the system's resize borders at the
            // sides and the bottom. A maximized window overhangs its display by its border, which its content leaves
            // out.
            if (window->customTitleBar && !window->fullscreen && wParam == TRUE) {
                NCCALCSIZE_PARAMS* sizes = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                LONG top = sizes->rgrc[0].top;
                DefWindowProcW(hwnd, message, wParam, lParam);
                sizes->rgrc[0].top = top;
                if (IsZoomed(hwnd))
                    sizes->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, window->dpi) +
                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window->dpi);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (window->customTitleBar && !window->fullscreen)
                return customHitTest(window, wParam, lParam);
            break;
        case WM_NCLBUTTONDOWN:
            if (window->customTitleBar && (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                window->pressedButton = LRESULT(wParam);
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (window->customTitleBar && (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                customButtonReleased(window, LRESULT(wParam));
                return 0;
            }
            break;
        case WM_NCMOUSEMOVE:
            // The pointer over a custom title bar is in the program's content, which shows a button's hover, and which
            // the pointer leaves only as it leaves the title bar too.
            if (window->customTitleBar &&
                    (wParam == HTCAPTION || wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)) {
                POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
                ScreenToClient(hwnd, &at);
                win32DeliverPointer(window, WM_MOUSEMOVE, 0, MAKELPARAM(at.x, at.y), true);
                if (wParam != HTCAPTION)
                    return 0;
            }
            break;
        case WM_MOUSELEAVE:
            win32PointerLeft(window, false);
            return 0;
        case WM_NCMOUSELEAVE:
            win32PointerLeft(window, true);
            break;
        case WM_GETMINMAXINFO: {
            // The system's least width fits the title bar's buttons, and the window may be as small as its frame around
            // its least extent.
            UINT dpi = window->dpi != 0 ? window->dpi : 96;
            MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            PrUVec2 least{
                window->minExtent.x > 1 ? window->minExtent.x : 1,
                window->minExtent.y > 1 ? window->minExtent.y : 1,
            };
            limits->ptMinTrackSize = frameSizeOf(window, least, dpi);
            if (window->maxExtent.x != 0 || window->maxExtent.y != 0) {
                POINT most = frameSizeOf(window, window->maxExtent, dpi);
                if (window->maxExtent.x != 0)
                    limits->ptMaxTrackSize.x = most.x;
                if (window->maxExtent.y != 0)
                    limits->ptMaxTrackSize.y = most.y;
            }
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            window->inSizeMove = true;
            break;
        case WM_EXITSIZEMOVE:
            // A frame that was requested during the move or the resize was skipped (runRequestedFrames), and is still
            // requested, so no later request schedules it.
            window->inSizeMove = false;
            if (window->frameRequested)
                scheduleFrame(window);
            break;
        case WM_MOVE:
            if (app->running && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != window->monitor)
                win32RequestFrame(window);
            placeDim(window);
            break;
        case WM_DPICHANGED: {
            window->dpi = HIWORD(wParam);
            win32NoteDisplaysChanged(app);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            win32RequestFrame(window);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            releaseDisplayObjects(window);
            win32RequestFrame(window);
            win32NoteDisplaysChanged(app);
            break;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA)
                win32NoteDisplaysChanged(app);
            [[fallthrough]];
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            refreshPreferences(window);
            win32RequestFrame(window);
            break;
        case WM_SETCURSOR:
            // A locked pointer hides its cursor.
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(window->locked ? nullptr : window->cursor);
                return TRUE;
            }
            break;
        case WM_SYSCOMMAND:
            // Alt or F10 alone would enter the menu loop of a window that has no menu, which takes the next key.
            if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
                return 0;
            break;
        case WM_TIMER:
            if (wParam == kTimerFrameAt) {
                KillTimer(hwnd, kTimerFrameAt);
                win32RequestFrame(window);
                return 0;
            }
            if (wParam != kTimerFrame)
                break;
            KillTimer(hwnd, kTimerFrame);
            if (window->frameRequested)
                runFrame(window);
            return 0;
        case kMessageDragStart:
            // A callback that runs a modal loop dispatches the message within it, and the drag waits for its return.
            if (appOf(window)->inCallback)
                PostMessageW(hwnd, kMessageDragStart, 0, 0);
            else
                win32StartDrag(window);
            return 0;
        case kMessageShow: {
            // The window shows after a frame, which waits for the callback that runs.
            if (!window->showing)
                return 0;
            if (app->inCallback) {
                PostMessageW(hwnd, kMessageShow, 0, 0);
                return 0;
            }
            uint64_t serial = window->serial;
            runFrame(window);
            if (win32IsOpen(app, window, serial) && window->showing) {
                window->showing = false;
                bool passive = window->overlay || window->role == PrAppWindowRole_Popup;
                ShowWindow(hwnd, passive ? SW_SHOWNOACTIVATE : SW_SHOW);
                setBlocking(window, true);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd)
                win32ReleaseButtons(window);
            return 0;
        case WM_KILLFOCUS:
            win32ReleaseKeys(window);
            break;
        case WM_ACTIVATEAPP:
            if (!wParam)
                closePopups(app, nullptr);
            win32LifecycleMayChange(app);
            break;
        case WM_INPUT:
            if (window->rawInput)
                win32DeliverRawInput(window, lParam);
            break;
        case WM_INITMENUPOPUP:
            win32MenuOpening(app, window, reinterpret_cast<HMENU>(wParam));
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == 0 || HIWORD(wParam) == 1) {
                win32MenuCommand(app, window, LOWORD(wParam));
                return 0;
            }
            break;
        case kMessageShowMenu:
            // The menu's modal loop waits for the callback that runs.
            if (app->inCallback) {
                PostMessageW(hwnd, kMessageShowMenu, 0, 0);
                return 0;
            }
            win32ShowContextMenu(window);
            return 0;
        case WM_SIZING:
            if (window->aspectRatio > 0) {
                keepAspectRatio(window, wParam, reinterpret_cast<RECT*>(lParam));
                return TRUE;
            }
            break;
        case WM_INPUTLANGCHANGE:
            win32KeyLabelsReset(app);
            win32DeliverKind(app, PrAppEventKind_KeyboardLayoutChanged);
            break;
        case WM_ACTIVATE:
            // Activation changes what prAppWindowIsActive returns, so the next frame should see it.
            if (LOWORD(wParam) != WA_INACTIVE)
                app->lastActive = window;
            else
                win32SetLock(window, false);
            win32RequestFrame(window);
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            // A press outside a popup closes it.
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
                closePopups(app, window);
            win32DeliverPointer(window, message, wParam, lParam);
            return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            win32DeliverPointer(window, message, wParam, lParam);
            return TRUE;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            win32DeliverScroll(window, wParam, lParam, message == WM_MOUSEHWHEEL);
            return 0;
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_POINTERUPDATE:
        case WM_POINTERCAPTURECHANGED:
            // A pen's or a touch's message handled here makes no mouse message.
            if (win32DeliverPenOrTouch(window, message, wParam))
                return 0;
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
            win32DeliverKey(window, message, wParam, lParam);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // The system keeps its own chords, such as Alt+F4.
            win32DeliverKey(window, message, wParam, lParam);
            break;
        case WM_CHAR:
            win32DeliverCharacter(window, wchar_t(wParam));
            return 0;
        case WM_SYSCHAR:
            // An Alt chord types nothing. Alt+Space opens the window's menu.
            if (wParam == L' ')
                break;
            return 0;
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return 0;
        case WM_UNICHAR:
            if (wParam == UNICODE_NOCHAR)
                return TRUE;
            win32DeliverCodePoint(window, uint32_t(wParam));
            return 0;
        case WM_IME_SETCONTEXT:
            // The program shows the composition, and the input method shows only its candidates.
            lParam &= ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW);
            break;
        case WM_IME_STARTCOMPOSITION:
            win32PlaceInputMethod(window);
            return 0;
        case WM_IME_COMPOSITION:
            win32DeliverComposition(window, lParam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            if (window->composing) {
                window->composing = false;
                win32DeliverText(window, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void win32WindowDestroy(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appWindowDeliveryDeferDestroy(window->delivery))
        return;
    Win32App* app = appOf(window);
    // The procedure ignores the window's messages from here on, such as the loss of the capture below, which would
    // otherwise deliver events to the window after it is freed.
    SetWindowLongPtrW(window->window, GWLP_USERDATA, 0);
    win32ForgetWindow(app, window);
    forgetTextInput(window);
    // A file dialog of the window closes as the user's cancel, and a locked pointer is released.
    if (app->dialog != nullptr && app->dialogParent == window->window)
        app->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (window->locked)
        ClipCursor(nullptr);
    setBlocking(window, false);
    win32SetPasswordInput(window, false);
    win32SetRawInput(window, false);
    // A window destroys the menu bar that it shows, which the tree frees instead.
    SetMenu(window->window, nullptr);
    win32MenuTreeReset(window->menuBar);
    win32MenuTreeReset(window->contextMenu);
    // The system destroys the windows that a window owns along with it, so its dialogs and popups lose their owner
    // first, and stay open until the program destroys them.
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        Win32Window* child = app->windows.data[i];
        if (child->parent != window)
            continue;
        child->parent = nullptr;
        child->blocking = false;
        SetWindowLongPtrW(child->window, GWLP_HWNDPARENT, 0);
    }
    if (window->dim != nullptr)
        DestroyWindow(window->dim);
    window->dim = nullptr;
    KillTimer(window->window, kTimerFrame);
    KillTimer(window->window, kTimerFrameAt);
    if (GetCapture() == window->window)
        ReleaseCapture();
    if (app->drag.window == window)
        app->drag.window = nullptr;
    win32DragsRevoke(window);
    appDragPendingReset(window->drag);
    releaseDisplayObjects(window);
    appTitleBarRegionsReset(window->titleBar);
    prRawStringReset(&window->titleText);
    DestroyWindow(window->window);
    if (window->icon != nullptr)
        DestroyIcon(window->icon);
    prMemoryFree(window->title);
    prMemoryFree(window);
}

PrAppFrame win32FrameState(PrAppWindow* base) {
    return frameAt(static_cast<Win32Window*>(base), win32SecondsNow());
}

void win32RequestFrame(PrAppWindow* base) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (!appOf(window)->running || window->frameRequested)
        return;
    window->frameRequested = true;
    scheduleFrame(window);
}

PrAppWindowState win32State(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (appOf(window)->headless)
        return PrAppWindowState_Normal;
    if (window->fullscreen)
        return PrAppWindowState_Fullscreen;
    if (IsIconic(window->window))
        return PrAppWindowState_Minimized;
    return IsZoomed(window->window) ? PrAppWindowState_Maximized : PrAppWindowState_Normal;
}

void win32SetState(PrAppWindow* base, PrAppWindowState wanted) {
    Win32Window* window = static_cast<Win32Window*>(base);
    if (appOf(window)->headless || win32State(window) == wanted)
        return;
    if (window->fullscreen)
        setFullscreen(window, false);
    switch (wanted) {
        case PrAppWindowState_Normal:
            if (IsIconic(window->window) || IsZoomed(window->window))
                ShowWindow(window->window, SW_RESTORE);
            break;
        case PrAppWindowState_Minimized:
            ShowWindow(window->window, SW_MINIMIZE);
            break;
        case PrAppWindowState_Maximized:
            if (!isFixed(window))
                ShowWindow(window->window, SW_MAXIMIZE);
            break;
        case PrAppWindowState_Fullscreen:
            if (IsIconic(window->window) || IsZoomed(window->window))
                ShowWindow(window->window, SW_RESTORE);
            setFullscreen(window, true);
            break;
        default:
            break;
    }
}

HICON PR_NULLABLE win32IconOf(PrPixelView pixels) {
    if (pixels.extent.x == 0 || pixels.extent.y == 0 || pixels.data == nullptr)
        return nullptr;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(pixels.extent.x);
    header.bV5Height = -LONG(pixels.extent.y);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr)
        return nullptr;
    uint8_t* out = static_cast<uint8_t*>(bits);
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(pixels.data) + size_t(y) * pixels.stride;
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            const uint8_t* in = row + size_t(x) * 4;
            uint8_t* pixel = out + (size_t(y) * pixels.extent.x + x) * 4;
            pixel[0] = in[2];
            pixel[1] = in[1];
            pixel[2] = in[0];
            pixel[3] = in[3];
        }
    }
    HBITMAP mask = CreateBitmap(int(pixels.extent.x), int(pixels.extent.y), 1, 1, nullptr);
    ICONINFO info{.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

PrStatus win32WindowCreate(PrApp* base, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    if (app->windowModel == PrAppWindowModel_Single && app->windows.count != 0)
        return PrStatus_NotSupported;
    Win32Window* window = allocZeroed<Win32Window>();
    window->destroyFn = win32WindowDestroy;
    window->appFn = windowApp;
    window->requestFrameFn = win32RequestFrame;
    window->setTitleFn = setTitle;
    window->titleFn = title;
    window->setExtentFn = setExtent;
    window->setExtentLimitsFn = setExtentLimits;
    window->positionFn = position;
    window->setPositionFn = setPosition;
    window->pointToDisplayFn = pointToDisplay;
    window->pointFromDisplayFn = pointFromDisplay;
    window->setStateFn = win32SetState;
    window->stateFn = win32State;
    window->isActiveFn = isActive;
    window->focusFn = focus;
    window->requestAttentionFn = requestAttention;
    window->setVisibleFn = setVisible;
    window->isVisibleFn = isVisible;
    window->setPointerPassthroughFn = setPointerPassthrough;
    window->setOpacityFn = setOpacity;
    window->setBackdropFn = setBackdrop;
    window->setPointerModeFn = win32SetPointerMode;
    window->pointerModeFn = win32PointerMode;
    window->shareFn = win32Share;
    window->setMenuBarFn = win32WindowSetMenuBar;
    window->showMenuFn = win32ShowMenu;
    window->dragStartFn = dragStart;
    window->surfaceFn = surface;
    window->setAspectRatioFn = setAspectRatio;
    window->setPopupPlacementFn = setPopupPlacement;
    window->isOccludedFn = isOccluded;
    window->setKeepAboveFn = setKeepAbove;
    window->setIconFn = setIcon;
    window->setProgressFn = setProgress;
    window->app = app;
    window->serial = ++app->lastSerial;
    window->delivery.handler = desc->handler;
    RawArray<wchar_t> title;
    windowsWiden(desc->title, title);
    window->title = title.data;
    window->titleText = prRawStringCopy(desc->title);
    window->minExtent = desc->minExtent;
    window->maxExtent = desc->maxExtent;
    window->role = desc->role;
    window->overlay = desc->role == PrAppWindowRole_Overlay;
    bool popup = desc->role == PrAppWindowRole_Popup;
    bool dialog = desc->role == PrAppWindowRole_Dialog;
    if (desc->parent != nullptr && (popup || dialog))
        window->parent = static_cast<Win32Window*>(desc->parent);
    window->modal = dialog && desc->modal && window->parent != nullptr;
    window->popup = desc->popup;
    window->shown = !desc->hidden;
    window->sheet = window->modal && desc->decoration != PrAppWindowDecoration_None;
    window->customTitleBar =
        desc->decoration == PrAppWindowDecoration_Custom && !window->overlay && !popup && !window->sheet;
    window->pressedButton = HTNOWHERE;
    window->extent = desc->extent;
    // A dialog has no minimize button, since it follows its parent, and a window without decoration has no title bar.
    window->style = styleOf(app);
    if (dialog)
        window->style &= ~DWORD(WS_MINIMIZEBOX);
    if (desc->decoration == PrAppWindowDecoration_None && !popup && !window->overlay)
        window->style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD style = window->style;
    if (isFixed(window))
        style &= ~DWORD(WS_THICKFRAME | WS_MAXIMIZEBOX);
    // An overlay is a popup above every other window, which the taskbar and the window switcher leave out, and which a
    // press does not activate. A popup is the same, but above its parent alone, which owns it.
    DWORD exStyle = 0;
    if (window->overlay) {
        style = WS_POPUP;
        exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    } else if (popup) {
        style = WS_POPUP;
        exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    }
    if (window->overlay || popup)
        window->style = WS_POPUP;
    // A transparent window has no redirection surface, which its swap chain's visual never uses, so that nothing lies
    // under the visual where its content is clear (applyBackdrop).
    window->transparent = desc->transparent || desc->backdrop != PrAppWindowBackdrop_Opaque;
    if (window->transparent)
        exStyle |= WS_EX_NOREDIRECTIONBITMAP;
    // A sheet has no title bar and no resize border, and the program draws all of it.
    if (window->sheet) {
        style = WS_POPUP;
        window->style = WS_POPUP;
    }
    // An owned window stays above its owner and off the taskbar. An Accessory application's windows are owned by its
    // message window, which keeps them off the taskbar.
    HWND owner = window->parent != nullptr ? window->parent->window : nullptr;
    if (owner == nullptr && desc->role == PrAppWindowRole_Normal && app->presence == PrAppPresence_Accessory)
        owner = app->messageWindow;
    window->window = CreateWindowExW(exStyle, kClassName, window->title, style, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, GetModuleHandleW(nullptr), window);
    if (window->window == nullptr) {
        prMemoryFree(window->title);
        prRawStringReset(&window->titleText);
        prMemoryFree(window);
        return PrStatus_Io;
    }
    window->dpi = GetDpiForWindow(window->window);
    if (window->dpi == 0)
        window->dpi = 96;
    // A sheet takes the rounded corners and the shadow of a window with a frame.
    if (window->sheet) {
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(window->window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(window->window, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    }
    window->backdrop = desc->backdrop;
    applyBackdrop(window);
    // The window's frame is computed again, now that the window can answer WM_NCCALCSIZE for a custom title bar.
    if (window->customTitleBar)
        SetWindowPos(window->window, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    placeWindow(window, Placement_Center);
    if (!app->headless && popup)
        placePopup(window);
    else if (!app->headless)
        placeNewWindow(app, window, desc);
    window->cursor = win32CursorFor(window->shape);
    win32DragsRegister(window);
    refreshPreferences(window);
    window->display = prAppDisplayDefault();
    window->opacity = 1;
    app->windows.push(window);
    if (desc->pointerPassthrough)
        setPointerPassthrough(window, true);
    win32ShowMenuBar(window);
    // An overlay and a popup show without taking the foreground.
    if (!app->headless && window->shown && (window->overlay || popup)) {
        ShowWindow(window->window, SW_SHOWNOACTIVATE);
    } else if (!app->headless && window->shown) {
        ShowWindow(window->window, SW_SHOW);
        SetForegroundWindow(window->window);
        setBlocking(window, true);
    }
    if (!app->headless && window->shown && desc->state != PrAppWindowState_Normal)
        win32SetState(window, desc->state);
    win32RequestFrame(window);
    *out = window;
    return PrStatus_Ok;
}

} // namespace pr_impl
