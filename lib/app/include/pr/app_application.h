// Applications: the process's event loop and its presence in the system.
//
// An application runs the event loop. It receives the events that concern the whole process, such as wakes, timers,
// quit requests, requests to open files and URLs, and lifecycle changes. It owns its windows (pr/app_window.h). The
// other headers of lib/app cover displays (pr/app_display.h), input (pr/app_input.h), data transfer (pr/app_data.h),
// menus (pr/app_menu.h), the shell (pr/app_shell.h) and the device (pr/app_device.h).
//
// An application and its windows are used on the thread that runs the application, unless a function says that any
// thread may call it. A process runs one application at a time.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An application, created by prAppCreate.
typedef struct PrApp PrApp;

// A window of an application (pr/app_window.h).
typedef struct PrAppWindow PrAppWindow;

// An event (pr/app_event.h).
typedef struct PrAppEvent PrAppEvent;

// The backends that lib/app provides. Default selects the platform's backend. A program can also supply a backend of
// its own (pr/app_backend.h).
enum {
    PrAppBackend_Default,
    PrAppBackend_AppKit,
    PrAppBackend_Win32,
    PrAppBackend_Web,
    PrAppBackend_Count,
};
typedef uint8_t PrAppBackend;

// Returns the backend named `name`, ignoring case, or Count if no backend has that name.
PrAppBackend prAppBackendFromName(PrStringView name);

// Returns the lowercase name of a backend.
PrStringView prAppBackendName(PrAppBackend backend);

// Returns true if the build includes the backend. The build includes Default if it includes any backend.
bool prAppBackendIsBuilt(PrAppBackend backend);

// Whether the application appears in the system's lists of running applications: the Dock, the taskbar and the
// application switcher. An Accessory application appears in none of them. It has only its windows and status items,
// like a menu bar utility or a launcher.
enum {
    PrAppPresence_Regular,
    PrAppPresence_Accessory,
};
typedef uint8_t PrAppPresence;

// How many windows an application has. A Single application has one window, which the program draws its popups and
// dialogs inside, and a second window answers NotSupported. The desktop sizes and places that window as any other, and
// a phone, a console or a web page shows it filling the display. A Multiple application has as many windows as it
// creates, where the platform has them.
enum {
    PrAppWindowModel_Single,
    PrAppWindowModel_Multiple,
};
typedef uint8_t PrAppWindowModel;

// Options for creating an application.
//
// A headless application never shows its windows, and the system never activates it. If `singleInstance` is not empty,
// it names the application within the user's session. When a second process with the same name launches, it passes its
// request to the running process as an Open event. Its own prAppRun then returns without calling the handler.
typedef struct PrAppOptions {
    bool headless;
    PrAppPresence presence;
    PrAppWindowModel windowModel;   // the model that the program asks for (prAppWindowModel)
    PrStringView singleInstance;
} PrAppOptions;

// Returns the default options: an application that shows on the screen, with Regular presence, a Single window and no
// single instance.
PrAppOptions prAppOptionsDefault(void);

// The callbacks of an application. Each one receives `context`, and every one must be set.
//
// `start` is called when the application begins running, and creates the first windows. If it returns false, the run
// stops. `event` receives the application's events. `shutdown` is called last when the run stops, whether or not
// `start` succeeded. After it returns, the application destroys the remaining windows and makes no further calls.
//
// The callbacks of an application and its windows run on its thread, and never during one another. An event that
// arrives during a callback is delivered after the callback returns.
typedef struct PrAppHandler {
    void* PR_NULLABLE context;
    bool (*start)(void* PR_NULLABLE context, PrApp* app);
    void (*event)(void* PR_NULLABLE context, PrApp* app, const PrAppEvent* event);
    void (*shutdown)(void* PR_NULLABLE context, PrApp* app);
} PrAppHandler;

// ---------------------------------------------------------------------------------------------------------------------
// Running

// Creates an application with `backend`. A null `options` means the defaults. Returns null if the build does not
// include the backend.
PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppOptions* PR_NULLABLE options);

// Frees the application. It must not be running.
void prAppDestroy(PrApp* PR_NULLABLE app);

// Runs the application with `handler` until it stops. On some backends, such as the web, this call never returns.
void prAppRun(PrApp* app, PrAppHandler handler);

// Stops the run. The application then calls the handler's `shutdown`.
void prAppStop(PrApp* app);

// Changes whether the application appears in the system's lists (PrAppOptions::presence).
void prAppSetPresence(PrApp* app, PrAppPresence presence);

// Returns the window model that the application runs with: the one that its options asked for, or Single where the
// platform has only one window.
PrAppWindowModel prAppWindowModel(const PrApp* app);

// Hides all of the application's windows, or shows the ones that hiding concealed, like the Hide command on macOS. The
// getter returns whether the application is hidden. Hiding activates the next application. Focusing a window shows the
// application again. A window that the program hid itself stays hidden. A change requests a frame of each window.
void prAppSetHidden(PrApp* app, bool hidden);
bool prAppIsHidden(const PrApp* app);

// Delivers a Wake event soon. Several calls before the next delivery produce one event. Any thread may call it.
void prAppWake(PrApp* app);

// Cancels the pending requests that were started with `context`: reads (pr/app_data.h), file dialogs and shares. Their
// callbacks are never called, and an open dialog closes. If a callback with `context` is running on another thread,
// this call waits for it to return, so the caller may free the context afterward. Any thread may call it.
void prAppCancel(PrApp* app, void* PR_NULLABLE context);

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle

// The application's state with the user. An Active application has the user's input. An Inactive application is visible
// but does not have the input, for example behind another application or under the system's interface. A Background
// application is not visible. A mobile system may suspend a Background application, or end it without warning. A
// LifecycleChanged event follows each change.
enum {
    PrAppLifecycle_Active,
    PrAppLifecycle_Inactive,
    PrAppLifecycle_Background,
};
typedef uint8_t PrAppLifecycle;

// Returns the name of a lifecycle state, as in "Background", or the null view if `lifecycle` is not one.
PrStringView prAppLifecycleName(PrAppLifecycle lifecycle);

// Returns the application's lifecycle state.
PrAppLifecycle prAppLifecycle(const PrApp* app);

// ---------------------------------------------------------------------------------------------------------------------
// Quitting
//
// The user or the system may ask the application to quit, through the Quit command, the Dock or the taskbar, or because
// the session is ending. The application receives a QuitRequested event. The program answers with prAppQuitRespond,
// either immediately or after asking the user, for example about unsaved changes. If the program accepts, the run
// stops.

// Answers the pending quit request. If `quit` is false, the application keeps running. The system may still end the
// session after its time limit.
void prAppQuitRespond(PrApp* app, bool quit);

// ---------------------------------------------------------------------------------------------------------------------
// Timers

// Delivers a Timer event with `tag` after `delay` seconds, and then every `interval` seconds. If `interval` is zero,
// the timer fires once. Setting a timer with an existing tag replaces that timer.
void prAppTimerSet(PrApp* app, uint64_t tag, double delay, double interval);

// Stops the timer with `tag`, if one is set.
void prAppTimerCancel(PrApp* app, uint64_t tag);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
