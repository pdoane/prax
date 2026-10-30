// Networks: connections to hosts, and the loop that runs their callbacks.

#include "pr/net.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/mutex.h"
#include "pr/thread.h"

#if defined(PR_NET_HAS_SOCKETS)
#include "pr/net_sockets.h"
#endif
#if defined(PR_NET_HAS_NULL)
#include "pr/net_null.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrNetBackend_Count] = {"default", "sockets", "null"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrNetBackend kBuilt[] = {
#if defined(PR_NET_HAS_SOCKETS)
    PrNetBackend_Sockets,
#endif
#if defined(PR_NET_HAS_NULL)
    PrNetBackend_Null,
#endif
    PrNetBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrNetBackend prNetBackendFromName(PrStringView name) {
    for (PrNetBackend backend = 0; backend < PrNetBackend_Count; ++backend) {
        if (asciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrNetBackend_Count;
}

PrStringView prNetBackendName(PrNetBackend backend) {
    return backend < PrNetBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prNetBackendIsBuilt(PrNetBackend backend) {
    for (PrNetBackend built : kBuilt) {
        if (built != PrNetBackend_Default && (built == backend || backend == PrNetBackend_Default))
            return true;
    }
    return false;
}

PrNet* PR_NULLABLE prNetCreate(PrNetBackend backend) {
    switch (backend == PrNetBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_NET_HAS_SOCKETS)
        case PrNetBackend_Sockets: return prNetSocketsCreate();
#endif
#if defined(PR_NET_HAS_NULL)
        case PrNetBackend_Null: return prNetNullCreate();
#endif
        default: return nullptr;
    }
}

void prNetDestroy(PrNet* PR_NULLABLE net) {
    if (net != nullptr)
        net->destroy(net);
}

void prNetPost(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context) {
    net->post(net, fn, context);
}

void prNetPostBlocking(PrNet* net, PrNetTaskFn fn, void* PR_NULLABLE context) {
    if (net->postBlocking != nullptr) {
        net->postBlocking(net, fn, context);
        return;
    }
    // The task runs on the loop's thread, and the caller waits until it has.
    struct Blocking {
        PrNetTaskFn fn;
        void* PR_NULLABLE context;
        PrMutex mutex;
        PrCondition done;
        bool finished;
    };
    Blocking blocking{.fn = fn, .context = context};
    prMutexInit(&blocking.mutex);
    prConditionInit(&blocking.done);
    prNetPost(net, [](void* PR_NULLABLE data) {
        Blocking* task = static_cast<Blocking*>(data);
        task->fn(task->context);
        prMutexLock(&task->mutex);
        task->finished = true;
        prConditionSignal(&task->done);
        prMutexUnlock(&task->mutex);
    }, &blocking);
    prMutexLock(&blocking.mutex);
    while (!blocking.finished)
        prConditionWait(&blocking.done, &blocking.mutex);
    prMutexUnlock(&blocking.mutex);
    prConditionReset(&blocking.done);
    prMutexReset(&blocking.mutex);
}

PrStatus prNetConnect(PrNet* net, const PrNetConnectDesc* desc, PrNetStream* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrStatus status = net->connect(net, desc, out);
    PR_ASSERT(status != PrStatus_Ok || ((*out) != nullptr && (*out)->net == net));
    return status;
}

void prNetStreamDestroy(PrNetStream* PR_NULLABLE stream) {
    if (stream != nullptr)
        stream->destroy(stream);
}

void prNetStreamSetEvent(PrNetStream* stream, PrNetStreamFn PR_NULLABLE event, void* PR_NULLABLE context) {
    stream->event = event;
    stream->context = context;
}

size_t prNetStreamRead(PrNetStream* stream, uint8_t* PR_NULLABLE output, size_t capacity) {
    PR_ASSERT(output != nullptr || capacity == 0);
    if (capacity == 0)
        return 0;
    return stream->read(stream, output, capacity);
}

void prNetStreamWrite(PrNetStream* stream, PrByteView bytes) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    if (bytes.size != 0)
        stream->write(stream, bytes);
}

PrStatus prNetStreamFinish(PrNetStream* stream) {
    if (stream->finish == nullptr)
        return PrStatus_NotSupported;
    return stream->finish(stream);
}

size_t prNetStreamQueued(const PrNetStream* stream) {
    return stream->queued(stream);
}

PrNetError prNetStreamError(const PrNetStream* stream) {
    return stream->error(stream);
}
