// The Cocoa backend's input: keys, modifiers, cursors, global hotkeys and key labels.

#pragma once

#include "app/backends/cocoa/src/app_cocoa.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the modifier or lock key whose flags changed in `event` is now down, or on.
bool cocoaModifierDown(NSEvent* event, PrAppKeyCode code);

// Describes the pen of a tablet event in `event`: its pressure, tilt and twist, and its eraser as
// kPrAppPointerButtonEraser. AppKit reports tilt in [-1, 1] on each axis with +y toward the top of the tablet, and the
// barrel's rotation in degrees counterclockwise.
void cocoaDescribePen(NSEvent* source, bool eraser, PrAppEvent* event);

// Returns the modifiers of AppKit's flags.
PrAppModifierFlags cocoaModifiersOf(NSEventModifierFlags flags);

// Returns the key of a key event, or a key whose code is Unknown for a key that the table does not name.
CocoaMacKey cocoaMacKeyOf(NSEvent* event);

// Returns the character that a key types with Shift and Option applied, and Control and Command not, or nil for a key
// that types none. A dead key types none until the key that it combines with.
NSString* PR_NULLABLE cocoaCharacterOf(NSEvent* event);

// Returns true if a key is a dead key: it types nothing itself, though it types a character without its modifiers.
bool cocoaIsDead(NSEvent* event);

// Returns the length in UTF-8 of the first `units` UTF-16 units of `string`, or zero if they are ill-formed.
uint32_t cocoaUtf8Length(NSString* string, NSUInteger units);

// Returns `text` as an NSString, or the empty string if it is not UTF-8.
NSString* cocoaStringOf(PrStringView text);

// Returns the system's nearest cursor to `cursor`. A cell shows the crosshair. macOS has no wait cursor of a program's
// own, since the system shows its own while a program does not respond, so a wait shows the busy arrow of progress.
NSCursor* cocoaCursorFor(PrAppCursor cursor);

// Implement the application's functions of their names (PrApp::hotkeyRegisterFn and the other).
void cocoaHotkeyUnregister(PrApp* app, uint64_t tag);
PrStatus cocoaHotkeyRegister(PrApp* app, PrAppHotkey hotkey, uint64_t tag);

// Unregisters every hotkey, and removes their handler, as the run ends.
void cocoaHotkeysReset(CocoaApp* app);

// Implements PrApp::keyCodeLabelFn.
PrStringView cocoaKeyCodeLabel(const PrApp* app, PrAppKeyCode code);

// Forgets the labels, for a change of the keyboard layout or the end of the application.
void cocoaKeyLabelsReset(CocoaApp* app);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
