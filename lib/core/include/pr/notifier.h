// Notifiers: a count of the changes to something, and the observers that are told of each.
//
// Whatever changes owns a notifier, and notifies it after each change. Whatever wants to know observes it. An observer
// that is told of a change asks the owner what changed, so a notification carries nothing but the fact. The count lets
// a holder that does not observe tell whether anything changed since it last looked: it records the count when it
// looks, and compares.
//
// Threads. Any thread may notify, observe and unobserve. A notification calls the observers on the notifying thread.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A count of changes and the observers of them, shared by reference.
typedef struct PrNotifier PrNotifier;

// Called with the observer's context after a change, on the notifying thread. It must not notify the same notifier, and
// must not take a lock that a caller of prNotifierUnobserve may hold while it unobserves.
typedef void (*PrNotifierFn)(void* PR_NULLABLE context);

// Creates a notifier with a count of zero and no observers. The caller holds the one reference.
PrNotifier* prNotifierCreate(void);

// Add and release a reference. The last release frees the notifier, which has no observers left.
void prNotifierRetain(PrNotifier* notifier);
void prNotifierRelease(PrNotifier* PR_NULLABLE notifier);

// Returns the number of notifications so far. A notification counts before it calls the observers.
uint64_t prNotifierCount(const PrNotifier* notifier);

// Counts a change, and calls each observer.
void prNotifierNotify(PrNotifier* notifier);

// Adds an observer that `fn` is called for with `context`, and returns a nonzero number that names it.
uint64_t prNotifierObserve(PrNotifier* notifier, PrNotifierFn fn, void* PR_NULLABLE context);

// Removes the observer that `observer` names. Once the call returns, its function is not running and is not called
// again, so its context may be freed. Called from an observer's function, the call does not wait for the notification
// that called it.
void prNotifierUnobserve(PrNotifier* notifier, uint64_t observer);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
