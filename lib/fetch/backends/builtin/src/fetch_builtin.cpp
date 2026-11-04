// The built-in fetcher (fetch_builtin.h).

#include "pr/fetch_builtin.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/fetch_blob.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/time.h"
#include "pr/url.h"
#include "pr/url_data.h"
#include "pr/url_percent.h"

#include "fetch/backends/builtin/src/fetch_cache.h"

namespace pr_impl {

namespace {

// The most body bytes that a fetch holds that arrived and were not read. While it holds this many, it reads no more of
// its exchange's content.
constexpr uint32_t kBodyMaxBytes = 256 * 1024;

// The bytes that one read from an exchange takes at most, and that one decode writes at most.
constexpr uint32_t kChunkBytes = 16 * 1024;

// The most redirects that a fetch follows (fetch/#http-redirect-fetch).
constexpr uint32_t kMaxRedirects = 20;

// The largest body that the cache stores. A larger one is fetched and not stored.
constexpr uint32_t kCacheMaxBodyBytes = 64 * 1024 * 1024;

struct BuiltinFetcher : PrFetchNetwork {
    PrHttp* PR_NULLABLE http;
    PrCompressCodec* PR_NULLABLE compress;
    PrStorage* PR_NULLABLE cache;
    PrRawString userAgentStorage;  // what the base's userAgent views
};

enum {
    FetchState_Starting,        // waiting for the task that starts it
    FetchState_Waiting,         // waiting for a response
    FetchState_Body,
    FetchState_Done,
    FetchState_Failed,
};
typedef uint8_t FetchState;

// Header names and values, which a fetch owns.
struct HeaderList {
    RawArray<PrRawString> names;
    RawArray<PrRawString> values;
};

void headersAdd(HeaderList* list, PrStringView name, PrStringView value) {
    list->names.push(prRawStringCopy(name));
    list->values.push(prRawStringCopy(value));
}

void headersClear(HeaderList* list) {
    for (uint32_t i = 0; i < list->names.count; ++i) {
        prRawStringReset(&list->names.data[i]);
        prRawStringReset(&list->values.data[i]);
    }
    list->names.clear();
    list->values.clear();
}

void headersReset(HeaderList* list) {
    headersClear(list);
    list->names.reset();
    list->values.reset();
}

// Appends views of the list's headers to `out`, as headers of the fetch API or of the HTTP request that a fetch sends,
// which both hold a name and a value.
template <typename Header>
void headersView(const HeaderList* list, RawArray<Header>* out) {
    for (uint32_t i = 0; i < list->names.count; ++i)
        out->push(Header{prRawStringView(&list->names.data[i]), prRawStringView(&list->values.data[i])});
}

PrStringView headersFind(const HeaderList* list, PrStringView name) {
    for (uint32_t i = 0; i < list->names.count; ++i) {
        if (prAsciiEqualsFold(prRawStringView(&list->names.data[i]), name))
            return prRawStringView(&list->values.data[i]);
    }
    return PrStringView{nullptr, 0};
}

// A fetch. The memory outlives the fetch's destruction while `holds` is not zero: while one of its handlers runs, which
// a client may destroy it from, and while a task that it posted waits.
struct BuiltinFetch : PrFetch {
    uint32_t holds;
    bool dead;
    PrFetchFn PR_NULLABLE event;
    void* PR_NULLABLE context;
    FetchState state;
    PrRawString url;               // the URL being fetched, without its fragment
    PrRawString method;
    HeaderList requestHeaders;  // the request's own headers
    HeaderList sent;            // the headers of the request last sent, with the fetcher's
    PrRawString body;
    PrFetchDestination destination;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;
    uint32_t redirects;
    PrHttpExchange* PR_NULLABLE exchange;
    bool local;                 // true for a data:, file: or about: URL, which the fetcher answers itself

    // What Fetch decides above HTTP: the request's serialized origin, which is "null" for an opaque origin, and its
    // tainted origin flag (fetch/#concept-request-tainted-origin); the referrer that the next request sends, which is
    // the null string for none; and the response tainting (fetch/#concept-request-response-tainting).
    PrRawString origin;
    bool taintedOrigin;
    PrRawString referrer;
    PrFetchReferrerPolicy referrerPolicy;
    PrFetchResponseType tainting;
    bool preflighting;          // true while the exchange is the CORS preflight (fetch/#cors-preflight-fetch)

    // The response: its type, status and headers, and its body as it arrives.
    PrFetchResponseType type;
    uint16_t status;
    HeaderList response;
    PrCompressDecoder* PR_NULLABLE decoder;
    RawArray<uint8_t> content;     // body bytes that arrived, from `contentStart`
    uint32_t contentStart;
    bool exchangeEnded;         // true once the exchange's content ended, with bytes of it still unread
    bool stalled;               // true while the body holds as much as it may, and the exchange holds the rest
    bool resumePosted;
    PrNetError error;

    // The cache's part: the record that a stale entry was read from while the request revalidates it, when the request
    // was sent, and a copy of the body that the cache stores once it ends.
    PrStorageRecord revalidated;
    double requested;
    bool storing;
    RawArray<uint8_t> stored;

    PrFetchTiming timing;

    // The blob that a blob URL named when the fetch was created (fetch/#scheme-fetch), or null.
    const PrFetchBlob* PR_NULLABLE blob;
};

void freeFetch(BuiltinFetch* fetch) {
    prFetchBlobRelease(fetch->blob);
    prRawStringReset(&fetch->url);
    prRawStringReset(&fetch->method);
    headersReset(&fetch->requestHeaders);
    headersReset(&fetch->sent);
    headersReset(&fetch->response);
    prRawStringReset(&fetch->body);
    prRawStringReset(&fetch->origin);
    prRawStringReset(&fetch->referrer);
    fetch->content.reset();
    fetch->stored.reset();
    prMemoryFree(fetch);
}

void release(BuiltinFetch* fetch) {
    PR_ASSERT(fetch->holds != 0);
    if (--fetch->holds == 0 && fetch->dead)
        freeFetch(fetch);
}

BuiltinFetcher* fetcherOf(BuiltinFetch* fetch) {
    return static_cast<BuiltinFetcher*>(fetch->fetcher);
}

void deliver(BuiltinFetch* fetch, PrFetchEvent event) {
    double now = prTimeMonotonicMilliseconds();
    if (event == PrFetchEvent_Response) {
        // An answer that no exchange received, as the cache's or a local one is, starts here.
        fetch->timing.requestStart = fetch->timing.requestStart != 0 ? fetch->timing.requestStart : now;
        fetch->timing.responseStart = fetch->timing.responseStart != 0 ? fetch->timing.responseStart : now;
    } else if (event == PrFetchEvent_End || event == PrFetchEvent_Fail) {
        fetch->timing.endTime = now;
    }
    if (!fetch->dead && fetch->event != nullptr)
        fetch->event(fetch->context, fetch, event);
}

void dropRevalidated(BuiltinFetch* fetch) {
    if (fetch->revalidated.handle != nullptr)
        prStorageRecordRelease(fetcherOf(fetch)->cache, &fetch->revalidated);
}

void dropExchange(BuiltinFetch* fetch) {
    prHttpExchangeDestroy(fetch->exchange);
    fetch->exchange = nullptr;
    prCompressDecoderDestroy(fetch->decoder);
    fetch->decoder = nullptr;
}

void fail(BuiltinFetch* fetch, PrNetError error) {
    if (fetch->state == FetchState_Failed || fetch->state == FetchState_Done)
        return;
    fetch->state = FetchState_Failed;
    fetch->error = error;
    fetch->storing = false;
    dropExchange(fetch);
    dropRevalidated(fetch);
    deliver(fetch, PrFetchEvent_Fail);
}

// The Accept that a request for `destination` sends if it has none (fetch/#fetching).
PrStringView acceptOf(PrFetchDestination destination) {
    switch (destination) {
        case PrFetchDestination_Document:
        case PrFetchDestination_Object:
            return "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
        case PrFetchDestination_Image:
            return "image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
        case PrFetchDestination_Style:
            return "text/css,*/*;q=0.1";
        default:
            return "*/*";
    }
}

bool isRedirect(uint16_t status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Returns true if the fetch's method is one that rfc 9110 section 9.2.1 calls safe.
bool isSafe(const BuiltinFetch* fetch) {
    PrStringView method = prRawStringView(&fetch->method);
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

// Returns true if two serialized origins are the same origin (html/#same-origin). An opaque origin, which serializes as
// "null", is the same as no other.
bool isSameOrigin(PrStringView a, PrStringView b) {
    return a == b && a != "null";
}

// Returns true if the URL's origin is the request's origin, apart from its tainted origin flag.
bool isSameOriginAs(const BuiltinFetch* fetch, const PrUrl* url) {
    PrRawString origin = prUrlOrigin(url);
    bool same = isSameOrigin(prRawStringView(&origin), prRawStringView(&fetch->origin));
    prRawStringReset(&origin);
    return same;
}

// The request's origin as its Origin header and the CORS check serialize it (fetch/#serializing-a-request-origin).
PrStringView serializedOrigin(const BuiltinFetch* fetch) {
    return fetch->taintedOrigin ? PrStringView{"null"} : prRawStringView(&fetch->origin);
}

// Returns true if the URL is potentially trustworthy (secure-contexts/#is-url-trustworthy).
bool isTrustworthy(const PrUrl* url) {
    PrStringView scheme = prUrlScheme(url);
    if (scheme == "about")
        return prUrlPath(url) == "blank" || prUrlPath(url) == "srcdoc";
    if (scheme == "data" || scheme == "https" || scheme == "wss" || scheme == "file")
        return true;
    PrStringView host = prUrlHost(url);
    return host == "[::1]" || prStringStartsWith(host, "127.") || host == "localhost" || prAsciiEndsWithFold(host, ".localhost");
}

// Returns true if the URL's scheme is local (fetch/#local-scheme), which a referrer never names.
bool isLocalScheme(const PrUrl* url) {
    PrStringView scheme = prUrlScheme(url);
    return scheme == "about" || scheme == "blob" || scheme == "data";
}

// Return the URL as a referrer sends it (referrer-policy/#strip-url): without its credentials and fragment, or as its
// origin alone. Each returns the null string for a URL whose scheme is local.
PrRawString strippedUrl(const PrUrl* url) {
    if (isLocalScheme(url))
        return prRawStringNull();
    PrUrl* copy = prUrlClone(url);
    prUrlSetComponent(copy, PrUrlComponent_Username, "");
    prUrlSetComponent(copy, PrUrlComponent_Password, "");
    PrRawString text = prUrlSerialize(copy, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(copy);
    return text;
}

PrRawString strippedOrigin(const PrUrl* url) {
    if (isLocalScheme(url))
        return prRawStringNull();
    PrRawString origin = prUrlOrigin(url);
    prRawStringAppend(&origin, "/");
    return origin;
}

// The longest referrer that a request sends whole. A longer one is sent as its origin
// (referrer-policy/#determine-requests-referrer).
constexpr size_t kReferrerMaxBytes = 4096;

// Recomputes the referrer that the request sends to `current`, its current URL, from the referrer that it sent before
// (referrer-policy/#determine-requests-referrer). A redirect recomputes it for the URL that it leads to.
void determineReferrer(BuiltinFetch* fetch, const PrUrl* current) {
    if (prRawStringIsNull(&fetch->referrer))
        return;
    PrUrl* source = nullptr;
    if (prUrlCreate(prRawStringView(&fetch->referrer), nullptr, PrCharset_Utf8, &source) != PrStatus_Ok) {
        prRawStringReset(&fetch->referrer);
        return;
    }
    PrRawString whole = strippedUrl(source);
    PrRawString origin = strippedOrigin(source);
    if (prRawStringView(&whole).size > kReferrerMaxBytes)
        prRawStringReplace(&whole, prRawStringView(&origin));
    PrRawString sourceOrigin = prUrlOrigin(source);
    PrRawString currentOrigin = prUrlOrigin(current);
    bool same = isSameOrigin(prRawStringView(&sourceOrigin), prRawStringView(&currentOrigin));
    bool downgrade = isTrustworthy(source) && !isTrustworthy(current);
    prRawStringReset(&sourceOrigin);
    prRawStringReset(&currentOrigin);
    prUrlDestroy(source);
    const PrRawString* chosen = nullptr;
    switch (fetch->referrerPolicy) {
        case PrFetchReferrerPolicy_NoReferrer:
            break;
        case PrFetchReferrerPolicy_NoReferrerWhenDowngrade:
            chosen = downgrade ? nullptr : &whole;
            break;
        case PrFetchReferrerPolicy_SameOrigin:
            chosen = same ? &whole : nullptr;
            break;
        case PrFetchReferrerPolicy_Origin:
            chosen = &origin;
            break;
        case PrFetchReferrerPolicy_StrictOrigin:
            chosen = downgrade ? nullptr : &origin;
            break;
        case PrFetchReferrerPolicy_OriginWhenCrossOrigin:
            chosen = same ? &whole : &origin;
            break;
        case PrFetchReferrerPolicy_Default:
        case PrFetchReferrerPolicy_StrictOriginWhenCrossOrigin:
            chosen = same ? &whole : downgrade ? nullptr : &origin;
            break;
        case PrFetchReferrerPolicy_UnsafeUrl:
            chosen = &whole;
            break;
    }
    if (chosen != nullptr && !prRawStringIsNull(chosen))
        prRawStringReplace(&fetch->referrer, prRawStringView(chosen));
    else
        prRawStringReset(&fetch->referrer);
    prRawStringReset(&whole);
    prRawStringReset(&origin);
}

// Adds the Origin header that the request sends to `current`, its current URL, if it sends one
// (fetch/#append-a-request-origin-header). A CORS request always sends it. Any other request sends it with a method
// other than GET and HEAD, as "null" if its referrer policy withholds its origin.
void appendOriginHeader(BuiltinFetch* fetch, const PrUrl* current) {
    PrStringView origin = serializedOrigin(fetch);
    PrStringView method = prRawStringView(&fetch->method);
    if (fetch->tainting != PrFetchResponseType_Cors) {
        if (method == "GET" || method == "HEAD")
            return;
        if (fetch->mode != PrFetchMode_Cors) {
            switch (fetch->referrerPolicy) {
                case PrFetchReferrerPolicy_NoReferrer:
                    origin = "null";
                    break;
                case PrFetchReferrerPolicy_Default:
                case PrFetchReferrerPolicy_NoReferrerWhenDowngrade:
                case PrFetchReferrerPolicy_StrictOrigin:
                case PrFetchReferrerPolicy_StrictOriginWhenCrossOrigin:
                    if (prStringStartsWith(origin, "https:") && prUrlScheme(current) != "https")
                        origin = "null";
                    break;
                case PrFetchReferrerPolicy_SameOrigin:
                    if (!isSameOriginAs(fetch, current))
                        origin = "null";
                    break;
                case PrFetchReferrerPolicy_Origin:
                case PrFetchReferrerPolicy_OriginWhenCrossOrigin:
                case PrFetchReferrerPolicy_UnsafeUrl:
                    break;
            }
        }
    }
    headersAdd(&fetch->sent, "Origin", origin);
}

// Appends to `out` the names of the request's own headers that a request to another origin may send only after a
// preflight (fetch/#cors-unsafe-request-header-names), lowercased, sorted and without repeats.
void unsafeHeaderNames(const HeaderList* headers, RawArray<PrRawString>* out) {
    // The most bytes of safelisted values that a request sends without a preflight.
    constexpr size_t kSafelistMaxBytes = 1024;
    RawArray<uint32_t> unsafe{};
    RawArray<uint32_t> safelisted{};
    size_t safelistedBytes = 0;
    for (uint32_t i = 0; i < headers->names.count; ++i) {
        PrStringView value = prRawStringView(&headers->values.data[i]);
        if (prFetchIsCorsSafelistedRequestHeader(prRawStringView(&headers->names.data[i]), value)) {
            safelisted.push(i);
            safelistedBytes += value.size;
        } else {
            unsafe.push(i);
        }
    }
    if (safelistedBytes > kSafelistMaxBytes)
        unsafe.append(safelisted.span());
    for (uint32_t k = 0; k < unsafe.count; ++k) {
        PrRawString name = prRawStringCopy(prRawStringView(&headers->names.data[unsafe.data[k]]));
        prAsciiLowerText(const_cast<char*>(prRawStringView(&name).data), prRawStringView(&name).size);
        uint32_t at = 0;
        while (at < out->count && prStringCompare(prRawStringView(&out->data[at]), prRawStringView(&name)) < 0)
            ++at;
        if (at < out->count && prRawStringView(&out->data[at]) == prRawStringView(&name)) {
            prRawStringReset(&name);
            continue;
        }
        out->insert(at, name);
    }
    unsafe.reset();
    safelisted.reset();
}

// Returns true if the request to another origin needs a preflight (fetch/#main-fetch).
bool needsPreflight(const BuiltinFetch* fetch) {
    if (!prFetchIsCorsSafelistedMethod(prRawStringView(&fetch->method)))
        return true;
    RawArray<PrRawString> names{};
    unsafeHeaderNames(&fetch->requestHeaders, &names);
    bool unsafe = names.count != 0;
    for (uint32_t i = 0; i < names.count; ++i)
        prRawStringReset(&names.data[i]);
    names.reset();
    return unsafe;
}

// Returns the value of the only header named `name`, or the null view if the list has none or more than one, as a CORS
// response header that is sent twice combines to a value that matches nothing.
PrStringView headersFindOnly(const HeaderList* list, PrStringView name) {
    PrStringView found{nullptr, 0};
    for (uint32_t i = 0; i < list->names.count; ++i) {
        if (!prAsciiEqualsFold(prRawStringView(&list->names.data[i]), name))
            continue;
        if (found.data != nullptr)
            return PrStringView{nullptr, 0};
        found = prRawStringView(&list->values.data[i]);
    }
    return found;
}

// Returns true if the response's headers pass the CORS check for the request (fetch/#cors-check).
bool passesCors(const BuiltinFetch* fetch, const HeaderList* response) {
    PrStringView allowed = headersFindOnly(response, "access-control-allow-origin");
    if (allowed.data == nullptr)
        return false;
    bool include = fetch->credentials == PrFetchCredentials_Include;
    if (!include && allowed == "*")
        return true;
    if (allowed != serializedOrigin(fetch))
        return false;
    return !include || headersFindOnly(response, "access-control-allow-credentials") == "true";
}

// Appends to `out` each token of the comma-separated values of the headers named `name`
// (fetch/#extract-header-list-values). Returns false if a value is not a token.
bool headerTokens(const HeaderList* list, PrStringView name, RawArray<PrStringView>* out) {
    for (uint32_t i = 0; i < list->names.count; ++i) {
        if (!prAsciiEqualsFold(prRawStringView(&list->names.data[i]), name))
            continue;
        PrStringView value = prRawStringView(&list->values.data[i]);
        size_t start = 0;
        while (start <= value.size) {
            size_t end = start;
            while (end < value.size && value.data[end] != ',')
                ++end;
            PrStringView item = prStringTrim(prStringSlice(value, start, end - start));
            for (size_t k = 0; k < item.size; ++k) {
                if (!prAsciiIsHttpToken(item.data[k]))
                    return false;
            }
            if (item.size != 0)
                out->push(item);
            start = end + 1;
        }
    }
    return true;
}

// Appends `bytes` to the body, decoded if the response has a content coding, and to the cache's copy. Returns false if
// they do not decode.
bool appendBody(BuiltinFetch* fetch, PrByteView bytes, bool final) {
    fetch->timing.encodedBodySize += bytes.size;
    if (fetch->contentStart != 0 && fetch->contentStart == fetch->content.count) {
        fetch->content.clear();
        fetch->contentStart = 0;
    }
    uint32_t before = fetch->content.count;
    bool decoded = true;
    if (fetch->decoder == nullptr) {
        fetch->content.append(bytes.data, uint32_t(bytes.size));
    } else {
        for (;;) {
            uint32_t at = fetch->content.count;
            fetch->content.resize(at + kChunkBytes);
            PrCompressRead read{};
            PrStatus status = prCompressDecoderRead(fetch->decoder, bytes, final, fetch->content.data + at, kChunkBytes, &read);
            fetch->content.resize(at + uint32_t(read.written));
            if (status != PrStatus_Ok) {
                decoded = false;
                break;
            }
            bytes.data = bytes.data != nullptr ? bytes.data + read.consumed : nullptr;
            bytes.size -= read.consumed;
            if (read.progress == PrCompressProgress_Done)
                break;
            if (read.progress == PrCompressProgress_Input && bytes.size == 0) {
                decoded = !final;
                break;
            }
        }
    }
    uint32_t added = fetch->content.count - before;
    fetch->timing.decodedBodySize += added;
    if (fetch->storing) {
        if (fetch->stored.count + added > kCacheMaxBodyBytes) {
            fetch->storing = false;
            fetch->stored.reset();
        } else {
            fetch->stored.append(fetch->content.data + before, added);
        }
    }
    return decoded;
}

// Stores the response as the cache keeps it, at the fetch's URL, with `body`.
void store(BuiltinFetch* fetch, PrByteView body) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    FetchCachedResponse response{};
    response.status = fetch->status;
    response.requested = fetch->requested;
    response.received = prTimeWallMilliseconds();
    for (uint32_t i = 0; i < fetch->response.names.count; ++i) {
        PrStringView name = prRawStringView(&fetch->response.names.data[i]);
        // The body is stored decoded, so the headers that describe its encoding on the wire do not apply to it.
        if (prAsciiEqualsFold(name, "content-encoding") || prAsciiEqualsFold(name, "content-length") ||
                prAsciiEqualsFold(name, "transfer-encoding"))
            continue;
        response.headers.push(PrFetchHeader{name, prRawStringView(&fetch->response.values.data[i])});
    }
    PrStringView vary = headersFind(&fetch->response, "vary");
    size_t start = 0;
    while (vary.data != nullptr && start <= vary.size) {
        size_t end = start;
        while (end < vary.size && vary.data[end] != ',')
            ++end;
        PrStringView name = prStringTrim(prStringSlice(vary, start, end - start));
        if (name.size != 0)
            response.varied.push(PrFetchHeader{name, headersFind(&fetch->sent, name)});
        start = end + 1;
    }
    response.body = body;
    PrTextBuffer record{};
    fetchCacheWrite(&response, &record);
    PrStringView bytes = prTextBufferView(&record);
    prStorageWrite(fetcher->cache, prRawStringView(&fetch->url),
        PrByteView{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size});
    prTextBufferReset(&record);
    fetchCachedResponseReset(&response);
}

// Reads the exchange's content into the body while the fetch has room. Delivers Readable for what arrived, then End
// once the content ended and every byte of it has been taken.
void drain(BuiltinFetch* fetch) {
    bool waiting = fetch->content.count > fetch->contentStart;
    uint32_t before = fetch->content.count - fetch->contentStart;
    uint8_t chunk[kChunkBytes];
    bool emptied = false;
    fetch->stalled = false;
    for (;;) {
        if (fetch->content.count - fetch->contentStart >= kBodyMaxBytes) {
            fetch->stalled = true;
            break;
        }
        size_t got = prHttpExchangeRead(fetch->exchange, chunk, sizeof(chunk));
        if (got == 0) {
            emptied = true;
            break;
        }
        if (!appendBody(fetch, PrByteView{chunk, got}, false)) {
            fail(fetch, PrNetError_Protocol);
            return;
        }
    }
    bool ended = fetch->exchangeEnded && emptied;
    if (ended && fetch->decoder != nullptr && !appendBody(fetch, PrByteView{nullptr, 0}, true)) {
        fail(fetch, PrNetError_Protocol);
        return;
    }
    if (fetch->content.count - fetch->contentStart > before && !waiting)
        deliver(fetch, PrFetchEvent_Readable);
    if (ended && !fetch->dead && fetch->state == FetchState_Body) {
        fetch->state = FetchState_Done;
        prCompressDecoderDestroy(fetch->decoder);
        fetch->decoder = nullptr;
        if (fetch->storing) {
            fetch->storing = false;
            store(fetch, PrByteView{fetch->stored.data, fetch->stored.count});
            fetch->stored.reset();
        }
        deliver(fetch, PrFetchEvent_End);
    }
}

void exchangeEvent(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event);
void fetchCurrent(BuiltinFetch* fetch);

// Follows the redirect that the response is (fetch/#http-redirect-fetch). Returns false if the response is to be
// received as it is.
bool followRedirect(BuiltinFetch* fetch) {
    PrStringView location = headersFind(&fetch->response, "location");
    if (!isRedirect(fetch->status) || location.data == nullptr || fetch->redirect == PrFetchRedirect_Manual)
        return false;
    if (fetch->redirect == PrFetchRedirect_Error || fetch->redirects == kMaxRedirects) {
        fail(fetch, PrNetError_Protocol);
        return true;
    }
    PrUrl* base = nullptr;
    PrUrl* next = nullptr;
    prUrlCreate(prRawStringView(&fetch->url), nullptr, PrCharset_Utf8, &base);
    PrStatus parsed = prUrlCreate(location, base, PrCharset_Utf8, &next);
    if (parsed != PrStatus_Ok || (prUrlScheme(next) != "http" && prUrlScheme(next) != "https")) {
        prUrlDestroy(base);
        prUrlDestroy(next);
        fail(fetch, PrNetError_Protocol);
        return true;
    }
    // A CORS request is not redirected to a URL with credentials, unless it stays at its own origin
    // (fetch/#http-redirect-fetch).
    PrRawString baseOrigin = prUrlOrigin(base);
    PrRawString nextOrigin = prUrlOrigin(next);
    bool credentialed = prUrlUsername(next).size != 0 || prUrlPassword(next).size != 0;
    bool blocked = fetch->mode == PrFetchMode_Cors && credentialed && (fetch->tainting == PrFetchResponseType_Cors ||
        !isSameOrigin(prRawStringView(&nextOrigin), prRawStringView(&fetch->origin)));
    // A redirect from another origin to a third taints the request's origin.
    if (!isSameOrigin(prRawStringView(&nextOrigin), prRawStringView(&baseOrigin)) &&
            !isSameOrigin(prRawStringView(&fetch->origin), prRawStringView(&baseOrigin)))
        fetch->taintedOrigin = true;
    fetch->timing.crossOriginRedirect = fetch->timing.crossOriginRedirect ||
        !isSameOrigin(prRawStringView(&nextOrigin), prRawStringView(&baseOrigin));
    prRawStringReset(&baseOrigin);
    prRawStringReset(&nextOrigin);
    prUrlDestroy(base);
    if (blocked) {
        prUrlDestroy(next);
        fail(fetch, PrNetError_Blocked);
        return true;
    }
    ++fetch->redirects;
    double now = prTimeMonotonicMilliseconds();
    fetch->timing.redirectCount = fetch->redirects;
    fetch->timing.redirectStart =
        fetch->timing.redirectStart != 0 ? fetch->timing.redirectStart : fetch->timing.startTime;
    fetch->timing.redirectEnd = now;
    fetch->timing.postRedirectStart = now;
    fetch->timing.requestStart = 0;
    fetch->timing.responseStart = 0;
    fetch->timing.cacheState = PrFetchCacheState_None;
    prRawStringReset(&fetch->url);
    fetch->url = prUrlSerialize(next, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(next);
    // A 303, or a 301 or 302 of a POST, is followed with a GET and no body.
    PrStringView method = prRawStringView(&fetch->method);
    if ((fetch->status == 303 && method != "GET" && method != "HEAD") ||
            ((fetch->status == 301 || fetch->status == 302) && method == "POST")) {
        prRawStringReplace(&fetch->method, "GET");
        prRawStringReset(&fetch->body);
    }
    dropExchange(fetch);
    headersClear(&fetch->response);
    fetch->status = 0;
    fetchCurrent(fetch);
    return true;
}

// Returns the type of the response that the fetch received, as its tainting and redirect mode make it.
PrFetchResponseType typeOf(const BuiltinFetch* fetch) {
    if (isRedirect(fetch->status) && fetch->redirect == PrFetchRedirect_Manual && fetch->mode != PrFetchMode_Navigate)
        return PrFetchResponseType_OpaqueRedirect;
    return fetch->tainting;
}

// Returns true if the response passes the TAO check (fetch/#concept-tao-check).
bool passesTimingAllow(const BuiltinFetch* fetch) {
    if (!fetch->timing.timingAllowPassed)
        return false;
    for (uint32_t i = 0; i < fetch->response.names.count; ++i) {
        if (!prAsciiEqualsFold(prRawStringView(&fetch->response.names.data[i]), "timing-allow-origin"))
            continue;
        PrStringView values = prRawStringView(&fetch->response.values.data[i]);
        size_t start = 0;
        while (start <= values.size) {
            size_t end = start;
            while (end < values.size && values.data[end] != ',')
                ++end;
            PrStringView value = prStringTrim(prStringSlice(values, start, end - start));
            if (value == "*" || value == serializedOrigin(fetch))
                return true;
            start = end + 1;
        }
    }
    if (fetch->mode == PrFetchMode_Navigate) {
        PrUrl* current = nullptr;
        prUrlCreate(prRawStringView(&fetch->url), nullptr, PrCharset_Utf8, &current);
        bool same = current != nullptr && isSameOriginAs(fetch, current);
        prUrlDestroy(current);
        if (!same)
            return false;
    }
    return fetch->tainting == PrFetchResponseType_Basic;
}

// Returns true if the response may be received: a CORS request's response passes the CORS check, and fails the fetch
// otherwise.
bool admits(BuiltinFetch* fetch) {
    fetch->timing.timingAllowPassed = passesTimingAllow(fetch);
    if (fetch->tainting != PrFetchResponseType_Cors || passesCors(fetch, &fetch->response))
        return true;
    fail(fetch, PrNetError_Blocked);
    return false;
}

// Answers the request with the cached response `entry`, whose bytes the cache holds until this returns.
void answerFromCache(BuiltinFetch* fetch, const FetchCachedResponse* entry) {
    fetch->status = entry->status;
    headersClear(&fetch->response);
    for (uint32_t i = 0; i < entry->headers.count; ++i)
        headersAdd(&fetch->response, entry->headers.data[i].name, entry->headers.data[i].value);
    if (!admits(fetch))
        return;
    if (fetch->redirect != PrFetchRedirect_Manual && followRedirect(fetch))
        return;
    fetch->type = typeOf(fetch);
    fetch->state = FetchState_Body;
    fetch->content.append(entry->body.data, uint32_t(entry->body.size));
    fetch->timing.encodedBodySize = entry->body.size;
    fetch->timing.decodedBodySize = entry->body.size;
    deliver(fetch, PrFetchEvent_Response);
    if (entry->body.size != 0)
        deliver(fetch, PrFetchEvent_Readable);
    if (!fetch->dead && fetch->state == FetchState_Body) {
        fetch->state = FetchState_Done;
        deliver(fetch, PrFetchEvent_End);
    }
}

// Looks the request up in the cache (fetch/#http-network-or-cache-fetch). Answers it and returns true for a fresh
// entry. Keeps a stale entry that has a validator, and adds the conditional headers that revalidate it to the request.
bool lookUp(BuiltinFetch* fetch, const RawArray<PrFetchHeader>& sent) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    FetchCacheControl asked = fetchCacheControlFromHeaders(sent.data, sent.count);
    if (fetcher->cache == nullptr || prRawStringView(&fetch->method) != "GET" || asked.noStore)
        return false;
    PrStorageRecord record{};
    if (!prStorageRead(fetcher->cache, prRawStringView(&fetch->url), &record))
        return false;
    FetchCachedResponse entry{};
    bool answered = false;
    if (fetchCacheRead(record.bytes, &entry) && fetchCacheVaryMatches(&entry, sent.data, sent.count)) {
        FetchCacheControl control = fetchCacheControlFromHeaders(entry.headers.data, entry.headers.count);
        double age = fetchCacheAge(&entry, prTimeWallMilliseconds());
        // A request's max-age limits the age of the response that it accepts (rfc 9111 section 5.2.1.1).
        bool tooOld = asked.hasMaxAge && age >= asked.maxAge * 1000;
        if (!asked.noCache && !control.noCache && !tooOld && age < fetchCacheLifetime(&entry)) {
            fetch->timing.cacheState = PrFetchCacheState_Local;
            answerFromCache(fetch, &entry);
            answered = true;
        } else {
            PrStringView etag = fetchCacheHeader(entry.headers.data, entry.headers.count, "etag");
            PrStringView modified = fetchCacheHeader(entry.headers.data, entry.headers.count, "last-modified");
            if (etag.data != nullptr)
                headersAdd(&fetch->sent, "If-None-Match", etag);
            if (modified.data != nullptr)
                headersAdd(&fetch->sent, "If-Modified-Since", modified);
            if (etag.data != nullptr || modified.data != nullptr) {
                fetch->revalidated = record;
                record = PrStorageRecord{};
            }
        }
    }
    fetchCachedResponseReset(&entry);
    if (record.handle != nullptr)
        prStorageRecordRelease(fetcher->cache, &record);
    return answered;
}

// Sends the fetch's request for `current`, its current URL, or answers it from the cache.
void request(BuiltinFetch* fetch, const PrUrl* current) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    headersClear(&fetch->sent);
    for (uint32_t i = 0; i < fetch->requestHeaders.names.count; ++i)
        headersAdd(&fetch->sent, prRawStringView(&fetch->requestHeaders.names.data[i]),
            prRawStringView(&fetch->requestHeaders.values.data[i]));
    if (!prRawStringIsNull(&fetch->referrer))
        headersAdd(&fetch->sent, "Referer", prRawStringView(&fetch->referrer));
    appendOriginHeader(fetch, current);
    if (headersFind(&fetch->sent, "user-agent").data == nullptr && fetcher->userAgent.size != 0)
        headersAdd(&fetch->sent, "User-Agent", fetcher->userAgent);
    if (headersFind(&fetch->sent, "accept").data == nullptr)
        headersAdd(&fetch->sent, "Accept", acceptOf(fetch->destination));
    if (headersFind(&fetch->sent, "accept-encoding").data == nullptr && fetcher->compress != nullptr)
        headersAdd(&fetch->sent, "Accept-Encoding", "gzip, deflate, br");
    RawArray<PrFetchHeader> sent{};
    headersView(&fetch->sent, &sent);
    fetch->timing.requestStart = prTimeMonotonicMilliseconds();
    bool answered = lookUp(fetch, sent);
    sent.reset();
    if (answered)
        return;
    RawArray<PrHttpHeader> headers{};
    headersView(&fetch->sent, &headers);
    PrStringView body = prRawStringView(&fetch->body);
    PrHttpRequestDesc desc{
        .method = prRawStringView(&fetch->method),
        .url = prRawStringView(&fetch->url),
        .headers = headers.data,
        .headerCount = headers.count,
        .body = PrByteView{reinterpret_cast<const uint8_t*>(body.data), body.size},
        .event = exchangeEvent,
        .context = fetch,
    };
    fetch->state = FetchState_Waiting;
    fetch->requested = prTimeWallMilliseconds();
    PrStatus status = prHttpExchangeCreate(fetcher->http, &desc, &fetch->exchange);
    headers.reset();
    if (status != PrStatus_Ok)
        fail(fetch, PrNetError_Protocol);
}

// Chooses the decoder of the response's content coding. An unknown coding is passed on as it was sent.
void chooseDecoder(BuiltinFetch* fetch) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    PrStringView coding = prStringTrim(headersFind(&fetch->response, "content-encoding"));
    if (fetcher->compress == nullptr || coding.size == 0)
        return;
    PrCompressFormat format = PrCompressFormat_Gzip;
    if (prAsciiEqualsFold(coding, "gzip") || prAsciiEqualsFold(coding, "x-gzip"))
        format = PrCompressFormat_Gzip;
    else if (prAsciiEqualsFold(coding, "deflate"))
        format = PrCompressFormat_Zlib;
    else if (prAsciiEqualsFold(coding, "br"))
        format = PrCompressFormat_Brotli;
    else
        return;
    prCompressDecoderCreate(fetcher->compress, format, &fetch->decoder);
}

// Answers a revalidation that the server answered with 304. The cached entry takes the 304's headers in place of its
// own (rfc 9111 section 4.3.4), is stored again, and answers the request with its body.
void answerRevalidated(BuiltinFetch* fetch, PrHttpExchange* exchange) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    FetchCachedResponse entry{};
    if (!fetchCacheRead(fetch->revalidated.bytes, &entry)) {
        fetchCachedResponseReset(&entry);
        dropRevalidated(fetch);
        fail(fetch, PrNetError_Protocol);
        return;
    }
    HeaderList merged{};
    uint32_t updates = prHttpExchangeHeaderCount(exchange);
    for (uint32_t i = 0; i < entry.headers.count; ++i) {
        bool replaced = false;
        for (uint32_t k = 0; k < updates && !replaced; ++k)
            replaced = prAsciiEqualsFold(prHttpExchangeHeader(exchange, k).name, entry.headers.data[i].name);
        if (!replaced)
            headersAdd(&merged, entry.headers.data[i].name, entry.headers.data[i].value);
    }
    for (uint32_t k = 0; k < updates; ++k) {
        PrHttpHeader header = prHttpExchangeHeader(exchange, k);
        if (!prAsciiEqualsFold(header.name, "content-length") && !prAsciiEqualsFold(header.name, "content-encoding"))
            headersAdd(&merged, header.name, header.value);
    }
    entry.headers.clear();
    headersView(&merged, &entry.headers);
    entry.requested = fetch->requested;
    entry.received = prTimeWallMilliseconds();
    PrTextBuffer record{};
    fetchCacheWrite(&entry, &record);
    dropExchange(fetch);
    fetch->timing.cacheState = PrFetchCacheState_Validated;
    answerFromCache(fetch, &entry);
    PrStringView bytes = prTextBufferView(&record);
    prStorageWrite(fetcher->cache, prRawStringView(&fetch->url),
        PrByteView{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size});
    prTextBufferReset(&record);
    fetchCachedResponseReset(&entry);
    headersReset(&merged);
    dropRevalidated(fetch);
}

// Receives the response that the exchange's head carried.
void receiveResponse(BuiltinFetch* fetch, PrHttpExchange* exchange) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    fetch->timing.responseStart = prTimeMonotonicMilliseconds();
    fetch->timing.nextHopProtocol = prHttpExchangeProtocol(exchange);
    fetch->status = prHttpExchangeStatus(exchange);
    if (fetch->status == 304 && fetch->revalidated.handle != nullptr) {
        answerRevalidated(fetch, exchange);
        return;
    }
    dropRevalidated(fetch);
    headersClear(&fetch->response);
    for (uint32_t i = 0; i < prHttpExchangeHeaderCount(exchange); ++i) {
        PrHttpHeader header = prHttpExchangeHeader(exchange, i);
        headersAdd(&fetch->response, header.name, header.value);
    }
    if (!admits(fetch))
        return;
    if (fetcher->cache != nullptr && !isSafe(fetch) && fetch->status >= 200 && fetch->status < 400) {
        // A request that may change the resource invalidates what the cache has of it (rfc 9111 section 4.4).
        prStorageRemove(fetcher->cache, prRawStringView(&fetch->url));
        PrStringView location = headersFind(&fetch->response, "location");
        PrUrl* base = nullptr;
        PrUrl* target = nullptr;
        prUrlCreate(prRawStringView(&fetch->url), nullptr, PrCharset_Utf8, &base);
        if (location.data != nullptr && prUrlCreate(location, base, PrCharset_Utf8, &target) == PrStatus_Ok) {
            PrRawString origin = prUrlOrigin(target);
            PrRawString own = prUrlOrigin(base);
            if (prRawStringView(&origin) == prRawStringView(&own)) {
                PrRawString key = prUrlSerialize(target, PrUrlSerialization_ExcludeFragment);
                prStorageRemove(fetcher->cache, prRawStringView(&key));
                prRawStringReset(&key);
            }
            prRawStringReset(&origin);
            prRawStringReset(&own);
        }
        prUrlDestroy(target);
        prUrlDestroy(base);
    }
    RawArray<PrFetchHeader> received{};
    headersView(&fetch->response, &received);
    bool storable = fetcher->cache != nullptr && prRawStringView(&fetch->method) == "GET" &&
        fetchCacheIsStorable(fetch->status, received.data, received.count) &&
        !fetchCacheControlFromHeaders(received.data, received.count).noStore;
    received.reset();
    if (isRedirect(fetch->status) && fetch->redirect != PrFetchRedirect_Manual) {
        // A redirect's body is not read, so the cache stores it without one.
        if (storable && fetch->redirect == PrFetchRedirect_Follow)
            store(fetch, PrByteView{nullptr, 0});
        if (followRedirect(fetch))
            return;
    }
    fetch->type = typeOf(fetch);
    fetch->state = FetchState_Body;
    fetch->storing = storable;
    chooseDecoder(fetch);
    deliver(fetch, PrFetchEvent_Response);
}

// Sends the CORS preflight of the request (fetch/#cors-preflight-fetch), whose response decides whether the request is
// sent.
void preflight(BuiltinFetch* fetch) {
    BuiltinFetcher* fetcher = fetcherOf(fetch);
    RawArray<PrRawString> names{};
    unsafeHeaderNames(&fetch->requestHeaders, &names);
    PrTextBuffer joined{};
    for (uint32_t i = 0; i < names.count; ++i) {
        if (i != 0)
            prTextBufferAppend(&joined, ",");
        prTextBufferAppend(&joined, prRawStringView(&names.data[i]));
        prRawStringReset(&names.data[i]);
    }
    names.reset();
    headersClear(&fetch->sent);
    headersAdd(&fetch->sent, "Accept", "*/*");
    headersAdd(&fetch->sent, "Access-Control-Request-Method", prRawStringView(&fetch->method));
    if (prTextBufferView(&joined).size != 0)
        headersAdd(&fetch->sent, "Access-Control-Request-Headers", prTextBufferView(&joined));
    prTextBufferReset(&joined);
    headersAdd(&fetch->sent, "Origin", serializedOrigin(fetch));
    if (!prRawStringIsNull(&fetch->referrer))
        headersAdd(&fetch->sent, "Referer", prRawStringView(&fetch->referrer));
    if (fetcher->userAgent.size != 0)
        headersAdd(&fetch->sent, "User-Agent", fetcher->userAgent);
    RawArray<PrHttpHeader> headers{};
    headersView(&fetch->sent, &headers);
    PrHttpRequestDesc desc{
        .method = "OPTIONS",
        .url = prRawStringView(&fetch->url),
        .headers = headers.data,
        .headerCount = headers.count,
        .event = exchangeEvent,
        .context = fetch,
    };
    fetch->state = FetchState_Waiting;
    fetch->preflighting = true;
    PrStatus status = prHttpExchangeCreate(fetcher->http, &desc, &fetch->exchange);
    headers.reset();
    if (status != PrStatus_Ok)
        fail(fetch, PrNetError_Protocol);
}

// Returns true if a preflight's Access-Control-Allow-Methods, `list`, allows the request's method: it names the method,
// or it holds `*` and the request has no credentials.
bool allowsMethod(const BuiltinFetch* fetch, const RawArray<PrStringView>& list) {
    bool wildcard = fetch->credentials != PrFetchCredentials_Include;
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.data[i] == prRawStringView(&fetch->method) || (wildcard && list.data[i] == "*"))
            return true;
    }
    return false;
}

// Returns true if a preflight's Access-Control-Allow-Headers, `list`, allows the header `name`: it names the header, or
// it holds `*`, the request has no credentials, and the header is not Authorization
// (fetch/#cors-non-wildcard-request-header-name).
bool allowsHeader(const BuiltinFetch* fetch, const RawArray<PrStringView>& list, PrStringView name) {
    bool wildcard = fetch->credentials != PrFetchCredentials_Include && !prAsciiEqualsFold(name, "authorization");
    for (uint32_t i = 0; i < list.count; ++i) {
        if (prAsciiEqualsFold(list.data[i], name) || (wildcard && list.data[i] == "*"))
            return true;
    }
    return false;
}

// Receives the response to the CORS preflight, and sends the request if the response allows it.
void receivePreflight(BuiltinFetch* fetch, PrHttpExchange* exchange) {
    HeaderList received{};
    for (uint32_t i = 0; i < prHttpExchangeHeaderCount(exchange); ++i) {
        PrHttpHeader header = prHttpExchangeHeader(exchange, i);
        headersAdd(&received, header.name, header.value);
    }
    uint16_t status = prHttpExchangeStatus(exchange);
    RawArray<PrStringView> methods{};
    RawArray<PrStringView> allowedHeaders{};
    bool allowed = prFetchIsOkStatus(status) && passesCors(fetch, &received) &&
        headerTokens(&received, "access-control-allow-methods", &methods) &&
        headerTokens(&received, "access-control-allow-headers", &allowedHeaders);
    if (allowed && !prFetchIsCorsSafelistedMethod(prRawStringView(&fetch->method)))
        allowed = allowsMethod(fetch, methods);
    RawArray<PrRawString> names{};
    unsafeHeaderNames(&fetch->requestHeaders, &names);
    for (uint32_t i = 0; i < names.count; ++i) {
        if (allowed)
            allowed = allowsHeader(fetch, allowedHeaders, prRawStringView(&names.data[i]));
        prRawStringReset(&names.data[i]);
    }
    names.reset();
    methods.reset();
    allowedHeaders.reset();
    headersReset(&received);
    fetch->preflighting = false;
    dropExchange(fetch);
    if (!allowed) {
        fail(fetch, PrNetError_Blocked);
        return;
    }
    PrUrl* current = nullptr;
    prUrlCreate(prRawStringView(&fetch->url), nullptr, PrCharset_Utf8, &current);
    request(fetch, current);
    prUrlDestroy(current);
}

// Fetches the request's current URL, as main fetch does for the URL that the request starts at and for each that a
// redirect leads to (fetch/#main-fetch). The request's mode and the origins of its URLs decide its response tainting. A
// CORS request that is not simple is preflighted.
void fetchCurrent(BuiltinFetch* fetch) {
    PrUrl* current = nullptr;
    prUrlCreate(prRawStringView(&fetch->url), nullptr, PrCharset_Utf8, &current);
    determineReferrer(fetch, current);
    bool basic = fetch->mode == PrFetchMode_Navigate ||
        (fetch->tainting == PrFetchResponseType_Basic && isSameOriginAs(fetch, current));
    if (basic) {
        request(fetch, current);
    } else if (fetch->mode == PrFetchMode_SameOrigin) {
        fail(fetch, PrNetError_Blocked);
    } else if (fetch->mode == PrFetchMode_NoCors) {
        // An opaque response cannot be a redirect that the client follows or sees.
        if (fetch->redirect != PrFetchRedirect_Follow) {
            fail(fetch, PrNetError_Blocked);
        } else {
            fetch->tainting = PrFetchResponseType_Opaque;
            request(fetch, current);
        }
    } else {
        fetch->tainting = PrFetchResponseType_Cors;
        if (needsPreflight(fetch))
            preflight(fetch);
        else
            request(fetch, current);
    }
    prUrlDestroy(current);
}

void exchangeEvent(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event) {
    BuiltinFetch* fetch = static_cast<BuiltinFetch*>(context);
    ++fetch->holds;
    switch (event) {
        case PrHttpEvent_Response:
            if (fetch->preflighting)
                receivePreflight(fetch, exchange);
            else
                receiveResponse(fetch, exchange);
            break;
        case PrHttpEvent_Readable:
            if (fetch->state == FetchState_Body)
                drain(fetch);
            break;
        case PrHttpEvent_End:
            if (fetch->state == FetchState_Body) {
                fetch->exchangeEnded = true;
                drain(fetch);
            }
            break;
        case PrHttpEvent_Fail:
            fail(fetch, prHttpExchangeError(exchange));
            break;
    }
    release(fetch);
}

// The MIME type of a file by its extension, compared without regard to ASCII case.
struct FileType {
    PrStringView extension;
    PrStringView type;
};

constexpr FileType kFileTypes[] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"xhtml", "application/xhtml+xml"},
    {"xht", "application/xhtml+xml"},
    {"xml", "application/xml"},
    {"svg", "image/svg+xml"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"mjs", "text/javascript"},
    {"json", "application/json"},
    {"md", "text/markdown"},
    {"markdown", "text/markdown"},
    {"txt", "text/plain"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
};

// Returns the MIME type of the file at `path` by its extension, or the null view for an extension that it does not
// know.
PrStringView fileTypeOf(PrStringView path) {
    size_t dot = prStringFindLastByte(path, '.');
    size_t slash = prStringFindLastByte(path, '/');
    if (dot == path.size || (slash != path.size && slash > dot))
        return PrStringView{nullptr, 0};
    PrStringView extension = prStringSlice(path, dot + 1, path.size - dot - 1);
    for (const FileType& entry : kFileTypes) {
        if (prAsciiEqualsFold(extension, entry.extension))
            return entry.type;
    }
    return PrStringView{nullptr, 0};
}

// Reads the file that the file: URL `url` names into the fetch's body, and returns its MIME type, or returns false if
// the file cannot be read. The path is the URL's percent-decoded path, without the slash before a Windows drive letter.
bool readFile(BuiltinFetch* fetch, PrStringView url, PrStringView* type) {
    PrUrl* parsed = nullptr;
    if (prUrlCreate(url, nullptr, PrCharset_Utf8, &parsed) != PrStatus_Ok)
        return false;
    PrStringView encoded = prUrlPath(parsed);
    PrTextBuffer path{};
    size_t size = prUrlPercentDecode(encoded, nullptr, 0);
    prTextBufferResize(&path, uint32_t(size));
    prUrlPercentDecode(encoded, path.data, size);
    prUrlDestroy(parsed);
    PrStringView decoded = prTextBufferView(&path);
    if (decoded.size >= 3 && decoded.data[0] == '/' && prAsciiIsLetter(decoded.data[1]) && decoded.data[2] == ':')
        decoded = prStringSlice(decoded, 1, decoded.size - 1);
    PrFileMapping mapping{};
    bool read = prStringFindByte(decoded, '\0') == decoded.size && prFileMap(decoded, &mapping);
    if (read)
        fetch->content.append(mapping.data, uint32_t(mapping.size));
    prFileUnmap(&mapping);
    *type = fileTypeOf(decoded);
    prTextBufferReset(&path);
    return read;
}

// Answers a data: URL (fetch/#data-url-processor), a file: URL or about:blank with a response that the fetcher makes.
void answerLocally(BuiltinFetch* fetch) {
    PrStringView url = prRawStringView(&fetch->url);
    fetch->state = FetchState_Body;
    PrRawString type = prRawStringNull();
    if (prAsciiStartsWithFold(url, "file:")) {
        PrStringView fileType{nullptr, 0};
        if (!readFile(fetch, url, &fileType)) {
            fail(fetch, PrNetError_Io);
            return;
        }
        if (fileType.data != nullptr)
            type = prRawStringCopy(fileType);
    } else if (prFetchBlobIsUrl(url)) {
        // fetch/#scheme-fetch: a GET of a blob that the store has, and otherwise a network error.
        const PrFetchBlob* blob = fetch->blob;
        if (blob == nullptr || prRawStringView(&fetch->method) != "GET") {
            fail(fetch, PrNetError_Protocol);
            return;
        }
        PrByteView bytes = prFetchBlobBytes(blob);
        fetch->content.append(bytes.data, uint32_t(bytes.size));
        PrTextBuffer length{};
        prTextBufferAppendU64(&length, bytes.size, 10);
        headersAdd(&fetch->response, "Content-Length", prTextBufferView(&length));
        prTextBufferReset(&length);
        type = prRawStringCopy(prFetchBlobType(blob));
    } else if (prAsciiStartsWithFold(url, "data:")) {
        PrRawString body = prRawStringNull();
        if (prUrlProcessData(url, &body, &type) != PrStatus_Ok) {
            prRawStringReset(&body);
            prRawStringReset(&type);
            fail(fetch, PrNetError_Protocol);
            return;
        }
        PrStringView bytes = prRawStringView(&body);
        fetch->content.append(reinterpret_cast<const uint8_t*>(bytes.data), uint32_t(bytes.size));
        prRawStringReset(&body);
    } else {
        type = prRawStringCopy("text/html;charset=utf-8");
    }
    if (!prRawStringIsNull(&type))
        headersAdd(&fetch->response, "Content-Type", prRawStringView(&type));
    prRawStringReset(&type);
    fetch->status = 200;
    deliver(fetch, PrFetchEvent_Response);
    if (fetch->content.count != 0)
        deliver(fetch, PrFetchEvent_Readable);
    if (!fetch->dead) {
        fetch->state = FetchState_Done;
        deliver(fetch, PrFetchEvent_End);
    }
}

// Starts the fetch, from a task, so that its events come after the call that made it.
void start(void* PR_NULLABLE context) {
    BuiltinFetch* fetch = static_cast<BuiltinFetch*>(context);
    if (!fetch->dead) {
        if (fetch->local)
            answerLocally(fetch);
        else if (fetcherOf(fetch)->http != nullptr && (prAsciiStartsWithFold(prRawStringView(&fetch->url), "http:") ||
                prAsciiStartsWithFold(prRawStringView(&fetch->url), "https:")))
            fetchCurrent(fetch);
        else
            fail(fetch, PrNetError_Protocol);
    }
    release(fetch);
}

// Takes more of the exchange's content, from a task, once the client read enough to make room.
void resume(void* PR_NULLABLE context) {
    BuiltinFetch* fetch = static_cast<BuiltinFetch*>(context);
    fetch->resumePosted = false;
    if (!fetch->dead && fetch->state == FetchState_Body && fetch->exchange != nullptr)
        drain(fetch);
    release(fetch);
}

// Fetcher functions

void destroyFetcher(PrFetchNetwork* base) {
    BuiltinFetcher* fetcher = static_cast<BuiltinFetcher*>(base);
    prRawStringReset(&fetcher->userAgentStorage);
    prFetchBlobStoreDestroy(fetcher->blobStore);
    prMemoryFree(fetcher);
}

PrStatus createFetch(PrFetchNetwork* base, const PrFetchRequestDesc* desc, PrFetchFn PR_NULLABLE event,
        void* PR_NULLABLE context, PrFetch* PR_NULLABLE* PR_NONNULL out) {
    PrUrl* url = nullptr;
    if (prUrlCreate(desc->url, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return PrStatus_InvalidData;
    PrUrl* referrer = nullptr;
    if (desc->referrer.data != nullptr &&
            prUrlCreate(desc->referrer, nullptr, PrCharset_Utf8, &referrer) != PrStatus_Ok) {
        prUrlDestroy(url);
        return PrStatus_InvalidData;
    }
    BuiltinFetch* fetch = allocZeroed<BuiltinFetch>();
    fetch->timing.startTime = prTimeMonotonicMilliseconds();
    fetch->timing.postRedirectStart = fetch->timing.startTime;
    fetch->timing.timingAllowPassed = true;
    if (referrer != nullptr)
        fetch->referrer = prUrlSerialize(referrer, PrUrlSerialization_Whole);
    prUrlDestroy(referrer);
    fetch->origin = prRawStringCopy(desc->origin.data != nullptr ? desc->origin : PrStringView{"null"});
    fetch->referrerPolicy = desc->referrerPolicy;
    fetch->mode = desc->mode;
    fetch->credentials = desc->credentials;
    fetch->fetcher = base;
    fetch->event = event;
    fetch->context = context;
    PrStringView scheme = prUrlScheme(url);
    fetch->local = scheme == "data" || scheme == "file" || scheme == "blob" || (scheme == "about" && prUrlPath(url) == "blank");
    fetch->url = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(url);
    if (prFetchBlobIsUrl(prRawStringView(&fetch->url)))
        fetch->blob = prFetchBlobStoreResolve(base->blobStore, prRawStringView(&fetch->url));
    fetch->method = prRawStringCopy(desc->method.data != nullptr ? desc->method : PrStringView{"GET"});
    for (uint32_t i = 0; i < desc->headerCount; ++i)
        headersAdd(&fetch->requestHeaders, desc->headers[i].name, desc->headers[i].value);
    fetch->body = prRawStringCopy(PrStringView{reinterpret_cast<const char*>(desc->body.data), desc->body.size});
    fetch->destination = desc->destination;
    fetch->redirect = desc->redirect;
    fetch->holds = 1;
    prNetPost(base->net, start, fetch);
    *out = fetch;
    return PrStatus_Ok;
}

void destroyFetch(PrFetchNetwork*, PrFetch* handle) {
    BuiltinFetch* fetch = static_cast<BuiltinFetch*>(handle);
    PR_ASSERT(!fetch->dead);
    fetch->dead = true;
    dropExchange(fetch);
    dropRevalidated(fetch);
    if (fetch->holds == 0)
        freeFetch(fetch);
}

uint16_t fetchStatus(const PrFetchNetwork*, const PrFetch* handle) {
    const BuiltinFetch* fetch = static_cast<const BuiltinFetch*>(handle);
    return fetch->state == FetchState_Failed || fetch->state < FetchState_Body ? 0 : fetch->status;
}

PrStringView fetchUrl(const PrFetchNetwork*, const PrFetch* handle) {
    return prRawStringView(&static_cast<const BuiltinFetch*>(handle)->url);
}

PrFetchResponseType fetchResponseType(const PrFetchNetwork*, const PrFetch* handle) {
    const BuiltinFetch* fetch = static_cast<const BuiltinFetch*>(handle);
    bool received = fetch->state != FetchState_Failed && fetch->state >= FetchState_Body;
    return received ? fetch->type : PrFetchResponseType_Basic;
}

uint32_t fetchHeaderCount(const PrFetchNetwork*, const PrFetch* handle) {
    const BuiltinFetch* fetch = static_cast<const BuiltinFetch*>(handle);
    return fetch->state == FetchState_Failed || fetch->state < FetchState_Body ? 0 : fetch->response.names.count;
}

PrFetchHeader fetchHeader(const PrFetchNetwork*, const PrFetch* handle, uint32_t index) {
    const BuiltinFetch* fetch = static_cast<const BuiltinFetch*>(handle);
    return PrFetchHeader{
        prRawStringView(&fetch->response.names.data[index]),
        prRawStringView(&fetch->response.values.data[index]),
    };
}

size_t fetchRead(PrFetchNetwork* base, PrFetch* handle, uint8_t* output, size_t capacity) {
    BuiltinFetch* fetch = static_cast<BuiltinFetch*>(handle);
    uint32_t available = fetch->content.count - fetch->contentStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    prMemCopy(output, fetch->content.data + fetch->contentStart, taken);
    fetch->contentStart += taken;
    if (fetch->contentStart == fetch->content.count) {
        fetch->content.clear();
        fetch->contentStart = 0;
    }
    if (fetch->stalled && fetch->state == FetchState_Body && !fetch->resumePosted) {
        fetch->resumePosted = true;
        ++fetch->holds;
        prNetPost(base->net, resume, fetch);
    }
    return taken;
}

PrNetError fetchError(const PrFetchNetwork*, const PrFetch* handle) {
    return static_cast<const BuiltinFetch*>(handle)->error;
}

PrStringView fetchSentReferrer(const PrFetchNetwork*, const PrFetch* handle) {
    const BuiltinFetch* fetch = static_cast<const BuiltinFetch*>(handle);
    return prRawStringIsNull(&fetch->referrer) ? PrStringView{nullptr, 0} : prRawStringView(&fetch->referrer);
}

PrFetchTiming fetchTiming(const PrFetchNetwork*, const PrFetch* handle) {
    return static_cast<const BuiltinFetch*>(handle)->timing;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFetchNetwork* prFetchBuiltinCreate(const PrFetchNetworkDesc* desc) {
    BuiltinFetcher* fetcher = allocZeroed<BuiltinFetcher>();
    fetcher->destroy = destroyFetcher;
    fetcher->fetchCreate = createFetch;
    fetcher->fetchDestroy = destroyFetch;
    fetcher->fetchStatus = fetchStatus;
    fetcher->fetchUrl = fetchUrl;
    fetcher->fetchResponseType = fetchResponseType;
    fetcher->fetchHeaderCount = fetchHeaderCount;
    fetcher->fetchHeader = fetchHeader;
    fetcher->fetchRead = fetchRead;
    fetcher->fetchError = fetchError;
    fetcher->fetchTiming = fetchTiming;
    fetcher->fetchSentReferrer = fetchSentReferrer;
    PR_ASSERT(desc->http == nullptr || desc->http->net == desc->net);
    fetcher->net = desc->net;
    fetcher->blobStore = prFetchBlobStoreCreate();
    fetcher->http = desc->http;
    fetcher->compress = desc->compress;
    fetcher->cache = desc->cache;
    PrStringView userAgent = desc->userAgent.data == nullptr ? kPrFetchDefaultUserAgent : desc->userAgent;
    fetcher->userAgentStorage = prRawStringCopy(userAgent);
    fetcher->userAgent = prRawStringView(&fetcher->userAgentStorage);
    return fetcher;
}
