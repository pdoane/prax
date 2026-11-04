// The performance timeline of a window.

#include "html/src/html_performance.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/mime.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/time.h"

#include "dom/src/dom_event.h"
#include "dom/src/dom_microsyntax.h"
#include "html/src/html_window.h"

namespace pr_impl {

namespace {

constexpr PrStringView kTypeNames[] = {"mark", "measure", "navigation", "resource"};

constexpr PrStringView kMomentNames[] = {
    "navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd", "fetchStart",
    "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart", "requestStart",
    "responseStart", "responseEnd", "domLoading", "domInteractive", "domContentLoadedEventStart",
    "domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd", "workerStart",
    "firstInterimResponseStart", "finalResponseHeadersStart",
};

// The earliest time of a moment that happened, since 0 says that it has not. A host's clock may stand still, as a
// virtual clock does while a page loads. It is the unit of the window's coarsened time.
constexpr double kFirstMoment = 0.1;

// The recommended minimum of resource-timing/#performance-resource-timing-buffer-size-limit.
constexpr uint32_t kDefaultResourceLimit = 250;

// What transferSize adds for a response's headers, in place of their real size
// (resource-timing/#dom-performanceresourcetiming-transfersize).
constexpr uint64_t kHeaderTransferBytes = 300;

void resetServerTiming(PrHtmlPerformanceEntry* entry) {
    for (uint32_t i = 0; i < entry->serverTiming.count; ++i) {
        prRawStringReset(&entry->serverTiming.data[i].name);
        prRawStringReset(&entry->serverTiming.data[i].description);
    }
    entry->serverTiming.reset();
}

HtmlTimeline* ensureTimeline(PrHtmlPerformance* performance) {
    if (performance->timeline == nullptr) {
        performance->timeline = allocZeroed<HtmlTimeline>();
        performance->timeline->resourceLimit = kDefaultResourceLimit;
    }
    return performance->timeline;
}

PrHtmlPerformanceEntry* createEntry(PrHtmlPerformance* performance, PrHtmlPerformanceEntryType type, PrStringView name) {
    PrHtmlPerformanceEntry* entry = allocZeroed<PrHtmlPerformanceEntry>();
    entry->references = 1;
    entry->sequence = ++ensureTimeline(performance)->sequence;
    entry->type = type;
    entry->name = prRawStringCopy(name);
    return entry;
}

bool isLegacyMomentName(PrStringView name, PrHtmlPerformanceMoment* out) {
    for (uint32_t i = 0; i < PrHtmlPerformanceMoment_LegacyCount; ++i) {
        if (kMomentNames[i] == name) {
            *out = PrHtmlPerformanceMoment(i);
            return true;
        }
    }
    return false;
}

void releaseEntries(RawArray<PrHtmlPerformanceEntry*>* entries) {
    for (uint32_t i = 0; i < entries->count; ++i)
        prHtmlPerformanceEntryRelease(entries->data[i]);
    entries->reset();
}

bool entryMatches(const PrHtmlPerformanceEntry* entry, PrHtmlPerformanceEntryType type, PrStringView name) {
    return (type == PrHtmlPerformanceEntryType_Count || entry->type == type) &&
        (name.data == nullptr || prRawStringView(&entry->name) == name);
}

// Writes `entries` to `out` in the order of their start times, and frees the array.
uint32_t writeSorted(RawArray<PrHtmlPerformanceEntry*>* entries, PrHtmlPerformanceEntry* PR_NONNULL* PR_NULLABLE out,
        uint32_t capacity) {
    sortItems(entries->span(), [](const PrHtmlPerformanceEntry* a, const PrHtmlPerformanceEntry* b) {
        return a->startTime != b->startTime ? a->startTime < b->startTime : a->sequence < b->sequence;
    });
    uint32_t total = entries->count;
    for (uint32_t i = 0; i < total && i < capacity; ++i)
        out[i] = entries->data[i];
    entries->reset();
    return total;
}

uint64_t droppedOf(const HtmlTimeline* timeline, uint32_t types) {
    return (types & (1u << PrHtmlPerformanceEntryType_Resource)) != 0 ? timeline->droppedResources : 0;
}

void notifyObserver(PrHtmlPerformanceObserver* observer) {
    if (observer->records.count == 0 || observer->performance == nullptr)
        return;
    PrHtmlPerformanceEntryList* list = prHtmlPerformanceObserverTakeRecords(observer);
    uint64_t dropped = droppedOf(observer->performance->timeline, observer->types);
    bool reportsDropped = observer->requiresDropped;
    observer->requiresDropped = false;
    observer->callback.invoke(observer->callback.context, observer, list, reportsDropped ? &dropped : nullptr);
    prHtmlPerformanceEntryListRelease(list);
}

// Runs the task of performance-timeline/#dfn-queue-the-performanceobserver-task.
void notifyObservers(void* data) {
    PrHtmlPerformance* performance = static_cast<PrHtmlPerformance*>(data);
    HtmlTimeline* timeline = performance->timeline;
    timeline->observerTaskQueued = false;
    RawArray<PrHtmlPerformanceObserver*> list;
    for (uint32_t i = 0; i < timeline->observers.count; ++i) {
        prHtmlPerformanceObserverRetain(timeline->observers.data[i]);
        list.push(timeline->observers.data[i]);
    }
    for (uint32_t i = 0; i < list.count; ++i) {
        notifyObserver(list.data[i]);
        prHtmlPerformanceObserverRelease(list.data[i]);
    }
    list.reset();
}

void queueObserverTask(PrHtmlPerformance* performance) {
    HtmlTimeline* timeline = ensureTimeline(performance);
    if (timeline->observerTaskQueued)
        return;
    timeline->observerTaskQueued = true;
    htmlWindowQueueTask(performance->window, notifyObservers, performance, nullptr);
}

// Passes the entry to the observers of its type (performance-timeline/#dfn-queue-a-performanceentry).
void passToObservers(PrHtmlPerformance* performance, PrHtmlPerformanceEntry* entry) {
    HtmlTimeline* timeline = ensureTimeline(performance);
    bool received = false;
    for (uint32_t i = 0; i < timeline->observers.count; ++i) {
        PrHtmlPerformanceObserver* observer = timeline->observers.data[i];
        if ((observer->types & (1u << entry->type)) != 0) {
            prHtmlPerformanceEntryRetain(entry);
            observer->records.push(entry);
            received = true;
        }
    }
    // A task with nothing to deliver has no effect that script can see, and would keep the event loop awake.
    if (received)
        queueObserverTask(performance);
}

void addToBuffer(HtmlTimeline* timeline, PrHtmlPerformanceEntry* entry) {
    prHtmlPerformanceEntryRetain(entry);
    timeline->buffers[entry->type].push(entry);
}

void copySecondaryBuffer(HtmlTimeline* timeline) {
    RawArray<PrHtmlPerformanceEntry*>& resources = timeline->buffers[PrHtmlPerformanceEntryType_Resource];
    uint32_t moved = 0;
    while (moved < timeline->secondaryResources.count && resources.count < timeline->resourceLimit)
        resources.push(timeline->secondaryResources.data[moved++]);
    timeline->secondaryResources.erase(0, moved);
}

// Runs resource-timing/#dfn-fire-a-buffer-full-event.
void fireBufferFull(void* data) {
    PrHtmlPerformance* performance = static_cast<PrHtmlPerformance*>(data);
    HtmlTimeline* timeline = performance->timeline;
    while (timeline->secondaryResources.count != 0) {
        uint32_t before = timeline->secondaryResources.count;
        if (timeline->buffers[PrHtmlPerformanceEntryType_Resource].count >= timeline->resourceLimit)
            domEventFire(prHtmlPerformanceAsEventTarget(performance), "resourcetimingbufferfull");
        // A listener can close the window, which frees the timeline.
        timeline = performance->timeline;
        if (timeline == nullptr)
            return;
        copySecondaryBuffer(timeline);
        if (before <= timeline->secondaryResources.count) {
            releaseEntries(&timeline->secondaryResources);
            break;
        }
    }
    timeline->bufferFullPending = false;
}

// Runs resource-timing/#dfn-add-a-PerformanceResourceTiming-entry. An entry that finds the buffer full counts as
// dropped for observers (performance-timeline/#dfn-determine-if-a-performance-entry-buffer-is-full), even if a listener
// of the buffer's full event makes room for it.
void addResource(PrHtmlPerformance* performance, PrHtmlPerformanceEntry* entry) {
    HtmlTimeline* timeline = ensureTimeline(performance);
    bool room = timeline->buffers[PrHtmlPerformanceEntryType_Resource].count < timeline->resourceLimit;
    if (room && !timeline->bufferFullPending) {
        addToBuffer(timeline, entry);
        return;
    }
    ++timeline->droppedResources;
    if (!timeline->bufferFullPending) {
        timeline->bufferFullPending = true;
        htmlWindowQueueTask(performance->window, fireBufferFull, performance, nullptr);
    }
    prHtmlPerformanceEntryRetain(entry);
    timeline->secondaryResources.push(entry);
}

// Converts a moment of a fetch to the window's time, or leaves 0 for one that has not happened.
double fetchMoment(const HtmlFetchAnchor& anchor, double moment, double now) {
    if (moment == 0)
        return 0;
    double time = anchor.windowTime + moment - anchor.monotonic;
    time = time < now ? time : now;
    return time > kFirstMoment ? time : kFirstMoment;
}

// What a report's entry shows of its fetch.
enum {
    FetchRevealFlag_Timing = 1u << 0,
    FetchRevealFlag_Redirects = 1u << 1,
};
typedef uint8_t FetchRevealFlags;

// Appends the metric of one field of a Server-Timing header, unless it has no name
// (server-timing/#parse-a-server-timing-header-field).
void appendServerTiming(PrHtmlPerformanceEntry* entry, PrStringView field) {
    size_t at = 0;
    while (at < field.size && field.data[at] != ';')
        ++at;
    PrStringView name = prStringTrim(prStringSlice(field, 0, at));
    if (name.size == 0)
        return;
    HtmlServerMetric metric{.name = prRawStringCopy(name), .description = prRawStringCopy("")};
    bool hasDuration = false;
    bool hasDescription = false;
    while (at < field.size) {
        ++at;
        size_t start = at;
        while (at < field.size && field.data[at] != '=' && field.data[at] != ';')
            ++at;
        PrStringView param = prStringTrim(prStringSlice(field, start, at - start));
        if (at == field.size || field.data[at] == ';') {
            hasDuration = hasDuration || prAsciiEqualsFold(param, "dur");
            hasDescription = hasDescription || prAsciiEqualsFold(param, "desc");
            continue;
        }
        ++at;
        while (at < field.size && prAsciiIsSpace(field.data[at]))
            ++at;
        PrTextBuffer value{};
        if (at < field.size && field.data[at] == '"') {
            ++at;
            while (at < field.size && field.data[at] != '"') {
                if (field.data[at] == '\\' && at + 1 < field.size)
                    ++at;
                prTextBufferAppend(&value, PrStringView{field.data + at, 1});
                ++at;
            }
            // A quoted string that does not end has no value.
            if (at == field.size)
                prTextBufferClear(&value);
        } else {
            // An unquoted value is the token that it starts with.
            start = at;
            while (at < field.size && prFetchIsHeaderName(PrStringView{field.data + at, 1}))
                ++at;
            prTextBufferAppend(&value, prStringSlice(field, start, at - start));
        }
        while (at < field.size && field.data[at] != ';')
            ++at;
        if (prAsciiEqualsFold(param, "dur") && !hasDuration) {
            hasDuration = true;
            double duration = 0;
            metric.duration = domParseFloat(prTextBufferView(&value), &duration) ? duration : 0;
        } else if (prAsciiEqualsFold(param, "desc") && !hasDescription) {
            hasDescription = true;
            prRawStringReset(&metric.description);
            metric.description = prRawStringCopy(prTextBufferView(&value));
        }
        prTextBufferReset(&value);
    }
    entry->serverTiming.push(metric);
}

// Replaces the entry's metrics with those of `header`, the Server-Timing headers joined by commas. A comma in a quoted
// string does not end a field.
void parseServerTiming(PrHtmlPerformanceEntry* entry, PrStringView header) {
    resetServerTiming(entry);
    size_t start = 0;
    bool quoted = false;
    for (size_t at = 0; at <= header.size; ++at) {
        if (at == header.size || (header.data[at] == ',' && !quoted)) {
            appendServerTiming(entry, prStringSlice(header, start, at - start));
            start = at + 1;
        } else if (header.data[at] == '\\' && quoted && at + 1 < header.size) {
            ++at;
        } else if (header.data[at] == '"') {
            quoted = !quoted;
        }
    }
}

// Fills the entry's response information and the moments of its fetch. The moments of a connection, which the fetcher
// does not measure, are those of a connection that was reused, which are fetchStart
// (navigation-timing-2/#dom-performancetiming-domainlookupstart).
void fillFromReport(PrHtmlPerformanceEntry* entry, const PrHtmlWindow* window, const HtmlResourceReport* report,
        FetchRevealFlags reveal) {
    bool revealTiming = (reveal & FetchRevealFlag_Timing) != 0;
    double now = htmlWindowNow(window);
    const PrFetchTiming& timing = report->timing;
    const HtmlFetchAnchor& anchor = report->anchor;
    double* moments = entry->moments;
    double start = fetchMoment(anchor, timing.startTime, now);
    double end = fetchMoment(anchor, timing.endTime, now);
    if (timing.startTime == 0)
        start = anchor.windowTime;
    if (timing.endTime == 0)
        end = now;
    moments[PrHtmlPerformanceMoment_ResponseEnd] = end;
    if (!revealTiming) {
        moments[PrHtmlPerformanceMoment_FetchStart] = start;
    } else {
        double fetchStart = timing.postRedirectStart != 0 ? fetchMoment(anchor, timing.postRedirectStart, now) : start;
        double responseStart = fetchMoment(anchor, timing.responseStart, now);
        moments[PrHtmlPerformanceMoment_FetchStart] = fetchStart;
        moments[PrHtmlPerformanceMoment_DomainLookupStart] = fetchStart;
        moments[PrHtmlPerformanceMoment_DomainLookupEnd] = fetchStart;
        moments[PrHtmlPerformanceMoment_ConnectStart] = fetchStart;
        moments[PrHtmlPerformanceMoment_ConnectEnd] = fetchStart;
        if (prAsciiStartsWithFold(report->url, "https:"))
            moments[PrHtmlPerformanceMoment_SecureConnectionStart] = fetchStart;
        moments[PrHtmlPerformanceMoment_RequestStart] = fetchMoment(anchor, timing.requestStart, now);
        moments[PrHtmlPerformanceMoment_ResponseStart] = responseStart;
        moments[PrHtmlPerformanceMoment_FinalResponseHeadersStart] = responseStart;
    }
    if ((reveal & FetchRevealFlag_Redirects) != 0) {
        moments[PrHtmlPerformanceMoment_RedirectStart] = fetchMoment(anchor, timing.redirectStart, now);
        moments[PrHtmlPerformanceMoment_RedirectEnd] = fetchMoment(anchor, timing.redirectEnd, now);
        entry->redirectCount = timing.redirectCount;
    }
    bool opaque = report->responseType == PrFetchResponseType_Opaque ||
        report->responseType == PrFetchResponseType_OpaqueRedirect;
    entry->responseStatus = opaque ? 0 : report->responseStatus;
    if (revealTiming) {
        entry->cacheState = timing.cacheState;
        entry->nextHopProtocol = timing.nextHopProtocol;
        parseServerTiming(entry, report->serverTiming);
    }
    if (!opaque) {
        entry->encodedBodySize = timing.encodedBodySize;
        entry->decodedBodySize = timing.decodedBodySize;
        prRawStringReset(&entry->contentType);
        if (report->contentType.data != nullptr) {
            PrTextBuffer essence{};
            prTextBufferAppend(&essence, prMimeTypeEssence(report->contentType));
            prTextBufferAsciiLower(&essence);
            entry->contentType = prRawStringCopy(prMimeTypeMinimize(prTextBufferView(&essence)));
            prTextBufferReset(&essence);
        }
        prRawStringReset(&entry->contentEncoding);
        PrStringView coding = report->contentEncoding;
        if (coding == "gzip" || coding == "x-gzip" || coding == "deflate" || coding == "br" || coding == "zstd")
            entry->contentEncoding = prRawStringCopy(coding);
    }
    // A response that fails the TAO check does not reveal its size on the wire.
    if (entry->cacheState == PrFetchCacheState_Local || !revealTiming)
        entry->transferSize = 0;
    else if (entry->cacheState == PrFetchCacheState_Validated)
        entry->transferSize = kHeaderTransferBytes;
    else
        entry->transferSize = entry->encodedBodySize + kHeaderTransferBytes;
}

// Navigation timing has no TAO check, since a document may read the timing of its own fetch. It hides only a redirect
// from another origin.
FetchRevealFlags navigationReveal(const HtmlResourceReport* report) {
    FetchRevealFlags reveal = FetchRevealFlag_Timing;
    if (!report->timing.crossOriginRedirect)
        reveal |= FetchRevealFlag_Redirects;
    return reveal;
}

bool isHttpUrl(PrStringView url) {
    return url.size >= 5 && (prAsciiStartsWithFold(url, "http:") || prAsciiStartsWithFold(url, "https:"));
}

// Runs user-timing/#convert-a-mark-to-a-timestamp.
PrHtmlPerformanceError timestampOf(const PrHtmlPerformance* performance, const PrHtmlPerformanceTimestamp& stamp,
        double* out) {
    if (stamp.kind == PrHtmlPerformanceTimestampKind_Time) {
        if (stamp.time < 0)
            return PrHtmlPerformanceError_Type;
        *out = stamp.time;
        return PrHtmlPerformanceError_None;
    }
    PrHtmlPerformanceMoment moment = 0;
    if (isLegacyMomentName(stamp.name, &moment)) {
        if (moment == PrHtmlPerformanceMoment_NavigationStart) {
            *out = 0;
            return PrHtmlPerformanceError_None;
        }
        double end = prHtmlPerformanceTimingMoment(performance, moment);
        if (end == 0)
            return PrHtmlPerformanceError_InvalidAccess;
        *out = end - prHtmlPerformanceTimingMoment(performance, PrHtmlPerformanceMoment_NavigationStart);
        return PrHtmlPerformanceError_None;
    }
    if (performance->timeline != nullptr) {
        const HtmlTimeline* timeline = performance->timeline;
        const RawArray<PrHtmlPerformanceEntry*>& marks = timeline->buffers[PrHtmlPerformanceEntryType_Mark];
        for (uint32_t i = marks.count; i-- != 0;) {
            if (prRawStringView(&marks.data[i]->name) == stamp.name) {
                *out = marks.data[i]->startTime;
                return PrHtmlPerformanceError_None;
            }
        }
    }
    return PrHtmlPerformanceError_Syntax;
}

} // namespace

HtmlFetchAnchor htmlPerformanceFetchAnchor(const PrHtmlWindow* window) {
    return HtmlFetchAnchor{htmlWindowNow(window), htmlWindowClock(window), prTimeMonotonicMilliseconds()};
}

void htmlPerformanceMarkResource(PrHtmlWindow* window, const HtmlResourceReport* report) {
    if (!isHttpUrl(report->url))
        return;
    PrHtmlPerformance* performance = &window->performance;
    PrHtmlPerformanceEntry* entry = createEntry(performance, PrHtmlPerformanceEntryType_Resource, report->url);
    entry->initiator = report->initiator;
    double start = report->anchor.windowTime;
    if (report->timing.startTime != 0)
        start = fetchMoment(report->anchor, report->timing.startTime, htmlWindowNow(window));
    entry->startTime = start;
    FetchRevealFlags reveal = 0;
    if (report->timing.timingAllowPassed)
        reveal = FetchRevealFlag_Timing | FetchRevealFlag_Redirects;
    fillFromReport(entry, window, report, reveal);
    entry->duration = entry->moments[PrHtmlPerformanceMoment_ResponseEnd] - start;
    passToObservers(performance, entry);
    addResource(performance, entry);
    prHtmlPerformanceEntryRelease(entry);
}

void htmlPerformanceBeginNavigation(PrHtmlWindow* window, PrStringView url, const HtmlResourceReport* PR_NULLABLE report,
        PrHtmlNavigationType type) {
    PrHtmlPerformance* performance = &window->performance;
    HtmlTimeline* timeline = ensureTimeline(performance);
    PrHtmlPerformanceEntry* entry = createEntry(performance, PrHtmlPerformanceEntryType_Navigation, url);
    entry->initiator = PrFetchInitiator_Navigation;
    entry->navigationType = type;
    double now = htmlWindowNow(window);
    if (report != nullptr) {
        fillFromReport(entry, window, report, navigationReveal(report));
    } else {
        entry->moments[PrHtmlPerformanceMoment_FetchStart] = 0;
        entry->moments[PrHtmlPerformanceMoment_RequestStart] = now;
        entry->moments[PrHtmlPerformanceMoment_ResponseStart] = now;
        entry->moments[PrHtmlPerformanceMoment_FinalResponseHeadersStart] = now;
        entry->moments[PrHtmlPerformanceMoment_ResponseEnd] = now;
        entry->responseStatus = 200;
    }
    entry->moments[PrHtmlPerformanceMoment_DomLoading] = now;
    timeline->navigation = entry;
    addToBuffer(timeline, entry);
    prHtmlPerformanceEntryRelease(entry);
}

void htmlPerformanceEndNavigationFetch(PrHtmlWindow* window, const HtmlResourceReport* report) {
    HtmlTimeline* timeline = window->performance.timeline;
    if (timeline == nullptr || timeline->navigation == nullptr)
        return;
    fillFromReport(timeline->navigation, window, report, navigationReveal(report));
}

void htmlPerformanceLoadMoment(PrHtmlWindow* window, PrHtmlPerformanceMoment moment) {
    HtmlTimeline* timeline = window->performance.timeline;
    if (timeline == nullptr || timeline->navigation == nullptr)
        return;
    PrHtmlPerformanceEntry* entry = timeline->navigation;
    if (entry->moments[moment] != 0)
        return;
    double now = htmlWindowNow(window);
    entry->moments[moment] = now > kFirstMoment ? now : kFirstMoment;
    if (moment == PrHtmlPerformanceMoment_LoadEventEnd)
        entry->duration = entry->moments[moment];
}

void htmlPerformanceQueueNavigation(PrHtmlWindow* window) {
    HtmlTimeline* timeline = window->performance.timeline;
    if (timeline != nullptr && timeline->navigation != nullptr)
        passToObservers(&window->performance, timeline->navigation);
}

void htmlPerformanceReset(PrHtmlPerformance* performance) {
    HtmlTimeline* timeline = performance->timeline;
    if (timeline == nullptr)
        return;
    performance->timeline = nullptr;
    for (uint32_t i = 0; i < timeline->observers.count; ++i) {
        PrHtmlPerformanceObserver* observer = timeline->observers.data[i];
        observer->performance = nullptr;
        observer->types = 0;
        if (observer->callback.registered != nullptr)
            observer->callback.registered(observer->callback.context, observer, false);
        prHtmlPerformanceObserverRelease(observer);
    }
    timeline->observers.reset();
    for (RawArray<PrHtmlPerformanceEntry*>& buffer : timeline->buffers)
        releaseEntries(&buffer);
    releaseEntries(&timeline->secondaryResources);
    prMemoryFree(timeline);
}

} // namespace pr_impl

using namespace pr_impl;

PrStringView prHtmlPerformanceEntryTypeName(PrHtmlPerformanceEntryType type) {
    PR_ASSERT(type < PrHtmlPerformanceEntryType_Count);
    return kTypeNames[type];
}

PrHtmlPerformanceEntryType prHtmlPerformanceEntryTypeFromName(PrStringView name) {
    for (uint32_t i = 0; i < PrHtmlPerformanceEntryType_Count; ++i) {
        if (kTypeNames[i] == name)
            return PrHtmlPerformanceEntryType(i);
    }
    return PrHtmlPerformanceEntryType_Count;
}

PrStringView prHtmlPerformanceMomentName(PrHtmlPerformanceMoment moment) {
    PR_ASSERT(moment < PrHtmlPerformanceMoment_Count);
    return kMomentNames[moment];
}

void prHtmlPerformanceEntryRetain(PrHtmlPerformanceEntry* entry) {
    ++entry->references;
}

void prHtmlPerformanceEntryRelease(PrHtmlPerformanceEntry* PR_NULLABLE entry) {
    if (entry == nullptr || --entry->references != 0)
        return;
    prRawStringReset(&entry->name);
    prRawStringReset(&entry->detail);
    prRawStringReset(&entry->contentType);
    prRawStringReset(&entry->contentEncoding);
    resetServerTiming(entry);
    prMemoryFree(entry);
}

PrStringView prHtmlPerformanceEntryName(const PrHtmlPerformanceEntry* entry) {
    return prRawStringView(&entry->name);
}

PrHtmlPerformanceEntryType prHtmlPerformanceEntryType(const PrHtmlPerformanceEntry* entry) {
    return entry->type;
}

double prHtmlPerformanceEntryStartTime(const PrHtmlPerformanceEntry* entry) {
    return entry->startTime;
}

double prHtmlPerformanceEntryDuration(const PrHtmlPerformanceEntry* entry) {
    return entry->duration;
}

PrStringView prHtmlPerformanceEntryDetail(const PrHtmlPerformanceEntry* entry) {
    return prRawStringView(&entry->detail);
}

double prHtmlPerformanceEntryMoment(const PrHtmlPerformanceEntry* entry, PrHtmlPerformanceMoment moment) {
    PR_ASSERT(moment < PrHtmlPerformanceMoment_Count);
    return entry->moments[moment];
}

PrHtmlResourceInfo prHtmlPerformanceEntryResourceInfo(const PrHtmlPerformanceEntry* entry) {
    PrStringView contentType = prRawStringView(&entry->contentType);
    PrStringView contentEncoding = prRawStringView(&entry->contentEncoding);
    return PrHtmlResourceInfo{
        .initiator = entry->initiator,
        .cacheState = entry->cacheState,
        .responseStatus = entry->responseStatus,
        .transferSize = entry->transferSize,
        .encodedBodySize = entry->encodedBodySize,
        .decodedBodySize = entry->decodedBodySize,
        .contentType = contentType.data != nullptr ? contentType : PrStringView{""},
        .contentEncoding = contentEncoding.data != nullptr ? contentEncoding : PrStringView{""},
        .nextHopProtocol = entry->nextHopProtocol.data != nullptr ? entry->nextHopProtocol : PrStringView{""},
    };
}

uint32_t prHtmlPerformanceEntryServerTimingCount(const PrHtmlPerformanceEntry* entry) {
    return entry->serverTiming.count;
}

PrHtmlServerTiming prHtmlPerformanceEntryServerTiming(const PrHtmlPerformanceEntry* entry, uint32_t index) {
    PR_ASSERT(index < entry->serverTiming.count);
    const HtmlServerMetric& metric = entry->serverTiming.data[index];
    return PrHtmlServerTiming{prRawStringView(&metric.name), prRawStringView(&metric.description), metric.duration};
}

PrHtmlNavigationType prHtmlPerformanceEntryNavigationType(const PrHtmlPerformanceEntry* entry) {
    return entry->navigationType;
}

uint32_t prHtmlPerformanceEntryRedirectCount(const PrHtmlPerformanceEntry* entry) {
    return entry->redirectCount;
}

void prHtmlPerformanceEntryListRetain(PrHtmlPerformanceEntryList* list) {
    ++list->references;
}

void prHtmlPerformanceEntryListRelease(PrHtmlPerformanceEntryList* PR_NULLABLE list) {
    if (list == nullptr || --list->references != 0)
        return;
    releaseEntries(&list->entries);
    prMemoryFree(list);
}

uint32_t prHtmlPerformanceEntryListEntries(const PrHtmlPerformanceEntryList* list, PrHtmlPerformanceEntryType type,
        PrStringView name, PrHtmlPerformanceEntry* PR_NONNULL* PR_NULLABLE out, uint32_t capacity) {
    RawArray<PrHtmlPerformanceEntry*> matches;
    for (uint32_t i = 0; i < list->entries.count; ++i) {
        if (entryMatches(list->entries.data[i], type, name))
            matches.push(list->entries.data[i]);
    }
    return writeSorted(&matches, out, capacity);
}

uint32_t prHtmlPerformanceEntries(const PrHtmlPerformance* performance, PrHtmlPerformanceEntryType type, PrStringView name,
        PrHtmlPerformanceEntry* PR_NONNULL* PR_NULLABLE out, uint32_t capacity) {
    if (performance->timeline == nullptr)
        return 0;
    RawArray<PrHtmlPerformanceEntry*> matches;
    for (uint32_t kind = 0; kind < PrHtmlPerformanceEntryType_Count; ++kind) {
        if (type != PrHtmlPerformanceEntryType_Count && type != kind)
            continue;
        const RawArray<PrHtmlPerformanceEntry*>& buffer = performance->timeline->buffers[kind];
        for (uint32_t i = 0; i < buffer.count; ++i) {
            if (entryMatches(buffer.data[i], type, name))
                matches.push(buffer.data[i]);
        }
    }
    return writeSorted(&matches, out, capacity);
}

PrHtmlPerformanceError prHtmlPerformanceEntryCreateMark(PrHtmlPerformance* performance, PrStringView name,
        const double* PR_NULLABLE startTime, PrStringView detail, PrHtmlPerformanceEntry* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrHtmlPerformanceMoment moment = 0;
    if (isLegacyMomentName(name, &moment))
        return PrHtmlPerformanceError_Syntax;
    if (startTime != nullptr && *startTime < 0)
        return PrHtmlPerformanceError_Type;
    PrHtmlPerformanceEntry* entry = createEntry(performance, PrHtmlPerformanceEntryType_Mark, name);
    entry->startTime = startTime != nullptr ? *startTime : prHtmlPerformanceNow(performance);
    if (detail.data != nullptr)
        entry->detail = prRawStringCopy(detail);
    *out = entry;
    return PrHtmlPerformanceError_None;
}

void prHtmlPerformanceMarkQueue(PrHtmlPerformance* performance, PrHtmlPerformanceEntry* mark) {
    passToObservers(performance, mark);
    addToBuffer(ensureTimeline(performance), mark);
}

PrHtmlPerformanceError prHtmlPerformanceMeasure(PrHtmlPerformance* performance, const PrHtmlPerformanceMeasureDesc* desc,
        PrHtmlPerformanceEntry* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    bool hasStart = desc->start.kind != PrHtmlPerformanceTimestampKind_None;
    bool hasEnd = desc->end.kind != PrHtmlPerformanceTimestampKind_None;
    if (desc->hasOptions) {
        if (desc->endMark.data != nullptr || (!hasStart && !hasEnd) || (hasStart && hasEnd && desc->hasDuration))
            return PrHtmlPerformanceError_Type;
    }
    PrHtmlPerformanceTimestamp duration{.kind = PrHtmlPerformanceTimestampKind_Time, .time = desc->duration};
    double durationTime = 0;
    PrHtmlPerformanceError error = PrHtmlPerformanceError_None;
    if (desc->hasDuration && (hasStart || hasEnd))
        error = timestampOf(performance, duration, &durationTime);
    double endTime = 0;
    if (desc->endMark.data != nullptr) {
        PrHtmlPerformanceTimestamp mark{.kind = PrHtmlPerformanceTimestampKind_Name, .name = desc->endMark};
        error = error != PrHtmlPerformanceError_None ? error : timestampOf(performance, mark, &endTime);
    } else if (hasEnd) {
        error = error != PrHtmlPerformanceError_None ? error : timestampOf(performance, desc->end, &endTime);
    } else if (hasStart && desc->hasDuration) {
        double start = 0;
        error = error != PrHtmlPerformanceError_None ? error : timestampOf(performance, desc->start, &start);
        endTime = start + durationTime;
    } else {
        endTime = prHtmlPerformanceNow(performance);
    }
    double startTime = 0;
    if (hasStart)
        error = error != PrHtmlPerformanceError_None ? error : timestampOf(performance, desc->start, &startTime);
    else if (hasEnd && desc->hasDuration)
        startTime = endTime - durationTime;
    if (error != PrHtmlPerformanceError_None)
        return error;
    PrHtmlPerformanceEntry* entry = createEntry(performance, PrHtmlPerformanceEntryType_Measure, desc->name);
    entry->startTime = startTime;
    entry->duration = endTime - startTime;
    if (desc->detail.data != nullptr)
        entry->detail = prRawStringCopy(desc->detail);
    passToObservers(performance, entry);
    addToBuffer(ensureTimeline(performance), entry);
    *out = entry;
    return PrHtmlPerformanceError_None;
}

void prHtmlPerformanceClear(PrHtmlPerformance* performance, PrHtmlPerformanceEntryType type, PrStringView name) {
    PR_ASSERT(type == PrHtmlPerformanceEntryType_Mark || type == PrHtmlPerformanceEntryType_Measure);
    if (performance->timeline == nullptr)
        return;
    RawArray<PrHtmlPerformanceEntry*>& buffer = performance->timeline->buffers[type];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < buffer.count; ++i) {
        if (name.data == nullptr || prRawStringView(&buffer.data[i]->name) == name)
            prHtmlPerformanceEntryRelease(buffer.data[i]);
        else
            buffer.data[kept++] = buffer.data[i];
    }
    buffer.count = kept;
}

void prHtmlPerformanceClearResourceTimings(PrHtmlPerformance* performance) {
    if (performance->timeline != nullptr)
        releaseEntries(&performance->timeline->buffers[PrHtmlPerformanceEntryType_Resource]);
}

void prHtmlPerformanceSetResourceTimingBufferSize(PrHtmlPerformance* performance, uint32_t size) {
    ensureTimeline(performance)->resourceLimit = size;
}

double prHtmlPerformanceTimingMoment(const PrHtmlPerformance* performance, PrHtmlPerformanceMoment moment) {
    PR_ASSERT(moment < PrHtmlPerformanceMoment_LegacyCount);
    double origin = prHtmlPerformanceTimeOrigin(performance);
    if (moment == PrHtmlPerformanceMoment_NavigationStart)
        return prF64Floor(origin);
    const HtmlTimeline* timeline = performance->timeline;
    if (timeline == nullptr || timeline->navigation == nullptr)
        return 0;
    double value = timeline->navigation->moments[moment];
    bool happened = value != 0 || moment == PrHtmlPerformanceMoment_FetchStart ||
        (moment >= PrHtmlPerformanceMoment_DomainLookupStart && moment <= PrHtmlPerformanceMoment_ConnectEnd);
    return happened ? prF64Floor(origin + value) : 0;
}

PrHtmlNavigationType prHtmlPerformanceNavigationType(const PrHtmlPerformance* performance) {
    const HtmlTimeline* timeline = performance->timeline;
    if (timeline == nullptr || timeline->navigation == nullptr)
        return PrHtmlNavigationType_Navigate;
    return timeline->navigation->navigationType;
}

uint32_t prHtmlPerformanceRedirectCount(const PrHtmlPerformance* performance) {
    const HtmlTimeline* timeline = performance->timeline;
    return timeline != nullptr && timeline->navigation != nullptr ? timeline->navigation->redirectCount : 0;
}

PrHtmlPerformanceObserver* prHtmlPerformanceObserverCreate(PrHtmlPerformance* performance,
        const PrHtmlPerformanceObserverCallback* callback) {
    PrHtmlPerformanceObserver* observer = allocZeroed<PrHtmlPerformanceObserver>();
    observer->references = 1;
    observer->performance = performance;
    observer->callback = *callback;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    return observer;
}

void prHtmlPerformanceObserverRetain(PrHtmlPerformanceObserver* observer) {
    ++observer->references;
}

void prHtmlPerformanceObserverRelease(PrHtmlPerformanceObserver* PR_NULLABLE observer) {
    if (observer == nullptr || --observer->references != 0)
        return;
    releaseEntries(&observer->records);
    if (observer->callback.release != nullptr)
        observer->callback.release(observer->callback.context);
    prMemoryFree(observer);
}

bool prHtmlPerformanceObserverObserve(PrHtmlPerformanceObserver* observer, const PrHtmlPerformanceObserverInit* init) {
    if (observer->mode != PrHtmlPerformanceObserverMode_None && observer->mode != init->mode)
        return false;
    observer->mode = init->mode;
    observer->requiresDropped = true;
    if (observer->performance == nullptr || init->types == 0)
        return true;
    HtmlTimeline* timeline = ensureTimeline(observer->performance);
    if (observer->types == 0) {
        prHtmlPerformanceObserverRetain(observer);
        timeline->observers.push(observer);
        if (observer->callback.registered != nullptr)
            observer->callback.registered(observer->callback.context, observer, true);
    }
    observer->types =
        init->mode == PrHtmlPerformanceObserverMode_Multiple ? init->types : observer->types | init->types;
    if (!init->buffered)
        return true;
    for (uint32_t kind = 0; kind < PrHtmlPerformanceEntryType_Count; ++kind) {
        if ((init->types & (1u << kind)) == 0)
            continue;
        const RawArray<PrHtmlPerformanceEntry*>& buffer = timeline->buffers[kind];
        for (uint32_t i = 0; i < buffer.count; ++i) {
            prHtmlPerformanceEntryRetain(buffer.data[i]);
            observer->records.push(buffer.data[i]);
        }
    }
    queueObserverTask(observer->performance);
    return true;
}

void prHtmlPerformanceObserverDisconnect(PrHtmlPerformanceObserver* observer) {
    releaseEntries(&observer->records);
    if (observer->types == 0 || observer->performance == nullptr)
        return;
    observer->types = 0;
    RawArray<PrHtmlPerformanceObserver*>& observers = observer->performance->timeline->observers;
    for (uint32_t i = 0; i < observers.count; ++i) {
        if (observers.data[i] == observer) {
            observers.erase(i);
            break;
        }
    }
    if (observer->callback.registered != nullptr)
        observer->callback.registered(observer->callback.context, observer, false);
    prHtmlPerformanceObserverRelease(observer);
}

PrHtmlPerformanceEntryList* prHtmlPerformanceObserverTakeRecords(PrHtmlPerformanceObserver* observer) {
    PrHtmlPerformanceEntryList* list = allocZeroed<PrHtmlPerformanceEntryList>();
    list->references = 1;
    list->entries = observer->records;
    observer->records = RawArray<PrHtmlPerformanceEntry*>{};
    return list;
}
