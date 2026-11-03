// The performance timeline of a window: its marks and measures, the timing of its document's navigation and of the
// resources that it fetches, and the observers of these entries.
//
// performance-timeline - https://w3c.github.io/performance-timeline/
// user-timing - https://w3c.github.io/user-timing/
// navigation-timing - https://w3c.github.io/navigation-timing/
// resource-timing - https://w3c.github.io/resource-timing/

#pragma once

#include "pr/base.h"
#include "pr/fetch.h"
#include "pr/html.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The types of entry on a timeline, in the alphabetical order of supportedEntryTypes. Count stands for any type where a
// call filters by type.
enum {
    PrHtmlPerformanceEntryType_Mark,
    PrHtmlPerformanceEntryType_Measure,
    PrHtmlPerformanceEntryType_Navigation,
    PrHtmlPerformanceEntryType_Resource,
    PrHtmlPerformanceEntryType_Count,
};
typedef uint8_t PrHtmlPerformanceEntryType;

// Returns the name of `type` as entryType spells it.
PrStringView prHtmlPerformanceEntryTypeName(PrHtmlPerformanceEntryType type);

// Returns the type that `name` spells, or Count if it spells none. The match is case sensitive.
PrHtmlPerformanceEntryType prHtmlPerformanceEntryTypeFromName(PrStringView name);

// The exceptions that the timeline's operations throw.
enum {
    PrHtmlPerformanceError_None,
    PrHtmlPerformanceError_Type,            // a TypeError
    PrHtmlPerformanceError_Syntax,          // a SyntaxError
    PrHtmlPerformanceError_InvalidAccess,   // an InvalidAccessError
};
typedef uint8_t PrHtmlPerformanceError;

// The moments of a fetch and of a document's load. The moments up to LegacyCount are the attributes of the obsolete
// PerformanceTiming (navigation-timing-2/#the-performancetiming-interface), whose names user timing reserves.
enum {
    PrHtmlPerformanceMoment_NavigationStart,
    PrHtmlPerformanceMoment_UnloadEventStart,
    PrHtmlPerformanceMoment_UnloadEventEnd,
    PrHtmlPerformanceMoment_RedirectStart,
    PrHtmlPerformanceMoment_RedirectEnd,
    PrHtmlPerformanceMoment_FetchStart,
    PrHtmlPerformanceMoment_DomainLookupStart,
    PrHtmlPerformanceMoment_DomainLookupEnd,
    PrHtmlPerformanceMoment_ConnectStart,
    PrHtmlPerformanceMoment_ConnectEnd,
    PrHtmlPerformanceMoment_SecureConnectionStart,
    PrHtmlPerformanceMoment_RequestStart,
    PrHtmlPerformanceMoment_ResponseStart,
    PrHtmlPerformanceMoment_ResponseEnd,
    PrHtmlPerformanceMoment_DomLoading,
    PrHtmlPerformanceMoment_DomInteractive,
    PrHtmlPerformanceMoment_DomContentLoadedEventStart,
    PrHtmlPerformanceMoment_DomContentLoadedEventEnd,
    PrHtmlPerformanceMoment_DomComplete,
    PrHtmlPerformanceMoment_LoadEventStart,
    PrHtmlPerformanceMoment_LoadEventEnd,
    PrHtmlPerformanceMoment_LegacyCount,

    PrHtmlPerformanceMoment_WorkerStart = PrHtmlPerformanceMoment_LegacyCount,
    PrHtmlPerformanceMoment_FirstInterimResponseStart,
    PrHtmlPerformanceMoment_FinalResponseHeadersStart,
    PrHtmlPerformanceMoment_Count,
};
typedef uint8_t PrHtmlPerformanceMoment;

// Returns the name of `moment` as its attribute spells it.
PrStringView prHtmlPerformanceMomentName(PrHtmlPerformanceMoment moment);

// How a navigation reached its document (navigation-timing-2/#enumdef-navigationtimingtype). The values are those of
// PerformanceNavigation's constants.
enum {
    PrHtmlNavigationType_Navigate,
    PrHtmlNavigationType_Reload,
    PrHtmlNavigationType_BackForward,
};
typedef uint8_t PrHtmlNavigationType;

// ---------------------------------------------------------------------------------------------------------------------
// Entries

// An entry of a timeline. An entry is immutable, except that a navigation's moments and duration are filled in as its
// document loads. An entry is reference counted.
typedef struct PrHtmlPerformanceEntry PrHtmlPerformanceEntry;

// Adds or releases a reference. The last release frees the entry.
void prHtmlPerformanceEntryRetain(PrHtmlPerformanceEntry* entry);
void prHtmlPerformanceEntryRelease(PrHtmlPerformanceEntry* PR_NULLABLE entry);

// The entry's attributes (performance-timeline/#the-performanceentry-interface).
PrStringView prHtmlPerformanceEntryName(const PrHtmlPerformanceEntry* entry);
PrHtmlPerformanceEntryType prHtmlPerformanceEntryType(const PrHtmlPerformanceEntry* entry);
double prHtmlPerformanceEntryStartTime(const PrHtmlPerformanceEntry* entry);
double prHtmlPerformanceEntryDuration(const PrHtmlPerformanceEntry* entry);

// The serialized detail of a mark or a measure, as prHtmlPerformanceEntryCreateMark received it, or the null view for
// none. The view is valid while the entry lives.
PrStringView prHtmlPerformanceEntryDetail(const PrHtmlPerformanceEntry* entry);

// A moment of a navigation or a resource, in milliseconds from the time origin, or 0 if it has not happened or the
// response does not reveal it. Another type of entry has no moments.
double prHtmlPerformanceEntryMoment(const PrHtmlPerformanceEntry* entry, PrHtmlPerformanceMoment moment);

// What a navigation or a resource reports of its response (resource-timing/#sec-performanceresourcetiming). The views
// are valid while the entry lives.
typedef struct PrHtmlResourceInfo {
    PrFetchInitiator initiator;
    PrFetchCacheState cacheState;
    uint16_t responseStatus;
    uint64_t transferSize;
    uint64_t encodedBodySize;
    uint64_t decodedBodySize;
    PrStringView contentType;
    PrStringView contentEncoding;
    PrStringView nextHopProtocol;
} PrHtmlResourceInfo;

// Returns what a navigation or a resource reports. Another type of entry reports nothing.
PrHtmlResourceInfo prHtmlPerformanceEntryResourceInfo(const PrHtmlPerformanceEntry* entry);

// A metric that a response reports (server-timing/#the-performanceservertiming-interface). The views are valid while
// the entry lives.
typedef struct PrHtmlServerTiming {
    PrStringView name;
    PrStringView description;
    double duration;
} PrHtmlServerTiming;

// Read the metrics of a navigation or a resource, by count and index.
uint32_t prHtmlPerformanceEntryServerTimingCount(const PrHtmlPerformanceEntry* entry);
PrHtmlServerTiming prHtmlPerformanceEntryServerTiming(const PrHtmlPerformanceEntry* entry, uint32_t index);

// Returns how the navigation of a navigation entry reached its document, and how many redirects it followed from its
// own origin.
PrHtmlNavigationType prHtmlPerformanceEntryNavigationType(const PrHtmlPerformanceEntry* entry);
uint32_t prHtmlPerformanceEntryRedirectCount(const PrHtmlPerformanceEntry* entry);

// A list of entries (performance-timeline/#performanceobserverentrylist-interface). A list is immutable and reference
// counted.
typedef struct PrHtmlPerformanceEntryList PrHtmlPerformanceEntryList;

// Adds or releases a reference. The last release frees the list.
void prHtmlPerformanceEntryListRetain(PrHtmlPerformanceEntryList* list);
void prHtmlPerformanceEntryListRelease(PrHtmlPerformanceEntryList* PR_NULLABLE list);

// Writes up to `capacity` entries of the list to `out` in the order of their start times, and returns the total: those
// of `type` unless it is Count, and those named `name` unless it is the null view
// (performance-timeline/#filter-buffer-by-name-and-type). The entries are valid while the list lives.
uint32_t prHtmlPerformanceEntryListEntries(const PrHtmlPerformanceEntryList* list, PrHtmlPerformanceEntryType type,
    PrStringView name, PrHtmlPerformanceEntry* PR_NONNULL* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// The timeline

// Writes up to `capacity` of the timeline's entries to `out` in the order of their start times, and returns the total:
// those of `type` unless it is Count, and those named `name` unless it is the null view
// (performance-timeline/#filter-buffer-map-by-name-and-type). The entries are valid until the timeline changes.
uint32_t prHtmlPerformanceEntries(const PrHtmlPerformance* performance, PrHtmlPerformanceEntryType type, PrStringView name,
    PrHtmlPerformanceEntry* PR_NONNULL* PR_NULLABLE out, uint32_t capacity);

// Creates a mark that is not on the timeline, as PerformanceMark's constructor does, with a reference that the caller
// holds. A null `startTime` means the current time. `detail` is an opaque serialized value, or the null view for none.
// On an error, `*out` is null (user-timing/#the-performancemark-constructor).
PrHtmlPerformanceError prHtmlPerformanceEntryCreateMark(PrHtmlPerformance* performance, PrStringView name,
    const double* PR_NULLABLE startTime, PrStringView detail, PrHtmlPerformanceEntry* PR_NULLABLE* PR_NONNULL out);

// Adds a mark that prHtmlPerformanceEntryCreateMark created to the timeline, and passes it to the observers of its
// type.
void prHtmlPerformanceMarkQueue(PrHtmlPerformance* performance, PrHtmlPerformanceEntry* mark);

// The kinds of value that can name a moment for a measure.
enum {
    PrHtmlPerformanceTimestampKind_None,
    PrHtmlPerformanceTimestampKind_Name,    // the name of a mark, or of an attribute of PerformanceTiming
    PrHtmlPerformanceTimestampKind_Time,    // a time in milliseconds from the time origin
};
typedef uint8_t PrHtmlPerformanceTimestampKind;

// A moment that a measure names.
typedef struct PrHtmlPerformanceTimestamp {
    PrHtmlPerformanceTimestampKind kind;
    PrStringView name;
    double time;
} PrHtmlPerformanceTimestamp;

// The arguments of performance.measure() (user-timing/#measure-method).
typedef struct PrHtmlPerformanceMeasureDesc {
    PrStringView name;
    PrHtmlPerformanceTimestamp start;
    PrHtmlPerformanceTimestamp end;
    double duration;
    bool hasDuration;

    // True if the second argument was a dictionary with any member, which constrains the other arguments.
    bool hasOptions;

    PrStringView endMark;   // the third argument, or the null view if it was not given
    PrStringView detail;    // an opaque serialized value, or the null view for none
} PrHtmlPerformanceMeasureDesc;

// Adds a measure to the timeline, and writes it to `*out` with a reference that the caller holds. On an error, the
// timeline does not change and `*out` is null.
PrHtmlPerformanceError prHtmlPerformanceMeasure(PrHtmlPerformance* performance, const PrHtmlPerformanceMeasureDesc* desc,
    PrHtmlPerformanceEntry* PR_NULLABLE* PR_NONNULL out);

// Removes the entries of `type`, which is Mark or Measure, that are named `name`, or all of them for the null view.
void prHtmlPerformanceClear(PrHtmlPerformance* performance, PrHtmlPerformanceEntryType type, PrStringView name);

// Removes every resource entry (resource-timing/#dom-performance-clearresourcetimings).
void prHtmlPerformanceClearResourceTimings(PrHtmlPerformance* performance);

// Sets how many resource entries the timeline holds (resource-timing/#dom-performance-setresourcetimingbuffersize). It
// keeps the entries that it holds beyond a smaller limit. A new entry that does not fit fires resourcetimingbufferfull
// at the Performance.
void prHtmlPerformanceSetResourceTimingBufferSize(PrHtmlPerformance* performance, uint32_t size);

// Returns a moment of the window's document as the obsolete PerformanceTiming reports it: whole milliseconds since the
// Unix epoch, or 0 if it has not happened. `moment` is below LegacyCount.
double prHtmlPerformanceTimingMoment(const PrHtmlPerformance* performance, PrHtmlPerformanceMoment moment);

// Returns how the navigation of the window's document reached it, and how many redirects it followed from its own
// origin, as the obsolete PerformanceNavigation reports them.
PrHtmlNavigationType prHtmlPerformanceNavigationType(const PrHtmlPerformance* performance);
uint32_t prHtmlPerformanceRedirectCount(const PrHtmlPerformance* performance);

// ---------------------------------------------------------------------------------------------------------------------
// Observers (performance-timeline/#the-performanceobserver-interface)

// A registration for entries of a timeline, which receives them in tasks. An observer is reference counted.
typedef struct PrHtmlPerformanceObserver PrHtmlPerformanceObserver;

// Called in a task with the entries that waited for the observer. `droppedEntries` points to the number of entries that
// the observed types dropped, for the first call after an observe, and is null otherwise. `entries` and
// `droppedEntries` are valid during the call.
typedef void (*PrHtmlPerformanceObserverFn)(void* PR_NULLABLE context, PrHtmlPerformanceObserver* observer,
    PrHtmlPerformanceEntryList* entries, const uint64_t* PR_NULLABLE droppedEntries);

// The function that an observer calls, with its context. The observer holds a reference to the context, through
// `retain` and `release`. `registered` is called with true when the observer begins to observe, and with false when it
// stops, so that a host can keep what the calls need for that time.
typedef struct PrHtmlPerformanceObserverCallback {
    PrHtmlPerformanceObserverFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
    void (*PR_NULLABLE registered)(void* PR_NULLABLE context, PrHtmlPerformanceObserver* observer, bool registered);
} PrHtmlPerformanceObserverCallback;

// Which form of observe() an observer uses, which it may not change.
enum {
    PrHtmlPerformanceObserverMode_None,
    PrHtmlPerformanceObserverMode_Multiple, // entryTypes, where a call replaces the types of the last
    PrHtmlPerformanceObserverMode_Single,   // type, where calls add up
};
typedef uint8_t PrHtmlPerformanceObserverMode;

// A call of observe(), after the types that the timeline does not support are removed.
typedef struct PrHtmlPerformanceObserverInit {
    PrHtmlPerformanceObserverMode mode;
    uint32_t types;             // one bit for each PrHtmlPerformanceEntryType
    bool buffered;              // the entries already on the timeline are delivered too; only for Single
} PrHtmlPerformanceObserverInit;

// Creates an observer of the timeline, with one reference that the caller holds.
PrHtmlPerformanceObserver* prHtmlPerformanceObserverCreate(PrHtmlPerformance* performance,
    const PrHtmlPerformanceObserverCallback* callback);

// Adds or releases a reference. The last release frees the observer.
void prHtmlPerformanceObserverRetain(PrHtmlPerformanceObserver* observer);
void prHtmlPerformanceObserverRelease(PrHtmlPerformanceObserver* PR_NULLABLE observer);

// Observes entries as `init` says. Returns false if the observer already uses the other mode, which throws an
// InvalidModificationError.
bool prHtmlPerformanceObserverObserve(PrHtmlPerformanceObserver* observer, const PrHtmlPerformanceObserverInit* init);

// Stops observing, and discards the entries that wait.
void prHtmlPerformanceObserverDisconnect(PrHtmlPerformanceObserver* observer);

// Removes the entries that wait, and returns them as a list with a reference that the caller holds.
PrHtmlPerformanceEntryList* prHtmlPerformanceObserverTakeRecords(PrHtmlPerformanceObserver* observer);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
