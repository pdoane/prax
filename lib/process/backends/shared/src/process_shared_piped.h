// A process whose standard streams are pipes that a thread of its backend serves: the bytes that the caller wrote and
// were not sent, and the bytes that arrived and were not read, which the caller's thread and the backend's thread
// share. A backend's process starts with it.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"
#include "pr/process.h"
#include "pr/process_backend.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The most bytes of a stream that arrived and were not read. A process stops reading the stream's pipe while it holds
// this many.
constexpr uint32_t kProcessReceivedMaxBytes = 1024 * 1024;

// The bytes that one read of a pipe takes at most.
constexpr uint32_t kProcessReadChunkBytes = 64 * 1024;

// Bytes that arrived on a stream: `bytes` from `start` holds what was not read.
struct ProcessReceived {
    RawArray<uint8_t> bytes;
    uint32_t start;
    bool ended;                     // the pipe reached its end, or the stream is not a pipe
};

struct ProcessPiped;

// Wakes the backend's thread: input was queued or closed, or a stream has room again.
using ProcessPipedNotifyFn = void (*)(ProcessPiped* process);

// `mutex` guards the fields after it, which the caller's thread and the backend's thread reach. The caller's thread
// alone reaches the fields before it.
struct ProcessPiped : PrProcess {
    ProcessPipedNotifyFn notify;
    PrProcessWakeFn PR_NULLABLE wake;
    void* PR_NULLABLE wakeContext;
    PrProcessReceiveFn PR_NULLABLE receive;
    void* PR_NULLABLE receiveContext;

    PrMutex mutex;
    ProcessReceived received[2];    // by PrProcessStream
    RawArray<uint8_t> queued;       // written and not yet sent, from `queuedStart`
    uint32_t queuedStart;
    bool inputClosing;              // the caller closed the input, which closes once what is queued is sent
    bool inputBroken;               // the process stopped reading its input, so writes are dropped
    bool exited;
    bool signaled;
    int32_t code;
};

// Starts the shared state of `process` for `desc`, and sets the members of its table that read and write the state. The
// backend sets `destroy` and `kill`.
void processPipedInit(ProcessPiped* process, const PrProcessDesc* desc, ProcessPipedNotifyFn notify);

// Frees the shared state, once the backend's thread has stopped.
void processPipedReset(ProcessPiped* process);

// Passes `size` bytes that arrived on `stream` to the receive function, if the process has one. Otherwise appends them
// to the stream, and calls the wake function if the caller had read everything before them. Returns true if the stream
// still has room, so that the pipe is read again.
bool processPipedReceive(ProcessPiped* process, PrProcessStream stream, const uint8_t* data, size_t size);

// Returns true if `stream` holds less than kProcessReceivedMaxBytes that were not read.
bool processPipedHasRoom(ProcessPiped* process, PrProcessStream stream);

// Records that `stream`'s pipe ended, and calls the wake function.
void processPipedEnd(ProcessPiped* process, PrProcessStream stream);

// Records how the process exited, and calls the wake function.
void processPipedExit(ProcessPiped* process, bool signaled, int32_t code);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
