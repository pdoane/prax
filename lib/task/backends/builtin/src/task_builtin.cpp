// The built-in task pool, which starts threads of its own.

#include "pr/task_builtin.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

// A job posted to the pool.
struct Job {
    PrTaskJobFn fn;
    void* PR_NULLABLE context;
};

// A loop has a number of seats, and wakes that many of the pool's threads. A thread that wakes to a new loop takes a
// seat if one is left, which numbers the thread within the loop, and then takes the loop's indices from `next` until
// none are left. Once the caller runs out of indices, it closes the remaining seats and waits for the threads that took
// a seat. A thread that wakes with no loop to join runs the oldest job. Each thread looks for a loop and a job again
// whenever it finishes one, so a wake that a loop takes leaves no job waiting. `mutex` guards the fields after it.
struct BuiltinPool : PrTaskPool {
    PrThread* PR_NULLABLE threads;
    uint32_t threadCount;
    Atomic<uint32_t> next;          // the loop's next index to take
    PrMutex mutex;
    PrCondition started;            // signaled when a loop starts or the pool ends
    PrCondition finished;           // signaled when the last of the threads running a loop finishes it
    PrTaskFn PR_NULLABLE fn;
    void* PR_NULLABLE context;
    uint32_t count;
    uint32_t loopWidth;             // the width of the loop running
    uint32_t loop;                  // the number of loops started
    uint32_t seats;                 // the number of the pool's threads that the loop still takes
    uint32_t running;               // the number of the pool's threads that took a seat and have not finished the loop
    Array<Job> jobs;                // the jobs posted and not yet started, oldest first
    bool ending;                    // set once the pool is destroyed, after which its threads end once no job is left
};

void take(BuiltinPool* pool, PrTaskFn fn, void* PR_NULLABLE context, uint32_t count, uint32_t thread) {
    for (uint32_t i = pool->next.fetchAdd(1); i < count; i = pool->next.fetchAdd(1))
        fn(context, i, thread);
}

void serve(void* PR_NULLABLE argument) {
    BuiltinPool* pool = static_cast<BuiltinPool*>(argument);
    prMutexLock(&pool->mutex);
    // The pool starts at loop 0, which no thread runs. This holds whether or not a loop starts before the thread does.
    uint32_t seen = 0;
    for (;;) {
        while (pool->loop == seen && pool->jobs.count == 0 && !pool->ending)
            prConditionWait(&pool->started, &pool->mutex);
        if (pool->loop == seen) {
            if (pool->jobs.count == 0)
                break;
            Job job = pool->jobs.data[0];
            pool->jobs.erase(0);
            prMutexUnlock(&pool->mutex);
            job.fn(job.context);
            prMutexLock(&pool->mutex);
            continue;
        }
        seen = pool->loop;
        if (pool->seats == 0)
            continue;
        // The first seat taken is thread 1, and the last is thread width - 1.
        uint32_t thread = pool->loopWidth - pool->seats;
        --pool->seats;
        ++pool->running;
        PrTaskFn fn = pool->fn;
        void* context = pool->context;
        uint32_t count = pool->count;
        prMutexUnlock(&pool->mutex);
        take(pool, fn, context, count, thread);
        prMutexLock(&pool->mutex);
        if (--pool->running == 0)
            prConditionSignal(&pool->finished);
    }
    prMutexUnlock(&pool->mutex);
}

void destroyPool(PrTaskPool* base) {
    BuiltinPool* pool = static_cast<BuiltinPool*>(base);
    prMutexLock(&pool->mutex);
    pool->ending = true;
    prConditionBroadcast(&pool->started);
    prMutexUnlock(&pool->mutex);
    for (uint32_t i = 0; i < pool->threadCount; ++i)
        prThreadJoin(&pool->threads[i]);
    PR_ASSERT(pool->jobs.count == 0);
    pool->jobs.reset();
    prMemoryFree(pool->threads);
    prConditionReset(&pool->finished);
    prConditionReset(&pool->started);
    prMutexReset(&pool->mutex);
    prMemoryFree(pool);
}

uint32_t poolWidth(const PrTaskPool* base) {
    return static_cast<const BuiltinPool*>(base)->threadCount + 1;
}

void run(PrTaskPool* base, uint32_t count, uint32_t width, PrTaskFn fn, void* PR_NULLABLE context) {
    BuiltinPool* pool = static_cast<BuiltinPool*>(base);
    prMutexLock(&pool->mutex);
    PR_ASSERT(pool->running == 0);
    pool->fn = fn;
    pool->context = context;
    pool->count = count;
    pool->loopWidth = width;
    pool->next.store(0);
    pool->seats = width - 1;
    ++pool->loop;
    for (uint32_t i = 0; i < pool->seats; ++i)
        prConditionSignal(&pool->started);
    prMutexUnlock(&pool->mutex);
    take(pool, fn, context, count, 0);
    // From here on, a thread that wakes finds no seat. The caller waits for the threads that took a seat, so the next
    // loop cannot start before they finish.
    prMutexLock(&pool->mutex);
    pool->seats = 0;
    while (pool->running != 0)
        prConditionWait(&pool->finished, &pool->mutex);
    prMutexUnlock(&pool->mutex);
}

void post(PrTaskPool* base, PrTaskJobFn fn, void* PR_NULLABLE context) {
    BuiltinPool* pool = static_cast<BuiltinPool*>(base);
    if (pool->threadCount == 0) {
        fn(context);
        return;
    }
    prMutexLock(&pool->mutex);
    pool->jobs.push(Job{fn, context});
    prConditionSignal(&pool->started);
    prMutexUnlock(&pool->mutex);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTaskPool* prTaskBuiltinCreate(uint32_t threads) {
    BuiltinPool* pool = allocZeroed<BuiltinPool>();
    pool->destroy = destroyPool;
    pool->width = poolWidth;
    pool->run = run;
    pool->post = post;
    pool->next.storeRelaxed(0);
    prMutexInit(&pool->mutex);
    prConditionInit(&pool->started);
    prConditionInit(&pool->finished);
    pool->threadCount = threads;
    if (threads != 0)
        pool->threads = static_cast<PrThread*>(prMemoryAllocZeroed(sizeof(PrThread) * threads));
    for (uint32_t i = 0; i < threads; ++i)
        prThreadStart(&pool->threads[i], serve, pool);
    return pool;
}
