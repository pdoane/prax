// The Windows backend of clocks and the system time: the performance counter, and the system time at its full
// precision. It serves Windows, and it is the default binding of clocks and the system time there.

#pragma once

#include "pr/base.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implement prTimePerfNow, the conversions of a difference of its ticks, and prTimeMonotonicMilliseconds. Both clocks
// are QueryPerformanceCounter, which advances while the system sleeps.
PrTimePerf prCoreWindowsTimePerfNow(void);
uint64_t prCoreWindowsTimePerfNanoseconds(uint64_t ticks);
double prCoreWindowsTimePerfMilliseconds(uint64_t ticks);
double prCoreWindowsTimeMonotonicMilliseconds(void);

// Implement prTimeSystemNow and prTimeLocalOffsetSeconds. The local time zone is the system's active time zone.
PrTimeSystem prCoreWindowsTimeSystemNow(void);
int32_t prCoreWindowsTimeLocalOffsetSeconds(PrTimeSystem time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
