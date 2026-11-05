// Layout workers: laying out trees and recording their scenes on a thread of their own, while the thread that changes
// the trees continues.
//
// A worker is the layout stage of a pipeline. It runs the jobs posted to it one at a time, in the order they were
// posted, whatever trees they are for. A job takes its tree from the caller until the worker is waited for, so the
// caller changes a tree and interns its records only while the worker has no job for it. In a build without threads
// (prThreadSupported), a worker has no thread, and the caller's thread runs each job before posting it returns.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_scene.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A thread that runs jobs in turn.
typedef struct PrLayoutWorker PrLayoutWorker;

// Called when a worker finishes the last job posted to it, at the point when waiting for the worker would not block. It
// is called on the worker's thread, or on the caller's thread in a build without threads. It must call no function of
// the worker.
typedef void (*PrLayoutWorkerDoneFn)(void* PR_NULLABLE context);

// Starts a worker's thread, which waits for jobs.
PrLayoutWorker* prLayoutWorkerCreate(void);

// Waits for the posted jobs, then ends the thread and frees the worker and the scenes that no one took.
void prLayoutWorkerDestroy(PrLayoutWorker* PR_NULLABLE worker);

// Posts a job that lays out `count` roots of `tree`, the viewport's first (prLayoutTreeLayout), and records its scene
// (prLayoutTreeScene). The job copies the roots. The tree belongs to the worker until prLayoutWorkerWait returns. The
// tree must have no job posted since the worker was last waited for.
void prLayoutWorkerPost(PrLayoutWorker* worker, PrLayoutTree* tree, const PrLayoutRoot* roots, uint32_t count);

// Waits for every posted job to finish. Does nothing if no job is running.
void prLayoutWorkerWait(PrLayoutWorker* worker);

// Returns true if a posted job has not finished, so that waiting would wait.
bool prLayoutWorkerIsBusy(const PrLayoutWorker* worker);

// Waits for the jobs, and returns the scene that the last job of `tree` recorded, with a reference that the caller
// holds. Returns null if no job of the tree has finished since its last scene was taken.
const PrLayoutScene* PR_NULLABLE prLayoutWorkerTakeScene(PrLayoutWorker* worker, const PrLayoutTree* tree);

// Returns how long the last finished job of `tree` took on the worker's thread, in milliseconds, or 0 if none has
// finished. Does not wait.
double prLayoutWorkerJobTime(const PrLayoutWorker* worker, const PrLayoutTree* tree);

// Waits for the jobs, and drops what the worker keeps for `tree`, which may then be destroyed.
void prLayoutWorkerForget(PrLayoutWorker* worker, const PrLayoutTree* tree);

// Sets the function called when the worker finishes its jobs from now on, or null for none. A worker starts with none.
void prLayoutWorkerSetDone(PrLayoutWorker* worker, PrLayoutWorkerDoneFn PR_NULLABLE done, void* PR_NULLABLE context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
