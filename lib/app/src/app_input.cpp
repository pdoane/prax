// Input: the names of keys, their labels, and global hotkeys.

#include "pr/app_input.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

// The names of the named keys, in the order of PrAppKey from PrAppKey_Left.
const char* const kKeyNames[] = {
    "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown", "Enter", "Escape",
    "Backspace", "Delete", "Tab", "Insert", "Shift", "Control", "Alt", "Meta", "CapsLock", "NumLock", "Fn",
    "ContextMenu", "Help", "Clear", "Dead", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13",
    "F14", "F15", "F16", "F17", "F18", "F19", "F20", "GoBack",
};
static_assert(countOf(kKeyNames) == PrAppKey_GoBack - PrAppKey_Left + 1, "a name for each key");

// The names of the codes, in the order of PrAppKeyCode.
const char* const kKeyCodeNames[] = {
    "Unidentified", "Backquote", "Backslash", "BracketLeft", "BracketRight", "Comma", "Digit0", "Digit1", "Digit2",
    "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Equal", "IntlBackslash", "IntlRo", "IntlYen",
    "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM", "KeyN",
    "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ", "Minus", "Period",
    "Quote", "Semicolon", "Slash", "AltLeft", "AltRight", "Backspace", "CapsLock", "ContextMenu", "ControlLeft",
    "ControlRight", "Enter", "MetaLeft", "MetaRight", "ShiftLeft", "ShiftRight", "Space", "Tab", "Lang1", "Lang2",
    "Delete", "End", "Help", "Home", "Insert", "PageDown", "PageUp", "ArrowDown", "ArrowLeft", "ArrowRight", "ArrowUp",
    "NumLock", "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8",
    "Numpad9", "NumpadAdd", "NumpadComma", "NumpadDecimal", "NumpadDivide", "NumpadEnter", "NumpadEqual",
    "NumpadMultiply", "NumpadSubtract", "Escape", "Fn", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
    "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "PrintScreen", "ScrollLock", "Pause",
};
static_assert(countOf(kKeyCodeNames) == PrAppKeyCode_Count, "a name for each code");

// The names of the enumerations, in the order of their enumerators.
const char* const kModifiers[] = {"Shift", "Control", "Alt", "Super", "CapsLock"};
static_assert(1u << (countOf(kModifiers) - 1) == PrAppModifierFlag_CapsLock, "a name for each modifier");

const char* const kPointerTypes[] = {"Mouse", "Pen", "Touch"};
static_assert(countOf(kPointerTypes) == PrAppPointerType_Touch + 1, "a name for each type of pointer");

const char* const kPointerModes[] = {"Free", "Confined", "Locked", "LockedRaw"};
static_assert(countOf(kPointerModes) == PrAppPointerMode_LockedRaw + 1, "a name for each pointer mode");

const char* const kCursors[] = {
    "Arrow", "None", "ContextMenu", "Help", "Hand", "Progress", "Wait", "Cell", "Crosshair", "Text", "VerticalText",
    "Alias", "Copy", "Move", "NoDrop", "NotAllowed", "Grab", "Grabbing", "ResizeN", "ResizeE", "ResizeS", "ResizeW",
    "ResizeNE", "ResizeNW", "ResizeSE", "ResizeSW", "ResizeX", "ResizeY", "ResizeNESW", "ResizeNWSE", "ResizeColumn",
    "ResizeRow", "AllScroll", "ZoomIn", "ZoomOut",
};
static_assert(countOf(kCursors) == PrAppCursor_Count, "a name for each cursor");

const char* const kGestureKinds[] = {"Magnify", "Rotate", "SmartMagnify", "Swipe"};
static_assert(countOf(kGestureKinds) == PrAppGestureKind_Swipe + 1, "a name for each kind of gesture");

const char* const kGesturePhases[] = {"Begin", "Change", "End", "Cancel"};
static_assert(countOf(kGesturePhases) == PrAppGesturePhase_Cancel + 1, "a name for each phase of a gesture");

const char* const kTextInputKinds[] = {"Text", "Password", "Number", "Decimal", "Phone", "Email", "Url", "Search"};
static_assert(countOf(kTextInputKinds) == PrAppTextInputKind_Search + 1, "a name for each kind of text input");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppModifierName(PrAppModifierFlags flag) {
    return appFlagNameOf(kModifiers, flag);
}

PrStringView prAppKeyName(PrAppKey key) {
    if (key < PrAppKey_Left || key > PrAppKey_GoBack)
        return "";
    return kKeyNames[key - PrAppKey_Left];
}

PrStringView prAppKeyCodeName(PrAppKeyCode code) {
    return kKeyCodeNames[code < PrAppKeyCode_Count ? code : PrAppKeyCode_Unidentified];
}

PrStringView prAppKeyCodeLabel(const PrApp* app, PrAppKeyCode code) {
    PR_ASSERT(code < PrAppKeyCode_Count);
    return app->keyCodeLabelFn != nullptr ? app->keyCodeLabelFn(app, code) : prAppKeyCodeName(code);
}

PrStringView prAppPointerTypeName(PrAppPointerType type) {
    return appNameOf(kPointerTypes, type);
}

PrStringView prAppPointerModeName(PrAppPointerMode mode) {
    return appNameOf(kPointerModes, mode);
}

PrStringView prAppCursorName(PrAppCursor cursor) {
    return appNameOf(kCursors, cursor);
}

PrStringView prAppGestureKindName(PrAppGestureKind kind) {
    return appNameOf(kGestureKinds, kind);
}

PrStringView prAppGesturePhaseName(PrAppGesturePhase phase) {
    return appNameOf(kGesturePhases, phase);
}

PrStringView prAppTextInputKindName(PrAppTextInputKind kind) {
    return appNameOf(kTextInputKinds, kind);
}

PrStatus prAppHotkeyRegister(PrApp* app, PrAppHotkey hotkey, uint64_t tag) {
    PR_ASSERT(hotkey.code != PrAppKeyCode_Unidentified && hotkey.code < PrAppKeyCode_Count);
    if (app->hotkeyRegisterFn == nullptr)
        return PrStatus_NotSupported;
    hotkey.modifiers &= PrAppModifierFlags(~PrAppModifierFlag_CapsLock);
    return app->hotkeyRegisterFn(app, hotkey, tag);
}

void prAppHotkeyUnregister(PrApp* app, uint64_t tag) {
    if (app->hotkeyUnregisterFn != nullptr)
        app->hotkeyUnregisterFn(app, tag);
}
