// Task pools: threads that share the iterations of a parallel loop with the thread that runs it, and that run jobs.

#include "pr/task.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#if defined(PR_TASK_HAS_BUILTIN)
#include "pr/task_builtin.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrTaskPoolBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrTaskPoolBackend kBuilt[] = {
#if defined(PR_TASK_HAS_BUILTIN)
    PrTaskPoolBackend_Builtin,
#endif
    PrTaskPoolBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTaskPoolBackend prTaskPoolBackendFromName(PrStringView name) {
    for (PrTaskPoolBackend backend = 0; backend < PrTaskPoolBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrTaskPoolBackend_Count;
}

PrStringView prTaskPoolBackendName(PrTaskPoolBackend backend) {
    return backend < PrTaskPoolBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prTaskPoolBackendIsBuilt(PrTaskPoolBackend backend) {
    for (PrTaskPoolBackend built : kBuilt) {
        if (built != PrTaskPoolBackend_Default && (built == backend || backend == PrTaskPoolBackend_Default))
            return true;
    }
    return false;
}

PrTaskPool* PR_NULLABLE prTaskPoolCreate(PrTaskPoolBackend backend, uint32_t threads) {
    switch (backend == PrTaskPoolBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_TASK_HAS_BUILTIN)
        case PrTaskPoolBackend_Builtin: return prTaskBuiltinCreate(threads);
#endif
        default: (void)threads; return nullptr;
    }
}

void prTaskPoolDestroy(PrTaskPool* PR_NULLABLE pool) {
    if (pool != nullptr && pool->destroy != nullptr)
        pool->destroy(pool);
}

uint32_t prTaskPoolWidth(const PrTaskPool* pool) {
    uint32_t width = pool->width(pool);
    PR_ASSERT(width >= 1);
    return width;
}

void prTaskPoolRun(PrTaskPool* pool, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context) {
    if (count == 0)
        return;
    uint32_t most = prTaskPoolWidth(pool);
    if (width > most)
        width = most;
    if (width > count)
        width = count;
    if (width <= 1) {
        for (uint32_t i = 0; i < count; ++i)
            fn(context, i, 0);
        return;
    }
    pool->run(pool, count, width, fn, context);
}

void prTaskPoolPost(PrTaskPool* pool, PrTaskJobFn fn, void* PR_NULLABLE context) {
    pool->post(pool, fn, context);
}
