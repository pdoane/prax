// A store that keeps its records in memory, for as long as it lives.

#pragma once

#include "pr/base.h"
#include "pr/storage.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a store in memory whose records hold at most `limitBytes`, and that stays under it as `eviction` says.
PrStorage* prStorageMemoryCreate(uint64_t limitBytes, PrStorageEviction eviction);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
