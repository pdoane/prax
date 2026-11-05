// The threads backend for a build without threads.

#include "pr/core_no_threads_thread.h"

bool prCoreNoThreadsThreadStart(PrThread*, PrThreadRunFn, void* PR_NULLABLE) {
    return false;
}

void prCoreNoThreadsThreadJoin(PrThread*) {}

uint32_t prCoreNoThreadsThreadHardwareCount(void) {
    return 1;
}

void prCoreNoThreadsMutexInit(PrMutex*) {}

void prCoreNoThreadsMutexReset(PrMutex*) {}

void prCoreNoThreadsMutexLock(PrMutex*) {}

void prCoreNoThreadsMutexUnlock(PrMutex*) {}

void prCoreNoThreadsConditionInit(PrCondition*) {}

void prCoreNoThreadsConditionReset(PrCondition*) {}

void prCoreNoThreadsConditionWait(PrCondition*, PrMutex*) {}

bool prCoreNoThreadsConditionWaitFor(PrCondition*, PrMutex*, double) {
    return false;
}

void prCoreNoThreadsConditionSignal(PrCondition*) {}

void prCoreNoThreadsConditionBroadcast(PrCondition*) {}
