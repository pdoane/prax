// The web application backend: the page's canvas, its events and its animation frames, through Emscripten's HTML5 API.

#include "pr/app_web.h"

#include "pr/alloc.h"
#include "pr/memory.h"
#include "pr/raw_string.h"
#include "pr/time.h"
#include "pr/utf8.h"

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

// The page's side of the backend (app_web.js).
extern "C" void prAppWebSetTitle(const char* title);
extern "C" void prAppWebSetCursor(const char* selector, const char* cursor);
extern "C" void prAppWebCanvasOrigin(const char* selector, double* x, double* y);
extern "C" void prAppWebOpenUrl(const char* url);
extern "C" void prAppWebWriteClipboard(const char* text);
extern "C" bool prAppWebHasFocus(void);
extern "C" int prAppWebNotify(const char* title, const char* body, uint32_t tagLow, uint32_t tagHigh);

namespace pr_impl {

namespace {

// The longest time, and the farthest distance in logical pixels, that two presses may be apart to count as one
// multi-click.
constexpr double kClickSeconds = 0.5;
constexpr float kClickDistance = 4;

// The page's CSS cursors, in the order of PrAppCursor.
const char* const kCursors[PrAppCursor_Count] = {
    "default", "none", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair", "text",
    "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab", "grabbing", "n-resize", "e-resize",
    "s-resize", "w-resize", "ne-resize", "nw-resize", "se-resize", "sw-resize", "ew-resize", "ns-resize",
    "nesw-resize", "nwse-resize", "col-resize", "row-resize", "all-scroll", "zoom-in", "zoom-out",
};

// The application. A page runs only one.
struct WebApp : PrApp {
    PrAppHandler handler;
    PrRawString title;
    bool running;
    bool frameRequested;            // true if an animation frame is requested
    bool woken;                     // true if a Wake event is due
    int frameTimeout;               // the timeout that requests a frame at the time that the last frame asked for, or 0
    double lastFrame;               // the time of the last frame, in seconds, or zero before the first

    // The presses of a multi-click.
    uint32_t clicks;
    uint8_t clickButton;
    double clickTime;
    PrVec2 clickPosition;
};

WebApp* appOf(void* app) {
    return static_cast<WebApp*>(app);
}

// The running application, which receives notification clicks, or null if none is running.
WebApp* PR_NULLABLE runningApp = nullptr;

double now() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// Returns the frame as the page shows the canvas now.
PrAppFrame frameNow(WebApp* app) {
    double width = 0;
    double height = 0;
    emscripten_get_element_css_size(PR_APP_WEB_CANVAS, &width, &height);
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = float(emscripten_get_device_pixel_ratio());
    double time = now();
    PrAppFrame frame{
        .time = time,
        .delta = app->lastFrame != 0 ? time - app->lastFrame : 0,
        .extent = PrVec2{float(width), float(height)},
        .display = display,
    };
    return frame;
}

bool animationFrame(double, void* context) {
    WebApp* app = appOf(context);
    app->frameRequested = false;
    if (!app->running)
        return false;
    PrAppFrame frame = frameNow(app);
    app->lastFrame = frame.time;
    prAppDeliverFrame(app, &app->handler, &frame);
    return false;
}

void requestFrame(PrApp* base) {
    WebApp* app = appOf(base);
    if (app->frameRequested || !app->running)
        return;
    app->frameRequested = true;
    emscripten_request_animation_frame(animationFrame, app);
}

void frameAtTime(void* context) {
    WebApp* app = appOf(context);
    app->frameTimeout = 0;
    requestFrame(app);
}

void requestFrameAt(PrApp* base, double time) {
    WebApp* app = appOf(base);
    if (app->frameTimeout != 0)
        emscripten_clear_timeout(app->frameTimeout);
    app->frameTimeout = 0;
    if (time != 0) {
        double wait = (time - now()) * 1000;
        app->frameTimeout = emscripten_set_timeout(frameAtTime, wait > 0 ? wait : 0, app);
    }
}

void deliver(WebApp* app, const PrAppEvent& event) {
    if (app->running)
        app->handler.event(app->handler.context, app, &event);
}

void deliverWake(void* context) {
    WebApp* app = appOf(context);
    app->woken = false;
    deliver(app, PrAppEvent{.kind = PrAppEventKind_Wake, .time = now()});
}

void wake(PrApp* base) {
    WebApp* app = appOf(base);
    if (app->woken)
        return;
    app->woken = true;
    emscripten_set_timeout(deliverWake, 0, app);
}

PrAppModifierFlags modifiersOf(bool shift, bool control, bool alt, bool meta) {
    return PrAppModifierFlags((shift ? PrAppModifierFlag_Shift : 0) | (control ? PrAppModifierFlag_Control : 0) |
        (alt ? PrAppModifierFlag_Alt : 0) | (meta ? PrAppModifierFlag_Super : 0));
}

// The pointer's position on the canvas, in logical pixels.
PrVec2 positionOf(const EmscriptenMouseEvent* event) {
    double x = 0;
    double y = 0;
    prAppWebCanvasOrigin(PR_APP_WEB_CANVAS, &x, &y);
    return PrVec2{float(event->clientX - x), float(event->clientY - y)};
}

// The page numbers its buttons primary, middle and secondary. The application numbers them primary, secondary and
// middle.
uint8_t buttonOf(unsigned short button) {
    return button == 1 ? 2 : button == 2 ? 1 : uint8_t(button);
}

bool mouse(int type, const EmscriptenMouseEvent* e, void* context) {
    WebApp* app = appOf(context);
    PrAppEvent event{
        .modifiers = modifiersOf(e->shiftKey, e->ctrlKey, e->altKey, e->metaKey),
        .time = now(),
        .pointer = {.position = positionOf(e)},
    };
    PrAppPointerEvent& pointer = event.pointer;
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) {
        event.kind = PrAppEventKind_PointerMove;
    } else {
        event.kind = type == EMSCRIPTEN_EVENT_MOUSEDOWN ? PrAppEventKind_PointerDown : PrAppEventKind_PointerUp;
        pointer.button = buttonOf(e->button);
    }
    if (event.kind == PrAppEventKind_PointerDown) {
        PrVec2 moved{pointer.position.x - app->clickPosition.x, pointer.position.y - app->clickPosition.y};
        bool continues = app->clicks != 0 && pointer.button == app->clickButton &&
            event.time - app->clickTime <= kClickSeconds && moved.x * moved.x + moved.y * moved.y <=
            kClickDistance * kClickDistance;
        app->clicks = continues ? app->clicks + 1 : 1;
        app->clickButton = pointer.button;
        app->clickTime = event.time;
        app->clickPosition = pointer.position;
        pointer.clicks = app->clicks;
    }
    deliver(app, event);
    // A press keeps the page from selecting text or dragging the canvas.
    return type == EMSCRIPTEN_EVENT_MOUSEDOWN;
}

bool wheel(int, const EmscriptenWheelEvent* e, void* context) {
    WebApp* app = appOf(context);
    PrAppEvent event{
        .kind = PrAppEventKind_Scroll,
        .modifiers = modifiersOf(e->mouse.shiftKey, e->mouse.ctrlKey, e->mouse.altKey, e->mouse.metaKey),
        .time = now(),
        .scroll = {
            .position = positionOf(&e->mouse),
            .delta = PrVec2{float(e->deltaX), float(e->deltaY)},
            .unit = PrAppScrollUnit(e->deltaMode == DOM_DELTA_PIXEL ? PrAppScrollUnit_Pixel : PrAppScrollUnit_Line),
        },
    };
    // A page of scrolling is a screenful of lines.
    if (e->deltaMode == DOM_DELTA_PAGE) {
        double width = 0;
        double height = 0;
        emscripten_get_element_css_size(PR_APP_WEB_CANVAS, &width, &height);
        float lines = float(height) / kPrAppScrollLineHeight;
        event.scroll.delta = PrVec2{event.scroll.delta.x * lines, event.scroll.delta.y * lines};
    }
    deliver(app, event);
    return true;
}

// Returns the key that a KeyboardEvent's key value means. A printable key is its ASCII character, uppercase for
// letters. A key that types any other character is the key at its position on a US layout.
PrAppKey keyOf(PrStringView key, PrAppKeyCode code) {
    uint32_t codePoint = 0;
    if (key.size != 0 && prUtf8Decode(key, 0, &codePoint) == key.size) {
        if (codePoint < 128)
            return PrAppKey(codePoint >= 'a' && codePoint <= 'z' ? codePoint - 'a' + 'A' : codePoint);
        PrStringView name = prAppKeyCodeName(code);
        if (name.size == 4 && PrStringView{name.data, 3} == "Key")
            return PrAppKey(name.data[3]);
        if (name.size == 6 && PrStringView{name.data, 5} == "Digit")
            return PrAppKey(name.data[5]);
        return PrAppKey_Unknown;
    }
    for (PrAppKey named = PrAppKey_Left; named <= PrAppKey_F20; ++named) {
        if (prAppKeyName(named) == key)
            return named;
    }
    return PrAppKey_Unknown;
}

PrAppKeyCode codeOf(PrStringView code) {
    for (PrAppKeyCode c = 0; c < PrAppKeyCode_Count; ++c) {
        if (prAppKeyCodeName(c) == code)
            return c;
    }
    return PrAppKeyCode_Unidentified;
}

bool key(int type, const EmscriptenKeyboardEvent* e, void* context) {
    WebApp* app = appOf(context);
    PrStringView value = e->key;
    PrAppKeyCode code = codeOf(e->code);
    PrAppEvent event{
        .kind = PrAppEventKind(type == EMSCRIPTEN_EVENT_KEYDOWN ? PrAppEventKind_KeyDown : PrAppEventKind_KeyUp),
        .modifiers = modifiersOf(e->shiftKey, e->ctrlKey, e->altKey, e->metaKey),
        .time = now(),
        .keyboard = {.key = keyOf(value, code), .code = code, .repeat = e->repeat},
    };
    // A key's text is its key value if that is one character, and the key is not pressed with Control or Command.
    uint32_t codePoint = 0;
    bool types = value.size != 0 && prUtf8Decode(value, 0, &codePoint) == value.size && !e->ctrlKey && !e->metaKey;
    if (types)
        event.keyboard.text = value;
    deliver(app, event);
    if (types && type == EMSCRIPTEN_EVENT_KEYDOWN)
        deliver(app, PrAppEvent{.kind = PrAppEventKind_Text, .time = event.time, .input = {.text = value}});
    // The page keeps the keys of its own shortcuts, and the application takes the rest.
    bool function = event.keyboard.key >= PrAppKey_F1 && event.keyboard.key <= PrAppKey_F20;
    return !e->ctrlKey && !e->metaKey && !function;
}

bool resize(int, const EmscriptenUiEvent*, void* context) {
    requestFrame(appOf(context));
    return false;
}

// Requests a frame when the page gains or loses focus, because that changes what prAppIsActive returns.
bool focusChanged(int, const EmscriptenFocusEvent*, void* context) {
    requestFrame(appOf(context));
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The application's functions

void destroy(PrApp* base) {
    WebApp* app = appOf(base);
    prRawStringReset(&app->title);
    prMemoryFree(app);
}

void run(PrApp* base, const PrAppHandler* handler) {
    WebApp* app = appOf(base);
    app->handler = *handler;
    app->running = true;
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, mouse);
    emscripten_set_mousedown_callback(PR_APP_WEB_CANVAS, app, false, mouse);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, mouse);
    emscripten_set_wheel_callback(PR_APP_WEB_CANVAS, app, false, wheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, key);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, key);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, resize);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, focusChanged);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, focusChanged);
    runningApp = app;
    PrAppFrame window = frameNow(app);
    app->handler.start(app->handler.context, app, &window);
    // The page's event loop runs the application from here, and main() does not continue.
    emscripten_exit_with_live_runtime();
}

void stop(PrApp* base) {
    WebApp* app = appOf(base);
    if (!app->running)
        return;
    app->running = false;
    runningApp = nullptr;
    emscripten_html5_remove_all_event_listeners();
    if (app->frameTimeout != 0)
        emscripten_clear_timeout(app->frameTimeout);
    app->frameTimeout = 0;
    app->handler.shutdown(app->handler.context, app);
}

void setCursor(PrApp*, PrAppCursor cursor) {
    prAppWebSetCursor(PR_APP_WEB_CANVAS, kCursors[cursor < PrAppCursor_Count ? cursor : PrAppCursor_Arrow]);
}

void setTitle(PrApp* base, PrStringView title) {
    WebApp* app = appOf(base);
    prRawStringReset(&app->title);
    app->title = prRawStringCopy(title);
    // The page reads a NUL-terminated copy.
    PrRawString terminated = prRawStringCopy(title);
    prRawStringAppend(&terminated, PrStringView{"", 1});
    prAppWebSetTitle(prRawStringView(&terminated).data);
    prRawStringReset(&terminated);
}

bool isActive(PrApp*) {
    return prAppWebHasFocus();
}

PrStatus notificationPost(PrApp* base, const PrAppNotificationDesc* desc) {
    if (!appOf(base)->running)
        return PrStatus_InvalidState;
    // The page reads NUL-terminated copies.
    PrRawString title = prRawStringCopy(desc->title);
    prRawStringAppend(&title, PrStringView{"", 1});
    PrRawString body = prRawStringCopy(desc->body);
    prRawStringAppend(&body, PrStringView{"", 1});
    int shown = prAppWebNotify(prRawStringView(&title).data, prRawStringView(&body).data, uint32_t(desc->tag),
        uint32_t(desc->tag >> 32));
    prRawStringReset(&title);
    prRawStringReset(&body);
    if (shown == 1)
        return PrStatus_NotAllowed;
    return shown == 0 ? PrStatus_Ok : PrStatus_NotSupported;
}

PrStatus surface(PrApp*, PrAppSurface* out) {
    *out = PrAppSurface{.kind = PrAppSurfaceKind_HtmlCanvas, .handle = const_cast<char*>(PR_APP_WEB_CANVAS)};
    return PrStatus_Ok;
}

PrStatus openUrl(PrApp*, PrStringView url) {
    PrRawString terminated = prRawStringCopy(url);
    prRawStringAppend(&terminated, PrStringView{"", 1});
    prAppWebOpenUrl(prRawStringView(&terminated).data);
    prRawStringReset(&terminated);
    return PrStatus_Ok;
}

// The page gives the clipboard's text only to a paste that the user makes, so a request gets no text.
void requestClipboardText(PrApp* base) {
    deliver(appOf(base), PrAppEvent{.kind = PrAppEventKind_Clipboard, .time = now()});
}

PrStatus setClipboardText(PrApp*, PrStringView text) {
    PrRawString terminated = prRawStringCopy(text);
    prRawStringAppend(&terminated, PrStringView{"", 1});
    prAppWebWriteClipboard(prRawStringView(&terminated).data);
    prRawStringReset(&terminated);
    return PrStatus_Ok;
}

void requestClipboardImage(PrApp* base) {
    deliver(appOf(base), PrAppEvent{.kind = PrAppEventKind_ClipboardImage, .time = now()});
}

PrStatus setClipboardImage(PrApp*, PrPixelView) {
    return PrStatus_NotSupported;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrApp* prAppWebCreate(const PrAppDesc* desc) {
    WebApp* app = allocZeroed<WebApp>();
    app->destroy = destroy;
    app->run = run;
    app->stop = stop;
    app->wake = wake;
    app->requestFrame = requestFrame;
    app->requestFrameAt = requestFrameAt;
    app->setCursor = setCursor;
    app->setTitle = setTitle;
    app->isActive = isActive;
    app->notificationPost = notificationPost;
    app->surface = surface;
    app->openUrl = openUrl;
    app->requestClipboardText = requestClipboardText;
    app->setClipboardText = setClipboardText;
    app->requestClipboardImage = requestClipboardImage;
    app->setClipboardImage = setClipboardImage;
    setTitle(app, desc->title);
    return app;
}

// The page calls this function by name, so the build keeps it.
EMSCRIPTEN_KEEPALIVE void prAppWebNotificationClicked(uint32_t tagLow, uint32_t tagHigh) {
    if (runningApp == nullptr)
        return;
    PrAppEvent event{
        .kind = PrAppEventKind_Notification,
        .time = now(),
        .notification = {.tag = (uint64_t(tagHigh) << 32) | tagLow},
    };
    deliver(runningApp, event);
}
