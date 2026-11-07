#include "pr/task.h"
#include "pr/task_backend.h"

#include "tools/harness/test.h"

#include <vector>

extern "C" int prTestTaskCApi(void);

namespace pr_impl {

namespace {

// A host's pool, which records the loops it is asked to run and runs each on the caller's thread.
struct HostPool : PrTaskPool {
    uint32_t poolWidth;
    std::vector<uint32_t> widths;   // the width of each loop that reached the backend
};

uint32_t hostWidth(const PrTaskPool* base) {
    return static_cast<const HostPool*>(base)->poolWidth;
}

void hostRun(PrTaskPool* base, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context) {
    HostPool* pool = static_cast<HostPool*>(base);
    pool->widths.push_back(width);
    for (uint32_t i = 0; i < count; ++i)
        fn(context, i, i % width);
}

// Runs a job at once, on the caller's thread.
void hostPost(PrTaskPool*, PrTaskJobFn fn, void* PR_NULLABLE context) {
    fn(context);
}

HostPool hostPool(uint32_t width) {
    HostPool pool;
    pool.destroyFn = nullptr;
    pool.widthFn = hostWidth;
    pool.runFn = hostRun;
    pool.postFn = hostPost;
    pool.poolWidth = width;
    return pool;
}

void sum(void* PR_NULLABLE context, uint32_t index, uint32_t) {
    *static_cast<uint32_t*>(context) += index;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestTaskCApi() == 0);
}

TEST_CASE("a host's pool runs the loops that are wider than one thread, no wider than it or their indices") {
    HostPool pool = hostPool(4);
    uint32_t total = 0;
    prTaskPoolRun(&pool, 10, 8, sum, &total);
    prTaskPoolRun(&pool, 3, 8, sum, &total);
    CHECK(total == 45 + 3);
    CHECK(pool.widths == std::vector<uint32_t>{4, 3});
}

TEST_CASE("a loop one thread wide, or of one index, runs on the caller without reaching the pool") {
    HostPool pool = hostPool(4);
    uint32_t total = 0;
    prTaskPoolRun(&pool, 10, 1, sum, &total);
    prTaskPoolRun(&pool, 1, 4, sum, &total);
    prTaskPoolRun(&pool, 0, 4, sum, &total);
    CHECK(total == 45);
    CHECK(pool.widths.empty());
    HostPool narrow = hostPool(1);
    prTaskPoolRun(&narrow, 10, 4, sum, &total);
    CHECK(narrow.widths.empty());
}

TEST_CASE("a job posted to a host's pool reaches it") {
    HostPool pool = hostPool(2);
    uint32_t total = 0;
    prTaskPoolPost(&pool, [](void* PR_NULLABLE context) { *static_cast<uint32_t*>(context) += 1; }, &total);
    CHECK(total == 1);
}

TEST_CASE("a pool with nothing to free is destroyed by doing nothing") {
    HostPool pool = hostPool(2);
    prTaskPoolDestroy(&pool);
    prTaskPoolDestroy(nullptr);
    CHECK(prTaskPoolWidth(&pool) == 2);
}
