// Fetches: resources requested by URL, as the web's documents and APIs request them (fetch/#fetching).
//
// A fetch follows the contract of its fetcher's network (pr/net.h): its callbacks run on the network's loop, and it is
// used there. A fetcher is an interface, so that a host can bring the fetching of a platform that has its own, as a
// browser does.
//
// fetch - https://fetch.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/http.h"
#include "pr/net.h"
#include "pr/status.h"
#include "pr/storage.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of fetches, created by prFetchNetworkCreate or by its backend's own constructor.
typedef struct PrFetchNetwork PrFetchNetwork;

// One fetch: a request, and the response that it receives.
typedef struct PrFetch PrFetch;

// A header (fetch/#concept-header): its name, matched without regard to ASCII case, and its value.
typedef struct PrFetchHeader {
    PrStringView name;
    PrStringView value;
} PrFetchHeader;

// What a fetched resource is for (fetch/#concept-request-destination).
enum {
    PrFetchDestination_Empty,   // a script's fetch() or XMLHttpRequest
    PrFetchDestination_Document,
    PrFetchDestination_Script,
    PrFetchDestination_Style,
    PrFetchDestination_Image,
    PrFetchDestination_Font,
    PrFetchDestination_Media,   // audio or video
    PrFetchDestination_Object,
};
typedef uint8_t PrFetchDestination;

// What a fetch does with a redirect (fetch/#concept-request-redirect-mode).
enum {
    PrFetchRedirect_Follow,     // fetches what the redirect names
    PrFetchRedirect_Error,      // fails
    PrFetchRedirect_Manual,     // receives the redirect as the response
};
typedef uint8_t PrFetchRedirect;

// How a request treats another origin (fetch/#concept-request-mode).
enum {
    PrFetchMode_NoCors,         // a response from another origin is opaque
    PrFetchMode_Cors,           // a response from another origin passes the CORS check, or the fetch fails
    PrFetchMode_SameOrigin,     // a request to another origin fails
    PrFetchMode_Navigate,       // a navigation's document, which may come from any origin
};
typedef uint8_t PrFetchMode;

// To which origins a request sends credentials, and from which it accepts them
// (fetch/#concept-request-credentials-mode).
enum {
    PrFetchCredentials_SameOrigin,  // to and from the request's own origin
    PrFetchCredentials_Omit,        // to and from none
    PrFetchCredentials_Include,     // to and from every origin; a CORS response must allow them
};
typedef uint8_t PrFetchCredentials;

// How much of its referrer a request sends (referrer-policy/#referrer-policies).
enum {
    PrFetchReferrerPolicy_Default,  // the empty policy, which acts as StrictOriginWhenCrossOrigin
    PrFetchReferrerPolicy_NoReferrer,
    PrFetchReferrerPolicy_NoReferrerWhenDowngrade,
    PrFetchReferrerPolicy_SameOrigin,
    PrFetchReferrerPolicy_Origin,
    PrFetchReferrerPolicy_StrictOrigin,
    PrFetchReferrerPolicy_OriginWhenCrossOrigin,
    PrFetchReferrerPolicy_StrictOriginWhenCrossOrigin,
    PrFetchReferrerPolicy_UnsafeUrl,
};
typedef uint8_t PrFetchReferrerPolicy;

// What a response reveals of itself (fetch/#concept-response-type). The fetch reads the whole response whatever its
// type. A client that hands the response to a script filters it as its type says (fetch/#concept-filtered-response).
enum {
    PrFetchResponseType_Basic,          // from the request's own origin, or a navigation's
    PrFetchResponseType_Cors,           // from another origin, which passed the CORS check
    PrFetchResponseType_Opaque,         // from another origin, for a no-cors request
    PrFetchResponseType_OpaqueRedirect, // a redirect that a request whose redirect mode is Manual received
};
typedef uint8_t PrFetchResponseType;

// What happened to a fetch. Each event is delivered once, except Readable.
enum {
    PrFetchEvent_Response,      // the response's status and headers arrived
    PrFetchEvent_Readable,      // bytes of the body arrived while none were waiting to be read
    PrFetchEvent_End,           // the body is complete; the bytes that arrived before can still be read
    PrFetchEvent_Fail,          // a network error (fetch/#concept-network-error), whose reason prFetchError gives
};
typedef uint8_t PrFetchEvent;

// Called on the network's loop with each event of `fetch`.
typedef void (*PrFetchFn)(void* PR_NULLABLE context, PrFetch* fetch, PrFetchEvent event);

// A request (fetch/#concept-request).
typedef struct PrFetchRequestDesc {
    PrStringView url;           // an absolute URL
    PrStringView method;        // a method, such as "POST", or the null view for GET

    // The request's headers. The fetcher adds the headers that the request leaves out, such as User-Agent and Accept.
    const PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;

    PrByteView body;
    PrFetchDestination destination;
    PrFetchMode mode;
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;

    // The serialized origin of the request (fetch/#concept-request-origin), such as "https://example.com", or "null"
    // for an opaque origin. The null view stands for a request without a client, such as a navigation that the user
    // starts, and is an opaque origin too.
    PrStringView origin;

    // The URL of the document or worker that makes the request (fetch/#concept-request-referrer), or the null view for
    // no referrer. The policy decides how much of it the request sends.
    PrStringView referrer;
    PrFetchReferrerPolicy referrerPolicy;

    // Called with the fetch's events, with its context.
    PrFetchFn PR_NULLABLE event;
    void* PR_NULLABLE context;
} PrFetchRequestDesc;

// The start of every fetch's state. `fetcher` is the source that made the fetch.
struct PrFetch {
    PrFetchNetwork* fetcher;
};

// The start of every source's state: the function table of its backend, then its network and its default User-Agent.
// Each member of the table implements the public function of the same name. A null `fetchResponseType` means that every
// response is Basic. Callers use the functions below, never these members.
struct PrFetchNetwork {
    void (*destroy)(PrFetchNetwork* fetcher);
    PrStatus (*fetchCreate)(PrFetchNetwork* fetcher, const PrFetchRequestDesc* desc, PrFetch* PR_NULLABLE* PR_NONNULL out);
    void (*fetchDestroy)(PrFetchNetwork* fetcher, PrFetch* fetch);
    uint16_t (*fetchStatus)(const PrFetchNetwork* fetcher, const PrFetch* fetch);
    PrStringView (*fetchUrl)(const PrFetchNetwork* fetcher, const PrFetch* fetch);
    PrFetchResponseType (*PR_NULLABLE fetchResponseType)(const PrFetchNetwork* fetcher, const PrFetch* fetch);
    uint32_t (*fetchHeaderCount)(const PrFetchNetwork* fetcher, const PrFetch* fetch);
    PrFetchHeader (*fetchHeader)(const PrFetchNetwork* fetcher, const PrFetch* fetch, uint32_t index);
    size_t (*fetchRead)(PrFetchNetwork* fetcher, PrFetch* fetch, uint8_t* output, size_t capacity);
    PrNetError (*fetchError)(const PrFetchNetwork* fetcher, const PrFetch* fetch);

    PrNet* net;                 // the network whose loop runs the fetches' callbacks

    // The default User-Agent value (fetch/#default-user-agent-value), which the source holds unchanged for its life.
    PrStringView userAgent;
};

// The library's backends of fetchers, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrFetchNetworkBackend_Default,
    PrFetchNetworkBackend_Builtin,
    PrFetchNetworkBackend_Count,
};
typedef uint8_t PrFetchNetworkBackend;

// Returns the backend that `name` names, as prFetchNetworkBackendName spells it, ignoring case, or Count if it names
// none.
PrFetchNetworkBackend prFetchNetworkBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prFetchNetworkBackendName(PrFetchNetworkBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prFetchNetworkBackendIsBuilt(PrFetchNetworkBackend backend);

// The engine's default User-Agent value (fetch/#default-user-agent-value), which a fetcher sends if its host gives
// none.
static const char kPrFetchDefaultUserAgent[] = "Mozilla/5.0 (compatible; Prax)";

// What a fetcher fetches with.
typedef struct PrFetchNetworkDesc {
    PrNet* net;                 // the network whose loop runs the fetches

    // The source of HTTP exchanges, on the same network. Without it, an http: or https: URL is a network error.
    PrHttp* PR_NULLABLE http;

    // The codec that decodes content codings. Without it, a request asks for none, and a body arrives as it was sent.
    PrCompressCodec* PR_NULLABLE compress;

    // The store of the HTTP cache (rfc 9111), which the fetcher reads and writes as a private cache does. Without it,
    // nothing is cached.
    PrStorage* PR_NULLABLE cache;

    // The User-Agent of each request that has none of its own, which the view is copied from. The null view stands for
    // kPrFetchDefaultUserAgent, and the empty view sends none.
    PrStringView userAgent;
} PrFetchNetworkDesc;

// Creates a fetcher by the backend, as `desc` describes. A backend's own header documents what it fetches. Returns null
// if the build does not have the backend. `desc`'s HTTP source, codec and store must outlive the fetcher.
PrFetchNetwork* PR_NULLABLE prFetchNetworkCreate(PrFetchNetworkBackend backend, const PrFetchNetworkDesc* desc);

// Frees the source. Its fetches must be destroyed first. Called on the network's loop, outside the callbacks of its
// fetches.
void prFetchNetworkDestroy(PrFetchNetwork* PR_NULLABLE fetcher);

// Returns the source's default User-Agent value (fetch/#default-user-agent-value): what a request that has no
// User-Agent of its own sends, and what navigator.userAgent gives a page that the source fetches for. The source holds
// it unchanged for its life, so any thread may read it. The view is valid while the source lives.
PrStringView prFetchNetworkUserAgent(const PrFetchNetwork* fetcher);

// Starts fetching what `desc` requests, and writes the fetch to `*out`. The fetch delivers Response once the response
// arrives, then its body, or Fail. Its first event comes after this call. Returns InvalidData for a URL or a referrer
// that does not parse. On failure, `*out` is null.
PrStatus prFetchCreate(PrFetchNetwork* fetcher, const PrFetchRequestDesc* desc, PrFetch* PR_NULLABLE* PR_NONNULL out);

// Abandons the fetch if it has not ended, and frees it. Its callback is not called again. A fetch may be destroyed
// during its own callback.
void prFetchDestroy(PrFetch* PR_NULLABLE fetch);

// Returns the response's status (fetch/#concept-response-status), or zero before Response and for a network error.
uint16_t prFetchStatus(const PrFetch* fetch);

// Returns the URL of the response, after the redirects that the fetch followed, without its fragment. The view is valid
// while the fetch lives.
PrStringView prFetchUrl(const PrFetch* fetch);

// Returns the response's type, which is Basic before Response and for a network error.
PrFetchResponseType prFetchResponseType(const PrFetch* fetch);

// Return the response's headers, or none before Response. Each view stays valid while the fetch lives.
uint32_t prFetchHeaderCount(const PrFetch* fetch);
PrFetchHeader prFetchHeader(const PrFetch* fetch, uint32_t index);

// Returns the value of the response's first header named `name`, or the null view if it has none.
PrStringView prFetchHeaderValue(const PrFetch* fetch, PrStringView name);

// Copies up to `capacity` bytes of the body that arrived into `output`, and returns how many it copied. The body is
// decoded from its content codings. Returns zero once every byte that arrived has been read. Readable is delivered
// again only when more bytes arrive after that.
size_t prFetchRead(PrFetch* fetch, uint8_t* PR_NULLABLE output, size_t capacity);

// Returns why the fetch failed, or None if it has not.
PrNetError prFetchError(const PrFetch* fetch);

// ---------------------------------------------------------------------------------------------------------------------
// Fetches of a client
//
// A request that a script makes through its client (fetch/#concept-request-client), the environment of a document or a
// worker, as fetch() and XMLHttpRequest make one. The client completes the request: it parses the URL against its base
// URL, gives the request its origin and its referrer, and fetches it with its fetcher. The client reads the whole
// response before it passes the response on.

// A request that a script makes through its client.
typedef struct PrFetchClientRequest {
    PrStringView url;                           // a URL, which parses against the client's base URL
    PrStringView method;                        // a normalized method that is not forbidden, or the null view for GET
    const PrFetchHeader* PR_NULLABLE headers;   // the headers that the script set, which its guard allowed
    uint32_t headerCount;
    PrByteView body;
    PrFetchMode mode;                           // any mode but Navigate
    PrFetchCredentials credentials;
    PrFetchRedirect redirect;
} PrFetchClientRequest;

// The response to a request of a client, filtered as its type says (fetch/#concept-filtered-response): a basic response
// without Set-Cookie, a CORS response with the headers that it exposes, and an opaque response with no status, URL,
// headers or body. The views are valid during the call that receives the response.
typedef struct PrFetchClientResponse {
    // Ok for a response. Any other status is a network error, and Lost says that the client went away before the
    // response arrived.
    PrStatus status;
    PrFetchResponseType type;
    uint16_t responseStatus;                    // the response's status (fetch/#concept-response-status)
    PrStringView url;                           // after the redirects that the fetch followed, without its fragment
    const PrFetchHeader* PR_NULLABLE headers;
    uint32_t headerCount;
    PrByteView body;                            // decoded from its content codings
} PrFetchClientResponse;

// Called once with the response to a request of a client.
typedef void (*PrFetchClientFn)(void* PR_NULLABLE context, const PrFetchClientResponse* response);

// ---------------------------------------------------------------------------------------------------------------------
// Methods and headers (fetch/#methods, fetch/#terminology-headers)
//
// Names are matched without regard to ASCII case. A value is the header's value as it would be sent.

// Returns true if `method` is a method (fetch/#concept-method): a token (rfc 9110, section 5.6.2).
bool prFetchIsMethod(PrStringView method);

// Returns true if `method` is CONNECT, TRACE or TRACK (fetch/#forbidden-method).
bool prFetchIsForbiddenMethod(PrStringView method);

// Returns true if `method` is GET, HEAD or POST (fetch/#cors-safelisted-method).
bool prFetchIsCorsSafelistedMethod(PrStringView method);

// Returns `method` with DELETE, GET, HEAD, OPTIONS, POST and PUT in upper case, and any other method as it is
// (fetch/#concept-method-normalize). The view is `method` itself or a literal.
PrStringView prFetchNormalizeMethod(PrStringView method);

// Returns true if `name` is a header name (fetch/#header-name): a token.
bool prFetchIsHeaderName(PrStringView name);

// Returns true if `value` is a header value (fetch/#header-value): no leading or trailing HTTP tab or space, and no
// NUL, CR or LF.
bool prFetchIsHeaderValue(PrStringView value);

// Returns `value` without its leading and trailing HTTP whitespace (fetch/#concept-header-value-normalize).
PrStringView prFetchNormalizeHeaderValue(PrStringView value);

// Returns true if a script may not set the header on a request (fetch/#forbidden-request-header), such as Host or
// Cookie, or an X-HTTP-Method-Override that names a forbidden method.
bool prFetchIsForbiddenRequestHeader(PrStringView name, PrStringView value);

// Returns true if a script may not set or read the header on a response: Set-Cookie and Set-Cookie2
// (fetch/#forbidden-response-header-name).
bool prFetchIsForbiddenResponseHeaderName(PrStringView name);

// Returns true if a request may send the header to another origin without a preflight
// (fetch/#cors-safelisted-request-header).
bool prFetchIsCorsSafelistedRequestHeader(PrStringView name, PrStringView value);

// Returns true if `name` is Accept, Accept-Language, Content-Language or Content-Type
// (fetch/#no-cors-safelisted-request-header-name).
bool prFetchIsNoCorsSafelistedRequestHeaderName(PrStringView name);

// Returns true if a no-cors request may carry the header (fetch/#no-cors-safelisted-request-header): a
// no-CORS-safelisted name with a value that is CORS-safelisted.
bool prFetchIsNoCorsSafelistedRequestHeader(PrStringView name, PrStringView value);

// Returns true if a CORS response reveals the header to a script (fetch/#concept-filtered-response-cors): a
// CORS-safelisted response-header name (fetch/#cors-safelisted-response-header-name), or one of the names that
// `exposed`, the value of the response's Access-Control-Expose-Headers, lists. A `*` in the list names every header
// unless the request's credentials mode is Include.
bool prFetchIsCorsExposedHeader(PrStringView name, PrStringView exposed, PrFetchCredentials credentials);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
