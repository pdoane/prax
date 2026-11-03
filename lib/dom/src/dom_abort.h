// Aborting ongoing activities: AbortController and AbortSignal.
//
// dom - https://dom.spec.whatwg.org/#aborting-ongoing-activities

#pragma once

#include "pr/dom_abort.h"
#include "pr/raw_array.h"

#include "dom/src/dom_event_target.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An abort algorithm, and the id that removes it.
struct DomAbortEntry {
    PrDomAbortAlgorithm algorithm;
    uint32_t id;
};

// A signal (dom/#interface-AbortSignal).
struct DomAbortSignal : PrDomEventTarget {
    uint32_t references;
    uint32_t lastId;                        // the id of the last algorithm added
    RawArray<DomAbortEntry> algorithms;     // in the order they were added

    // The signals that a dependent signal follows, and the dependent signals that follow this one, each in the order
    // they were linked (dom/#abortsignal-source-signals, dom/#abortsignal-dependent-signals). Neither list holds
    // references. A signal leaves the lists of the others when it is freed.
    RawArray<PrDomAbortSignal*> sources;
    RawArray<PrDomAbortSignal*> dependents;

    PrDomAbortReasonFn PR_NULLABLE reasonHandler;
    void* PR_NULLABLE reasonContext;
    bool aborted;
    bool dependent;                         // made by prDomAbortSignalCreateDependent (dom/#abortsignal-dependent)
};

// Adds or releases a reference. The last release frees the signal.
void domAbortSignalRetain(PrDomAbortSignal* signal);
void domAbortSignalRelease(PrDomAbortSignal* PR_NULLABLE signal);

} // namespace pr_impl

// C opaque types
struct PrDomAbortSignal : pr_impl::DomAbortSignal {};

PR_ASSUME_NONNULL_END
