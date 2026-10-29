// Agents (html/#similar-origin-window-agent): the state that the windows of one event loop share.
//
// An agent holds its custom element reactions stack, its event loop and the rendering pipeline of its documents. The
// event loop (html/#event-loop) runs the tasks of every global object on it in turn. The rendering pipeline consists of
// the threads that a restyle divides its work among while the agent's thread waits for them, and the layout worker that
// lays out the documents and records their scenes while the agent's thread continues. Every window of the agent,
// including the window of an iframe, shares them, so adding documents adds neither event loops nor pipeline threads.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/dom.h"
#include "pr/layout_worker.h"
#include "pr/mutex.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The callbacks through which an agent's event loop runs the tasks of one of its global objects. A global object, such
// as an HTML Window, keeps its own task queues and timers. Each callback is called with the global.
struct DomGlobalTasks {
    double (*delay)(void* global);      // returns milliseconds until a task can run: 0 if one can run now, -1 if none
    bool (*run)(void* global);          // runs at most one task, and returns true if one ran
    bool (*idle)(void* global);         // returns true if nothing is pending except animation frames
};

// A global object on an agent's event loop.
struct DomAgentGlobal {
    void* global;
    const DomGlobalTasks* tasks;
};

// A task that a thread posted to an agent (prDomAgentPost).
struct DomAgentTask {
    PrDomAgentTaskFn fn;
    void* PR_NULLABLE context;
};

// An agent (html/#similar-origin-window-agent). Each element on a queue of its custom element reactions stack
// (html/#custom-element-reactions-stack), or on its backup element queue, is held (domNodeHold) until its reactions
// have run.
struct DomAgent {
    Array<Array<PrDomElement* PR_NONNULL>> queues;
    Array<PrDomElement* PR_NONNULL> backup;

    // The elements that HTML element constructors created during a synchronous construction. Each is held until the
    // construction is done.
    Array<PrDomElement* PR_NONNULL> constructed;

    uint32_t constructing;   // number of synchronous constructions running
    bool processingBackup;   // html/#processing-the-backup-element-queue

    Array<DomAgentGlobal> globals;          // globals on the event loop, in the order they were added
    uint32_t nextGlobal;                    // global to look for a task in first next time, so that each takes its turn
    bool runningTask;

    PrDomAgentOptions options;
    PrLayoutWorker* PR_NULLABLE worker;     // created by the first job that a presentation posts
    PrDomAgentLayoutDoneFn PR_NULLABLE layoutDone;
    void* PR_NULLABLE layoutDoneContext;

    // The tasks that threads post. `postMutex` guards `posted` and the wake function, which other threads reach. The
    // agent's thread takes the posted tasks into `taken` and runs them from `takenAt`. `postedCount` counts the tasks
    // posted that have not run.
    PrMutex postMutex;
    Array<DomAgentTask> posted;
    PrDomAgentWakeFn PR_NULLABLE wake;
    void* PR_NULLABLE wakeContext;
    Array<DomAgentTask> taken;
    uint32_t takenAt;
    Atomic<uint32_t> postedCount;
};

// Initializes an agent whose storage is zeroed, with `options`.
void domAgentInit(PrDomAgent* agent, const PrDomAgentOptions& options);

// Adds a global object to the agent's event loop, or removes it.
void domAgentAddGlobal(PrDomAgent* agent, void* global, const DomGlobalTasks* tasks);
void domAgentRemoveGlobal(PrDomAgent* agent, void* global);

// Run the agent's event loop. prDomAgentNextTaskDelay, prDomAgentIsIdle, prDomAgentRunTask and prDomAgentSpinTask
// describe their contracts.
double domAgentNextTaskDelay(const PrDomAgent* agent);
bool domAgentIsIdle(const PrDomAgent* agent);
bool domAgentRunTask(PrDomAgent* agent);
bool domAgentSpinTask(PrDomAgent* agent);

// Post a task to the agent from any thread, and set the function that wakes its host. prDomAgentPost and
// prDomAgentSetWake describe their contracts.
void domAgentPost(PrDomAgent* agent, PrDomAgentTaskFn fn, void* PR_NULLABLE context);
void domAgentSetWake(PrDomAgent* agent, PrDomAgentWakeFn PR_NULLABLE wake, void* PR_NULLABLE context);

// Returns the number of threads that a restyle of the agent's documents divides its work among, including the agent's
// own thread: what the options ask for, no wider than the pool.
uint32_t domAgentRestyleWidth(PrDomAgent* agent);

// Returns the pool whose threads a restyle divides its work among, or null if a restyle runs on the agent's thread
// alone.
PrTaskPool* PR_NULLABLE domAgentRestylePool(PrDomAgent* agent);

// Returns the layout worker of the agent's documents, creating it on first use.
PrLayoutWorker* domAgentLayoutWorker(PrDomAgent* agent);

// Runs the tasks posted to the agent that have not run, then frees the agent and its threads, and drops the reactions
// on its backup element queue. The agent's reactions stack must be empty, no synchronous construction may be running,
// no presentation may use its threads, and no global may be on its event loop.
void domAgentDestroy(PrDomAgent* PR_NULLABLE agent);

} // namespace pr_impl

// C opaque types
struct PrDomAgent : pr_impl::DomAgent {};

PR_ASSUME_NONNULL_END
