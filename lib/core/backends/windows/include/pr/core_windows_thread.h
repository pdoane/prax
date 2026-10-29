// The Windows backend of threads. Its threads are Win32 threads, its mutexes slim reader/writer locks held exclusively,
// and its conditions Win32 condition variables. It serves Windows, and it is the default binding of threads there.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Starts a thread that calls `run` with `context`, and returns false if it cannot.
bool prCoreWindowsThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context);

// Waits for the thread to return.
void prCoreWindowsThreadJoin(PrThread* thread);

// Returns the number of threads that the machine runs at once, across its processor groups, which is at least 1.
uint32_t prCoreWindowsThreadHardwareCount(void);

// Suspends the calling thread for about `milliseconds`, on a high-resolution timer.
void prCoreWindowsThreadSleep(double milliseconds);

// Implement prMutexInit, prMutexReset, prMutexLock and prMutexUnlock.
void prCoreWindowsMutexInit(PrMutex* mutex);
void prCoreWindowsMutexReset(PrMutex* mutex);
void prCoreWindowsMutexLock(PrMutex* mutex);
void prCoreWindowsMutexUnlock(PrMutex* mutex);

// Implement prConditionInit, prConditionReset, prConditionWait, prConditionSignal and prConditionBroadcast.
void prCoreWindowsConditionInit(PrCondition* condition);
void prCoreWindowsConditionReset(PrCondition* condition);
void prCoreWindowsConditionWait(PrCondition* condition, PrMutex* mutex);
void prCoreWindowsConditionSignal(PrCondition* condition);
void prCoreWindowsConditionBroadcast(PrCondition* condition);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
