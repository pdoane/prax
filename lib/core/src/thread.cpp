// Threads and conditions through the platform.

#include "pr/thread.h"

#include "pr/assert.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_THREAD_START)
#error "a target without a default for the group binds PR_PLATFORM_THREAD_START"
#endif

bool prThreadSupported(void) {
    return PR_PLATFORM_THREAD_SUPPORTED();
}

void prThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context) {
    if (!PR_PLATFORM_THREAD_SUPPORTED())
        PR_FATAL("cannot start a thread in a build without threads");
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
    // With one thread, the state that the caller waits for cannot change.
    if (!PR_PLATFORM_THREAD_SUPPORTED())
        PR_FATAL("waits on a condition that no thread can signal");
    PR_PLATFORM_CONDITION_WAIT(condition, mutex);
}

bool prConditionWaitFor(PrCondition* condition, PrMutex* mutex, double milliseconds) {
    double bounded = milliseconds > 0 ? milliseconds : 0;
    // With one thread, no signal comes, so the wait lasts its whole time.
    if (!PR_PLATFORM_THREAD_SUPPORTED()) {
        PR_PLATFORM_THREAD_SLEEP(bounded);
        return false;
    }
    return PR_PLATFORM_CONDITION_WAIT_FOR(condition, mutex, bounded);
}

void prConditionSignal(PrCondition* condition) {
    PR_PLATFORM_CONDITION_SIGNAL(condition);
}

void prConditionBroadcast(PrCondition* condition) {
    PR_PLATFORM_CONDITION_BROADCAST(condition);
}
