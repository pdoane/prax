// The Windows backend of random bytes.

#include "pr/core_windows_random.h"

#include "pr/core_windows_fail.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

void prCoreWindowsRandomBytes(void* out, size_t size) {
    // A call fills at most ULONG_MAX bytes.
    uint8_t* bytes = static_cast<uint8_t*>(out);
    while (size != 0) {
        ULONG chunk = size > 0x40000000 ? 0x40000000 : ULONG(size);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            prCoreWindowsFailed("the system's random generator is unavailable", PrStringView(__FILE__),
                uint32_t(__LINE__));
        bytes += chunk;
        size -= chunk;
    }
}
