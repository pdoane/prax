// Task pools: threads that share the iterations of a parallel loop with the thread that runs it, and that run jobs
// posted to them in the background, such as a store's writes to disk.
//
// A pool is a backend interface. A host that schedules work of its own, such as a game engine's job system, implements
// a pool over it, and the library's loops and jobs run on the host's threads. The built-in backend starts threads of
// its own (pr/task_builtin.h).

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A pool that runs the iterations of loops on several threads. Each backend provides its own constructor.
typedef struct PrTaskPool PrTaskPool;

// The function that a loop calls for each index. `thread` is the number of the calling thread within the loop, below
// the loop's width: 0 for the thread that runs the loop, and 1 or more for the threads that help it. Two calls with the
// same `thread` never run at once, so a loop can give each thread number scratch space of its own.
typedef void (*PrTaskFn)(void* PR_NULLABLE context, uint32_t index, uint32_t thread);

// A job that a pool runs once (prTaskPoolPost).
typedef void (*PrTaskJobFn)(void* PR_NULLABLE context);

// The function table of a pool, placed at the start of the backend's state. Each member implements the public function
// of the same name. A null `destroy` means there is nothing to free. Callers use the functions below, never these
// members.
//
// `run` is called only for a loop of at least two indices and a width from 2 to the pool's width, no more than the
// number of indices. It calls `fn` once with each index, on the caller's thread as thread 0 and on at most `width - 1`
// other threads, numbered from 1, and returns once every call has returned.
//
// `post` may be called from any thread, at any time, including from a job, and returns without waiting for the job.
struct PrTaskPool {
    void (*PR_NULLABLE destroy)(PrTaskPool* pool);
    uint32_t (*width)(const PrTaskPool* pool);
    void (*run)(PrTaskPool* pool, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context);
    void (*post)(PrTaskPool* pool, PrTaskJobFn fn, void* PR_NULLABLE context);
};

// The library's backends of pools, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrTaskPoolBackend_Default,
    PrTaskPoolBackend_Builtin,
    PrTaskPoolBackend_Count,
};
typedef uint8_t PrTaskPoolBackend;

// Returns the backend that `name` names, as prTaskPoolBackendName spells it, ignoring case, or Count if it names none.
PrTaskPoolBackend prTaskPoolBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prTaskPoolBackendName(PrTaskPoolBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prTaskPoolBackendIsBuilt(PrTaskPoolBackend backend);

// Creates a pool of `threads` threads by the backend, which may be zero, so that a loop is at most `threads + 1` wide.
// Returns null if the build does not have the backend.
PrTaskPool* PR_NULLABLE prTaskPoolCreate(PrTaskPoolBackend backend, uint32_t threads);

// Frees the pool, once the jobs posted to it have run. No loop may be running, and no job may be posted meanwhile.
void prTaskPoolDestroy(PrTaskPool* PR_NULLABLE pool);

// Returns the number of threads that can take part in a loop, including the thread that runs it. It is at least 1.
uint32_t prTaskPoolWidth(const PrTaskPool* pool);

// Calls `fn` with each index below `count`, in no particular order, and returns once every call has returned.
//
// The loop is at most `width` threads wide, and no wider than the pool or `count`. A loop one thread wide runs on the
// caller's thread alone. Only one thread may run loops on a pool, and it runs them one at a time.
void prTaskPoolRun(PrTaskPool* pool, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context);

// Calls `fn` with `context` once, on one of the pool's threads, some time after the call, which returns without waiting
// for it. Jobs may run in any order and at once, so work that must stay in order posts its next job when the last
// finishes. A job may block, as for reading or writing files, which keeps its thread from the pool's loops meanwhile.
// May be called from any thread.
void prTaskPoolPost(PrTaskPool* pool, PrTaskJobFn fn, void* PR_NULLABLE context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
