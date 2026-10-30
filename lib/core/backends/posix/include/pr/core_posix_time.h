// The POSIX backend of clocks, which it reads with clock_gettime. It serves the Unix-like targets, and it is the
// default binding of clocks on them.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implement prTimeMonotonicMilliseconds, prTimeWallMilliseconds and prTimeLocalOffsetMilliseconds. The local time zone
// is the C library's (localtime_r), which follows TZ.
double prCorePosixTimeMonotonicMilliseconds(void);
double prCorePosixTimeWallMilliseconds(void);
double prCorePosixTimeLocalOffsetMilliseconds(double wall);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
