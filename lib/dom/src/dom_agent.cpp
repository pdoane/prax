// Agents (html/#similar-origin-window-agent): the state that the windows of one event loop share.

#include "dom/src/dom_agent.h"

#include "pr/assert.h"
#include "pr/memory.h"
#include "pr/task.h"

#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// The maximum number of threads that a restyle divides its work among if the options leave the number to the agent.
constexpr uint32_t kMaxRestyleThreads = 8;

// Runs the task posted first that has not run, and returns true, or returns false if none waits.
bool runPosted(PrDomAgent* agent) {
    if (agent->takenAt == agent->taken.count) {
        agent->taken.clear();
        agent->takenAt = 0;
        prMutexLock(&agent->postMutex);
        Array<DomAgentTask> swapped = agent->taken;
        agent->taken = agent->posted;
        agent->posted = swapped;
        prMutexUnlock(&agent->postMutex);
        if (agent->taken.count == 0)
            return false;
    }
    DomAgentTask task = agent->taken.data[agent->takenAt++];
    bool outer = agent->runningTask;
    agent->runningTask = true;
    task.fn(task.context);
    agent->runningTask = outer;
    agent->postedCount.fetchSub(1);
    return true;
}

// Runs at most one task: a posted task, or else a task of the global whose turn it is. A task that spins the event loop
// runs one during its own, and is still running afterward.
bool runNextTask(PrDomAgent* agent) {
    if (runPosted(agent))
        return true;
    // Each global whose task can run takes its turn, from the one after the global that ran last.
    uint32_t count = agent->globals.count;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t i = (agent->nextGlobal + k) % count;
        DomAgentGlobal g = agent->globals.data[i];
        if (g.tasks->delay(g.global) != 0)
            continue;
        bool outer = agent->runningTask;
        agent->runningTask = true;
        bool ran = g.tasks->run(g.global);
        agent->runningTask = outer;
        if (!ran)
            continue;
        // The task may have added or removed globals. Next time, the global after this one in the list goes first.
        uint32_t at = 0;
        while (at < agent->globals.count && agent->globals.data[at].global != g.global)
            ++at;
        agent->nextGlobal = at < agent->globals.count ? at + 1 : 0;
        return true;
    }
    return false;
}

} // namespace

void domAgentInit(PrDomAgent* agent, const PrDomAgentOptions& options) {
    agent->options = options;
    prMutexInit(&agent->postMutex);
    agent->postedCount.store(0);
}

void domAgentAddGlobal(PrDomAgent* agent, void* global, const DomGlobalTasks* tasks) {
    agent->globals.push(DomAgentGlobal{global, tasks});
}

void domAgentRemoveGlobal(PrDomAgent* agent, void* global) {
    for (uint32_t i = 0; i < agent->globals.count; ++i) {
        if (agent->globals.data[i].global != global)
            continue;
        agent->globals.erase(i);
        if (agent->nextGlobal > i)
            --agent->nextGlobal;
        return;
    }
}

double domAgentNextTaskDelay(const PrDomAgent* agent) {
    if (agent->postedCount.load() != 0)
        return 0;
    double next = -1;
    for (uint32_t i = 0; i < agent->globals.count; ++i) {
        const DomAgentGlobal& g = agent->globals.data[i];
        double delay = g.tasks->delay(g.global);
        if (delay >= 0 && (next < 0 || delay < next))
            next = delay;
    }
    return next;
}

bool domAgentIsIdle(const PrDomAgent* agent) {
    if (agent->postedCount.load() != 0)
        return false;
    for (uint32_t i = 0; i < agent->globals.count; ++i) {
        const DomAgentGlobal& g = agent->globals.data[i];
        if (!g.tasks->idle(g.global))
            return false;
    }
    return true;
}

bool domAgentRunTask(PrDomAgent* agent) {
    return !agent->runningTask && runNextTask(agent);
}

bool domAgentSpinTask(PrDomAgent* agent) {
    return runNextTask(agent);
}

void domAgentPost(PrDomAgent* agent, PrDomAgentTaskFn fn, void* PR_NULLABLE context) {
    agent->postedCount.fetchAdd(1);
    prMutexLock(&agent->postMutex);
    agent->posted.push(DomAgentTask{fn, context});
    PrDomAgentWakeFn wake = agent->wake;
    void* wakeContext = agent->wakeContext;
    prMutexUnlock(&agent->postMutex);
    if (wake != nullptr)
        wake(wakeContext);
}

void domAgentSetWake(PrDomAgent* agent, PrDomAgentWakeFn PR_NULLABLE wake, void* PR_NULLABLE context) {
    prMutexLock(&agent->postMutex);
    agent->wake = wake;
    agent->wakeContext = context;
    prMutexUnlock(&agent->postMutex);
}

uint32_t domAgentRestyleWidth(PrDomAgent* agent) {
    PrTaskPool* pool = agent->options.restylePool;
    if (pool == nullptr)
        return 1;
    uint32_t available = prTaskPoolWidth(pool);
    uint32_t asked = agent->options.restyleThreads != 0 ? agent->options.restyleThreads : kMaxRestyleThreads;
    uint32_t width = available < asked ? available : asked;
    return width != 0 ? width : 1;
}

PrTaskPool* PR_NULLABLE domAgentRestylePool(PrDomAgent* agent) {
    return domAgentRestyleWidth(agent) > 1 ? agent->options.restylePool : nullptr;
}

PrLayoutWorker* domAgentLayoutWorker(PrDomAgent* agent) {
    if (agent->worker == nullptr) {
        agent->worker = prLayoutWorkerCreate();
        prLayoutWorkerSetDone(agent->worker, agent->layoutDone, agent->layoutDoneContext);
    }
    return agent->worker;
}

void domAgentDestroy(PrDomAgent* PR_NULLABLE agent) {
    if (agent == nullptr)
        return;
    while (runPosted(agent)) {
    }
    // Reactions that no microtask ran are dropped.
    PR_ASSERT(agent->queues.count == 0);
    for (uint32_t i = 0; i < agent->backup.count; ++i)
        domNodeUnhold(agent->backup.data[i]);
    PR_ASSERT(agent->constructed.count == 0);
    PR_ASSERT(agent->globals.count == 0);
    agent->globals.reset();
    agent->queues.reset();
    agent->backup.reset();
    agent->constructed.reset();
    agent->posted.reset();
    agent->taken.reset();
    prMutexReset(&agent->postMutex);
    prLayoutWorkerDestroy(agent->worker);
    prMemoryFree(agent);
}

} // namespace pr_impl
