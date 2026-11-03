// The network over the system's sockets (net_sockets.h): its loop, streams and resolver, over the calls of
// net_sockets_platform.h.

#include "pr/net_sockets.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/thread.h"

#include "net/backends/sockets/src/net_sockets_platform.h"

namespace pr_impl {

namespace {

// The most bytes that a stream holds that arrived and were not read. A stream stops reading from its socket while it
// holds this many.
constexpr uint32_t kReceivedMaxBytes = 256 * 1024;

// The bytes that one receive reads at most.
constexpr uint32_t kReceiveChunkBytes = 64 * 1024;

// The longest domain name (rfc 1035), with room for its terminator.
constexpr uint32_t kHostMaxBytes = 254;

struct Network;
struct Stream;

// A name to resolve on the resolver's thread. The resolver reads `host` and `port` and writes the result. `stream` is
// read and written only on the loop: it is cleared if the stream is destroyed before the result arrives.
struct ResolveJob {
    Network* network;
    Stream* PR_NULLABLE stream;
    char host[kHostMaxBytes + 1];
    uint16_t port;
    PrNetError error;
    Array<SocketsAddress> addresses;
};

enum {
    StreamState_Resolving,
    StreamState_Connecting,
    StreamState_Open,
    StreamState_Failed,
};
typedef uint8_t StreamState;

// A TCP connection. Its bytes are buffered both ways: `received` from `receivedStart` holds what arrived and was not
// read, and `unsent` from `unsentStart` holds what was written and not yet sent.
struct Stream : PrNetStream {
    SocketsHandle socket;           // kSocketsHandleNone while none is open
    StreamState state;
    ResolveJob* PR_NULLABLE job;
    Array<SocketsAddress> addresses;
    uint32_t nextAddress;           // the address to try after the one connecting
    PrNetError failure;
    PrNetError lastAttempt;         // why the last address failed
    Array<uint8_t> received;
    uint32_t receivedStart;
    Array<uint8_t> unsent;
    uint32_t unsentStart;
    bool finishing;                 // true once the client finished, until the socket's sending side is shut down
    bool finished;
    bool ended;                     // true once the peer ended
    bool writableOwed;              // true if bytes were written since the queue last drained
    bool dead;                      // true once destroyed; the loop frees it after the callbacks that are running
};

struct Task {
    PrNetTaskFn fn;
    void* PR_NULLABLE context;
};

// `mutex` guards the fields after it, which other threads reach. The loop alone reaches the rest.
struct Network : PrNet {
    PrThread loop;
    PrThread resolver;
    SocketsSystem* system;
    Array<Stream*> streams;
    Array<SocketsPollEntry> polls;
    Array<Stream*> polled;          // the stream of each entry of `polls`
    Array<Task> running;
    bool stopSeen;                  // true once the loop saw `stopping`, after which it no longer waits
    PrMutex mutex;
    PrCondition jobsChanged;
    Array<Task> tasks;
    Array<ResolveJob*> jobs;
    bool stopping;                  // the loop exits once the tasks posted before run
    bool resolverStopping;
};

void deliver(Stream* stream, PrNetStreamEvent event) {
    if (!stream->dead && stream->event != nullptr)
        stream->event(stream->context, stream, event);
}

void closeSocket(Stream* stream) {
    if (stream->socket != kSocketsHandleNone) {
        socketsClose(stream->socket);
        stream->socket = kSocketsHandleNone;
    }
}

void fail(Stream* stream, PrNetError error) {
    closeSocket(stream);
    stream->state = StreamState_Failed;
    stream->failure = error;
    deliver(stream, PrNetStreamEvent_Fail);
}

// Starts connecting to the next address, or fails once none is left.
void connectNext(Stream* stream) {
    while (stream->nextAddress < stream->addresses.count) {
        const SocketsAddress& address = stream->addresses.data[stream->nextAddress++];
        SocketsHandle socket = socketsConnect(address, &stream->lastAttempt);
        if (socket != kSocketsHandleNone) {
            stream->socket = socket;
            stream->state = StreamState_Connecting;
            return;
        }
    }
    fail(stream, stream->lastAttempt);
}

// Delivers the result of a resolution on the loop, and frees the job.
void resolved(void* PR_NULLABLE context) {
    ResolveJob* job = static_cast<ResolveJob*>(context);
    Stream* stream = job->stream;
    if (stream != nullptr) {
        stream->job = nullptr;
        if (job->addresses.count == 0) {
            fail(stream, job->error);
        } else {
            stream->addresses = job->addresses;
            job->addresses = Array<SocketsAddress>{};
            connectNext(stream);
        }
    }
    job->addresses.reset();
    prMemoryFree(job);
}

void post(PrNet* base, PrNetTaskFn fn, void* PR_NULLABLE context) {
    Network* network = static_cast<Network*>(base);
    prMutexLock(&network->mutex);
    network->tasks.push(Task{fn, context});
    prMutexUnlock(&network->mutex);
    socketsSystemWake(network->system);
}

void serveResolver(void* PR_NULLABLE argument) {
    Network* network = static_cast<Network*>(argument);
    prMutexLock(&network->mutex);
    for (;;) {
        while (network->jobs.count == 0 && !network->resolverStopping)
            prConditionWait(&network->jobsChanged, &network->mutex);
        if (network->jobs.count == 0)
            break;
        ResolveJob* job = network->jobs.data[0];
        network->jobs.erase(0);
        prMutexUnlock(&network->mutex);
        job->error = socketsResolve(job->host, job->port, &job->addresses);
        post(network, resolved, job);
        prMutexLock(&network->mutex);
    }
    prMutexUnlock(&network->mutex);
}

// Reads what the socket has, up to the stream's limit, and delivers Readable and End.
void receive(Stream* stream) {
    bool waiting = stream->received.count > stream->receivedStart;
    bool arrived = false;
    bool ended = false;
    while (stream->received.count - stream->receivedStart < kReceivedMaxBytes) {
        if (stream->receivedStart != 0 && stream->receivedStart == stream->received.count) {
            stream->received.clear();
            stream->receivedStart = 0;
        }
        uint32_t at = stream->received.count;
        stream->received.resize(at + kReceiveChunkBytes);
        PrNetError error = PrNetError_None;
        int64_t got = socketsReceive(stream->socket, stream->received.data + at, kReceiveChunkBytes, &error);
        stream->received.resize(got > 0 ? at + uint32_t(got) : at);
        if (got > 0) {
            arrived = true;
            continue;
        }
        if (got == 0) {
            ended = true;
        } else if (error != PrNetError_None) {
            fail(stream, error);
            return;
        }
        break;
    }
    if (arrived && !waiting)
        deliver(stream, PrNetStreamEvent_Readable);
    if (ended && !stream->dead) {
        stream->ended = true;
        deliver(stream, PrNetStreamEvent_End);
    }
}

// Sends what the queue holds until the socket takes no more, and delivers Writable once the queue drains.
void flush(Stream* stream) {
    while (stream->unsentStart < stream->unsent.count) {
        PrNetError error = PrNetError_None;
        int64_t sent = socketsSend(stream->socket, stream->unsent.data + stream->unsentStart,
            stream->unsent.count - stream->unsentStart, &error);
        if (sent < 0) {
            if (error != PrNetError_None)
                fail(stream, error);
            return;
        }
        stream->unsentStart += uint32_t(sent);
    }
    stream->unsent.clear();
    stream->unsentStart = 0;
    if (stream->finishing && !stream->finished) {
        socketsShutdownSend(stream->socket);
        stream->finished = true;
    }
    if (stream->writableOwed) {
        stream->writableOwed = false;
        deliver(stream, PrNetStreamEvent_Writable);
    }
}

void connected(Stream* stream, SocketsEventFlags ready) {
    PrNetError error = socketsConnectResult(stream->socket, ready);
    if (error != PrNetError_None) {
        stream->lastAttempt = error;
        closeSocket(stream);
        connectNext(stream);
        return;
    }
    stream->state = StreamState_Open;
    deliver(stream, PrNetStreamEvent_Open);
}

void serve(Stream* stream, SocketsEventFlags ready) {
    if (stream->dead || stream->socket == kSocketsHandleNone)
        return;
    if (stream->state == StreamState_Connecting) {
        if ((ready & (SocketsEventFlag_Write | SocketsEventFlag_Error | SocketsEventFlag_HangUp)) != 0)
            connected(stream, ready);
        return;
    }
    if ((ready & (SocketsEventFlag_Read | SocketsEventFlag_HangUp | SocketsEventFlag_Error)) != 0 && !stream->ended)
        receive(stream);
    if (!stream->dead && stream->state == StreamState_Open && (ready & SocketsEventFlag_Write) != 0)
        flush(stream);
    if (!stream->dead && stream->state == StreamState_Open && (ready & SocketsEventFlag_Error) != 0)
        fail(stream, socketsError(stream->socket));
}

void freeStream(Stream* stream) {
    stream->addresses.reset();
    stream->received.reset();
    stream->unsent.reset();
    prMemoryFree(stream);
}

// Frees the streams destroyed since the last sweep.
void sweep(Network* network) {
    for (uint32_t i = 0; i < network->streams.count;) {
        Stream* stream = network->streams.data[i];
        if (stream->dead) {
            freeStream(stream);
            network->streams.erase(i);
        } else {
            ++i;
        }
    }
}

// Runs the tasks posted so far. Returns false once the network stops and every task posted before has run.
bool runTasks(Network* network) {
    prMutexLock(&network->mutex);
    Array<Task> swapped = network->tasks;
    network->tasks = network->running;
    network->running = swapped;
    bool stopping = network->stopping;
    prMutexUnlock(&network->mutex);
    network->stopSeen = stopping;
    for (uint32_t i = 0; i < network->running.count; ++i)
        network->running.data[i].fn(network->running.data[i].context);
    bool ran = network->running.count != 0;
    network->running.clear();
    return !stopping || ran;
}

void serveLoop(void* PR_NULLABLE argument) {
    Network* network = static_cast<Network*>(argument);
    while (runTasks(network)) {
        sweep(network);
        network->polls.clear();
        network->polled.clear();
        for (uint32_t i = 0; i < network->streams.count; ++i) {
            Stream* stream = network->streams.data[i];
            if (stream->socket == kSocketsHandleNone)
                continue;
            SocketsEventFlags events = 0;
            if (stream->state == StreamState_Connecting) {
                events = SocketsEventFlag_Write;
            } else {
                if (!stream->ended && stream->received.count - stream->receivedStart < kReceivedMaxBytes)
                    events |= SocketsEventFlag_Read;
                if (stream->unsentStart < stream->unsent.count || (stream->finishing && !stream->finished))
                    events |= SocketsEventFlag_Write;
            }
            // A socket that waits for nothing is left out: WSAPoll reports a hang-up for a socket whose peer ended
            // whatever it waits for, so the loop would never block while the stream holds as much as it may.
            if (events == 0)
                continue;
            network->polls.push(SocketsPollEntry{.handle = stream->socket, .events = events});
            network->polled.push(stream);
        }
        if (!socketsSystemPoll(network->system, network->polls.data, network->polls.count, !network->stopSeen))
            continue;
        for (uint32_t i = 0; i < network->polled.count; ++i) {
            SocketsEventFlags ready = network->polls.data[i].ready;
            if (ready != 0)
                serve(network->polled.data[i], ready);
        }
        sweep(network);
    }
    sweep(network);
}

// Stream functions

void destroyStream(PrNetStream* base) {
    Stream* stream = static_cast<Stream*>(base);
    PR_ASSERT(!stream->dead);
    closeSocket(stream);
    if (stream->job != nullptr)
        stream->job->stream = nullptr;
    stream->job = nullptr;
    stream->dead = true;
}

size_t readStream(PrNetStream* base, uint8_t* output, size_t capacity) {
    Stream* stream = static_cast<Stream*>(base);
    uint32_t available = stream->received.count - stream->receivedStart;
    uint32_t taken = capacity < available ? uint32_t(capacity) : available;
    prMemCopy(output, stream->received.data + stream->receivedStart, taken);
    stream->receivedStart += taken;
    if (stream->receivedStart == stream->received.count) {
        stream->received.clear();
        stream->receivedStart = 0;
    }
    return taken;
}

void writeStream(PrNetStream* base, PrByteView bytes) {
    Stream* stream = static_cast<Stream*>(base);
    if (stream->state == StreamState_Failed || stream->finishing)
        return;
    PR_ASSERT(bytes.size <= UINT32_MAX - stream->unsent.count);
    stream->unsent.append(bytes.data, uint32_t(bytes.size));
    stream->writableOwed = true;
}

PrStatus finishStream(PrNetStream* base) {
    static_cast<Stream*>(base)->finishing = true;
    return PrStatus_Ok;
}

size_t queuedStream(const PrNetStream* base) {
    const Stream* stream = static_cast<const Stream*>(base);
    return stream->unsent.count - stream->unsentStart;
}

PrNetError errorStream(const PrNetStream* base) {
    return static_cast<const Stream*>(base)->failure;
}

// Network functions

void destroyNetwork(PrNet* base) {
    Network* network = static_cast<Network*>(base);
    // The resolver stops first, so that the results it posts run on the loop before the loop stops.
    prMutexLock(&network->mutex);
    network->resolverStopping = true;
    prConditionBroadcast(&network->jobsChanged);
    prMutexUnlock(&network->mutex);
    prThreadJoin(&network->resolver);
    prMutexLock(&network->mutex);
    network->stopping = true;
    prMutexUnlock(&network->mutex);
    socketsSystemWake(network->system);
    prThreadJoin(&network->loop);
    PR_ASSERT(network->streams.count == 0);
    network->streams.reset();
    network->polls.reset();
    network->polled.reset();
    network->running.reset();
    network->tasks.reset();
    network->jobs.reset();
    socketsSystemDestroy(network->system);
    prConditionReset(&network->jobsChanged);
    prMutexReset(&network->mutex);
    prMemoryFree(network);
}

bool isValidHost(PrStringView host) {
    if (host.size == 0 || host.size > kHostMaxBytes)
        return false;
    for (size_t i = 0; i < host.size; ++i) {
        char c = host.data[i];
        if (c == 0 || c == '[' || c == ']' || c == '/' || c == ' ')
            return false;
    }
    return true;
}

PrStatus connectNetwork(PrNet* base, const PrNetConnectDesc* desc, PrNetStream* PR_NULLABLE* PR_NONNULL out) {
    Network* network = static_cast<Network*>(base);
    if (!isValidHost(desc->host))
        return PrStatus_InvalidData;
    Stream* stream = allocZeroed<Stream>();
    stream->net = network;
    stream->event = desc->event;
    stream->context = desc->context;
    stream->destroy = destroyStream;
    stream->read = readStream;
    stream->write = writeStream;
    stream->finish = finishStream;
    stream->queued = queuedStream;
    stream->error = errorStream;
    stream->socket = kSocketsHandleNone;
    stream->state = StreamState_Resolving;
    stream->lastAttempt = PrNetError_Unreachable;
    ResolveJob* job = allocZeroed<ResolveJob>();
    job->network = network;
    job->stream = stream;
    prMemCopy(job->host, desc->host.data, desc->host.size);
    job->port = desc->port;
    stream->job = job;
    network->streams.push(stream);
    prMutexLock(&network->mutex);
    network->jobs.push(job);
    prConditionSignal(&network->jobsChanged);
    prMutexUnlock(&network->mutex);
    *out = stream;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrNet* prNetSocketsCreate(void) {
    Network* network = allocZeroed<Network>();
    network->destroy = destroyNetwork;
    network->post = post;
    network->connect = connectNetwork;
    network->system = socketsSystemCreate();
    prMutexInit(&network->mutex);
    prConditionInit(&network->jobsChanged);
    prThreadStart(&network->resolver, serveResolver, network);
    prThreadStart(&network->loop, serveLoop, network);
    return network;
}
