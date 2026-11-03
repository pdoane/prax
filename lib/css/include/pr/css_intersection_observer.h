// IntersectionObserver: how much of each observed element shows within a root, reported each time it crosses a
// threshold.
//
// intersection-observer - https://w3c.github.io/IntersectionObserver/
//
// An observer belongs to a document. The document's rendering update computes its observations and queues their entries
// (prCssDocumentUpdateIntersectionObservations), and a task of the document's event loop delivers them
// (prCssDocumentNotifyIntersectionObservers). The root is the document's viewport, unless the observer names an element
// or a document of its own. An observer is reference counted, and its caller holds a reference. The observer holds a
// reference to its document, to its root and to each element that it observes. A document's observers are used by its
// agent's thread.

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"
#include "pr/raw_string.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An observer of the intersections of elements with a root (intersection-observer/#intersection-observer-interface).
typedef struct PrCssIntersectionObserver PrCssIntersectionObserver;

// What an observer is made with (intersection-observer/#dictdef-intersectionobserverinit). `root` is an element or a
// document, or null for the document's viewport. The margins are CSS text that intersection-observer/#parse-a-margin
// reads. The thresholds are ratios from 0 to 1, in any order, and none means a single threshold of 0. `delay` is the
// least time between two of a target's updates, in milliseconds.
typedef struct PrCssIntersectionObserverInit {
    PrDomNode* PR_NULLABLE root;
    PrStringView rootMargin;
    PrStringView scrollMargin;
    const double* PR_NULLABLE thresholds;
    uint32_t thresholdCount;
    int32_t delay;
    bool trackVisibility;
} PrCssIntersectionObserverInit;

// Returns an init with the dictionary's defaults: no root, margins of 0px, a threshold of 0, and no delay.
static inline PrCssIntersectionObserverInit prCssIntersectionObserverInitDefault(void) {
    PrCssIntersectionObserverInit value;
    value.root = NULL;
    value.rootMargin = PR_SV("0px");
    value.scrollMargin = PR_SV("0px");
    value.thresholds = NULL;
    value.thresholdCount = 0;
    value.delay = 0;
    value.trackVisibility = false;
    return value;
}

// How much of an element showed within the root at a time (intersection-observer/#intersection-observer-entry). The
// rectangles are in the viewport's coordinates. The intersection ratio is the part of the target's area that the
// intersection covers, or 1 or 0 for a target of no area by whether it intersects.
typedef struct PrCssIntersectionObserverEntry {
    double time;                    // milliseconds since the document's time origin
    PrDomRect rootBounds;
    PrDomRect boundingClientRect;
    PrDomRect intersectionRect;
    double intersectionRatio;
    PrDomElement* target;
    bool isIntersecting;
    bool isVisible;
} PrCssIntersectionObserverEntry;

// Called when the observer has entries to deliver, which the callback takes with prCssIntersectionObserverTakeRecords
// (intersection-observer/#notify-intersection-observers-algo).
typedef void (*PrCssIntersectionCallbackFn)(void* PR_NULLABLE context, PrCssIntersectionObserver* observer);

// The callback of an observer. The observer keeps a copy, and calls retain and release on the copy's context if they
// are given. `observing`, if given, is called with true when the observer starts to observe an element, and with false
// once it observes none. An observer lives while it observes an element, even if its caller holds no reference
// (intersection-observer/#lifetime). Any data that the caller keeps for the observer must last that long.
typedef struct PrCssIntersectionCallback {
    PrCssIntersectionCallbackFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
    void (*PR_NULLABLE observing)(void* PR_NULLABLE context, PrCssIntersectionObserver* observer, bool observing);
} PrCssIntersectionCallback;

// Creates an observer of `document` that calls `callback`, with one reference that the caller holds
// (intersection-observer/#initialize-new-intersection-observer). Returns Syntax for a margin that is not one, and
// OutOfRange for a threshold outside 0 to 1. On failure, `*out` is null.
PrStatus prCssIntersectionObserverCreate(PrDomDocument* document, const PrCssIntersectionObserverInit* init,
    const PrCssIntersectionCallback* callback, PrCssIntersectionObserver* PR_NULLABLE* PR_NONNULL out);

// Adds or releases a reference. The last release frees the observer and its queued entries.
void prCssIntersectionObserverRetain(PrCssIntersectionObserver* observer);
void prCssIntersectionObserverRelease(PrCssIntersectionObserver* PR_NULLABLE observer);

// Return the observer's root, or null for the document's viewport
// (intersection-observer/#dom-intersectionobserver-root), its least time between updates, and whether it tracks
// visibility.
PrDomNode* PR_NULLABLE prCssIntersectionObserverRoot(const PrCssIntersectionObserver* observer);
int32_t prCssIntersectionObserverDelay(const PrCssIntersectionObserver* observer);
bool prCssIntersectionObserverTrackVisibility(const PrCssIntersectionObserver* observer);

// Return the margins, each as its four offsets serialized (intersection-observer/#dom-intersectionobserver-rootmargin).
PrRawString prCssIntersectionObserverRootMargin(const PrCssIntersectionObserver* observer);
PrRawString prCssIntersectionObserverScrollMargin(const PrCssIntersectionObserver* observer);

// Returns the thresholds in ascending order, valid while the observer lives, and writes their number to `count`.
const double* prCssIntersectionObserverThresholds(const PrCssIntersectionObserver* observer, uint32_t* count);

// Observes `target`, unless the observer observes it already (intersection-observer/#observe-target-element). The next
// rendering update reports its intersection.
void prCssIntersectionObserverObserve(PrCssIntersectionObserver* observer, PrDomElement* target);

// Stops observing `target` (intersection-observer/#unobserve-target-element), or stops observing every element
// (intersection-observer/#dom-intersectionobserver-disconnect). Entries already queued stay queued.
void prCssIntersectionObserverUnobserve(PrCssIntersectionObserver* observer, PrDomElement* target);
void prCssIntersectionObserverDisconnect(PrCssIntersectionObserver* observer);

// Takes up to `capacity` of the oldest queued entries into `out` and returns how many it took
// (intersection-observer/#dom-intersectionobserver-takerecords). Each entry taken holds a reference to its target,
// which the caller releases. A caller takes them all by calling until it takes none.
uint32_t prCssIntersectionObserverTakeRecords(PrCssIntersectionObserver* observer,
    PrCssIntersectionObserverEntry* out, uint32_t capacity);

// Returns true if an observer of the document observes an element whose intersection no rendering update has computed
// yet (intersection-observer/#pending-initial-observation), so that the next rendering update must run.
bool prCssDocumentIntersectionObservationsPending(const PrDomDocument* document);

// Runs the update intersection observations steps of a rendering update at `time`, in milliseconds since the document's
// time origin (intersection-observer/#update-intersection-observations-algo). It computes each observed element's
// intersection from the layout, which it brings up to date, and queues an entry for each that crossed a threshold.
// Returns true if it queued an entry while no delivery was queued, for which the caller queues a task that calls
// prCssDocumentNotifyIntersectionObservers (intersection-observer/#queue-intersection-observer-task).
bool prCssDocumentUpdateIntersectionObservations(PrDomDocument* document, double time);

// Calls each observer of the document that has queued entries
// (intersection-observer/#notify-intersection-observers-algo).
void prCssDocumentNotifyIntersectionObservers(PrDomDocument* document);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
