// Calendar dates: days counted from the Unix epoch, and the dates of the proleptic Gregorian calendar that they name.
//
// A day is a whole number of days from 1970-01-01, which is day zero, and negative before it. A year is astronomical:
// year 0 is 1 BC.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A date of the proleptic Gregorian calendar.
typedef struct PrTimeDate {
    int64_t year;
    uint32_t month;                 // 1 through 12
    uint32_t day;                   // 1 through the days of the month
} PrTimeDate;

// The days of the week, numbered as JavaScript's Date numbers them.
enum {
    PrTimeWeekday_Sunday,
    PrTimeWeekday_Monday,
    PrTimeWeekday_Tuesday,
    PrTimeWeekday_Wednesday,
    PrTimeWeekday_Thursday,
    PrTimeWeekday_Friday,
    PrTimeWeekday_Saturday,
};
typedef uint8_t PrTimeWeekday;

// Returns true if `year` has a February 29.
bool prTimeIsLeapYear(int64_t year);

// Returns the number of days in `month` of `year`. The month is 1 through 12.
uint32_t prTimeDaysInMonth(int64_t year, uint32_t month);

// Returns the date of the day `days`.
PrTimeDate prTimeDateFromDays(int64_t days);

// Returns the day of `date`, whose month and day must be in range.
int64_t prTimeDateToDays(PrTimeDate date);

// Returns the day of the week of the day `days`.
PrTimeWeekday prTimeWeekdayFromDays(int64_t days);

// Returns the day that the time `milliseconds` since the epoch falls on, as a clock `offset` milliseconds ahead of UTC
// shows it, such as the local time zone's (prTimeLocalOffsetMilliseconds). The time must be finite.
int64_t prTimeDaysFromMilliseconds(double milliseconds, double offset);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
