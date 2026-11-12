// The built-in source of HTTP exchanges (http_builtin.h).

#include "pr/http_builtin.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/text_buffer.h"
#include "pr/url.h"

namespace pr_impl {

namespace {

// The most content bytes that an exchange holds that arrived and were not read. While it holds this many, its
// connection holds the rest of what arrived, and reads no more from its stream once it holds as much again.
constexpr uint32_t kBodyMaxBytes = 256 * 1024;

// The longest head of a response: its status line and its header fields.
constexpr uint32_t kHeadMaxBytes = 64 * 1024;

// The bytes that one read from a connection's stream takes at most.
constexpr uint32_t kReadChunkBytes = 16 * 1024;

constexpr uint32_t kDefaultConnectionsPerOrigin = 6;

struct BuiltinHttp;
struct Connection;

enum {
    ExchangeState_Queued,       // waiting for a connection
    ExchangeState_Head,         // sent, or being sent, and waiting for the response's head
    ExchangeState_Content,
    ExchangeState_Done,
    ExchangeState_Failed,
};
typedef uint8_t ExchangeState;

// How the content of a response ends (rfc 9112 section 6.3).
enum {
    Framing_None,               // the response has no content
    Framing_Length,             // after Content-Length bytes
    Framing_Chunked,            // at the last chunk
    Framing_Close,              // when the connection closes
};
typedef uint8_t Framing;

// Where a chunked decoder is (rfc 9112 section 7.1).
enum {
    ChunkState_Size,            // reading a chunk's size line
    ChunkState_Data,
    ChunkState_DataEnd,         // reading the CRLF after a chunk's data
    ChunkState_Trailer,         // reading the trailer fields after the last chunk
};
typedef uint8_t ChunkState;

// A header field of a response, as ranges of the response's head.
struct FieldSpan {
    uint32_t nameStart;
    uint32_t nameSize;
    uint32_t valueStart;
    uint32_t valueSize;
};

// Memory that outlives its owner's destruction while `holds` is not zero: while one of its handlers runs, which a
// client may destroy it from, and while a task that it posted waits.
struct Held {
    uint32_t holds;
    bool dead;
};

struct Exchange : PrHttpExchange {
    Held held;
    PrHttpExchangeFn PR_NULLABLE event;
    void* PR_NULLABLE context;
    PrRawString origin;            // the scheme, host and port, which the exchange shares connections by
    PrRawString host;              // the request's host, without an IPv6 address's brackets
    uint16_t port;

    // Where the exchange connects: the request's host and port, or those that a host rule gives.
    PrRawString target;
    uint16_t targetPort;
    bool secure;
    bool head;                  // true for a HEAD request, whose response has no content
    bool retried;               // true once the request was sent again on a new connection
    PrRawString request;        // the request's bytes
    ExchangeState state;
    Connection* PR_NULLABLE connection;
    uint16_t status;
    PrRawString response;          // the final response's head, which `fields` index
    RawArray<FieldSpan> fields;
    Framing framing;
    uint64_t remaining;         // the bytes left in the content, or in the chunk
    ChunkState chunk;
    RawArray<uint8_t> content;     // content that arrived, from `contentStart`
    uint32_t contentStart;
    PrNetError error;
};

// A connection to an origin, which carries one exchange at a time. `input` from `inputStart` holds what arrived and was
// not parsed.
struct Connection {
    Held held;
    BuiltinHttp* http;
    PrRawString origin;
    PrNetStream* PR_NULLABLE stream;
    Exchange* PR_NULLABLE exchange;
    RawArray<uint8_t> input;
    uint32_t inputStart;
    uint32_t served;            // the number of responses that the connection completed
    bool reusable;              // true if the current response leaves the connection open
    bool arrived;               // true once a byte of the current response arrived
    bool ended;                 // true once the stream ended
    bool stalled;               // true while the connection leaves its stream's bytes unread, having no room
    bool resumePosted;
};

// A host rule (PrHttpHostRule), with copies of its names.
struct HostRule {
    PrRawString host;
    uint16_t port;
    PrRawString target;
    uint16_t targetPort;
};

struct BuiltinHttp : PrHttp {
    PrTls* PR_NULLABLE tls;
    const PrByteView* PR_NULLABLE anchors;
    uint32_t anchorCount;
    RawArray<HostRule> hostRules;
    uint32_t perOrigin;
    RawArray<Exchange*> queue;     // exchanges waiting for a connection, in the order they were made
    RawArray<Connection*> connections;
    bool pumpPosted;
    bool dead;                  // true once destroyed, while a posted pump waits
};

constexpr PrStringView kProtocols[] = {"http/1.1"};

PrStringView viewOf(const PrRawString& string) {
    return prRawStringView(&string);
}

PrStringView bytesView(const RawArray<uint8_t>& bytes, uint32_t start) {
    return PrStringView{reinterpret_cast<const char*>(bytes.data) + start, bytes.count - start};
}

bool isToken(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsHttpToken(text.data[i]))
            return false;
    }
    return true;
}

bool isFieldValue(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == '\r' || text.data[i] == '\n' || text.data[i] == 0)
            return false;
    }
    return true;
}

// Returns true if the comma-separated list `value` has `token`, without regard to ASCII case.
bool listHas(PrStringView value, PrStringView token) {
    size_t start = 0;
    while (start <= value.size) {
        size_t end = start;
        while (end < value.size && value.data[end] != ',')
            ++end;
        if (prAsciiEqualsFold(stringTrimBy(prStringSlice(value, start, end - start), prAsciiIsTabOrSpace), token))
            return true;
        start = end + 1;
    }
    return false;
}

// The last element of the comma-separated list `value`.
PrStringView listLast(PrStringView value) {
    size_t comma = prStringFindLastByte(value, ',');
    return stringTrimBy(comma == value.size ? value : prStringSlice(value, comma + 1, value.size - comma - 1),
        prAsciiIsTabOrSpace);
}

void hold(Held* held) {
    ++held->holds;
}

// Delivers `event` to the exchange's client, unless the exchange was destroyed.
void deliver(Exchange* exchange, PrHttpEvent event) {
    if (!exchange->held.dead && exchange->event != nullptr)
        exchange->event(exchange->context, exchange, event);
}

void freeExchange(Exchange* exchange) {
    prRawStringReset(&exchange->origin);
    prRawStringReset(&exchange->host);
    prRawStringReset(&exchange->target);
    prRawStringReset(&exchange->request);
    prRawStringReset(&exchange->response);
    exchange->fields.reset();
    exchange->content.reset();
    prMemoryFree(exchange);
}

void releaseExchange(Exchange* exchange) {
    PR_ASSERT(exchange->held.holds != 0);
    if (--exchange->held.holds == 0 && exchange->held.dead)
        freeExchange(exchange);
}

void freeConnection(Connection* connection) {
    prRawStringReset(&connection->origin);
    connection->input.reset();
    prMemoryFree(connection);
}

void releaseConnection(Connection* connection) {
    PR_ASSERT(connection->held.holds != 0);
    if (--connection->held.holds == 0 && connection->held.dead)
        freeConnection(connection);
}

void pump(void* PR_NULLABLE context);

void schedulePump(BuiltinHttp* http) {
    if (http->pumpPosted || http->queue.count == 0)
        return;
    http->pumpPosted = true;
    prNetPost(http->net, pump, http);
}

// Closes the connection's stream and drops it from its source. Its memory goes once nothing holds it. Its origin then
// has room for another connection, which a waiting exchange takes.
void closeConnection(Connection* connection) {
    if (connection->held.dead)
        return;
    connection->held.dead = true;
    BuiltinHttp* http = connection->http;
    http->connections.removeFirst(connection);
    prNetStreamDestroy(connection->stream);
    connection->stream = nullptr;
    if (connection->held.holds == 0)
        freeConnection(connection);
    schedulePump(http);
}

// Detaches the exchange from its connection, which closes unless the exchange completed the response and the connection
// stays open for another. Either way, a waiting exchange takes the connection or its place.
void detach(Exchange* exchange, bool keep) {
    Connection* connection = exchange->connection;
    if (connection == nullptr)
        return;
    exchange->connection = nullptr;
    connection->exchange = nullptr;
    if (keep && connection->reusable && connection->inputStart == connection->input.count && !connection->ended) {
        connection->input.clear();
        connection->inputStart = 0;
        connection->arrived = false;
        schedulePump(connection->http);
    } else {
        closeConnection(connection);
    }
}

void failExchange(Exchange* exchange, PrNetError error) {
    if (exchange->state == ExchangeState_Failed || exchange->state == ExchangeState_Done)
        return;
    exchange->state = ExchangeState_Failed;
    exchange->error = error;
    detach(exchange, false);
    deliver(exchange, PrHttpEvent_Fail);
}

void completeExchange(Exchange* exchange) {
    exchange->state = ExchangeState_Done;
    if (exchange->connection != nullptr)
        ++exchange->connection->served;
    detach(exchange, true);
    deliver(exchange, PrHttpEvent_End);
}

// Returns the line that starts at `start` of `text`, without its line ending, and sets `*next` past the ending. Returns
// false if no line ending has arrived. A bare LF ends a line too (rfc 9112 section 2.2).
bool lineAt(PrStringView text, size_t start, PrStringView* line, size_t* next) {
    size_t end = start;
    while (end < text.size && text.data[end] != '\n')
        ++end;
    if (end == text.size)
        return false;
    size_t lineEnd = end > start && text.data[end - 1] == '\r' ? end - 1 : end;
    *line = prStringSlice(text, start, lineEnd - start);
    *next = end + 1;
    return true;
}

// Parses a Content-Length, which may repeat one value in a list (rfc 9110 section 8.6).
bool parseLength(PrStringView value, uint64_t* out) {
    bool found = false;
    size_t start = 0;
    while (start <= value.size) {
        size_t end = start;
        while (end < value.size && value.data[end] != ',')
            ++end;
        PrStringView digits = stringTrimBy(prStringSlice(value, start, end - start), prAsciiIsTabOrSpace);
        if (digits.size == 0 || digits.size > 18)
            return false;
        uint64_t length = 0;
        for (size_t i = 0; i < digits.size; ++i) {
            if (!prAsciiIsDigit(digits.data[i]))
                return false;
            length = length * 10 + uint64_t(digits.data[i] - '0');
        }
        if (found && length != *out)
            return false;
        *out = length;
        found = true;
        start = end + 1;
    }
    return found;
}

// Reads the response's status line and header fields, if they have all arrived. Interim responses are skipped. Returns
// false if the exchange or its connection went away, or the head has not all arrived.
bool parseHead(Connection* connection, Exchange* exchange) {
    for (;;) {
        PrStringView line{};
        size_t next = 0;
        // Empty lines before the status line are ignored (rfc 9112 section 2.2).
        while (lineAt(bytesView(connection->input, connection->inputStart), 0, &line, &next) && line.size == 0)
            connection->inputStart += uint32_t(next);
        // The head ends at an empty line.
        PrStringView input = bytesView(connection->input, connection->inputStart);
        size_t at = 0;
        bool complete = false;
        while (lineAt(input, at, &line, &next)) {
            at = next;
            if (line.size == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            if (input.size > kHeadMaxBytes)
                failExchange(exchange, PrNetError_Protocol);
            return false;
        }
        PrStringView head = prStringSlice(input, 0, at);
        lineAt(head, 0, &line, &next);
        if (line.size < 12 || !prStringStartsWith(line, "HTTP/1.") || line.data[8] != ' ' ||
                !prAsciiIsDigit(line.data[9]) || !prAsciiIsDigit(line.data[10]) || !prAsciiIsDigit(line.data[11])) {
            failExchange(exchange, PrNetError_Protocol);
            return false;
        }
        bool http11 = line.data[7] == '1';
        uint16_t status = uint16_t((line.data[9] - '0') * 100 + (line.data[10] - '0') * 10 + (line.data[11] - '0'));
        connection->inputStart += uint32_t(at);
        if (status >= 100 && status < 200) {
            // An upgrade to another protocol is not asked for, so it is not accepted.
            if (status == 101) {
                failExchange(exchange, PrNetError_Protocol);
                return false;
            }
            continue;
        }
        prRawStringReplace(&exchange->response, head);
        PrStringView stored = viewOf(exchange->response);
        exchange->fields.clear();
        size_t cursor = next;
        bool chunked = false;
        bool encoded = false;
        bool lengthGiven = false;
        bool closes = !http11;
        uint64_t length = 0;
        while (lineAt(stored, cursor, &line, &next) && line.size != 0) {
            uint32_t lineStart = uint32_t(line.data - stored.data);
            cursor = next;
            size_t colon = prStringFindByte(line, ':');
            PrStringView name = prStringSlice(line, 0, colon);
            if (colon == line.size || !isToken(name)) {
                // A line that folds the field before it, or is not a field, is malformed (rfc 9112 section 5.2).
                failExchange(exchange, PrNetError_Protocol);
                return false;
            }
            PrStringView afterColon = prStringSlice(line, colon + 1, line.size - colon - 1);
            PrStringView value = stringTrimBy(afterColon, prAsciiIsTabOrSpace);
            exchange->fields.push(FieldSpan{
                lineStart, uint32_t(name.size), uint32_t(value.data - stored.data),
                uint32_t(value.size),
            });
            if (prAsciiEqualsFold(name, "transfer-encoding")) {
                encoded = true;
                chunked = prAsciiEqualsFold(listLast(value), "chunked");
            } else if (prAsciiEqualsFold(name, "content-length")) {
                if (!parseLength(value, &length) || (lengthGiven && length != exchange->remaining)) {
                    failExchange(exchange, PrNetError_Protocol);
                    return false;
                }
                lengthGiven = true;
                exchange->remaining = length;
            } else if (prAsciiEqualsFold(name, "connection")) {
                if (listHas(value, "close"))
                    closes = true;
                else if (!http11 && listHas(value, "keep-alive"))
                    closes = false;
            }
        }
        exchange->status = status;
        if (exchange->head || status == 204 || status == 304) {
            exchange->framing = Framing_None;
        } else if (encoded) {
            exchange->framing = chunked ? Framing_Chunked : Framing_Close;
            exchange->chunk = ChunkState_Size;
        } else if (lengthGiven) {
            exchange->framing = Framing_Length;
            exchange->remaining = length;
        } else {
            exchange->framing = Framing_Close;
        }
        // A 204 or 304 has no content, whatever its head says. A server that declares content for one may send it, and
        // what arrives after the head could be taken for the next response's, so the connection carries no other.
        bool undelivered = (status == 204 || status == 304) && !exchange->head && (encoded || length != 0);
        connection->reusable = !closes && exchange->framing != Framing_Close && !undelivered;
        exchange->state = ExchangeState_Content;
        deliver(exchange, PrHttpEvent_Response);
        return !exchange->held.dead && !connection->held.dead && exchange->state == ExchangeState_Content;
    }
}

// Moves up to `size` bytes of the connection's input into the exchange's content.
void take(Connection* connection, Exchange* exchange, size_t size) {
    if (exchange->contentStart != 0 && exchange->contentStart == exchange->content.count) {
        exchange->content.clear();
        exchange->contentStart = 0;
    }
    exchange->content.append(connection->input.data + connection->inputStart, uint32_t(size));
    connection->inputStart += uint32_t(size);
}

// Returns the number of content bytes that the exchange has room for.
size_t roomOf(const Exchange* exchange) {
    uint32_t held = exchange->content.count - exchange->contentStart;
    return held < kBodyMaxBytes ? kBodyMaxBytes - held : 0;
}

// Moves the content that arrived into the exchange, as far as it has room, and completes the exchange at the content's
// end. Returns true if the content ended.
bool parseContent(Connection* connection, Exchange* exchange) {
    for (;;) {
        PrStringView input = bytesView(connection->input, connection->inputStart);
        size_t room = roomOf(exchange);
        switch (exchange->framing) {
            case Framing_None:
                return true;
            case Framing_Close: {
                size_t size = input.size < room ? input.size : room;
                take(connection, exchange, size);
                return false;
            }
            case Framing_Length: {
                size_t size = input.size < room ? input.size : room;
                if (size > exchange->remaining)
                    size = size_t(exchange->remaining);
                take(connection, exchange, size);
                exchange->remaining -= size;
                return exchange->remaining == 0;
            }
            case Framing_Chunked:
                break;
        }
        PrStringView line{};
        size_t next = 0;
        switch (exchange->chunk) {
            case ChunkState_Size: {
                if (!lineAt(input, 0, &line, &next)) {
                    if (input.size > 1024)
                        failExchange(exchange, PrNetError_Protocol);
                    return false;
                }
                uint64_t size = 0;
                size_t digits = 0;
                while (digits < line.size && prAsciiIsHexDigit(line.data[digits]) && digits < 15) {
                    size = size * 16 + prAsciiHexValue(line.data[digits]);
                    ++digits;
                }
                if (digits == 0 || (digits < line.size && line.data[digits] != ';' && line.data[digits] != ' ' &&
                        line.data[digits] != '\t')) {
                    failExchange(exchange, PrNetError_Protocol);
                    return false;
                }
                connection->inputStart += uint32_t(next);
                exchange->remaining = size;
                exchange->chunk = size == 0 ? ChunkState_Trailer : ChunkState_Data;
                break;
            }
            case ChunkState_Data: {
                size_t size = input.size < room ? input.size : room;
                if (size > exchange->remaining)
                    size = size_t(exchange->remaining);
                if (size == 0)
                    return false;
                take(connection, exchange, size);
                exchange->remaining -= size;
                if (exchange->remaining == 0)
                    exchange->chunk = ChunkState_DataEnd;
                break;
            }
            case ChunkState_DataEnd:
                if (!lineAt(input, 0, &line, &next))
                    return false;
                if (line.size != 0) {
                    failExchange(exchange, PrNetError_Protocol);
                    return false;
                }
                connection->inputStart += uint32_t(next);
                exchange->chunk = ChunkState_Size;
                break;
            case ChunkState_Trailer:
                if (!lineAt(input, 0, &line, &next))
                    return false;
                connection->inputStart += uint32_t(next);
                if (line.size == 0)
                    return true;
                break;
        }
    }
}

// Reads what the stream has, while the connection holds less unparsed input than an exchange's content holds.
void readStream(Connection* connection) {
    uint8_t chunk[kReadChunkBytes];
    connection->stalled = false;
    while (connection->stream != nullptr) {
        if (connection->input.count - connection->inputStart >= kBodyMaxBytes) {
            connection->stalled = true;
            return;
        }
        if (connection->inputStart != 0 && connection->inputStart == connection->input.count) {
            connection->input.clear();
            connection->inputStart = 0;
        }
        size_t got = prNetStreamRead(connection->stream, chunk, sizeof(chunk));
        if (got == 0)
            return;
        connection->input.append(chunk, uint32_t(got));
        connection->arrived = true;
    }
}

// Parses what arrived for the connection's exchange, and delivers what it finds.
void process(Connection* connection) {
    Exchange* exchange = connection->exchange;
    if (exchange == nullptr) {
        // Bytes on an idle connection are not a response to anything, so the connection cannot be trusted.
        if (connection->inputStart != connection->input.count)
            closeConnection(connection);
        return;
    }
    hold(&exchange->held);
    if (exchange->state == ExchangeState_Head && !parseHead(connection, exchange)) {
        releaseExchange(exchange);
        return;
    }
    if (exchange->state == ExchangeState_Content && !exchange->held.dead) {
        bool waiting = exchange->content.count > exchange->contentStart;
        uint32_t before = exchange->content.count - exchange->contentStart;
        bool ended = parseContent(connection, exchange);
        bool arrived = exchange->content.count - exchange->contentStart > before;
        if (exchange->state == ExchangeState_Content) {
            if (arrived && !waiting)
                deliver(exchange, PrHttpEvent_Readable);
            if (ended && !exchange->held.dead && exchange->state == ExchangeState_Content)
                completeExchange(exchange);
        }
    }
    releaseExchange(exchange);
}

// Handles the end of the connection's stream, or its failure with `error`.
void streamEnded(Connection* connection, PrNetError error) {
    connection->ended = true;
    Exchange* exchange = connection->exchange;
    if (exchange == nullptr) {
        closeConnection(connection);
        return;
    }
    BuiltinHttp* http = connection->http;
    if (exchange->state == ExchangeState_Head && !connection->arrived && connection->served != 0 && !exchange->retried) {
        // A kept connection that the server closed before it answered. The request is sent again on a new one.
        exchange->retried = true;
        exchange->state = ExchangeState_Queued;
        detach(exchange, false);
        http->queue.insert(0, exchange);
        schedulePump(http);
        return;
    }
    if (exchange->state == ExchangeState_Content && exchange->framing == Framing_Close && error == PrNetError_None) {
        completeExchange(exchange);
        return;
    }
    failExchange(exchange, error != PrNetError_None ? error : PrNetError_Reset);
}

void connectionEvent(void* PR_NULLABLE context, PrNetStream* stream, PrNetStreamEvent event) {
    Connection* connection = static_cast<Connection*>(context);
    hold(&connection->held);
    switch (event) {
        case PrNetStreamEvent_Open:
        case PrNetStreamEvent_Writable:
            break;
        case PrNetStreamEvent_Readable:
            readStream(connection);
            process(connection);
            break;
        case PrNetStreamEvent_End:
            readStream(connection);
            process(connection);
            if (!connection->held.dead) {
                // Content that the exchange has no room for yet is parsed as the client reads.
                Exchange* exchange = connection->exchange;
                if (exchange != nullptr && exchange->state == ExchangeState_Content && roomOf(exchange) == 0 &&
                        connection->inputStart != connection->input.count)
                    connection->ended = true;
                else
                    streamEnded(connection, PrNetError_None);
            }
            break;
        case PrNetStreamEvent_Fail:
            streamEnded(connection, prNetStreamError(stream));
            break;
    }
    releaseConnection(connection);
}

// Parses more of the connection's input, from a task, once its exchange's client read enough to make room.
void resume(void* PR_NULLABLE context) {
    Connection* connection = static_cast<Connection*>(context);
    connection->resumePosted = false;
    if (!connection->held.dead) {
        process(connection);
        if (!connection->held.dead) {
            readStream(connection);
            process(connection);
        }
        if (!connection->held.dead && connection->ended && connection->inputStart == connection->input.count)
            streamEnded(connection, PrNetError_None);
    }
    releaseConnection(connection);
}

Connection* openConnection(BuiltinHttp* http, Exchange* exchange) {
    Connection* connection = allocZeroed<Connection>();
    connection->http = http;
    connection->origin = prRawStringCopy(viewOf(exchange->origin));
    PrStatus status = PrStatus_Ok;
    if (exchange->secure) {
        PrTlsSessionDesc desc{
            .host = viewOf(exchange->target),
            .port = exchange->targetPort,
            .serverName = viewOf(exchange->host),
            .protocols = kProtocols,
            .protocolCount = 1,
            .anchors = http->anchors,
            .anchorCount = http->anchorCount,
            .event = connectionEvent,
            .context = connection,
        };
        PrTlsSession* session = nullptr;
        status = http->tls != nullptr ? prTlsConnect(http->tls, &desc, &session) : PrStatus_NotSupported;
        connection->stream = prTlsSessionAsStream(session);
    } else {
        PrNetConnectDesc desc{
            .host = viewOf(exchange->target),
            .port = exchange->targetPort,
            .event = connectionEvent,
            .context = connection,
        };
        status = prNetConnect(http->net, &desc, &connection->stream);
    }
    if (status != PrStatus_Ok) {
        freeConnection(connection);
        return nullptr;
    }
    http->connections.push(connection);
    return connection;
}

void assign(Connection* connection, Exchange* exchange) {
    connection->exchange = exchange;
    connection->arrived = false;
    connection->reusable = false;
    exchange->connection = connection;
    exchange->state = ExchangeState_Head;
    PrStringView request = viewOf(exchange->request);
    prNetStreamWrite(connection->stream, prStringBytes(request));
}

// Returns true if the rule's host matches `host`: the same name, or a subdomain of a wildcard's name.
bool ruleMatches(const HostRule& rule, PrStringView host) {
    PrStringView pattern = viewOf(rule.host);
    if (!prStringStartsWith(pattern, "*."))
        return prAsciiEqualsFold(pattern, host);
    PrStringView suffix = prStringSlice(pattern, 1, pattern.size - 1);
    return host.size > suffix.size && prAsciiEndsWithFold(host, suffix);
}

void freeHttp(BuiltinHttp* http) {
    http->queue.reset();
    http->connections.reset();
    for (uint32_t i = 0; i < http->hostRules.count; ++i) {
        prRawStringReset(&http->hostRules.data[i].host);
        prRawStringReset(&http->hostRules.data[i].target);
    }
    http->hostRules.reset();
    prMemoryFree(http);
}

// Gives each waiting exchange a connection to its origin: an idle one, or a new one while the origin has fewer than the
// source allows. Runs in a task, so that a failure is delivered after the call that made the exchange.
void pump(void* PR_NULLABLE context) {
    BuiltinHttp* http = static_cast<BuiltinHttp*>(context);
    http->pumpPosted = false;
    if (http->dead) {
        freeHttp(http);
        return;
    }
    for (bool progressed = true; progressed;) {
        progressed = false;
        for (uint32_t i = 0; i < http->queue.count && !progressed; ++i) {
            Exchange* exchange = http->queue.data[i];
            PrStringView origin = viewOf(exchange->origin);
            Connection* idle = nullptr;
            uint32_t open = 0;
            for (uint32_t k = 0; k < http->connections.count; ++k) {
                Connection* connection = http->connections.data[k];
                if (viewOf(connection->origin) != origin)
                    continue;
                ++open;
                if (connection->exchange == nullptr && idle == nullptr)
                    idle = connection;
            }
            if (idle == nullptr && open >= http->perOrigin)
                continue;
            http->queue.erase(i);
            progressed = true;
            Connection* connection = idle != nullptr ? idle : openConnection(http, exchange);
            if (connection != nullptr) {
                assign(connection, exchange);
            } else {
                hold(&exchange->held);
                failExchange(exchange, exchange->secure ? PrNetError_Protocol : PrNetError_Io);
                releaseExchange(exchange);
            }
        }
    }
}

// Writes the request's bytes: its request line, Host, the client's fields, the framing fields and the content.
PrStatus serializeRequest(const PrHttpRequestDesc* desc, const PrUrl* url, PrTextBuffer* out) {
    PrStringView method = desc->method.data != nullptr ? desc->method : PrStringView{"GET"};
    if (!isToken(method))
        return PrStatus_InvalidData;
    prTextBufferAppend(out, method);
    prTextBufferAppendChar(out, ' ');
    PrStringView path = prUrlPath(url);
    prTextBufferAppend(out, path.size != 0 ? path : PrStringView{"/"});
    PrStringView query = prUrlQuery(url);
    if (query.data != nullptr) {
        prTextBufferAppendChar(out, '?');
        prTextBufferAppend(out, query);
    }
    prTextBufferAppend(out, " HTTP/1.1\r\nHost: ");
    prTextBufferAppend(out, prUrlHost(url));
    if (prUrlPort(url) >= 0) {
        prTextBufferAppendChar(out, ':');
        prTextBufferAppendU64(out, uint64_t(prUrlPort(url)), 10);
    }
    prTextBufferAppend(out, "\r\n");
    for (uint32_t i = 0; i < desc->headerCount; ++i) {
        const PrHttpHeader& field = desc->headers[i];
        if (!isToken(field.name) || !isFieldValue(field.value))
            return PrStatus_InvalidData;
        if (prAsciiEqualsFold(field.name, "host") || prAsciiEqualsFold(field.name, "content-length") ||
                prAsciiEqualsFold(field.name, "transfer-encoding") || prAsciiEqualsFold(field.name, "connection"))
            continue;
        prTextBufferAppend(out, field.name);
        prTextBufferAppend(out, ": ");
        prTextBufferAppend(out, field.value);
        prTextBufferAppend(out, "\r\n");
    }
    if (desc->body.size != 0 || !(method == "GET" || method == "HEAD")) {
        prTextBufferAppend(out, "Content-Length: ");
        prTextBufferAppendU64(out, desc->body.size, 10);
        prTextBufferAppend(out, "\r\n");
    }
    prTextBufferAppend(out, "\r\n");
    prTextBufferAppend(out, PrStringView{reinterpret_cast<const char*>(desc->body.data), uint32_t(desc->body.size)});
    return PrStatus_Ok;
}

// Source functions

void destroyHttp(PrHttp* base) {
    BuiltinHttp* http = static_cast<BuiltinHttp*>(base);
    PR_ASSERT(http->queue.count == 0);
    while (http->connections.count != 0) {
        PR_ASSERT(http->connections.data[0]->exchange == nullptr);
        closeConnection(http->connections.data[0]);
    }
    if (http->pumpPosted) {
        http->dead = true;
        return;
    }
    freeHttp(http);
}

PrStatus createExchange(PrHttp* base, const PrHttpRequestDesc* desc, PrHttpExchange* PR_NULLABLE* PR_NONNULL out) {
    BuiltinHttp* http = static_cast<BuiltinHttp*>(base);
    PrUrl* url = nullptr;
    if (prUrlCreate(desc->url, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
        return PrStatus_InvalidData;
    PrStringView scheme = prUrlScheme(url);
    bool secure = scheme == "https";
    if (!secure && scheme != "http") {
        prUrlDestroy(url);
        return PrStatus_NotSupported;
    }
    PrTextBuffer request{};
    PrStatus status = serializeRequest(desc, url, &request);
    PrStringView host = prUrlHost(url);
    if (status != PrStatus_Ok || host.size == 0) {
        prTextBufferReset(&request);
        prUrlDestroy(url);
        return PrStatus_InvalidData;
    }
    Exchange* exchange = allocZeroed<Exchange>();
    exchange->http = http;
    exchange->event = desc->event;
    exchange->context = desc->context;
    exchange->secure = secure;
    exchange->head = desc->method == "HEAD";
    int32_t port = prUrlPort(url);
    exchange->port = uint16_t(port >= 0 ? port : secure ? 443 : 80);
    // An IPv6 address is serialized in brackets, which a connection does not take.
    if (host.size > 2 && host.data[0] == '[')
        host = prStringSlice(host, 1, host.size - 2);
    exchange->host = prRawStringCopy(host);
    exchange->target = prRawStringCopy(host);
    exchange->targetPort = exchange->port;
    for (uint32_t i = 0; i < http->hostRules.count; ++i) {
        const HostRule& rule = http->hostRules.data[i];
        if (ruleMatches(rule, host) && (rule.port == 0 || rule.port == exchange->port)) {
            prRawStringReplace(&exchange->target, viewOf(rule.target));
            exchange->targetPort = rule.targetPort != 0 ? rule.targetPort : exchange->port;
            break;
        }
    }
    PrTextBuffer origin{};
    prTextBufferAppend(&origin, scheme);
    prTextBufferAppend(&origin, "://");
    prTextBufferAppend(&origin, prUrlHost(url));
    prTextBufferAppendChar(&origin, ':');
    prTextBufferAppendU64(&origin, exchange->port, 10);
    exchange->origin = prTextBufferCopy(&origin);
    prTextBufferReset(&origin);
    exchange->request = prTextBufferCopy(&request);
    prTextBufferReset(&request);
    prUrlDestroy(url);
    http->queue.push(exchange);
    schedulePump(http);
    *out = exchange;
    return PrStatus_Ok;
}

void destroyExchange(PrHttp* base, PrHttpExchange* handle) {
    BuiltinHttp* http = static_cast<BuiltinHttp*>(base);
    Exchange* exchange = static_cast<Exchange*>(handle);
    PR_ASSERT(!exchange->held.dead);
    exchange->held.dead = true;
    if (exchange->state == ExchangeState_Queued)
        http->queue.removeFirst(exchange);
    // An exchange abandoned before its response ended leaves its connection with no way to find the next response. A
    // response with no content, such as a 304, ends with its head.
    bool ended = exchange->state == ExchangeState_Done ||
        (exchange->state == ExchangeState_Content && exchange->framing == Framing_None);
    if (ended && exchange->state != ExchangeState_Done && exchange->connection != nullptr)
        ++exchange->connection->served;
    detach(exchange, ended);
    if (exchange->held.holds == 0)
        freeExchange(exchange);
}

uint16_t exchangeStatus(const PrHttp*, const PrHttpExchange* handle) {
    return static_cast<const Exchange*>(handle)->status;
}

uint32_t exchangeHeaderCount(const PrHttp*, const PrHttpExchange* handle) {
    const Exchange* exchange = static_cast<const Exchange*>(handle);
    return exchange->status != 0 ? exchange->fields.count : 0;
}

PrHttpHeader exchangeHeader(const PrHttp*, const PrHttpExchange* handle, uint32_t index) {
    const Exchange* exchange = static_cast<const Exchange*>(handle);
    PrStringView stored = viewOf(exchange->response);
    const FieldSpan& span = exchange->fields.data[index];
    return PrHttpHeader{
        prStringSlice(stored, span.nameStart, span.nameSize),
        prStringSlice(stored, span.valueStart, span.valueSize),
    };
}

size_t exchangeRead(PrHttp* base, PrHttpExchange* handle, uint8_t* output, size_t capacity) {
    Exchange* exchange = static_cast<Exchange*>(handle);
    uint32_t available = exchange->content.count - exchange->contentStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    prMemCopy(output, exchange->content.data + exchange->contentStart, taken);
    exchange->contentStart += taken;
    if (exchange->contentStart == exchange->content.count) {
        exchange->content.clear();
        exchange->contentStart = 0;
    }
    // Input that waited for room is parsed in a task, so that its events come after this call.
    Connection* connection = exchange->connection;
    if (connection != nullptr && !connection->resumePosted &&
            (connection->inputStart != connection->input.count || connection->stalled || connection->ended)) {
        connection->resumePosted = true;
        hold(&connection->held);
        prNetPost(base->net, resume, connection);
    }
    return taken;
}

PrNetError exchangeError(const PrHttp*, const PrHttpExchange* handle) {
    return static_cast<const Exchange*>(handle)->error;
}

PrStringView exchangeProtocol(const PrHttp*, const PrHttpExchange*) {
    return kProtocols[0];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrHttp* prHttpBuiltinCreate(const PrHttpDesc* desc) {
    BuiltinHttp* http = allocZeroed<BuiltinHttp>();
    http->destroy = destroyHttp;
    http->exchangeCreate = createExchange;
    http->exchangeDestroy = destroyExchange;
    http->exchangeStatus = exchangeStatus;
    http->exchangeHeaderCount = exchangeHeaderCount;
    http->exchangeHeader = exchangeHeader;
    http->exchangeRead = exchangeRead;
    http->exchangeError = exchangeError;
    http->exchangeProtocol = exchangeProtocol;
    http->net = desc->net;
    http->tls = desc->tls;
    http->anchors = desc->anchors;
    http->anchorCount = desc->anchorCount;
    for (uint32_t i = 0; i < desc->hostRuleCount; ++i) {
        const PrHttpHostRule& rule = desc->hostRules[i];
        HostRule copied = {prRawStringCopy(rule.host), rule.port, prRawStringCopy(rule.target), rule.targetPort};
        http->hostRules.push(copied);
    }
    http->perOrigin = desc->connectionsPerOrigin != 0 ? desc->connectionsPerOrigin : kDefaultConnectionsPerOrigin;
    return http;
}
