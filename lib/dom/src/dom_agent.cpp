// Agents (html/#similar-origin-window-agent): the state that the windows of one event loop share.

#include "dom/src/dom_agent.h"

#include "pr/assert.h"
#include "pr/memory.h"
#include "pr/task.h"
#include "pr/time.h"

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

// Ends the agent's wait. Called with `postMutex` held.
void signal(PrDomAgent* agent) {
    agent->signaled = true;
    prConditionSignal(&agent->woken);
}

// The layout worker's done function: wakes the agent's wait, then calls the host's (prDomAgentSetLayoutDone).
void layoutDone(void* PR_NULLABLE context) {
    PrDomAgent* agent = static_cast<PrDomAgent*>(context);
    domAgentWake(agent);
    if (agent->layoutDone != nullptr)
        agent->layoutDone(agent->layoutDoneContext);
}

} // namespace

void domAgentInit(PrDomAgent* agent, const PrDomAgentOptions& options) {
    agent->options = options;
    prMutexInit(&agent->postMutex);
    prConditionInit(&agent->woken);
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
    signal(agent);
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

void domAgentAddHook(Array<DomAgentTask>* hooks, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    hooks->push(DomAgentTask{fn, context});
}

void domAgentRemoveHook(Array<DomAgentTask>* hooks, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    for (uint32_t i = 0; i < hooks->count; ++i) {
        if (hooks->data[i].fn == fn && hooks->data[i].context == context) {
            hooks->erase(i);
            return;
        }
    }
}

bool domAgentRun(PrDomAgent* agent, double milliseconds) {
    PR_ASSERT(!agent->runningTask);
    double deadline = prTimeMonotonicMilliseconds() + milliseconds;
    bool ran = false;
    while (domAgentNextTaskDelay(agent) == 0 && runNextTask(agent)) {
        ran = true;
        // A checkpoint may remove itself or another, so each is called by its position as the array stands.
        for (uint32_t i = 0; i < agent->checkpoints.count; ++i)
            agent->checkpoints.data[i].fn(agent->checkpoints.data[i].context);
        if (prTimeMonotonicMilliseconds() >= deadline)
            break;
    }
    for (uint32_t i = 0; i < agent->updates.count; ++i)
        agent->updates.data[i].fn(agent->updates.data[i].context);
    return ran;
}

bool domAgentWait(PrDomAgent* agent, double milliseconds) {
    // A task that can run ends the wait at once, and a timer's delay bounds it.
    double delay = domAgentNextTaskDelay(agent);
    double limit = delay >= 0 && (milliseconds < 0 || delay < milliseconds) ? delay : milliseconds;
    prMutexLock(&agent->postMutex);
    if (limit != 0 && !agent->signaled && !agent->stopped) {
        if (limit < 0)
            prConditionWait(&agent->woken, &agent->postMutex);
        else
            prConditionWaitFor(&agent->woken, &agent->postMutex, limit);
    }
    agent->signaled = false;
    bool running = !agent->stopped;
    prMutexUnlock(&agent->postMutex);
    return running;
}

void domAgentWake(PrDomAgent* agent) {
    prMutexLock(&agent->postMutex);
    signal(agent);
    prMutexUnlock(&agent->postMutex);
}

void domAgentStop(PrDomAgent* agent) {
    prMutexLock(&agent->postMutex);
    agent->stopped = true;
    signal(agent);
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
        prLayoutWorkerSetDone(agent->worker, layoutDone, agent);
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
    agent->checkpoints.reset();
    agent->updates.reset();
    prConditionReset(&agent->woken);
    prMutexReset(&agent->postMutex);
    prLayoutWorkerDestroy(agent->worker);
    prMemoryFree(agent);
}

} // namespace pr_impl
