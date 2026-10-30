// HTTP exchanges: a request sent and its response received (rfc 9110), over the connections of a network (pr/net.h).
//
// An exchange follows the network's contract: its callbacks run on the network's loop, and it is used there. A source
// of exchanges follows no redirect and decodes no content coding. Fetch does both (pr/fetch.h).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/net.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/tls.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of exchanges, which keeps the connections that they share, created by prHttpCreate or by its backend's own
// constructor.
typedef struct PrHttp PrHttp;

// One request and its response.
typedef struct PrHttpExchange PrHttpExchange;

// A header field (rfc 9110 section 5): its name, matched without regard to ASCII case, and its value.
typedef struct PrHttpHeader {
    PrStringView name;
    PrStringView value;
} PrHttpHeader;

// What happened to an exchange. Each event is delivered once, except Readable.
enum {
    PrHttpEvent_Response,       // the status and the headers of the final response arrived
    PrHttpEvent_Readable,       // bytes of the response's content arrived while none were waiting to be read
    PrHttpEvent_End,            // the content is complete; the bytes that arrived before can still be read
    PrHttpEvent_Fail,           // the exchange failed, for the reason that prHttpExchangeError gives, and ends
};
typedef uint8_t PrHttpEvent;

// Called on the network's loop with each event of `exchange`.
typedef void (*PrHttpExchangeFn)(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event);

// What an exchange requests.
typedef struct PrHttpRequestDesc {
    PrStringView method;        // a token, such as "GET", or the null view for GET
    PrStringView url;           // an absolute http: or https: URL, whose fragment is not sent

    // The header fields that the request carries. The source writes the fields that frame the message itself: Host,
    // Content-Length, Transfer-Encoding and Connection.
    const PrHttpHeader* PR_NULLABLE headers;
    uint32_t headerCount;

    PrByteView body;

    // Called with the exchange's events, with its context.
    PrHttpExchangeFn PR_NULLABLE event;
    void* PR_NULLABLE context;
} PrHttpRequestDesc;

// The start of every exchange's state. `http` is the source that made the exchange.
struct PrHttpExchange {
    PrHttp* http;
};

// The start of every source's state: the function table of its backend, then its network. Each member of the table
// implements the public function of the same name. Callers use the functions below, never these members.
struct PrHttp {
    void (*destroy)(PrHttp* http);
    PrStatus (*exchangeCreate)(PrHttp* http, const PrHttpRequestDesc* desc, PrHttpExchange* PR_NULLABLE* PR_NONNULL out);
    void (*exchangeDestroy)(PrHttp* http, PrHttpExchange* exchange);
    uint16_t (*exchangeStatus)(const PrHttp* http, const PrHttpExchange* exchange);
    uint32_t (*exchangeHeaderCount)(const PrHttp* http, const PrHttpExchange* exchange);
    PrHttpHeader (*exchangeHeader)(const PrHttp* http, const PrHttpExchange* exchange, uint32_t index);
    size_t (*exchangeRead)(PrHttp* http, PrHttpExchange* exchange, uint8_t* output, size_t capacity);
    PrNetError (*exchangeError)(const PrHttp* http, const PrHttpExchange* exchange);

    PrNet* net;                 // the network whose loop runs the exchanges' callbacks
};

// The library's backends of sources, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrHttpBackend_Default,
    PrHttpBackend_Builtin,
    PrHttpBackend_Count,
};
typedef uint8_t PrHttpBackend;

// Returns the backend that `name` names, as prHttpBackendName spells it, ignoring case, or Count if it names none.
PrHttpBackend prHttpBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prHttpBackendName(PrHttpBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prHttpBackendIsBuilt(PrHttpBackend backend);

// A rule that connects the requests to a host at another address, as if the host's name resolved there. The request
// keeps its own host: its Host field, its origin and the server name that TLS verifies are the host's.
typedef struct PrHttpHostRule {
    // The host of the requests that the rule applies to, matched without regard to case. A host that starts with `*.`
    // matches each subdomain of the rest, and not the rest itself.
    PrStringView host;
    uint16_t port;              // the port of those requests, or zero for any port

    // The host to connect to instead, a domain name or an address, and its port, or zero for the request's own port.
    PrStringView target;
    uint16_t targetPort;
} PrHttpHostRule;

// What a source connects with.
typedef struct PrHttpDesc {
    PrNet* net;

    // The source of the sessions that https: exchanges run over. Without it, an https: exchange fails.
    PrTls* PR_NULLABLE tls;

    // DER certificates that every session trusts as roots, in addition to the platform's, which must outlive the
    // source.
    const PrByteView* PR_NULLABLE anchors;
    uint32_t anchorCount;

    // The rules that redirect connections, as a test runner points a browser's names at a local server. The first rule
    // that matches a request applies. The source copies them.
    const PrHttpHostRule* PR_NULLABLE hostRules;
    uint32_t hostRuleCount;

    // The most connections that the source keeps open to one origin, or zero for six.
    uint32_t connectionsPerOrigin;
} PrHttpDesc;

// Creates a source by the backend, as `desc` describes. A backend's own header documents how it connects. Returns null
// if the build does not have the backend. `desc`'s network and TLS source must outlive the source.
PrHttp* PR_NULLABLE prHttpCreate(PrHttpBackend backend, const PrHttpDesc* desc);

// Closes the source's connections and frees it. Its exchanges must be destroyed first. Called on the network's loop,
// outside the callbacks of its exchanges.
void prHttpDestroy(PrHttp* PR_NULLABLE http);

// Starts the exchange that `desc` describes, and writes it to `*out`. The exchange delivers Response once the final
// response's head arrives, then its content, or Fail. Returns InvalidData for a URL that does not parse or a method
// that is not a token, and NotSupported for a URL whose scheme is not http or https. On failure, `*out` is null.
PrStatus prHttpExchangeCreate(PrHttp* http, const PrHttpRequestDesc* desc, PrHttpExchange* PR_NULLABLE* PR_NONNULL out);

// Abandons the exchange if it has not ended, and frees it. Its callback is not called again. An exchange may be
// destroyed during its own callback.
void prHttpExchangeDestroy(PrHttpExchange* PR_NULLABLE exchange);

// Returns the status code of the final response, or zero before Response.
uint16_t prHttpExchangeStatus(const PrHttpExchange* exchange);

// Return the header fields of the final response, in the order they arrived, or none before Response. Each view stays
// valid while the exchange lives.
uint32_t prHttpExchangeHeaderCount(const PrHttpExchange* exchange);
PrHttpHeader prHttpExchangeHeader(const PrHttpExchange* exchange, uint32_t index);

// Copies up to `capacity` bytes of content that arrived into `output`, and returns how many it copied. Returns zero
// once every byte that arrived has been read. Readable is delivered again only when more bytes arrive after that.
size_t prHttpExchangeRead(PrHttpExchange* exchange, uint8_t* PR_NULLABLE output, size_t capacity);

// Returns why the exchange failed, or None if it has not.
PrNetError prHttpExchangeError(const PrHttpExchange* exchange);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
