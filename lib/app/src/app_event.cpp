// Events: their details by kind.

#include "pr/app_event.h"

#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

bool isPointerEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_PointerMove || kind == PrAppEventKind_PointerDown ||
        kind == PrAppEventKind_PointerUp || kind == PrAppEventKind_PointerCancel ||
        kind == PrAppEventKind_PointerEnter || kind == PrAppEventKind_PointerLeave;
}

bool isKeyboardEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_KeyDown || kind == PrAppEventKind_KeyUp;
}

// Returns true for the drag events at a position: DragEnter, DragOver and Drop.
bool isPlacedDragEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_DragEnter || kind == PrAppEventKind_DragOver || kind == PrAppEventKind_Drop;
}

bool isInputEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_Text || kind == PrAppEventKind_Paste ||
        kind == PrAppEventKind_CompositionUpdate || kind == PrAppEventKind_CompositionEnd;
}

// The names of the enumerations, in the order of their enumerators.
const char* const kEventKinds[] = {
    "PointerMove", "PointerDown", "PointerUp", "PointerCancel", "PointerEnter", "PointerLeave", "Scroll", "Gesture",
    "TrackpadTouches", "KeyDown", "KeyUp", "Text", "Paste", "CompositionUpdate", "CompositionEnd", "DragEnter",
    "DragOver", "DragLeave", "Drop", "DragEnd", "DragPromise", "CloseRequested", "Timer", "Hotkey", "Notification",
    "StatusItem", "MenuOpening", "MenuItem", "Wake", "QuitRequested", "Open", "LifecycleChanged", "LowMemory",
    "ClipboardChanged", "DisplaysChanged", "LocaleChanged", "KeyboardLayoutChanged", "PowerChanged", "Suspend",
    "Resume", "ScreenLocked", "ScreenUnlocked", "Sensor",
};
static_assert(countOf(kEventKinds) == PrAppEventKind_Sensor + 1, "a name for each kind of event");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppEventKindName(PrAppEventKind kind) {
    return appNameOf(kEventKinds, kind);
}

PrVec2 prAppEventScrollPixels(const PrAppEvent* event, float lineHeight) {
    PR_ASSERT(event->kind == PrAppEventKind_Scroll);
    if (event->scroll.unit == PrAppScrollUnit_Pixel)
        return event->scroll.delta;
    return PrVec2{event->scroll.delta.x * lineHeight, event->scroll.delta.y * lineHeight};
}

PrStringView prAppEventText(const PrAppEvent* event) {
    if (isKeyboardEvent(event->kind))
        return event->keyboard.text;
    if (isInputEvent(event->kind))
        return event->input.text;
    return "";
}

void prAppEventSetText(PrAppEvent* event, PrStringView text) {
    if (isKeyboardEvent(event->kind))
        event->keyboard.text = text;
    else if (isInputEvent(event->kind))
        event->input.text = text;
}

bool prAppEventPosition(const PrAppEvent* event, PrVec2* out) {
    if (isPointerEvent(event->kind)) {
        *out = event->pointer.position;
        return true;
    }
    if (event->kind == PrAppEventKind_Scroll) {
        *out = event->scroll.position;
        return true;
    }
    if (event->kind == PrAppEventKind_Gesture) {
        *out = event->gesture.position;
        return true;
    }
    if (isPlacedDragEvent(event->kind)) {
        *out = event->drag.position;
        return true;
    }
    *out = PrVec2{};
    return false;
}

void prAppEventSetPosition(PrAppEvent* event, PrVec2 position) {
    if (isPointerEvent(event->kind))
        event->pointer.position = position;
    else if (event->kind == PrAppEventKind_Scroll)
        event->scroll.position = position;
    else if (event->kind == PrAppEventKind_Gesture)
        event->gesture.position = position;
    else if (isPlacedDragEvent(event->kind))
        event->drag.position = position;
}
