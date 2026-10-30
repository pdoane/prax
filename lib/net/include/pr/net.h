// Networks: connections to hosts, and the loop that runs their callbacks.
//
// A network owns a loop, which is one thread or one serial queue. The loop runs every callback of the network's
// streams, and of every layer built over the network, one at a time. A callback never runs during the call that caused
// it. A network and its streams are used on the loop, except for prNetPost, prNetPostBlocking and prNetDestroy.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of connections, with its loop. Each backend provides its own constructor.
typedef struct PrNet PrNet;

// A connection that carries bytes both ways, in order, such as a TCP connection or a TLS session over one.
typedef struct PrNetStream PrNetStream;

// What happened to a stream. Each event is delivered once, except Readable and Writable.
enum {
    PrNetStreamEvent_Open,      // the connection is established, and the bytes written so far are being sent
    PrNetStreamEvent_Readable,  // bytes arrived while none were waiting to be read
    PrNetStreamEvent_Writable,  // every byte written so far was handed to the transport
    PrNetStreamEvent_End,       // the peer sends nothing more; the bytes that arrived before can still be read
    PrNetStreamEvent_Fail,      // the stream failed, for the reason that prNetStreamError gives, and ends
};
typedef uint8_t PrNetStreamEvent;

// Why a stream failed.
enum {
    PrNetError_None,
    PrNetError_NameNotResolved, // the host's name has no address
    PrNetError_Refused,         // the host refused the connection
    PrNetError_Unreachable,     // no route reaches the host
    PrNetError_TimedOut,
    PrNetError_Reset,           // the peer or a middlebox aborted the connection
    PrNetError_Protocol,        // the protocol that the stream runs failed, and the layer that runs it says why
    PrNetError_Blocked,         // a layer's policy refused the request or its response, as a failed CORS check does
    PrNetError_Io,              // the system failed in another way
};
typedef uint8_t PrNetError;

// Called on the network's loop with each event of `stream`.
typedef void (*PrNetStreamFn)(void* PR_NULLABLE context, PrNetStream* stream, PrNetStreamEvent event);

// Called on the network's loop with the context that prNetPost was given.
typedef void (*PrNetTaskFn)(void* PR_NULLABLE context);

// What a connection is made to.
typedef struct PrNetConnectDesc {
    PrStringView host;              // a domain name, or an IPv4 or IPv6 address
    uint16_t port;

    // Called with the stream's events, with its context.
    PrNetStreamFn PR_NULLABLE event;
    void* PR_NULLABLE context;
} PrNetConnectDesc;

// The function table of a network, placed at the start of the backend's state. Each member implements the public
// function of the same name. Callers use the functions below, never these members.
struct PrNet {
    void (*destroy)(PrNet* net);
    void (*post)(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);

    // Null for a backend whose loop is a thread of its own, which prNetPostBlocking waits for.
    void (*PR_NULLABLE postBlocking)(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);
    PrStatus (*connect)(PrNet* net, const PrNetConnectDesc* desc, PrNetStream* PR_NULLABLE* PR_NONNULL out);
};

// The start of every stream's state: the function table of the stream's backend, then the network whose loop runs the
// stream's callbacks and the client's callback. Each member of the table implements the public function of the same
// name. A null `finish` means the stream cannot end its sending side alone. Callers use the functions below, never
// these members.
struct PrNetStream {
    void (*destroy)(PrNetStream* stream);
    size_t (*read)(PrNetStream* stream, uint8_t* output, size_t capacity);
    void (*write)(PrNetStream* stream, PrByteView bytes);
    PrStatus (*PR_NULLABLE finish)(PrNetStream* stream);
    size_t (*queued)(const PrNetStream* stream);
    PrNetError (*error)(const PrNetStream* stream);

    PrNet* net;
    PrNetStreamFn PR_NULLABLE event;
    void* PR_NULLABLE context;
};

// The library's backends of networks, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrNetBackend_Default,
    PrNetBackend_Sockets,
    PrNetBackend_Null,
    PrNetBackend_Count,
};
typedef uint8_t PrNetBackend;

// Returns the backend that `name` names, as prNetBackendName spells it, ignoring case, or Count if it names none.
PrNetBackend prNetBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prNetBackendName(PrNetBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prNetBackendIsBuilt(PrNetBackend backend);

// Creates a network by the backend. A backend's own header documents what its loop is. Returns null if the build does
// not have the backend.
PrNet* PR_NULLABLE prNetCreate(PrNetBackend backend);

// Stops the loop and frees the network. Its streams, and every layer built over it, must be destroyed first. Called
// from a thread other than the loop's. The tasks that were posted before run first.
void prNetDestroy(PrNet* PR_NULLABLE net);

// Runs `fn` with `context` on the loop, after the tasks posted before it. Any thread may call it.
void prNetPost(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);

// Runs `fn` with `context` on the loop, after the tasks posted before it, and returns once `fn` has returned, as an
// owner does to destroy what lives on the loop. Any thread but the loop's may call it. A network whose loop runs on its
// owner's thread (prNetNullCreate) runs the loop during the call.
void prNetPostBlocking(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);

// Starts a connection to `desc`'s host, and writes its stream to `*out`. The stream delivers Open once it connects, or
// Fail if it cannot. A name that resolves to several addresses is tried at each in turn. Returns InvalidData for a host
// that is neither a domain name nor an address. On failure, `*out` is null.
PrStatus prNetConnect(PrNet* net, const PrNetConnectDesc* desc, PrNetStream* PR_NULLABLE* PR_NONNULL out);

// Closes the stream at once, without waiting for queued bytes, and frees it. Its callback is not called again. A stream
// may be destroyed during its own callback.
void prNetStreamDestroy(PrNetStream* PR_NULLABLE stream);

// Replaces the callback that the stream's events are delivered to, as a layer does that takes over a stream.
void prNetStreamSetEvent(PrNetStream* stream, PrNetStreamFn PR_NULLABLE event, void* PR_NULLABLE context);

// Copies up to `capacity` bytes that arrived into `output`, and returns how many it copied. Returns zero once every
// byte that arrived has been read. Readable is delivered again only when more bytes arrive after that.
size_t prNetStreamRead(PrNetStream* stream, uint8_t* PR_NULLABLE output, size_t capacity);

// Queues a copy of `bytes` to be sent after the bytes written before. A write before Open is sent once the stream
// opens. A write after Fail, or after prNetStreamFinish, is dropped.
void prNetStreamWrite(PrNetStream* stream, PrByteView bytes);

// Ends what the stream sends, once the queued bytes are sent: a TCP FIN, or a TLS close_notify. The stream can still
// read until the peer ends. Returns NotSupported, and changes nothing, if the stream cannot end its sending side while
// it reads. Such a stream ends both sides when it is destroyed.
PrStatus prNetStreamFinish(PrNetStream* stream);

// Returns the number of bytes written that the stream has not yet handed to its transport.
size_t prNetStreamQueued(const PrNetStream* stream);

// Returns why the stream failed, or None if it has not.
PrNetError prNetStreamError(const PrNetStream* stream);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
