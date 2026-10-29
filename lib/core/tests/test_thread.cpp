#include "pr/thread.h"

#include "doctest/doctest.h"

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

TEST_CASE("a thread runs beside its starter, which waits on a condition for what it does") {
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
