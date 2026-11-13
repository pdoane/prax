// Memory through the platform, and the count of live allocations.

#include "pr/memory.h"

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/mem.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_ALLOC_BYTES)
#error "a target without a default for the group binds PR_PLATFORM_ALLOC_BYTES"
#endif

namespace pr_impl {

namespace {

#if defined(PR_ALLOC_COUNT)
Atomic<size_t> live;
#endif

void counted(void* PR_NULLABLE memory, bool made) {
#if defined(PR_ALLOC_COUNT)
    if (memory == nullptr)
        return;
    live.fetchAddRelaxed(made ? size_t(1) : size_t(-1));
#else
    (void)memory;
    (void)made;
#endif
}

// Returns the size to ask the platform for, which never asks for zero bytes.
size_t requested(size_t size) {
    return size != 0 ? size : 1;
}

// Returns `memory`, the result of an allocation. A null result is fatal.
void* checked(void* PR_NULLABLE memory) {
    if (memory == nullptr)
        PR_FATAL("out of memory");
    counted(memory, true);
    return memory;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void* prMemoryAlloc(size_t size) {
    return checked(PR_PLATFORM_ALLOC_BYTES(requested(size)));
}

void* prMemoryAllocZeroed(size_t size) {
#if defined(PR_PLATFORM_ALLOC_ZEROED_BYTES)
    return checked(PR_PLATFORM_ALLOC_ZEROED_BYTES(requested(size)));
#else
    void* memory = checked(PR_PLATFORM_ALLOC_BYTES(requested(size)));
    prMemZero(memory, size);
    return memory;
#endif
}

void* prMemoryRealloc(void* PR_NULLABLE memory, size_t size) {
    if (memory == nullptr)
        return prMemoryAlloc(size);
    void* moved = PR_PLATFORM_REALLOC_BYTES(memory, requested(size));
    if (moved == nullptr)
        PR_FATAL("out of memory");
    return moved;
}

void prMemoryFree(void* PR_NULLABLE memory) {
    if (memory == nullptr)
        return;
    counted(memory, false);
    PR_PLATFORM_FREE_BYTES(memory);
}

size_t prMemoryUsableSize(const void* memory) {
#if defined(PR_PLATFORM_USABLE_BYTES)
    return PR_PLATFORM_USABLE_BYTES(memory);
#else
    (void)memory;
    return 0;
#endif
}

void prMemoryTrim(void) {
#if defined(PR_PLATFORM_TRIM_BYTES)
    PR_PLATFORM_TRIM_BYTES();
#endif
}

size_t prMemoryLiveCount(void) {
#if defined(PR_ALLOC_COUNT)
    return live.loadRelaxed();
#else
    return 0;
#endif
}
