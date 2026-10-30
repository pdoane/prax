// The POSIX backend of random bytes.

#include "pr/core_posix_random.h"

#include "pr/core_posix_fail.h"

#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

void prCorePosixRandomBytes(void* out, size_t size) {
    // getentropy fills at most 256 bytes in one call.
    constexpr size_t kMaxChunk = 256;
    uint8_t* bytes = static_cast<uint8_t*>(out);
    for (size_t at = 0; at < size; at += kMaxChunk) {
        size_t chunk = size - at < kMaxChunk ? size - at : kMaxChunk;
        if (getentropy(bytes + at, chunk) != 0)
            prCorePosixFailed("the system's random generator is unavailable", PrStringView(__FILE__),
                uint32_t(__LINE__));
    }
}
