// Fetches: resources requested by URL, as the web's documents and APIs request them.

#include "pr/fetch.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/mime.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#if defined(PR_FETCH_HAS_BUILTIN)
#include "pr/fetch_builtin.h"
#endif

namespace pr_impl {
namespace {

constexpr PrStringView kBackendNames[PrFetchNetworkBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrFetchNetworkBackend kBuilt[] = {
#if defined(PR_FETCH_HAS_BUILTIN)
    PrFetchNetworkBackend_Builtin,
#endif
    PrFetchNetworkBackend_Default,
};

bool isToken(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!asciiIsHttpToken(text.data[i]))
            return false;
    }
    return true;
}

// Returns true if `value` has a CORS-unsafe request-header byte (fetch/#cors-unsafe-request-header-byte).
bool hasUnsafeHeaderByte(PrStringView value) {
    for (size_t i = 0; i < value.size; ++i) {
        uint8_t c = uint8_t(value.data[i]);
        if ((c < 0x20 && c != '\t') || c == '"' || c == '(' || c == ')' || c == ':' || c == '<' || c == '>' ||
                c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '{' || c == '}' || c == 0x7f)
            return true;
    }
    return false;
}

// Returns true if a Range header's value is a simple range (fetch/#simple-range-header-value): one range of bytes, from
// a first byte to an optional last byte that does not precede it.
bool isSimpleRange(PrStringView value) {
    if (!stringStartsWith(value, "bytes="))
        return false;
    size_t at = 6;
    uint64_t first = 0;
    size_t start = at;
    while (at < value.size && asciiIsDigit(value.data[at]) && at - start < 19)
        first = first * 10 + uint64_t(value.data[at++] - '0');
    if (at == start || at == value.size || value.data[at] != '-')
        return false;
    ++at;
    start = at;
    uint64_t last = 0;
    while (at < value.size && asciiIsDigit(value.data[at]) && at - start < 19)
        last = last * 10 + uint64_t(value.data[at++] - '0');
    return at == value.size && (at == start || first <= last);
}

// Returns true if `visit` returns true for an item of the comma-separated list `value`, as
// fetch/#header-value-get-decode-and-split splits it: each without the HTTP tab or space around it, and with each
// quoted string as its contents, unescaped. A comma inside a quoted string does not separate items.
template <typename Visit>
bool anyListItem(PrStringView value, Visit visit) {
    TextBuffer item;
    bool found = false;
    for (size_t i = 0; !found && i <= value.size; ++i) {
        if (i < value.size && value.data[i] == '"') {
            // fetch/#collect-an-http-quoted-string, with only its contents kept.
            for (++i; i < value.size && value.data[i] != '"'; ++i) {
                if (value.data[i] == '\\' && i + 1 < value.size)
                    ++i;
                textAppendChar(&item, value.data[i]);
            }
            continue;
        }
        if (i < value.size && value.data[i] != ',') {
            textAppendChar(&item, value.data[i]);
            continue;
        }
        found = visit(stringTrim(textView(&item), asciiIsTabOrSpace));
        textClear(&item);
    }
    textReset(&item);
    return found;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

PrStringView prFetchInitiatorName(PrFetchInitiator initiator) {
    constexpr PrStringView kNames[] = {
        "other", "audio", "beacon", "css", "embed", "fetch", "frame", "iframe", "image", "img", "input", "link",
        "navigation", "object", "script", "track", "video", "xmlhttprequest",
    };
    PR_ASSERT(initiator <= PrFetchInitiator_XmlHttpRequest);
    return kNames[initiator];
}

PrFetchNetworkBackend prFetchNetworkBackendFromName(PrStringView name) {
    for (PrFetchNetworkBackend backend = 0; backend < PrFetchNetworkBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrFetchNetworkBackend_Count;
}

PrStringView prFetchNetworkBackendName(PrFetchNetworkBackend backend) {
    return backend < PrFetchNetworkBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prFetchNetworkBackendIsBuilt(PrFetchNetworkBackend backend) {
    for (PrFetchNetworkBackend built : kBuilt) {
        if (built != PrFetchNetworkBackend_Default && (built == backend || backend == PrFetchNetworkBackend_Default))
            return true;
    }
    return false;
}

PrFetchNetwork* PR_NULLABLE prFetchNetworkCreate(PrFetchNetworkBackend backend, const PrFetchNetworkDesc* desc) {
    switch (backend == PrFetchNetworkBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_FETCH_HAS_BUILTIN)
        case PrFetchNetworkBackend_Builtin: return prFetchBuiltinCreate(desc);
#endif
        default: return nullptr;
    }
}

void prFetchNetworkDestroy(PrFetchNetwork* PR_NULLABLE fetcher) {
    if (fetcher != nullptr)
        fetcher->destroy(fetcher);
}

PrStringView prFetchNetworkUserAgent(const PrFetchNetwork* fetcher) {
    return fetcher->userAgent;
}

PrStatus prFetchCreate(PrFetchNetwork* fetcher, const PrFetchRequestDesc* desc, PrFetchFn PR_NULLABLE event,
        void* PR_NULLABLE context, PrFetch* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(desc->headers != nullptr || desc->headerCount == 0);
    PR_ASSERT(desc->body.data != nullptr || desc->body.size == 0);
    PR_ASSERT(desc->destination <= PrFetchDestination_Object && desc->redirect <= PrFetchRedirect_Manual);
    PR_ASSERT(desc->mode <= PrFetchMode_Navigate && desc->credentials <= PrFetchCredentials_Include);
    PR_ASSERT(desc->referrerPolicy <= PrFetchReferrerPolicy_UnsafeUrl);
    *out = nullptr;
    PrStatus status = fetcher->fetchCreate(fetcher, desc, event, context, out);
    PR_ASSERT(status != PrStatus_Ok || ((*out) != nullptr && (*out)->fetcher == fetcher));
    return status;
}

void prFetchDestroy(PrFetch* PR_NULLABLE fetch) {
    if (fetch != nullptr)
        fetch->fetcher->fetchDestroy(fetch->fetcher, fetch);
}

uint16_t prFetchStatus(const PrFetch* fetch) {
    return fetch->fetcher->fetchStatus(fetch->fetcher, fetch);
}

PrStringView prFetchUrl(const PrFetch* fetch) {
    return fetch->fetcher->fetchUrl(fetch->fetcher, fetch);
}

PrFetchResponseType prFetchResponseType(const PrFetch* fetch) {
    if (fetch->fetcher->fetchResponseType == nullptr)
        return PrFetchResponseType_Basic;
    return fetch->fetcher->fetchResponseType(fetch->fetcher, fetch);
}

uint32_t prFetchHeaderCount(const PrFetch* fetch) {
    return fetch->fetcher->fetchHeaderCount(fetch->fetcher, fetch);
}

PrFetchHeader prFetchHeader(const PrFetch* fetch, uint32_t index) {
    PR_ASSERT(index < prFetchHeaderCount(fetch));
    return fetch->fetcher->fetchHeader(fetch->fetcher, fetch, index);
}

PrStringView prFetchHeaderValue(const PrFetch* fetch, PrStringView name) {
    uint32_t count = prFetchHeaderCount(fetch);
    for (uint32_t i = 0; i < count; ++i) {
        PrFetchHeader header = prFetchHeader(fetch, i);
        if (asciiEqualsFold(header.name, name))
            return header.value;
    }
    return PrStringView{nullptr, 0};
}

size_t prFetchRead(PrFetch* fetch, uint8_t* PR_NULLABLE output, size_t capacity) {
    PR_ASSERT(output != nullptr || capacity == 0);
    if (capacity == 0)
        return 0;
    return fetch->fetcher->fetchRead(fetch->fetcher, fetch, output, capacity);
}

PrNetError prFetchError(const PrFetch* fetch) {
    return fetch->fetcher->fetchError(fetch->fetcher, fetch);
}

PrStringView prFetchSentReferrer(const PrFetch* fetch) {
    if (fetch->fetcher->fetchSentReferrer == nullptr)
        return PrStringView{nullptr, 0};
    return fetch->fetcher->fetchSentReferrer(fetch->fetcher, fetch);
}

PrFetchTiming prFetchTiming(const PrFetch* fetch) {
    if (fetch->fetcher->fetchTiming == nullptr)
        return PrFetchTiming{};
    return fetch->fetcher->fetchTiming(fetch->fetcher, fetch);
}

// ---------------------------------------------------------------------------------------------------------------------
// Whole responses

bool prFetchIsOkStatus(uint16_t status) {
    return status >= 200 && status <= 299;
}

// ---------------------------------------------------------------------------------------------------------------------
// Methods and headers

bool prFetchIsMethod(PrStringView method) {
    return isToken(method);
}

bool prFetchIsForbiddenMethod(PrStringView method) {
    return asciiEqualsFold(method, "CONNECT") || asciiEqualsFold(method, "TRACE") || asciiEqualsFold(method, "TRACK");
}

bool prFetchIsCorsSafelistedMethod(PrStringView method) {
    return method == "GET" || method == "HEAD" || method == "POST";
}

PrStringView prFetchNormalizeMethod(PrStringView method) {
    constexpr PrStringView kNormalized[] = {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"};
    for (PrStringView normalized : kNormalized) {
        if (asciiEqualsFold(method, normalized))
            return normalized;
    }
    return method;
}

bool prFetchIsHeaderName(PrStringView name) {
    return isToken(name);
}

bool prFetchIsHeaderValue(PrStringView value) {
    if (value.size != 0 && (asciiIsTabOrSpace(value.data[0]) || asciiIsTabOrSpace(value.data[value.size - 1])))
        return false;
    for (size_t i = 0; i < value.size; ++i) {
        char c = value.data[i];
        if (c == '\0' || c == '\n' || c == '\r')
            return false;
    }
    return true;
}

PrStringView prFetchNormalizeHeaderValue(PrStringView value) {
    return stringTrim(value, asciiIsHttpSpace);
}

bool prFetchIsForbiddenRequestHeader(PrStringView name, PrStringView value) {
    constexpr PrStringView kForbidden[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
        "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin",
        "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via",
    };
    for (PrStringView forbidden : kForbidden) {
        if (asciiEqualsFold(name, forbidden))
            return true;
    }
    if (asciiStartsWithFold(name, "proxy-") || asciiStartsWithFold(name, "sec-"))
        return true;
    if (asciiEqualsFold(name, "x-http-method") || asciiEqualsFold(name, "x-http-method-override") ||
            asciiEqualsFold(name, "x-method-override"))
        return anyListItem(value, prFetchIsForbiddenMethod);
    return false;
}

bool prFetchIsForbiddenResponseHeaderName(PrStringView name) {
    return asciiEqualsFold(name, "set-cookie") || asciiEqualsFold(name, "set-cookie2");
}

bool prFetchIsCorsSafelistedRequestHeader(PrStringView name, PrStringView value) {
    constexpr size_t kSafelistedMaxBytes = 128;
    if (value.size > kSafelistedMaxBytes)
        return false;
    if (asciiEqualsFold(name, "accept"))
        return !hasUnsafeHeaderByte(value);
    if (asciiEqualsFold(name, "accept-language") || asciiEqualsFold(name, "content-language")) {
        for (size_t i = 0; i < value.size; ++i) {
            char c = value.data[i];
            if (!asciiIsDigit(c) && !asciiIsLetter(c) && c != ' ' && c != '*' && c != ',' && c != '-' && c != '.' &&
                    c != ';' && c != '=')
                return false;
        }
        return true;
    }
    if (asciiEqualsFold(name, "content-type")) {
        if (hasUnsafeHeaderByte(value))
            return false;
        PrStringView essence = prMimeTypeEssence(value);
        return asciiEqualsFold(essence, "application/x-www-form-urlencoded") ||
            asciiEqualsFold(essence, "multipart/form-data") || asciiEqualsFold(essence, "text/plain");
    }
    if (asciiEqualsFold(name, "range"))
        return isSimpleRange(value);
    return false;
}

bool prFetchIsNoCorsSafelistedRequestHeaderName(PrStringView name) {
    return asciiEqualsFold(name, "accept") || asciiEqualsFold(name, "accept-language") ||
        asciiEqualsFold(name, "content-language") || asciiEqualsFold(name, "content-type");
}

bool prFetchIsNoCorsSafelistedRequestHeader(PrStringView name, PrStringView value) {
    return prFetchIsNoCorsSafelistedRequestHeaderName(name) && prFetchIsCorsSafelistedRequestHeader(name, value);
}

bool prFetchIsCorsExposedHeader(PrStringView name, PrStringView exposed, PrFetchCredentials credentials) {
    constexpr PrStringView kSafelisted[] = {
        "cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma",
    };
    for (PrStringView safelisted : kSafelisted) {
        if (asciiEqualsFold(name, safelisted))
            return true;
    }
    if (prFetchIsForbiddenResponseHeaderName(name))
        return false;
    return anyListItem(exposed, [&](PrStringView item) {
        return asciiEqualsFold(item, name) || (item == "*" && credentials != PrFetchCredentials_Include);
    });
}
