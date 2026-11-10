// Input: keys, pointers, gestures, cursors, text input and global hotkeys.

#pragma once

#include "pr/app_application.h"
#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Keys

// Keys identified by meaning, not by position. A printable key is its ASCII character, uppercase for letters. The other
// keys are numbered from PrAppKey_Left. Each named key matches a key value of UI Events (uievents-key), which
// prAppKeyName returns.
enum {
    PrAppKey_Unknown = 0,
    PrAppKey_Left = 256,
    PrAppKey_Right,
    PrAppKey_Up,
    PrAppKey_Down,
    PrAppKey_Home,
    PrAppKey_End,
    PrAppKey_PageUp,
    PrAppKey_PageDown,
    PrAppKey_Enter,
    PrAppKey_Escape,
    PrAppKey_Backspace,
    PrAppKey_Delete,
    PrAppKey_Tab,
    PrAppKey_Insert,
    PrAppKey_Shift,
    PrAppKey_Control,
    PrAppKey_Alt,
    PrAppKey_Meta,
    PrAppKey_CapsLock,
    PrAppKey_NumLock,
    PrAppKey_Fn,
    PrAppKey_ContextMenu,
    PrAppKey_Help,
    PrAppKey_Clear,
    PrAppKey_Dead,              // a dead key, which the layout combines with the next key
    PrAppKey_F1,
    PrAppKey_F2,
    PrAppKey_F3,
    PrAppKey_F4,
    PrAppKey_F5,
    PrAppKey_F6,
    PrAppKey_F7,
    PrAppKey_F8,
    PrAppKey_F9,
    PrAppKey_F10,
    PrAppKey_F11,
    PrAppKey_F12,
    PrAppKey_F13,
    PrAppKey_F14,
    PrAppKey_F15,
    PrAppKey_F16,
    PrAppKey_F17,
    PrAppKey_F18,
    PrAppKey_F19,
    PrAppKey_F20,
    PrAppKey_GoBack,            // the system's back key, as on Android
};
typedef uint16_t PrAppKey;

// Keys identified by position on the keyboard, independent of the layout. The set covers a standard 104-key keyboard
// and a Mac keyboard. Each code matches a code of UI Events (uievents-code), which prAppKeyCodeName returns. A key that
// types a character is named for the character that it types on a US layout.
enum {
    PrAppKeyCode_Unidentified,
    PrAppKeyCode_Backquote,
    PrAppKeyCode_Backslash,
    PrAppKeyCode_BracketLeft,
    PrAppKeyCode_BracketRight,
    PrAppKeyCode_Comma,
    PrAppKeyCode_Digit0,
    PrAppKeyCode_Digit1,
    PrAppKeyCode_Digit2,
    PrAppKeyCode_Digit3,
    PrAppKeyCode_Digit4,
    PrAppKeyCode_Digit5,
    PrAppKeyCode_Digit6,
    PrAppKeyCode_Digit7,
    PrAppKeyCode_Digit8,
    PrAppKeyCode_Digit9,
    PrAppKeyCode_Equal,
    PrAppKeyCode_IntlBackslash,
    PrAppKeyCode_IntlRo,
    PrAppKeyCode_IntlYen,
    PrAppKeyCode_KeyA,
    PrAppKeyCode_KeyB,
    PrAppKeyCode_KeyC,
    PrAppKeyCode_KeyD,
    PrAppKeyCode_KeyE,
    PrAppKeyCode_KeyF,
    PrAppKeyCode_KeyG,
    PrAppKeyCode_KeyH,
    PrAppKeyCode_KeyI,
    PrAppKeyCode_KeyJ,
    PrAppKeyCode_KeyK,
    PrAppKeyCode_KeyL,
    PrAppKeyCode_KeyM,
    PrAppKeyCode_KeyN,
    PrAppKeyCode_KeyO,
    PrAppKeyCode_KeyP,
    PrAppKeyCode_KeyQ,
    PrAppKeyCode_KeyR,
    PrAppKeyCode_KeyS,
    PrAppKeyCode_KeyT,
    PrAppKeyCode_KeyU,
    PrAppKeyCode_KeyV,
    PrAppKeyCode_KeyW,
    PrAppKeyCode_KeyX,
    PrAppKeyCode_KeyY,
    PrAppKeyCode_KeyZ,
    PrAppKeyCode_Minus,
    PrAppKeyCode_Period,
    PrAppKeyCode_Quote,
    PrAppKeyCode_Semicolon,
    PrAppKeyCode_Slash,
    PrAppKeyCode_AltLeft,
    PrAppKeyCode_AltRight,
    PrAppKeyCode_Backspace,
    PrAppKeyCode_CapsLock,
    PrAppKeyCode_ContextMenu,
    PrAppKeyCode_ControlLeft,
    PrAppKeyCode_ControlRight,
    PrAppKeyCode_Enter,
    PrAppKeyCode_MetaLeft,
    PrAppKeyCode_MetaRight,
    PrAppKeyCode_ShiftLeft,
    PrAppKeyCode_ShiftRight,
    PrAppKeyCode_Space,
    PrAppKeyCode_Tab,
    PrAppKeyCode_Lang1,
    PrAppKeyCode_Lang2,
    PrAppKeyCode_Delete,
    PrAppKeyCode_End,
    PrAppKeyCode_Help,
    PrAppKeyCode_Home,
    PrAppKeyCode_Insert,
    PrAppKeyCode_PageDown,
    PrAppKeyCode_PageUp,
    PrAppKeyCode_ArrowDown,
    PrAppKeyCode_ArrowLeft,
    PrAppKeyCode_ArrowRight,
    PrAppKeyCode_ArrowUp,
    PrAppKeyCode_NumLock,
    PrAppKeyCode_Numpad0,
    PrAppKeyCode_Numpad1,
    PrAppKeyCode_Numpad2,
    PrAppKeyCode_Numpad3,
    PrAppKeyCode_Numpad4,
    PrAppKeyCode_Numpad5,
    PrAppKeyCode_Numpad6,
    PrAppKeyCode_Numpad7,
    PrAppKeyCode_Numpad8,
    PrAppKeyCode_Numpad9,
    PrAppKeyCode_NumpadAdd,
    PrAppKeyCode_NumpadComma,
    PrAppKeyCode_NumpadDecimal,
    PrAppKeyCode_NumpadDivide,
    PrAppKeyCode_NumpadEnter,
    PrAppKeyCode_NumpadEqual,
    PrAppKeyCode_NumpadMultiply,
    PrAppKeyCode_NumpadSubtract,
    PrAppKeyCode_Escape,
    PrAppKeyCode_Fn,
    PrAppKeyCode_F1,
    PrAppKeyCode_F2,
    PrAppKeyCode_F3,
    PrAppKeyCode_F4,
    PrAppKeyCode_F5,
    PrAppKeyCode_F6,
    PrAppKeyCode_F7,
    PrAppKeyCode_F8,
    PrAppKeyCode_F9,
    PrAppKeyCode_F10,
    PrAppKeyCode_F11,
    PrAppKeyCode_F12,
    PrAppKeyCode_F13,
    PrAppKeyCode_F14,
    PrAppKeyCode_F15,
    PrAppKeyCode_F16,
    PrAppKeyCode_F17,
    PrAppKeyCode_F18,
    PrAppKeyCode_F19,
    PrAppKeyCode_F20,
    PrAppKeyCode_PrintScreen,
    PrAppKeyCode_ScrollLock,
    PrAppKeyCode_Pause,
    PrAppKeyCode_Count,
};
typedef uint8_t PrAppKeyCode;

// The modifier keys held during an event, and the lock keys that are on.
enum {
    PrAppModifierFlag_Shift = 1 << 0,
    PrAppModifierFlag_Control = 1 << 1,
    PrAppModifierFlag_Alt = 1 << 2,
    PrAppModifierFlag_Super = 1 << 3,
    PrAppModifierFlag_CapsLock = 1 << 4,
};
typedef uint8_t PrAppModifierFlags;

// Returns the name of one modifier flag, as in "Shift", or the null view if `flag` is not exactly one.
PrStringView prAppModifierName(PrAppModifierFlags flag);

// Returns the UI Events key value of a named key, such as "ArrowLeft" for PrAppKey_Left. Returns an empty view for a
// printable key and for PrAppKey_Unknown.
PrStringView prAppKeyName(PrAppKey key);

// Returns the UI Events code of a key position, such as "KeyA", or "Unidentified".
PrStringView prAppKeyCodeName(PrAppKeyCode code);

// Returns the label of the key at `code` in the current keyboard layout, for display in a keyboard shortcut. For
// example, it returns "A" for KeyQ on a French layout. A key that types no character returns its name. The view is
// valid until the layout changes, which a KeyboardLayoutChanged event reports.
PrStringView prAppKeyCodeLabel(const PrApp* app, PrAppKeyCode code);

// ---------------------------------------------------------------------------------------------------------------------
// Pointers

// The unit of a scroll's delta. A trackpad reports pixels, and a wheel that turns in steps reports lines.
enum {
    PrAppScrollUnit_Pixel,
    PrAppScrollUnit_Line,
};
typedef uint8_t PrAppScrollUnit;

// The height of a line in logical pixels, for a program that scrolls by lines and has no line height of its own.
static const float kPrAppScrollLineHeight = 40.0f;

// The kinds of pointing device, as Pointer Events names them (pointerevents3/#dom-pointerevent-pointertype).
enum {
    PrAppPointerType_Mouse,
    PrAppPointerType_Pen,
    PrAppPointerType_Touch,
};
typedef uint8_t PrAppPointerType;

// Returns the name of a type of pointer, as in "Mouse", or the null view if `type` is not one.
PrStringView prAppPointerTypeName(PrAppPointerType type);

// The pointer id of the mouse. There is only one mouse pointer.
enum { kPrAppPointerMouse = 1 };

// The button number of a pen's eraser (pointerevents3/#the-button-property). A pen's tip is the primary button, 0, and
// its barrel button is the secondary button, 1.
enum { kPrAppPointerButtonEraser = 5 };

// How a window constrains the mouse pointer (prAppWindowSetPointerMode). A window confines or locks the pointer only
// while it is active. When the window stops being active, the mode returns to Free.
enum {
    PrAppPointerMode_Free,
    PrAppPointerMode_Confined,      // the pointer stays visible and inside the window's content

    // The cursor is hidden and does not move. PointerMove events still report `movement`. Locked reports the motion
    // that the cursor would have made, with the system's acceleration. LockedRaw reports the device's own motion,
    // without acceleration.
    PrAppPointerMode_Locked,
    PrAppPointerMode_LockedRaw,
};
typedef uint8_t PrAppPointerMode;

// Returns the name of a pointer mode, as in "Confined", or the null view if `mode` is not one.
PrStringView prAppPointerModeName(PrAppPointerMode mode);

// ---------------------------------------------------------------------------------------------------------------------
// Cursors

// Cursor shapes. They are the predefined cursors of CSS (css-ui-4/#cursor), so a document's cursor maps directly to
// one. A backend shows the platform's closest cursor, or the arrow if no platform cursor is close.
enum {
    PrAppCursor_Arrow,          // default
    PrAppCursor_None,           // no cursor shows
    PrAppCursor_ContextMenu,    // a context menu is available
    PrAppCursor_Help,
    PrAppCursor_Hand,           // a link or another control that activates
    PrAppCursor_Progress,       // the program is busy, and the user can still interact with it
    PrAppCursor_Wait,           // the program is busy, and the user cannot interact with it
    PrAppCursor_Cell,           // a cell or a set of cells can be selected
    PrAppCursor_Crosshair,
    PrAppCursor_Text,
    PrAppCursor_VerticalText,
    PrAppCursor_Alias,          // a drag makes an alias or a shortcut
    PrAppCursor_Copy,           // a drag copies
    PrAppCursor_Move,           // a drag moves
    PrAppCursor_NoDrop,         // a drop is not allowed here
    PrAppCursor_NotAllowed,
    PrAppCursor_Grab,           // something can be grabbed
    PrAppCursor_Grabbing,       // something is being grabbed

    // Resizing that moves one edge or corner. A platform may show these cursors pointing outward only.
    PrAppCursor_ResizeN,
    PrAppCursor_ResizeE,
    PrAppCursor_ResizeS,
    PrAppCursor_ResizeW,
    PrAppCursor_ResizeNE,
    PrAppCursor_ResizeNW,
    PrAppCursor_ResizeSE,
    PrAppCursor_ResizeSW,

    // Resizing in both directions along an axis.
    PrAppCursor_ResizeX,        // east and west
    PrAppCursor_ResizeY,        // north and south
    PrAppCursor_ResizeNESW,
    PrAppCursor_ResizeNWSE,
    PrAppCursor_ResizeColumn,   // a divider between columns moves left or right
    PrAppCursor_ResizeRow,      // a divider between rows moves up or down

    PrAppCursor_AllScroll,      // the content scrolls in any direction
    PrAppCursor_ZoomIn,
    PrAppCursor_ZoomOut,
    PrAppCursor_Count,
};
typedef uint8_t PrAppCursor;

// Returns the name of a cursor, as in "ResizeNE", or the null view if `cursor` is not one.
PrStringView prAppCursorName(PrAppCursor cursor);

// ---------------------------------------------------------------------------------------------------------------------
// Gestures

// The gestures that the system recognizes on a trackpad or a touch screen, according to the user's settings. The fields
// of PrAppGestureEvent hold their amounts.
enum {
    PrAppGestureKind_Magnify,       // a pinch
    PrAppGestureKind_Rotate,        // a two-finger rotation
    PrAppGestureKind_SmartMagnify,  // a two-finger double tap, which toggles the zoom of the content under it
    PrAppGestureKind_Swipe,         // a swipe between pages
};
typedef uint8_t PrAppGestureKind;

// Returns the name of a kind of gesture, as in "Magnify", or the null view if `kind` is not one.
PrStringView prAppGestureKindName(PrAppGestureKind kind);

// The phase of a gesture. A gesture that the system recognizes immediately, such as SmartMagnify, has only Begin.
enum {
    PrAppGesturePhase_Begin,
    PrAppGesturePhase_Change,
    PrAppGesturePhase_End,
    PrAppGesturePhase_Cancel,
};
typedef uint8_t PrAppGesturePhase;

// Returns the name of a gesture's phase, as in "Begin", or the null view if `phase` is not one.
PrStringView prAppGesturePhaseName(PrAppGesturePhase phase);

// A touch on a trackpad. The position is in [0, 1] on each axis, from the trackpad's top left. The id is unique among
// the touches that are on the trackpad at the same time.
typedef struct PrAppTrackpadTouch {
    uint32_t id;
    PrVec2 position;
} PrAppTrackpadTouch;

// ---------------------------------------------------------------------------------------------------------------------
// Text input

// A range of a string, in bytes from its start.
typedef struct PrAppTextRange {
    uint32_t start;
    uint32_t end;
} PrAppTextRange;

// The kind of text being edited. It selects the on-screen keyboard and how the system treats the typed text. Password
// text bypasses the input method, and other processes cannot observe it while it is typed.
enum {
    PrAppTextInputKind_Text,
    PrAppTextInputKind_Password,
    PrAppTextInputKind_Number,
    PrAppTextInputKind_Decimal,
    PrAppTextInputKind_Phone,
    PrAppTextInputKind_Email,
    PrAppTextInputKind_Url,
    PrAppTextInputKind_Search,
};
typedef uint8_t PrAppTextInputKind;

// Returns the name of a kind of text input, as in "Password", or the null view if `kind` is not one.
PrStringView prAppTextInputKindName(PrAppTextInputKind kind);

// ---------------------------------------------------------------------------------------------------------------------
// Global hotkeys
//
// A global hotkey is a key combination that the application receives even when another application is active, such as
// the shortcut that opens a launcher. While the application holds the hotkey, other applications do not receive it.

// A key, by position, and the modifier keys held with it. Caps Lock is ignored.
typedef struct PrAppHotkey {
    PrAppKeyCode code;
    PrAppModifierFlags modifiers;
} PrAppHotkey;

// Registers `hotkey`. Each press delivers a Hotkey event with `tag`, until the program unregisters the hotkey or the
// run stops. Registering an existing tag replaces its key combination. Returns InvalidState if the application is not
// running, NotSupported if the backend has no global hotkeys, and NotAllowed if the system or another application holds
// the combination.
PrStatus prAppHotkeyRegister(PrApp* app, PrAppHotkey hotkey, uint64_t tag);

// Unregisters the hotkey with `tag`, if one is registered.
void prAppHotkeyUnregister(PrApp* app, uint64_t tag);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
