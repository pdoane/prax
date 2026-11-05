// The threads backend for a build without threads (PR_PLATFORM_THREADS), such as the web without shared memory. The
// program runs on one thread. No thread starts, so a mutex is never contended, and locking it does nothing. The backend
// serves every target, and sleeps through the target's own family.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns false: no thread starts.
bool prCoreNoThreadsThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context);

// Does nothing, since no thread started.
void prCoreNoThreadsThreadJoin(PrThread* thread);

// Returns 1.
uint32_t prCoreNoThreadsThreadHardwareCount(void);

// Implement prMutexInit, prMutexReset, prMutexLock and prMutexUnlock, each of which does nothing.
void prCoreNoThreadsMutexInit(PrMutex* mutex);
void prCoreNoThreadsMutexReset(PrMutex* mutex);
void prCoreNoThreadsMutexLock(PrMutex* mutex);
void prCoreNoThreadsMutexUnlock(PrMutex* mutex);

// Implement prConditionInit, prConditionReset, prConditionSignal and prConditionBroadcast, each of which does nothing,
// and prConditionWait and prConditionWaitFor. Core never calls the waits, because no other thread could signal the
// condition (prThreadSupported).
void prCoreNoThreadsConditionInit(PrCondition* condition);
void prCoreNoThreadsConditionReset(PrCondition* condition);
void prCoreNoThreadsConditionWait(PrCondition* condition, PrMutex* mutex);
bool prCoreNoThreadsConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds);
void prCoreNoThreadsConditionSignal(PrCondition* condition);
void prCoreNoThreadsConditionBroadcast(PrCondition* condition);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
