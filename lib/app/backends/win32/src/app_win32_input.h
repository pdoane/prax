// The Win32 backend's input: keys, text and input methods, pointers, cursors, and global hotkeys.

#pragma once

#include "app/backends/win32/src/app_win32.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the modifier keys that are down now, with Caps Lock if it is on.
PrAppModifierFlags win32ModifiersNow();

// Returns what a key types with Shift and AltGr applied, and Control and Alt not. Windows reports AltGr as Control with
// the right Alt key. A layout that has no AltGr types nothing with that pair, and then the key types what it types
// without them. The keyboard's dead-key state is left as it was.
Win32KeyText win32TranslateKey(UINT message, WPARAM virtualKey, LPARAM flags);

// Returns the system's nearest cursor to `cursor`, or null for none, which hides the pointer over the window. Windows
// has no system cursors for a context menu, a cell, an alias, a copy, a grab or a zoom. A cell shows the crosshair, a
// grab shows the hand, and the others show the arrow.
HCURSOR PR_NULLABLE win32CursorFor(PrAppCursor cursor);

// Returns a point of the window's client area, in physical pixels, in the window's logical pixels.
PrVec2 win32PositionOf(Win32Window* window, int x, int y);

// Takes the system's report that the pointer left the client area or the frame. The pointer leaves the content only if
// it is over neither, and with a button down, only once the buttons are up.
void win32PointerLeft(Win32Window* window, bool frame);

// Starts or stops the mouse's raw input to the window, which reports the device's motion without the system's
// acceleration.
void win32SetRawInput(Win32Window* window, bool raw);

// Delivers the motion of the mouse's raw input as a PointerMove, in the device's own units.
void win32DeliverRawInput(Win32Window* window, LPARAM lParam);

// Locks the pointer to the window where it is, or releases it. The cursor hides, and stays within the window, where
// each move returns it to the lock's place, so that the next move reports how far it went. A lock takes effect only
// while the window is the foreground window, and never for a headless application.
void win32SetLock(Win32Window* window, bool locked);

// Delivers a mouse message of the client area, or of a custom title bar if `frame` is set, which the system reports as
// the frame's.
void win32DeliverPointer(Win32Window* window, UINT message, WPARAM wParam, LPARAM lParam, bool frame = false);

// Delivers the WM_POINTER message of a pen or a touch, and returns true. Returns false for a mouse, whose messages
// DefWindowProc turns into the mouse's own. A pen's eraser, at its end or as its button, presses
// kPrAppPointerButtonEraser, and its barrel button presses the secondary button. The pointer's id is the system's plus
// one, so that it never equals kPrAppPointerMouse.
bool win32DeliverPenOrTouch(Win32Window* window, UINT message, WPARAM wParam);

// Releases the buttons that are down where the pointer last was, for a window that lost the pointer's capture, as to
// another window or a system dialog, which reports no release of its own.
void win32ReleaseButtons(Win32Window* window);

// Releases the keys that are down, for a window that lost the keyboard, where the keys' releases go instead. A lock key
// reports its state, not its movement, and is left as it is.
void win32ReleaseKeys(Win32Window* window);

// Delivers a wheel's turn in lines, positive right and down, as many per notch as the user sets. A notch is
// WHEEL_DELTA, and a precise wheel or a touchpad reports parts of one. A setting of a page per notch scrolls the
// window's height.
void win32DeliverScroll(Win32Window* window, WPARAM wParam, LPARAM lParam, bool horizontal);

// Delivers a key message, WM_KEYDOWN or WM_KEYUP or their WM_SYS forms, as KeyDown or KeyUp.
void win32DeliverKey(Win32Window* window, UINT message, WPARAM virtualKey, LPARAM flags);

// Delivers an event of `kind` with `text`: a Text event, or a change of the composition with its selection.
void win32DeliverText(Win32Window* window, PrAppEventKind kind, PrStringView text, PrAppTextRange selection);

// Delivers a character that WM_CHAR types. A character beyond the BMP arrives as two messages, one per surrogate. A
// control character types nothing: the program reads its key.
void win32DeliverCharacter(Win32Window* window, wchar_t unit);

// Delivers a character that WM_UNICHAR types, as a Text event.
void win32DeliverCodePoint(Win32Window* window, uint32_t codePoint);

// Places the input method's windows at the caret that the program last gave, in the window's client area.
void win32PlaceInputMethod(Win32Window* window);

// Delivers a change to the composition. A result ends the composition and follows as a Text event, and a new
// composition may begin in the same message.
void win32DeliverComposition(Win32Window* window, LPARAM flags);

// A password bypasses the input method, which the window sets aside while one is edited.
void win32SetPasswordInput(Win32Window* window, bool password);

// Forgets the labels of the keys, which the next query computes again for the current layout.
void win32KeyLabelsReset(Win32App* app);

// Delivers the press of the hotkey whose id is `id`.
void win32HotkeyPressed(Win32App* app, int id);

// Implements prAppHotkeyRegister.
PrStatus win32HotkeyRegister(PrApp* base, PrAppHotkey hotkey, uint64_t tag);

// Implements prAppHotkeyUnregister.
void win32HotkeyUnregister(PrApp* base, uint64_t tag);

// Returns the label of the key at `code`: the character that it types in the current layout, which MapVirtualKey gives
// uppercase, or the key's name for a key that types none.
PrStringView win32KeyCodeLabel(const PrApp* base, PrAppKeyCode code);

// The window locks the pointer for Locked and LockedRaw alike, and confines it within its client area for Confined.
void win32SetPointerMode(PrAppWindow* base, PrAppPointerMode mode);

// Implements prAppWindowPointerMode.
PrAppPointerMode win32PointerMode(const PrAppWindow* base);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
