// The Window
//
// html - https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-window-object
// html - https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers

#pragma once

#include "pr/dom.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/string_view.h"

#include "css/src/css_media.h"
#include "dom/src/dom_node.h"
#include "html/src/html_history.h"
#include "html/src/html_storage.h"
#include "html/src/html_window_member.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomAgent;

// A timeout or an interval waiting to run (html/#timers).
struct HtmlTimer {
    PrHtmlTimerHandler handler;
    double deadline;        // the time on the window's clock at which the timer is due
    int32_t id;             // zero for a timer that no id names (htmlWindowRunAfterTimeout)
    int32_t interval;       // the milliseconds between the runs of an interval, or -1 for a timeout
    uint32_t nesting;       // html/#timer-nesting-level
    bool endless;           // set again each time it fires, for work that never ends (htmlWindowSetTimeout)
};

// A task queued to run after the current one (html/#queue-a-task).
struct HtmlTask {
    void (*run)(void* data);
    void* data;
    void (*PR_NULLABLE drop)(void* data);    // frees the data of a task that is cleared before it runs
    bool delaysLoad;                         // true if the load event waits for the task (html/#delay-the-load-event)
};

// An animation frame callback waiting for the next rendering opportunity (html/#list-of-animation-frame-callbacks).
struct HtmlFrameRequest {
    PrHtmlFrameRequestCallback callback;
    uint32_t handle;
};

// The rate of rendering opportunities, in frames per second, and the time between them in milliseconds.
constexpr double kHtmlFrameRate = 60;
constexpr double kHtmlFrameInterval = 1000.0 / kHtmlFrameRate;

// Declared below, and in html_media.h.
struct HtmlWindow;
struct HtmlMedia;

// An iframe element and the window of the browsing context that it contains (html/#content-navigable).
struct HtmlFrame {
    PrDomElement* container;
    PrHtmlWindow* window;
};

// The Window of a browsing context (html/#the-window-object).
struct HtmlWindow : PrDomEventTarget {
    PrDomDocument* document;
    PrDomAgent* agent;                        // the agent whose event loop runs the window (html/#relevant-agent)
    CssMediaEnvironment media;                // the environment that its document's media queries are evaluated against
    PrHtmlPreferenceObject preferences[PrHtmlPreference_Count];
    PrHtmlWindow* PR_NULLABLE parent;         // the window whose document holds the iframe of this window
    PrDomElement* PR_NULLABLE currentScript;  // the current script of its document (html/#dom-document-currentscript)
    Array<HtmlFrame> frames;                  // the iframes of its document, each with its current window
    Array<HtmlMedia*> mediaElements;          // the media elements of its document, which it runs

    // The services of the host that its document's canvases use: the device that the document is presented on, which
    // holds their bitmaps, or null for bitmaps in memory, and the codec that encodes them.
    PrGpuDevice* PR_NULLABLE device;
    PrImageCodec* PR_NULLABLE images;

    // The default User-Agent value of the host's fetcher (prFetchNetworkUserAgent), or empty without one, and the
    // navigator compatibility mode of the host.
    PrStringView userAgent;
    PrHtmlNavigatorCompatibilityMode navigatorCompatibilityMode;

    // The canvases whose bitmaps changed since they were last shown, which the next rendering update shows. Each holds
    // its element (domNodeHold).
    Array<PrDomElement*> canvases;
    Array<HtmlTimer> timers;                  // in the order in which they were set
    Array<HtmlTask> tasks;                    // in the order in which they were queued
    uint32_t taskHead;                        // the first task that has not run
    uint32_t delayingTasks;                   // the number of pending tasks that delay the load event
    Array<HtmlFrameRequest> frameRequests;    // in the order in which they were requested

    // The requests that the running frame took over. A canceled request has a null invoke.
    Array<HtmlFrameRequest> frameRunning;

    double (*PR_NULLABLE clock)(void* PR_NULLABLE context);   // the host's clock, or null for the system's
    void* PR_NULLABLE clockContext;
    PrHtmlAnimationFrames animationFrames;                   // which animation frames the host's event loop runs
    void (*PR_NULLABLE rendered)(void* PR_NULLABLE context); // called after each rendering update, or null
    void* PR_NULLABLE renderedContext;
    double lastFrame;          // the clock's time of the last rendering opportunity
    double timeOrigin;         // hr-time-3/#sec-time-origin, on the clock
    double timeOriginWall;     // the same moment on the wall clock, in timeWallMilliseconds
    PrHtmlPerformance performance;
    HtmlSessionHistory* history;                    // the session history that its documents take entries in
    uint64_t historyDocument;                       // its document's number in the history, or 0 before it has one
    uint32_t historyGeneration;                     // changed each time its history state is set (prHtmlHistoryState)
    PrHtmlStorage storage[PrHtmlStorageKind_Count]; // by kind, whose providers are its host's

    // The custom element registry of the window's scripts, which each of the window's documents uses.
    const PrDomCustomElementRegistry* PR_NULLABLE customElements;

    int32_t nextTimerId;       // the id that the last timer was given
    uint32_t nextFrameHandle;  // the handle that the last frame request was given
    bool runningTask;          // true while a task or a timer's handler is on the stack

    // The timer whose handler is running, or 0, its nesting level, and whether the handler cleared it, which ends an
    // interval.
    int32_t runningTimer;
    uint32_t runningNesting;
    bool runningTimerCleared;

    // True if the loaded document has not been revealed yet, and is to be revealed at the next rendering update
    // (html/#has-been-revealed).
    bool revealing;

    bool streaming;            // true while its document's input is still arriving and its parser is still running
};

// Sets a timer to run `handler` after `timeout` milliseconds, and every `timeout` milliseconds after that if it
// `repeats` (html/#timer-initialisation-steps), and returns its id. The id is positive and not in use by another timer.
// A timer nested more than five deep in the handlers of timers waits at least 4 milliseconds. An `endless` timer is one
// of a series that never ends, such as the time updates of media that plays in a loop. Like an animation that runs
// forever, it does not keep the window from being idle (htmlWindowIsIdle).
int32_t htmlWindowSetTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler,
                             int32_t timeout, bool repeats, bool endless);

// Cancels the timer `id` and releases its handler. Does nothing if no timer has that id.
void htmlWindowClearTimeout(PrHtmlWindow* window, int32_t id);

// Sets a timer that no id names to run `handler` after `milliseconds` (html/#run-steps-after-a-timeout).
void htmlWindowRunAfterTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, double milliseconds);

// Returns true if the window's document is render-blocked (html/#render-blocked). This is an HTML document that is
// still parsing its head, which may yet add a sheet that its first rendering needs, or that is waiting for a sheet that
// its parser added there. The rendering of a render-blocked document is not updated.
bool htmlWindowRenderBlocked(const PrHtmlWindow* window);

// Returns the time on the window's clock, which is its host's clock or the system's monotonic clock.
double htmlWindowClock(const PrHtmlWindow* window);

// Restarts the window's clock, as for the global object of a new document. The new time origin is the current time, and
// so is the time of the last rendering opportunity.
void htmlWindowStartClock(PrHtmlWindow* window);

// Returns the milliseconds since the window's time origin, coarsened (hr-time-3/#dfn-coarsen-time)
// (hr-time-3/#dom-performance-now).
double htmlWindowNow(const PrHtmlWindow* window);

// Returns the time of the window's last rendering opportunity, on the same clock as htmlWindowNow.
double htmlWindowFrameTime(const PrHtmlWindow* window);

// Queues `run` with `data` in a task. `drop` frees `data` if the task is cleared before it runs. If `delaysLoad` is
// set, the document's load event waits for the task.
void htmlWindowQueueTask(PrHtmlWindow* window, void (*run)(void* data), void* data,
                         void (*PR_NULLABLE drop)(void* data) = nullptr, bool delaysLoad = false);

// Returns the origin of the window's document (html/#concept-origin), serialized
// (html/#ascii-serialisation-of-an-origin). This is the origin of the document's URL, or the origin of the parent
// window for a document at about:blank or about:srcdoc.
PrString htmlWindowOrigin(const PrHtmlWindow* window);

// Requests that `callback` run at the next rendering opportunity, and returns the request's handle.
uint32_t htmlWindowRequestAnimationFrame(PrHtmlWindow* window,
                                         const PrHtmlFrameRequestCallback* callback);

// Cancels the request `handle` and releases its callback, even while its frame runs.
void htmlWindowCancelAnimationFrame(PrHtmlWindow* window, uint32_t handle);

// Drops every task, timer and frame request without running it.
void htmlWindowClearTasks(PrHtmlWindow* window);

// Returns true if nothing is pending except the rendering updates that animations want.
bool htmlWindowIsIdle(const PrHtmlWindow* window);

// Returns the number of milliseconds until the next task can run: zero if one can run already, and -1 if there is none.
// A queued task can run immediately, and a timer once it is due. A rendering update can run at the next rendering
// opportunity if there are frame requests or the document is to be revealed.
double htmlWindowNextTaskDelay(const PrHtmlWindow* window);

// Runs the first queued task. If no task is queued, runs whichever of the timers and the rendering opportunity is due
// first. The rendering opportunity reveals the document (html/#reveal) and runs the animation frame callbacks
// (html/#run-the-animation-frame-callbacks). Returns false if nothing ran, which includes a call from inside a task.
bool htmlWindowRunTask(PrHtmlWindow* window);

// Runs a task as htmlWindowRunTask does, from inside a task too, which waits for it as a task that spins the event loop
// waits (html/#spin-the-event-loop).
bool htmlWindowSpinTask(PrHtmlWindow* window);

// Runs the queued tasks in order until none of them delays the load event. This is the spin of the event loop that
// html/#the-end performs before the load event, except that it leaves the timers and rendering alone.
void htmlWindowRunLoadDelaying(PrHtmlWindow* window);

// Returns the window of the browsing context that `container` contains, or null for an element that contains none.
PrHtmlWindow* PR_NULLABLE htmlFrameWindow(const PrDomElement* container);

// Returns the element that contains the browsing context of `document`, such as its iframe, or null for a top-level
// document or a document that its browsing context no longer shows.
PrDomElement* PR_NULLABLE htmlFrameContainer(const PrDomDocument* document);

// Returns the first element in tree order that is a named object for `name` (html/#named-access-on-the-window-object).
// Such an element has the id `name`, or is an HTML embed, form, img or object element whose name attribute is `name`.
// Returns null for an empty `name`.
PrDomElement* PR_NULLABLE htmlWindowNamedObject(PrHtmlWindow* window, PrStringView name);

// Scrolls to the part of the window's document that its URL's fragment indicates, which becomes the document's target
// element (html/#scroll-to-the-fragment-identifier).
void htmlWindowScrollToFragment(PrHtmlWindow* window);

// Follows the hyperlink of the a or area element `element` of the window's document (html/#following-hyperlinks-2). A
// URL of the document with a fragment navigates to that fragment.
void htmlWindowFollowHyperlink(PrHtmlWindow* window, PrDomElement* element);

} // namespace pr_impl

// C opaque types
struct PrHtmlWindow : pr_impl::HtmlWindow {};

PR_ASSUME_NONNULL_END
