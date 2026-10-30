// Fetch loads: whole responses fetched on the network's loop for another thread.
//
// A load fetches a whole response for a thread other than the network's loop, such as a page's thread or a window's.
// That thread is the load's owner. It creates and destroys the load, and receives the response through a queue that
// runs tasks on it in the order they were posted, such as an agent's (prDomAgentPost) or a cache's (prCachePost). The
// fetch runs on the network's loop, which reads the body as it arrives.

#pragma once

#include "pr/base.h"
#include "pr/fetch.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A task that a queue runs on the owner's thread.
typedef void (*PrFetchTaskFn)(void* PR_NULLABLE context);

// Posts `fn` to run with `context` on the owner's thread, after the tasks posted before it. Called on the network's
// loop.
typedef void (*PrFetchPostFn)(void* PR_NULLABLE queue, PrFetchTaskFn fn, void* PR_NULLABLE context);

// A fetch of a whole response for an owner thread.
typedef struct PrFetchLoad PrFetchLoad;

// How a load reaches its owner, and what it accepts.
typedef struct PrFetchLoadDesc {
    PrFetchPostFn post;
    void* PR_NULLABLE queue;                    // passed to `post`
    PrFetchResponseFn done;
    void* PR_NULLABLE context;                  // passed to `done`

    // The largest body that the load reads, in bytes, or zero for no limit. A larger body ends the load with Limit.
    uint64_t maxBodySize;
} PrFetchLoadDesc;

// Starts a load of what `request` asks for. The load copies the request. It calls `done` once, on the owner's thread,
// unless it is destroyed first. The status is Ok for a response, InvalidData for a URL or a referrer that does not
// parse, Io for a network error, and Limit for a body over the limit. A response whose status is not ok is still a
// response, which the owner checks (prFetchIsOkStatus).
PrFetchLoad* prFetchLoadCreate(PrFetchNetwork* fetcher, const PrFetchRequestDesc* request, const PrFetchLoadDesc* desc);

// Abandons the load if it has not called `done`, and frees it. `done` is not called after this call, which may be made
// during `done`. Called on the owner's thread. The load posts its cancellation to the network's loop, so the fetcher is
// destroyed by a task that is posted to the loop after this call.
void prFetchLoadDestroy(PrFetchLoad* PR_NULLABLE load);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
