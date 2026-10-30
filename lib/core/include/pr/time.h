// Clocks, through the platform: a monotonic clock, the wall clock, and the local time zone.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The monotonic clock, in milliseconds since an unspecified origin. Adjusting the wall clock does not change it, and
// readings are comparable only within one process.
double prTimeMonotonicMilliseconds(void);

// The wall clock, in milliseconds since the Unix epoch. Adjusting the clock changes it.
double prTimeWallMilliseconds(void);

// Returns the offset of the local time zone from UTC at the wall clock's time `wall`, in milliseconds, which the local
// time is ahead of UTC. The offset includes daylight saving time if it is in effect at that time.
double prTimeLocalOffsetMilliseconds(double wall);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
