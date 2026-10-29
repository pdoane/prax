// The mimalloc backend of memory. Memory comes from mimalloc. Under AddressSanitizer, memory comes from the system
// allocator instead, because AddressSanitizer instruments that allocator. The backend serves every target, and it is
// the default binding of memory in a build that chooses it (PR_PLATFORM_MIMALLOC).

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Allocates `size` bytes, aligned for any type, or returns null if the host refuses. `size` is not zero.
void* PR_NULLABLE prCoreMimallocAllocBytes(size_t size);

// Allocates `size` zeroed bytes, aligned for any type, or returns null if the host refuses. `size` is not zero.
void* PR_NULLABLE prCoreMimallocAllocZeroedBytes(size_t size);

// Resizes `memory`, which this backend allocated, to `size` bytes, and returns the result, which may be `memory`
// itself. The contents are kept up to the smaller of the two sizes. Returns null and leaves `memory` as it was if the
// host refuses. `size` is not zero.
void* PR_NULLABLE prCoreMimallocReallocBytes(void* memory, size_t size);

// Frees `memory`, which this backend allocated.
void prCoreMimallocFreeBytes(void* memory);

// Returns the number of bytes that `memory` can hold, which is at least the number it was allocated with.
size_t prCoreMimallocUsableBytes(const void* memory);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
