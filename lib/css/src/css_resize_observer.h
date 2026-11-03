// ResizeObserver's observers and their observations
//
// resize-observer-1 - https://drafts.csswg.org/resize-observer-1/

#pragma once

#include "pr/css_resize_observer.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An element that an observer observes (resize-observer-1/#resizeobservation), and the size of the observed box that
// was last delivered.
struct CssResizeObservation {
    PrDomElement* target;                       // holds a reference
    PrCssResizeObserverBox box;
    PrCssResizeObserverSize lastReported;

    // True if the last gather found that the size changed, for a target deeper than the gather's depth
    // (resize-observer-1/#dom-resizeobserver-activetargets-slot).
    bool active;
};

// A ResizeObserver (resize-observer-1/#resize-observer-interface): its callback, and the elements that it observes in
// the order that it began to observe them.
struct CssResizeObserver {
    uint32_t references;
    PrDomDocument* document;                    // holds a reference
    PrCssResizeCallback callback;
    RawArray<CssResizeObservation> observations;

    // True if the last gather found a size that changed, for a target no deeper than the gather's depth
    // (resize-observer-1/#dom-resizeobserver-skippedtargets-slot).
    bool skipped;
};

} // namespace pr_impl

// The handle of an observer.
struct PrCssResizeObserver : pr_impl::CssResizeObserver {};

PR_ASSUME_NONNULL_END
