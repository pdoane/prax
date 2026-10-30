// ResizeObserver: the sizes of the boxes of observed elements, reported each time they change.
//
// resize-observer-1 - https://drafts.csswg.org/resize-observer-1/
//
// An observer belongs to a document, whose rendering update delivers its observations
// (prCssDocumentBroadcastResizeObservations). An observer is reference counted, and its caller holds a reference. The
// observer holds a reference to its document and to each element that it observes. A document's observers are used by
// its agent's thread.

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An observer of the sizes of elements' boxes (resize-observer-1/#resize-observer-interface).
typedef struct PrCssResizeObserver PrCssResizeObserver;

// The box that an observation measures (resize-observer-1/#enumdef-resizeobserverboxoptions).
enum {
    PrCssResizeObserverBox_ContentBox,
    PrCssResizeObserverBox_BorderBox,
    PrCssResizeObserverBox_DevicePixelContentBox,
};
typedef uint8_t PrCssResizeObserverBox;

// The size of a box along the inline and block axes of its writing mode (resize-observer-1/#resizeobserversize).
typedef struct PrCssResizeObserverSize {
    double inlineSize;
    double blockSize;
} PrCssResizeObserverSize;

// The sizes of an element's boxes, as an observer reports them (resize-observer-1/#resize-observer-entry-interface).
// The content rectangle is the content box, offset by the box's left and top padding.
typedef struct PrCssResizeObserverEntry {
    PrDomElement* target;
    PrDomRect contentRect;
    PrCssResizeObserverSize borderBoxSize;
    PrCssResizeObserverSize contentBoxSize;
    PrCssResizeObserverSize devicePixelContentBoxSize;
} PrCssResizeObserverEntry;

// Called with the entries of the observations that changed, which are valid for the call
// (resize-observer-1/#broadcast-active-resize-observations).
typedef void (*PrCssResizeCallbackFn)(void* PR_NULLABLE context, PrCssResizeObserver* observer,
    const PrCssResizeObserverEntry* entries, uint32_t count);

// The callback of an observer. The observer keeps a copy, and calls retain and release on the copy's context if they
// are given. `observing`, if given, is called with true when the observer starts to observe an element, and with false
// once it observes none. An observer lives while it observes an element, even if its caller holds no reference
// (resize-observer-1/#lifetime). Any data that the caller keeps for the observer must last that long.
typedef struct PrCssResizeCallback {
    PrCssResizeCallbackFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
    void (*PR_NULLABLE observing)(void* PR_NULLABLE context, PrCssResizeObserver* observer, bool observing);
} PrCssResizeCallback;

// Creates an observer of `document` that calls `callback`, with one reference that the caller holds.
PrCssResizeObserver* prCssResizeObserverCreate(PrDomDocument* document, const PrCssResizeCallback* callback);

// Adds or releases a reference. The last release frees the observer.
void prCssResizeObserverRetain(PrCssResizeObserver* self);
void prCssResizeObserverRelease(PrCssResizeObserver* PR_NULLABLE self);

// Observes `target`'s box `box`, in place of any observation of `target` that the observer had
// (resize-observer-1/#dom-resizeobserver-observe). The first delivery reports the box's size, which is zero if the
// element has no box.
void prCssResizeObserverObserve(PrCssResizeObserver* self, PrDomElement* target, PrCssResizeObserverBox box);

// Stops observing `target` (resize-observer-1/#dom-resizeobserver-unobserve), or stops observing every element
// (resize-observer-1/#dom-resizeobserver-disconnect).
void prCssResizeObserverUnobserve(PrCssResizeObserver* self, PrDomElement* target);
void prCssResizeObserverDisconnect(PrCssResizeObserver* self);

// Returns true if the next rendering update has observations to deliver that no change to the presentation asks for: an
// observation that began since the last update, or one that the last update skipped. The observations of a box whose
// size changes are gathered by the rendering update that the change to the presentation asks for.
bool prCssDocumentResizeObservationsPending(const PrDomDocument* document);

// Runs the resize observation steps of a rendering update (html/#update-the-rendering). It gathers the observations
// whose sizes changed, from a layout that it brings up to date, and calls each observer that has any. It repeats for
// the observations deeper in the flat tree than the shallowest one delivered, until none changed. `pixelRatio` is the
// number of device pixels per CSS pixel. Returns true if an observation that changed was not delivered, for which the
// caller reports the loop error (resize-observer-1/#deliver-resize-loop-error-notification).
bool prCssDocumentBroadcastResizeObservations(PrDomDocument* document, double pixelRatio);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
