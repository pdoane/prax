// The web application backend: the page's windows, each a canvas, their events and their animation frames, through
// Emscripten's HTML5 API.

#include "pr/app_web.h"

#include "pr/alloc.h"
#include "pr/app_backend.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/time.h"
#include "pr/utf8.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_delivery.h"
#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/shared/src/app_shared_title_bar.h"

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

// The page's side of the backend (app_web.js).
extern "C" void prAppWebSetTitle(const char* PR_NULLABLE title, size_t titleSize);
extern "C" void prAppWebSetCursor(const char* PR_NULLABLE selector, size_t selectorSize, const char* PR_NULLABLE cursor,
    size_t cursorSize);
extern "C" void prAppWebOpenUrl(const char* PR_NULLABLE url, size_t urlSize);
extern "C" void prAppWebClipboardWrite(const char* PR_NULLABLE text, size_t textSize);
extern "C" int prAppWebClipboardWriteImage(const uint8_t* pixels, uint32_t width, uint32_t height);
extern "C" void prAppWebClipboardRead(uint32_t id, bool image);
extern "C" void prAppWebListenForPaste(bool listen);
extern "C" void prAppWebListenForLaunches(bool listen);
extern "C" bool prAppWebHasFocus(void);
extern "C" void prAppWebFocus(void);
extern "C" void prAppWebListenForDisplays(bool listen);
extern "C" bool prAppWebListenForHost(const char* PR_NULLABLE host, size_t hostSize, bool listen);
extern "C" void prAppWebHostExtent(const char* PR_NULLABLE host, size_t hostSize, double* out);
extern "C" bool prAppWebWindowCreate(const char* PR_NULLABLE host, size_t hostSize, uint32_t window,
    const char* PR_NULLABLE title, size_t titleSize, bool systemTitleBar, uint32_t sheetParent, bool sheetFramed);
extern "C" void prAppWebPreferences(double* out);
extern "C" void prAppWebListenForPreferences(bool listen);
extern "C" void prAppWebWindowDestroy(uint32_t window);
extern "C" void prAppWebWindowPlace(uint32_t window, double x, double y, double width, double height, double z,
    int flags, double opacity);
extern "C" void prAppWebWindowSetTitle(uint32_t window, const char* PR_NULLABLE title, size_t titleSize);
extern "C" void prAppWebWindowFocus(uint32_t window);
extern "C" void prAppWebWindowBounds(uint32_t window, double* out);
extern "C" bool prAppWebIsFullscreen(const char* PR_NULLABLE selector, size_t selectorSize);
extern "C" void prAppWebHostSetFullscreen(const char* PR_NULLABLE host, size_t hostSize, bool fullscreen);
extern "C" bool prAppWebHostIsFullscreen(const char* PR_NULLABLE host, size_t hostSize);
extern "C" void prAppWebSetFullscreen(const char* PR_NULLABLE selector, size_t selectorSize, bool fullscreen);
extern "C" int prAppWebNotify(const char* PR_NULLABLE title, size_t titleSize, const char* PR_NULLABLE body,
    size_t bodySize, uint32_t tagLow, uint32_t tagHigh);
extern "C" void prAppWebSetPointerLock(const char* PR_NULLABLE selector, size_t selectorSize, bool locked, bool raw);
extern "C" void prAppWebOpenFiles(uint32_t id, bool multiple, const char* PR_NULLABLE accept, size_t acceptSize);
extern "C" void prAppWebReadFile(uint32_t id, uint32_t key, uint32_t index, double offset, double size);
extern "C" void prAppWebFileSetRelease(uint32_t key);
extern "C" void prAppWebListenForServices(bool listen);
extern "C" int prAppWebLifecycle(void);
extern "C" void prAppWebPower(double* out);
extern "C" void prAppWebSetAwake(bool display);
extern "C" bool prAppWebShare(uint32_t id, const char* PR_NULLABLE text, size_t textSize, const char* PR_NULLABLE url,
    size_t urlSize);
extern "C" int prAppWebKeyLabel(const char* PR_NULLABLE code, size_t codeSize, char* out, int capacity);
extern "C" void prAppWebVibrate(int haptic);
extern "C" void prAppWebSetBadge(const char* PR_NULLABLE text, size_t textSize);
extern "C" int prAppWebSensor(int sensor, bool enable, double interval);
extern "C" void prAppWebLockOrientation(int orientations);
extern "C" void prAppWebSetIcon(const uint8_t* PR_NULLABLE pixels, uint32_t width, uint32_t height);

namespace pr_impl {

namespace {

// The longest time, and the farthest distance in logical pixels, that two presses may be apart to count as one
// multi-click.
constexpr double kClickSeconds = 0.5;
constexpr float kClickDistance = 4;

// The height of a window's title bar in CSS px: the bar that the page draws for a System window, and the band that a
// Custom window draws itself. It matches app_web.js.
constexpr float kTitleBarHeight = 30;

// How far each window that opens at its Default place is from the last, and how much of a window stays in the host when
// the user moves it toward an edge, in CSS px.
constexpr float kCascade = 24;
constexpr float kKeptInHost = 48;

// The parts of a window that a press on its frame moves: its edges, or the whole window.
enum {
    FrameEdge_Left = 1 << 0,
    FrameEdge_Top = 1 << 1,
    FrameEdge_Right = 1 << 2,
    FrameEdge_Bottom = 1 << 3,
    FrameEdge_Move = 1 << 4,
};
typedef uint8_t FrameEdges;

// How the page shows a window's frame (prAppWebWindowPlace). It matches app_web.js.
enum {
    PlaceFlag_Visible = 1 << 0,
    PlaceFlag_Fill = 1 << 1,            // the frame fills the host
    PlaceFlag_Passthrough = 1 << 2,
    PlaceFlag_Blur = 1 << 3,            // the page blurs what is behind the frame
    PlaceFlag_Resizable = 1 << 4,       // the frame has resize borders
    PlaceFlag_Blocks = 1 << 5,          // a layer in the parent's frame dims it and takes its input
    PlaceFlag_NoTitleBar = 1 << 6,      // the page's title bar hides, for a fullscreen window
    PlaceFlag_Minimized = 1 << 7,       // the shelf shows a button that restores the window
};
typedef uint8_t PlaceFlags;

// The page's CSS cursors, in the order of PrAppCursor.
constexpr PrStringView kCursors[PrAppCursor_Count] = {
    "default", "none", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair", "text",
    "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab", "grabbing", "n-resize", "e-resize",
    "s-resize", "w-resize", "ne-resize", "nw-resize", "se-resize", "sw-resize", "ew-resize", "ns-resize",
    "nesw-resize", "nwse-resize", "col-resize", "row-resize", "all-scroll", "zoom-in", "zoom-out",
};

struct WebWindow;

// A timer of the program: its tag, its id, which its timeout carries, the page's timeout, and the interval of its
// repeats, or zero.
struct WebTimer {
    uint64_t tag;
    uint32_t id;
    int timeout;
    double interval;
};

// The application. A page runs only one.
struct WebApp : PrApp {
    PrAppHandler handler;           // the handler of the run
    AppDataSources sources;
    AppDataRequests requests;
    bool running;
    bool woken;                     // true if a Wake event is due
    PrAppWindowModel windowModel;
    PrRawString host;               // the CSS selector of the element that holds the windows, with its NUL

    // The windows, from the bottom of the page's order to the top, and the one whose canvas has the keyboard, or null.
    RawArray<WebWindow*> windows;
    WebWindow* PR_NULLABLE active;
    uint32_t lastWindowId;

    // The pointer's last position in the host, in CSS px, if a window has reported it.
    PrVec2 pointerAt;
    bool hasPointer;

    // A drag that a window started, which the backend carries between the page's windows while the press that began it
    // lasts: the id of its source window, or 0 for none, its content and the operations that it allows, and the window
    // under the pointer, or 0.
    uint32_t dragSource;
    AppDataStore dragContent;
    PrAppDragOperationFlags dragOperations;
    uint32_t dragOver;

    PrAppLifecycle lifecycle;       // the lifecycle that the last LifecycleChanged event reported
    RawArray<WebTimer> timers;
    uint32_t lastTimerId;

    // The label of each key code in the page's keyboard layout, computed on first use (prAppKeyCodeLabel), which a
    // query of the application fills. An empty label that is computed means the key's name.
    mutable PrRawString keyLabels[PrAppKeyCode_Count];
    mutable bool keyLabeled[PrAppKeyCode_Count];
};

// A window: a canvas in a frame that the backend adds to the host.
struct WebWindow : PrAppWindow {
    WebApp* app;
    AppWindowDelivery delivery;
    uint32_t id;
    PrRawString selector;           // the CSS selector of the canvas, with its NUL
    PrRawString title;
    bool frameRequested;            // true if an animation frame is requested
    int frameTimeout;               // the timeout that requests a frame at the time that the last frame asked for, or 0
    double lastFrame;               // the time of the last frame, in seconds, or zero before the first

    WebWindow* PR_NULLABLE parent;  // of a Dialog or a Popup, while the parent is open
    PrAppWindowRole role;
    PrAppWindowDecoration decoration;
    bool modal;
    bool fills;                     // the window of a Single application, which fills the host
    PrAppPopupPlacement popup;

    // The canvas's place in the host while the window is Normal, in CSS px, and the limits of its extent.
    PrRect rect;
    PrUVec2 minExtent;
    PrUVec2 maxExtent;
    float aspectRatio;

    PrAppWindowState state;         // Normal, Minimized or Maximized. The page reports Fullscreen
    bool shown;
    bool keepAbove;
    bool passthrough;
    float opacity;
    PrAppWindowBackdrop backdrop;

    // The regions of a Custom title bar, and what a press on the frame or the title bar does until its release: the
    // edges that it moves, the press's place in the host and the window's place when it began, and the button that it
    // pressed, or Client for none.
    AppTitleBarRegions titleBar;
    FrameEdges moving;
    PrVec2 pressAt;
    PrRect pressRect;
    PrAppTitleBarRegionKind pressedButton;

    // The presses of a multi-click.
    uint32_t clicks;
    uint8_t clickButton;
    double clickTime;
    PrVec2 clickPosition;

    PrAppDragOperation dropOperation;   // the answer to a drag over the canvas, from the latest output

    // The pointer's buttons that are down, by bit of PrAppPointerEvent::button.
    uint32_t buttons;

    // The pointer's last position over the canvas, and whether the canvas locks the pointer there, in `lockedMode`.
    PrVec2 pointer;
    bool locked;
    PrAppPointerMode lockedMode;
};

WebApp* appOf(void* app) {
    return static_cast<WebApp*>(app);
}

WebWindow* windowOf(void* window) {
    return static_cast<WebWindow*>(window);
}

const WebApp* appOf(const void* app) {
    return static_cast<const WebApp*>(app);
}

const WebWindow* windowOf(const void* window) {
    return static_cast<const WebWindow*>(window);
}

// The running application, which receives the page's events, or null if none is running.
WebApp* PR_NULLABLE runningApp = nullptr;

double now() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// Returns the window of the running application with `id`, or null.
WebWindow* PR_NULLABLE windowWithId(uint32_t id) {
    WebApp* app = runningApp;
    for (uint32_t i = 0; app != nullptr && i < app->windows.count; ++i) {
        if (app->windows.data[i]->id == id)
            return app->windows.data[i];
    }
    return nullptr;
}

// Returns the host's extent in CSS px.
PrVec2 hostExtent(const WebApp* app) {
    double extent[2] = {};
    PrStringView host = prRawStringView(&app->host);
    prAppWebHostExtent(host.data, host.size, extent);
    return PrVec2{float(extent[0]), float(extent[1])};
}

// Returns true if `window` belongs to `ancestor`, as a Dialog or a Popup of it or of its own dialogs and popups.
bool isDescendant(const WebWindow* window, const WebWindow* ancestor) {
    for (const WebWindow* at = window->parent; at != nullptr; at = at->parent) {
        if (at == ancestor)
            return true;
    }
    return false;
}

// Returns true if the user moves and resizes the window: a Normal window or a dialog of a Multiple application, but a
// modal dialog, which is a sheet over its parent.
bool isFloating(const WebWindow* window) {
    bool dialog = window->role == PrAppWindowRole_Dialog && !window->modal;
    return !window->fills && (window->role == PrAppWindowRole_Normal || dialog);
}

// Returns true if the page draws the window's title bar: a System window that floats.
bool hasPageTitleBar(const WebWindow* window) {
    return isFloating(window) && window->decoration == PrAppWindowDecoration_System;
}

// Returns true if the window covers its display. The window of a Single application, which fills the page, covers the
// machine's display through the page's Fullscreen API. Any other window covers the host.
bool isFullscreen(const WebWindow* window) {
    if (window->fills) {
        PrStringView selector = prRawStringView(&window->selector);
        return prAppWebIsFullscreen(selector.data, selector.size);
    }
    return window->state == PrAppWindowState_Fullscreen;
}

// The title bar that the program draws: the band of a Custom window that floats, and none in any other window.
PrAppTitleBar titleBarOf(WebWindow* window) {
    if (!isFloating(window) || window->decoration != PrAppWindowDecoration_Custom || isFullscreen(window))
        return PrAppTitleBar{};
    return PrAppTitleBar{.height = kTitleBarHeight, .flags = PrAppTitleBarFlag_ProgramButtons};
}

// Returns the frame as the page shows the window now.
PrAppFrame frameNow(WebWindow* window) {
    double width = 0;
    double height = 0;
    emscripten_get_element_css_size(prRawStringView(&window->selector).data, &width, &height);
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = float(emscripten_get_device_pixel_ratio());
    double time = now();
    // The page reports the user's preferences through its media queries.
    double read[8] = {};
    prAppWebPreferences(read);
    PrAppUserPreferences preferences = prAppUserPreferencesDefault();
    preferences.colorScheme = PrAppColorScheme(read[0] != 0 ? PrAppColorScheme_Dark : PrAppColorScheme_Light);
    preferences.contrast = PrAppContrast(read[1]);
    preferences.reducedMotion = read[2] != 0;
    preferences.reducedTransparency = read[3] != 0;
    preferences.accentColor = PrColor{float(read[4]), float(read[5]), float(read[6]), float(read[7])};
    PrAppFrame frame{
        .time = time,
        .delta = window->lastFrame != 0 ? time - window->lastFrame : 0,
        .extent = PrVec2{float(width), float(height)},
        .display = display,
        .preferences = preferences,
        .titleBar = titleBarOf(window),
        .backdrop = window->backdrop,
    };
    return frame;
}

void windowDestroy(PrAppWindow* base);
void applyOutput(WebWindow* window, const PrAppOutput& output);
void requestFrame(PrAppWindow* base);

PrAppFrame frameState(PrAppWindow* base) {
    return frameNow(windowOf(base));
}

// Delivers an event to the window. A window that the program destroyed during the call is freed.
void deliver(WebWindow* window, const PrAppEvent& event) {
    if (!window->app->running)
        return;
    if (!appWindowDeliverEvent(window->delivery, window, &event, frameState))
        windowDestroy(window);
}

// Delivers an event to the application's handler.
void deliverToApp(WebApp* app, const PrAppEvent& event) {
    if (app->running)
        app->handler.event(app->handler.context, app, &event);
}

// Returns the window that takes the keyboard: the one whose canvas has the page's focus, or else the topmost visible
// Normal window.
WebWindow* PR_NULLABLE keyboardWindow(WebApp* app) {
    if (app->active != nullptr)
        return app->active;
    for (uint32_t i = app->windows.count; i-- > 0;) {
        WebWindow* window = app->windows.data[i];
        if (window->role == PrAppWindowRole_Normal && window->shown && window->state != PrAppWindowState_Minimized)
            return window;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Placement and order

// Returns the layer of the page's order that the window is in: an overlay above everything, a window that keeps above
// above the others, and a Dialog or a Popup with the window that it belongs to.
uint32_t layerOf(const WebWindow* window) {
    if (window->role == PrAppWindowRole_Overlay)
        return 2;
    const WebWindow* root = window;
    while (root->parent != nullptr)
        root = root->parent;
    return root->keepAbove ? 1 : 0;
}

// Shows the window in the page as its state and place have it.
void place(WebWindow* window, uint32_t z) {
    bool fullscreen = window->state == PrAppWindowState_Fullscreen;
    bool maximized = window->state == PrAppWindowState_Maximized;
    bool shown = window->shown && window->state != PrAppWindowState_Minimized;
    bool filled = window->fills || maximized || fullscreen;
    bool resizable = isFloating(window) && !filled && window->decoration != PrAppWindowDecoration_None &&
        (window->minExtent.x == 0 || window->minExtent.x != window->maxExtent.x ||
            window->minExtent.y != window->maxExtent.y);
    PlaceFlags flags = PlaceFlags((shown ? PlaceFlag_Visible : 0) | (filled ? PlaceFlag_Fill : 0) |
        (window->passthrough ? PlaceFlag_Passthrough : 0) |
        (window->backdrop == PrAppWindowBackdrop_Blur ? PlaceFlag_Blur : 0) | (resizable ? PlaceFlag_Resizable : 0) |
        (shown && window->modal ? PlaceFlag_Blocks : 0) | (fullscreen ? PlaceFlag_NoTitleBar : 0) |
        (window->shown && window->state == PrAppWindowState_Minimized ? PlaceFlag_Minimized : 0));
    const PrRect& r = window->rect;
    prAppWebWindowPlace(window->id, double(r.left), double(r.top), double(r.right - r.left), double(r.bottom - r.top), z,
        flags, double(window->opacity));
}

// Gives each window its place in the page's order, by layer, and shows every window where it is.
void restack(WebApp* app) {
    uint32_t z = 2;
    for (uint32_t layer = 0; layer < 3; ++layer) {
        for (uint32_t i = 0; i < app->windows.count; ++i) {
            WebWindow* window = app->windows.data[i];
            if (layerOf(window) != layer)
                continue;
            place(window, z);
            // A modal dialog's layer goes just under it.
            z += 2;
        }
    }
}

// Raises the window to the top of its layer, with the dialogs and popups that belong to it above it.
void raise(WebWindow* window) {
    WebApp* app = window->app;
    RawArray<WebWindow*> lifted;
    lifted.push(window);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        WebWindow* other = app->windows.data[i];
        if (other == window)
            continue;
        if (isDescendant(other, window))
            lifted.push(other);
        else
            app->windows.data[kept++] = other;
    }
    app->windows.count = kept;
    for (uint32_t i = 0; i < lifted.count; ++i)
        app->windows.push(lifted.data[i]);
    lifted.reset();
    restack(app);
}

// Keeps the window's extent within its limits, its aspect ratio and the host, and enough of it in the host to grab.
void clamp(WebWindow* window, FrameEdges edges) {
    PrVec2 host = hostExtent(window->app);
    PrRect& r = window->rect;
    float width = r.right - r.left;
    float height = r.bottom - r.top;
    auto limit = [](float value, uint32_t low, uint32_t high, float most) {
        if (high != 0 && value > float(high))
            value = float(high);
        if (value > most)
            value = most;
        return value < float(low) ? float(low) : value;
    };
    float titleBar = hasPageTitleBar(window) ? kTitleBarHeight : 0;
    width = limit(width, window->minExtent.x, window->maxExtent.x, host.x);
    height = limit(height, window->minExtent.y, window->maxExtent.y, host.y - titleBar);
    if (window->aspectRatio > 0)
        height = width / window->aspectRatio;
    // A resize keeps the edges that it does not move where they were.
    if ((edges & FrameEdge_Left) != 0)
        r.left = r.right - width;
    if ((edges & FrameEdge_Top) != 0)
        r.top = r.bottom - height;
    r.right = r.left + width;
    r.bottom = r.top + height;
    float dx = 0;
    float dy = 0;
    if (r.right < kKeptInHost)
        dx = kKeptInHost - r.right;
    else if (r.left > host.x - kKeptInHost)
        dx = host.x - kKeptInHost - r.left;
    if (r.top - titleBar < 0)
        dy = titleBar - r.top;
    else if (r.top > host.y - kKeptInHost)
        dy = host.y - kKeptInHost - r.top;
    r = PrRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

// Places a popup against its anchor on its parent, within the host.
void placePopup(WebWindow* window) {
    WebWindow* parent = window->parent;
    if (parent == nullptr)
        return;
    PrRect anchor = window->popup.anchor;
    PrRect onHost{
        anchor.left + parent->rect.left, anchor.top + parent->rect.top, anchor.right + parent->rect.left,
        anchor.bottom + parent->rect.top,
    };
    PrVec2 host = hostExtent(window->app);
    PrVec2 extent{window->rect.right - window->rect.left, window->rect.bottom - window->rect.top};
    PrAppPopupPlacement placement = window->popup;
    placement.anchor = onHost;
    window->rect = prAppPopupRect(&placement, extent, PrRect{0, 0, host.x, host.y}, PrAppLayoutDirection_LeftToRight);
}

// Centers a modal dialog on its parent, which it follows as a sheet.
void placeSheet(WebWindow* window) {
    const WebWindow* parent = window->parent;
    if (parent == nullptr)
        return;
    float width = window->rect.right - window->rect.left;
    float height = window->rect.bottom - window->rect.top;
    float left = (parent->rect.left + parent->rect.right - width) / 2;
    float top = (parent->rect.top + parent->rect.bottom - height) / 2;
    window->rect = PrRect{left, top, left + width, top + height};
}

// Places the popups and the sheets of `window` again, which move with it.
void placeChildren(WebWindow* window) {
    for (uint32_t i = 0; i < window->app->windows.count; ++i) {
        WebWindow* child = window->app->windows.data[i];
        if (child->parent != window)
            continue;
        if (child->role == PrAppWindowRole_Popup)
            placePopup(child);
        else if (child->role == PrAppWindowRole_Dialog && child->modal)
            placeSheet(child);
    }
}

// Places a new window: a popup against its anchor, a dialog centered on its parent, a window at the position that its
// description names, centered, or cascaded from the last window that opened at its Default place.
void placeNew(WebWindow* window, const PrAppWindowDesc* desc) {
    window->rect = PrRect{0, 0, float(desc->extent.x), float(desc->extent.y)};
    if (window->role == PrAppWindowRole_Popup) {
        placePopup(window);
        return;
    }
    // The extent is limited before the window is placed, so that a window larger than the host opens inside it.
    clamp(window, 0);
    PrVec2 extent{window->rect.right - window->rect.left, window->rect.bottom - window->rect.top};
    PrVec2 host = hostExtent(window->app);
    PrRect around{0, 0, host.x, host.y};
    if (window->parent != nullptr)
        around = window->parent->rect;
    PrVec2 at{(around.left + around.right - extent.x) / 2, (around.top + around.bottom - extent.y) / 2};
    if (desc->placement == PrAppWindowPlacement_Position) {
        at = desc->position.position;
    } else if (desc->placement == PrAppWindowPlacement_Default && window->parent == nullptr) {
        // The window cascades from the topmost window that floats before it.
        for (uint32_t i = window->app->windows.count; i-- > 0;) {
            WebWindow* previous = window->app->windows.data[i];
            if (previous != window && isFloating(previous) && previous->role == PrAppWindowRole_Normal) {
                at = PrVec2{previous->rect.left + kCascade, previous->rect.top + kCascade};
                break;
            }
        }
    }
    window->rect = PrRect{at.x, at.y, at.x + extent.x, at.y + extent.y};
    clamp(window, 0);
}

// Asks each popup to close but the one at `kept` and those that it belongs to, as for a press elsewhere. A null `kept`
// asks every popup.
void closePopupsExcept(WebApp* app, WebWindow* PR_NULLABLE kept) {
    RawArray<uint32_t> ids;
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        WebWindow* window = app->windows.data[i];
        bool keep = kept != nullptr && (window == kept || isDescendant(kept, window));
        if (window->role == PrAppWindowRole_Popup && window->shown && !keep)
            ids.push(window->id);
    }
    // A popup may close another as it closes, so each is found by its id.
    for (uint32_t i = 0; i < ids.count; ++i) {
        if (WebWindow* popup = windowWithId(ids.data[i]))
            deliver(popup, PrAppEvent{.kind = PrAppEventKind_CloseRequested, .time = now()});
    }
    ids.reset();
}

// Makes the window the one that has the keyboard, and shows its title as the page's.
void activate(WebWindow* window) {
    WebApp* app = window->app;
    WebWindow* previous = app->active;
    app->active = window;
    if (window->role == PrAppWindowRole_Normal) {
        PrStringView title = prRawStringView(&window->title);
        prAppWebSetTitle(title.data, title.size);
    }
    if (previous != nullptr && previous != window)
        requestFrame(previous);
    requestFrame(window);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window's functions

// Delivers the window's frame. The window's id is the context, so that a frame that is due after the window closes
// finds no window.
bool animationFrame(double, void* context) {
    WebWindow* window = windowWithId(uint32_t(reinterpret_cast<uintptr_t>(context)));
    if (window == nullptr)
        return false;
    window->frameRequested = false;
    if (!window->app->running)
        return false;
    PrAppFrame frame = frameNow(window);
    window->lastFrame = frame.time;
    PrAppOutput output{};
    if (!appWindowDeliverFrame(window->delivery, window, &frame, &output)) {
        windowDestroy(window);
        return false;
    }
    applyOutput(window, output);
    return false;
}

void windowDestroy(PrAppWindow* base) {
    WebWindow* window = windowOf(base);
    if (appWindowDeliveryDeferDestroy(window->delivery))
        return;
    WebApp* app = window->app;
    if (window->frameTimeout != 0)
        emscripten_clear_timeout(window->frameTimeout);
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        WebWindow* other = app->windows.data[i];
        // The dialogs and popups of a window that closes stay, with no parent.
        if (other->parent == window)
            other->parent = nullptr;
    }
    uint32_t index = app->windows.find(window);
    if (index != UINT32_MAX)
        app->windows.erase(index);
    if (app->active == window)
        app->active = nullptr;
    prAppWebWindowDestroy(window->id);
    appTitleBarRegionsReset(window->titleBar);
    prRawStringReset(&window->selector);
    prRawStringReset(&window->title);
    prMemoryFree(window);
}

PrApp* windowApp(const PrAppWindow* base) {
    return static_cast<const WebWindow*>(base)->app;
}

void requestFrame(PrAppWindow* base) {
    WebWindow* window = windowOf(base);
    if (window->frameRequested || !window->app->running)
        return;
    window->frameRequested = true;
    emscripten_request_animation_frame(animationFrame, reinterpret_cast<void*>(uintptr_t(window->id)));
}

void frameAtTime(void* context) {
    WebWindow* window = windowWithId(uint32_t(reinterpret_cast<uintptr_t>(context)));
    if (window == nullptr)
        return;
    window->frameTimeout = 0;
    requestFrame(window);
}

void requestFrameAt(PrAppWindow* base, double time) {
    WebWindow* window = windowOf(base);
    if (window->frameTimeout != 0)
        emscripten_clear_timeout(window->frameTimeout);
    window->frameTimeout = 0;
    if (time != 0) {
        double wait = (time - now()) * 1000;
        window->frameTimeout = emscripten_set_timeout(frameAtTime, wait > 0 ? wait : 0,
            reinterpret_cast<void*>(uintptr_t(window->id)));
    }
}

// Applies the output of the window's frame. The page places no input method windows, so the text input goes unused.
void applyOutput(WebWindow* window, const PrAppOutput& output) {
    PrAppCursor cursor = output.cursor < PrAppCursor_Count ? output.cursor : PrAppCursor(PrAppCursor_Arrow);
    PrStringView selector = prRawStringView(&window->selector);
    prAppWebSetCursor(selector.data, selector.size, kCursors[cursor].data, kCursors[cursor].size);
    appTitleBarRegionsSet(window->titleBar, output);
    if (output.wantsFrame)
        requestFrame(window);
    requestFrameAt(window, output.frameTime);
    window->dropOperation = output.dropOperation;
}

// Writes the canvas of `window` in the host, in CSS px, as the page lays it out.
PrRect boundsOf(WebWindow* window) {
    double bounds[4] = {};
    prAppWebWindowBounds(window->id, bounds);
    return PrRect{float(bounds[0]), float(bounds[1]), float(bounds[0] + bounds[2]), float(bounds[1] + bounds[3])};
}

// Returns the topmost window that takes the pointer at `at` in the host, or null.
WebWindow* PR_NULLABLE windowAt(WebApp* app, PrVec2 at) {
    for (uint32_t layer = 3; layer-- > 0;) {
        for (uint32_t i = app->windows.count; i-- > 0;) {
            WebWindow* window = app->windows.data[i];
            bool shown = window->shown && window->state != PrAppWindowState_Minimized && !window->passthrough;
            if (layerOf(window) != layer || !shown)
                continue;
            PrRect bounds = boundsOf(window);
            if (at.x >= bounds.left && at.x < bounds.right && at.y >= bounds.top && at.y < bounds.bottom)
                return window;
        }
    }
    return nullptr;
}

// Returns the drag's event for `window` at `at` in the host.
PrAppDragEvent dragEventOf(WebApp* app, WebWindow* window, PrVec2 at) {
    PrRect bounds = boundsOf(window);
    PrAppDataContent content = appDataStoreContent(app->dragContent);
    return PrAppDragEvent{
        .position = {at.x - bounds.left, at.y - bounds.top},
        .forms = content.forms,
        .operations = app->dragOperations,
        .fromThisWindow = window->id == app->dragSource,
        .payload = content.payload,
    };
}

// Ends the drag with `operation`, which the source learns from DragEnd.
void endDrag(WebApp* app, PrAppDragOperation operation) {
    WebWindow* source = windowWithId(app->dragSource);
    PrAppDataContent content = appDataStoreContent(app->dragContent);
    PrAppDragEvent drag{
        .forms = content.forms,
        .operations = app->dragOperations,
        .operation = operation,
        .fromThisWindow = true,
        .payload = content.payload,
    };
    app->dragSource = 0;
    app->dragOver = 0;
    if (source != nullptr)
        deliver(source, PrAppEvent{.kind = PrAppEventKind_DragEnd, .time = now(), .drag = drag});
    appDataStoreClear(app->dragContent);
}

// Moves the drag to `at` in the host: the window that it leaves receives DragLeave, the window that it enters
// DragEnter, and the window under it DragOver.
void moveDrag(WebApp* app, PrVec2 at, PrAppModifierFlags modifiers) {
    WebWindow* over = windowAt(app, at);
    uint32_t overId = over != nullptr ? over->id : 0;
    bool entered = overId != app->dragOver;
    if (entered) {
        if (WebWindow* left = windowWithId(app->dragOver)) {
            PrAppEvent leave{.kind = PrAppEventKind_DragLeave, .modifiers = modifiers, .time = now()};
            leave.drag = dragEventOf(app, left, at);
            deliver(left, leave);
        }
        app->dragOver = overId;
    }
    if ((over = windowWithId(overId)) == nullptr)
        return;
    PrAppEventKind kind = entered ? PrAppEventKind_DragEnter : PrAppEventKind_DragOver;
    PrAppEvent event{.kind = kind, .modifiers = modifiers, .time = now()};
    event.drag = dragEventOf(app, over, at);
    deliver(over, event);
    if ((over = windowWithId(overId)) != nullptr)
        requestFrame(over);
}

// Drops the drag at `at` in the host on the window under it, if that window's latest answer accepts it, and ends the
// drag.
void dropDrag(WebApp* app, PrVec2 at, PrAppModifierFlags modifiers) {
    WebWindow* target = windowAt(app, at);
    PrAppDragOperation operation = PrAppDragOperation_None;
    if (target != nullptr)
        operation = appDragAnswer(target->dropOperation, app->dragOperations);
    if (target != nullptr && operation != PrAppDragOperation_None) {
        // The drop's source holds a copy of the drag's content, for the target to read during the event.
        AppDataStore* store = allocZeroed<AppDataStore>();
        appDataStoreSet(*store, appDataStoreContent(app->dragContent));
        PrAppDataSource source = appDataSourceCreate(app->sources, store, nullptr);
        PrAppEvent drop{.kind = PrAppEventKind_Drop, .modifiers = modifiers, .time = now()};
        drop.drag = dragEventOf(app, target, at);
        drop.drag.operations = PrAppDragOperationFlags(1u << operation);
        drop.drag.operation = operation;
        drop.drag.drop = source;
        deliver(target, drop);
        appDataSourceRelease(app->sources, source);
    } else if (WebWindow* over = windowWithId(app->dragOver)) {
        PrAppEvent leave{.kind = PrAppEventKind_DragLeave, .modifiers = modifiers, .time = now()};
        leave.drag = dragEventOf(app, over, at);
        deliver(over, leave);
    }
    endDrag(app, operation);
}

// Starts the drag that a window asked for, from the page's event loop, never during a callback. A drag needs the press
// that began it, and ends at once with None without it. The source window's press is canceled, and the backend carries
// the drag between the page's windows from here. The page cannot start a drag of the browser's from a canvas, so the
// drag stays in the page.
void startDrag(void* PR_NULLABLE context) {
    WebWindow* window = windowWithId(uint32_t(reinterpret_cast<uintptr_t>(context)));
    if (window == nullptr)
        return;
    WebApp* app = window->app;
    if ((window->buttons & 1u) == 0 || !app->hasPointer) {
        endDrag(app, PrAppDragOperation_None);
        return;
    }
    window->buttons = 0;
    uint32_t id = window->id;
    PrAppEvent cancel{
        .kind = PrAppEventKind_PointerCancel,
        .time = now(),
        .pointer = {.position = window->pointer, .primary = true, .id = kPrAppPointerMouse},
    };
    deliver(window, cancel);
    if (windowWithId(id) != nullptr)
        moveDrag(app, app->pointerAt, 0);
}

void dragStart(PrAppWindow* base, const PrAppDragDesc* desc) {
    WebWindow* window = windowOf(base);
    WebApp* app = window->app;
    // A drag replaces the one in flight, which ends with None.
    if (app->dragSource != 0)
        endDrag(app, PrAppDragOperation_None);
    app->dragSource = window->id;
    app->dragOver = 0;
    app->dragOperations = desc->operations;
    appDataStoreSet(app->dragContent, desc->content);
    emscripten_async_call(startDrag, reinterpret_cast<void*>(uintptr_t(window->id)), 0);
}

void setTitle(PrAppWindow* base, PrStringView title) {
    WebWindow* window = windowOf(base);
    prRawStringReset(&window->title);
    window->title = prRawStringCopy(title);
    // The page's title follows the window that has the keyboard, or the only one.
    prAppWebWindowSetTitle(window->id, title.data, title.size);
    if (window->app->active == window || window->app->windows.count == 1)
        prAppWebSetTitle(title.data, title.size);
    requestFrame(window);
}

PrStringView title(const PrAppWindow* base) {
    return prRawStringView(&static_cast<const WebWindow*>(base)->title);
}

void setExtent(PrAppWindow* base, PrUVec2 extent) {
    WebWindow* window = windowOf(base);
    if (window->fills)
        return;
    PrRect& r = window->rect;
    r = PrRect{r.left, r.top, r.left + float(extent.x), r.top + float(extent.y)};
    clamp(window, 0);
    if (window->role == PrAppWindowRole_Popup)
        placePopup(window);
    restack(window->app);
    requestFrame(window);
}

void setExtentLimits(PrAppWindow* base, PrUVec2 minExtent, PrUVec2 maxExtent) {
    WebWindow* window = windowOf(base);
    window->minExtent = minExtent;
    window->maxExtent = maxExtent;
    if (window->fills)
        return;
    clamp(window, 0);
    restack(window->app);
    requestFrame(window);
}

void setAspectRatio(PrAppWindow* base, float ratio) {
    windowOf(base)->aspectRatio = ratio;
}

// The display is the host, and a window that fills it has no position of its own.
constexpr PrAppDisplayId kHostDisplay = 1;

// Returns the top left of the window's content in the host. A window that fills the host has its content at the host's
// origin, below the title bar that the page draws, if any.
PrVec2 contentOrigin(const WebWindow* window) {
    if (window->fills || window->state == PrAppWindowState_Fullscreen)
        return PrVec2{};
    if (window->state == PrAppWindowState_Maximized)
        return PrVec2{0, hasPageTitleBar(window) ? kTitleBarHeight : 0};
    return PrVec2{window->rect.left, window->rect.top};
}

bool position(const PrAppWindow* base, PrAppDisplayPosition* out) {
    const WebWindow* window = windowOf(base);
    if (window->fills)
        return false;
    *out = PrAppDisplayPosition{.display = kHostDisplay, .position = contentOrigin(window)};
    return true;
}

bool pointToDisplay(const PrAppWindow* base, PrVec2 point, PrAppDisplayPosition* out) {
    PrVec2 origin = contentOrigin(windowOf(base));
    *out = PrAppDisplayPosition{.display = kHostDisplay, .position = PrVec2{origin.x + point.x, origin.y + point.y}};
    return true;
}

bool pointFromDisplay(const PrAppWindow* base, const PrAppDisplayPosition* position, PrVec2* out) {
    if (position->display != kHostDisplay)
        return false;
    PrVec2 origin = contentOrigin(windowOf(base));
    *out = PrVec2{position->position.x - origin.x, position->position.y - origin.y};
    return true;
}

void setPosition(PrAppWindow* base, const PrAppDisplayPosition* at) {
    WebWindow* window = windowOf(base);
    if (window->fills || isFullscreen(window))
        return;
    if (window->state == PrAppWindowState_Maximized)
        window->state = PrAppWindowState_Normal;
    PrRect& r = window->rect;
    r = PrRect{at->position.x, at->position.y, at->position.x + r.right - r.left, at->position.y + r.bottom - r.top};
    clamp(window, 0);
    restack(window->app);
    requestFrame(window);
}

void setPopupPlacement(PrAppWindow* base, const PrAppPopupPlacement* placement) {
    WebWindow* window = windowOf(base);
    window->popup = *placement;
    placePopup(window);
    restack(window->app);
}

PrAppWindowState state(const PrAppWindow* base) {
    const WebWindow* window = windowOf(base);
    if (window->fills)
        return isFullscreen(window) ? PrAppWindowState(PrAppWindowState_Fullscreen) : PrAppWindowState_Normal;
    return window->state;
}

// The window of a Single application takes only Normal and Fullscreen, which asks the page to show its canvas on the
// machine's display. In any other window, Maximized and Fullscreen fill the host, and Minimized hides the window.
void setState(PrAppWindow* base, PrAppWindowState wanted) {
    WebWindow* window = windowOf(base);
    if (window->fills) {
        bool fullscreen = wanted == PrAppWindowState_Fullscreen;
        if (fullscreen != isFullscreen(window)) {
            PrStringView selector = prRawStringView(&window->selector);
            prAppWebSetFullscreen(selector.data, selector.size, fullscreen);
        }
    } else {
        window->state = wanted;
    }
    if (window->app->active == window && wanted == PrAppWindowState_Minimized)
        window->app->active = nullptr;
    restack(window->app);
    requestFrame(window);
}

void setVisible(PrAppWindow* base, bool visible) {
    WebWindow* window = windowOf(base);
    if (window->shown == visible)
        return;
    window->shown = visible;
    if (!visible && window->app->active == window)
        window->app->active = nullptr;
    restack(window->app);
    requestFrame(window);
}

bool isVisible(const PrAppWindow* base) {
    return windowOf(base)->shown;
}

// A hidden page shows none of its windows.
bool isOccluded(const PrAppWindow* base) {
    const WebWindow* window = windowOf(base);
    return !window->shown || window->state == PrAppWindowState_Minimized ||
        prAppWebLifecycle() == PrAppLifecycle_Background;
}

// A window is active while the page has focus and the window has the keyboard, which the window of a Single application
// always has.
bool isActive(const PrAppWindow* base) {
    const WebWindow* window = windowOf(base);
    return prAppWebHasFocus() && (window->fills || window->app->active == window);
}

void focus(PrAppWindow* base) {
    WebWindow* window = windowOf(base);
    window->shown = true;
    if (window->state == PrAppWindowState_Minimized)
        window->state = PrAppWindowState_Normal;
    raise(window);
    prAppWebFocus();
    prAppWebWindowFocus(window->id);
    activate(window);
}

// The page blurs what is behind a Blur window, and shows it through a transparent swap chain.
void setBackdrop(PrAppWindow* base, PrAppWindowBackdrop backdrop) {
    WebWindow* window = windowOf(base);
    window->backdrop = backdrop;
    restack(window->app);
    requestFrame(window);
}

void setOpacity(PrAppWindow* base, float opacity) {
    WebWindow* window = windowOf(base);
    window->opacity = opacity;
    restack(window->app);
}

// A window that keeps above stays above the page's other windows, which are all the page has.
void setKeepAbove(PrAppWindow* base, bool above) {
    WebWindow* window = windowOf(base);
    window->keepAbove = above;
    restack(window->app);
}

void setPointerPassthrough(PrAppWindow* base, bool passthrough) {
    WebWindow* window = windowOf(base);
    window->passthrough = passthrough;
    restack(window->app);
}

// The window's icon is the page's favicon. The pixels are copied row by row, packed, for the page to read.
void setIcon(PrAppWindow*, PrPixelView pixels) {
    if (pixels.data == nullptr || pixels.extent.x == 0 || pixels.extent.y == 0) {
        prAppWebSetIcon(nullptr, 0, 0);
        return;
    }
    size_t row = size_t(pixels.extent.x) * 4;
    uint8_t* packed = static_cast<uint8_t*>(prMemoryAlloc(row * pixels.extent.y));
    for (uint32_t y = 0; y < pixels.extent.y; ++y)
        prMemCopy(packed + row * y, static_cast<const uint8_t*>(pixels.data) + size_t(y) * pixels.stride, row);
    prAppWebSetIcon(packed, pixels.extent.x, pixels.extent.y);
    prMemoryFree(packed);
}

PrStatus surface(const PrAppWindow* base, PrSurface* out) {
    const WebWindow* window = windowOf(base);
    *out = PrSurface{
        .kind = PrSurfaceKind_HtmlCanvas,
        .handle = const_cast<char*>(prRawStringView(&window->selector).data),
    };
    return PrStatus_Ok;
}

// The page locks the pointer for Locked and LockedRaw, which asks the browser for movement without acceleration, and
// cannot confine it.
void setPointerMode(PrAppWindow* base, PrAppPointerMode mode) {
    WebWindow* window = windowOf(base);
    window->lockedMode = mode;
    bool locked = mode == PrAppPointerMode_Locked || mode == PrAppPointerMode_LockedRaw;
    PrStringView selector = prRawStringView(&window->selector);
    prAppWebSetPointerLock(selector.data, selector.size, locked, mode == PrAppPointerMode_LockedRaw);
}

PrAppPointerMode pointerMode(const PrAppWindow* base) {
    const WebWindow* window = windowOf(base);
    return window->locked ? window->lockedMode : PrAppPointerMode(PrAppPointerMode_Free);
}

// ---------------------------------------------------------------------------------------------------------------------
// Moves, resizes and the buttons of a window's frame

// Moves or resizes the window by the press that began on its frame, to the pointer at `at` in the host.
void dragFrame(WebWindow* window, PrVec2 at) {
    PrVec2 moved{at.x - window->pressAt.x, at.y - window->pressAt.y};
    PrRect r = window->pressRect;
    FrameEdges edges = window->moving;
    if ((edges & FrameEdge_Move) != 0) {
        r = PrRect{r.left + moved.x, r.top + moved.y, r.right + moved.x, r.bottom + moved.y};
        edges = 0;
    } else {
        if ((edges & FrameEdge_Left) != 0)
            r.left += moved.x;
        if ((edges & FrameEdge_Top) != 0)
            r.top += moved.y;
        if ((edges & FrameEdge_Right) != 0)
            r.right += moved.x;
        if ((edges & FrameEdge_Bottom) != 0)
            r.bottom += moved.y;
    }
    window->rect = r;
    clamp(window, edges);
    placeChildren(window);
    restack(window->app);
    requestFrame(window);
}

// Begins a move or a resize of the window by a press at `at` in the host. A maximized window that the user moves
// returns to Normal, under the pointer.
void beginFrameDrag(WebWindow* window, FrameEdges edges, PrVec2 at) {
    if (window->state == PrAppWindowState_Maximized && (edges & FrameEdge_Move) != 0) {
        float width = window->rect.right - window->rect.left;
        float height = window->rect.bottom - window->rect.top;
        window->rect = PrRect{at.x - width / 2, at.y, at.x + width / 2, at.y + height};
        window->state = PrAppWindowState_Normal;
    }
    window->moving = edges;
    window->pressAt = at;
    window->pressRect = window->rect;
}

// Acts on a button of the frame: Minimize, Maximize, which also restores, and Close, which asks the program.
void pressFrameButton(WebWindow* window, PrAppTitleBarRegionKind kind) {
    if (kind == PrAppTitleBarRegionKind_Minimize) {
        setState(window, PrAppWindowState_Minimized);
    } else if (kind == PrAppTitleBarRegionKind_Maximize) {
        bool maximized = window->state == PrAppWindowState_Maximized;
        setState(window, PrAppWindowState(maximized ? PrAppWindowState_Normal : PrAppWindowState_Maximized));
    } else if (kind == PrAppTitleBarRegionKind_Close) {
        deliver(window, PrAppEvent{.kind = PrAppEventKind_CloseRequested, .time = now()});
    }
}

// Takes a pointer event that lands on a Custom title bar's regions, other than Client, and returns true if it took it.
// A press on the drag region moves the window, and a double press maximizes it. A button acts when the press that began
// on it ends on it. Moves over a button still reach the program, for its hover state.
bool takeTitleBarPointer(WebWindow* window, const PrAppEvent& event) {
    const PrAppPointerEvent& pointer = event.pointer;
    PrVec2 at{window->rect.left + pointer.position.x, window->rect.top + pointer.position.y};
    if (event.kind == PrAppEventKind_PointerMove && window->moving != 0) {
        dragFrame(window, at);
        return true;
    }
    if (event.kind == PrAppEventKind_PointerUp && (window->moving != 0 || window->pressedButton != 0)) {
        PrAppTitleBarRegionKind pressed = window->pressedButton;
        window->moving = 0;
        window->pressedButton = PrAppTitleBarRegionKind_Client;
        if (pressed != PrAppTitleBarRegionKind_Client && appTitleBarRegionAt(window->titleBar, pointer.position) == pressed)
            pressFrameButton(window, pressed);
        return true;
    }
    if (event.kind != PrAppEventKind_PointerDown || pointer.button != 0 || !isFloating(window))
        return false;
    PrAppTitleBarRegionKind kind = appTitleBarRegionAt(window->titleBar, pointer.position);
    if (kind == PrAppTitleBarRegionKind_Client)
        return false;
    if (kind == PrAppTitleBarRegionKind_Drag && pointer.clicks == 2)
        pressFrameButton(window, PrAppTitleBarRegionKind_Maximize);
    else if (kind == PrAppTitleBarRegionKind_Drag)
        beginFrameDrag(window, FrameEdge_Move, at);
    else
        window->pressedButton = kind;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The page's events

PrAppModifierFlags modifiersOf(bool shift, bool control, bool alt, bool meta) {
    return PrAppModifierFlags((shift ? PrAppModifierFlag_Shift : 0) | (control ? PrAppModifierFlag_Control : 0) |
        (alt ? PrAppModifierFlag_Alt : 0) | (meta ? PrAppModifierFlag_Super : 0));
}

// Returns the modifiers that the page packs as Shift, Control, Alt and Meta from the lowest bit up.
PrAppModifierFlags modifiersOf(int packed) {
    return modifiersOf((packed & 1) != 0, (packed & 2) != 0, (packed & 4) != 0, (packed & 8) != 0);
}

// The page numbers its buttons primary, middle and secondary. The application numbers them primary, secondary and
// middle.
uint8_t buttonOf(unsigned short button) {
    return button == 1 ? 2 : button == 2 ? 1 : uint8_t(button);
}

// Returns the number of presses in the multi-click that a press of `button` at `position` continues or starts.
uint32_t countClick(WebWindow* window, uint8_t button, PrVec2 position, double time) {
    PrVec2 moved{position.x - window->clickPosition.x, position.y - window->clickPosition.y};
    bool continues = window->clicks != 0 && button == window->clickButton && time - window->clickTime <= kClickSeconds &&
        moved.x * moved.x + moved.y * moved.y <= kClickDistance * kClickDistance;
    window->clicks = continues ? window->clicks + 1 : 1;
    window->clickButton = button;
    window->clickTime = time;
    window->clickPosition = position;
    return window->clicks;
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

// Delivers a key to the window that has the keyboard.
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
    WebWindow* window = keyboardWindow(app);
    if (window == nullptr)
        return false;
    uint32_t id = window->id;
    deliver(window, event);
    window = windowWithId(id);
    if (window != nullptr && types && type == EMSCRIPTEN_EVENT_KEYDOWN)
        deliver(window, PrAppEvent{.kind = PrAppEventKind_Text, .time = event.time, .input = {.text = value}});
    // The page keeps the keys of its own shortcuts, and the application takes the rest. The paste shortcut is the
    // application's, which reads the clipboard itself, so the page fires no paste event for it.
    bool function = event.keyboard.key >= PrAppKey_F1 && event.keyboard.key <= PrAppKey_F20;
    bool paste = (e->ctrlKey || e->metaKey) && event.keyboard.key == 'V';
    return paste || (!e->ctrlKey && !e->metaKey && !function);
}

// Requests a frame of each window when the page gains or loses focus, which changes what prAppWindowIsActive returns,
// and when the page enters or leaves fullscreen, which changes prAppWindowState. Losing focus closes the popups.
void requestFrames(WebApp* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i)
        requestFrame(app->windows.data[i]);
}

bool focusChanged(int type, const EmscriptenFocusEvent*, void* context) {
    WebApp* app = appOf(context);
    if (type == EMSCRIPTEN_EVENT_BLUR)
        closePopupsExcept(app, nullptr);
    requestFrames(app);
    return false;
}

bool fullscreenChanged(int, const EmscriptenFullscreenChangeEvent*, void* context) {
    requestFrames(appOf(context));
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The application's functions

void keyLabelsReset(WebApp* app);

void destroy(PrApp* base) {
    WebApp* app = appOf(base);
    appDataRequestsReset(app->requests);
    appDataSourcesReset(app->sources);
    keyLabelsReset(app);
    app->windows.reset();
    appDataStoreReset(app->dragContent);
    prRawStringReset(&app->host);
    prMemoryFree(app);
}

void stop(PrApp* base);

void run(PrApp* base, PrAppHandler handler) {
    WebApp* app = appOf(base);
    app->handler = handler;
    app->running = true;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, key);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, key);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, focusChanged);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, app, false, focusChanged);
    emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, app, false, fullscreenChanged);
    prAppWebListenForPaste(true);
    prAppWebListenForLaunches(true);
    prAppWebListenForDisplays(true);
    prAppWebListenForPreferences(true);
    PrStringView host = prRawStringView(&app->host);
    prAppWebListenForHost(host.data, host.size, true);
    prAppWebListenForServices(true);
    app->lifecycle = PrAppLifecycle(prAppWebLifecycle());
    runningApp = app;
    if (!handler.start(handler.context, app))
        stop(app);
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
    prAppWebListenForPaste(false);
    prAppWebListenForLaunches(false);
    prAppWebListenForDisplays(false);
    prAppWebListenForPreferences(false);
    PrStringView host = prRawStringView(&app->host);
    prAppWebListenForHost(host.data, host.size, false);
    prAppWebListenForServices(false);
    prAppWebSetAwake(false);
    for (uint32_t i = 0; i < app->timers.count; ++i)
        emscripten_clear_timeout(app->timers.data[i].timeout);
    app->timers.reset();
    app->handler.shutdown(app->handler.context, app);
    // The windows that the program left open close with the run.
    while (app->windows.count != 0)
        prAppWindowDestroy(app->windows.data[app->windows.count - 1]);
}

void deliverWake(void* context) {
    WebApp* app = appOf(context);
    app->woken = false;
    if (app->running)
        deliverToApp(app, PrAppEvent{.kind = PrAppEventKind_Wake, .time = now()});
}

void wake(PrApp* base) {
    WebApp* app = appOf(base);
    if (app->woken)
        return;
    app->woken = true;
    emscripten_set_timeout(deliverWake, 0, app);
}

PrAppWindowModel windowModel(const PrApp* base) {
    return appOf(base)->windowModel;
}

// The page knows the pointer only over its windows, so the position is where a window last saw it.
bool pointerPosition(const PrApp* base, PrAppDisplayPosition* out) {
    const WebApp* app = appOf(base);
    if (!app->hasPointer)
        return false;
    *out = PrAppDisplayPosition{.display = kHostDisplay, .position = app->pointerAt};
    return true;
}

PrStatus notificationPost(PrApp* base, const PrAppNotificationDesc* desc) {
    if (!appOf(base)->running)
        return PrStatus_InvalidState;
    int shown =
        prAppWebNotify(desc->title.data, desc->title.size, desc->body.data, desc->body.size, uint32_t(desc->tag),
            uint32_t(desc->tag >> 32));
    if (shown == 1)
        return PrStatus_NotAllowed;
    return shown == 0 ? PrStatus_Ok : PrStatus_NotSupported;
}

PrStatus openUrl(PrApp*, PrStringView url) {
    prAppWebOpenUrl(url.data, url.size);
    return PrStatus_Ok;
}

// An answer that the backend gives from the page's event loop, never during a callback: content from a source's store,
// none with a status, or a dialog's source.
struct WebAnswer {
    uint32_t id;
    PrStatus status;
    PrAppDataSource source;
    bool fromSource;            // a request of content, which the source's store answers
    bool dialog;                // a dialog's answer, which hands over `source`
};

void answerNow(void* PR_NULLABLE context) {
    WebAnswer answer = *static_cast<WebAnswer*>(context);
    prMemoryFree(context);
    if (runningApp == nullptr)
        return;
    WebApp* app = runningApp;
    if (answer.dialog) {
        appDataRequestsAnswerSource(app->requests, answer.id, answer.status, answer.source);
        appDataSourceRelease(app->sources, answer.source);
        return;
    }
    AppDataStore* store = answer.fromSource ? appDataSourceStore(app->sources, answer.source) : nullptr;
    PrStatus status = answer.fromSource && store == nullptr ? PrStatus_NotFound : answer.status;
    PrAppDataSource unused = kPrAppDataSourceNone;
    AppDataAnswer kind = PrAppDataReadKind_Text;
    if (!appDataRequestsFind(app->requests, answer.id, &unused, &kind))
        return;
    if (kind == PrAppDataReadKind_File)
        appDataRequestsAnswerBytes(app->requests, answer.id, status, PrByteView{});
    else if (kind == AppDataAnswer_Status)
        appDataRequestsAnswerStatus(app->requests, answer.id, status);
    else
        appDataRequestsAnswerFromStore(app->requests, answer.id, status, store);
}

void answerSoon(WebAnswer answer) {
    WebAnswer* held = allocZeroed<WebAnswer>();
    *held = answer;
    emscripten_async_call(answerNow, held, 0);
}

// Answers a request of content: the page reads the clipboard's text and image, and answers through
// prAppWebClipboardTextRead or prAppWebClipboardImageRead. The page's clipboard holds no other form that it reveals,
// and the page has no selection. A source's store answers a read of the source.
void readContent(WebApp* app, uint32_t id, PrAppDataSource source, AppDataAnswer kind) {
    if (source == kPrAppDataSourceClipboard && (kind == PrAppDataReadKind_Text || kind == PrAppDataReadKind_Image)) {
        prAppWebClipboardRead(id, kind == PrAppDataReadKind_Image);
        return;
    }
    bool system = source == kPrAppDataSourceClipboard || source == kPrAppDataSourceSelection;
    bool supported = kind == PrAppDataReadKind_Files || source != kPrAppDataSourceClipboard;
    (void)app;
    answerSoon(WebAnswer{
        .id = id,
        .status = PrStatus(supported ? PrStatus_Ok : PrStatus_NotSupported),
        .source = source,
        .fromSource = !system && source != kPrAppDataSourceNone,
    });
}

uint32_t addRequest(PrApp* base, PrAppDataSource source, AppDataAnswer kind, void (*done)(void),
        void* PR_NULLABLE context, PrStringView type = {}) {
    return appDataRequestsAdd(appOf(base)->requests, source, kind, done, context, type);
}

void dataRetain(PrApp* base, PrAppDataSource source) {
    appDataSourceRetain(appOf(base)->sources, source);
}

void dataRelease(PrApp* base, PrAppDataSource source) {
    appDataSourceRelease(appOf(base)->sources, source);
}

// The page keeps a source's files, by the key that the source's handle holds, and reads a file's bytes as the program
// asks for them, answering through prAppWebFileRead.
void readFile(WebApp* app, uint32_t id, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size) {
    uintptr_t key = uintptr_t(appDataSourceNative(app->sources, source));
    if (key == 0) {
        answerSoon(WebAnswer{.id = id, .status = PrStatus_NotFound});
        return;
    }
    prAppWebReadFile(id, uint32_t(key), index, double(offset), double(size));
}

void dataRead(PrApp* base, const PrAppDataRead* read) {
    AppDataAnswer kind = read->kind;
    uint32_t id = addRequest(base, read->source, kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File)
        readFile(appOf(base), id, read->source, read->index, read->offset, read->size);
    else
        readContent(appOf(base), id, read->source, kind);
}

// The page writes the clipboard asynchronously, and reports only whether it can write an image. It writes the content's
// image if it has one, and its text otherwise.
PrStatus dataWrite(PrApp*, PrAppDataSource source, const PrAppDataContent* content) {
    if (source == kPrAppDataSourceSelection)
        return PrStatus_NotSupported;
    if ((content->forms & PrAppDataFormFlag_Image) == 0) {
        PrStringView text = (content->forms & PrAppDataFormFlag_Text) != 0 ? content->text : PrStringView("");
        prAppWebClipboardWrite(text.data, text.size);
        return PrStatus_Ok;
    }
    PrPixelView pixels = content->image;
    size_t row = size_t(pixels.extent.x) * 4;
    int result = 0;
    if (pixels.stride == row) {
        result = prAppWebClipboardWriteImage(pixels.data, pixels.extent.x, pixels.extent.y);
    } else {
        // The page reads the rows packed.
        uint8_t* packed = static_cast<uint8_t*>(prMemoryAlloc(row * pixels.extent.y));
        for (uint32_t y = 0; y < pixels.extent.y; ++y)
            prMemCopy(packed + y * row, pixels.data + y * pixels.stride, row);
        result = prAppWebClipboardWriteImage(packed, pixels.extent.x, pixels.extent.y);
        prMemoryFree(packed);
    }
    return result == 0 ? PrStatus_Ok : PrStatus_NotSupported;
}

// The page has no files of its own to write.
void dataWriteFile(PrApp* base, PrAppDataSource source, uint32_t, uint64_t, PrByteView, PrAppDataStatusFn done,
        void* PR_NULLABLE context) {
    uint32_t id = addRequest(base, source, AppDataAnswer_Status, (void (*)(void))done, context);
    answerSoon(WebAnswer{.id = id, .status = PrStatus_NotSupported});
}

void dataCancel(PrApp* base, void* PR_NULLABLE context) {
    appDataRequestsCancel(appOf(base)->requests, context);
}

// The page opens files through a file input, and has no dialog to save with or to choose directories. The input's
// extensions are the types', each with its dot. The page answers through prAppWebFilesChosen.
void fileDialog(PrApp* base, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    WebApp* app = appOf(base);
    uint32_t id = addRequest(base, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, context);
    bool directories = (desc->flags & PrAppFileDialogFlag_Directories) != 0;
    if (!app->running || desc->kind != PrAppFileDialogKind_Open || directories) {
        answerSoon(WebAnswer{.id = id, .status = PrStatus_NotSupported, .dialog = true});
        return;
    }
    PrRawString accept{};
    for (uint32_t i = 0; i < desc->typeCount; ++i) {
        PrStringView rest = desc->types[i].extensions;
        while (!rest.isEmpty()) {
            size_t comma = 0;
            while (comma < rest.size && rest.data[comma] != ',')
                ++comma;
            PrStringView extension = prStringTrim(PrStringView{rest.data, comma});
            if (!extension.isEmpty()) {
                if (prRawStringView(&accept).size != 0)
                    prRawStringAppend(&accept, ",");
                prRawStringAppend(&accept, ".");
                prRawStringAppend(&accept, extension);
            }
            rest = comma < rest.size ? PrStringView{rest.data + comma + 1, rest.size - comma - 1} : PrStringView{};
        }
    }
    PrStringView extensions = prRawStringView(&accept);
    prAppWebOpenFiles(id, (desc->flags & PrAppFileDialogFlag_Multiple) != 0, extensions.data, extensions.size);
    prRawStringReset(&accept);
}

// Shares through the Web Share API, which takes text and one URL. The page answers through prAppWebShared. A browser
// shares only while the page handles the user's input.
void share(PrAppWindow* window, const PrAppDataContent* content, PrRect, PrAppShareFn done, void* PR_NULLABLE context) {
    PrApp* app = windowOf(window)->app;
    uint32_t id = appDataRequestsAdd(appOf(app)->requests, kPrAppDataSourceNone, AppDataAnswer_Status,
        (void (*)(void))done, context);
    PrStringView text = (content->forms & PrAppDataFormFlag_Text) != 0 ? content->text : PrStringView{};
    bool hasUrl = (content->forms & PrAppDataFormFlag_Urls) != 0 && content->urlCount != 0;
    PrStringView url = hasUrl ? content->urls[0] : PrStringView{};
    bool shared = prAppWebShare(id, text.data, text.size, url.data, url.size);
    if (!shared)
        answerSoon(WebAnswer{.id = id, .status = PrStatus_NotSupported});
}

// ---------------------------------------------------------------------------------------------------------------------
// Services: timers, the lifecycle, power, key labels, haptics, the badge, sensors and orientations

void deliverTimer(void* context);

// Arms the timeout of a timer, which carries the timer's id.
void timerArm(WebTimer& timer, double seconds) {
    double milliseconds = seconds * 1000;
    timer.timeout = emscripten_set_timeout(deliverTimer, milliseconds > 0 ? milliseconds : 0,
        reinterpret_cast<void*>(uintptr_t(timer.id)));
}

// Delivers the firing of a timer. A repeating timer arms its next firing before the delivery, so that the program may
// set or cancel the tag during the event.
void deliverTimer(void* context) {
    WebApp* app = runningApp;
    if (app == nullptr)
        return;
    uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(context));
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        WebTimer& timer = app->timers.data[i];
        if (timer.id != id)
            continue;
        uint64_t tag = timer.tag;
        if (timer.interval > 0)
            timerArm(timer, timer.interval);
        else
            app->timers.erase(i);
        deliverToApp(app, PrAppEvent{.kind = PrAppEventKind_Timer, .time = now(), .tag = tag});
        return;
    }
}

void timerCancel(PrApp* base, uint64_t tag) {
    WebApp* app = appOf(base);
    for (uint32_t i = 0; i < app->timers.count; ++i) {
        if (app->timers.data[i].tag == tag) {
            emscripten_clear_timeout(app->timers.data[i].timeout);
            app->timers.erase(i);
            return;
        }
    }
}

void timerSet(PrApp* base, uint64_t tag, double delay, double interval) {
    WebApp* app = appOf(base);
    if (!app->running)
        return;
    timerCancel(base, tag);
    app->timers.push(WebTimer{.tag = tag, .id = ++app->lastTimerId, .interval = interval > 0 ? interval : 0});
    timerArm(app->timers.data[app->timers.count - 1], delay);
}

PrAppLifecycle lifecycle(const PrApp* base) {
    const WebApp* app = appOf(base);
    return app->running ? app->lifecycle : PrAppLifecycle(prAppWebLifecycle());
}

// The Battery Status API reports the battery's charge and whether it charges, which a device without a battery reports
// as full and charging.
PrAppPower power(const PrApp*) {
    double values[3] = {0, -1, 0};
    prAppWebPower(values);
    return PrAppPower{.source = PrAppPowerSource(values[0]), .level = float(values[1]), .charging = values[2] != 0};
}

// The page keeps only the display awake, through the Screen Wake Lock API.
void setAwake(PrApp*, PrAppAwakeFlags flags) {
    prAppWebSetAwake((flags & PrAppAwakeFlag_Display) != 0);
}

void keyLabelsReset(WebApp* app) {
    for (uint32_t i = 0; i < PrAppKeyCode_Count; ++i) {
        prRawStringReset(&app->keyLabels[i]);
        app->keyLabeled[i] = false;
    }
}

// Returns the label of the key at `code` from the Keyboard Map API, where the browser offers it, or the key's name.
PrStringView keyCodeLabel(const PrApp* base, PrAppKeyCode code) {
    const WebApp* app = appOf(base);
    if (code >= PrAppKeyCode_Count)
        return prAppKeyCodeName(PrAppKeyCode_Unidentified);
    if (!app->keyLabeled[code]) {
        PrStringView name = prAppKeyCodeName(code);
        char label[16];
        int size = prAppWebKeyLabel(name.data, name.size, label, int(sizeof(label)));
        // A layout map that is still loading leaves the label to be computed again.
        if (size >= 0) {
            app->keyLabeled[code] = true;
            prRawStringReset(&app->keyLabels[code]);
            if (size > 0)
                app->keyLabels[code] = prRawStringCopy(PrStringView{label, size_t(size)});
        }
    }
    if (!prRawStringIsEmpty(&app->keyLabels[code]))
        return prRawStringView(&app->keyLabels[code]);
    return prAppKeyCodeName(code);
}

void hapticPlay(PrApp*, PrAppHaptic haptic) {
    prAppWebVibrate(int(haptic));
}

void setBadge(PrApp*, PrStringView badge) {
    prAppWebSetBadge(badge.data, badge.size);
}

PrStatus sensorEnable(PrApp*, PrAppSensor sensor, bool enable, double interval) {
    return prAppWebSensor(int(sensor), enable, interval) == 0 ? PrStatus_Ok : PrStatus_NotSupported;
}

void setOrientations(PrApp*, PrAppOrientationFlags orientations) {
    prAppWebLockOrientation(int(orientations));
}

// Makes a source of the files that the page keeps under `key`: `count` files, whose names are in `names`, `nameBytes`
// bytes of names each ended by a NUL, and whose sizes are in `sizes`. The source's handle is the key, and the page lets
// the files go once the source's last reference goes.
PrAppDataSource fileSourceOf(WebApp* app, uint32_t key, const char* names, uint32_t nameBytes, const double* sizes,
        uint32_t count, AppDataStore* store) {
    uint32_t start = 0;
    uint32_t index = 0;
    for (uint32_t i = 0; i < nameBytes && index < count; ++i) {
        if (names[i] != '\0')
            continue;
        appDataStoreAddFile(*store, PrAppDataFile{
            .name = PrStringView{names + start, i - start},
            .size = uint64_t(sizes[index]),
        });
        start = i + 1;
        ++index;
    }
    return appDataSourceCreate(app->sources, store, reinterpret_cast<void*>(uintptr_t(key)));
}

// Returns the status of a clipboard read that the page reports as `result`: 0 for a read, 1 for a refusal, and 2 for a
// page that cannot read.
PrStatus statusOfRead(int result) {
    switch (result) {
        case 0: return PrStatus_Ok;
        case 1: return PrStatus_NotAllowed;
        default: return PrStatus_NotSupported;
    }
}

// The page's one display is the host, whose whole extent is its work area. Returns the page's only display: the host,
// which the windows of a Multiple application float in.
PrAppDisplayInfo hostDisplay(const PrApp* base) {
    PrVec2 extent = hostExtent(appOf(base));
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = float(emscripten_get_device_pixel_ratio());
    return PrAppDisplayInfo{
        .id = kHostDisplay,
        .display = display,
        .extent = extent,
        .workArea = PrRect{0, 0, extent.x, extent.y},
        .main = true,
    };
}

uint32_t displays(const PrApp* base, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity) {
    if (capacity != 0)
        out[0] = hostDisplay(base);
    return 1;
}

bool displayInfo(const PrApp* base, PrAppDisplayId id, PrAppDisplayInfo* out) {
    if (id != kHostDisplay)
        return false;
    *out = hostDisplay(base);
    return true;
}

PrStatus windowCreate(PrApp* base, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    WebApp* app = appOf(base);
    if (!app->running)
        return PrStatus_InvalidState;
    if (app->windowModel == PrAppWindowModel_Single && app->windows.count != 0)
        return PrStatus_NotSupported;
    WebWindow* window = allocZeroed<WebWindow>();
    window->destroyFn = windowDestroy;
    window->appFn = windowApp;
    window->requestFrameFn = requestFrame;
    window->setTitleFn = setTitle;
    window->titleFn = title;
    window->setExtentFn = setExtent;
    window->setExtentLimitsFn = setExtentLimits;
    window->setAspectRatioFn = setAspectRatio;
    window->positionFn = position;
    window->setPositionFn = setPosition;
    window->pointToDisplayFn = pointToDisplay;
    window->pointFromDisplayFn = pointFromDisplay;
    window->setPopupPlacementFn = setPopupPlacement;
    window->setStateFn = setState;
    window->stateFn = state;
    window->setVisibleFn = setVisible;
    window->isVisibleFn = isVisible;
    window->isOccludedFn = isOccluded;
    window->isActiveFn = isActive;
    window->focusFn = focus;
    window->setBackdropFn = setBackdrop;
    window->setOpacityFn = setOpacity;
    window->setKeepAboveFn = setKeepAbove;
    window->setPointerPassthroughFn = setPointerPassthrough;
    window->setPointerModeFn = setPointerMode;
    window->pointerModeFn = pointerMode;
    window->dragStartFn = dragStart;
    window->shareFn = share;
    window->surfaceFn = surface;
    window->setIconFn = setIcon;
    window->app = app;
    window->delivery.handler = desc->handler;
    window->id = ++app->lastWindowId;
    window->selector = prRawStringCopy("#prax-window-");
    prRawStringAppendU64(&window->selector, window->id, 10);
    window->title = prRawStringCopy(desc->title);
    window->parent = static_cast<WebWindow*>(desc->parent);
    window->role = desc->role;
    window->decoration = desc->decoration;
    window->modal = desc->modal;
    window->fills = app->windowModel == PrAppWindowModel_Single;
    window->popup = desc->popup;
    window->minExtent = desc->minExtent;
    window->maxExtent = desc->maxExtent;
    window->shown = !desc->hidden;
    window->passthrough = desc->pointerPassthrough;
    window->opacity = 1;
    window->backdrop = desc->backdrop;
    // A modal dialog's parent dims under it. A dialog with decoration is a sheet, and one without is the program's own
    // drawing.
    uint32_t sheetParent = window->modal && window->parent != nullptr ? window->parent->id : 0;
    bool framed = desc->decoration != PrAppWindowDecoration_None;
    PrStringView host = prRawStringView(&app->host);
    bool created = prAppWebWindowCreate(host.data, host.size, window->id, desc->title.data, desc->title.size,
        hasPageTitleBar(window), sheetParent, framed);
    if (!created) {
        prRawStringReset(&window->selector);
        prRawStringReset(&window->title);
        prMemoryFree(window);
        return PrStatus_NotFound;
    }
    app->windows.push(window);
    if (!window->fills)
        placeNew(window, desc);
    if (desc->state != PrAppWindowState_Normal && window->shown)
        setState(window, desc->state);
    if (window->shown && window->role != PrAppWindowRole_Popup && window->role != PrAppWindowRole_Overlay)
        focus(window);
    else
        restack(app);
    if (window->app->windows.count == 1)
        setTitle(window, desc->title);
    requestFrame(window);
    *out = window;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAppWebOptions prAppWebOptionsDefault(void) {
    return PrAppWebOptions{};
}

PrApp* prAppWebCreate(const PrAppOptions* options, const PrAppWebOptions* PR_NULLABLE web) {
    WebApp* app = allocZeroed<WebApp>();
    app->destroyFn = destroy;
    app->runFn = run;
    app->stopFn = stop;
    app->wakeFn = wake;
    app->windowModelFn = windowModel;
    app->pointerPositionFn = pointerPosition;
    app->notificationPostFn = notificationPost;
    app->openUrlFn = openUrl;
    app->cancelFn = dataCancel;
    app->dataRetainFn = dataRetain;
    app->dataReleaseFn = dataRelease;
    app->dataReadFn = dataRead;
    app->dataWriteFn = dataWrite;
    app->dataWriteFileFn = dataWriteFile;
    app->windowCreateFn = windowCreate;
    app->displaysFn = displays;
    app->displayInfoFn = displayInfo;
    app->fileDialogFn = fileDialog;
    app->lifecycleFn = lifecycle;
    app->timerSetFn = timerSet;
    app->timerCancelFn = timerCancel;
    app->keyCodeLabelFn = keyCodeLabel;
    app->setBadgeFn = setBadge;
    app->powerFn = power;
    app->setAwakeFn = setAwake;
    app->sensorEnableFn = sensorEnable;
    app->hapticPlayFn = hapticPlay;
    app->setOrientationsFn = setOrientations;
    app->windowModel = options->windowModel;
    PrStringView host = web != nullptr ? web->host : PrStringView{};
    app->host = prRawStringCopy(host.isEmpty() ? PrStringView("body") : host);
    appDataSourcesInit(app->sources);
    app->sources.releaseNative = [](void* native) { prAppWebFileSetRelease(uint32_t(uintptr_t(native))); };
    appDataRequestsInit(app->requests, app->sources);
    return app;
}

void prAppWebSetHostFullscreen(PrApp* app, bool fullscreen) {
    PrStringView host = prRawStringView(&appOf(app)->host);
    prAppWebHostSetFullscreen(host.data, host.size, fullscreen);
}

bool prAppWebIsHostFullscreen(PrApp* app) {
    PrStringView host = prRawStringView(&appOf(app)->host);
    return prAppWebHostIsFullscreen(host.data, host.size);
}

// The page calls these functions by name, so the build keeps them.
EMSCRIPTEN_KEEPALIVE void prAppWebNotificationClicked(uint32_t tagLow, uint32_t tagHigh) {
    if (runningApp == nullptr)
        return;
    deliverToApp(runningApp, PrAppEvent{
        .kind = PrAppEventKind_Notification,
        .time = now(),
        .tag = (uint64_t(tagHigh) << 32) | tagLow,
    });
}

EMSCRIPTEN_KEEPALIVE void prAppWebClipboardTextRead(uint32_t id, int result, const char* PR_NULLABLE text) {
    if (runningApp == nullptr)
        return;
    AppDataStore store{};
    appDataStoreSetText(store, text != nullptr ? PrStringView(text) : PrStringView(""));
    appDataRequestsAnswerFromStore(runningApp->requests, id, statusOfRead(result), &store);
    appDataStoreReset(store);
}

EMSCRIPTEN_KEEPALIVE void prAppWebClipboardImageRead(uint32_t id, int result, const uint8_t* PR_NULLABLE pixels,
        uint32_t width, uint32_t height) {
    if (runningApp == nullptr)
        return;
    AppDataStore store{};
    if (pixels != nullptr)
        appDataStoreSetImage(store, PrPixelView{pixels, size_t(width) * 4, PrUVec2{width, height}});
    appDataRequestsAnswerFromStore(runningApp->requests, id, statusOfRead(result), &store);
    appDataStoreReset(store);
}

EMSCRIPTEN_KEEPALIVE void prAppWebPointer(uint32_t windowId, int kind, int type, int32_t id, bool primary, double x,
        double y, int button, double pressure, double tiltX, double tiltY, double twist, double width, double height,
        int modifiers, double movementX, double movementY) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return;
    constexpr PrAppEventKind kKinds[] = {
        PrAppEventKind_PointerMove, PrAppEventKind_PointerDown, PrAppEventKind_PointerUp, PrAppEventKind_PointerCancel,
        PrAppEventKind_PointerEnter, PrAppEventKind_PointerLeave,
    };
    constexpr PrAppPointerType kTypes[] = {PrAppPointerType_Mouse, PrAppPointerType_Pen, PrAppPointerType_Touch};
    // A locked pointer stays where the lock began, and crosses nothing.
    PrAppEventKind eventKind = kKinds[kind >= 0 && kind < 6 ? kind : 0];
    bool crossing = eventKind == PrAppEventKind_PointerEnter || eventKind == PrAppEventKind_PointerLeave;
    if (window->locked && crossing)
        return;
    PrVec2 position = window->locked ? window->pointer : PrVec2{float(x), float(y)};
    window->pointer = position;
    PrAppEvent event{
        .kind = eventKind,
        .modifiers = modifiersOf(modifiers),
        .time = now(),
        .pointer = {
            .position = position,
            .type = kTypes[type >= 0 && type < 3 ? type : 0],
            .primary = primary,
            .pressure = float(pressure),
            .tilt = PrVec2{float(tiltX), float(tiltY)},
            .twist = float(twist),
        },
    };
    WebApp* app = window->app;
    PrRect bounds = boundsOf(window);
    app->pointerAt = PrVec2{bounds.left + position.x, bounds.top + position.y};
    app->hasPointer = true;
    // A drag that the window started takes its pointer until the press ends.
    if (app->dragSource == windowId && primary) {
        if (eventKind == PrAppEventKind_PointerMove)
            moveDrag(app, app->pointerAt, event.modifiers);
        else if (eventKind == PrAppEventKind_PointerUp)
            dropDrag(app, app->pointerAt, event.modifiers);
        else if (eventKind == PrAppEventKind_PointerCancel)
            endDrag(app, PrAppDragOperation_None);
        return;
    }
    // A move reports no button. The page's ids for pens and touches are moved past the mouse's.
    PrAppPointerEvent& pointer = event.pointer;
    if (event.kind != PrAppEventKind_PointerMove)
        pointer.button = buttonOf(uint16_t(button > 0 ? button : 0));
    pointer.id = pointer.type == PrAppPointerType_Mouse ? kPrAppPointerMouse : uint32_t(id) + 2;
    // Only a touch has a contact area. The page reports one pixel for any other pointer.
    if (pointer.type == PrAppPointerType_Touch)
        pointer.contactSize = PrVec2{float(width), float(height)};
    if (event.kind == PrAppEventKind_PointerDown && pointer.button < 32)
        window->buttons |= 1u << pointer.button;
    else if (event.kind == PrAppEventKind_PointerUp && pointer.button < 32)
        window->buttons &= ~(1u << pointer.button);
    else if (event.kind == PrAppEventKind_PointerCancel)
        window->buttons = 0;
    if (event.kind == PrAppEventKind_PointerDown) {
        pointer.clicks = countClick(window, pointer.button, pointer.position, event.time);
        // A press closes the popups that it is not in, and brings its window forward with the keyboard. A popup takes
        // no keyboard.
        closePopupsExcept(window->app, window);
        window = windowWithId(windowId);
        if (window == nullptr)
            return;
        // The canvas takes the page's focus before the press arrives, so the press raises its window whether or not it
        // has the keyboard already.
        if (window->role != PrAppWindowRole_Popup)
            focus(window);
    }
    if (event.kind == PrAppEventKind_PointerMove)
        pointer.movement = PrVec2{float(movementX), float(movementY)};
    if (takeTitleBarPointer(window, event))
        return;
    deliver(window, event);
}

EMSCRIPTEN_KEEPALIVE void prAppWebPointerLockChanged(uint32_t windowId) {
    WebApp* app = runningApp;
    for (uint32_t i = 0; app != nullptr && i < app->windows.count; ++i) {
        WebWindow* window = app->windows.data[i];
        bool locked = window->id == windowId;
        if (window->locked == locked)
            continue;
        window->locked = locked;
        requestFrame(window);
    }
}

EMSCRIPTEN_KEEPALIVE void prAppWebWheel(uint32_t windowId, double x, double y, double deltaX, double deltaY, int mode,
        int modifiers) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return;
    PrVec2 position{float(x), float(y)};
    // Browsers report a trackpad's pinch as a wheel with Control held, whose delta is the change of scale in
    // hundredths, shrinking for a positive delta.
    if ((modifiers & 2) != 0 && mode == 0) {
        PrAppEvent pinch{
            .kind = PrAppEventKind_Gesture,
            .time = now(),
            .gesture = {
                .kind = PrAppGestureKind_Magnify,
                .phase = PrAppGesturePhase_Change,
                .position = position,
                .magnification = float(-deltaY) / 100.0f,
            },
        };
        deliver(window, pinch);
        return;
    }
    PrAppEvent event{
        .kind = PrAppEventKind_Scroll,
        .modifiers = modifiersOf(modifiers),
        .time = now(),
        .scroll = {
            .position = position,
            .delta = PrVec2{float(deltaX), float(deltaY)},
            .unit = PrAppScrollUnit(mode == 0 ? PrAppScrollUnit_Pixel : PrAppScrollUnit_Line),
        },
    };
    // A page of scrolling is a screenful of lines.
    if (mode == 2) {
        float lines = (window->rect.bottom - window->rect.top) / kPrAppScrollLineHeight;
        if (window->fills || window->state == PrAppWindowState_Maximized)
            lines = hostExtent(window->app).y / kPrAppScrollLineHeight;
        event.scroll.delta = PrVec2{event.scroll.delta.x * lines, event.scroll.delta.y * lines};
    }
    deliver(window, event);
}

EMSCRIPTEN_KEEPALIVE void prAppWebWindowFocused(uint32_t windowId, bool focused) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return;
    if (focused) {
        activate(window);
    } else if (window->app->active == window) {
        window->app->active = nullptr;
        requestFrame(window);
    }
}

EMSCRIPTEN_KEEPALIVE void prAppWebFrameDrag(uint32_t windowId, int edges, int phase, double x, double y) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr || !isFloating(window))
        return;
    PrVec2 at{float(x), float(y)};
    if (phase == 0) {
        closePopupsExcept(window->app, window);
        if ((window = windowWithId(windowId)) == nullptr)
            return;
        focus(window);
        beginFrameDrag(window, FrameEdges(edges), at);
    } else if (window->moving != 0) {
        dragFrame(window, at);
        if (phase == 2)
            window->moving = 0;
    }
}

EMSCRIPTEN_KEEPALIVE void prAppWebFrameButton(uint32_t windowId, int kind) {
    if (WebWindow* window = windowWithId(windowId))
        pressFrameButton(window, PrAppTitleBarRegionKind(kind));
}

EMSCRIPTEN_KEEPALIVE void prAppWebWindowRestore(uint32_t windowId) {
    if (WebWindow* window = windowWithId(windowId))
        focus(window);
}

EMSCRIPTEN_KEEPALIVE void prAppWebPressOutside(void) {
    if (runningApp != nullptr)
        closePopupsExcept(runningApp, nullptr);
}

EMSCRIPTEN_KEEPALIVE void prAppWebFilesChosen(uint32_t id, uint32_t key, const char* names, uint32_t nameBytes,
        const double* sizes, uint32_t count) {
    if (runningApp == nullptr)
        return;
    // A cancel answers None.
    PrAppDataSource source = kPrAppDataSourceNone;
    if (count != 0) {
        AppDataStore* store = allocZeroed<AppDataStore>();
        source = fileSourceOf(runningApp, key, names, nameBytes, sizes, count, store);
    }
    appDataRequestsAnswerSource(runningApp->requests, id, PrStatus_Ok, source);
    appDataSourceRelease(runningApp->sources, source);
}

EMSCRIPTEN_KEEPALIVE void prAppWebLaunched(const char* url, uint32_t key, const char* names, uint32_t nameBytes,
        const double* sizes, uint32_t count) {
    WebApp* app = runningApp;
    if (app == nullptr)
        return;
    AppDataStore* store = allocZeroed<AppDataStore>();
    if (PrStringView(url).size != 0)
        appDataStoreAddUrl(*store, url);
    PrAppDataSource source = fileSourceOf(app, key, names, nameBytes, sizes, count, store);
    PrAppEvent event{.kind = PrAppEventKind_Open, .time = now()};
    event.open.source = source;
    deliverToApp(app, event);
    appDataSourceRelease(app->sources, source);
}

EMSCRIPTEN_KEEPALIVE void prAppWebFileRead(uint32_t id, int result, const uint8_t* PR_NULLABLE bytes, uint32_t size) {
    if (runningApp == nullptr)
        return;
    PrStatus status = result == 0 ? PrStatus_Ok : result == 1 ? PrStatus_NotFound : PrStatus_Io;
    appDataRequestsAnswerBytes(runningApp->requests, id, status, PrByteView{bytes, size});
}

EMSCRIPTEN_KEEPALIVE void prAppWebDisplaysChanged(void) {
    WebApp* app = runningApp;
    if (app == nullptr)
        return;
    deliverToApp(app, PrAppEvent{.kind = PrAppEventKind_DisplaysChanged, .time = now()});
    if (runningApp == nullptr)
        return;
    // The windows stay in the host, and the popups against their anchors.
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        WebWindow* window = app->windows.data[i];
        if (isFloating(window))
            clamp(window, 0);
    }
    for (uint32_t i = 0; i < app->windows.count; ++i)
        placeChildren(app->windows.data[i]);
    restack(app);
    requestFrames(app);
}

EMSCRIPTEN_KEEPALIVE void prAppWebPreferencesChanged(void) {
    if (runningApp != nullptr)
        requestFrames(runningApp);
}

EMSCRIPTEN_KEEPALIVE void prAppWebLifecycleChanged(void) {
    WebApp* app = runningApp;
    if (app == nullptr)
        return;
    PrAppLifecycle lifecycle = PrAppLifecycle(prAppWebLifecycle());
    if (lifecycle == app->lifecycle)
        return;
    app->lifecycle = lifecycle;
    deliverToApp(app, PrAppEvent{.kind = PrAppEventKind_LifecycleChanged, .time = now()});
}

EMSCRIPTEN_KEEPALIVE void prAppWebAppEvent(int notice) {
    WebApp* app = runningApp;
    if (app == nullptr)
        return;
    static const PrAppEventKind kKinds[] = {
        PrAppEventKind_Suspend,
        PrAppEventKind_Resume,
        PrAppEventKind_LocaleChanged,
        PrAppEventKind_PowerChanged,
        PrAppEventKind_KeyboardLayoutChanged,
    };
    if (notice < 0 || size_t(notice) >= countOf(kKinds))
        return;
    if (kKinds[notice] == PrAppEventKind_KeyboardLayoutChanged)
        keyLabelsReset(app);
    deliverToApp(app, PrAppEvent{.kind = kKinds[notice], .time = now()});
}

EMSCRIPTEN_KEEPALIVE void prAppWebShared(uint32_t id, int result) {
    if (runningApp == nullptr)
        return;
    PrStatus status = PrStatus_Ok;
    if (result == 1)
        status = PrStatus_NotAllowed;
    else if (result != 0)
        status = PrStatus_Io;
    appDataRequestsAnswerStatus(runningApp->requests, id, status);
}

EMSCRIPTEN_KEEPALIVE void prAppWebSensorReading(int sensor, double x, double y, double z, double w) {
    if (runningApp == nullptr)
        return;
    PrAppEvent event{.kind = PrAppEventKind_Sensor, .time = now()};
    event.sensor = PrAppSensorEvent{.sensor = PrAppSensor(sensor), .values = {float(x), float(y), float(z), float(w)}};
    deliverToApp(runningApp, event);
}

EMSCRIPTEN_KEEPALIVE void prAppWebGesture(uint32_t windowId, int kind, int phase, double x, double y,
        double magnification, double rotation) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return;
    PrAppEvent event{.kind = PrAppEventKind_Gesture, .time = now()};
    event.gesture = PrAppGestureEvent{
        .kind = PrAppGestureKind(kind),
        .phase = PrAppGesturePhase(phase),
        .position = PrVec2{float(x), float(y)},
        .magnification = float(magnification),
        .rotation = float(rotation),
    };
    deliver(window, event);
}

EMSCRIPTEN_KEEPALIVE void prAppWebPasted(const char* text) {
    if (runningApp == nullptr)
        return;
    if (WebWindow* window = keyboardWindow(runningApp))
        deliver(window, PrAppEvent{.kind = PrAppEventKind_Paste, .time = now(), .input = {.text = text}});
}

EMSCRIPTEN_KEEPALIVE int prAppWebDrag(uint32_t windowId, int kind, double x, double y, int content, int operations,
        int modifiers) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return PrAppDragOperation_None;
    constexpr PrAppEventKind kKinds[] = {PrAppEventKind_DragEnter, PrAppEventKind_DragOver, PrAppEventKind_DragLeave};
    deliver(window, PrAppEvent{
        .kind = kKinds[kind >= 0 && kind < 3 ? kind : 1],
        .modifiers = modifiersOf(modifiers),
        .time = now(),
        .drag = {
            .position = PrVec2{float(x), float(y)},
            .forms = PrAppDataFormFlags(content),
            .operations = PrAppDragOperationFlags(operations),
        },
    });
    if ((window = windowWithId(windowId)) == nullptr)
        return PrAppDragOperation_None;
    requestFrame(window);
    return prAppWebDropAnswer(windowId, operations);
}

EMSCRIPTEN_KEEPALIVE int prAppWebDropAnswer(uint32_t windowId, int operations) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return PrAppDragOperation_None;
    return appDragAnswer(window->dropOperation, PrAppDragOperationFlags(operations));
}

EMSCRIPTEN_KEEPALIVE void prAppWebDrop(uint32_t windowId, double x, double y, int operation, int modifiers,
        const char* text, uint32_t key, const char* names, uint32_t nameBytes, const double* sizes, uint32_t count) {
    WebWindow* window = windowWithId(windowId);
    if (window == nullptr)
        return;
    WebApp* app = window->app;
    AppDataStore* store = allocZeroed<AppDataStore>();
    PrStringView dropped = text;
    if (dropped.size != 0)
        appDataStoreSetText(*store, dropped);
    // The page keeps the files, whose bytes it reads as the program asks for them.
    PrAppDataSource source = fileSourceOf(app, key, names, nameBytes, sizes, count, store);
    PrAppDataFormFlags forms = store->forms;
    deliver(window, PrAppEvent{
        .kind = PrAppEventKind_Drop,
        .modifiers = modifiersOf(modifiers),
        .time = now(),
        .drag = {
            .position = PrVec2{float(x), float(y)},
            .forms = forms,
            .operations = PrAppDragOperationFlags(1u << (operation & 3)),
            .operation = PrAppDragOperation(operation & 3),
            .drop = source,
        },
    });
    appDataSourceRelease(app->sources, source);
    if ((window = windowWithId(windowId)) != nullptr)
        requestFrame(window);
}
