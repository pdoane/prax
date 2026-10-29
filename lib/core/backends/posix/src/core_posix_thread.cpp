// The POSIX backend of threads.

#include "pr/core_posix_thread.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

namespace pr_impl {

namespace {

// The stack of each thread (prThreadStart). It is reserved, and the system commits it as the thread uses it.
constexpr size_t kStackBytes = size_t(8) << 20;

// The function and context that a thread runs. pthread passes this structure to the thread as its argument, and the
// thread frees it.
struct Start {
    PrThreadRunFn run;
    void* PR_NULLABLE context;
};

void* PR_NULLABLE begin(void* PR_NULLABLE argument) {
    Start copy = *static_cast<Start*>(argument);
    free(argument);
    copy.run(copy.context);
    return nullptr;
}

pthread_t* nativeOf(PrThread* thread) {
    static_assert(sizeof(pthread_t) <= sizeof(thread->storage));
    static_assert(alignof(pthread_t) <= alignof(PrThread));
    return reinterpret_cast<pthread_t*>(thread->storage);
}

pthread_mutex_t* nativeOf(PrMutex* mutex) {
    static_assert(sizeof(pthread_mutex_t) <= sizeof(mutex->storage));
    static_assert(alignof(pthread_mutex_t) <= alignof(PrMutex));
    return reinterpret_cast<pthread_mutex_t*>(mutex->storage);
}

pthread_cond_t* nativeOf(PrCondition* condition) {
    static_assert(sizeof(pthread_cond_t) <= sizeof(condition->storage));
    static_assert(alignof(pthread_cond_t) <= alignof(PrCondition));
    return reinterpret_cast<pthread_cond_t*>(condition->storage);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prCorePosixThreadStart(PrThread* thread, PrThreadRunFn run, void* PR_NULLABLE context) {
    Start* start = static_cast<Start*>(malloc(sizeof(Start)));
    if (start == nullptr)
        return false;
    *start = Start{run, context};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kStackBytes);
    int created = pthread_create(nativeOf(thread), &attributes, begin, start);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        free(start);
        return false;
    }
    return true;
}

void prCorePosixThreadJoin(PrThread* thread) {
    pthread_join(*nativeOf(thread), nullptr);
}

uint32_t prCorePosixThreadHardwareCount(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? uint32_t(count) : 1;
}

void prCorePosixThreadSleep(double milliseconds) {
    if (milliseconds > 0)
        usleep(useconds_t(milliseconds * 1000));
}

void prCorePosixMutexInit(PrMutex* mutex) {
    pthread_mutex_init(nativeOf(mutex), nullptr);
}

void prCorePosixMutexReset(PrMutex* mutex) {
    pthread_mutex_destroy(nativeOf(mutex));
}

void prCorePosixMutexLock(PrMutex* mutex) {
    pthread_mutex_lock(nativeOf(mutex));
}

void prCorePosixMutexUnlock(PrMutex* mutex) {
    pthread_mutex_unlock(nativeOf(mutex));
}

void prCorePosixConditionInit(PrCondition* condition) {
    pthread_cond_init(nativeOf(condition), nullptr);
}

void prCorePosixConditionReset(PrCondition* condition) {
    pthread_cond_destroy(nativeOf(condition));
}

void prCorePosixConditionWait(PrCondition* condition, PrMutex* mutex) {
    pthread_cond_wait(nativeOf(condition), nativeOf(mutex));
}

void prCorePosixConditionSignal(PrCondition* condition) {
    pthread_cond_signal(nativeOf(condition));
}

void prCorePosixConditionBroadcast(PrCondition* condition) {
    pthread_cond_broadcast(nativeOf(condition));
}
