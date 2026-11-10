// Applications: the interface's contract around each backend's operations.

#include "pr/app.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/utf8.h"
#include "pr/vec.h"

#if defined(PR_APP_HAS_COCOA)
#include "pr/app_cocoa.h"
#endif
#if defined(PR_APP_HAS_WIN32)
#include "pr/app_win32.h"
#endif
#if defined(PR_APP_HAS_WEB)
#include "pr/app_web.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrAppBackend_Count] = {"default", "cocoa", "win32", "web"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrAppBackend kBuilt[] = {
#if defined(PR_APP_HAS_COCOA)
    PrAppBackend_Cocoa,
#endif
#if defined(PR_APP_HAS_WIN32)
    PrAppBackend_Win32,
#endif
#if defined(PR_APP_HAS_WEB)
    PrAppBackend_Web,
#endif
    PrAppBackend_Default,
};

// The names of the named keys, in the order of PrAppKey from PrAppKey_Left.
const char* const kKeyNames[] = {
    "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown", "Enter", "Escape",
    "Backspace", "Delete", "Tab", "Insert", "Shift", "Control", "Alt", "Meta", "CapsLock", "NumLock", "Fn",
    "ContextMenu", "Help", "Clear", "Dead", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13",
    "F14", "F15", "F16", "F17", "F18", "F19", "F20",
};
static_assert(countOf(kKeyNames) == PrAppKey_F20 - PrAppKey_Left + 1, "a name for each key");

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

// Returns true if events of `kind` use the union's pointer, keyboard or input member.
bool isPointerEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_PointerMove || kind == PrAppEventKind_PointerDown || kind == PrAppEventKind_PointerUp;
}

bool isKeyboardEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_KeyDown || kind == PrAppEventKind_KeyUp;
}

bool isInputEvent(PrAppEventKind kind) {
    return kind == PrAppEventKind_Text || kind == PrAppEventKind_Clipboard ||
        kind == PrAppEventKind_CompositionUpdate || kind == PrAppEventKind_CompositionEnd;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

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
    *out = PrVec2{};
    return false;
}

void prAppEventSetPosition(PrAppEvent* event, PrVec2 position) {
    if (isPointerEvent(event->kind))
        event->pointer.position = position;
    else if (event->kind == PrAppEventKind_Scroll)
        event->scroll.position = position;
}

PrStringView prAppKeyName(PrAppKey key) {
    if (key < PrAppKey_Left || key > PrAppKey_F20)
        return "";
    return kKeyNames[key - PrAppKey_Left];
}

PrStringView prAppKeyCodeName(PrAppKeyCode code) {
    return kKeyCodeNames[code < PrAppKeyCode_Count ? code : PrAppKeyCode_Unidentified];
}

PrAppBackend prAppBackendFromName(PrStringView name) {
    for (PrAppBackend backend = 0; backend < PrAppBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrAppBackend_Count;
}

PrStringView prAppBackendName(PrAppBackend backend) {
    return backend < PrAppBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prAppBackendIsBuilt(PrAppBackend backend) {
    for (PrAppBackend built : kBuilt) {
        if (built != PrAppBackend_Default && (built == backend || backend == PrAppBackend_Default))
            return true;
    }
    return false;
}

PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppDesc* desc) {
    // A build without a backend has no use for the description.
    (void)desc;
    switch (backend == PrAppBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_APP_HAS_COCOA)
        case PrAppBackend_Cocoa: return prAppCocoaCreate(desc);
#endif
#if defined(PR_APP_HAS_WIN32)
        case PrAppBackend_Win32: return prAppWin32Create(desc);
#endif
#if defined(PR_APP_HAS_WEB)
        case PrAppBackend_Web: return prAppWebCreate(desc);
#endif
        default: return nullptr;
    }
}

void prAppDestroy(PrApp* PR_NULLABLE app) {
    if (app != nullptr && app->destroy != nullptr)
        app->destroy(app);
}

void prAppRun(PrApp* app, const PrAppHandler* handler) {
    PR_ASSERT(handler->start != nullptr && handler->frame != nullptr && handler->event != nullptr &&
        handler->shutdown != nullptr);
    app->run(app, handler);
}

void prAppStop(PrApp* app) {
    app->stop(app);
}

void prAppDeliverFrame(PrApp* app, const PrAppHandler* handler, const PrAppFrame* frame) {
    PrAppOutput output = handler->frame(handler->context, app, frame);
    app->setCursor(app, output.cursor);
    if (output.hasTextInput && app->setTextInputRect != nullptr)
        app->setTextInputRect(app, output.textInput);
    if (output.wantsFrame)
        app->requestFrame(app);
    app->requestFrameAt(app, output.frameTime);
}

void prAppWake(PrApp* app) {
    app->wake(app);
}

void prAppRequestFrame(PrApp* app) {
    app->requestFrame(app);
}

PrAppOutput prAppOutputPlaced(PrAppOutput output, PrVec2 origin, float scale) {
    output.textInput = PrRect{
        .left = origin.x + output.textInput.left * scale,
        .top = origin.y + output.textInput.top * scale,
        .right = origin.x + output.textInput.right * scale,
        .bottom = origin.y + output.textInput.bottom * scale,
    };
    return output;
}

PrAppOutput prAppOutputMerge(PrAppOutput first, PrAppOutput second, bool pointerInSecond) {
    const PrAppOutput& input = second.hasTextInput ? second : first;
    double frameTime = first.frameTime != 0 ? first.frameTime : second.frameTime;
    if (second.frameTime != 0 && second.frameTime < frameTime)
        frameTime = second.frameTime;
    return PrAppOutput{
        .cursor = pointerInSecond ? second.cursor : first.cursor,
        .wantsFrame = first.wantsFrame || second.wantsFrame,
        .hasTextInput = input.hasTextInput,
        .textInput = input.textInput,
        .frameTime = frameTime,
    };
}

void prAppSetTitle(PrApp* app, PrStringView title) {
    app->setTitle(app, title);
}

void prAppSetExtent(PrApp* app, PrUVec2 extent) {
    if (app->setExtent != nullptr)
        app->setExtent(app, extent);
}

void prAppRequestAttention(PrApp* app) {
    if (app->requestAttention != nullptr)
        app->requestAttention(app);
}

bool prAppIsActive(PrApp* app) {
    return app->isActive == nullptr || app->isActive(app);
}

PrStatus prAppNotificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PR_ASSERT(prUtf8Validate(desc->title) == desc->title.size && prUtf8Validate(desc->body) == desc->body.size);
    if (app->notificationPost == nullptr)
        return PrStatus_NotSupported;
    return app->notificationPost(app, desc);
}

PrStatus prAppSurface(PrApp* app, PrAppSurface* out) {
    return app->surface(app, out);
}

PrStatus prAppOpenUrl(PrApp* app, PrStringView url) {
    return app->openUrl(app, url);
}

void prAppRequestClipboardText(PrApp* app) {
    app->requestClipboardText(app);
}

PrStatus prAppSetClipboardText(PrApp* app, PrStringView text) {
    PR_ASSERT(prUtf8Validate(text) == text.size);
    return app->setClipboardText(app, text);
}

void prAppRequestClipboardImage(PrApp* app) {
    app->requestClipboardImage(app);
}

PrStatus prAppSetClipboardImage(PrApp* app, PrPixelView pixels) {
    PR_ASSERT(pixels.extent.x != 0 && pixels.extent.y != 0);
    return app->setClipboardImage(app, pixels);
}
