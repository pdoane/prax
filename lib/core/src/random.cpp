// Random numbers: unpredictable bytes through the platform, and seeds from them.

#include "pr/random.h"

#include "pr/assert.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_RANDOM_BYTES)
#error "a target without a default for the group binds PR_PLATFORM_RANDOM_BYTES"
#endif

void prRandomBytes(void* PR_NULLABLE out, size_t size) {
    PR_ASSERT(out != nullptr || size == 0);
    if (size != 0)
        PR_PLATFORM_RANDOM_BYTES(out, size);
}

uint64_t prRandomSeed(void) {
    uint64_t seed = 0;
    prRandomBytes(&seed, sizeof(seed));
    return seed;
}
