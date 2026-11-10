// The POSIX backend of random bytes.

#include "pr/core_posix_random.h"

#include "pr/core_posix_fail.h"

#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

void prCorePosixRandomBytes(void* out, size_t size) {
#if defined(__APPLE__)
    // Apple's systems draw it from the kernel's generator and never fail. iOS declares no getentropy.
    arc4random_buf(out, size);
#else
    // getentropy fills at most 256 bytes in one call.
    constexpr size_t kMaxChunk = 256;
    uint8_t* bytes = static_cast<uint8_t*>(out);
    for (size_t at = 0; at < size; at += kMaxChunk) {
        size_t chunk = size - at < kMaxChunk ? size - at : kMaxChunk;
        if (getentropy(bytes + at, chunk) != 0)
            prCorePosixFailed("the system's random generator is unavailable", PrStringView(__FILE__),
                uint32_t(__LINE__));
    }
#endif
}
