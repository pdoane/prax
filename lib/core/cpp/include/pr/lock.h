// Locks: a mutex held for a scope.

#pragma once

#include "pr/base.h"
#include "pr/mutex.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Holds `mutex` from its construction to the end of its scope.
struct Lock {
    explicit Lock(PrMutex* mutex) : held(mutex) { prMutexLock(held); }
    ~Lock() { prMutexUnlock(held); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    PrMutex* held;
};

} // namespace pr

PR_ASSUME_NONNULL_END
