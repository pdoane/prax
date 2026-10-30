// The objects that a window presents as members. The address of each member is its handle. Each is a part of its window
// (DomPartTarget), so a reference to a member keeps the window alive.
//
// html - https://html.spec.whatwg.org/
// mediaqueries-5 - https://drafts.csswg.org/mediaqueries-5/
// hr-time-3 - https://w3c.github.io/hr-time/
// screen-orientation - https://w3c.github.io/screen-orientation/

#pragma once

#include "pr/html.h"

#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A user preference that a script can override (mediaqueries-5/#preferenceobject).
struct HtmlPreferenceObject : DomPartTarget {
    PrHtmlWindow* window;       // the owner, as the window
    PrHtmlPreference preference;
};

// The clock of a window's scripts (hr-time-3/#sec-performance).
struct HtmlPerformance : DomPartTarget {
    PrHtmlWindow* window;       // the owner, as the window
};

// The orientation of a window's screen (screen-orientation/#screenorientation-interface).
struct HtmlScreenOrientation : DomPartTarget {
    PrHtmlWindow* window;             // the owner, as the window
    PrHtmlOrientationType reported;   // the type at the last rendering update, which a change is measured from
};

} // namespace pr_impl

// C opaque types
struct PrHtmlPreferenceObject : pr_impl::HtmlPreferenceObject {};
struct PrHtmlPerformance : pr_impl::HtmlPerformance {};
struct PrHtmlScreenOrientation : pr_impl::HtmlScreenOrientation {};

PR_ASSUME_NONNULL_END
