// JavaScript binding of the performance timeline.

#include "html/quickjs/src/html_performance_quickjs.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/dom_event_handler.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/html_performance.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"
#include "html/quickjs/src/html_clone_quickjs.h"

namespace pr_impl {

namespace {

JSClassID performanceServerTimingClass;
JSClassID performanceEntryClass;
JSClassID performanceMarkClass;
JSClassID performanceMeasureClass;
JSClassID performanceResourceTimingClass;
JSClassID performanceNavigationTimingClass;
JSClassID performanceObserverClass;
JSClassID performanceEntryListClass;
JSClassID performanceTimingClass;
JSClassID performanceNavigationClass;

JSValue throwError(JSContext* ctx, PrHtmlPerformanceError error) {
    switch (error) {
        case PrHtmlPerformanceError_Syntax:
            return jsDomExceptionThrow(ctx, "SyntaxError");
        case PrHtmlPerformanceError_InvalidAccess:
            return jsDomExceptionThrow(ctx, "InvalidAccessError");
        default:
            return JS_ThrowTypeError(ctx, "Invalid performance timestamp");
    }
}

constexpr JsReferences kEntryReferences = {
    [](void* handle) { prHtmlPerformanceEntryRetain(static_cast<PrHtmlPerformanceEntry*>(handle)); },
    [](void* handle) { prHtmlPerformanceEntryRelease(static_cast<PrHtmlPerformanceEntry*>(handle)); },
    false,
};

constexpr JsReferences kObserverReferences = {
    [](void* handle) { prHtmlPerformanceObserverRetain(static_cast<PrHtmlPerformanceObserver*>(handle)); },
    [](void* handle) { prHtmlPerformanceObserverRelease(static_cast<PrHtmlPerformanceObserver*>(handle)); },
    true,
};

constexpr JsReferences kEntryListReferences = {
    [](void* handle) { prHtmlPerformanceEntryListRetain(static_cast<PrHtmlPerformanceEntryList*>(handle)); },
    [](void* handle) { prHtmlPerformanceEntryListRelease(static_cast<PrHtmlPerformanceEntryList*>(handle)); },
    false,
};

// Returns the object of an entry, which is the same object for as long as script holds it, and which takes its own
// reference.
JSValue entryToJs(JSContext* ctx, PrHtmlPerformanceEntry* entry) {
    JSClassID id = performanceEntryClass;
    switch (prHtmlPerformanceEntryType(entry)) {
        case PrHtmlPerformanceEntryType_Mark:
            id = performanceMarkClass;
            break;
        case PrHtmlPerformanceEntryType_Measure:
            id = performanceMeasureClass;
            break;
        case PrHtmlPerformanceEntryType_Navigation:
            id = performanceNavigationTimingClass;
            break;
        case PrHtmlPerformanceEntryType_Resource:
            id = performanceResourceTimingClass;
            break;
    }
    return jsWrap(ctx, id, JS_UNDEFINED, entry, false);
}

JSValue entriesToJs(JSContext* ctx, PrHtmlPerformanceEntry* PR_NONNULL const* PR_NULLABLE entries, uint32_t count) {
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i)
        JS_SetPropertyUint32(ctx, array, i, entryToJs(ctx, entries[i]));
    return array;
}

// ---------------------------------------------------------------------------------------------------------------------
// PerformanceEntry and the interfaces that derive from it

// The attributes of PerformanceEntry, selected by the magic.
enum {
    EntryAttribute_Name,
    EntryAttribute_EntryType,
    EntryAttribute_StartTime,
    EntryAttribute_Duration,
    EntryAttribute_Count,
};
typedef uint8_t EntryAttribute;

JSValue entryAttribute(JSContext* ctx, JSValueConst entryObject, int magic) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case EntryAttribute_Name:
            return jsNewStringView(ctx, prHtmlPerformanceEntryName(entry));
        case EntryAttribute_EntryType:
            return jsNewStringView(ctx, prHtmlPerformanceEntryTypeName(prHtmlPerformanceEntryType(entry)));
        case EntryAttribute_StartTime:
            return JS_NewFloat64(ctx, prHtmlPerformanceEntryStartTime(entry));
        default:
            return JS_NewFloat64(ctx, prHtmlPerformanceEntryDuration(entry));
    }
}

// The detail of a mark or a measure, deserialized once for each object (user-timing/#dom-performancemark-detail).
JSValue entryDetail(JSContext* ctx, JSValueConst entryObject) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    PrStringView detail = prHtmlPerformanceEntryDetail(entry);
    if (detail.data == nullptr)
        return JS_NULL;
    return jsSameObject(ctx, entryObject, "detail", [&] { return jsHtmlDeserialize(ctx, detail, JS_UNDEFINED); });
}

JSValue entryMoment(JSContext* ctx, JSValueConst entryObject, int magic) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, prHtmlPerformanceEntryMoment(entry, PrHtmlPerformanceMoment(magic)));
}

// The attributes of PerformanceResourceTiming that are not moments, selected by the magic.
enum {
    ResourceAttribute_InitiatorType,
    ResourceAttribute_DeliveryType,
    ResourceAttribute_NextHopProtocol,
    ResourceAttribute_TransferSize,
    ResourceAttribute_EncodedBodySize,
    ResourceAttribute_DecodedBodySize,
    ResourceAttribute_ResponseStatus,
    ResourceAttribute_ContentType,
    ResourceAttribute_ContentEncoding,
    ResourceAttribute_Count,
};
typedef uint8_t ResourceAttribute;

constexpr const char* kResourceAttributeNames[] = {
    "initiatorType", "deliveryType", "nextHopProtocol", "transferSize", "encodedBodySize", "decodedBodySize",
    "responseStatus", "contentType", "contentEncoding",
};

JSValue entryResourceAttribute(JSContext* ctx, JSValueConst entryObject, int magic) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    PrHtmlResourceInfo info = prHtmlPerformanceEntryResourceInfo(entry);
    switch (magic) {
        case ResourceAttribute_InitiatorType:
            return jsNewStringView(ctx, prFetchInitiatorName(info.initiator));
        case ResourceAttribute_DeliveryType:
            return jsNewStringView(ctx, info.cacheState != PrFetchCacheState_None ? "cache" : "");
        case ResourceAttribute_NextHopProtocol:
            return jsNewStringView(ctx, info.nextHopProtocol);
        case ResourceAttribute_TransferSize:
            return JS_NewFloat64(ctx, double(info.transferSize));
        case ResourceAttribute_EncodedBodySize:
            return JS_NewFloat64(ctx, double(info.encodedBodySize));
        case ResourceAttribute_DecodedBodySize:
            return JS_NewFloat64(ctx, double(info.decodedBodySize));
        case ResourceAttribute_ResponseStatus:
            return JS_NewInt32(ctx, info.responseStatus);
        case ResourceAttribute_ContentType:
            return jsNewStringView(ctx, info.contentType);
        default:
            return jsNewStringView(ctx, info.contentEncoding);
    }
}

// A metric's object, which holds a copy of the metric, since its entry may go first.
struct JsServerTiming {
    PrString name;
    PrString description;
    double duration;
};

void freeServerTiming(JsServerTiming* PR_NULLABLE metric) {
    if (metric == nullptr)
        return;
    prStringReset(&metric->name);
    prStringReset(&metric->description);
    prMemoryFree(metric);
}

JSValue serverTimingName(JSContext* ctx, JSValueConst metricObject) {
    JsServerTiming* metric = jsThis<JsServerTiming>(ctx, metricObject, performanceServerTimingClass);
    return metric != nullptr ? jsNewStringView(ctx, prStringView(&metric->name)) : JS_EXCEPTION;
}

JSValue serverTimingDescription(JSContext* ctx, JSValueConst metricObject) {
    JsServerTiming* metric = jsThis<JsServerTiming>(ctx, metricObject, performanceServerTimingClass);
    return metric != nullptr ? jsNewStringView(ctx, prStringView(&metric->description)) : JS_EXCEPTION;
}

JSValue serverTimingDuration(JSContext* ctx, JSValueConst metricObject) {
    JsServerTiming* metric = jsThis<JsServerTiming>(ctx, metricObject, performanceServerTimingClass);
    return metric != nullptr ? JS_NewFloat64(ctx, metric->duration) : JS_EXCEPTION;
}

JSValue serverTimingToJson(JSContext* ctx, JSValueConst timingObject, int, JSValueConst*) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "name", serverTimingName(ctx, timingObject));
    JS_SetPropertyStr(ctx, object, "duration", serverTimingDuration(ctx, timingObject));
    JS_SetPropertyStr(ctx, object, "description", serverTimingDescription(ctx, timingObject));
    return object;
}

// serverTiming, one frozen array for each object (server-timing/#the-servertiming-attribute).
JSValue entryServerTiming(JSContext* ctx, JSValueConst entryObject) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, entryObject, "serverTiming", [&] {
        JSValue array = JS_NewArray(ctx);
        for (uint32_t i = 0; i < prHtmlPerformanceEntryServerTimingCount(entry); ++i) {
            PrHtmlServerTiming metric = prHtmlPerformanceEntryServerTiming(entry, i);
            JsServerTiming* copy = allocZeroed<JsServerTiming>();
            copy->name = prStringCopy(metric.name);
            copy->description = prStringCopy(metric.description);
            copy->duration = metric.duration;
            JS_SetPropertyUint32(ctx, array, i, jsWrap(ctx, performanceServerTimingClass, JS_UNDEFINED, copy, true));
        }
        JS_FreezeObject(ctx, array);
        return array;
    });
}

JSValue navigationType(JSContext* ctx, JSValueConst entryObject) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    switch (prHtmlPerformanceEntryNavigationType(entry)) {
        case PrHtmlNavigationType_Reload:
            return jsNewStringView(ctx, "reload");
        case PrHtmlNavigationType_BackForward:
            return jsNewStringView(ctx, "back_forward");
        default:
            return jsNewStringView(ctx, "navigate");
    }
}

JSValue navigationRedirectCount(JSContext* ctx, JSValueConst entryObject) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    return entry != nullptr ? JS_NewInt32(ctx, int32_t(prHtmlPerformanceEntryRedirectCount(entry))) : JS_EXCEPTION;
}

// The moments that PerformanceResourceTiming and PerformanceNavigationTiming add as attributes.
constexpr PrHtmlPerformanceMoment kFetchMoments[] = {
    PrHtmlPerformanceMoment_WorkerStart, PrHtmlPerformanceMoment_RedirectStart, PrHtmlPerformanceMoment_RedirectEnd,
    PrHtmlPerformanceMoment_FetchStart, PrHtmlPerformanceMoment_DomainLookupStart,
    PrHtmlPerformanceMoment_DomainLookupEnd, PrHtmlPerformanceMoment_ConnectStart, PrHtmlPerformanceMoment_ConnectEnd,
    PrHtmlPerformanceMoment_SecureConnectionStart, PrHtmlPerformanceMoment_RequestStart,
    PrHtmlPerformanceMoment_FinalResponseHeadersStart, PrHtmlPerformanceMoment_FirstInterimResponseStart,
    PrHtmlPerformanceMoment_ResponseStart, PrHtmlPerformanceMoment_ResponseEnd,
};

constexpr PrHtmlPerformanceMoment kLoadMoments[] = {
    PrHtmlPerformanceMoment_UnloadEventStart, PrHtmlPerformanceMoment_UnloadEventEnd,
    PrHtmlPerformanceMoment_DomInteractive, PrHtmlPerformanceMoment_DomContentLoadedEventStart,
    PrHtmlPerformanceMoment_DomContentLoadedEventEnd, PrHtmlPerformanceMoment_DomComplete,
    PrHtmlPerformanceMoment_LoadEventStart, PrHtmlPerformanceMoment_LoadEventEnd,
};

// toJSON, which runs webidl/#default-tojson-steps: an object of the attributes of each interface of the entry.
JSValue entryToJson(JSContext* ctx, JSValueConst entryObject, int, JSValueConst*) {
    PrHtmlPerformanceEntry* entry = jsThis<PrHtmlPerformanceEntry>(ctx, entryObject, performanceEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    JSValue object = JS_NewObject(ctx);
    constexpr const char* kEntryNames[] = {"name", "entryType", "startTime", "duration"};
    for (int i = 0; i < EntryAttribute_Count; ++i)
        JS_SetPropertyStr(ctx, object, kEntryNames[i], entryAttribute(ctx, entryObject, i));
    PrHtmlPerformanceEntryType type = prHtmlPerformanceEntryType(entry);
    bool navigation = type == PrHtmlPerformanceEntryType_Navigation;
    if (type == PrHtmlPerformanceEntryType_Mark || type == PrHtmlPerformanceEntryType_Measure)
        JS_SetPropertyStr(ctx, object, "detail", entryDetail(ctx, entryObject));
    if (!navigation && type != PrHtmlPerformanceEntryType_Resource)
        return object;
    for (int i = 0; i < ResourceAttribute_Count; ++i)
        JS_SetPropertyStr(ctx, object, kResourceAttributeNames[i], entryResourceAttribute(ctx, entryObject, i));
    JS_SetPropertyStr(ctx, object, "serverTiming", entryServerTiming(ctx, entryObject));
    uint32_t fetchCount = uint32_t(countOf(kFetchMoments));
    uint32_t count = fetchCount + (navigation ? uint32_t(countOf(kLoadMoments)) : 0);
    for (uint32_t i = 0; i < count; ++i) {
        PrHtmlPerformanceMoment moment = i < fetchCount ? kFetchMoments[i] : kLoadMoments[i - fetchCount];
        PrStringView text = prHtmlPerformanceMomentName(moment);
        JSAtom name = JS_NewAtomLen(ctx, text.data, text.size);
        JS_SetProperty(ctx, object, name, entryMoment(ctx, entryObject, moment));
        JS_FreeAtom(ctx, name);
    }
    if (navigation) {
        JS_SetPropertyStr(ctx, object, "type", navigationType(ctx, entryObject));
        JS_SetPropertyStr(ctx, object, "redirectCount", navigationRedirectCount(ctx, entryObject));
    }
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// Performance's members

// Reads the `type` argument at `index` of a call that filters by type. Returns false with `*empty` set for a type that
// the timeline does not know, which has no entries, and false without it for an exception.
bool readTypeFilter(StringArgument* name, JSValueConst value, PrHtmlPerformanceEntryType* out, bool* empty) {
    if (!name->read(value))
        return false;
    *out = prHtmlPerformanceEntryTypeFromName(name->value());
    *empty = *out == PrHtmlPerformanceEntryType_Count;
    return !*empty;
}

// The methods that read a timeline or a list by name and type, selected by the magic.
enum {
    EntriesCall_All,
    EntriesCall_ByType,
    EntriesCall_ByName,
};
typedef uint8_t EntriesCall;

// Reads the filter of a call. Returns false if the call is complete, with `*result` set to its value.
bool readFilter(JSContext* ctx, int argc, JSValueConst* argv, int magic, StringArgument* nameArgument,
        StringArgument* typeArgument, PrHtmlPerformanceEntryType* type, PrStringView* name, JSValue* result) {
    *type = PrHtmlPerformanceEntryType_Count;
    *name = PrStringView{nullptr, 0};
    bool empty = false;
    if (magic == EntriesCall_ByType) {
        if (!jsArgCount(ctx, argc, 1) || !readTypeFilter(typeArgument, argv[0], type, &empty)) {
            *result = empty ? JS_NewArray(ctx) : JS_EXCEPTION;
            return false;
        }
    } else if (magic == EntriesCall_ByName) {
        if (!jsArgCount(ctx, argc, 1) || !nameArgument->read(argv[0])) {
            *result = JS_EXCEPTION;
            return false;
        }
        *name = nameArgument->value();
        if (jsArgGiven(argc, argv, 1) && !readTypeFilter(typeArgument, argv[1], type, &empty)) {
            *result = empty ? JS_NewArray(ctx) : JS_EXCEPTION;
            return false;
        }
    }
    return true;
}

JSValue timelineEntries(JSContext* ctx, JSValueConst performanceObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    StringArgument nameArgument = {ctx};
    StringArgument typeArgument = {ctx};
    PrHtmlPerformanceEntryType type = 0;
    PrStringView name{};
    JSValue result = JS_UNDEFINED;
    if (!readFilter(ctx, argc, argv, magic, &nameArgument, &typeArgument, &type, &name, &result))
        return result;
    uint32_t total = prHtmlPerformanceEntries(performance, type, name, nullptr, 0);
    Array<PrHtmlPerformanceEntry*> entries;
    entries.resize(total);
    prHtmlPerformanceEntries(performance, type, name, entries.data, total);
    result = entriesToJs(ctx, entries.data, total);
    entries.reset();
    return result;
}

// Reads the detail member of a dictionary, serialized into `*out` if it is present and not null. Returns false with an
// exception thrown.
bool readDetail(JSContext* ctx, JSValueConst options, JsHtmlSerialized* out, bool* given) {
    JSValue detail = JS_GetPropertyStr(ctx, options, "detail");
    if (JS_IsException(detail))
        return false;
    *given = !JS_IsUndefined(detail);
    bool ok = JS_IsUndefined(detail) || JS_IsNull(detail) || jsHtmlSerialize(ctx, detail, nullptr, nullptr, out);
    JS_FreeValue(ctx, detail);
    return ok;
}

// Creates a mark from the arguments of PerformanceMark's constructor (user-timing/#dom-performance-mark), and returns
// null with an exception thrown if it cannot.
PrHtmlPerformanceEntry* PR_NULLABLE createMark(JSContext* ctx, PrHtmlPerformance* performance, int argc, JSValueConst* argv) {
    StringArgument name = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return nullptr;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, options, &present))
        return nullptr;
    double startTime = 0;
    bool hasStart = false;
    JsHtmlSerialized detail{};
    if (present) {
        bool given = false;
        if (!readDetail(ctx, options, &detail, &given))
            return nullptr;
        JSValue start = JS_GetPropertyStr(ctx, options, "startTime");
        hasStart = !JS_IsUndefined(start);
        bool ok = !JS_IsException(start) && (!hasStart || JS_ToFloat64(ctx, &startTime, start) >= 0);
        JS_FreeValue(ctx, start);
        if (ok && hasStart && !prF64IsFinite(startTime)) {
            ok = false;
            JS_ThrowTypeError(ctx, "startTime is not finite");
        }
        if (!ok) {
            jsHtmlSerializedReset(&detail);
            return nullptr;
        }
    }
    PrHtmlPerformanceEntry* mark = nullptr;
    PrHtmlPerformanceError error = prHtmlPerformanceEntryCreateMark(performance, name.value(), hasStart ? &startTime : nullptr,
        prStringView(&detail.bytes), &mark);
    jsHtmlSerializedReset(&detail);
    if (error != PrHtmlPerformanceError_None)
        throwError(ctx, error);
    return mark;
}

// Returns the object of an entry that the caller holds a reference to, and releases the reference.
JSValue takeEntry(JSContext* ctx, PrHtmlPerformanceEntry* entry) {
    JSValue object = entryToJs(ctx, entry);
    prHtmlPerformanceEntryRelease(entry);
    return object;
}

JSValue mark(JSContext* ctx, JSValueConst performanceObject, int argc, JSValueConst* argv) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    PrHtmlPerformanceEntry* entry = createMark(ctx, performance, argc, argv);
    if (entry == nullptr)
        return JS_EXCEPTION;
    prHtmlPerformanceMarkQueue(performance, entry);
    return takeEntry(ctx, entry);
}

JSValue markConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "PerformanceMark needs a window");
    PrHtmlPerformanceEntry* entry = createMark(ctx, prHtmlWindowPerformance(window), argc, argv);
    if (entry == nullptr)
        return JS_EXCEPTION;
    return jsConstructed(ctx, newTarget, takeEntry(ctx, entry));
}

// Reads a member of a measure's options that is (DOMString or DOMHighResTimeStamp). `*text` holds the string of a name.
// Returns false with an exception thrown.
bool readTimestamp(JSContext* ctx, JSValueConst options, const char* member, StringArgument* text,
        PrHtmlPerformanceTimestamp* out) {
    JSValue value = JS_GetPropertyStr(ctx, options, member);
    bool ok = !JS_IsException(value);
    if (ok && JS_IsNumber(value)) {
        out->kind = PrHtmlPerformanceTimestampKind_Time;
        JS_ToFloat64(ctx, &out->time, value);
    } else if (ok && !JS_IsUndefined(value)) {
        out->kind = PrHtmlPerformanceTimestampKind_Name;
        ok = text->read(value);
        out->name = text->value();
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// measure(measureName, startOrMeasureOptions, endMark) (user-timing/#measure-method).
JSValue measure(JSContext* ctx, JSValueConst performanceObject, int argc, JSValueConst* argv) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    StringArgument name = {ctx};
    StringArgument startText = {ctx};
    StringArgument endText = {ctx};
    StringArgument endMark = {ctx};
    if (performance == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlPerformanceMeasureDesc desc{.name = name.value()};
    JsHtmlSerialized detail{};
    JSValueConst first = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (JS_IsObject(first)) {
        bool hasDetail = false;
        JSValue duration = JS_GetPropertyStr(ctx, first, "duration");
        bool ok = !JS_IsException(duration) && readTimestamp(ctx, first, "start", &startText, &desc.start) &&
            readTimestamp(ctx, first, "end", &endText, &desc.end);
        desc.hasDuration = ok && !JS_IsUndefined(duration);
        ok = ok && (!desc.hasDuration || JS_ToFloat64(ctx, &desc.duration, duration) >= 0);
        JS_FreeValue(ctx, duration);
        ok = ok && readDetail(ctx, first, &detail, &hasDetail);
        if (!ok) {
            jsHtmlSerializedReset(&detail);
            return JS_EXCEPTION;
        }
        desc.hasOptions = hasDetail || desc.hasDuration || desc.start.kind != PrHtmlPerformanceTimestampKind_None ||
            desc.end.kind != PrHtmlPerformanceTimestampKind_None;
        desc.detail = prStringView(&detail.bytes);
    } else if (!JS_IsUndefined(first)) {
        if (!startText.read(first))
            return JS_EXCEPTION;
        desc.start.kind = PrHtmlPerformanceTimestampKind_Name;
        desc.start.name = startText.value();
    }
    if (jsArgGiven(argc, argv, 2)) {
        if (!endMark.read(argv[2])) {
            jsHtmlSerializedReset(&detail);
            return JS_EXCEPTION;
        }
        desc.endMark = endMark.value();
    }
    PrHtmlPerformanceEntry* entry = nullptr;
    PrHtmlPerformanceError error = prHtmlPerformanceMeasure(performance, &desc, &entry);
    jsHtmlSerializedReset(&detail);
    return error == PrHtmlPerformanceError_None ? takeEntry(ctx, entry) : throwError(ctx, error);
}

// clearMarks and clearMeasures, with the type of entry as the magic.
JSValue clearEntries(JSContext* ctx, JSValueConst performanceObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    StringArgument name = {ctx};
    bool given = jsArgGiven(argc, argv, 0);
    if (performance == nullptr || (given && !name.read(argv[0])))
        return JS_EXCEPTION;
    prHtmlPerformanceClear(performance, PrHtmlPerformanceEntryType(magic), given ? name.value() : PrStringView{});
    return JS_UNDEFINED;
}

JSValue clearResourceTimings(JSContext* ctx, JSValueConst performanceObject, int, JSValueConst*) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    prHtmlPerformanceClearResourceTimings(performance);
    return JS_UNDEFINED;
}

JSValue setResourceTimingBufferSize(JSContext* ctx, JSValueConst performanceObject, int argc, JSValueConst* argv) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    uint32_t size = 0;
    if (performance == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &size, argv[0]) < 0)
        return JS_EXCEPTION;
    prHtmlPerformanceSetResourceTimingBufferSize(performance, size);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// PerformanceTiming and PerformanceNavigation, the obsolete views of the window's navigation, whose objects hold their
// Performance's handle.

JSValue timingObject(JSContext* ctx, JSValueConst performanceObject) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, performanceObject, "timing",
        [&] { return jsWrapAs(ctx, performanceTimingClass, performanceObject, performance, false); });
}

JSValue navigationObject(JSContext* ctx, JSValueConst performanceObject) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, performanceObject, "navigation", [&] {
        return jsWrapAs(ctx, performanceNavigationClass, performanceObject, performance, false);
    });
}

JSValue timingMoment(JSContext* ctx, JSValueConst performanceObject, int magic) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, performanceTimingClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, prHtmlPerformanceTimingMoment(performance, PrHtmlPerformanceMoment(magic)));
}

JSValue timingToJson(JSContext* ctx, JSValueConst performanceObject, int, JSValueConst*) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, performanceTimingClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    JSValue object = JS_NewObject(ctx);
    for (uint32_t i = 0; i < PrHtmlPerformanceMoment_LegacyCount; ++i) {
        PrStringView text = prHtmlPerformanceMomentName(PrHtmlPerformanceMoment(i));
        JSAtom name = JS_NewAtomLen(ctx, text.data, text.size);
        JS_SetProperty(ctx, object, name, timingMoment(ctx, performanceObject, int(i)));
        JS_FreeAtom(ctx, name);
    }
    return object;
}

JSValue legacyNavigationType(JSContext* ctx, JSValueConst performanceObject) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, performanceNavigationClass);
    return performance != nullptr ? JS_NewInt32(ctx, prHtmlPerformanceNavigationType(performance)) : JS_EXCEPTION;
}

JSValue legacyRedirectCount(JSContext* ctx, JSValueConst performanceObject) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, performanceNavigationClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, int32_t(prHtmlPerformanceRedirectCount(performance)));
}

JSValue legacyNavigationToJson(JSContext* ctx, JSValueConst timingObject, int, JSValueConst*) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "type", legacyNavigationType(ctx, timingObject));
    JS_SetPropertyStr(ctx, object, "redirectCount", legacyRedirectCount(ctx, timingObject));
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// PerformanceObserver and PerformanceObserverEntryList

JSValue listEntries(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlPerformanceEntryList* list = jsThis<PrHtmlPerformanceEntryList>(ctx, listObject, performanceEntryListClass);
    if (list == nullptr)
        return JS_EXCEPTION;
    StringArgument nameArgument = {ctx};
    StringArgument typeArgument = {ctx};
    PrHtmlPerformanceEntryType type = 0;
    PrStringView name{};
    JSValue result = JS_UNDEFINED;
    if (!readFilter(ctx, argc, argv, magic, &nameArgument, &typeArgument, &type, &name, &result))
        return result;
    uint32_t total = prHtmlPerformanceEntryListEntries(list, type, name, nullptr, 0);
    Array<PrHtmlPerformanceEntry*> entries;
    entries.resize(total);
    prHtmlPerformanceEntryListEntries(list, type, name, entries.data, total);
    result = entriesToJs(ctx, entries.data, total);
    entries.reset();
    return result;
}

void invokeObserver(void* PR_NULLABLE context, PrHtmlPerformanceObserver* observer, PrHtmlPerformanceEntryList* entries,
        const uint64_t* PR_NULLABLE droppedEntries) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue object = jsWrap(ctx, performanceObserverClass, JS_UNDEFINED, observer, false);
    JSValue list = jsWrap(ctx, performanceEntryListClass, JS_UNDEFINED, entries, false);
    JSValue options = JS_NewObject(ctx);
    if (droppedEntries != nullptr)
        JS_SetPropertyStr(ctx, options, "droppedEntriesCount", JS_NewFloat64(ctx, double(*droppedEntries)));
    JSValue args[] = {list, object, options};
    JSValue result = jsCallbackCall(callback, nullptr, object, 3, args);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, options);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, object);
}

// A registered observer lasts until it stops observing, whether script can reach it or not. A host may destroy the
// engine before its pages, which detaches the callback, and then no object remains to pin.
void observerRegistered(void* PR_NULLABLE context, PrHtmlPerformanceObserver* observer, bool registered) {
    JSRuntime* runtime = static_cast<JsCallback*>(context)->runtime;
    if (runtime != nullptr)
        jsPin(runtime, performanceObserverClass, observer, registered);
}

JSValue observerConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "PerformanceObserver needs a window");
    CallbackArgument<PrHtmlPerformanceObserverCallback> callback = {ctx, JS_UNDEFINED};
    if (!jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeObserver))
        return JS_EXCEPTION;
    PrHtmlPerformanceObserverCallback value = *callback.get();
    value.registered = observerRegistered;
    PrHtmlPerformanceObserver* observer = prHtmlPerformanceObserverCreate(prHtmlWindowPerformance(window), &value);
    JSValue object = jsWrap(ctx, performanceObserverClass, JS_UNDEFINED, observer, true);
    if (JS_IsException(object)) {
        prHtmlPerformanceObserverRelease(observer);
        return object;
    }
    jsCallbackOwn(callback.callback, object);
    return jsConstructed(ctx, newTarget, object);
}

// Adds the type that `value` names to `*types` if the timeline supports it. Returns false with an exception thrown.
bool addType(JSContext* ctx, JSValueConst value, uint32_t* types) {
    StringArgument name = {ctx};
    if (!name.read(value))
        return false;
    PrHtmlPerformanceEntryType type = prHtmlPerformanceEntryTypeFromName(name.value());
    if (type != PrHtmlPerformanceEntryType_Count)
        *types |= 1u << type;
    return true;
}

// observe(options) (performance-timeline/#observe-method).
JSValue observerObserve(JSContext* ctx, JSValueConst observerObject, int argc, JSValueConst* argv) {
    PrHtmlPerformanceObserver* observer =
        jsThis<PrHtmlPerformanceObserver>(ctx, observerObject, performanceObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, options, &present))
        return JS_EXCEPTION;
    if (!present)
        return JS_ThrowTypeError(ctx, "observe needs entryTypes or type");
    JSValue buffered = JS_GetPropertyStr(ctx, options, "buffered");
    JSValue entryTypes = JS_GetPropertyStr(ctx, options, "entryTypes");
    JSValue type = JS_GetPropertyStr(ctx, options, "type");
    PrHtmlPerformanceObserverInit init{.buffered = !JS_IsUndefined(buffered) && JS_ToBool(ctx, buffered) > 0};
    bool hasEntryTypes = !JS_IsUndefined(entryTypes);
    bool hasType = !JS_IsUndefined(type);
    JSValue result = JS_UNDEFINED;
    if (JS_IsException(buffered) || JS_IsException(entryTypes) || JS_IsException(type)) {
        result = JS_EXCEPTION;
    } else if (!hasEntryTypes && !hasType) {
        result = JS_ThrowTypeError(ctx, "observe needs entryTypes or type");
    } else if (hasEntryTypes && hasType) {
        result = JS_ThrowTypeError(ctx, "entryTypes takes no other member");
    } else {
        bool read = true;
        if (hasEntryTypes) {
            init.mode = PrHtmlPerformanceObserverMode_Multiple;
            init.buffered = false;
            read = jsSequence(ctx, entryTypes, [&](JSValue item) {
                bool ok = addType(ctx, item, &init.types);
                JS_FreeValue(ctx, item);
                return ok;
            });
        } else {
            init.mode = PrHtmlPerformanceObserverMode_Single;
            read = addType(ctx, type, &init.types);
        }
        if (!read)
            result = JS_EXCEPTION;
        else if (!prHtmlPerformanceObserverObserve(observer, &init))
            result = jsDomExceptionThrow(ctx, "InvalidModificationError");
    }
    JS_FreeValue(ctx, buffered);
    JS_FreeValue(ctx, entryTypes);
    JS_FreeValue(ctx, type);
    return result;
}

JSValue observerDisconnect(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrHtmlPerformanceObserver* observer =
        jsThis<PrHtmlPerformanceObserver>(ctx, observerObject, performanceObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    prHtmlPerformanceObserverDisconnect(observer);
    return JS_UNDEFINED;
}

JSValue observerTakeRecords(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrHtmlPerformanceObserver* observer =
        jsThis<PrHtmlPerformanceObserver>(ctx, observerObject, performanceObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    PrHtmlPerformanceEntryList* list = prHtmlPerformanceObserverTakeRecords(observer);
    uint32_t total = prHtmlPerformanceEntryListEntries(list, PrHtmlPerformanceEntryType_Count, PrStringView{nullptr, 0},
        nullptr, 0);
    Array<PrHtmlPerformanceEntry*> entries;
    entries.resize(total);
    prHtmlPerformanceEntryListEntries(list, PrHtmlPerformanceEntryType_Count, PrStringView{nullptr, 0}, entries.data, total);
    JSValue result = entriesToJs(ctx, entries.data, total);
    entries.reset();
    prHtmlPerformanceEntryListRelease(list);
    return result;
}

// supportedEntryTypes, whose array the getter's function holds: one frozen array for each realm
// (performance-timeline/#supportedentrytypes-attribute).
JSValue supportedEntryTypes(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    return JS_DupValue(ctx, data[0]);
}

void defineSupportedEntryTypes(JSContext* ctx, JSValueConst constructor) {
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < PrHtmlPerformanceEntryType_Count; ++i)
        JS_SetPropertyUint32(ctx, array, i,
            jsNewStringView(ctx, prHtmlPerformanceEntryTypeName(PrHtmlPerformanceEntryType(i))));
    JS_FreezeObject(ctx, array);
    JSValue getter = JS_NewCFunctionData(ctx, supportedEntryTypes, 0, 0, 1, &array);
    JSAtom name = JS_NewAtom(ctx, "supportedEntryTypes");
    JS_DefinePropertyGetSet(ctx, constructor, name, getter, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, name);
    JS_FreeValue(ctx, array);
}

#define PR_MOMENT_GET(name, moment) PR_JS_MAGIC_GET(name, entryMoment, PrHtmlPerformanceMoment_##moment)
#define PR_TIMING_GET(name, moment) PR_JS_MAGIC_GET(name, timingMoment, PrHtmlPerformanceMoment_##moment)

constexpr JSCFunctionListEntry kEntryProto[] = {
    PR_JS_TAG("PerformanceEntry"),
    PR_JS_MAGIC_GET("name", entryAttribute, EntryAttribute_Name),
    PR_JS_MAGIC_GET("entryType", entryAttribute, EntryAttribute_EntryType),
    PR_JS_MAGIC_GET("startTime", entryAttribute, EntryAttribute_StartTime),
    PR_JS_MAGIC_GET("duration", entryAttribute, EntryAttribute_Duration),
    PR_JS_METHOD("toJSON", 0, entryToJson),
};

constexpr JSCFunctionListEntry kMarkProto[] = {PR_JS_TAG("PerformanceMark"), PR_JS_GET("detail", entryDetail)};

constexpr JSCFunctionListEntry kMeasureProto[] = {PR_JS_TAG("PerformanceMeasure"), PR_JS_GET("detail", entryDetail)};

constexpr JSCFunctionListEntry kResourceProto[] = {
    PR_JS_TAG("PerformanceResourceTiming"),
    PR_JS_MAGIC_GET("initiatorType", entryResourceAttribute, ResourceAttribute_InitiatorType),
    PR_JS_MAGIC_GET("deliveryType", entryResourceAttribute, ResourceAttribute_DeliveryType),
    PR_JS_MAGIC_GET("nextHopProtocol", entryResourceAttribute, ResourceAttribute_NextHopProtocol),
    PR_MOMENT_GET("workerStart", WorkerStart),
    PR_MOMENT_GET("redirectStart", RedirectStart),
    PR_MOMENT_GET("redirectEnd", RedirectEnd),
    PR_MOMENT_GET("fetchStart", FetchStart),
    PR_MOMENT_GET("domainLookupStart", DomainLookupStart),
    PR_MOMENT_GET("domainLookupEnd", DomainLookupEnd),
    PR_MOMENT_GET("connectStart", ConnectStart),
    PR_MOMENT_GET("connectEnd", ConnectEnd),
    PR_MOMENT_GET("secureConnectionStart", SecureConnectionStart),
    PR_MOMENT_GET("requestStart", RequestStart),
    PR_MOMENT_GET("finalResponseHeadersStart", FinalResponseHeadersStart),
    PR_MOMENT_GET("firstInterimResponseStart", FirstInterimResponseStart),
    PR_MOMENT_GET("responseStart", ResponseStart),
    PR_MOMENT_GET("responseEnd", ResponseEnd),
    PR_JS_MAGIC_GET("transferSize", entryResourceAttribute, ResourceAttribute_TransferSize),
    PR_JS_MAGIC_GET("encodedBodySize", entryResourceAttribute, ResourceAttribute_EncodedBodySize),
    PR_JS_MAGIC_GET("decodedBodySize", entryResourceAttribute, ResourceAttribute_DecodedBodySize),
    PR_JS_MAGIC_GET("responseStatus", entryResourceAttribute, ResourceAttribute_ResponseStatus),
    PR_JS_MAGIC_GET("contentType", entryResourceAttribute, ResourceAttribute_ContentType),
    PR_JS_MAGIC_GET("contentEncoding", entryResourceAttribute, ResourceAttribute_ContentEncoding),
    PR_JS_GET("serverTiming", entryServerTiming),
    PR_JS_METHOD("toJSON", 0, entryToJson),
};

constexpr JSCFunctionListEntry kServerTimingProto[] = {
    PR_JS_TAG("PerformanceServerTiming"),
    PR_JS_GET("name", serverTimingName),
    PR_JS_GET("duration", serverTimingDuration),
    PR_JS_GET("description", serverTimingDescription),
    PR_JS_METHOD("toJSON", 0, serverTimingToJson),
};

constexpr JSCFunctionListEntry kNavigationTimingProto[] = {
    PR_JS_TAG("PerformanceNavigationTiming"),
    PR_MOMENT_GET("unloadEventStart", UnloadEventStart),
    PR_MOMENT_GET("unloadEventEnd", UnloadEventEnd),
    PR_MOMENT_GET("domInteractive", DomInteractive),
    PR_MOMENT_GET("domContentLoadedEventStart", DomContentLoadedEventStart),
    PR_MOMENT_GET("domContentLoadedEventEnd", DomContentLoadedEventEnd),
    PR_MOMENT_GET("domComplete", DomComplete),
    PR_MOMENT_GET("loadEventStart", LoadEventStart),
    PR_MOMENT_GET("loadEventEnd", LoadEventEnd),
    PR_JS_GET("type", navigationType),
    PR_JS_GET("redirectCount", navigationRedirectCount),
    PR_JS_METHOD("toJSON", 0, entryToJson),
};

constexpr JSCFunctionListEntry kObserverProto[] = {
    PR_JS_TAG("PerformanceObserver"),
    PR_JS_METHOD("observe", 0, observerObserve),
    PR_JS_METHOD("disconnect", 0, observerDisconnect),
    PR_JS_METHOD("takeRecords", 0, observerTakeRecords),
};

constexpr JSCFunctionListEntry kEntryListProto[] = {
    PR_JS_TAG("PerformanceObserverEntryList"),
    PR_JS_MAGIC("getEntries", 0, listEntries, EntriesCall_All),
    PR_JS_MAGIC("getEntriesByType", 1, listEntries, EntriesCall_ByType),
    PR_JS_MAGIC("getEntriesByName", 1, listEntries, EntriesCall_ByName),
};

constexpr JSCFunctionListEntry kTimingProto[] = {
    PR_JS_TAG("PerformanceTiming"),
    PR_TIMING_GET("navigationStart", NavigationStart),
    PR_TIMING_GET("unloadEventStart", UnloadEventStart),
    PR_TIMING_GET("unloadEventEnd", UnloadEventEnd),
    PR_TIMING_GET("redirectStart", RedirectStart),
    PR_TIMING_GET("redirectEnd", RedirectEnd),
    PR_TIMING_GET("fetchStart", FetchStart),
    PR_TIMING_GET("domainLookupStart", DomainLookupStart),
    PR_TIMING_GET("domainLookupEnd", DomainLookupEnd),
    PR_TIMING_GET("connectStart", ConnectStart),
    PR_TIMING_GET("connectEnd", ConnectEnd),
    PR_TIMING_GET("secureConnectionStart", SecureConnectionStart),
    PR_TIMING_GET("requestStart", RequestStart),
    PR_TIMING_GET("responseStart", ResponseStart),
    PR_TIMING_GET("responseEnd", ResponseEnd),
    PR_TIMING_GET("domLoading", DomLoading),
    PR_TIMING_GET("domInteractive", DomInteractive),
    PR_TIMING_GET("domContentLoadedEventStart", DomContentLoadedEventStart),
    PR_TIMING_GET("domContentLoadedEventEnd", DomContentLoadedEventEnd),
    PR_TIMING_GET("domComplete", DomComplete),
    PR_TIMING_GET("loadEventStart", LoadEventStart),
    PR_TIMING_GET("loadEventEnd", LoadEventEnd),
    PR_JS_METHOD("toJSON", 0, timingToJson),
};

constexpr JSCFunctionListEntry kNavigationProto[] = {
    PR_JS_TAG("PerformanceNavigation"),
    PR_JS_GET("type", legacyNavigationType),
    PR_JS_GET("redirectCount", legacyRedirectCount),
    PR_JS_METHOD("toJSON", 0, legacyNavigationToJson),
};

constexpr JsConstant kNavigationConstants[] = {
    {"TYPE_NAVIGATE", PrHtmlNavigationType_Navigate},
    {"TYPE_RELOAD", PrHtmlNavigationType_Reload},
    {"TYPE_BACK_FORWARD", PrHtmlNavigationType_BackForward},
    {"TYPE_RESERVED", 255},
};

constexpr JSCFunctionListEntry kPerformanceMembers[] = {
    PR_JS_MAGIC("getEntries", 0, timelineEntries, EntriesCall_All),
    PR_JS_MAGIC("getEntriesByType", 1, timelineEntries, EntriesCall_ByType),
    PR_JS_MAGIC("getEntriesByName", 1, timelineEntries, EntriesCall_ByName),
    PR_JS_METHOD("mark", 1, mark),
    PR_JS_METHOD("measure", 1, measure),
    PR_JS_MAGIC("clearMarks", 0, clearEntries, PrHtmlPerformanceEntryType_Mark),
    PR_JS_MAGIC("clearMeasures", 0, clearEntries, PrHtmlPerformanceEntryType_Measure),
    PR_JS_METHOD("clearResourceTimings", 0, clearResourceTimings),
    PR_JS_METHOD("setResourceTimingBufferSize", 1, setResourceTimingBufferSize),
    PR_JS_DOM_EVENT_HANDLER(onresourcetimingbufferfull),
    PR_JS_GET("timing", timingObject),
    PR_JS_GET("navigation", navigationObject),
};

void releaseEntry(PrHtmlPerformanceEntry* PR_NULLABLE entry) {
    prHtmlPerformanceEntryRelease(entry);
}

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("PerformanceServerTiming", performanceServerTimingClass,
            (jsFinalizer<&performanceServerTimingClass, freeServerTiming>), kServerTimingProto),
    },
    {
        PR_JS_CLASS("PerformanceEntry", performanceEntryClass,
            (jsFinalizer<&performanceEntryClass, releaseEntry>), kEntryProto),
        .references = &kEntryReferences,
    },
    {
        PR_JS_CLASS("PerformanceMark", performanceMarkClass, (jsFinalizer<&performanceMarkClass, releaseEntry>),
            kMarkProto),
        .constructor = markConstruct,
        .length = 1,
        .base = &performanceEntryClass,
    },
    {
        PR_JS_CLASS("PerformanceMeasure", performanceMeasureClass,
            (jsFinalizer<&performanceMeasureClass, releaseEntry>), kMeasureProto),
        .base = &performanceEntryClass,
    },
    {
        PR_JS_CLASS("PerformanceResourceTiming", performanceResourceTimingClass,
            (jsFinalizer<&performanceResourceTimingClass, releaseEntry>), kResourceProto),
        .base = &performanceEntryClass,
    },
    {
        PR_JS_CLASS("PerformanceNavigationTiming", performanceNavigationTimingClass,
            (jsFinalizer<&performanceNavigationTimingClass, releaseEntry>), kNavigationTimingProto),
        .base = &performanceResourceTimingClass,
    },
    {
        PR_JS_CLASS("PerformanceObserver", performanceObserverClass,
            (jsFinalizer<&performanceObserverClass, prHtmlPerformanceObserverRelease>), kObserverProto),
        .constructor = observerConstruct,
        .length = 1,
        .references = &kObserverReferences,
    },
    {
        PR_JS_CLASS("PerformanceObserverEntryList", performanceEntryListClass,
            (jsFinalizer<&performanceEntryListClass, prHtmlPerformanceEntryListRelease>), kEntryListProto),
        .references = &kEntryListReferences,
    },
    {PR_JS_CLASS("PerformanceTiming", performanceTimingClass, jsViewFinalizer<&performanceTimingClass>, kTimingProto)},
    {
        PR_JS_CLASS("PerformanceNavigation", performanceNavigationClass, jsViewFinalizer<&performanceNavigationClass>,
            kNavigationProto),
        .constants = kNavigationConstants,
        .constantCount = int(countOf(kNavigationConstants)),
    },
};

} // namespace

void jsHtmlPerformanceBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses) {
        JSValue constructor = jsDefineClass(ctx, definition);
        if (definition.id == &performanceObserverClass)
            defineSupportedEntryTypes(ctx, constructor);
        JS_FreeValue(ctx, constructor);
    }
    JSValue proto = JS_GetClassProto(ctx, htmlPerformanceClass);
    JS_SetPropertyFunctionList(ctx, proto, kPerformanceMembers, int(countOf(kPerformanceMembers)));
    JS_FreeValue(ctx, proto);
}

} // namespace pr_impl
