#include "pr/atomic.h"
#include "pr/notifier.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// An observer that counts its calls, and may unobserve itself from its call.
struct Counter {
    PrNotifier* notifier = nullptr;
    uint64_t id = 0;
    uint32_t calls = 0;
    bool leaves = false;

    static void called(void* PR_NULLABLE context) {
        Counter* counter = static_cast<Counter*>(context);
        ++counter->calls;
        if (counter->leaves)
            prNotifierUnobserve(counter->notifier, counter->id);
    }
};

// What the observer of a call that runs long records.
struct Slow {
    Atomic<uint32_t> entered;
    Atomic<uint32_t> finished;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a notifier counts its changes and tells each observer") {
    PrNotifier* notifier = prNotifierCreate();
    CHECK(prNotifierCount(notifier) == 0);
    prNotifierNotify(notifier);
    CHECK(prNotifierCount(notifier) == 1);

    Counter a{.notifier = notifier};
    Counter b{.notifier = notifier};
    a.id = prNotifierObserve(notifier, Counter::called, &a);
    b.id = prNotifierObserve(notifier, Counter::called, &b);
    CHECK(a.id != 0);
    CHECK(a.id != b.id);
    prNotifierNotify(notifier);
    CHECK(a.calls == 1);
    CHECK(b.calls == 1);
    CHECK(prNotifierCount(notifier) == 2);

    prNotifierUnobserve(notifier, a.id);
    prNotifierNotify(notifier);
    CHECK(a.calls == 1);
    CHECK(b.calls == 2);
    prNotifierUnobserve(notifier, b.id);
    prNotifierRelease(notifier);
}

TEST_CASE("an observer may unobserve itself during its call") {
    PrNotifier* notifier = prNotifierCreate();
    Counter leaving{.notifier = notifier, .leaves = true};
    Counter staying{.notifier = notifier};
    leaving.id = prNotifierObserve(notifier, Counter::called, &leaving);
    staying.id = prNotifierObserve(notifier, Counter::called, &staying);
    prNotifierNotify(notifier);
    prNotifierNotify(notifier);
    CHECK(leaving.calls == 1);
    CHECK(staying.calls == 2);
    prNotifierUnobserve(notifier, staying.id);
    prNotifierRelease(notifier);
}

TEST_CASE("an unobserve waits for a call that is running on another thread") {
    if (!prThreadSupported())
        return;
    Slow slow{};
    PrNotifier* notifier = prNotifierCreate();
    uint64_t id = prNotifierObserve(notifier, [](void* PR_NULLABLE context) {
        Slow* held = static_cast<Slow*>(context);
        held->entered.store(1);
        prThreadSleep(50);
        held->finished.store(1);
    }, &slow);
    PrThread thread{};
    prThreadStart(&thread, [](void* PR_NULLABLE context) { prNotifierNotify(static_cast<PrNotifier*>(context)); }, notifier);
    while (slow.entered.load() == 0)
        prThreadSleep(1);
    prNotifierUnobserve(notifier, id);
    CHECK(slow.finished.load() == 1);
    prThreadJoin(&thread);
    prNotifierRelease(notifier);
}
