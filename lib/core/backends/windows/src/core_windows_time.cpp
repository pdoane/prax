// The Windows backend of clocks and the system time.

#include "pr/core_windows_time.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pr_impl {

namespace {

uint64_t fileTimeTicks(FILETIME time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// The frequency of the performance counter, which is fixed at boot, and its tick in milliseconds.
struct Frequency {
    uint64_t ticksPerSecond;
    double milliseconds;
};

const Frequency& frequency() {
    static const Frequency value = [] {
        LARGE_INTEGER ticks;
        QueryPerformanceFrequency(&ticks);
        return Frequency{uint64_t(ticks.QuadPart), 1e3 / double(ticks.QuadPart)};
    }();
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTimePerf prCoreWindowsTimePerfNow(void) {
    // The call succeeds on every Windows since XP.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return PrTimePerf{uint64_t(now.QuadPart)};
}

uint64_t prCoreWindowsTimePerfNanoseconds(uint64_t ticks) {
    // Whole seconds and the ticks within a second convert apart, so that a long time does not overflow.
    uint64_t perSecond = frequency().ticksPerSecond;
    return ticks / perSecond * 1000000000 + ticks % perSecond * 1000000000 / perSecond;
}

double prCoreWindowsTimePerfMilliseconds(uint64_t ticks) {
    return double(ticks) * frequency().milliseconds;
}

double prCoreWindowsTimeMonotonicMilliseconds(void) {
    // The performance counter counts from boot, including the time that the system is in standby, hibernates or is in
    // connected standby.
    return double(prCoreWindowsTimePerfNow().ticks) * frequency().milliseconds;
}

PrTimeSystem prCoreWindowsTimeSystemNow(void) {
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    return prTimeSystemFromWindowsFileTime(fileTimeTicks(now));
}

int32_t prCoreWindowsTimeLocalOffsetSeconds(PrTimeSystem time) {
    // The offset is the local time less UTC, both as FILETIME ticks. A time that the system cannot convert, such as one
    // before 1601, is taken to be in UTC. A SYSTEMTIME holds whole milliseconds, and the offset whole seconds.
    if (time.ticks < kPrTimeSystemWindowsFileTimeEpoch)
        return 0;
    uint64_t ticks = prTimeSystemToWindowsFileTime(time);
    ticks -= ticks % kPrTimeSystemTicksPerSecond;
    FILETIME utcTime{DWORD(ticks), DWORD(ticks >> 32)};
    SYSTEMTIME utc;
    SYSTEMTIME local;
    FILETIME localTime;
    if (!FileTimeToSystemTime(&utcTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
            !SystemTimeToFileTime(&local, &localTime))
        return 0;
    return int32_t((int64_t(fileTimeTicks(localTime)) - int64_t(ticks)) / int64_t(kPrTimeSystemTicksPerSecond));
}
