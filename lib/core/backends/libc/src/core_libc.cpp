// The C library backend of memory.

#include "pr/core_libc.h"

#include <stdlib.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

void* PR_NULLABLE prCoreLibcAllocBytes(size_t size) {
    return malloc(size);
}

void* PR_NULLABLE prCoreLibcAllocZeroedBytes(size_t size) {
    return calloc(1, size);
}

void* PR_NULLABLE prCoreLibcReallocBytes(void* memory, size_t size) {
    return realloc(memory, size);
}

void prCoreLibcFreeBytes(void* memory) {
    free(memory);
}

size_t prCoreLibcUsableBytes(const void* memory) {
#if defined(__APPLE__)
    return malloc_size(memory);
#elif defined(_WIN32)
    return _msize(const_cast<void*>(memory));
#else
    return malloc_usable_size(const_cast<void*>(memory));
#endif
}

void prCoreLibcTrimBytes(void) {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}
