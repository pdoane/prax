// The Apple backend of clocks, which it reads from Mach's time base. It serves Apple's targets, and it is the default
// binding of clocks on them.

#pragma once

#include "pr/base.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implement prTimePerfNow, the conversions of a difference of its ticks, and prTimeMonotonicMilliseconds. The
// performance counter is mach_absolute_time at the rate of mach_timebase_info, and the monotonic clock is
// mach_continuous_time, which advances while the system sleeps.
PrTimePerf prCoreAppleTimePerfNow(void);
uint64_t prCoreAppleTimePerfNanoseconds(uint64_t ticks);
double prCoreAppleTimePerfMilliseconds(uint64_t ticks);
double prCoreAppleTimeMonotonicMilliseconds(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
