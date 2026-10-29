// The objects that a window presents as members. The address of each member is its handle.
//
// html - https://html.spec.whatwg.org/
// mediaqueries-5 - https://drafts.csswg.org/mediaqueries-5/
// hr-time-3 - https://w3c.github.io/hr-time/

#pragma once

#include "pr/html.h"

#include "dom/src/dom_event_target.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A user preference that a script can override (mediaqueries-5/#preferenceobject).
struct HtmlPreferenceObject : PrDomEventTarget {
    PrHtmlWindow* window;
    PrHtmlPreference preference;
};

// The clock of a window's scripts (hr-time-3/#sec-performance).
struct HtmlPerformance : PrDomEventTarget {
    PrHtmlWindow* window;
};

} // namespace pr_impl

// C opaque types
struct PrHtmlPreferenceObject : pr_impl::HtmlPreferenceObject {};
struct PrHtmlPerformance : pr_impl::HtmlPerformance {};

PR_ASSUME_NONNULL_END
