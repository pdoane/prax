// Address space through the platform.

#include "pr/vmem.h"

#include "pr/assert.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_VMEM_RESERVE)
#error "a build without the default platform binds PR_PLATFORM_VMEM_RESERVE"
#endif

void* prVmemReserve(size_t size) {
    void* base = PR_PLATFORM_VMEM_RESERVE(size);
    if (base == nullptr)
        PR_FATAL("out of address space");
    return base;
}

void prVmemCommit(void* base, size_t size) {
    if (size != 0 && !PR_PLATFORM_VMEM_COMMIT(base, size))
        PR_FATAL("out of memory");
}

void prVmemFree(void* PR_NULLABLE base, size_t size) {
    if (base != nullptr && size != 0)
        PR_PLATFORM_VMEM_FREE(base, size);
}
