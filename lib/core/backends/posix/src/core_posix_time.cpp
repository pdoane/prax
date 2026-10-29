// The POSIX backend of clocks.

#include "pr/core_posix_time.h"

#include "pr/core_posix_fail.h"

#include <time.h>

namespace pr_impl {

namespace {

// Returns the reading of `clock` in milliseconds. A clock that cannot be read is fatal.
double milliseconds(clockid_t clock, const char* name) {
    timespec now;
    if (clock_gettime(clock, &now) != 0)
        prCorePosixFailed(PrStringView(name), PrStringView(__FILE__), uint32_t(__LINE__));
    return double(now.tv_sec) * 1e3 + double(now.tv_nsec) / 1e6;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

double prCorePosixTimeMonotonicMilliseconds(void) {
    return milliseconds(CLOCK_MONOTONIC, "monotonic clock is unavailable");
}

double prCorePosixTimeWallMilliseconds(void) {
    return milliseconds(CLOCK_REALTIME, "wall clock is unavailable");
}
