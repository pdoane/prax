// The null network (net_null.h).

#include "pr/net_null.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mutex.h"
#include "pr/net_backend.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

struct Task {
    PrNetTaskFn fn;
    void* PR_NULLABLE context;
};

// `mutex` guards `tasks`, which other threads post to. The loop alone reaches the rest.
struct NullNet : PrNet {
    RawArray<Task> running;
    bool inRun;                 // true while prNetNullRun is on the stack
    PrMutex mutex;
    RawArray<Task> tasks;
};

// Runs the tasks posted so far. Returns true if one ran.
bool runPosted(NullNet* network) {
    prMutexLock(&network->mutex);
    RawArray<Task> swapped = network->tasks;
    network->tasks = network->running;
    network->running = swapped;
    prMutexUnlock(&network->mutex);
    for (uint32_t i = 0; i < network->running.count; ++i)
        network->running.data[i].fn(network->running.data[i].context);
    bool ran = network->running.count != 0;
    network->running.clear();
    return ran;
}

bool run(NullNet* network) {
    PR_ASSERT(!network->inRun);
    network->inRun = true;
    bool ran = false;
    while (runPosted(network))
        ran = true;
    network->inRun = false;
    return ran;
}

void destroyNetwork(PrNet* base) {
    NullNet* network = static_cast<NullNet*>(base);
    run(network);
    network->running.reset();
    network->tasks.reset();
    prMutexReset(&network->mutex);
    prMemoryFree(network);
}

void post(PrNet* base, PrNetTaskFn fn, void* PR_NULLABLE context) {
    NullNet* network = static_cast<NullNet*>(base);
    prMutexLock(&network->mutex);
    network->tasks.push(Task{fn, context});
    prMutexUnlock(&network->mutex);
}

// The loop runs on the caller's thread, so a blocking post runs it.
void postBlocking(PrNet* base, PrNetTaskFn fn, void* PR_NULLABLE context) {
    post(base, fn, context);
    run(static_cast<NullNet*>(base));
}

PrStatus connectNetwork(PrNet*, const PrNetConnectDesc*, PrNetStream* PR_NULLABLE* PR_NONNULL) {
    return PrStatus_NotSupported;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrNet* prNetNullCreate(void) {
    NullNet* network = allocZeroed<NullNet>();
    network->destroyFn = destroyNetwork;
    network->postFn = post;
    network->postBlockingFn = postBlocking;
    network->connectFn = connectNetwork;
    prMutexInit(&network->mutex);
    return network;
}

bool prNetNullRun(PrNet* net) {
    PR_ASSERT(net->destroyFn == destroyNetwork);
    return run(static_cast<NullNet*>(net));
}
