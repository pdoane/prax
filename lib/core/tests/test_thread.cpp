#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A count that a thread raises and the test waits for, under a mutex.
struct Shared {
    PrMutex mutex;
    PrCondition changed;
    uint32_t count;
};

void raise(void* PR_NULLABLE context) {
    Shared* shared = static_cast<Shared*>(context);
    for (uint32_t i = 0; i < 1000; ++i) {
        prMutexLock(&shared->mutex);
        ++shared->count;
        prConditionBroadcast(&shared->changed);
        prMutexUnlock(&shared->mutex);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a thread runs beside its starter, which waits on a condition for what it does" *
        doctest::skip(!prThreadSupported())) {
    Shared shared{};
    prMutexInit(&shared.mutex);
    prConditionInit(&shared.changed);
    PrThread a{};
    PrThread b{};
    prThreadStart(&a, raise, &shared);
    prThreadStart(&b, raise, &shared);
    prMutexLock(&shared.mutex);
    while (shared.count < 2000)
        prConditionWait(&shared.changed, &shared.mutex);
    prMutexUnlock(&shared.mutex);
    prThreadJoin(&a);
    prThreadJoin(&b);
    CHECK(shared.count == 2000);
    prConditionReset(&shared.changed);
    prMutexReset(&shared.mutex);
}

TEST_CASE("a timed wait returns false once its time passes") {
    Shared shared{};
    prMutexInit(&shared.mutex);
    prConditionInit(&shared.changed);
    prMutexLock(&shared.mutex);
    CHECK_FALSE(prConditionWaitFor(&shared.changed, &shared.mutex, 5));
    CHECK_FALSE(prConditionWaitFor(&shared.changed, &shared.mutex, 0));
    CHECK_FALSE(prConditionWaitFor(&shared.changed, &shared.mutex, -1));
    prMutexUnlock(&shared.mutex);
    prConditionReset(&shared.changed);
    prMutexReset(&shared.mutex);
}

TEST_CASE("a timed wait returns true when it is signaled first" * doctest::skip(!prThreadSupported())) {
    Shared shared{};
    prMutexInit(&shared.mutex);
    prConditionInit(&shared.changed);
    prMutexLock(&shared.mutex);
    PrThread thread{};
    prThreadStart(&thread, raise, &shared);
    bool signaled = false;
    while (shared.count < 1000)
        signaled = prConditionWaitFor(&shared.changed, &shared.mutex, 60000) || signaled;
    prMutexUnlock(&shared.mutex);
    prThreadJoin(&thread);
    CHECK(signaled);
    prConditionReset(&shared.changed);
    prMutexReset(&shared.mutex);
}

TEST_CASE("a build without threads runs on one, whose mutexes are never contended" * doctest::skip(prThreadSupported())) {
    CHECK(prThreadHardwareCount() == 1);
    PrMutex mutex;
    prMutexInit(&mutex);
    prMutexLock(&mutex);
    prMutexUnlock(&mutex);
    prMutexReset(&mutex);
}
