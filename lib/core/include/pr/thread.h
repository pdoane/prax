// Threads, and waiting between them, through the platform.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A thread that the program started. Its storage belongs to the platform. It must not be copied or moved while it runs.
typedef struct PrThread {
    uint64_t storage[2];
} PrThread;

// The function that a thread runs. It receives the context that prThreadStart was given.
typedef void (*PrThreadRunFn)(void* PR_NULLABLE context);

// Returns true if the platform starts threads. A build without them (PR_PLATFORM_THREADS) runs on one thread: starting
// a thread and waiting on a condition are fatal, since nothing else would run, and a mutex is never contended.
bool prThreadSupported(void);

// Starts a thread that calls `run` with `context`, and returns at once. A thread that cannot start is fatal. The
// thread's stack is as large as a main thread's on the platforms that the library runs on, 8 MiB, so that a thread runs
// what a main thread does, such as a script that QuickJS bounds by its stack size.
void prThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context);

// Waits for the thread's `run` to return. A thread is joined exactly once.
void prThreadJoin(PrThread* thread);

// Returns the number of threads that the machine runs at once, which is at least 1.
uint32_t prThreadHardwareCount(void);

// Suspends the calling thread for about `milliseconds`, which may be fractional.
void prThreadSleep(double milliseconds);

// A condition variable that threads wait on while they hold a mutex. Another thread signals it when the state that they
// wait for changes. Its storage belongs to the platform. It is initialized before it is shared, and reset after it is
// no longer shared. It cannot be copied or moved.
typedef struct PrCondition {
    uint64_t storage[8];
} PrCondition;

// Initializes `condition`.
void prConditionInit(PrCondition* condition);

// Frees what `condition` holds. No thread may wait on it.
void prConditionReset(PrCondition* condition);

// Unlocks `mutex`, which the caller holds, waits until the condition is signaled, and locks `mutex` again. A wait may
// return without a signal, so a waiter tests the state it waits for again.
void prConditionWait(PrCondition* condition, PrMutex* mutex);

// Waits as prConditionWait does, for at most `milliseconds`. Returns false if the time passed first. A wait of zero or
// less, or one that is not a number, returns false at once, having released and locked `mutex`.
bool prConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds);

// Wakes at least one of the threads waiting on the condition, if any are waiting.
void prConditionSignal(PrCondition* condition);

// Wakes the threads waiting on the condition.
void prConditionBroadcast(PrCondition* condition);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
