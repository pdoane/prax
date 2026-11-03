// Aborting ongoing activities: AbortController and AbortSignal.
//
// dom - https://dom.spec.whatwg.org/#aborting-ongoing-activities

#pragma once

#include "pr/base.h"
#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The handles of the objects that this header declares. Each section below describes its own handles.
typedef struct PrDomAbortController PrDomAbortController;

// ---------------------------------------------------------------------------------------------------------------------
// AbortSignal : EventTarget (dom)
//
// A signal says whether an operation was aborted, and why (dom/#interface-AbortSignal). The reason is a value of the
// caller's language, which the caller who aborts passes through the signal to each signal that the abort reaches. A
// signal does not keep it: its reason handler is given it as the signal is aborted, and keeps it where the language's
// collector can see it, as the caller keeps any value of its language that a DOM object carries. A dependent signal
// (dom/#abortsignal-dependent) follows the signals that it was made from. Neither side holds a reference to the other.

// An AbortSignal as the interface it inherits from.
PR_HANDLE_UPCAST(DomAbortSignal, DomEventTarget, EventTarget)

// Returns an EventTarget as an AbortSignal, or null if it is not one.
PrDomAbortSignal* PR_NULLABLE prDomAbortSignalQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates a signal that is not aborted. The caller holds the one reference.
PrDomAbortSignal* prDomAbortSignalCreate(void);

// Creates a signal that is already aborted (dom/#dom-abortsignal-abort), whose reason the caller keeps. The caller
// holds the one reference.
PrDomAbortSignal* prDomAbortSignalCreateAborted(void);

// Creates a signal that depends on `signals` (dom/#create-a-dependent-abort-signal). If one of them is aborted, the new
// signal is aborted, and its reason is that of the first such signal, which the caller keeps for it. Otherwise it
// aborts when any of them aborts. The caller holds the one reference.
PrDomAbortSignal* prDomAbortSignalCreateDependent(PrDomAbortSignal* PR_NONNULL const* PR_NULLABLE signals, uint32_t count);

// Adds or releases a reference. The last release frees the signal.
void prDomAbortSignalRetain(PrDomAbortSignal* signal);
void prDomAbortSignalRelease(PrDomAbortSignal* PR_NULLABLE signal);

// Returns true if the signal is aborted.
bool prDomAbortSignalAborted(const PrDomAbortSignal* signal);

// Called as the signal is aborted with `reason`, the caller's value, which is valid only during the call. The signals
// that an abort reaches are each given the reason before any of them runs its abort algorithms.
typedef void (*PrDomAbortReasonFn)(void* PR_NULLABLE context, PrDomAbortSignal* signal, const void* PR_NULLABLE reason);

// Sets the function that is given the signal's reason, and replaces any before it. A null `invoke` removes it.
void prDomAbortSignalSetReasonHandler(PrDomAbortSignal* signal, PrDomAbortReasonFn PR_NULLABLE invoke, void* PR_NULLABLE context);

// Aborts the signal with `reason`, then the signals that depend on it (dom/#abortsignal-signal-abort). Each signal runs
// its abort algorithms and fires abort, and every signal is aborted, and its reason handler given `reason`, before the
// first algorithm runs. Does nothing if the signal is already aborted.
void prDomAbortSignalSignalAbort(PrDomAbortSignal* signal, const void* PR_NULLABLE reason);

// Called when the signal aborts.
typedef void (*PrDomAbortAlgorithmFn)(void* PR_NULLABLE context, PrDomAbortSignal* signal);

// An abort algorithm (dom/#abortsignal-abort-algorithms). The signal keeps a copy, and calls `retain` and `release` on
// the copy's context if they are given.
typedef struct PrDomAbortAlgorithm {
    PrDomAbortAlgorithmFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrDomAbortAlgorithm;

// Adds `algorithm` to the steps that run when the signal aborts (dom/#abortsignal-add), and returns the id that removes
// it (dom/#abortsignal-remove). An aborted signal adds nothing and returns zero. Removing an id that the signal does
// not have does nothing. An algorithm runs once, and is released after it runs.
uint32_t prDomAbortSignalAdd(PrDomAbortSignal* signal, const PrDomAbortAlgorithm* algorithm);
void prDomAbortSignalRemove(PrDomAbortSignal* signal, uint32_t id);

// ---------------------------------------------------------------------------------------------------------------------
// AbortController (dom)
//
// The owner of a signal, which aborts it (dom/#interface-abortcontroller).

// Creates a controller with a new signal.
PrDomAbortController* prDomAbortControllerCreate(void);

// Frees the controller. Its signal lives on while it has references.
void prDomAbortControllerDestroy(PrDomAbortController* PR_NULLABLE controller);

// Returns the controller's signal.
PrDomAbortSignal* prDomAbortControllerSignal(PrDomAbortController* controller);

// Aborts the controller's signal with `reason` (dom/#dom-abortcontroller-abort), as prDomAbortSignalSignalAbort does.
void prDomAbortControllerAbort(PrDomAbortController* controller, const void* PR_NULLABLE reason);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
