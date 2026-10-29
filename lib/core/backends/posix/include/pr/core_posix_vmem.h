// The POSIX backend of address space, which it reserves with mmap and commits with mprotect. It serves the Unix-like
// targets, and it is the default binding of address space on them.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Reserves `size` bytes of address space, or returns null if the host refuses. `size` is not zero.
void* PR_NULLABLE prCorePosixVmemReserve(size_t size);

// Commits the first `size` bytes of the reservation at `base`, and returns false if the host refuses.
bool prCorePosixVmemCommit(void* base, size_t size);

// Releases the reservation at `base` of `size` bytes.
void prCorePosixVmemFree(void* base, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
