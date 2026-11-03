// The Window.
//
// html - https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers

#include "html/src/html_window.h"

#include "pr/ascii.h"
#include "pr/css_intersection_observer.h"
#include "pr/css_presentation.h"
#include "pr/css_resize_observer.h"
#include "pr/css_view.h"
#include "pr/float.h"
#include "pr/html.h"
#include "pr/string_ops.h"
#include "pr/time.h"
#include "pr/url.h"
#include "pr/url_percent.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_view.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event.h"
#include "html/src/html_canvas.h"
#include "html/src/html_focus.h"
#include "html/src/html_interface.h"
#include "html/src/html_media.h"
#include "html/src/html_media_source.h"
#include "html/src/html_page.h"

namespace pr_impl {
namespace {

uint32_t timerIndex(const PrHtmlWindow* window, int32_t id) {
    for (uint32_t i = 0; i < window->timers.count; ++i) {
        if (window->timers.data[i].id == id)
            return i;
    }
    return window->timers.count;
}

uint32_t nextTimerIndex(const PrHtmlWindow* window) {
    uint32_t index = window->timers.count;
    for (uint32_t i = 0; i < window->timers.count; ++i) {
        if (index == window->timers.count || window->timers.data[i].deadline < window->timers.data[index].deadline)
            index = i;
    }
    return index;
}

void release(const PrHtmlTimerHandler& handler) {
    if (handler.release != nullptr)
        handler.release(handler.context);
}

void release(const PrHtmlFrameRequestCallback& callback) {
    if (callback.release != nullptr)
        callback.release(callback.context);
}

void release(const PrHtmlIdleRequestCallback& callback) {
    if (callback.release != nullptr)
        callback.release(callback.context);
}

// Coarsens a time to 100 microseconds, as hr-time-3/#dfn-coarsen-time specifies for a context that is not cross-origin
// isolated.
double coarsen(double milliseconds) {
    return prF64Floor(milliseconds * 10) / 10;
}

// Returns the time of the next rendering opportunity. Any frame requests run at that time.
double nextFrame(const PrHtmlWindow* window) {
    return window->lastFrame + kHtmlFrameInterval;
}

// Returns the presentation of the window's document, or null if there is none.
PrCssPresentation* PR_NULLABLE presentationOf(const PrHtmlWindow* window) {
    const PrDomDocument* document = window->document;
    PrCssPresentation* presentation = document != nullptr ? cssPresentationOf(document) : nullptr;
    return presentation != nullptr ? presentation : nullptr;
}

// Returns the time of the next rendering opportunity that runs a frame. This is normally the next opportunity. Suppose
// that the window runs only the frames that are observed (PrHtmlAnimationFrames_Observed), and a frame would change
// nothing but the styles that animations animate. Then it is the last opportunity at or before the next animation event
// or timer, on the same grid, so that the frame that runs then sees the same state that it would have seen otherwise.
double frameDue(const PrHtmlWindow* window) {
    double due = nextFrame(window);
    PrCssPresentation* presentation = presentationOf(window);
    if (window->animationFrames != PrHtmlAnimationFrames_Observed || window->frameRequests.count != 0 ||
            window->revealing || window->taskHead != window->tasks.count || presentation == nullptr ||
            cssPresentationHasWork(presentation))
        return due;
    double event = cssAnimateNextEventTime(&presentation->animations);
    double target = event >= 0 ? window->timeOrigin + event : -1;
    uint32_t timer = nextTimerIndex(window);
    if (timer != window->timers.count && (target < 0 || window->timers.data[timer].deadline < target))
        target = window->timers.data[timer].deadline;
    if (target < 0)
        return due;
    double frames = prF64Floor((target - window->lastFrame) / kHtmlFrameInterval);
    return frames > 1 ? window->lastFrame + frames * kHtmlFrameInterval : due;
}

// Returns true if the next rendering opportunity updates the rendering. It does if there are frame requests, if the
// document is to be revealed, if a top-level document has autofocus candidates, if its animations are running, if
// scroll or animation events are due, if a canvas or a video shows a picture that is out of date, or if its styles
// changed.
bool wantsFrame(const PrHtmlWindow* window) {
    if (htmlWindowRenderBlocked(window))
        return false;
    PrCssPresentation* presentation = presentationOf(window);
    return window->frameRequests.count != 0 || window->revealing || window->canvases.count != 0 ||
        htmlMediaWantsFrame(window) ||
        (window->parent == nullptr && window->document != nullptr && htmlFocusHasAutofocus(window->document)) ||
        (window->document != nullptr && prCssDocumentResizeObservationsPending(window->document)) ||
        (window->document != nullptr && prCssDocumentIntersectionObservationsPending(window->document)) ||
        (presentation != nullptr &&
            (prCssPresentationIsAnimating(presentation) || cssPresentationHasWork(presentation)));
}

// Implements html/#reveal: the first rendering update of a document notifies its window that the document is revealed.
void reveal(PrHtmlWindow* window) {
    if (!window->revealing)
        return;
    window->revealing = false;
    PrHtmlPageRevealEvent* event =
        domEventCreate<PrHtmlPageRevealEvent>("pagereveal", prDomEventInitDefault(), PrDomEventKind_PageReveal);
    event->trusted = true;
    domEventDispatch(window, event);
    domEventRelease(event);
}

// Reports that resize observations went undelivered (resize-observer-1/#deliver-resize-loop-error-notification), in the
// same way that html/#report-the-exception reports an exception: with an ErrorEvent at the window.
void reportResizeLoopError(PrHtmlWindow* window) {
    PrHtmlErrorEventInit init = prHtmlErrorEventInitDefault();
    init.event.cancelable = true;
    init.message = "ResizeObserver loop completed with undelivered notifications.";
    PrHtmlErrorEvent* event = prHtmlErrorEventCreate("error", &init);
    event->trusted = true;
    domEventDispatch(window, event);
    domEventRelease(event);
}

// Delivers the entries of the document's intersection observers
// (intersection-observer/#queue-intersection-observer-task).
void notifyIntersectionObservers(void* data) {
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(data);
    if (window->document != nullptr)
        prCssDocumentNotifyIntersectionObservers(window->document);
}

// Fires change at the window's screen orientation if its type changed since the last rendering update
// (screen-orientation/#dfn-screen-orientation-change-steps).
void updateScreenOrientation(PrHtmlWindow* window) {
    PrHtmlScreenOrientation* orientation = prHtmlWindowScreenOrientation(window);
    PrHtmlOrientationType type = prHtmlScreenOrientationType(orientation);
    if (type == orientation->reported)
        return;
    orientation->reported = type;
    domEventFire(orientation, "change");
}

// Runs a rendering update (html/#update-the-rendering). The document is revealed, a top-level document flushes its
// autofocus candidates, its screen orientation fires change if it changed, its scroll events are fired, and its
// animations are updated and fire their events. Then html/#run-the-animation-frame-callbacks runs the callbacks that
// were requested before the update started, each with the frame's time. A callback canceled before its turn does not
// run. Last, the document's styles are computed, its resize observers are called with the boxes whose sizes changed, a
// focused element that is no longer focusable loses the focus, its intersection observations are computed, and its host
// lays it out afterward. The entries of the intersection observations wait for a task. A document with observations is
// laid out to compute them, and a document whose text may request fonts is laid out so that the fonts start loading.
void runFrame(PrHtmlWindow* window, double now) {
    window->lastFrame = now;
    reveal(window);
    if (window->parent == nullptr && window->document != nullptr)
        htmlFocusFlushAutofocus(window->document);
    updateScreenOrientation(window);
    if (PrCssPresentation* presentation = presentationOf(window))
        cssPresentationRunScrollSteps(presentation);
    double time = htmlWindowFrameTime(window);
    // web-animations-1/#update-animations-and-send-events: the document's animations move to the frame's time, and fire
    // the events for what that changed.
    if (PrCssPresentation* presentation = presentationOf(window)) {
        prCssPresentationSetTime(presentation, time);
        cssPresentationSendAnimationEvents(presentation);
    }
    window->frameRunning = window->frameRequests;
    window->frameRequests = {};
    for (uint32_t i = 0; i < window->frameRunning.count; ++i) {
        PrHtmlFrameRequestCallback callback = window->frameRunning.data[i].callback;
        if (callback.invoke == nullptr)
            continue;
        window->frameRunning.data[i].callback.invoke = nullptr;
        callback.invoke(callback.context, time);
        release(callback);
    }
    window->frameRunning.reset();
    // The canvases that the callbacks and the tasks before them drew show what they drew.
    htmlCanvasShow(window);
    htmlMediaShow(window);
    // Styles are computed for the frame, in the same way as when the host lays out, which starts the transitions that
    // style changes make. Computing styles does not wait for a presentation thread that is still laying out the last
    // frame.
    if (PrCssPresentation* presentation = presentationOf(window)) {
        cssPresentationUpdateStyle(presentation);
        if (prCssDocumentBroadcastResizeObservations(window->document, prHtmlWindowDevicePixelRatio(window)))
            reportResizeLoopError(window);
        htmlFocusFixup(window->document);
        if (prCssDocumentUpdateIntersectionObservations(window->document, time))
            htmlWindowQueueTask(window, notifyIntersectionObservers, window);
        // The text requests the fonts that it uses as it is laid out (css-fonts-4/#font-matching-algorithm).
        if (cssPresentationAwaitsFontRequests(presentation))
            cssPresentationFlush(presentation);
    }
    if (window->rendered != nullptr) {
        if (PrCssPresentation* presentation = presentationOf(window))
            cssPresentationFlush(presentation);
        window->rendered(window->renderedContext);
    }
}

// Runs the first queued task, if there is one.
bool runQueued(PrHtmlWindow* window) {
    if (window->taskHead == window->tasks.count)
        return false;
    HtmlTask task = window->tasks.data[window->taskHead++];
    if (window->taskHead == window->tasks.count) {
        window->tasks.count = 0;
        window->taskHead = 0;
    }
    window->delayingTasks -= task.delaysLoad ? 1 : 0;
    bool outer = window->runningTask;
    window->runningTask = true;
    task.run(task.data);
    window->runningTask = outer;
    return true;
}

// The queued tasks that run in a row while a timer or an animation frame is due.
constexpr uint32_t kMaxQueuedRun = 16;

// The longest idle period (requestidlecallback/#start-an-idle-period-algorithm), in milliseconds.
constexpr double kIdleMaxPeriod = 50;

// A request's timeout, which a timer of the window runs.
struct IdleTimeout {
    PrHtmlWindow* window;
    uint32_t handle;
};

// Returns the first time after which the idle period must yield: `deadline`, or a timer of the page's own, or an
// animation frame that the window wants. A request's own timeout does not end an idle period.
double idleCap(const PrHtmlWindow* window, double deadline);

// Removes the request `handle` from `list` and writes it to `out`. Returns false if the list does not hold it.
bool takeIdleRequest(Array<HtmlIdleRequest>* list, uint32_t handle, HtmlIdleRequest* out) {
    for (uint32_t i = 0; i < list->count; ++i) {
        if (list->data[i].handle == handle) {
            *out = list->data[i];
            list->erase(i);
            return true;
        }
    }
    return false;
}

void invokeIdle(PrHtmlWindow* window, const HtmlIdleRequest& request, double deadline, bool didTimeout) {
    bool running = window->runningIdle;
    window->runningIdle = true;
    request.callback.invoke(request.callback.context, deadline, didTimeout);
    window->runningIdle = running;
    release(request.callback);
}

// Runs a request whose timeout passed (requestidlecallback/#invoke-idle-callback-timeout-algorithm).
void idleTimedOut(void* PR_NULLABLE context) {
    IdleTimeout* timeout = static_cast<IdleTimeout*>(context);
    PrHtmlWindow* window = timeout->window;
    HtmlIdleRequest request{};
    if (takeIdleRequest(&window->idleRequests, timeout->handle, &request) ||
            takeIdleRequest(&window->idleRunnable, timeout->handle, &request)) {
        request.timer = 0;
        invokeIdle(window, request, htmlWindowClock(window), true);
    }
}

// Runs an idle callback, if the event loop has nothing else to do. An idle period starts if none is running or the
// current one has run out of requests, and takes the requests made so far. Returns false if no callback ran.
bool runIdle(PrHtmlWindow* window, double now) {
    if (window->idleRunnable.count == 0 && window->idleRequests.count == 0)
        return false;
    if (window->idleRunnable.count == 0 || now >= window->idleDeadline) {
        window->idleDeadline = idleCap(window, now + kIdleMaxPeriod);
        for (uint32_t i = 0; i < window->idleRequests.count; ++i)
            window->idleRunnable.push(window->idleRequests.data[i]);
        window->idleRequests.clear();
    }
    if (window->idleRunnable.count == 0 || now >= window->idleDeadline)
        return false;
    HtmlIdleRequest request = window->idleRunnable.data[0];
    window->idleRunnable.erase(0);
    if (request.timer != 0)
        htmlWindowClearTimeout(window, request.timer);
    bool outer = window->runningTask;
    window->runningTask = true;
    invokeIdle(window, request, window->idleDeadline, false);
    window->runningTask = outer;
    return true;
}

double idleCap(const PrHtmlWindow* window, double deadline) {
    for (uint32_t i = 0; i < window->timers.count; ++i) {
        const HtmlTimer& timer = window->timers.data[i];
        if (timer.handler.invoke != idleTimedOut && timer.deadline < deadline)
            deadline = timer.deadline;
    }
    if (wantsFrame(window) && frameDue(window) < deadline)
        deadline = frameDue(window);
    return deadline;
}

// Runs at most one task of the window: a queued task, or else the earliest due timer or animation frame, or else an
// idle callback. A task that spins the event loop runs one during its own, and is still running afterward.
bool runNextTask(PrHtmlWindow* window) {
    // A queued task runs first, unless a long run of them has gone by, so that tasks that queue each other do not
    // starve what is due.
    if (window->queuedRun < kMaxQueuedRun && runQueued(window)) {
        ++window->queuedRun;
        return true;
    }
    uint32_t index = nextTimerIndex(window);
    double now = htmlWindowClock(window);
    bool timer = index != window->timers.count && window->timers.data[index].deadline <= now;
    double frameAt = frameDue(window);
    bool frame = wantsFrame(window) && frameAt <= now;
    bool outer = window->runningTask;
    window->queuedRun = 0;
    if (frame && (!timer || frameAt <= window->timers.data[index].deadline)) {
        window->runningTask = true;
        runFrame(window, now);
        window->runningTask = outer;
        return true;
    }
    if (!timer) {
        if (runQueued(window)) {
            window->queuedRun = 1;
            return true;
        }
        return runIdle(window, now);
    }

    // Take ownership before invoking, because the callback may cancel or add other timers. An interval that its handler
    // did not clear is set again with the same id (html/#timer-initialisation-steps).
    HtmlTimer fired = window->timers.data[index];
    window->timers.erase(index);
    int32_t runningTimer = window->runningTimer;
    uint32_t runningNesting = window->runningNesting;
    bool runningCleared = window->runningTimerCleared;
    window->runningTimer = fired.id;
    window->runningNesting = fired.nesting;
    window->runningTimerCleared = false;
    window->runningTask = true;
    fired.handler.invoke(fired.handler.context);
    if (fired.interval >= 0 && !window->runningTimerCleared) {
        fired.nesting = fired.nesting + 1;
        int32_t timeout = fired.nesting > 5 && fired.interval < 4 ? 4 : fired.interval;
        fired.deadline = htmlWindowClock(window) + double(timeout);
        window->timers.push(fired);
    } else {
        release(fired.handler);
    }
    window->runningTimer = runningTimer;
    window->runningNesting = runningNesting;
    window->runningTimerCleared = runningCleared;
    window->runningTask = outer;
    return true;
}

// Returns the element that a fragment names (html/#find-a-potential-indicated-element): the first element in tree order
// whose ID is the fragment, or else the first a element whose name is the fragment.
PrDomElement* PR_NULLABLE potentialIndicated(PrDomDocument* document, PrStringView fragment) {
    if (PrDomElement* element = domElementById(document, fragment))
        return element;
    for (PrDomNode* node = domNodeNext(document, document); node != nullptr; node = domNodeNext(document, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domIsHtml(element, PrDomTag_A) && domAttributeValue(element, "name") == fragment)
            return element;
    }
    return nullptr;
}

// Makes `element` the document's target element, or clears the target if `element` is null, and restyles the elements
// that depend on :target.
void setTarget(PrDomDocument* document, PrDomElement* PR_NULLABLE element) {
    PrDomElement* from = document->target;
    document->target = element;
    CssPseudoClass state = CssPseudoClass_Target;
    if (from != element)
        cssPresentationStateMoved(from, element, &state, 1);
}

// Frees a window once its last reference is released (DomCountedTarget::destroy).
void freeWindow(DomCountedTarget* target) {
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(target);
    domNodeRelease(window->document);
    window->timers.reset();
    window->tasks.reset();
    window->frameRequests.reset();
    window->frameRunning.reset();
    window->canvases.reset();
    window->mediaElements.reset();
    window->frames.reset();
    prMemoryFree(window);
}

// Returns the window whose document's URL gives the window's document its origin. Two windows with the same source
// share an origin even if it is opaque, whose serialization does not distinguish one from another.
const PrHtmlWindow* originSource(const PrHtmlWindow* window) {
    while (window->parent != nullptr) {
        PrString href = prUrlSerialize(window->document->url, PrUrlSerialization_ExcludeFragment);
        bool inherits = prStringView(&href) == "about:blank" || prStringView(&href) == "about:srcdoc";
        prStringReset(&href);
        if (!inherits)
            break;
        window = window->parent;
    }
    return window;
}

// Sets the last activation timestamp of the descendants of `window` that have the origin of `origin` to `now`.
void notifyDescendants(PrHtmlWindow* window, const PrHtmlWindow* origin, double now) {
    for (uint32_t i = 0; i < window->frames.count; ++i) {
        PrHtmlWindow* frame = window->frames.data[i].window;
        if (htmlWindowIsSameOrigin(frame, origin))
            frame->lastActivation = now;
        notifyDescendants(frame, origin, now);
    }
}

void consumeActivation(PrHtmlWindow* window) {
    if (window->lastActivation != kPrF64Infinity)
        window->lastActivation = -kPrF64Infinity;
    for (uint32_t i = 0; i < window->frames.count; ++i)
        consumeActivation(window->frames.data[i].window);
}

// Returns the target of a hyperlink (html/#get-an-element's-target): its target attribute, or else that of the first
// base element that has one. A value that looks like dangling markup means a new browsing context.
PrStringView hyperlinkTarget(const PrDomDocument* document, const PrDomElement* element) {
    PrStringView target = domAttributeValue(element, "target");
    if (target.data == nullptr && document->baseElements != 0) {
        const PrDomNode* node = domNodeNext(document, document);
        while (target.data == nullptr && node != nullptr) {
            const PrDomElement* base = domElementFromNode(node);
            if (base != nullptr && domIsHtml(base, PrDomTag_Base))
                target = domAttributeValue(base, "target");
            node = domNodeNext(document, node);
        }
    }
    bool hasBreak = false;
    bool hasLess = false;
    for (size_t i = 0; i < target.size; ++i) {
        hasBreak = hasBreak || target.data[i] == '\t' || target.data[i] == '\n' || target.data[i] == '\r';
        hasLess = hasLess || target.data[i] == '<';
    }
    return hasBreak && hasLess ? PrStringView{"_blank"} : target;
}

} // namespace

int32_t htmlWindowSetTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, int32_t timeout, bool repeats,
        bool endless) {
    int32_t id;
    do {
        id = window->nextTimerId == INT32_MAX ? 1 : window->nextTimerId + 1;
        window->nextTimerId = id;
    } while (timerIndex(window, id) != window->timers.count || id == window->runningTimer);

    // html/#timer-initialisation-steps: a timer set by a timer's handler is nested one deeper.
    uint32_t nesting = window->runningTimer != 0 ? window->runningNesting + 1 : 0;
    timeout = timeout > 0 ? timeout : 0;
    if (nesting > 5 && timeout < 4)
        timeout = 4;
    HtmlTimer timer{*handler, htmlWindowClock(window) + double(timeout), id, repeats ? timeout : -1, nesting, endless};
    if (timer.handler.retain != nullptr)
        timer.handler.retain(timer.handler.context);
    window->timers.push(timer);
    return id;
}

void htmlWindowClearTimeout(PrHtmlWindow* window, int32_t id) {
    // An interval that clears itself while its handler runs is not set again. Zero names no timer.
    if (id != 0 && id == window->runningTimer)
        window->runningTimerCleared = true;
    uint32_t index = id > 0 ? timerIndex(window, id) : window->timers.count;
    if (index == window->timers.count)
        return;
    PrHtmlTimerHandler handler = window->timers.data[index].handler;
    window->timers.erase(index);
    release(handler);
}

void htmlWindowRunAfterTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, double milliseconds) {
    HtmlTimer timer{
        .handler = *handler,
        .deadline = htmlWindowClock(window) + (milliseconds > 0 ? milliseconds : 0),
        .id = 0,
        .interval = -1,
    };
    if (timer.handler.retain != nullptr)
        timer.handler.retain(timer.handler.context);
    window->timers.push(timer);
}

bool htmlWindowRenderBlocked(const PrHtmlWindow* window) {
    // html/#allows-adding-render-blocking-elements holds for an HTML document whose body element is null, and
    // html/#render-blocking-element-set holds the sheets that its parser added there.
    PrDomDocument* document = window->document;
    if (document == nullptr || document->kind != DomDocumentKind_Html)
        return false;
    return (window->streaming && domDocumentBody(document) == nullptr) || cssDocumentBlocksRendering(document);
}

double htmlWindowClock(const PrHtmlWindow* window) {
    return window->clock != nullptr ? window->clock(window->clockContext) : prTimeMonotonicMilliseconds();
}

PrHtmlWindow* htmlWindowCreate(void) {
    PrHtmlWindow* window = allocZeroed<PrHtmlWindow>();
    window->type = DomTargetType_Window;
    window->references = 1;
    window->destroy = freeWindow;
    window->lastActivation = kPrF64Infinity;
    for (PrHtmlPreference preference = 0; preference < PrHtmlPreference_Count; ++preference) {
        PrHtmlPreferenceObject& object = window->preferences[preference];
        object.type = DomTargetType_Preference;
        object.owner = window;
        object.window = window;
        object.preference = preference;
    }
    window->performance.type = DomTargetType_Performance;
    window->performance.owner = window;
    window->performance.window = window;
    window->orientation.type = DomTargetType_ScreenOrientation;
    window->orientation.owner = window;
    window->orientation.window = window;
    window->orientation.reported = PrHtmlOrientationType_LandscapePrimary;
    return window;
}

void htmlWindowStartClock(PrHtmlWindow* window) {
    window->timeOrigin = htmlWindowClock(window);
    window->timeOriginWall = prTimeWallMilliseconds();
    window->lastFrame = window->timeOrigin;
}

void htmlWindowBackdateClock(PrHtmlWindow* window, double elapsed) {
    window->timeOrigin -= elapsed;
    window->timeOriginWall -= elapsed;
}

void htmlWindowRevokeObjectUrls(PrHtmlWindow* window) {
    htmlMediaSourceUrlsReset(window);
    PrHtmlPage* page = htmlDocumentPage(window->document);
    PrString origin = htmlWindowOrigin(window);
    for (uint32_t i = 0; i < window->objectUrls.count; ++i) {
        if (page != nullptr && page->host.fetcher != nullptr)
            prFetchBlobUrlRevoke(page->host.fetcher, prStringView(&window->objectUrls.data[i]), prStringView(&origin));
        prStringReset(&window->objectUrls.data[i]);
    }
    prStringReset(&origin);
    window->objectUrls.reset();
}

double htmlWindowNow(const PrHtmlWindow* window) {
    return coarsen(htmlWindowClock(window) - window->timeOrigin);
}

double htmlWindowFrameTime(const PrHtmlWindow* window) {
    return coarsen(window->lastFrame - window->timeOrigin);
}

void htmlWindowQueueTask(PrHtmlWindow* window, void (*run)(void* data), void* data,
        void (*PR_NULLABLE drop)(void* data), bool delaysLoad) {
    // Tasks that have run are released once they make up at least half of the queue, so that a pop has constant cost.
    if (window->taskHead != 0 && window->taskHead * 2 >= window->tasks.count) {
        window->tasks.erase(0, window->taskHead);
        window->taskHead = 0;
    }
    window->tasks.push(HtmlTask{run, data, drop, delaysLoad});
    window->delayingTasks += delaysLoad ? 1 : 0;
}

PrString htmlWindowOrigin(const PrHtmlWindow* window) {
    return prUrlOrigin(originSource(window)->document->url);
}

bool htmlWindowIsSameOrigin(const PrHtmlWindow* a, const PrHtmlWindow* b) {
    if (originSource(a) == originSource(b))
        return true;
    PrString originA = htmlWindowOrigin(a);
    PrString originB = htmlWindowOrigin(b);
    bool same = prStringView(&originA) == prStringView(&originB) && prStringView(&originA) != "null";
    prStringReset(&originA);
    prStringReset(&originB);
    return same;
}

void htmlWindowNotifyDescendants(PrHtmlWindow* window, double now) {
    notifyDescendants(window, window, now);
}

void htmlWindowConsumeActivation(PrHtmlWindow* window) {
    PrHtmlWindow* top = window;
    while (top->parent != nullptr)
        top = top->parent;
    consumeActivation(top);
}

uint32_t htmlWindowRequestIdleCallback(PrHtmlWindow* window, const PrHtmlIdleRequestCallback* callback, uint32_t timeout) {
    uint32_t handle = ++window->nextIdleHandle;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    HtmlIdleRequest request{.callback = *callback, .handle = handle, .timer = 0, .endless = window->runningIdle};
    if (timeout > 0) {
        IdleTimeout* context = allocZeroed<IdleTimeout>();
        *context = IdleTimeout{window, handle};
        PrHtmlTimerHandler handler{
            .invoke = idleTimedOut,
            .context = context,
            .retain = nullptr,
            .release = [](void* PR_NULLABLE data) { prMemoryFree(data); },
        };
        int32_t clamped = timeout > 0x7fffffffu ? 0x7fffffff : int32_t(timeout);
        request.timer = htmlWindowSetTimeout(window, &handler, clamped, false, true);
    }
    window->idleRequests.push(request);
    return handle;
}

void htmlWindowCancelIdleCallback(PrHtmlWindow* window, uint32_t handle) {
    HtmlIdleRequest request{};
    if (!takeIdleRequest(&window->idleRequests, handle, &request) && !takeIdleRequest(&window->idleRunnable, handle, &request))
        return;
    if (request.timer != 0)
        htmlWindowClearTimeout(window, request.timer);
    release(request.callback);
}

double htmlWindowIdleTimeRemaining(const PrHtmlWindow* window, double deadline) {
    double remaining = idleCap(window, deadline) - htmlWindowClock(window);
    return remaining > 0 ? remaining : 0;
}

uint32_t htmlWindowRequestAnimationFrame(PrHtmlWindow* window, const PrHtmlFrameRequestCallback* callback) {
    uint32_t handle = ++window->nextFrameHandle;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    window->frameRequests.push(HtmlFrameRequest{*callback, handle});
    return handle;
}

void htmlWindowCancelAnimationFrame(PrHtmlWindow* window, uint32_t handle) {
    for (uint32_t i = 0; i < window->frameRunning.count; ++i) {
        HtmlFrameRequest* request = &window->frameRunning.data[i];
        if (request->handle == handle && request->callback.invoke != nullptr) {
            release(request->callback);
            request->callback.invoke = nullptr;
            return;
        }
    }
    for (uint32_t i = 0; i < window->frameRequests.count; ++i) {
        HtmlFrameRequest* request = &window->frameRequests.data[i];
        if (request->handle != handle)
            continue;
        release(request->callback);
        window->frameRequests.erase(i);
        return;
    }
}

void htmlWindowClearTasks(PrHtmlWindow* window) {
    for (uint32_t i = window->taskHead; i < window->tasks.count; ++i) {
        if (window->tasks.data[i].drop != nullptr)
            window->tasks.data[i].drop(window->tasks.data[i].data);
    }
    window->tasks.reset();
    window->taskHead = 0;
    window->delayingTasks = 0;
    for (uint32_t i = 0; i < window->frameRequests.count; ++i)
        release(window->frameRequests.data[i].callback);
    window->frameRequests.reset();
    for (uint32_t i = 0; i < window->idleRequests.count; ++i)
        release(window->idleRequests.data[i].callback);
    for (uint32_t i = 0; i < window->idleRunnable.count; ++i)
        release(window->idleRunnable.data[i].callback);
    window->idleRequests.reset();
    window->idleRunnable.reset();
    while (window->timers.count != 0) {
        PrHtmlTimerHandler handler = window->timers.last().handler;
        window->timers.erase(window->timers.count - 1);
        release(handler);
    }
    window->timers.reset();
}

bool htmlWindowIsIdle(const PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->timers.count; ++i) {
        // An interval, like an endless timer, runs until something ends it, which may never happen.
        if (!window->timers.data[i].endless && window->timers.data[i].interval < 0)
            return false;
    }
    for (uint32_t i = 0; i < window->idleRequests.count; ++i) {
        if (!window->idleRequests.data[i].endless)
            return false;
    }
    for (uint32_t i = 0; i < window->idleRunnable.count; ++i) {
        if (!window->idleRunnable.data[i].endless)
            return false;
    }
    PrCssPresentation* presentation = presentationOf(window);
    return window->taskHead == window->tasks.count && window->frameRequests.count == 0 && !window->revealing &&
        !htmlMediaAwaitsPicture(window) && (presentation == nullptr || (!cssPresentationHasWork(presentation) &&
            !cssAnimateHaveEventsAhead(&presentation->animations)));
}

bool htmlWindowIsFullyActive(const PrHtmlWindow* window) {
    for (const PrHtmlWindow* child = window; child->parent != nullptr; child = child->parent) {
        bool held = false;
        for (uint32_t i = 0; i < child->parent->frames.count; ++i)
            held = held || child->parent->frames.data[i].window == child;
        if (!held)
            return false;
    }
    return window->document != nullptr && htmlDocumentPage(window->document) != nullptr;
}

double htmlWindowNextTaskDelay(const PrHtmlWindow* window) {
    if (window->taskHead != window->tasks.count || window->idleRequests.count != 0 || window->idleRunnable.count != 0)
        return 0;
    uint32_t index = nextTimerIndex(window);
    bool frame = wantsFrame(window);
    if (index == window->timers.count && !frame)
        return -1;
    double frameAt = frameDue(window);
    double due = index != window->timers.count ? window->timers.data[index].deadline : frameAt;
    if (frame && frameAt < due)
        due = frameAt;
    double delay = due - htmlWindowClock(window);
    return delay > 0 ? delay : 0;
}

bool htmlWindowRunTask(PrHtmlWindow* window) {
    return !window->runningTask && htmlWindowSpinTask(window);
}

bool htmlWindowSpinTask(PrHtmlWindow* window) {
    // A task may replace the window's document, and with it the window, which the page then releases. The window lives
    // until the task returns.
    domEventTargetRetain(window);
    bool ran = runNextTask(window);
    domEventTargetRelease(window);
    return ran;
}

void htmlWindowRunLoadDelaying(PrHtmlWindow* window) {
    while (window->delayingTasks != 0 && !window->runningTask && runQueued(window)) {
    }
}

PrHtmlWindow* PR_NULLABLE htmlFrameWindow(const PrDomElement* container) {
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(container->document->global);
    for (uint32_t i = 0; window != nullptr && i < window->frames.count; ++i) {
        if (window->frames.data[i].container == container)
            return window->frames.data[i].window;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE htmlFrameContainer(const PrDomDocument* document) {
    const PrDomEventTarget* global = document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return nullptr;
    const PrHtmlWindow* window = static_cast<const PrHtmlWindow*>(global);
    if (window->parent == nullptr || window->document != document)
        return nullptr;
    for (uint32_t i = 0; i < window->parent->frames.count; ++i) {
        const HtmlFrame& frame = window->parent->frames.data[i];
        if (frame.window == window)
            return frame.container;
    }
    return nullptr;
}

PrHtmlWindow* PR_NULLABLE htmlWindowFrameInTreeOrder(const PrHtmlWindow* window, uint32_t index) {
    if (index >= window->frames.count)
        return nullptr;
    // The frame at `index` has that many frames before it.
    for (uint32_t i = 0; i < window->frames.count; ++i) {
        PrDomNode* node = prDomElementAsNode(window->frames.data[i].container);
        uint32_t before = 0;
        for (uint32_t k = 0; k < window->frames.count; ++k) {
            PrDomNode* other = prDomElementAsNode(window->frames.data[k].container);
            if (k != i && (prDomNodeCompareDocumentPosition(node, other) & PrDomNodePosition_Preceding) != 0)
                ++before;
        }
        if (before == index)
            return window->frames.data[i].window;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE htmlWindowNamedObject(PrHtmlWindow* window, PrStringView name) {
    if (name.size == 0)
        return nullptr;
    PrDomDocument* document = window->document;
    for (PrDomNode* node = domNodeNext(document, document); node != nullptr; node = domNodeNext(document, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        if (domElementId(element) == name)
            return element;
        if (element->space != PrDomNamespace_Html)
            continue;
        switch (element->tag) {
            case PrDomTag_Embed:
            case PrDomTag_Form:
            case PrDomTag_Img:
            case PrDomTag_Object: {
                const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "name");
                if (attribute != nullptr && prStringView(&attribute->value) == name)
                    return element;
                break;
            }
            default:
                break;
        }
    }
    return nullptr;
}

void htmlWindowScrollToFragment(PrHtmlWindow* window) {
    PrDomDocument* document = window->document;
    PrStringView written = prUrlFragment(document->url);
    if (written.data == nullptr) {
        setTarget(document, nullptr);
        return;
    }
    // The indicated part (html/#select-the-indicated-part) is an element that the fragment names, as written or
    // decoded, or the top of the document for an empty fragment or "top".
    PrDomElement* indicated = written.size != 0 ? potentialIndicated(document, written) : nullptr;
    bool top = written.size == 0;
    if (indicated == nullptr && !top) {
        PrTextBuffer decoded{};
        prTextBufferReserve(&decoded, uint32_t(written.size));
        decoded.size = uint32_t(prUrlPercentDecode(written, decoded.data, written.size));
        indicated = potentialIndicated(document, prTextBufferView(&decoded));
        top = indicated == nullptr && prAsciiEqualsFold(prTextBufferView(&decoded), "top");
        prTextBufferReset(&decoded);
    }
    setTarget(document, indicated);
    if (indicated != nullptr) {
        PrCssScrollIntoViewOptions options = prCssScrollIntoViewOptionsDefault();
        prCssElementScrollIntoView(indicated, &options);
        htmlFocusFragment(indicated);
    } else if (top) {
        cssViewScrollViewport(document, PrLayoutVec{});
    }
}

void htmlWindowFollowHyperlink(PrHtmlWindow* window, PrDomElement* element) {
    PrDomDocument* document = window->document;
    PrStringView href = domElementLinkHref(element);
    if (element->document != document || !domIsConnected(element) || href.data == nullptr)
        return;
    PrUrl* url = nullptr;
    PrStatus status = domDocumentParseUrl(document, href, &url);
    if (status != PrStatus_Ok)
        return;
    PrString text = prUrlSerialize(url, PrUrlSerialization_Whole);
    prUrlDestroy(url);
    PrHtmlWindow* target = htmlPageChooseTarget(window, hyperlinkTarget(document, element));
    if (target != nullptr)
        prHtmlWindowSetLocation(target, window, PrUrlComponent_Href, prStringView(&text), PrHtmlHistoryHandling_Push);
    else
        htmlPageOpenWindow(window, prStringView(&text));
    prStringReset(&text);
}

} // namespace pr_impl
