// The Windows backend of clocks: the performance counter, and the system time at its full precision. It serves Windows,
// and it is the default binding of clocks there.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Implement prTimeMonotonicMilliseconds and prTimeWallMilliseconds.
double prCoreWindowsTimeMonotonicMilliseconds(void);
double prCoreWindowsTimeWallMilliseconds(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
