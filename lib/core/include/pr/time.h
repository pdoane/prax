// Clocks, through the platform: a monotonic clock and the wall clock.

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

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
