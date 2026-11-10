// The events that an application and its windows receive from the system and the user.

#pragma once

#include "pr/app_application.h"
#include "pr/app_data.h"
#include "pr/app_device.h"
#include "pr/app_input.h"
#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of event. A window receives the pointer, scroll, key, drag and window events. A menu sends its events to
// the window that shows it, or to the application for the menu bar and status items. The application receives the rest.
enum {
    // --- Pointers ---
    //
    // These follow Pointer Events (pointerevents3). PointerCancel means that the system took over the pointer, and no
    // PointerUp follows it. PointerEnter comes before the first move over the window's content, and PointerLeave after
    // the last. A custom title bar is part of the content. If the pointer leaves the window with a button down, the
    // window still receives its moves and its PointerUp, and PointerLeave follows the PointerUp.
    PrAppEventKind_PointerMove,
    PrAppEventKind_PointerDown,
    PrAppEventKind_PointerUp,
    PrAppEventKind_PointerCancel,
    PrAppEventKind_PointerEnter,
    PrAppEventKind_PointerLeave,

    // --- Scrolling and gestures ---
    //
    // Gesture reports a gesture that the system recognized, such as a pinch. TrackpadTouches reports the touches on a
    // trackpad each time they change, while the pointer is over the window.
    PrAppEventKind_Scroll,
    PrAppEventKind_Gesture,
    PrAppEventKind_TrackpadTouches,

    // --- Keys and text ---
    //
    // Text carries the text that a key typed. Paste carries text that the user pasted from a system menu. An input
    // method's composition begins with the first CompositionUpdate, and each update carries the whole composed text.
    // CompositionEnd carries the committed text, which is empty if the user canceled the composition. A Text event with
    // the committed text follows it.
    PrAppEventKind_KeyDown,
    PrAppEventKind_KeyUp,
    PrAppEventKind_Text,
    PrAppEventKind_Paste,
    PrAppEventKind_CompositionUpdate,
    PrAppEventKind_CompositionEnd,

    // --- Drags ---
    //
    // A drag over the window begins with DragEnter and ends with DragLeave or Drop. It may come from any application,
    // including this one. DragOver follows each move and repeats while the pointer rests. The window's latest output
    // gives the drop operation that answers each event. DragEnd ends a drag that the window started. DragPromise asks
    // the window for a file that its drag promised.
    PrAppEventKind_DragEnter,
    PrAppEventKind_DragOver,
    PrAppEventKind_DragLeave,
    PrAppEventKind_Drop,
    PrAppEventKind_DragEnd,
    PrAppEventKind_DragPromise,

    // --- Windows ---
    //
    // CloseRequested means that the user asked to close the window. The window stays open until the program closes it.
    PrAppEventKind_CloseRequested,

    // --- Tagged ---
    //
    // Each event carries the tag that the program gave to its timer, hotkey, notification, status item, menu or menu
    // item.
    PrAppEventKind_Timer,
    PrAppEventKind_Hotkey,
    PrAppEventKind_Notification,        // the user clicked the notification
    PrAppEventKind_StatusItem,          // the user clicked a status item that has no menu
    PrAppEventKind_MenuOpening,         // the menu is about to open, so the program can update its items
    PrAppEventKind_MenuItem,            // the user chose the menu item

    // --- Application ---
    PrAppEventKind_Wake,                // a thread called prAppWake
    PrAppEventKind_QuitRequested,       // the program answers with prAppQuitRespond
    PrAppEventKind_Open,                // the system asks the application to open files or URLs
    PrAppEventKind_LifecycleChanged,    // the value of prAppLifecycle changed
    PrAppEventKind_LowMemory,           // the system is low on memory, and the program should free what it can
    PrAppEventKind_ClipboardChanged,
    PrAppEventKind_DisplaysChanged,     // the list from prAppDisplays changed
    PrAppEventKind_LocaleChanged,       // the user changed their languages or regional formats
    PrAppEventKind_KeyboardLayoutChanged,

    // --- Device ---
    PrAppEventKind_PowerChanged,        // the value of prAppPower changed
    PrAppEventKind_Suspend,             // the system is about to sleep
    PrAppEventKind_Resume,              // the system woke from sleep
    PrAppEventKind_ScreenLocked,
    PrAppEventKind_ScreenUnlocked,
    PrAppEventKind_Sensor,              // a reading from a sensor that prAppSensorEnable turned on
};
typedef uint8_t PrAppEventKind;

// Returns the name of a kind of event, as in "PointerMove", or the null view if `kind` is not one.
PrStringView prAppEventKindName(PrAppEventKind kind);

// The details of a pointer event (pointerevents3/#pointerevent-interface). The position is in logical pixels from the
// window's top left. `id` is unique among the pointers that are down at the same time. `primary` is true for the
// primary pointer of its type.
//
// `pressure` is in [0, 1]. A device without pressure reports 0.5 while a button is down. `tilt` holds the angles from
// the surface's normal toward +x and toward +y, in degrees, each in [-90, 90]. `twist` is the clockwise rotation about
// the pointer's axis, in degrees, in [0, 360). `contactSize` is the width and height of the contact in logical pixels,
// or zero if the device does not report it.
//
// `movement` is the distance that the cursor moved since the window's previous pointer event, in logical pixels
// (pointerlock-2). It is zero for every kind but PointerMove. While the window locks the pointer, PrAppPointerMode says
// what it reports.
typedef struct PrAppPointerEvent {
    PrVec2 position;
    uint8_t button;             // the button pressed or released, 0 for the primary
    PrAppPointerType type;
    bool primary;
    uint32_t clicks;            // PointerDown: the number of presses in the current multi-click, including this one
    uint32_t id;
    float pressure;
    PrVec2 tilt;
    float twist;
    PrVec2 contactSize;
    PrVec2 movement;
} PrAppPointerEvent;

// The details of a Scroll event. The position is in logical pixels from the window's top left. The delta is positive to
// the right and down, in `unit`. prAppEventScrollPixels converts it to pixels.
typedef struct PrAppScrollEvent {
    PrVec2 position;
    PrVec2 delta;
    PrAppScrollUnit unit;
} PrAppScrollEvent;

// The details of a Gesture event. The position is in logical pixels from the window's top left. `magnification` is the
// change of scale since the gesture's previous event, as a fraction: 0.1 means 10% larger. `rotation` is the clockwise
// change of angle, in degrees. `swipe` is the direction in which the content moves. One of its axes is -1 or 1, and the
// other is 0.
typedef struct PrAppGestureEvent {
    PrAppGestureKind kind;
    PrAppGesturePhase phase;
    PrVec2 position;
    float magnification;
    float rotation;
    PrVec2 swipe;
} PrAppGestureEvent;

// The details of a TrackpadTouches event: every touch that is on the trackpad.
typedef struct PrAppTrackpadEvent {
    const PrAppTrackpadTouch* PR_NULLABLE touches;
    uint32_t count;
} PrAppTrackpadEvent;

// The details of a KeyDown or KeyUp event. `key` is what the key means, and `code` is where it is on the keyboard.
//
// `text` is the character that the key types with Shift and the layout's third level (AltGr or Option) applied, and
// with Control and Command ignored. For example, it is `a` for A and for Control+A, `A` for Shift+A, and `å` for
// Option+A on a Mac. It is empty if the key types nothing.
//
// A modifier key sends KeyDown when pressed and KeyUp when released. A lock key sends KeyDown when it turns on and
// KeyUp when it turns off. The Text event for a key comes after its KeyDown and before the next KeyDown, so that a
// program that handles the key can discard its text.
//
// `composing` is true if the input method uses the key in a composition. The program ignores such a key and acts on the
// composition events instead.
typedef struct PrAppKeyboardEvent {
    PrAppKey key;
    PrAppKeyCode code;
    bool repeat;                // KeyDown: true if the key is held and repeating
    bool composing;
    PrStringView text;
} PrAppKeyboardEvent;

// The details of a Text, Paste, CompositionUpdate or CompositionEnd event.
typedef struct PrAppInputEvent {
    PrStringView text;
    PrAppTextRange selection;   // CompositionUpdate: the range of the text that the input method selects
} PrAppInputEvent;

// The details of a drag event. The position is in logical pixels from the window's top left. DragLeave and DragEnd have
// no position.
//
// `forms` lists the forms of content that the drag carries. Until the drop, the system reveals only the forms. If
// `forms` includes Payload, `payload` holds the payload of a drag from this application. `operations` lists the
// operations that the source allows, narrowed by the modifier keys that the user holds.
//
// In Drop, `operation` is the operation of the drop, and `drop` holds the dropped content. `drop` is valid during the
// event. In DragEnd, `operation` is what the target did, or None if the drag was canceled or refused.
//
// The system takes over the pointer while a drag is in progress. The window that starts the drag receives
// PointerCancel. The window under the pointer receives drag events in place of pointer events.
typedef struct PrAppDragEvent {
    PrVec2 position;
    PrAppDataFormFlags forms;
    PrAppDragOperationFlags operations;
    PrAppDragOperation operation;
    bool fromThisWindow;        // true if the drag started in the window that receives the event
    PrAppDataPayload payload;
    PrAppDataSource drop;
} PrAppDragEvent;

// The details of a DragPromise event. The target of a drop asks for the file of the promise at `index`
// (PrAppDragDesc::promises). The program writes the file's content to the one file of `destination`
// (prAppDataWriteFile), and then ends the promise (prAppWindowDragPromiseEnd). `destination` is valid until the promise
// ends.
typedef struct PrAppDragPromiseEvent {
    uint32_t index;
    PrAppDataSource destination;
} PrAppDragPromiseEvent;

// The details of a QuitRequested event. `sessionEnding` is true if the user is logging out or shutting down. The system
// then waits only a limited time for the answer.
typedef struct PrAppQuitEvent {
    bool sessionEnding;
} PrAppQuitEvent;

// The details of an Open event. `source` holds the files or URLs to open, in its Files and Urls forms. `arguments`
// holds the command-line arguments of the launch that made the request, without the program's path. It is empty if the
// system made the request.
typedef struct PrAppOpenEvent {
    PrAppDataSource source;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;
} PrAppOpenEvent;

// The details of a Sensor event. The reading is in the first three or four values. The axes follow the natural
// orientation of the device's display: +x points right, +y up, and +z out of the display.
typedef struct PrAppSensorEvent {
    PrAppSensor sensor;
    float values[4];
} PrAppSensorEvent;

// An event. The trailing comments of the union say which kinds keep their details in each member. Other kinds have no
// details. The strings and arrays in an event are valid during its delivery.
struct PrAppEvent {
    PrAppEventKind kind;
    PrAppModifierFlags modifiers;
    double time;                // seconds, from a monotonic clock
    union {
        PrAppPointerEvent pointer;      // every pointer kind
        PrAppScrollEvent scroll;        // Scroll
        PrAppGestureEvent gesture;      // Gesture
        PrAppTrackpadEvent trackpad;    // TrackpadTouches
        PrAppKeyboardEvent keyboard;    // KeyDown, KeyUp
        PrAppInputEvent input;          // Text, Paste, CompositionUpdate, CompositionEnd
        PrAppDragEvent drag;            // every drag kind but DragPromise
        PrAppDragPromiseEvent promise;  // DragPromise
        uint64_t tag;                   // every tagged kind
        PrAppQuitEvent quit;            // QuitRequested
        PrAppOpenEvent open;            // Open
        PrAppSensorEvent sensor;        // Sensor
    };
};

// Returns a Scroll event's delta in logical pixels, given the height of a line.
PrVec2 prAppEventScrollPixels(const PrAppEvent* event, float lineHeight);

// Returns or replaces the text of a keyboard or input event. For any other event, the getter returns an empty view and
// the setter does nothing.
PrStringView prAppEventText(const PrAppEvent* event);
void prAppEventSetText(PrAppEvent* event, PrStringView text);

// Returns or replaces the position of a pointer, scroll or gesture event, or of a DragEnter, DragOver or Drop event.
// The getter writes the position to `out` and returns true. For any other event, the getter writes zero and returns
// false, and the setter does nothing.
bool prAppEventPosition(const PrAppEvent* event, PrVec2* out);
void prAppEventSetPosition(PrAppEvent* event, PrVec2 position);

// Returns a window's event in the logical pixels of a surface placed in the window, as prAppOutputPlaced places the
// surface's output: `origin` is the surface's origin in the window, and `scale` is the number of window logical pixels
// per surface logical pixel. The event's position, a pointer's movement and contact size, and a scroll's delta in
// pixels are the surface's. Its other members are the window's.
PrAppEvent prAppEventPlaced(const PrAppEvent* event, PrVec2 origin, float scale);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
