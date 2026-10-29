// IntersectionObserver's observers, their observations and their queued entries
//
// intersection-observer - https://w3c.github.io/IntersectionObserver/

#pragma once

#include "pr/array.h"
#include "pr/css_intersection_observer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An offset of a margin: a length in pixels, or a percentage of the width of the rectangle that it grows
// (intersection-observer/#parse-a-margin).
struct CssIntersectionOffset {
    double value;
    bool percent;
};

// An element that an observer observes, and what its last update found
// (intersection-observer/#intersectionobserverregistration).
struct CssIntersectionObservation {
    PrDomElement* target;                       // holds a reference
    int32_t previousThresholdIndex;             // -1 until the first update
    bool previousIsIntersecting;
    bool previousIsVisible;
    bool updated;                               // true once an update computed the intersection
    double lastUpdateTime;                      // the time of that update, if `updated`
};

// An IntersectionObserver (intersection-observer/#intersection-observer-interface): its options, the elements that it
// observes in the order that it began to observe them, and the entries that wait for delivery, oldest first. Each
// queued entry holds a reference to its target.
struct CssIntersectionObserver {
    uint32_t references;
    PrDomDocument* document;                    // holds a reference
    PrDomNode* PR_NULLABLE root;                // holds a reference
    PrCssIntersectionCallback callback;

    // The offsets of the margins, top, right, bottom and left.
    CssIntersectionOffset rootMargin[4];
    CssIntersectionOffset scrollMargin[4];

    Array<double> thresholds;                   // ascending
    int32_t delay;
    bool trackVisibility;
    Array<CssIntersectionObservation> observations;
    Array<PrCssIntersectionObserverEntry> queued;
};

} // namespace pr_impl

// The handle of an observer.
struct PrCssIntersectionObserver : pr_impl::CssIntersectionObserver {};

PR_ASSUME_NONNULL_END
