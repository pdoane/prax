// Aborting ongoing activities: AbortController and AbortSignal.
//
// dom - https://dom.spec.whatwg.org/#aborting-ongoing-activities

#include "dom/src/dom_abort.h"

#include "pr/alloc.h"
#include "pr/assert.h"

#include "dom/src/dom_event.h"

namespace pr_impl {
namespace {

// A controller and the signal that it holds a reference to (dom/#interface-abortcontroller).
struct DomAbortController {
    PrDomAbortSignal* signal;
};

PrDomAbortSignal* createSignal() {
    PrDomAbortSignal* signal = allocZeroed<PrDomAbortSignal>();
    signal->type = DomTargetType_AbortSignal;
    signal->references = 1;
    return signal;
}

void release(const PrDomAbortAlgorithm& algorithm) {
    if (algorithm.release != nullptr)
        algorithm.release(algorithm.context);
}

// Marks the signal aborted, and gives its reason handler `reason`.
void setReason(PrDomAbortSignal* signal, const void* PR_NULLABLE reason) {
    PR_ASSERT(!signal->aborted);
    signal->aborted = true;
    if (signal->reasonHandler != nullptr)
        signal->reasonHandler(signal->reasonContext, signal, reason);
}

// Makes `dependent` follow `source`, once.
void link(PrDomAbortSignal* dependent, PrDomAbortSignal* source) {
    if (dependent->sources.find(source) != kArrayMissing)
        return;
    dependent->sources.push(source);
    source->dependents.push(dependent);
}

// Runs the signal's abort algorithms and fires abort at it (dom/#run-the-abort-steps). Each algorithm leaves the list
// before it runs, so that an algorithm that another one removes does not run. An aborted signal adds none.
void runAbortSteps(PrDomAbortSignal* signal) {
    while (signal->algorithms.count != 0) {
        PrDomAbortAlgorithm algorithm = signal->algorithms.data[0].algorithm;
        signal->algorithms.erase(0);
        algorithm.invoke(algorithm.context, signal);
        release(algorithm);
    }
    signal->algorithms.reset();
    domEventFire(signal, "abort");
}

} // namespace

void domAbortSignalRetain(PrDomAbortSignal* signal) {
    ++signal->references;
}

void domAbortSignalRelease(PrDomAbortSignal* PR_NULLABLE signal) {
    if (signal == nullptr || --signal->references != 0)
        return;
    for (uint32_t i = 0; i < signal->sources.count; ++i)
        signal->sources.data[i]->dependents.removeFirst(signal);
    for (uint32_t i = 0; i < signal->dependents.count; ++i)
        signal->dependents.data[i]->sources.removeFirst(signal);
    signal->sources.reset();
    signal->dependents.reset();
    for (uint32_t i = 0; i < signal->algorithms.count; ++i)
        release(signal->algorithms.data[i].algorithm);
    signal->algorithms.reset();
    domListenersDestroy(signal->listeners);
    prMemoryFree(signal);
}

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDomAbortController : DomAbortController {};

PrDomAbortSignal* PR_NULLABLE prDomAbortSignalQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_AbortSignal ? static_cast<PrDomAbortSignal*>(object) : nullptr;
}

PrDomAbortSignal* prDomAbortSignalCreate(void) {
    return createSignal();
}

PrDomAbortSignal* prDomAbortSignalCreateAborted(void) {
    PrDomAbortSignal* signal = createSignal();
    signal->aborted = true;
    return signal;
}

PrDomAbortSignal* prDomAbortSignalCreateDependent(PrDomAbortSignal* PR_NONNULL const* PR_NULLABLE signals, uint32_t count) {
    PrDomAbortSignal* result = createSignal();
    for (uint32_t i = 0; i < count; ++i) {
        if (signals[i]->aborted) {
            result->aborted = true;
            return result;
        }
    }
    result->dependent = true;
    for (uint32_t i = 0; i < count; ++i) {
        PrDomAbortSignal* signal = signals[i];
        if (!signal->dependent) {
            link(result, signal);
            continue;
        }
        // A dependent signal's sources are never dependent, so a chain of dependents follows the signals at its root.
        for (uint32_t j = 0; j < signal->sources.count; ++j) {
            PR_ASSERT(!signal->sources.data[j]->aborted && !signal->sources.data[j]->dependent);
            link(result, signal->sources.data[j]);
        }
    }
    return result;
}

void prDomAbortSignalRetain(PrDomAbortSignal* self) {
    domAbortSignalRetain(self);
}

void prDomAbortSignalRelease(PrDomAbortSignal* PR_NULLABLE self) {
    domAbortSignalRelease(self);
}

bool prDomAbortSignalAborted(const PrDomAbortSignal* self) {
    return self->aborted;
}

void prDomAbortSignalSetReasonHandler(PrDomAbortSignal* self, PrDomAbortReasonFn PR_NULLABLE invoke, void* PR_NULLABLE context) {
    self->reasonHandler = invoke;
    self->reasonContext = context;
}

void prDomAbortSignalSignalAbort(PrDomAbortSignal* self, const void* PR_NULLABLE reason) {
    if (self->aborted)
        return;
    setReason(self, reason);
    // Every dependent is aborted before any listener runs, and each is held while the listeners run.
    Array<PrDomAbortSignal*> dependents;
    for (uint32_t i = 0; i < self->dependents.count; ++i) {
        PrDomAbortSignal* dependent = self->dependents.data[i];
        if (dependent->aborted)
            continue;
        setReason(dependent, reason);
        domAbortSignalRetain(dependent);
        dependents.push(dependent);
    }
    domAbortSignalRetain(self);
    runAbortSteps(self);
    for (uint32_t i = 0; i < dependents.count; ++i) {
        runAbortSteps(dependents.data[i]);
        domAbortSignalRelease(dependents.data[i]);
    }
    domAbortSignalRelease(self);
    dependents.reset();
}

uint32_t prDomAbortSignalAdd(PrDomAbortSignal* self, const PrDomAbortAlgorithm* algorithm) {
    if (self->aborted)
        return 0;
    if (algorithm->retain != nullptr)
        algorithm->retain(algorithm->context);
    uint32_t id = ++self->lastId;
    self->algorithms.push({*algorithm, id});
    return id;
}

void prDomAbortSignalRemove(PrDomAbortSignal* self, uint32_t id) {
    for (uint32_t i = 0; i < self->algorithms.count; ++i) {
        if (self->algorithms.data[i].id == id) {
            PrDomAbortAlgorithm algorithm = self->algorithms.data[i].algorithm;
            self->algorithms.erase(i);
            release(algorithm);
            return;
        }
    }
}

PrDomAbortController* prDomAbortControllerCreate(void) {
    PrDomAbortController* controller = allocZeroed<PrDomAbortController>();
    controller->signal = createSignal();
    return controller;
}

void prDomAbortControllerDestroy(PrDomAbortController* PR_NULLABLE self) {
    if (self == nullptr)
        return;
    domAbortSignalRelease(self->signal);
    prMemoryFree(self);
}

PrDomAbortSignal* prDomAbortControllerSignal(PrDomAbortController* self) {
    return self->signal;
}

void prDomAbortControllerAbort(PrDomAbortController* self, const void* PR_NULLABLE reason) {
    prDomAbortSignalSignalAbort(self->signal, reason);
}
