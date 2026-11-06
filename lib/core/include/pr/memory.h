// Memory that the library allocates through the platform. Running out of memory ends the process, so no call here
// returns null.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Allocates `size` bytes of uninitialized memory, aligned for any type.
void* prMemoryAlloc(size_t size);

// Allocates `size` bytes of zeroed memory, aligned for any type.
void* prMemoryAllocZeroed(size_t size);

// Resizes `memory` to `size` bytes and returns the result, which may be `memory` itself. Otherwise `memory` is freed.
// The contents are kept up to the smaller of the two sizes. If `memory` is null, the call allocates.
void* prMemoryRealloc(void* PR_NULLABLE memory, size_t size);

// Frees `memory`. Null is a no-op.
void prMemoryFree(void* PR_NULLABLE memory);

// Returns the number of bytes that `memory` can hold, which is at least the number it was allocated with, or zero if
// the platform cannot tell.
size_t prMemoryUsableSize(const void* memory);

// Gives the free memory that the allocator holds back to the system. Otherwise the allocator may keep that memory for a
// while in case it is allocated again. A program calls this after it frees a large amount at once, so that its
// footprint falls immediately. The cost grows with the amount of free memory held. On a platform that has no such call,
// it does nothing.
void prMemoryTrim(void);

// Returns the number of allocations made and not yet freed, or zero in a build that does not count them.
size_t prMemoryLiveCount(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
