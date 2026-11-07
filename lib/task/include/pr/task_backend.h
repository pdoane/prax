// The interface that task pool backends implement. Callers use the functions of pr/task.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/task.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a pool's backend, at the start of its state. Each implements the function of pr/task.h that it is
// named for. A null `destroyFn` means that there is nothing to free.
//
// `runFn` is called only for a loop of at least two indices and a width from 2 to the pool's width, no more than the
// number of indices. It calls `fn` once with each index, on the caller's thread as thread 0 and on at most `width - 1`
// other threads, numbered from 1, and returns once every call has returned.
//
// `postFn` may be called from any thread, at any time, including from a job, and returns without waiting for the job.
struct PrTaskPool {
    void (*PR_NULLABLE destroyFn)(PrTaskPool* pool);
    uint32_t (*widthFn)(const PrTaskPool* pool);
    void (*runFn)(PrTaskPool* pool, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context);
    void (*postFn)(PrTaskPool* pool, PrTaskJobFn fn, void* PR_NULLABLE context);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
