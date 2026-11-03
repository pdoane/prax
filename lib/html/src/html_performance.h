// The performance timeline's implementation, and how a page reports its fetches and its document's load to it.

#pragma once

#include "pr/array.h"
#include "pr/html_performance.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A metric of a Server-Timing header (server-timing/#the-performanceservertiming-interface).
struct HtmlServerMetric {
    PrString name;
    PrString description;
    double duration;
};

// An entry of a timeline.
struct HtmlPerformanceEntry {
    uint32_t references;
    uint32_t sequence;              // the order of creation, which orders entries with the same start time
    PrHtmlPerformanceEntryType type;
    PrHtmlNavigationType navigationType;
    uint32_t redirectCount;
    PrString name;
    PrString detail;
    double startTime;
    double duration;

    // The moments of a navigation or a resource, by PrHtmlPerformanceMoment.
    double moments[PrHtmlPerformanceMoment_Count];

    PrFetchInitiator initiator;
    PrFetchCacheState cacheState;
    uint16_t responseStatus;
    uint64_t transferSize;
    uint64_t encodedBodySize;
    uint64_t decodedBodySize;
    PrString contentType;
    PrString contentEncoding;
    PrStringView nextHopProtocol;
    Array<HtmlServerMetric> serverTiming;
};

// An immutable list of entries, as an observer receives them.
struct HtmlPerformanceEntryList {
    uint32_t references;
    Array<PrHtmlPerformanceEntry*> entries;     // each with a reference
};

// The entries of each type, the resource entries that wait for room, and the registered observers, each with a
// reference. Each buffer is in the order that its entries were added.
struct HtmlTimeline {
    Array<PrHtmlPerformanceEntry*> buffers[PrHtmlPerformanceEntryType_Count];   // by PrHtmlPerformanceEntryType
    Array<PrHtmlPerformanceEntry*> secondaryResources;
    Array<PrHtmlPerformanceObserver*> observers;
    PrHtmlPerformanceEntry* PR_NULLABLE navigation;     // the entry of the window's document, in its buffer
    uint64_t droppedResources;
    uint32_t resourceLimit;
    uint32_t sequence;              // the sequence of the last entry
    bool observerTaskQueued;
    bool bufferFullPending;
};

// A registration for entries.
struct HtmlPerformanceObserver {
    uint32_t references;
    PrHtmlPerformance* PR_NULLABLE performance;     // null once the window's timeline is reset
    PrHtmlPerformanceObserverCallback callback;
    Array<PrHtmlPerformanceEntry*> records;         // each with a reference
    uint32_t types;                                 // one bit for each observed PrHtmlPerformanceEntryType
    PrHtmlPerformanceObserverMode mode;
    bool requiresDropped;
};

// The two clocks at the moment that a fetch started. A fetch's moments are measured on the monotonic clock of the
// network's thread, and a window's time is measured on its host's clock, which may be a virtual clock. The anchor
// relates them. A moment is never later than the time at which the window learns of it.
struct HtmlFetchAnchor {
    double windowTime;              // the window's time, in milliseconds from its time origin
    double clock;                   // the time on the clock of the window's host (htmlWindowClock)
    double monotonic;               // the time on the clock of prTimeMonotonicMilliseconds
};

// Returns the anchor of a fetch that starts now.
HtmlFetchAnchor htmlPerformanceFetchAnchor(const PrHtmlWindow* window);

// What a page reports of a finished fetch.
struct HtmlResourceReport {
    PrStringView url;
    PrFetchInitiator initiator;
    PrFetchTiming timing;
    HtmlFetchAnchor anchor;
    PrFetchResponseType responseType;
    uint16_t responseStatus;
    PrStringView contentType;       // the Content-Type header, or the null view for none
    PrStringView contentEncoding;   // the Content-Encoding header, or the null view for none
    PrStringView serverTiming;      // the Server-Timing headers joined by commas, or the null view for none
    PrStringView sentReferrer;      // the referrer that the request sent, or the null view for none
};

// Adds an entry for a fetch to the window's timeline (resource-timing/#dfn-mark-resource-timing). A fetch that does not
// use HTTP has no entry.
void htmlPerformanceMarkResource(PrHtmlWindow* window, const HtmlResourceReport* report);

// Adds the entry of the navigation of the window's document at `url` to the timeline, without passing it to observers
// (navigation-timing-2/#create-the-navigation-timing-entry). The window's time origin is the navigation's start. A null
// `report` stands for a document that no fetch of the page fetched, whose fetch's moments are now.
void htmlPerformanceBeginNavigation(PrHtmlWindow* window, PrStringView url, const HtmlResourceReport* PR_NULLABLE report,
    PrHtmlNavigationType type);

// Updates the navigation's entry with the measurements of its finished fetch.
void htmlPerformanceEndNavigationFetch(PrHtmlWindow* window, const HtmlResourceReport* report);

// Records a moment of the document's load at the window's current time. A moment keeps its first value, as
// document.open() would otherwise change it (navigation-timing-2/#dom-performancetiming-domcomplete).
void htmlPerformanceLoadMoment(PrHtmlWindow* window, PrHtmlPerformanceMoment moment);

// Passes the navigation's entry to the observers, once its document has loaded
// (navigation-timing-2/#queue-the-navigation-timing-entry).
void htmlPerformanceQueueNavigation(PrHtmlWindow* window);

// Unregisters the observers and frees the timeline of a window that is closing.
void htmlPerformanceReset(PrHtmlPerformance* performance);

} // namespace pr_impl

// C opaque types
struct PrHtmlPerformanceEntry : pr_impl::HtmlPerformanceEntry {};
struct PrHtmlPerformanceEntryList : pr_impl::HtmlPerformanceEntryList {};
struct PrHtmlPerformanceObserver : pr_impl::HtmlPerformanceObserver {};

PR_ASSUME_NONNULL_END
