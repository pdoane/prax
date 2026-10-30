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

// Coarsens a time to 100 microseconds, as hr-time-3/#dfn-coarsen-time specifies for a context that is not cross-origin
// isolated.
double coarsen(double milliseconds) {
    return floorF64(milliseconds * 10) / 10;
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
    double frames = floorF64((target - window->lastFrame) / kHtmlFrameInterval);
    return frames > 1 ? window->lastFrame + frames * kHtmlFrameInterval : due;
}

// Returns true if the next rendering opportunity updates the rendering. It does if there are frame requests, if the
// document is to be revealed, if a top-level document has autofocus candidates, if its animations are running, if
// scroll or animation events are due, or if its styles changed.
bool wantsFrame(const PrHtmlWindow* window) {
    if (htmlWindowRenderBlocked(window))
        return false;
    PrCssPresentation* presentation = presentationOf(window);
    return window->frameRequests.count != 0 || window->revealing || window->canvases.count != 0 ||
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

// Runs at most one task of the window: a queued task, or else the earliest due timer or animation frame. A task that
// spins the event loop runs one during its own, and is still running afterward.
bool runNextTask(PrHtmlWindow* window) {
    if (runQueued(window))
        return true;
    uint32_t index = nextTimerIndex(window);
    double now = htmlWindowClock(window);
    bool timer = index != window->timers.count && window->timers.data[index].deadline <= now;
    double frameAt = frameDue(window);
    bool frame = wantsFrame(window) && frameAt <= now;
    bool outer = window->runningTask;
    if (frame && (!timer || frameAt <= window->timers.data[index].deadline)) {
        window->runningTask = true;
        runFrame(window, now);
        window->runningTask = outer;
        return true;
    }
    if (!timer)
        return false;

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
    window->timers.reset();
    window->tasks.reset();
    window->frameRequests.reset();
    window->frameRunning.reset();
    window->canvases.reset();
    window->mediaElements.reset();
    window->frames.reset();
    prMemoryFree(window);
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
    while (window->parent != nullptr) {
        PrString href = prUrlSerialize(window->document->url, PrUrlSerialization_Whole);
        bool inherits = prStringView(&href) == "about:blank" || prStringView(&href) == "about:srcdoc";
        prStringReset(&href);
        if (!inherits)
            break;
        window = window->parent;
    }
    return prUrlOrigin(window->document->url);
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
    PrCssPresentation* presentation = presentationOf(window);
    return window->taskHead == window->tasks.count && window->frameRequests.count == 0 && !window->revealing &&
        (presentation == nullptr || (!cssPresentationHasWork(presentation) &&
            !cssAnimateHaveEventsAhead(&presentation->animations)));
}

double htmlWindowNextTaskDelay(const PrHtmlWindow* window) {
    if (window->taskHead != window->tasks.count)
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
    return !window->runningTask && runNextTask(window);
}

bool htmlWindowSpinTask(PrHtmlWindow* window) {
    return runNextTask(window);
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
        TextBuffer decoded{};
        textReserve(&decoded, uint32_t(written.size));
        decoded.text.count = uint32_t(prUrlPercentDecode(written, decoded.text.data, written.size));
        indicated = potentialIndicated(document, textView(&decoded));
        top = indicated == nullptr && asciiEqualsFold(textView(&decoded), "top");
        decoded.text.reset();
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
    prHtmlWindowSetLocation(window, PrUrlComponent_Href, prStringView(&text), PrHtmlHistoryHandling_Push);
    prStringReset(&text);
}

} // namespace pr_impl
