// The Windows backend of address space.

#include "pr/core_windows_vmem.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void* PR_NULLABLE prCoreWindowsVmemReserve(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool prCoreWindowsVmemCommit(void* base, size_t size) {
    return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

// A release frees the whole reservation, and takes no size.
void prCoreWindowsVmemFree(void* base, size_t size) {
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
}
