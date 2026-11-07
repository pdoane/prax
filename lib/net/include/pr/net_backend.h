// The interface that network backends implement. Callers use the functions of pr/net.h, which call these. Only backends
// include this header.

#pragma once

#include "pr/base.h"
#include "pr/net.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a network's backend, at the start of its state. Each implements the function of pr/net.h that it is
// named for. A null `postBlockingFn` means that the loop is a thread of its own, which prNetPostBlocking waits for.
struct PrNet {
    void (*destroyFn)(PrNet* net);
    void (*postFn)(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);
    void (*PR_NULLABLE postBlockingFn)(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context);
    PrStatus (*connectFn)(PrNet* net, const PrNetConnectDesc* desc, PrNetStream* PR_NULLABLE* PR_NONNULL out);
};

// The functions of a stream's backend, at the start of its state. Each implements the function of pr/net.h that it is
// named for. A null `finishFn` means that the stream cannot end its sending side alone.
struct PrNetStream {
    void (*destroyFn)(PrNetStream* stream);
    void (*setEventFn)(PrNetStream* stream, PrNetStreamFn PR_NULLABLE event, void* PR_NULLABLE context);
    size_t (*readFn)(PrNetStream* stream, uint8_t* output, size_t capacity);
    void (*writeFn)(PrNetStream* stream, PrByteView bytes);
    PrStatus (*PR_NULLABLE finishFn)(PrNetStream* stream);
    size_t (*queuedFn)(const PrNetStream* stream);
    PrNetError (*errorFn)(const PrNetStream* stream);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
