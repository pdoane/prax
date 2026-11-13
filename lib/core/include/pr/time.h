// Clocks, through the platform: the performance counter, the monotonic clock, the system time, and the local time zone.
//
// The clocks serve different needs, and their readings do not mix:
// - The performance counter measures how long work takes. It is the cheapest reading of the platform, and it converts
//   only a difference of two readings. Whether it advances while the system sleeps is unspecified.
// - The monotonic clock times deadlines, timeouts and timestamps, in milliseconds. It advances while the system sleeps,
//   so that a deadline set before a sleep has passed after it.
// - The system time is the moment on the calendar, which adjusting the system's clock changes.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A reading of the performance counter. Its ticks count from an unspecified origin at the platform's rate, and readings
// are comparable only within one process.
typedef struct PrTimePerf {
    uint64_t ticks;
} PrTimePerf;

// Reads the performance counter.
PrTimePerf prTimePerfNow(void);

// Return the time from `start` to `end`, two readings of the performance counter, in nanoseconds exactly or as
// milliseconds or seconds. `end` must not be before `start`.
uint64_t prTimePerfNanoseconds(PrTimePerf start, PrTimePerf end);
double prTimePerfMilliseconds(PrTimePerf start, PrTimePerf end);
double prTimePerfSeconds(PrTimePerf start, PrTimePerf end);

// The monotonic clock, in milliseconds since an unspecified origin. It advances while the system sleeps, adjusting the
// system's clock does not change it, and readings are comparable only within one process.
double prTimeMonotonicMilliseconds(void);

// The system time: units of 100 ns since 1600-03-01 00:00 UTC, without leap seconds, as Unix time counts. The epoch
// starts a 400-year cycle of the Gregorian calendar, with the leap day at the end of its years, so that a date
// (pr/date_time.h) follows from unsigned divisions. 64 bits reach past the year 58000. Adjusting the system's clock
// changes it.
typedef struct PrTimeSystem {
    uint64_t ticks;
} PrTimeSystem;

// The ticks of the system time in a second, and the system time of 1970-01-01 00:00 UTC, the Unix epoch.
PR_CONSTANT uint64_t kPrTimeSystemTicksPerSecond = 10000000;
PR_CONSTANT uint64_t kPrTimeSystemUnixEpoch = 116709120000000000;

// Reads the system time.
PrTimeSystem prTimeSystemNow(void);

// The system time of 1601-01-01 00:00 UTC, the epoch of a Windows FILETIME, which counts in the same units.
PR_CONSTANT uint64_t kPrTimeSystemWindowsFileTimeEpoch = 264384000000000;

// Returns the system time of `seconds` and `nanoseconds` since the Unix epoch, as a timespec holds a time, to the tick
// below it. A time before the epoch of the system time is its epoch.
PrTimeSystem prTimeSystemFromUnix(int64_t seconds, uint32_t nanoseconds);

// Returns the system time of a Windows FILETIME, whose two halves `fileTime` holds as one number.
PrTimeSystem prTimeSystemFromWindowsFileTime(uint64_t fileTime);

// Returns `time` as a Windows FILETIME, its two halves as one number. A time before the FILETIME's epoch is its epoch.
uint64_t prTimeSystemToWindowsFileTime(PrTimeSystem time);

// Returns the system time of `milliseconds` since the Unix epoch, as JavaScript's Date counts it, to the tick below it.
// A time before the epoch of the system time, or NaN, is its epoch, and one past its range is the last tick.
PrTimeSystem prTimeSystemFromUnixMilliseconds(double milliseconds);

// Returns `time` in milliseconds since the Unix epoch, as JavaScript's Date counts it.
double prTimeSystemToUnixMilliseconds(PrTimeSystem time);

// Returns the time from `start` to `end`, two system times, in milliseconds, which is negative if `end` is before
// `start`.
double prTimeSystemMilliseconds(PrTimeSystem start, PrTimeSystem end);

// Returns the offset of the local time zone from UTC at `time`, in seconds, which the local time is ahead of UTC. The
// offset includes daylight saving time if it is in effect at that time.
int32_t prTimeLocalOffsetSeconds(PrTimeSystem time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
