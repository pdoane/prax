// The mimalloc backend of memory.

#include "pr/core_mimalloc.h"

#if PR_ADDRESS_SANITIZER && defined(__APPLE__)
#include <malloc/malloc.h>
#include <stdlib.h>
#elif PR_ADDRESS_SANITIZER
#include <malloc.h>
#include <stdlib.h>
#else
#include "mimalloc.h"
#endif

// Memory comes from the system allocator under AddressSanitizer, which instruments it, and from mimalloc otherwise.

void* PR_NULLABLE prCoreMimallocAllocBytes(size_t size) {
#if PR_ADDRESS_SANITIZER
    return malloc(size);
#else
    return mi_malloc(size);
#endif
}

void* PR_NULLABLE prCoreMimallocAllocZeroedBytes(size_t size) {
#if PR_ADDRESS_SANITIZER
    return calloc(1, size);
#else
    return mi_zalloc(size);
#endif
}

void* PR_NULLABLE prCoreMimallocReallocBytes(void* memory, size_t size) {
#if PR_ADDRESS_SANITIZER
    return realloc(memory, size);
#else
    return mi_realloc(memory, size);
#endif
}

void prCoreMimallocFreeBytes(void* memory) {
#if PR_ADDRESS_SANITIZER
    free(memory);
#else
    mi_free(memory);
#endif
}

size_t prCoreMimallocUsableBytes(const void* memory) {
#if PR_ADDRESS_SANITIZER && defined(__APPLE__)
    return malloc_size(memory);
#elif PR_ADDRESS_SANITIZER
    return malloc_usable_size(const_cast<void*>(memory));
#else
    return mi_usable_size(memory);
#endif
}
