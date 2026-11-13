// Clocks through the platform.

#include "pr/time.h"

#include "pr/float.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_TIME_PERF_NOW)
#error "a target without a default for the group binds PR_PLATFORM_TIME_PERF_NOW"
#endif
#if !defined(PR_PLATFORM_TIME_SYSTEM_NOW)
#error "a target without a default for the group binds PR_PLATFORM_TIME_SYSTEM_NOW"
#endif

namespace pr_impl {

namespace {

constexpr uint64_t kTicksPerSecond = kPrTimeSystemTicksPerSecond;
constexpr uint64_t kTicksPerMillisecond = kTicksPerSecond / 1000;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTimePerf prTimePerfNow(void) {
    return PR_PLATFORM_TIME_PERF_NOW();
}

uint64_t prTimePerfNanoseconds(PrTimePerf start, PrTimePerf end) {
    return PR_PLATFORM_TIME_PERF_NANOSECONDS(end.ticks - start.ticks);
}

double prTimePerfMilliseconds(PrTimePerf start, PrTimePerf end) {
    return PR_PLATFORM_TIME_PERF_MILLISECONDS(end.ticks - start.ticks);
}

double prTimePerfSeconds(PrTimePerf start, PrTimePerf end) {
    return PR_PLATFORM_TIME_PERF_MILLISECONDS(end.ticks - start.ticks) / 1e3;
}

double prTimeMonotonicMilliseconds(void) {
    return PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS();
}

PrTimeSystem prTimeSystemNow(void) {
    return PR_PLATFORM_TIME_SYSTEM_NOW();
}

PrTimeSystem prTimeSystemFromUnix(int64_t seconds, uint32_t nanoseconds) {
    constexpr int64_t kEpochSeconds = int64_t(kPrTimeSystemUnixEpoch / kTicksPerSecond);
    if (seconds < -kEpochSeconds)
        return PrTimeSystem{0};
    return PrTimeSystem{uint64_t(seconds + kEpochSeconds) * kTicksPerSecond + nanoseconds / 100};
}

PrTimeSystem prTimeSystemFromWindowsFileTime(uint64_t fileTime) {
    return PrTimeSystem{fileTime + kPrTimeSystemWindowsFileTimeEpoch};
}

uint64_t prTimeSystemToWindowsFileTime(PrTimeSystem time) {
    return time.ticks > kPrTimeSystemWindowsFileTimeEpoch ? time.ticks - kPrTimeSystemWindowsFileTimeEpoch : 0;
}

// The conversions to and from milliseconds take whole milliseconds and the ticks within one apart, because a count of
// ticks since either epoch has more digits than a double holds.
PrTimeSystem prTimeSystemFromUnixMilliseconds(double milliseconds) {
    constexpr int64_t kEpochMilliseconds = int64_t(kPrTimeSystemUnixEpoch / kTicksPerMillisecond);
    constexpr double kLastMillisecond = double(UINT64_MAX / kTicksPerMillisecond - 1) - double(kEpochMilliseconds);
    if (!(milliseconds >= double(-kEpochMilliseconds)))
        return PrTimeSystem{0};
    if (milliseconds >= kLastMillisecond)
        return PrTimeSystem{UINT64_MAX};
    double whole = prF64Floor(milliseconds);
    uint64_t within = uint64_t((milliseconds - whole) * double(kTicksPerMillisecond));
    return PrTimeSystem{uint64_t(int64_t(whole) + kEpochMilliseconds) * kTicksPerMillisecond + within};
}

double prTimeSystemToUnixMilliseconds(PrTimeSystem time) {
    int64_t ticks = int64_t(time.ticks - kPrTimeSystemUnixEpoch);
    int64_t whole = ticks / int64_t(kTicksPerMillisecond);
    int64_t within = ticks % int64_t(kTicksPerMillisecond);
    return double(whole) + double(within) / double(kTicksPerMillisecond);
}

double prTimeSystemMilliseconds(PrTimeSystem start, PrTimeSystem end) {
    return double(int64_t(end.ticks - start.ticks)) / double(kTicksPerMillisecond);
}

int32_t prTimeLocalOffsetSeconds(PrTimeSystem time) {
    return PR_PLATFORM_TIME_LOCAL_OFFSET_SECONDS(time);
}
