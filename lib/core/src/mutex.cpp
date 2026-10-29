// Mutexes through the platform.

#include "pr/mutex.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_THREAD_START)
#error "a build without the default platform binds PR_PLATFORM_THREAD_START"
#endif

void prMutexInit(PrMutex* mutex) {
    PR_PLATFORM_MUTEX_INIT(mutex);
}

void prMutexReset(PrMutex* mutex) {
    PR_PLATFORM_MUTEX_RESET(mutex);
}

void prMutexLock(PrMutex* mutex) {
    PR_PLATFORM_MUTEX_LOCK(mutex);
}

void prMutexUnlock(PrMutex* mutex) {
    PR_PLATFORM_MUTEX_UNLOCK(mutex);
}
