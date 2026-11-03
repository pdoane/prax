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

double prCoreWindowsTimeLocalOffsetMilliseconds(double wall) {
    // The offset is the local time less UTC, both as FILETIME ticks. A time that the system cannot convert, such as one
    // before 1601, is taken to be in UTC.
    int64_t ticks = int64_t(wall * 1e4) + 116444736000000000ll;
    if (ticks < 0)
        return 0;
    FILETIME utcTime{DWORD(uint64_t(ticks)), DWORD(uint64_t(ticks) >> 32)};
    SYSTEMTIME utc;
    SYSTEMTIME local;
    FILETIME localTime;
    if (!FileTimeToSystemTime(&utcTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
            !SystemTimeToFileTime(&local, &localTime))
        return 0;
    uint64_t localTicks = (uint64_t(localTime.dwHighDateTime) << 32) | localTime.dwLowDateTime;
    // A SYSTEMTIME holds whole milliseconds, so the local time has lost what `ticks` holds below them.
    return double(int64_t(localTicks) - (ticks - ticks % 10000)) / 1e4;
}
