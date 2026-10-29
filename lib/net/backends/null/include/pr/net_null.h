// The null network: it connects to nothing, and its loop runs on its owner's thread when the owner runs it.

#pragma once

#include "pr/base.h"
#include "pr/net.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a network that connects to nothing: prNetConnect returns NotSupported. Its loop runs only when its owner
// calls prNetNullRun, on the owner's thread. It carries layers that answer requests themselves, such as an intercepting
// fetcher (pr/fetch_intercept.h), so that a host that runs it between its own tasks sees the same order on every run.
// prNetDestroy runs the tasks still posted, on the calling thread.
PrNet* prNetNullCreate(void);

// Runs the loop of `net`, a network that prNetNullCreate made: the tasks posted to it, and the tasks that they post,
// until none is left. Returns true if a task ran. The calling thread is the loop's while the call runs, and a task must
// not call it.
bool prNetNullRun(PrNet* net);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
