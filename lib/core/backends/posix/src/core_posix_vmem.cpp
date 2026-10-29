// The POSIX backend of address space.

#include "pr/core_posix_vmem.h"

#include <sys/mman.h>

void* PR_NULLABLE prCorePosixVmemReserve(size_t size) {
    void* base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return base != MAP_FAILED ? base : nullptr;
}

bool prCorePosixVmemCommit(void* base, size_t size) {
    return mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void prCorePosixVmemFree(void* base, size_t size) {
    munmap(base, size);
}
