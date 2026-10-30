// The POSIX backend of threads. Its threads, mutexes and conditions are POSIX threads. It serves the Unix-like targets,
// and it is the default binding of threads on them.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Starts a thread that calls `run` with `context`, and returns false if it cannot.
bool prCorePosixThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context);

// Waits for the thread to return.
void prCorePosixThreadJoin(PrThread* thread);

// Returns the number of threads that the machine runs at once, which is at least 1.
uint32_t prCorePosixThreadHardwareCount(void);

// Suspends the calling thread for about `milliseconds`.
void prCorePosixThreadSleep(double milliseconds);

// Implement prMutexInit, prMutexReset, prMutexLock and prMutexUnlock.
void prCorePosixMutexInit(PrMutex* mutex);
void prCorePosixMutexReset(PrMutex* mutex);
void prCorePosixMutexLock(PrMutex* mutex);
void prCorePosixMutexUnlock(PrMutex* mutex);

// Implement prConditionInit, prConditionReset, prConditionWait, prConditionWaitFor, prConditionSignal and
// prConditionBroadcast. The wait of prCorePosixConditionWaitFor is zero or more milliseconds.
void prCorePosixConditionInit(PrCondition* condition);
void prCorePosixConditionReset(PrCondition* condition);
void prCorePosixConditionWait(PrCondition* condition, PrMutex* mutex);
bool prCorePosixConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds);
void prCorePosixConditionSignal(PrCondition* condition);
void prCorePosixConditionBroadcast(PrCondition* condition);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
