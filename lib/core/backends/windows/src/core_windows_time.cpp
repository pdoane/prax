// The Windows backend of clocks.

#include "pr/core_windows_time.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

double prCoreWindowsTimeMonotonicMilliseconds(void) {
    // The counter's frequency is fixed at boot. Both calls succeed on every Windows since XP.
    static const double kMillisecondsPerTick = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1e3 / double(frequency.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * kMillisecondsPerTick;
}

double prCoreWindowsTimeWallMilliseconds(void) {
    // A FILETIME counts units of 100 ns from 1601, which is 11644473600 seconds before the Unix epoch.
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    uint64_t ticks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return double(int64_t(ticks - 116444736000000000ull)) / 1e4;
}
