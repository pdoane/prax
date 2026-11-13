// The POSIX backend of clocks and the system time, which it reads with clock_gettime. It serves the Unix-like targets,
// and it is the default binding of the system time on them, and of clocks on those other than Apple's.

#pragma once

#include "pr/base.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implement prTimePerfNow, the conversions of a difference of its ticks, and prTimeMonotonicMilliseconds. The
// performance counter is CLOCK_MONOTONIC in nanoseconds. The monotonic clock is CLOCK_BOOTTIME on Linux, which advances
// while the system is suspended, and CLOCK_MONOTONIC elsewhere.
PrTimePerf prCorePosixTimePerfNow(void);
uint64_t prCorePosixTimePerfNanoseconds(uint64_t ticks);
double prCorePosixTimePerfMilliseconds(uint64_t ticks);
double prCorePosixTimeMonotonicMilliseconds(void);

// Implement prTimeSystemNow and prTimeLocalOffsetSeconds. The system time is CLOCK_REALTIME, and the local time zone is
// the C library's (localtime_r), which follows TZ.
PrTimeSystem prCorePosixTimeSystemNow(void);
int32_t prCorePosixTimeLocalOffsetSeconds(PrTimeSystem time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
