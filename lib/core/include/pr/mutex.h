// Mutual exclusion between threads, through the platform.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A non-recursive lock held by one thread at a time. Its storage belongs to the platform. It is initialized before it
// is shared, and reset after it is no longer shared. It cannot be copied or moved.
typedef struct PrMutex {
    uint64_t storage[8];
} PrMutex;

// Initializes `mutex`.
void prMutexInit(PrMutex* mutex);

// Frees what `mutex` holds. No thread may hold it.
void prMutexReset(PrMutex* mutex);

// Locks `mutex`, waiting while another thread holds it.
void prMutexLock(PrMutex* mutex);

// Unlocks `mutex`, which the calling thread holds.
void prMutexUnlock(PrMutex* mutex);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
