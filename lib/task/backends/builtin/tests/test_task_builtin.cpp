#include "pr/atomic.h"
#include "pr/task_builtin.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The calls of a loop: the number of calls for each index, and the threads that made them.
struct Calls {
    Atomic<uint32_t> counts[1000];
    Atomic<uint32_t> threads[8];    // by thread number, the number of calls that the thread made
};

void count(void* PR_NULLABLE context, uint32_t index, uint32_t thread) {
    Calls* calls = static_cast<Calls*>(context);
    calls->counts[index].fetchAdd(1);
    calls->threads[thread].fetchAdd(1);
}

// Returns the number of threads that made calls, and checks that none is numbered `width` or above.
uint32_t threadsUsed(const Calls& calls, uint32_t width) {
    uint32_t threads = 0;
    for (uint32_t t = 0; t < 8; ++t) {
        if (calls.threads[t].load() == 0)
            continue;
        CHECK(t < width);
        ++threads;
    }
    return threads;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a loop calls each index once, from the pool's threads and the caller's" *
        doctest::skip(!prThreadSupported())) {
    PrTaskPool* pool = prTaskBuiltinCreate(3);
    CHECK(prTaskPoolWidth(pool) == 4);
    for (uint32_t loop = 0; loop < 20; ++loop) {
        Calls calls{};
        prTaskPoolRun(pool, 1000, prTaskPoolWidth(pool), count, &calls);
        bool once = true;
        for (const Atomic<uint32_t>& c : calls.counts)
            once = once && c.load() == 1;
        CHECK(once);
        uint32_t total = 0;
        for (uint32_t t = 0; t < 4; ++t)
            total += calls.threads[t].load();
        CHECK(total == 1000);
    }
    prTaskPoolDestroy(pool);
}

TEST_CASE("a pool of no threads runs a loop on the caller's") {
    PrTaskPool* pool = prTaskBuiltinCreate(0);
    Calls calls{};
    prTaskPoolRun(pool, 10, 4, count, &calls);
    CHECK(calls.threads[0].load() == 10);
    CHECK(calls.counts[9].load() == 1);
    prTaskPoolDestroy(pool);
    CHECK(prThreadHardwareCount() >= 1);
}

TEST_CASE("a loop runs on no more threads than its width, each numbered below it") {
    PrTaskPool* pool = prTaskBuiltinCreate(7);
    for (uint32_t width = 1; width <= 3; ++width) {
        for (uint32_t loop = 0; loop < 20; ++loop) {
            Calls calls{};
            prTaskPoolRun(pool, 1000, width, count, &calls);
            CHECK(threadsUsed(calls, width) <= width);
            uint32_t total = 0;
            for (const Atomic<uint32_t>& t : calls.threads)
                total += t.load();
            CHECK(total == 1000);
        }
    }
    prTaskPoolDestroy(pool);
}

TEST_CASE("a loop of fewer indices than threads runs on no more threads than it has indices") {
    PrTaskPool* pool = prTaskBuiltinCreate(7);
    for (uint32_t loop = 0; loop < 50; ++loop) {
        Calls calls{};
        prTaskPoolRun(pool, 2, prTaskPoolWidth(pool), count, &calls);
        CHECK(threadsUsed(calls, 2) <= 2);
        CHECK(calls.counts[0].load() == 1);
        CHECK(calls.counts[1].load() == 1);
    }
    prTaskPoolDestroy(pool);
}

TEST_CASE("every job posted runs once, from any thread, before the pool is destroyed" *
        doctest::skip(!prThreadSupported())) {
    PrTaskPool* pool = prTaskBuiltinCreate(3);
    Atomic<uint32_t> runs{};
    auto job = [](void* PR_NULLABLE context) { static_cast<Atomic<uint32_t>*>(context)->fetchAdd(1); };
    PrThread poster;
    struct Posting {
        PrTaskPool* pool;
        Atomic<uint32_t>* runs;
        PrTaskJobFn job;
    } posting{pool, &runs, job};
    prThreadStart(&poster, [](void* PR_NULLABLE context) {
        Posting* p = static_cast<Posting*>(context);
        for (uint32_t i = 0; i < 100; ++i)
            prTaskPoolPost(p->pool, p->job, p->runs);
    }, &posting);
    for (uint32_t i = 0; i < 100; ++i)
        prTaskPoolPost(pool, job, &runs);
    prThreadJoin(&poster);
    prTaskPoolDestroy(pool);
    CHECK(runs.load() == 200);
}

TEST_CASE("a loop runs while a job blocks one of the pool's threads" * doctest::skip(!prThreadSupported())) {
    PrTaskPool* pool = prTaskBuiltinCreate(2);
    Atomic<uint32_t> release{};
    prTaskPoolPost(pool, [](void* PR_NULLABLE context) {
        Atomic<uint32_t>* until = static_cast<Atomic<uint32_t>*>(context);
        while (until->load() == 0)
            prThreadSleep(0.1);
    }, &release);
    Calls calls{};
    prTaskPoolRun(pool, 1000, 3, count, &calls);
    for (uint32_t i = 0; i < 1000; ++i)
        CHECK(calls.counts[i].load() == 1);
    release.store(1);
    prTaskPoolDestroy(pool);
}

TEST_CASE("a pool of no threads runs a job on the thread that posts it") {
    PrTaskPool* pool = prTaskBuiltinCreate(0);
    uint32_t runs = 0;
    prTaskPoolPost(pool, [](void* PR_NULLABLE context) { ++*static_cast<uint32_t*>(context); }, &runs);
    CHECK(runs == 1);
    prTaskPoolDestroy(pool);
}

TEST_CASE("in a build without threads, a pool has none, and runs its loops and jobs on the caller" *
        doctest::skip(prThreadSupported())) {
    PrTaskPool* pool = prTaskBuiltinCreate(3);
    CHECK(prTaskPoolWidth(pool) == 1);
    Calls calls{};
    prTaskPoolRun(pool, 10, 4, count, &calls);
    CHECK(calls.threads[0].load() == 10);
    uint32_t runs = 0;
    prTaskPoolPost(pool, [](void* PR_NULLABLE context) { ++*static_cast<uint32_t*>(context); }, &runs);
    CHECK(runs == 1);
    prTaskPoolDestroy(pool);
}
