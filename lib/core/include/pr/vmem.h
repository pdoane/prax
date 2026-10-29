// Virtual memory, through the platform: address space that is reserved first and committed as it is used. A reservation
// is a contiguous range of virtual address space, and committing makes its memory available for use.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Reserves `size` bytes of address space, rounded up to a page, and returns its base. Nothing may be read or written
// until it is committed. `size` is not zero. Running out of address space ends the process.
void* prVmemReserve(size_t size);

// Makes the first `size` bytes of the reservation at `base` readable and writable, until the reservation is freed. The
// memory reads as zero. Running out of memory ends the process.
void prVmemCommit(void* base, size_t size);

// Releases the reservation at `base`. `size` is the size that was reserved. Null is a no-op.
void prVmemFree(void* PR_NULLABLE base, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
