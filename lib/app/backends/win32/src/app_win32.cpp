// The Win32 application backend: a Windows window and the Win32 message loop.

#include "pr/app_win32.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/vec.h"

#include "app/backends/win32/src/win32_browser.h"
#include "app/backends/win32/src/win32_text.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <imm.h>
#include <objbase.h>
#include <shellapi.h>

namespace pr_impl {

namespace {

// The messages that the backend posts to its own window.
constexpr UINT kMessageFrame = WM_APP + 0;      // the pacer saw the display refresh
constexpr UINT kMessageWake = WM_APP + 1;
constexpr UINT kMessageClipboard = WM_APP + 2;  // the program requested the clipboard's text

// The timer that times the frames of a window that is not composed, and its period.
constexpr UINT_PTR kTimerFrame = 1;
constexpr UINT kHiddenFrameMilliseconds = 16;

// How often the display's headroom is read again, in seconds, while frames run. The headroom changes with the SDR
// brightness that the user sets, which Windows announces to no window.
constexpr double kDisplayRefreshSeconds = 1.0;

// The luminance of SDR white in nits that Windows assumes when it reports none.
constexpr float kDefaultSdrWhiteNits = 80.0f;

// ToUnicodeEx's flag that leaves the keyboard's dead-key state unchanged (Windows 10, version 1607).
constexpr UINT kToUnicodeKeepState = 1u << 2;

constexpr wchar_t kClassName[] = L"PrAppWin32";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

// What a key types, which the message loop reads before TranslateMessage records a dead key that the key begins. Read
// after that, a dead key would combine with itself.
struct KeyText {
    UINT message;
    WPARAM virtualKey;
    LPARAM flags;
    bool dead;                  // the key is a dead key, which types nothing until the key it combines with
    int count;                  // UTF-16 units
    wchar_t units[8];
};

// An event that arrived while a callback ran, which waits for the callback to return, with its own copy of its text.
struct HeldEvent {
    PrAppEvent event;
    Array<char> text;
};

// What a key that is down reports, so that the window can release it when it loses the keyboard.
struct HeldKey {
    bool down;
    PrAppKey key;
};

// The pointer buttons that the window can report down, by PrAppEvent::button.
constexpr uint8_t kPointerButtons = 5;

struct Win32App : PrApp {
    PrAppHandler handler;
    wchar_t* title;             // NUL-terminated
    PrUVec2 extent;             // the extent that the window opens at, in logical pixels
    bool headless;
    bool running;
    bool inCallback;            // a callback is running, and no other may start
    bool frameRequested;
    bool frameDue;              // the pacer's refresh for the requested frame came, and the frame runs once no other
                                // message waits
    bool framed;                // a frame ran in this run
    bool inSizeMove;            // the user moves or resizes the window, in the system's modal loop
    bool composing;             // an input method composes text
    double lastFrame;
    HWND window;
    HCURSOR cursor;
    UINT dpi;
    wchar_t highSurrogate;      // the first half of a character that WM_CHAR splits, or zero
    KeyText keyText;
    PrAppUserPreferences preferences;

    // The display that shows the window, as it was last read, and the DXGI objects that it was read from.
    PrAppDisplay display;
    HMONITOR monitor;
    double displayRead;
    IDXGIFactory1* factory;
    IDXGIOutput6* output;

    // The thread that waits for the display's refresh after a frame is requested, and the event that it waits on.
    PrThread pacer;
    HANDLE pacerRequest;
    Atomic<bool> pacerQuit;

    // The window that prAppWake posts to, read from any thread, or null outside a run.
    Atomic<HWND> wakeWindow;
    Atomic<bool> wakePending;

    // The events that arrived during the running callback, in order. A system call that a callback makes may dispatch
    // messages, and the events that they deliver wait here, so that no callback nests in another.
    Array<HeldEvent> held;

    // The pointer buttons that are down, by bit of PrAppEvent::button, where the pointer last was, and the keys that
    // are down, by their position. The window releases them when it loses the pointer's capture or the keyboard, which
    // then reports no release of its own.
    uint8_t buttons;
    PrVec2 pointer;
    HeldKey keys[PrAppKeyCode_Count];

    // The text of the event being delivered, and scratch storage for conversions.
    Array<char> utf8;
    Array<wchar_t> utf16;
};

// The window's style. The title bar's minimize and maximize buttons set a least width that no window message lowers, so
// a window that is never shown has none, and opens at any extent.
DWORD styleOf(const Win32App* app) {
    return app->headless ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME : WS_OVERLAPPEDWINDOW;
}

double secondsNow() {
    return prTimeMonotonicMilliseconds() / 1e3;
}

Win32App* PR_NULLABLE appOf(HWND window) {
    return reinterpret_cast<Win32App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool isDown(int virtualKey) {
    return (GetKeyState(virtualKey) & 0x8000) != 0;
}

PrAppModifierFlags modifiersNow() {
    PrAppModifierFlags modifiers = 0;
    if ((GetKeyState(VK_CAPITAL) & 1) != 0)
        modifiers |= PrAppModifierFlag_CapsLock;
    if (isDown(VK_SHIFT))
        modifiers |= PrAppModifierFlag_Shift;
    if (isDown(VK_CONTROL))
        modifiers |= PrAppModifierFlag_Control;
    if (isDown(VK_MENU))
        modifiers |= PrAppModifierFlag_Alt;
    if (isDown(VK_LWIN) || isDown(VK_RWIN))
        modifiers |= PrAppModifierFlag_Super;
    return modifiers;
}

// A key's position by its scan code in set 1, with 0xE000 added for a key that Windows reports as extended.
struct ScanKey {
    uint16_t scan;
    PrAppKeyCode code;
};

constexpr ScanKey kScanKeys[] = {
    {0x01, PrAppKeyCode_Escape}, {0x02, PrAppKeyCode_Digit1}, {0x03, PrAppKeyCode_Digit2},
    {0x04, PrAppKeyCode_Digit3}, {0x05, PrAppKeyCode_Digit4}, {0x06, PrAppKeyCode_Digit5},
    {0x07, PrAppKeyCode_Digit6}, {0x08, PrAppKeyCode_Digit7}, {0x09, PrAppKeyCode_Digit8},
    {0x0A, PrAppKeyCode_Digit9}, {0x0B, PrAppKeyCode_Digit0}, {0x0C, PrAppKeyCode_Minus}, {0x0D, PrAppKeyCode_Equal},
    {0x0E, PrAppKeyCode_Backspace}, {0x0F, PrAppKeyCode_Tab}, {0x10, PrAppKeyCode_KeyQ}, {0x11, PrAppKeyCode_KeyW},
    {0x12, PrAppKeyCode_KeyE}, {0x13, PrAppKeyCode_KeyR}, {0x14, PrAppKeyCode_KeyT}, {0x15, PrAppKeyCode_KeyY},
    {0x16, PrAppKeyCode_KeyU}, {0x17, PrAppKeyCode_KeyI}, {0x18, PrAppKeyCode_KeyO}, {0x19, PrAppKeyCode_KeyP},
    {0x1A, PrAppKeyCode_BracketLeft}, {0x1B, PrAppKeyCode_BracketRight}, {0x1C, PrAppKeyCode_Enter},
    {0x1D, PrAppKeyCode_ControlLeft}, {0x1E, PrAppKeyCode_KeyA}, {0x1F, PrAppKeyCode_KeyS},
    {0x20, PrAppKeyCode_KeyD}, {0x21, PrAppKeyCode_KeyF}, {0x22, PrAppKeyCode_KeyG}, {0x23, PrAppKeyCode_KeyH},
    {0x24, PrAppKeyCode_KeyJ}, {0x25, PrAppKeyCode_KeyK}, {0x26, PrAppKeyCode_KeyL}, {0x27, PrAppKeyCode_Semicolon},
    {0x28, PrAppKeyCode_Quote}, {0x29, PrAppKeyCode_Backquote}, {0x2A, PrAppKeyCode_ShiftLeft},
    {0x2B, PrAppKeyCode_Backslash}, {0x2C, PrAppKeyCode_KeyZ}, {0x2D, PrAppKeyCode_KeyX}, {0x2E, PrAppKeyCode_KeyC},
    {0x2F, PrAppKeyCode_KeyV}, {0x30, PrAppKeyCode_KeyB}, {0x31, PrAppKeyCode_KeyN}, {0x32, PrAppKeyCode_KeyM},
    {0x33, PrAppKeyCode_Comma}, {0x34, PrAppKeyCode_Period}, {0x35, PrAppKeyCode_Slash},
    {0x36, PrAppKeyCode_ShiftRight}, {0x37, PrAppKeyCode_NumpadMultiply}, {0x38, PrAppKeyCode_AltLeft},
    {0x39, PrAppKeyCode_Space}, {0x3A, PrAppKeyCode_CapsLock}, {0x3B, PrAppKeyCode_F1}, {0x3C, PrAppKeyCode_F2},
    {0x3D, PrAppKeyCode_F3}, {0x3E, PrAppKeyCode_F4}, {0x3F, PrAppKeyCode_F5}, {0x40, PrAppKeyCode_F6},
    {0x41, PrAppKeyCode_F7}, {0x42, PrAppKeyCode_F8}, {0x43, PrAppKeyCode_F9}, {0x44, PrAppKeyCode_F10},
    {0x45, PrAppKeyCode_Pause}, {0x46, PrAppKeyCode_ScrollLock}, {0x47, PrAppKeyCode_Numpad7},
    {0x48, PrAppKeyCode_Numpad8}, {0x49, PrAppKeyCode_Numpad9}, {0x4A, PrAppKeyCode_NumpadSubtract},
    {0x4B, PrAppKeyCode_Numpad4}, {0x4C, PrAppKeyCode_Numpad5}, {0x4D, PrAppKeyCode_Numpad6},
    {0x4E, PrAppKeyCode_NumpadAdd}, {0x4F, PrAppKeyCode_Numpad1}, {0x50, PrAppKeyCode_Numpad2},
    {0x51, PrAppKeyCode_Numpad3}, {0x52, PrAppKeyCode_Numpad0}, {0x53, PrAppKeyCode_NumpadDecimal},
    {0x54, PrAppKeyCode_PrintScreen}, {0x56, PrAppKeyCode_IntlBackslash}, {0x57, PrAppKeyCode_F11},
    {0x58, PrAppKeyCode_F12}, {0x59, PrAppKeyCode_NumpadEqual}, {0x64, PrAppKeyCode_F13}, {0x65, PrAppKeyCode_F14},
    {0x66, PrAppKeyCode_F15}, {0x67, PrAppKeyCode_F16}, {0x68, PrAppKeyCode_F17}, {0x69, PrAppKeyCode_F18},
    {0x6A, PrAppKeyCode_F19}, {0x6B, PrAppKeyCode_F20}, {0x71, PrAppKeyCode_Lang2}, {0x72, PrAppKeyCode_Lang1},
    {0x73, PrAppKeyCode_IntlRo}, {0x7D, PrAppKeyCode_IntlYen}, {0x7E, PrAppKeyCode_NumpadComma},
    {0xE01C, PrAppKeyCode_NumpadEnter}, {0xE01D, PrAppKeyCode_ControlRight}, {0xE035, PrAppKeyCode_NumpadDivide},
    {0xE037, PrAppKeyCode_PrintScreen}, {0xE038, PrAppKeyCode_AltRight}, {0xE045, PrAppKeyCode_NumLock},
    {0xE046, PrAppKeyCode_Pause}, {0xE047, PrAppKeyCode_Home}, {0xE048, PrAppKeyCode_ArrowUp},
    {0xE049, PrAppKeyCode_PageUp}, {0xE04B, PrAppKeyCode_ArrowLeft}, {0xE04D, PrAppKeyCode_ArrowRight},
    {0xE04F, PrAppKeyCode_End}, {0xE050, PrAppKeyCode_ArrowDown}, {0xE051, PrAppKeyCode_PageDown},
    {0xE052, PrAppKeyCode_Insert}, {0xE053, PrAppKeyCode_Delete}, {0xE05B, PrAppKeyCode_MetaLeft},
    {0xE05C, PrAppKeyCode_MetaRight}, {0xE05D, PrAppKeyCode_ContextMenu},
};

PrAppKeyCode codeOf(uint16_t scan) {
    for (const ScanKey& key : kScanKeys) {
        if (key.scan == scan)
            return key.code;
    }
    return PrAppKeyCode_Unidentified;
}

// The scan code of a key message, as kScanKeys numbers it. A message that carries none, as one that a program
// synthesizes may, takes the scan code that the layout gives its virtual key.
uint16_t scanOf(WPARAM virtualKey, LPARAM flags) {
    uint16_t scan = uint16_t((flags >> 16) & 0xFF);
    if (scan == 0)
        return uint16_t(MapVirtualKeyW(UINT(virtualKey), MAPVK_VK_TO_VSC_EX));
    return (flags & (LPARAM(1) << 24)) != 0 ? uint16_t(0xE000 | scan) : scan;
}

// A key that types no character, by its virtual key.
struct NamedKey {
    uint8_t virtualKey;
    PrAppKey key;
};

constexpr NamedKey kNamedKeys[] = {
    {VK_LEFT, PrAppKey_Left}, {VK_RIGHT, PrAppKey_Right}, {VK_UP, PrAppKey_Up}, {VK_DOWN, PrAppKey_Down},
    {VK_HOME, PrAppKey_Home}, {VK_END, PrAppKey_End}, {VK_PRIOR, PrAppKey_PageUp}, {VK_NEXT, PrAppKey_PageDown},
    {VK_RETURN, PrAppKey_Enter}, {VK_ESCAPE, PrAppKey_Escape}, {VK_BACK, PrAppKey_Backspace},
    {VK_DELETE, PrAppKey_Delete}, {VK_TAB, PrAppKey_Tab}, {VK_INSERT, PrAppKey_Insert}, {VK_SHIFT, PrAppKey_Shift},
    {VK_LSHIFT, PrAppKey_Shift}, {VK_RSHIFT, PrAppKey_Shift}, {VK_CONTROL, PrAppKey_Control},
    {VK_LCONTROL, PrAppKey_Control}, {VK_RCONTROL, PrAppKey_Control}, {VK_MENU, PrAppKey_Alt},
    {VK_LMENU, PrAppKey_Alt}, {VK_RMENU, PrAppKey_Alt}, {VK_LWIN, PrAppKey_Meta}, {VK_RWIN, PrAppKey_Meta},
    {VK_CAPITAL, PrAppKey_CapsLock}, {VK_NUMLOCK, PrAppKey_NumLock}, {VK_APPS, PrAppKey_ContextMenu},
    {VK_HELP, PrAppKey_Help}, {VK_CLEAR, PrAppKey_Clear},
};

PrAppKey namedKeyOf(WPARAM virtualKey) {
    if (virtualKey >= VK_F1 && virtualKey <= VK_F20)
        return PrAppKey(PrAppKey_F1 + (virtualKey - VK_F1));
    for (const NamedKey& named : kNamedKeys) {
        if (named.virtualKey == virtualKey)
            return named.key;
    }
    return PrAppKey_Unknown;
}

// Returns true if `count` UTF-16 units begin with a character that prints: not a control character or delete.
bool isPrintable(const wchar_t* units, int count) {
    return count > 0 && units[0] >= 32 && units[0] != 127 && (units[0] < 0x80 || units[0] > 0x9F);
}

// Returns what a key types with Shift and AltGr applied, and Control and Alt not. Windows reports AltGr as Control with
// the right Alt key. A layout that has no AltGr types nothing with that pair, and then the key types what it types
// without them. The keyboard's dead-key state is left as it was.
KeyText translateKey(UINT message, WPARAM virtualKey, LPARAM flags) {
    KeyText text{};
    text.message = message;
    text.virtualKey = virtualKey;
    text.flags = flags;
    BYTE state[256];
    if (!GetKeyboardState(state))
        return text;
    UINT scan = UINT((flags >> 16) & 0xFF);
    HKL layout = GetKeyboardLayout(0);
    if ((state[VK_RMENU] & 0x80) != 0) {
        text.count = ToUnicodeEx(UINT(virtualKey), scan, state, text.units, int(countOf(text.units)),
                                 kToUnicodeKeepState, layout);
        if (text.count < 0 || isPrintable(text.units, text.count)) {
            text.dead = text.count < 0;
            text.count = text.dead ? 0 : text.count;
            return text;
        }
    }
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
    text.count = ToUnicodeEx(UINT(virtualKey), scan, state, text.units, int(countOf(text.units)), kToUnicodeKeepState,
                             layout);
    text.dead = text.count < 0;
    if (text.dead || !isPrintable(text.units, text.count))
        text.count = 0;
    return text;
}

// Calls the program's event callback, as no other callback runs.
void callEvent(Win32App* app, const PrAppEvent& event) {
    app->inCallback = true;
    app->handler.event(app->handler.context, app, &event);
    app->inCallback = false;
}

// Ends a callback, and delivers the events that arrived during it, and those that arrive during their own delivery, in
// order. The events of a run that stopped are dropped.
void endCallback(Win32App* app) {
    app->inCallback = false;
    for (uint32_t i = 0; i < app->held.count && app->running; ++i) {
        // An event that its delivery holds goes to the end of the array, which may move the array but not the text.
        PrAppEvent event = app->held.data[i].event;
        event.text = PrStringView{app->held.data[i].text.data != nullptr ? app->held.data[i].text.data : "",
                                  app->held.data[i].text.count};
        callEvent(app, event);
    }
    for (uint32_t i = 0; i < app->held.count; ++i)
        app->held.data[i].text.reset();
    app->held.clear();
}

// Delivers an event, or holds it until the running callback returns.
void deliver(Win32App* app, const PrAppEvent& event) {
    if (!app->running)
        return;
    if (app->inCallback) {
        HeldEvent held{.event = event};
        if (event.text.size != 0)
            held.text.append(event.text.data, uint32_t(event.text.size));
        app->held.push(held);
        return;
    }
    callEvent(app, event);
    endCallback(app);
}

// ---------------------------------------------------------------------------------------------------------------------
// Displays and preferences

// The DXGI output that shows `monitor`, or null. The caller releases it.
IDXGIOutput6* PR_NULLABLE outputOf(IDXGIFactory1* factory, HMONITOR monitor) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; SUCCEEDED(factory->EnumAdapters1(a, &adapter)); ++a) {
        IDXGIOutput* output = nullptr;
        IDXGIOutput6* found = nullptr;
        for (UINT o = 0; found == nullptr && SUCCEEDED(adapter->EnumOutputs(o, &output)); ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&found));
            output->Release();
        }
        adapter->Release();
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

// The luminance of SDR white in nits on `monitor`, which the user sets for a display that shows HDR.
float sdrWhiteNits(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (!GetMonitorInfoW(monitor, &info)
        || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return kDefaultSdrWhiteNits;
    Array<DISPLAYCONFIG_PATH_INFO> paths;
    Array<DISPLAYCONFIG_MODE_INFO> modes;
    paths.resize(pathCount);
    modes.resize(modeCount);
    float nits = kDefaultSdrWhiteNits;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data, &modeCount, modes.data, nullptr)
        == ERROR_SUCCESS) {
        for (uint32_t i = 0; i < pathCount; ++i) {
            const DISPLAYCONFIG_PATH_INFO& path = paths.data[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
                || CompareStringOrdinal(source.viewGdiDeviceName, -1, info.szDevice, -1, TRUE) != CSTR_EQUAL)
                continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            white.header.size = sizeof(white);
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id = path.targetInfo.id;
            // The level is in thousandths of 80 nits.
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
                nits = float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
            break;
        }
    }
    paths.reset();
    modes.reset();
    return nits;
}

// The chromaticities of an RGB space's primaries, red, green then blue, as CIE xy.
struct Primaries {
    PrColorSpace space;
    float xy[6];
};

constexpr Primaries kGamuts[] = {
    {PrColorSpace_Srgb, {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f}},
    {PrColorSpace_DisplayP3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f}},
    {PrColorSpace_Rec2020, {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f}},
};

// The RGB space whose primaries are nearest the display's.
PrColorSpace gamutOf(const DXGI_OUTPUT_DESC1& desc) {
    const float display[6] = {desc.RedPrimary[0],   desc.RedPrimary[1],  desc.GreenPrimary[0],
                              desc.GreenPrimary[1], desc.BluePrimary[0], desc.BluePrimary[1]};
    PrColorSpace nearest = PrColorSpace_Srgb;
    float nearestDistance = 0;
    for (uint32_t g = 0; g < countOf(kGamuts); ++g) {
        float distance = 0;
        for (uint32_t i = 0; i < 6; ++i)
            distance += (display[i] - kGamuts[g].xy[i]) * (display[i] - kGamuts[g].xy[i]);
        if (g == 0 || distance < nearestDistance) {
            nearest = kGamuts[g].space;
            nearestDistance = distance;
        }
    }
    return nearest;
}

void releaseDisplayObjects(Win32App* app) {
    if (app->output != nullptr)
        app->output->Release();
    if (app->factory != nullptr)
        app->factory->Release();
    app->output = nullptr;
    app->factory = nullptr;
    app->monitor = nullptr;
}

// Reads the headroom and gamut of the display that shows the window again, if the window moved to another display, the
// displays changed, or the last read is older than kDisplayRefreshSeconds. DXGI describes an output as it was when its
// factory was created, so a stale factory is replaced. A display shows more than SDR only while Windows composes it in
// HDR, and then its headroom is its peak luminance over SDR white. In SDR, Windows composes in sRGB.
void refreshDisplay(Win32App* app, double now) {
    HMONITOR monitor = MonitorFromWindow(app->window, MONITOR_DEFAULTTONEAREST);
    if (monitor == app->monitor && now - app->displayRead < kDisplayRefreshSeconds)
        return;
    app->displayRead = now;
    if (app->factory != nullptr && !app->factory->IsCurrent())
        releaseDisplayObjects(app);
    if (monitor != app->monitor) {
        if (app->output != nullptr)
            app->output->Release();
        app->output = nullptr;
        if (app->factory == nullptr
            && FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&app->factory))))
            app->factory = nullptr;
        if (app->factory != nullptr)
            app->output = outputOf(app->factory, monitor);
        app->monitor = monitor;
    }
    PrAppDisplay& display = app->display;
    display = prAppDisplayDefault();
    DXGI_OUTPUT_DESC1 desc;
    if (app->output != nullptr && SUCCEEDED(app->output->GetDesc1(&desc))
        && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        float headroom = desc.MaxLuminance / sdrWhiteNits(monitor);
        display.headroom = headroom > 1 ? headroom : 1;
        display.potentialHeadroom = display.headroom;
        display.gamut = gamutOf(desc);
    }
}

DWORD personalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size)
        != ERROR_SUCCESS)
        return fallback;
    return value;
}

// Reads the user's preferences, and matches the window's title bar to the color scheme. A contrast theme is a palette
// of the user's, and its scheme is the one that its window background belongs to.
void refreshPreferences(Win32App* app) {
    PrAppUserPreferences& preferences = app->preferences;
    preferences = PrAppUserPreferences{};
    if (personalizeValue(L"AppsUseLightTheme", 1) == 0)
        preferences.colorScheme = PrAppColorScheme_Dark;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        preferences.contrast = PrAppContrast_More;
        COLORREF background = GetSysColor(COLOR_WINDOW);
        uint32_t luma = 2126u * GetRValue(background) + 7152u * GetGValue(background) + 722u * GetBValue(background);
        preferences.colorScheme = luma < 10000u * 128u ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
    }
    BOOL animation = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0))
        preferences.reducedMotion = !animation;
    preferences.reducedTransparency = personalizeValue(L"EnableTransparency", 1) == 0;
    BOOL dark = preferences.colorScheme == PrAppColorScheme_Dark;
    DwmSetWindowAttribute(app->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

// The window's client extent in logical pixels.
PrVec2 extentOf(Win32App* app) {
    RECT client;
    if (!GetClientRect(app->window, &client))
        return PrVec2{0, 0};
    return PrVec2{float(client.right) * 96.0f / float(app->dpi), float(client.bottom) * 96.0f / float(app->dpi)};
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

// The pacer: each time a frame is requested, it waits for the display's refresh and posts kMessageFrame to the window.
// Waiting on a thread of its own keeps the message loop free for input, and the message reaches the window also during
// a modal loop, which dispatches posted messages as the main loop does.
void pace(void* PR_NULLABLE context) {
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
        PostMessageW(app->window, kMessageFrame, 0, 0);
    }
    if (timer != nullptr)
        CloseHandle(timer);
}

// Arranges for a frame to run. A window that is hidden or minimized is not composed, so its frames are timed instead of
// paced to the display.
void scheduleFrame(Win32App* app) {
    if (!IsWindowVisible(app->window) || IsIconic(app->window))
        SetTimer(app->window, kTimerFrame, kHiddenFrameMilliseconds, nullptr);
    else
        SetEvent(app->pacerRequest);
}

void runFrame(Win32App* app) {
    if (!app->running)
        return;
    if (app->inCallback) {
        // A callback that pumps messages, as some system calls do, cannot run a frame within it, so the frame waits.
        app->frameRequested = true;
        scheduleFrame(app);
        return;
    }
    app->frameRequested = false;
    app->frameDue = false;
    double now = secondsNow();
    PrAppFrame frame{};
    frame.time = now;
    frame.delta = app->framed ? now - app->lastFrame : 0;
    frame.extent = extentOf(app);
    refreshDisplay(app, now);
    frame.display = app->display;
    frame.display.pixelRatio = float(app->dpi) / 96.0f;
    frame.preferences = app->preferences;
    app->framed = true;
    app->lastFrame = now;
    app->inCallback = true;
    app->handler.frame(app->handler.context, app, &frame);
    endCallback(app);
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

PrVec2 positionOf(Win32App* app, int x, int y) {
    return PrVec2{float(x) * 96.0f / float(app->dpi), float(y) * 96.0f / float(app->dpi)};
}

// Returns true if the key state of a pointer message has a button down.
bool anyButtonDown(WPARAM keyState) {
    WORD buttons = LOWORD(keyState);
    return (buttons & MK_LBUTTON) != 0 || (buttons & MK_RBUTTON) != 0 || (buttons & MK_MBUTTON) != 0
           || (buttons & MK_XBUTTON1) != 0 || (buttons & MK_XBUTTON2) != 0;
}

void deliverPointer(Win32App* app, UINT message, WPARAM wParam, LPARAM lParam) {
    PrAppEvent event{};
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.position = positionOf(app, int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam)));
    switch (message) {
        case WM_MOUSEMOVE: event.kind = PrAppEventKind_PointerMove; break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: event.kind = PrAppEventKind_PointerDown; break;
        default: event.kind = PrAppEventKind_PointerUp; break;
    }
    switch (message) {
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: event.button = 1; break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: event.button = 2; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: event.button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4; break;
        default: break;
    }
    app->pointer = event.position;
    uint8_t bit = uint8_t(1u << event.button);
    if (event.kind == PrAppEventKind_PointerDown)
        app->buttons |= bit;
    else if (event.kind == PrAppEventKind_PointerUp)
        app->buttons &= uint8_t(~bit);
    // The window captures the pointer while a button is down, so that a drag reports its moves and its release outside
    // the window. The buttons are recorded first, since a release of the capture reports a loss of it.
    if (event.kind == PrAppEventKind_PointerDown && GetCapture() != app->window)
        SetCapture(app->window);
    else if (event.kind == PrAppEventKind_PointerUp && !anyButtonDown(wParam)
             && GetCapture() == app->window)
        ReleaseCapture();
    deliver(app, event);
}

// Releases the buttons that are down where the pointer last was, for a window that lost the pointer's capture, as to
// another window or a system dialog, which reports no release of its own.
void releaseButtons(Win32App* app) {
    for (uint8_t button = 0; button < kPointerButtons; ++button) {
        uint8_t bit = uint8_t(1u << button);
        if ((app->buttons & bit) == 0)
            continue;
        app->buttons &= uint8_t(~bit);
        PrAppEvent event{};
        event.kind = PrAppEventKind_PointerUp;
        event.time = secondsNow();
        event.modifiers = modifiersNow();
        event.button = button;
        event.position = app->pointer;
        deliver(app, event);
    }
}

// Releases the keys that are down, for a window that lost the keyboard, where the keys' releases go instead. A lock key
// reports its state, not its movement, and is left as it is.
void releaseKeys(Win32App* app) {
    for (uint32_t code = 0; code < countOf(app->keys); ++code) {
        HeldKey& held = app->keys[code];
        if (!held.down)
            continue;
        held.down = false;
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyUp;
        event.time = secondsNow();
        event.modifiers = modifiersNow();
        event.key = held.key;
        event.code = PrAppKeyCode(code);
        event.text = "";
        deliver(app, event);
    }
}

// Delivers a wheel's turn in lines, positive right and down, as many per notch as the user sets. A notch is
// WHEEL_DELTA, and a precise wheel or a touchpad reports parts of one. A setting of a page per notch scrolls the
// window's height.
void deliverScroll(Win32App* app, WPARAM wParam, LPARAM lParam, bool horizontal) {
    POINT point{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
    ScreenToClient(app->window, &point);
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    PrVec2 extent = extentOf(app);
    float perNotch = lines == WHEEL_PAGESCROLL ? (horizontal ? extent.x : extent.y) / kPrAppScrollLineHeight
                                               : float(lines);
    float notches = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
    PrAppEvent event{};
    event.kind = PrAppEventKind_Scroll;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.position = positionOf(app, point.x, point.y);
    event.scrollUnit = PrAppScrollUnit_Line;
    // A vertical notch away from the user is positive and scrolls up. A horizontal tilt to the right is positive.
    if (horizontal)
        event.delta.x = notches * perNotch;
    else
        event.delta.y = -notches * perNotch;
    deliver(app, event);
}

void deliverKey(Win32App* app, UINT message, WPARAM virtualKey, LPARAM flags) {
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    PrAppEvent event{};
    event.kind = down ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
    event.time = secondsNow();
    event.modifiers = modifiersNow();
    event.code = codeOf(scanOf(virtualKey, flags));
    event.repeat = down && (flags & (LPARAM(1) << 30)) != 0;
    event.key = namedKeyOf(virtualKey);
    event.text = "";
    if (virtualKey == VK_CAPITAL || virtualKey == VK_NUMLOCK) {
        // A lock key reports KeyDown as it turns on and KeyUp as it turns off, whichever way the key moves.
        if (!down || event.repeat)
            return;
        event.kind = (GetKeyState(int(virtualKey)) & 1) != 0 ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
        deliver(app, event);
        return;
    }
    // An input method takes the key, which types nothing itself.
    if (virtualKey == VK_PROCESSKEY) {
        event.key = PrAppKey_Unknown;
        deliver(app, event);
        return;
    }
    if (event.key == PrAppKey_Unknown) {
        const KeyText& stashed = app->keyText;
        KeyText text = stashed.message == message && stashed.virtualKey == virtualKey && stashed.flags == flags
                           ? stashed
                           : translateKey(message, virtualKey, flags);
        if (text.dead) {
            event.key = PrAppKey_Dead;
        } else if (text.count != 0) {
            event.text = win32Narrow(text.units, text.count, app->utf8);
            wchar_t first = text.units[0];
            if (first >= L'a' && first <= L'z')
                first = wchar_t(first - L'a' + L'A');
            event.key = first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
        }
    }
    app->keyText = KeyText{};
    if (event.code != PrAppKeyCode_Unidentified)
        app->keys[event.code] = HeldKey{.down = down, .key = event.key};
    deliver(app, event);
}

void deliverText(Win32App* app, PrAppEventKind kind, PrStringView text, PrAppTextRange selection) {
    PrAppEvent event{};
    event.kind = kind;
    event.time = secondsNow();
    event.text = text;
    event.selection = selection;
    deliver(app, event);
}

// Delivers a character that WM_CHAR types. A character beyond the BMP arrives as two messages, one per surrogate. A
// control character types nothing: the program reads its key.
void deliverCharacter(Win32App* app, wchar_t unit) {
    if (IS_HIGH_SURROGATE(unit)) {
        app->highSurrogate = unit;
        return;
    }
    wchar_t units[2] = {unit, 0};
    int count = 1;
    if (IS_LOW_SURROGATE(unit)) {
        if (app->highSurrogate == 0)
            return;
        units[0] = app->highSurrogate;
        units[1] = unit;
        count = 2;
    }
    app->highSurrogate = 0;
    if (!isPrintable(units, count))
        return;
    deliverText(app, PrAppEventKind_Text, win32Narrow(units, count, app->utf8), PrAppTextRange{});
}

void deliverCodePoint(Win32App* app, uint32_t codePoint) {
    wchar_t units[2];
    int count = 1;
    if (codePoint >= 0x10000) {
        units[0] = wchar_t(0xD800 + ((codePoint - 0x10000) >> 10));
        units[1] = wchar_t(0xDC00 + ((codePoint - 0x10000) & 0x3FF));
        count = 2;
    } else {
        units[0] = wchar_t(codePoint);
    }
    if (isPrintable(units, count))
        deliverText(app, PrAppEventKind_Text, win32Narrow(units, count, app->utf8), PrAppTextRange{});
}

// Places the input method's windows at the window's bottom left.
void placeInputMethod(Win32App* app) {
    HIMC context = ImmGetContext(app->window);
    if (context == nullptr)
        return;
    RECT client;
    GetClientRect(app->window, &client);
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = POINT{0, client.bottom};
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = POINT{0, client.bottom};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(app->window, context);
}

// Reads one of the composition's strings into the scratch storage and returns its length in UTF-16 units.
int compositionString(Win32App* app, HIMC context, DWORD index) {
    LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes <= 0)
        return 0;
    app->utf16.resize(uint32_t(bytes) / sizeof(wchar_t) + 1);
    ImmGetCompositionStringW(context, index, app->utf16.data, DWORD(bytes));
    return int(uint32_t(bytes) / sizeof(wchar_t));
}

// The part of the composition that the input method converts, from its clause attributes, or the caret if no clause is
// a target. The range is in UTF-16 units.
void compositionSelection(HIMC context, int count, int* start, int* end) {
    Array<uint8_t> attributes;
    LONG size = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
    *start = -1;
    *end = -1;
    if (size > 0) {
        attributes.resize(uint32_t(size));
        ImmGetCompositionStringW(context, GCS_COMPATTR, attributes.data, DWORD(size));
        for (int i = 0; i < count && i < size; ++i) {
            uint8_t attribute = attributes.data[i];
            if (attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED) {
                if (*start < 0)
                    *start = i;
                *end = i + 1;
            }
        }
    }
    attributes.reset();
    if (*start < 0) {
        LONG caret = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        *start = caret >= 0 && caret <= count ? int(caret) : count;
        *end = *start;
    }
}

// Delivers a change to the composition. A result ends the composition and follows as a Text event, and a new
// composition may begin in the same message.
void deliverComposition(Win32App* app, LPARAM flags) {
    HIMC context = ImmGetContext(app->window);
    if (context == nullptr)
        return;
    if ((flags & GCS_RESULTSTR) != 0) {
        int count = compositionString(app, context, GCS_RESULTSTR);
        PrStringView text = win32Narrow(app->utf16.data, count, app->utf8);
        if (app->composing) {
            app->composing = false;
            deliverText(app, PrAppEventKind_CompositionEnd, text, PrAppTextRange{});
        }
        if (text.size != 0)
            deliverText(app, PrAppEventKind_Text, text, PrAppTextRange{});
    }
    if ((flags & GCS_COMPSTR) != 0) {
        int count = compositionString(app, context, GCS_COMPSTR);
        if (count == 0) {
            if (app->composing) {
                app->composing = false;
                deliverText(app, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
        } else {
            int start = 0;
            int end = 0;
            compositionSelection(context, count, &start, &end);
            PrAppTextRange selection{win32Utf8Length(app->utf16.data, start), win32Utf8Length(app->utf16.data, end)};
            app->composing = true;
            deliverText(app, PrAppEventKind_CompositionUpdate, win32Narrow(app->utf16.data, count, app->utf8),
                        selection);
        }
    }
    ImmReleaseContext(app->window, context);
}

// Opens the clipboard. Another process may hold it for a moment, so the open is tried a few times.
bool openClipboard(HWND window) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window))
            return true;
        Sleep(1);
    }
    return false;
}

void deliverClipboard(Win32App* app) {
    PrStringView text = "";
    if (openClipboard(app->window)) {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        const wchar_t* units = data != nullptr ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
        if (units != nullptr) {
            // The text ends at its NUL, or at the end of its memory if another program wrote none.
            size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            size_t count = 0;
            while (count < capacity && units[count] != 0)
                ++count;
            text = win32Narrow(units, int(count < size_t(INT32_MAX) ? count : size_t(INT32_MAX)), app->utf8);
            GlobalUnlock(data);
        }
        CloseClipboard();
    }
    PrAppEvent event{};
    event.kind = PrAppEventKind_Clipboard;
    event.time = secondsNow();
    event.text = text;
    deliver(app, event);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    Win32App* app = appOf(window);
    if (app == nullptr)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CLOSE:
            app->stop(app);
            return 0;
        case WM_ERASEBKGND:
            // The frames fill the window, and an erase would flash between them.
            return 1;
        case WM_SIZE:
            // A frame at each step of a resize presents at the window's new size before the compositor shows it.
            if (wParam != SIZE_MINIMIZED)
                runFrame(app);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            if (app->frameRequested && !app->inSizeMove)
                runFrame(app);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            // The system's least width fits the title bar's buttons, and the window may be as small as its frame.
            RECT frame{0, 0, 1, 1};
            AdjustWindowRectExForDpi(&frame, styleOf(app), FALSE, 0, app->dpi != 0 ? app->dpi : 96);
            MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = POINT{frame.right - frame.left, frame.bottom - frame.top};
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            app->inSizeMove = true;
            break;
        case WM_EXITSIZEMOVE:
            app->inSizeMove = false;
            break;
        case WM_MOVE:
            if (app->running && MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != app->monitor)
                app->requestFrame(app);
            break;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            app->requestFrame(app);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            releaseDisplayObjects(app);
            app->requestFrame(app);
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            refreshPreferences(app);
            app->requestFrame(app);
            break;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(app->cursor);
                return TRUE;
            }
            break;
        case WM_SYSCOMMAND:
            // Alt or F10 alone would enter the menu loop of a window that has no menu, which takes the next key.
            if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0)
                return 0;
            break;
        case WM_TIMER:
            if (wParam != kTimerFrame)
                break;
            KillTimer(window, kTimerFrame);
            if (app->frameRequested)
                runFrame(app);
            return 0;
        case kMessageFrame:
            // The run's loop holds the pacer's message back, so another loop dispatched it: the system's modal loop for
            // a move or a resize. A frame that waits behind input goes to the frame timer, whose message comes only
            // when no other waits, so that the loop sees the input, such as the button's release that ends a resize,
            // even where a frame takes longer than a refresh.
            if (!app->frameRequested)
                return 0;
            if (HIWORD(GetQueueStatus(QS_INPUT)) != 0)
                SetTimer(window, kTimerFrame, USER_TIMER_MINIMUM, nullptr);
            else
                runFrame(app);
            return 0;
        case kMessageWake: {
            app->wakePending.store(false);
            PrAppEvent event{};
            event.kind = PrAppEventKind_Wake;
            event.time = secondsNow();
            deliver(app, event);
            return 0;
        }
        case kMessageClipboard:
            deliverClipboard(app);
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != window)
                releaseButtons(app);
            return 0;
        case WM_KILLFOCUS:
            releaseKeys(app);
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            deliverPointer(app, message, wParam, lParam);
            return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            deliverPointer(app, message, wParam, lParam);
            return TRUE;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            deliverScroll(app, wParam, lParam, message == WM_MOUSEHWHEEL);
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
            deliverKey(app, message, wParam, lParam);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // The system keeps its own chords, such as Alt+F4.
            deliverKey(app, message, wParam, lParam);
            break;
        case WM_CHAR:
            deliverCharacter(app, wchar_t(wParam));
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
            deliverCodePoint(app, uint32_t(wParam));
            return 0;
        case WM_IME_SETCONTEXT:
            // The program shows the composition, and the input method shows only its candidates.
            lParam &= ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW);
            break;
        case WM_IME_STARTCOMPOSITION:
            placeInputMethod(app);
            return 0;
        case WM_IME_COMPOSITION:
            deliverComposition(app, lParam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            if (app->composing) {
                app->composing = false;
                deliverText(app, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// One of the system's cursors, by its IDC_ name, which is an integer whatever character type the name is spelled in.
template <typename Name>
HCURSOR systemCursor(Name name) {
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(name)));
}

void registerClass(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kClassName, &existing))
        return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(IDI_APPLICATION)));
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);
}

// Sizes the window's client area to the extent that the application opens at. A window that is shown is clamped to its
// display's work area and centered on it.
void placeWindow(Win32App* app) {
    DWORD style = styleOf(app);
    RECT frame{0, 0, MulDiv(int(app->extent.x), int(app->dpi), 96), MulDiv(int(app->extent.y), int(app->dpi), 96)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, app->dpi);
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    int x = 0;
    int y = 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (app->headless || !GetMonitorInfoW(MonitorFromWindow(app->window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        flags |= SWP_NOMOVE;
    } else {
        RECT work = monitor.rcWork;
        width = width < work.right - work.left ? width : work.right - work.left;
        height = height < work.bottom - work.top ? height : work.bottom - work.top;
        x = work.left + (work.right - work.left - width) / 2;
        y = work.top + (work.bottom - work.top - height) / 2;
    }
    SetWindowPos(app->window, nullptr, x, y, width, height, flags);
}

void destroy(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    app->held.reset();
    app->utf8.reset();
    app->utf16.reset();
    prMemoryFree(app->title);
    prMemoryFree(app);
}

void run(PrApp* base, const PrAppHandler* handler) {
    Win32App* app = static_cast<Win32App*>(base);
    app->handler = *handler;
    // The shell, which opens URLs, and the input methods expect a single-threaded apartment on the window's thread. A
    // thread that already joined another apartment keeps it.
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClass(instance);
    app->window = CreateWindowExW(0, kClassName, app->title, styleOf(app), CW_USEDEFAULT, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, app);
    if (app->window == nullptr)
        PR_FATAL("the application's window could not be created");
    app->dpi = GetDpiForWindow(app->window);
    if (app->dpi == 0)
        app->dpi = 96;
    placeWindow(app);
    if (app->cursor == nullptr)
        app->cursor = systemCursor(IDC_ARROW);
    refreshPreferences(app);
    app->display = prAppDisplayDefault();
    app->displayRead = 0;
    app->pacerRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    app->pacerQuit.store(false);
    prThreadStart(&app->pacer, pace, app);
    app->wakePending.store(false);
    app->wakeWindow.store(app->window);
    if (!app->headless) {
        ShowWindow(app->window, SW_SHOW);
        SetForegroundWindow(app->window);
    }
    app->running = true;
    app->framed = false;
    app->inCallback = true;
    app->handler.start(app->handler.context, app);
    endCallback(app);
    app->requestFrame(app);
    // A frame runs when nothing else waits in the queue, as WM_PAINT does, since a posted message comes ahead of input:
    // a frame that takes longer than a refresh would otherwise find the next frame's message waiting each time it
    // returned, and the window would take no input. The pacer's message only marks the frame due here. A modal loop,
    // such as a move's or a resize's, dispatches it to the window, which runs the frame at once.
    while (app->running) {
        MSG message;
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (app->frameDue && app->frameRequested)
                runFrame(app);
            else
                WaitMessage();
            continue;
        }
        if (message.message == WM_QUIT) {
            app->stop(app);
            break;
        }
        if (message.message == kMessageFrame && message.hwnd == app->window) {
            app->frameDue = true;
            continue;
        }
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)
            app->keyText = translateKey(message.message, message.wParam, message.lParam);
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app->inCallback = true;
    app->handler.shutdown(app->handler.context, app);
    endCallback(app);
    app->wakeWindow.store(nullptr);
    app->pacerQuit.store(true);
    SetEvent(app->pacerRequest);
    prThreadJoin(&app->pacer);
    CloseHandle(app->pacerRequest);
    app->pacerRequest = nullptr;
    KillTimer(app->window, kTimerFrame);
    if (GetCapture() == app->window)
        ReleaseCapture();
    releaseDisplayObjects(app);
    SetWindowLongPtrW(app->window, GWLP_USERDATA, 0);
    DestroyWindow(app->window);
    app->window = nullptr;
    app->frameRequested = false;
    app->frameDue = false;
    app->composing = false;
    app->highSurrogate = 0;
    app->buttons = 0;
    memZero(app->keys, sizeof(app->keys));
    if (com)
        CoUninitialize();
}

void stop(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return;
    app->running = false;
    KillTimer(app->window, kTimerFrame);
    // The system's modal loop for a move or a resize ends when it is canceled, and the main loop then sees the stop.
    if (app->inSizeMove)
        PostMessageW(app->window, WM_CANCELMODE, 0, 0);
    PostMessageW(app->window, WM_NULL, 0, 0);
}

void wake(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->wakePending.exchange(true))
        return;
    HWND window = app->wakeWindow.load();
    if (window == nullptr || !PostMessageW(window, kMessageWake, 0, 0))
        app->wakePending.store(false);
}

void requestFrame(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running || app->frameRequested)
        return;
    app->frameRequested = true;
    scheduleFrame(app);
}

void setCursor(PrApp* base, PrAppCursor cursor) {
    Win32App* app = static_cast<Win32App*>(base);
    HCURSOR selected = systemCursor(IDC_ARROW);
    switch (cursor) {
        case PrAppCursor_Hand: selected = systemCursor(IDC_HAND); break;
        case PrAppCursor_Text: selected = systemCursor(IDC_IBEAM); break;
        case PrAppCursor_Crosshair: selected = systemCursor(IDC_CROSS); break;
        case PrAppCursor_ResizeX: selected = systemCursor(IDC_SIZEWE); break;
        case PrAppCursor_ResizeY: selected = systemCursor(IDC_SIZENS); break;
        default: break;
    }
    if (selected == app->cursor)
        return;
    app->cursor = selected;
    if (app->window == nullptr)
        return;
    // WM_SETCURSOR applies the shape on the pointer's next move, and a pointer that rests over the window takes it now.
    POINT point;
    RECT client;
    if (GetCursorPos(&point) && WindowFromPoint(point) == app->window && ScreenToClient(app->window, &point)
        && GetClientRect(app->window, &client) && PtInRect(&client, point))
        SetCursor(selected);
}

PrStatus surface(PrApp* base, PrAppSurface* out) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->window == nullptr)
        return PrStatus_InvalidState;
    out->kind = PrAppSurfaceKind_Hwnd;
    out->handle = app->window;
    return PrStatus_Ok;
}

// Returns true if `text` is an absolute URL: a scheme (RFC 3986, section 3.1), a colon, and characters that a URL holds
// unescaped. A scheme of one letter is a drive, and the text a path.
bool isUrl(PrStringView text) {
    size_t colon = 0;
    while (colon < text.size && text.data[colon] != ':')
        ++colon;
    if (colon < 2 || colon == text.size)
        return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = text.data[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!letter && (i == 0 || !other))
            return false;
    }
    for (size_t i = colon + 1; i < text.size; ++i) {
        unsigned char c = static_cast<unsigned char>(text.data[i]);
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`'
            || c == '{' || c == '|' || c == '}')
            return false;
    }
    return true;
}

PrStatus openUrl(PrApp* base, PrStringView url) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!isUrl(url))
        return PrStatus_Syntax;
    win32Widen(url, app->utf16);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", app->utf16.data, nullptr, nullptr, SW_SHOWNORMAL);
    // A result above 32 is success, as the function's older interface defines it.
    return reinterpret_cast<INT_PTR>(result) > 32 ? PrStatus_Ok : PrStatus_Io;
}

void requestClipboardText(PrApp* base) {
    Win32App* app = static_cast<Win32App*>(base);
    if (app->window != nullptr)
        PostMessageW(app->window, kMessageClipboard, 0, 0);
}

PrStatus setClipboardText(PrApp* base, PrStringView text) {
    Win32App* app = static_cast<Win32App*>(base);
    // The clipboard takes data only from an owner window, which exists during a run.
    if (app->window == nullptr)
        return PrStatus_Io;
    win32Widen(text, app->utf16);
    size_t bytes = size_t(app->utf16.count) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr)
        return PrStatus_Io;
    void* at = GlobalLock(memory);
    if (at == nullptr) {
        GlobalFree(memory);
        return PrStatus_Io;
    }
    memCopy(at, app->utf16.data, bytes);
    GlobalUnlock(memory);
    if (!openClipboard(app->window)) {
        GlobalFree(memory);
        return PrStatus_Io;
    }
    // The clipboard owns the memory once it takes it.
    bool set = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!set)
        GlobalFree(memory);
    return set ? PrStatus_Ok : PrStatus_Io;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrApp* prAppWin32Create(const PrAppDesc* desc) {
    // The call fails if the process already chose an awareness, which then stands.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Win32App* app = allocZeroed<Win32App>();
    app->destroy = destroy;
    app->run = run;
    app->stop = stop;
    app->wake = wake;
    app->requestFrame = requestFrame;
    app->setCursor = setCursor;
    app->surface = surface;
    app->openUrl = openUrl;
    app->requestClipboardText = requestClipboardText;
    app->setClipboardText = setClipboardText;
    app->browserCreate = [](PrApp* self, const PrAppBrowserDesc* browserDesc, PrAppBrowser** out) {
        return win32BrowserCreate(self, static_cast<Win32App*>(self)->window, browserDesc, out);
    };
    app->browserDestroy = win32BrowserDestroy;
    app->browserResize = win32BrowserResize;
    app->browserLoad = win32BrowserLoad;
    app->browserEvaluate = win32BrowserEvaluate;
    app->browserCapture = win32BrowserCapture;
    app->browserShow = win32BrowserShow;
    app->browserHide = win32BrowserHide;
    app->browserPrint = win32BrowserPrint;
    app->browserProcesses = win32BrowserProcesses;
    Array<wchar_t> title;
    win32Widen(desc->title, title);
    app->title = title.data;
    app->extent = desc->extent;
    app->headless = desc->headless;
    return app;
}
