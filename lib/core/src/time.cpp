// Clocks through the platform.

#include "pr/time.h"

#include "core/src/config.h"

#if !defined(PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS)
#error "a build without the default platform binds PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS"
#endif

double prTimeMonotonicMilliseconds(void) {
    return PR_PLATFORM_TIME_MONOTONIC_MILLISECONDS();
}

double prTimeWallMilliseconds(void) {
    return PR_PLATFORM_TIME_WALL_MILLISECONDS();
}

double prTimeLocalOffsetMilliseconds(double wall) {
    return PR_PLATFORM_TIME_LOCAL_OFFSET_MILLISECONDS(wall);
}
