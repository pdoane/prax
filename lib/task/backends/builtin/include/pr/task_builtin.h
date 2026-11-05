// The built-in task pool, which starts threads of its own.

#pragma once

#include "pr/base.h"
#include "pr/task.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a pool of `threads` threads, which may be zero, so that a loop is at most `threads + 1` wide. The threads
// wait without running until a loop or a job wakes them, and a loop wakes only as many as it is wide. A thread runs the
// jobs posted, in the order they were posted, whenever it is not running a loop. A pool of no threads runs a job on the
// thread that posts it, before prTaskPoolPost returns. In a build without threads (prThreadSupported), every pool has
// no threads.
PrTaskPool* prTaskBuiltinCreate(uint32_t threads);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
