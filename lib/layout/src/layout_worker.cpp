// Layout workers: laying out trees and recording their scenes on a thread of their own, while the thread that changes
// the trees continues.

#include "pr/layout_worker.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/thread.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// A job posted and not yet run, with a copy of the roots that it lays out, which the job owns until it runs.
struct WorkerJob {
    PrLayoutTree* tree;
    PrLayoutRoot* roots;
    uint32_t rootCount;
};

// What a worker keeps for a tree's jobs.
struct WorkerResult {
    const PrLayoutTree* tree;
    const PrLayoutScene* PR_NULLABLE scene; // the last job's scene, until it is taken
    double jobTime;                         // how long the last job took, in milliseconds
};

// A layout worker. The fields after `mutex` are guarded by it.
struct LayoutWorker {
    PrThread thread;
    mutable PrMutex mutex;
    PrCondition changed;            // signaled when a job is posted, the jobs finish, or the worker is told to end
    RawArray<WorkerJob> jobs;       // posted, in order, from `next`
    uint32_t next;
    RawArray<WorkerResult> results;
    PrLayoutWorkerDoneFn PR_NULLABLE done;
    void* PR_NULLABLE doneContext;
    bool running;                   // true if a posted job has not finished
    bool ending;
};

// Returns what the worker keeps for `tree`, creating it if the worker keeps nothing.
WorkerResult& resultOf(LayoutWorker* w, const PrLayoutTree* tree) {
    for (uint32_t i = 0; i < w->results.count; ++i) {
        if (w->results.data[i].tree == tree)
            return w->results.data[i];
    }
    WorkerResult& made = w->results.emplace();
    made = WorkerResult{tree, nullptr, 0};
    return made;
}

// Lays the tree out and records its scene, which the job's thread hands over when it finishes.
const PrLayoutScene* runJob(PrLayoutTree* tree, const PrLayoutRoot* roots, uint32_t count) {
    prLayoutTreeLayout(tree, roots, count);
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    return scene;
}

// Runs the job without the lock, and keeps its scene and its time with the lock held again.
void runLocked(LayoutWorker* w, WorkerJob job) {
    prMutexUnlock(&w->mutex);
    PrTimePerf started = prTimePerfNow();
    const PrLayoutScene* scene = runJob(job.tree, job.roots, job.rootCount);
    prMemoryFree(job.roots);
    double took = prTimePerfMilliseconds(started, prTimePerfNow());
    prMutexLock(&w->mutex);
    WorkerResult& result = resultOf(w, job.tree);
    prLayoutSceneRelease(result.scene);
    result.scene = scene;
    result.jobTime = took;
}

void serve(void* PR_NULLABLE context) {
    LayoutWorker* w = static_cast<LayoutWorker*>(context);
    prMutexLock(&w->mutex);
    for (;;) {
        while (w->next == w->jobs.count && !w->ending)
            prConditionWait(&w->changed, &w->mutex);
        if (w->next == w->jobs.count)
            break;
        runLocked(w, w->jobs.data[w->next++]);
        if (w->next != w->jobs.count)
            continue;
        w->jobs.clear();
        w->next = 0;
        w->running = false;
        prConditionBroadcast(&w->changed);
        // The caller may post the next jobs as soon as it is notified, and posting waits for the lock.
        PrLayoutWorkerDoneFn done = w->done;
        void* doneContext = w->doneContext;
        if (done != nullptr) {
            prMutexUnlock(&w->mutex);
            done(doneContext);
            prMutexLock(&w->mutex);
        }
    }
    prMutexUnlock(&w->mutex);
}

// Waits for the posted jobs, with the lock held.
void waitLocked(LayoutWorker* w) {
    while (w->running)
        prConditionWait(&w->changed, &w->mutex);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrLayoutWorker : LayoutWorker {};

PrLayoutWorker* prLayoutWorkerCreate(void) {
    PrLayoutWorker* w = allocZeroed<PrLayoutWorker>();
    prMutexInit(&w->mutex);
    prConditionInit(&w->changed);
    if (prThreadSupported())
        prThreadStart(&w->thread, serve, w);
    return w;
}

void prLayoutWorkerDestroy(PrLayoutWorker* PR_NULLABLE worker) {
    if (worker == nullptr)
        return;
    prMutexLock(&worker->mutex);
    waitLocked(worker);
    worker->ending = true;
    prConditionBroadcast(&worker->changed);
    prMutexUnlock(&worker->mutex);
    if (prThreadSupported())
        prThreadJoin(&worker->thread);
    for (uint32_t i = 0; i < worker->results.count; ++i)
        prLayoutSceneRelease(worker->results.data[i].scene);
    worker->results.reset();
    worker->jobs.reset();
    prConditionReset(&worker->changed);
    prMutexReset(&worker->mutex);
    prMemoryFree(worker);
}

void prLayoutWorkerPost(PrLayoutWorker* worker, PrLayoutTree* tree, const PrLayoutRoot* roots, uint32_t count) {
    PR_ASSERT(count >= 1);
    PrLayoutRoot* copied = static_cast<PrLayoutRoot*>(prMemoryAlloc(count * sizeof(PrLayoutRoot)));
    prMemCopy(copied, roots, count * sizeof(PrLayoutRoot));
    prMutexLock(&worker->mutex);
    for (uint32_t i = worker->next; i < worker->jobs.count; ++i)
        PR_ASSERT(worker->jobs.data[i].tree != tree);
    // Without threads, the caller's thread runs the job before the post returns, and the worker is never busy.
    if (!prThreadSupported()) {
        runLocked(worker, WorkerJob{tree, copied, count});
        PrLayoutWorkerDoneFn done = worker->done;
        void* doneContext = worker->doneContext;
        prMutexUnlock(&worker->mutex);
        if (done != nullptr)
            done(doneContext);
        return;
    }
    worker->jobs.push(WorkerJob{tree, copied, count});
    worker->running = true;
    prConditionBroadcast(&worker->changed);
    prMutexUnlock(&worker->mutex);
}

void prLayoutWorkerWait(PrLayoutWorker* worker) {
    prMutexLock(&worker->mutex);
    waitLocked(worker);
    prMutexUnlock(&worker->mutex);
}

bool prLayoutWorkerIsBusy(const PrLayoutWorker* worker) {
    prMutexLock(&worker->mutex);
    bool running = worker->running;
    prMutexUnlock(&worker->mutex);
    return running;
}

const PrLayoutScene* PR_NULLABLE prLayoutWorkerTakeScene(PrLayoutWorker* worker, const PrLayoutTree* tree) {
    prMutexLock(&worker->mutex);
    waitLocked(worker);
    WorkerResult& result = resultOf(worker, tree);
    const PrLayoutScene* scene = result.scene;
    result.scene = nullptr;
    prMutexUnlock(&worker->mutex);
    return scene;
}

double prLayoutWorkerJobTime(const PrLayoutWorker* worker, const PrLayoutTree* tree) {
    prMutexLock(&worker->mutex);
    double time = 0;
    for (uint32_t i = 0; i < worker->results.count; ++i) {
        if (worker->results.data[i].tree == tree)
            time = worker->results.data[i].jobTime;
    }
    prMutexUnlock(&worker->mutex);
    return time;
}

void prLayoutWorkerForget(PrLayoutWorker* worker, const PrLayoutTree* tree) {
    prMutexLock(&worker->mutex);
    waitLocked(worker);
    for (uint32_t i = 0; i < worker->results.count; ++i) {
        if (worker->results.data[i].tree != tree)
            continue;
        prLayoutSceneRelease(worker->results.data[i].scene);
        worker->results.data[i] = worker->results.data[worker->results.count - 1];
        worker->results.pop();
        break;
    }
    prMutexUnlock(&worker->mutex);
}

void prLayoutWorkerSetDone(PrLayoutWorker* worker, PrLayoutWorkerDoneFn PR_NULLABLE done, void* PR_NULLABLE context) {
    prMutexLock(&worker->mutex);
    worker->done = done;
    worker->doneContext = context;
    prMutexUnlock(&worker->mutex);
}
