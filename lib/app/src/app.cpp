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

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrAppBackend_Count] = {"default", "cocoa", "win32"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrAppBackend kBuilt[] = {
#if defined(PR_APP_HAS_COCOA)
    PrAppBackend_Cocoa,
#endif
#if defined(PR_APP_HAS_WIN32)
    PrAppBackend_Win32,
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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrVec2 prAppEventScrollPixels(const PrAppEvent* event, float lineHeight) {
    PR_ASSERT(event->kind == PrAppEventKind_Scroll);
    if (event->scrollUnit == PrAppScrollUnit_Pixel)
        return event->delta;
    return PrVec2{event->delta.x * lineHeight, event->delta.y * lineHeight};
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
        if (asciiEqualsFold(kBackendNames[backend], name))
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
    switch (backend == PrAppBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_APP_HAS_COCOA)
        case PrAppBackend_Cocoa: return prAppCocoaCreate(desc);
#endif
#if defined(PR_APP_HAS_WIN32)
        case PrAppBackend_Win32: return prAppWin32Create(desc);
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

void prAppWake(PrApp* app) {
    app->wake(app);
}

void prAppRequestFrame(PrApp* app) {
    app->requestFrame(app);
}

void prAppSetCursor(PrApp* app, PrAppCursor cursor) {
    app->setCursor(app, cursor);
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

PrStatus prAppBrowserCreate(PrApp* app, const PrAppBrowserDesc* desc, PrAppBrowser* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (app->browserCreate == nullptr)
        return PrStatus_NotSupported;
    PR_ASSERT(desc->pixelRatio >= 0);
    PrStatus status = app->browserCreate(app, desc, out);
    PR_ASSERT(status != PrStatus_Ok || ((*out) != nullptr && (*out)->app == app));
    return status;
}

void prAppBrowserDestroy(PrAppBrowser* PR_NULLABLE browser) {
    if (browser != nullptr)
        browser->app->browserDestroy(browser->app, browser);
}

void prAppBrowserResize(PrAppBrowser* browser, PrUVec2 viewport) {
    browser->app->browserResize(browser->app, browser, viewport);
}

void prAppBrowserLoad(PrAppBrowser* browser, PrStringView url, PrAppBrowserDoneFn done, void* PR_NULLABLE context) {
    browser->app->browserLoad(browser->app, browser, url, done, context);
}

void prAppBrowserEvaluate(PrAppBrowser* browser, PrStringView script, PrAppBrowserResultFn done, void* PR_NULLABLE context) {
    browser->app->browserEvaluate(browser->app, browser, script, done, context);
}

void prAppBrowserCapture(PrAppBrowser* browser, PrRect rect, PrUVec2 size, PrAppBrowserCaptureFn done,
        void* PR_NULLABLE context) {
    browser->app->browserCapture(browser->app, browser, rect, size, done, context);
}

void prAppBrowserShow(PrAppBrowser* browser, PrRect clip, PrRect frame) {
    browser->app->browserShow(browser->app, browser, clip, frame);
}

void prAppBrowserHide(PrAppBrowser* browser) {
    browser->app->browserHide(browser->app, browser);
}

PrStatus prAppBrowserPrint(PrAppBrowser* browser, PrStringView path, PrVec2 paper, PrAppBrowserDoneFn done,
        void* PR_NULLABLE context) {
    PrApp* app = browser->app;
    if (app->browserPrint == nullptr)
        return PrStatus_NotSupported;
    app->browserPrint(app, browser, path, paper, done, context);
    return PrStatus_Ok;
}

uint32_t prAppBrowserProcesses(PrAppBrowser* browser, PrAppBrowserProcess* PR_NULLABLE out, uint32_t capacity) {
    PrApp* app = browser->app;
    return app->browserProcesses != nullptr ? app->browserProcesses(app, browser, out, capacity) : 0;
}
