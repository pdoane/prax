// The POSIX backend of clocks and the system time.

#include "pr/core_posix_time.h"

#include "pr/core_posix_fail.h"

#include <time.h>

namespace pr_impl {

namespace {

// Reads `clock`. A clock that cannot be read is fatal.
timespec readClock(clockid_t clock, const char* name) {
    timespec now;
    if (clock_gettime(clock, &now) != 0)
        prCorePosixFailed(PrStringView(name), PrStringView(__FILE__), uint32_t(__LINE__));
    return now;
}

#if defined(__linux__)
constexpr clockid_t kMonotonicClock = CLOCK_BOOTTIME;
#else
constexpr clockid_t kMonotonicClock = CLOCK_MONOTONIC;
#endif

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTimePerf prCorePosixTimePerfNow(void) {
    timespec now = readClock(CLOCK_MONOTONIC, "performance counter is unavailable");
    return PrTimePerf{uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec)};
}

uint64_t prCorePosixTimePerfNanoseconds(uint64_t ticks) {
    return ticks;
}

double prCorePosixTimePerfMilliseconds(uint64_t ticks) {
    return double(ticks) / 1e6;
}

double prCorePosixTimeMonotonicMilliseconds(void) {
    timespec now = readClock(kMonotonicClock, "monotonic clock is unavailable");
    return double(now.tv_sec) * 1e3 + double(now.tv_nsec) / 1e6;
}

PrTimeSystem prCorePosixTimeSystemNow(void) {
    timespec now = readClock(CLOCK_REALTIME, "system time is unavailable");
    return prTimeSystemFromUnix(int64_t(now.tv_sec), uint32_t(now.tv_nsec));
}

int32_t prCorePosixTimeLocalOffsetSeconds(PrTimeSystem time) {
    // A time that the C library cannot convert, such as one past its range, is taken to be in UTC.
    time_t seconds = time_t(int64_t(time.ticks / kPrTimeSystemTicksPerSecond) -
        int64_t(kPrTimeSystemUnixEpoch / kPrTimeSystemTicksPerSecond));
    tm local{};
    if (localtime_r(&seconds, &local) == nullptr)
        return 0;
    return int32_t(local.tm_gmtoff);
}
