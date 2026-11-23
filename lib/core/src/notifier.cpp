// Notifiers.

#include "pr/notifier.h"

#include "pr/atomic.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

namespace pr_impl {

// One observer: the number that names it, and what it calls.
struct NotifierObserver {
    uint64_t id;
    PrNotifierFn fn;
    void* PR_NULLABLE context;
};

// `notifying` counts the notifications that are calling observers. An unobserve waits until it is zero, unless the
// unobserving thread is one of them.
struct Notifier {
    Atomic<uint32_t> references;
    Atomic<uint64_t> count;
    PrMutex lock;
    PrCondition idle;
    RawArray<NotifierObserver> observers;
    uint64_t nextId;
    uint32_t notifying;
};

namespace {

// The notifier whose observers the thread is calling, if any.
thread_local const Notifier* PR_NULLABLE tNotifying = nullptr;

bool isObserved(const Notifier& notifier, uint64_t id) {
    for (const NotifierObserver& observer : notifier.observers.span()) {
        if (observer.id == id)
            return true;
    }
    return false;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrNotifier : Notifier {};

PrNotifier* prNotifierCreate(void) {
    PrNotifier* notifier = static_cast<PrNotifier*>(prMemoryAllocZeroed(sizeof(PrNotifier)));
    notifier->references.storeRelaxed(1);
    notifier->count.storeRelaxed(0);
    prMutexInit(&notifier->lock);
    prConditionInit(&notifier->idle);
    return notifier;
}

void prNotifierRetain(PrNotifier* notifier) {
    notifier->references.fetchAdd(1);
}

void prNotifierRelease(PrNotifier* PR_NULLABLE notifier) {
    if (notifier == nullptr || notifier->references.fetchSub(1) != 1)
        return;
    notifier->observers.reset();
    prMutexReset(&notifier->lock);
    prConditionReset(&notifier->idle);
    prMemoryFree(notifier);
}

uint64_t prNotifierCount(const PrNotifier* notifier) {
    return notifier->count.load();
}

void prNotifierNotify(PrNotifier* notifier) {
    notifier->count.fetchAdd(1);
    prMutexLock(&notifier->lock);
    if (notifier->observers.count == 0) {
        prMutexUnlock(&notifier->lock);
        return;
    }
    ++notifier->notifying;
    RawArray<NotifierObserver> called;
    for (const NotifierObserver& observer : notifier->observers.span())
        called.push(observer);
    prMutexUnlock(&notifier->lock);
    const Notifier* outer = tNotifying;
    tNotifying = notifier;
    for (const NotifierObserver& observer : called.span()) {
        // An observer that an earlier one removed is not called.
        prMutexLock(&notifier->lock);
        bool observed = isObserved(*notifier, observer.id);
        prMutexUnlock(&notifier->lock);
        if (observed)
            observer.fn(observer.context);
    }
    tNotifying = outer;
    called.reset();
    prMutexLock(&notifier->lock);
    --notifier->notifying;
    prConditionBroadcast(&notifier->idle);
    prMutexUnlock(&notifier->lock);
}

uint64_t prNotifierObserve(PrNotifier* notifier, PrNotifierFn fn, void* PR_NULLABLE context) {
    prMutexLock(&notifier->lock);
    uint64_t id = ++notifier->nextId;
    notifier->observers.push(NotifierObserver{id, fn, context});
    prMutexUnlock(&notifier->lock);
    return id;
}

void prNotifierUnobserve(PrNotifier* notifier, uint64_t observer) {
    prMutexLock(&notifier->lock);
    for (uint32_t i = 0; i < notifier->observers.count; ++i) {
        if (notifier->observers.data[i].id == observer) {
            notifier->observers.erase(i);
            break;
        }
    }
    while (notifier->notifying != 0 && tNotifying != notifier)
        prConditionWait(&notifier->idle, &notifier->lock);
    prMutexUnlock(&notifier->lock);
}
