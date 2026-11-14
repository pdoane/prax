// The Win32 backend's input: keys, text and input methods, pointers, cursors, and global hotkeys.

#include "app/backends/win32/src/app_win32_input.h"

#include "app/backends/win32/src/app_win32_window.h"

namespace pr_impl {

namespace {

bool isDown(int virtualKey) {
    return (GetKeyState(virtualKey) & 0x8000) != 0;
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

// One of the system's cursors, by its IDC_ name, which is an integer whatever character type the name is spelled in.
template <typename Name>
HCURSOR systemCursor(Name name) {
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(reinterpret_cast<ULONG_PTR>(name)));
}

// Returns true if the key state of a pointer message has a button down.
bool anyButtonDown(WPARAM keyState) {
    WORD buttons = LOWORD(keyState);
    return (buttons & MK_LBUTTON) != 0 || (buttons & MK_RBUTTON) != 0 || (buttons & MK_MBUTTON) != 0 ||
        (buttons & MK_XBUTTON1) != 0 || (buttons & MK_XBUTTON2) != 0;
}

// Returns the number of presses in the multi-click that a press of `button` at `at`, in client pixels, continues or
// starts, by the system's settings.
uint32_t countClick(Win32Window* window, uint8_t button, POINT at, double time) {
    bool continues = window->clicks != 0 && button == window->clickButton &&
        time - window->clickTime <= double(GetDoubleClickTime()) / 1000 &&
        abs(at.x - window->clickPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        abs(at.y - window->clickPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    window->clicks = continues ? window->clicks + 1 : 1;
    window->clickTime = time;
    window->clickPoint = at;
    window->clickButton = button;
    return window->clicks;
}

// Asks the system to report the pointer's leaving of the client area, or of the frame, once for each time it comes.
void trackLeave(Win32Window* window, bool frame) {
    bool& tracking = frame ? window->trackingFrame : window->trackingClient;
    if (tracking)
        return;
    TRACKMOUSEEVENT track{
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = DWORD(TME_LEAVE | (frame ? TME_NONCLIENT : 0)),
        .hwndTrack = window->window,
    };
    tracking = TrackMouseEvent(&track) != FALSE;
}

void deliverCrossing(Win32Window* window, PrAppEventKind kind, PrVec2 position) {
    PrAppEvent event{};
    event.kind = kind;
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.pointer.position = position;
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    win32Deliver(window, event);
}

// Returns true if the pointer is over the window's content: its client area, or a part of its custom title bar.
bool pointerOverContent(Win32Window* window) {
    POINT at{};
    if (!GetCursorPos(&at) || WindowFromPoint(at) != window->window)
        return false;
    LRESULT hit = SendMessageW(window->window, WM_NCHITTEST, 0, MAKELPARAM(uint16_t(at.x), uint16_t(at.y)));
    bool titleBar = hit == HTCAPTION || hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
    return hit == HTCLIENT || (window->customTitleBar && titleBar);
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
    RawArray<uint8_t> attributes;
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

// Returns the length in UTF-8 of the first `count` UTF-16 units of `units`, or zero if they are ill-formed.
uint32_t utf8Length(const wchar_t* units, int count) {
    uint32_t size = prWindowsNarrow(units, uint32_t(count), nullptr, 0);
    return size != kPrWindowsStringInvalid ? size : 0;
}

UINT virtualKeyOf(PrAppKeyCode code);

UINT hotkeyModifiersOf(PrAppModifierFlags modifiers) {
    UINT flags = MOD_NOREPEAT;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        flags |= MOD_SHIFT;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        flags |= MOD_CONTROL;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        flags |= MOD_ALT;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        flags |= MOD_WIN;
    return flags;
}

// Returns the virtual key of the key at `code` in the current layout, or zero for none.
UINT virtualKeyOf(PrAppKeyCode code) {
    for (const ScanKey& key : kScanKeys) {
        if (key.code == code)
            return MapVirtualKeyW(key.scan, MAPVK_VSC_TO_VK_EX);
    }
    return 0;
}

} // namespace

PrAppModifierFlags win32ModifiersNow() {
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

Win32KeyText win32TranslateKey(UINT message, WPARAM virtualKey, LPARAM flags) {
    Win32KeyText text{};
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
    text.count = ToUnicodeEx(UINT(virtualKey), scan, state, text.units, int(countOf(text.units)), kToUnicodeKeepState, layout);
    text.dead = text.count < 0;
    if (text.dead || !isPrintable(text.units, text.count))
        text.count = 0;
    return text;
}

HCURSOR PR_NULLABLE win32CursorFor(PrAppCursor cursor) {
    switch (cursor) {
        case PrAppCursor_None: return nullptr;
        case PrAppCursor_Help: return systemCursor(IDC_HELP);
        case PrAppCursor_Hand: return systemCursor(IDC_HAND);
        case PrAppCursor_Progress: return systemCursor(IDC_APPSTARTING);
        case PrAppCursor_Wait: return systemCursor(IDC_WAIT);
        case PrAppCursor_Cell: return systemCursor(IDC_CROSS);
        case PrAppCursor_Crosshair: return systemCursor(IDC_CROSS);
        case PrAppCursor_Text: return systemCursor(IDC_IBEAM);
        case PrAppCursor_VerticalText: return systemCursor(IDC_IBEAM);
        case PrAppCursor_Move: return systemCursor(IDC_SIZEALL);
        case PrAppCursor_NoDrop: return systemCursor(IDC_NO);
        case PrAppCursor_NotAllowed: return systemCursor(IDC_NO);
        case PrAppCursor_Grab: return systemCursor(IDC_HAND);
        case PrAppCursor_Grabbing: return systemCursor(IDC_HAND);
        case PrAppCursor_ResizeN: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeS: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeY: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeRow: return systemCursor(IDC_SIZENS);
        case PrAppCursor_ResizeE: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeW: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeX: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeColumn: return systemCursor(IDC_SIZEWE);
        case PrAppCursor_ResizeNE: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeSW: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeNESW: return systemCursor(IDC_SIZENESW);
        case PrAppCursor_ResizeNW: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_ResizeSE: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_ResizeNWSE: return systemCursor(IDC_SIZENWSE);
        case PrAppCursor_AllScroll: return systemCursor(IDC_SIZEALL);
        default: return systemCursor(IDC_ARROW);
    }
}

PrVec2 win32PositionOf(Win32Window* window, int x, int y) {
    float scale = 96.0f / float(window->dpi);
    return PrVec2{float(x) * scale, float(y) * scale};
}

void win32PointerLeft(Win32Window* window, bool frame) {
    (frame ? window->trackingFrame : window->trackingClient) = false;
    if (!window->inside || window->locked || pointerOverContent(window))
        return;
    if (window->buttons != 0) {
        window->leaving = true;
        return;
    }
    window->inside = false;
    deliverCrossing(window, PrAppEventKind_PointerLeave, window->pointer);
}

void win32SetRawInput(Win32Window* window, bool raw) {
    if (window->rawInput == raw)
        return;
    window->rawInput = raw;
    RAWINPUTDEVICE device{
        .usUsagePage = 0x01,    // generic desktop controls
        .usUsage = 0x02,        // mouse
        .dwFlags = raw ? 0u : DWORD(RIDEV_REMOVE),
        .hwndTarget = raw ? window->window : nullptr,
    };
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

void win32DeliverRawInput(Win32Window* window, LPARAM lParam) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) ==
            UINT(-1) || input.header.dwType != RIM_TYPEMOUSE)
        return;
    const RAWMOUSE& mouse = input.data.mouse;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0 || (mouse.lLastX == 0 && mouse.lLastY == 0))
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_PointerMove;
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.pointer.position = window->lockedAt;
    event.pointer.type = PrAppPointerType_Mouse;
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    event.pointer.movement = PrVec2{float(mouse.lLastX), float(mouse.lLastY)};
    win32Deliver(window, event);
}

void win32SetLock(Win32Window* window, bool locked) {
    if (window->locked == locked)
        return;
    if (locked && (appOf(window)->headless || GetForegroundWindow() != window->window))
        return;
    window->locked = locked;
    if (locked) {
        window->lockedAt = window->pointer;
        POINT at{
            MulDiv(int(window->pointer.x), int(window->dpi), 96),
            MulDiv(int(window->pointer.y), int(window->dpi), 96),
        };
        ClientToScreen(window->window, &at);
        window->lockedScreen = at;
        RECT client{};
        GetClientRect(window->window, &client);
        MapWindowPoints(window->window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        SetCursor(window->cursor);
    }
    win32SetRawInput(window, locked && window->pointerMode == PrAppPointerMode_LockedRaw);
    win32RequestFrame(window);
}

void win32DeliverPointer(Win32Window* window, UINT message, WPARAM wParam, LPARAM lParam, bool frame) {
    PrAppEvent event{};
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.pointer.position = win32PositionOf(window, int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam)));
    if (message == WM_MOUSEMOVE) {
        if (window->locked) {
            // The move that returns the cursor to the lock's place moved nothing.
            POINT at{};
            GetCursorPos(&at);
            float scale = 96.0f / float(window->dpi);
            PrVec2 movement{float(at.x - window->lockedScreen.x) * scale, float(at.y - window->lockedScreen.y) * scale};
            if (movement.x == 0 && movement.y == 0)
                return;
            SetCursorPos(window->lockedScreen.x, window->lockedScreen.y);
            // A raw lock reports the device's motion from its raw input instead.
            if (window->rawInput)
                return;
            event.pointer.position = window->lockedAt;
            event.pointer.movement = movement;
        } else if (window->inside) {
            event.pointer.movement = PrVec2{
                event.pointer.position.x - window->pointer.x,
                event.pointer.position.y - window->pointer.y,
            };
        }
    }
    trackLeave(window, frame);
    if (!window->inside && !window->locked) {
        window->inside = true;
        deliverCrossing(window, PrAppEventKind_PointerEnter, event.pointer.position);
    }
    window->leaving = window->leaving && !pointerOverContent(window);
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
        case WM_RBUTTONUP: event.pointer.button = 1; break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: event.pointer.button = 2; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: event.pointer.button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4; break;
        default: break;
    }
    window->pointer = event.pointer.position;
    uint8_t bit = uint8_t(1u << event.pointer.button);
    if (event.kind == PrAppEventKind_PointerDown) {
        window->buttons |= bit;
        POINT at{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
        event.pointer.clicks = countClick(window, event.pointer.button, at, event.time);
    }
    else if (event.kind == PrAppEventKind_PointerUp)
        window->buttons &= uint8_t(~bit);
    event.pointer.primary = true;
    event.pointer.id = kPrAppPointerMouse;
    event.pointer.pressure = window->buttons != 0 ? 0.5f : 0.0f;
    // The window captures the pointer while a button is down, so that a drag reports its moves and its release outside
    // the window. The buttons are recorded first, since a release of the capture reports a loss of it.
    if (event.kind == PrAppEventKind_PointerDown && GetCapture() != window->window)
        SetCapture(window->window);
    else if (event.kind == PrAppEventKind_PointerUp && !anyButtonDown(wParam) && GetCapture() == window->window)
        ReleaseCapture();
    win32Deliver(window, event);
    // A pointer that left during the press leaves once its last button is up.
    if (event.kind == PrAppEventKind_PointerUp && window->leaving && window->buttons == 0) {
        window->leaving = false;
        window->inside = false;
        deliverCrossing(window, PrAppEventKind_PointerLeave, window->pointer);
    }
}

bool win32DeliverPenOrTouch(Win32Window* window, UINT message, WPARAM wParam) {
    UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type) || (type != PT_PEN && type != PT_TOUCH))
        return false;
    PrAppEvent event{};
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.pointer.id = id + 1;
    POINTER_INFO info{};
    bool pressureKnown = false;
    float scale = 96.0f / float(window->dpi);
    if (type == PT_PEN) {
        POINTER_PEN_INFO pen{};
        if (!GetPointerPenInfo(id, &pen))
            return false;
        info = pen.pointerInfo;
        event.pointer.type = PrAppPointerType_Pen;
        // The system reports pressure in [0, 1024], and tilt and rotation in degrees.
        if ((pen.penMask & PEN_MASK_PRESSURE) != 0) {
            event.pointer.pressure = float(pen.pressure) / 1024.0f;
            pressureKnown = true;
        }
        if ((pen.penMask & PEN_MASK_TILT_X) != 0)
            event.pointer.tilt.x = float(pen.tiltX);
        if ((pen.penMask & PEN_MASK_TILT_Y) != 0)
            event.pointer.tilt.y = float(pen.tiltY);
        if ((pen.penMask & PEN_MASK_ROTATION) != 0)
            event.pointer.twist = float(pen.rotation % 360);
        if ((pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0)
            event.pointer.button = kPrAppPointerButtonEraser;
        else if ((pen.penFlags & PEN_FLAG_BARREL) != 0)
            event.pointer.button = 1;
    } else {
        POINTER_TOUCH_INFO touch{};
        if (!GetPointerTouchInfo(id, &touch))
            return false;
        info = touch.pointerInfo;
        event.pointer.type = PrAppPointerType_Touch;
        if ((touch.touchMask & TOUCH_MASK_PRESSURE) != 0) {
            event.pointer.pressure = float(touch.pressure) / 1024.0f;
            pressureKnown = true;
        }
        if ((touch.touchMask & TOUCH_MASK_CONTACTAREA) != 0) {
            event.pointer.contactSize = PrVec2{
                float(touch.rcContact.right - touch.rcContact.left) * scale,
                float(touch.rcContact.bottom - touch.rcContact.top) * scale,
            };
        }
    }
    bool contact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    if (!pressureKnown || !contact)
        event.pointer.pressure = contact ? 0.5f : 0.0f;
    event.pointer.primary = (info.pointerFlags & POINTER_FLAG_PRIMARY) != 0;
    POINT at = info.ptPixelLocation;
    ScreenToClient(window->window, &at);
    event.pointer.position = win32PositionOf(window, at.x, at.y);
    switch (message) {
        case WM_POINTERDOWN:
            event.kind = PrAppEventKind_PointerDown;
            event.pointer.clicks = countClick(window, event.pointer.button, at, event.time);
            break;
        case WM_POINTERUP:
            event.kind = PrAppEventKind_PointerUp;
            if ((info.pointerFlags & POINTER_FLAG_CANCELED) != 0)
                event.kind = PrAppEventKind_PointerCancel;
            break;
        case WM_POINTERCAPTURECHANGED: event.kind = PrAppEventKind_PointerCancel; break;
        default: event.kind = PrAppEventKind_PointerMove; break;
    }
    win32Deliver(window, event);
    return true;
}

void win32ReleaseButtons(Win32Window* window) {
    for (uint8_t button = 0; button < kPointerButtons; ++button) {
        uint8_t bit = uint8_t(1u << button);
        if ((window->buttons & bit) == 0)
            continue;
        window->buttons &= uint8_t(~bit);
        PrAppEvent event{};
        event.kind = PrAppEventKind_PointerUp;
        event.time = win32SecondsNow();
        event.modifiers = win32ModifiersNow();
        event.pointer.button = button;
        event.pointer.position = window->pointer;
        event.pointer.primary = true;
        event.pointer.id = kPrAppPointerMouse;
        event.pointer.pressure = window->buttons != 0 ? 0.5f : 0.0f;
        win32Deliver(window, event);
    }
}

void win32ReleaseKeys(Win32Window* window) {
    for (uint32_t code = 0; code < countOf(window->keys); ++code) {
        Win32HeldKey& held = window->keys[code];
        if (!held.down)
            continue;
        held.down = false;
        PrAppEvent event{};
        event.kind = PrAppEventKind_KeyUp;
        event.time = win32SecondsNow();
        event.modifiers = win32ModifiersNow();
        event.keyboard.key = held.key;
        event.keyboard.code = PrAppKeyCode(code);
        event.keyboard.text = "";
        win32Deliver(window, event);
    }
}

void win32DeliverScroll(Win32Window* window, WPARAM wParam, LPARAM lParam, bool horizontal) {
    POINT point{int16_t(LOWORD(lParam)), int16_t(HIWORD(lParam))};
    ScreenToClient(window->window, &point);
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    PrVec2 extent = win32ExtentOf(window);
    float perNotch =
        lines == WHEEL_PAGESCROLL ? (horizontal ? extent.x : extent.y) / kPrAppScrollLineHeight : float(lines);
    float notches = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
    PrAppEvent event{};
    event.kind = PrAppEventKind_Scroll;
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.scroll.position = win32PositionOf(window, point.x, point.y);
    event.scroll.unit = PrAppScrollUnit_Line;
    // A vertical notch away from the user is positive and scrolls up. A horizontal tilt to the right is positive.
    if (horizontal)
        event.scroll.delta.x = notches * perNotch;
    else
        event.scroll.delta.y = -notches * perNotch;
    win32Deliver(window, event);
}

void win32DeliverKey(Win32Window* window, UINT message, WPARAM virtualKey, LPARAM flags) {
    Win32App* app = appOf(window);
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    PrAppEvent event{};
    event.kind = down ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
    event.time = win32SecondsNow();
    event.modifiers = win32ModifiersNow();
    event.keyboard.code = codeOf(scanOf(virtualKey, flags));
    event.keyboard.repeat = down && (flags & (LPARAM(1) << 30)) != 0;
    event.keyboard.key = namedKeyOf(virtualKey);
    event.keyboard.text = "";
    if (virtualKey == VK_CAPITAL || virtualKey == VK_NUMLOCK) {
        // A lock key reports KeyDown as it turns on and KeyUp as it turns off, whichever way the key moves.
        if (!down || event.keyboard.repeat)
            return;
        event.kind = (GetKeyState(int(virtualKey)) & 1) != 0 ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp;
        win32Deliver(window, event);
        return;
    }
    // An input method takes the key, which types nothing itself.
    if (virtualKey == VK_PROCESSKEY) {
        event.keyboard.key = PrAppKey_Unknown;
        win32Deliver(window, event);
        return;
    }
    if (event.keyboard.key == PrAppKey_Unknown) {
        const Win32KeyText& stashed = app->keyText;
        Win32KeyText text;
        if (stashed.message == message && stashed.virtualKey == virtualKey && stashed.flags == flags)
            text = stashed;
        else
            text = win32TranslateKey(message, virtualKey, flags);
        if (text.dead) {
            event.keyboard.key = PrAppKey_Dead;
        } else if (text.count != 0) {
            event.keyboard.text = windowsNarrow(text.units, uint32_t(text.count), app->utf8);
            wchar_t first = text.units[0];
            if (first >= L'a' && first <= L'z')
                first = wchar_t(first - L'a' + L'A');
            event.keyboard.key = first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
        }
    }
    app->keyText = Win32KeyText{};
    if (event.keyboard.code != PrAppKeyCode_Unidentified)
        window->keys[event.keyboard.code] = Win32HeldKey{.down = down, .key = event.keyboard.key};
    win32Deliver(window, event);
}

void win32DeliverText(Win32Window* window, PrAppEventKind kind, PrStringView text, PrAppTextRange selection) {
    PrAppEvent event{};
    event.kind = kind;
    event.time = win32SecondsNow();
    event.input.text = text;
    event.input.selection = selection;
    win32Deliver(window, event);
}

void win32DeliverCharacter(Win32Window* window, wchar_t unit) {
    if (IS_HIGH_SURROGATE(unit)) {
        window->highSurrogate = unit;
        return;
    }
    wchar_t units[2] = {unit, 0};
    int count = 1;
    if (IS_LOW_SURROGATE(unit)) {
        if (window->highSurrogate == 0)
            return;
        units[0] = window->highSurrogate;
        units[1] = unit;
        count = 2;
    }
    window->highSurrogate = 0;
    if (!isPrintable(units, count))
        return;
    PrStringView text = windowsNarrow(units, uint32_t(count), appOf(window)->utf8);
    win32DeliverText(window, PrAppEventKind_Text, text, PrAppTextRange{});
}

void win32DeliverCodePoint(Win32Window* window, uint32_t codePoint) {
    wchar_t units[2];
    int count = 1;
    if (codePoint >= 0x10000) {
        units[0] = wchar_t(0xD800 + ((codePoint - 0x10000) >> 10));
        units[1] = wchar_t(0xDC00 + ((codePoint - 0x10000) & 0x3FF));
        count = 2;
    } else {
        units[0] = wchar_t(codePoint);
    }
    if (isPrintable(units, count)) {
        PrStringView text = windowsNarrow(units, uint32_t(count), appOf(window)->utf8);
        win32DeliverText(window, PrAppEventKind_Text, text, PrAppTextRange{});
    }
}

void win32PlaceInputMethod(Win32Window* window) {
    HIMC context = ImmGetContext(window->window);
    if (context == nullptr)
        return;
    float scale = float(GetDpiForWindow(window->window)) / USER_DEFAULT_SCREEN_DPI;
    POINT below{LONG(window->textInputRect.left * scale), LONG(window->textInputRect.bottom * scale)};
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = below;
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = below;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window->window, context);
}

void win32DeliverComposition(Win32Window* window, LPARAM flags) {
    Win32App* app = appOf(window);
    HIMC context = ImmGetContext(window->window);
    if (context == nullptr)
        return;
    if ((flags & GCS_RESULTSTR) != 0) {
        int count = compositionString(app, context, GCS_RESULTSTR);
        PrStringView text = windowsNarrow(app->utf16.data, uint32_t(count), app->utf8);
        if (window->composing) {
            window->composing = false;
            win32DeliverText(window, PrAppEventKind_CompositionEnd, text, PrAppTextRange{});
        }
        if (text.size != 0)
            win32DeliverText(window, PrAppEventKind_Text, text, PrAppTextRange{});
    }
    if ((flags & GCS_COMPSTR) != 0) {
        int count = compositionString(app, context, GCS_COMPSTR);
        if (count == 0) {
            if (window->composing) {
                window->composing = false;
                win32DeliverText(window, PrAppEventKind_CompositionEnd, "", PrAppTextRange{});
            }
        } else {
            int start = 0;
            int end = 0;
            compositionSelection(context, count, &start, &end);
            PrAppTextRange selection{utf8Length(app->utf16.data, start), utf8Length(app->utf16.data, end)};
            window->composing = true;
            PrStringView text = windowsNarrow(app->utf16.data, uint32_t(count), app->utf8);
            win32DeliverText(window, PrAppEventKind_CompositionUpdate, text, selection);
        }
    }
    ImmReleaseContext(window->window, context);
}

void win32SetPasswordInput(Win32Window* window, bool password) {
    if (password && window->inputContext == nullptr) {
        window->inputContext = ImmAssociateContext(window->window, nullptr);
    } else if (!password && window->inputContext != nullptr) {
        ImmAssociateContext(window->window, window->inputContext);
        window->inputContext = nullptr;
    }
}

void win32KeyLabelsReset(Win32App* app) {
    for (uint32_t i = 0; i < PrAppKeyCode_Count; ++i) {
        prRawStringReset(&app->keyLabels[i]);
        app->keyLabeled[i] = false;
    }
}

void win32HotkeyPressed(Win32App* app, int id) {
    for (uint32_t i = 0; i < app->hotkeys.count; ++i) {
        if (app->hotkeys.data[i].id != id)
            continue;
        PrAppEvent event{};
        event.kind = PrAppEventKind_Hotkey;
        event.time = win32SecondsNow();
        event.modifiers = win32ModifiersNow();
        event.tag = app->hotkeys.data[i].tag;
        win32Deliver(app, nullptr, event);
        return;
    }
}

PrStatus win32HotkeyRegister(PrApp* base, PrAppHotkey hotkey, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    if (!app->running)
        return PrStatus_InvalidState;
    UINT key = virtualKeyOf(hotkey.code);
    if (app->headless || key == 0)
        return PrStatus_NotSupported;
    win32HotkeyUnregister(app, tag);
    // An application's hotkey ids are in [1, 0xBFFF].
    app->lastHotkeyId = app->lastHotkeyId >= 0xBFFF ? 1 : app->lastHotkeyId + 1;
    if (!RegisterHotKey(app->messageWindow, app->lastHotkeyId, hotkeyModifiersOf(hotkey.modifiers), key))
        return PrStatus_NotAllowed;
    app->hotkeys.push(Win32Hotkey{.tag = tag, .id = app->lastHotkeyId});
    return PrStatus_Ok;
}

void win32HotkeyUnregister(PrApp* base, uint64_t tag) {
    Win32App* app = static_cast<Win32App*>(base);
    for (uint32_t i = 0; i < app->hotkeys.count; ++i) {
        if (app->hotkeys.data[i].tag == tag) {
            UnregisterHotKey(app->messageWindow, app->hotkeys.data[i].id);
            app->hotkeys.erase(i);
            return;
        }
    }
}

PrStringView win32KeyCodeLabel(const PrApp* base, PrAppKeyCode code) {
    const Win32App* app = static_cast<const Win32App*>(base);
    if (code >= PrAppKeyCode_Count)
        return prAppKeyCodeName(PrAppKeyCode_Unidentified);
    if (!app->keyLabeled[code]) {
        app->keyLabeled[code] = true;
        prRawStringReset(&app->keyLabels[code]);
        UINT virtualKey = virtualKeyOf(code);
        // The high bit marks a dead key, whose character is the accent that it adds.
        UINT character = virtualKey != 0 ? MapVirtualKeyExW(virtualKey, MAPVK_VK_TO_CHAR, GetKeyboardLayout(0)) : 0;
        character &= 0x7FFFFFFF;
        if (character > ' ' && character != 0x7F) {
            wchar_t unit = wchar_t(character);
            app->keyLabels[code] = prRawStringCopy(windowsNarrow(&unit, 1, app->utf8));
        }
    }
    if (!prRawStringIsEmpty(&app->keyLabels[code]))
        return prRawStringView(&app->keyLabels[code]);
    return prAppKeyCodeName(code);
}

void win32SetPointerMode(PrAppWindow* base, PrAppPointerMode mode) {
    Win32Window* window = static_cast<Win32Window*>(base);
    window->pointerMode = mode;
    win32SetLock(window, mode == PrAppPointerMode_Locked || mode == PrAppPointerMode_LockedRaw);
    if (mode == PrAppPointerMode_Confined && GetForegroundWindow() == window->window) {
        RECT client{};
        GetClientRect(window->window, &client);
        MapWindowPoints(window->window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
    } else if (!window->locked) {
        ClipCursor(nullptr);
    }
}

PrAppPointerMode win32PointerMode(const PrAppWindow* base) {
    const Win32Window* window = static_cast<const Win32Window*>(base);
    if (window->locked || (window->pointerMode == PrAppPointerMode_Confined && GetForegroundWindow() == window->window))
        return window->pointerMode;
    return PrAppPointerMode_Free;
}

} // namespace pr_impl
