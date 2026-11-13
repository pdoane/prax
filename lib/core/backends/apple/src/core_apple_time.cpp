// The Apple backend of clocks.

#include "pr/core_apple_time.h"

#include <mach/mach_time.h>

namespace pr_impl {

namespace {

// The time base of the Mach clocks, which is fixed at boot, and its tick in milliseconds.
struct Timebase {
    mach_timebase_info_data_t info;
    double milliseconds;
};

const Timebase& timebase() {
    static const Timebase value = [] {
        Timebase base{};
        mach_timebase_info(&base.info);
        base.milliseconds = double(base.info.numer) / double(base.info.denom) / 1e6;
        return base;
    }();
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTimePerf prCoreAppleTimePerfNow(void) {
    return PrTimePerf{mach_absolute_time()};
}

uint64_t prCoreAppleTimePerfNanoseconds(uint64_t ticks) {
    // The ticks divide by the denominator before they multiply, so that a long time does not overflow.
    const mach_timebase_info_data_t& info = timebase().info;
    return ticks / info.denom * info.numer + ticks % info.denom * info.numer / info.denom;
}

double prCoreAppleTimePerfMilliseconds(uint64_t ticks) {
    return double(ticks) * timebase().milliseconds;
}

double prCoreAppleTimeMonotonicMilliseconds(void) {
    return double(mach_continuous_time()) * timebase().milliseconds;
}
