// Threads and conditions through the platform.

#include "pr/thread.h"

#include "pr/assert.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_THREAD_START)
#error "a build without the default platform binds PR_PLATFORM_THREAD_START"
#endif

void prThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context) {
    if (!PR_PLATFORM_THREAD_START(thread, run, context))
        PR_FATAL("cannot start a thread");
}

void prThreadJoin(PrThread* thread) {
    PR_PLATFORM_THREAD_JOIN(thread);
}

uint32_t prThreadHardwareCount(void) {
    uint32_t count = PR_PLATFORM_THREAD_HARDWARE_COUNT();
    return count != 0 ? count : 1;
}

void prThreadSleep(double milliseconds) {
    PR_PLATFORM_THREAD_SLEEP(milliseconds);
}

void prConditionInit(PrCondition* condition) {
    PR_PLATFORM_CONDITION_INIT(condition);
}

void prConditionReset(PrCondition* condition) {
    PR_PLATFORM_CONDITION_RESET(condition);
}

void prConditionWait(PrCondition* condition, PrMutex* mutex) {
    PR_PLATFORM_CONDITION_WAIT(condition, mutex);
}

bool prConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds) {
    return PR_PLATFORM_CONDITION_WAIT_FOR(condition, mutex, milliseconds > 0 ? milliseconds : 0);
}

void prConditionSignal(PrCondition* condition) {
    PR_PLATFORM_CONDITION_SIGNAL(condition);
}

void prConditionBroadcast(PrCondition* condition) {
    PR_PLATFORM_CONDITION_BROADCAST(condition);
}
