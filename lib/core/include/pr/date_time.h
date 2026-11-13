// Dates and times of the proleptic Gregorian calendar: the date and time of day that a clock some offset from UTC
// shows, the days counted from the Unix epoch that dates name, the system times (pr/time.h) that they are, and the text
// of the common formats that write them.
//
// A day is a whole number of days from 1970-01-01, which is day zero, and negative before it. A year is astronomical:
// year 0 is 1 BC. A date and time holds no leap second's place in the count of days: second 60 is read as a leap
// second, and converts as the second after 59.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The days of the week, numbered as JavaScript's Date numbers them.
enum {
    PrDateTimeWeekday_Sunday,
    PrDateTimeWeekday_Monday,
    PrDateTimeWeekday_Tuesday,
    PrDateTimeWeekday_Wednesday,
    PrDateTimeWeekday_Thursday,
    PrDateTimeWeekday_Friday,
    PrDateTimeWeekday_Saturday,
};
typedef uint8_t PrDateTimeWeekday;

// A date and time of day, as a clock `offset` seconds ahead of UTC shows it. The conversions that make one set its
// weekday, and those that read one do not read it.
typedef struct PrDateTime {
    int64_t year;
    uint8_t month;                  // 1 through 12
    uint8_t day;                    // 1 through the days of the month
    uint8_t hour;                   // 0 through 23
    uint8_t minute;                 // 0 through 59
    uint8_t second;                 // 0 through 60
    PrDateTimeWeekday weekday;
    uint32_t nanosecond;            // 0 through 999999999
    int32_t offset;                 // the seconds that the clock is ahead of UTC, and negative behind it
} PrDateTime;

// Returns true if `year` has a February 29.
bool prDateTimeIsLeapYear(int64_t year);

// Returns the number of days in `month` of `year`. The month is 1 through 12.
uint32_t prDateTimeDaysInMonth(int64_t year, uint32_t month);

// Returns the day of the week of the day `days`.
PrDateTimeWeekday prDateTimeWeekdayFromDays(int64_t days);

// Returns midnight UTC at the start of the day `days`.
PrDateTime prDateTimeFromDays(int64_t days);

// Returns the day that the date of `dateTime` names, whose month and day must be in range. Its time of day and offset
// are not read.
int64_t prDateTimeToDays(PrDateTime dateTime);

// Returns the date and time of `time` on a clock `offset` seconds ahead of UTC, such as the local time zone's
// (prTimeLocalOffsetSeconds).
PrDateTime prDateTimeFromSystem(PrTimeSystem time, int32_t offset);

// Returns the system time of `dateTime`, whose fields must be in range. A time before the epoch of the system time is
// its epoch, and one past its range is its last tick.
PrTimeSystem prDateTimeToSystem(PrDateTime dateTime);

// The formats of the text of a date and time.
enum {
    // RFC 3339's profile of ISO 8601, as in "2026-01-15T10:30:00Z" and "2026-01-15T10:30:00.25+05:30". A fraction of a
    // second is written if there is one, with the digits that it needs. A year outside 0 through 9999 is written as ISO
    // 8601's expanded years, with a sign and six or more digits, as JavaScript's Date writes it. Reading takes a
    // lowercase "t" or "z", and an offset without its colon.
    PrDateTimeFormat_Iso8601,
    // RFC 5322's date and time, as in "Thu, 15 Jan 2026 10:30:00 +0530". Reading takes a date without its weekday, a
    // time without its seconds, and the zones UT, GMT and the North American zones that RFC 5322 makes obsolete.
    PrDateTimeFormat_Rfc5322,
    // RFC 9110's HTTP-date, as in "Thu, 15 Jan 2026 05:00:00 GMT", which is always in UTC. Reading takes the obsolete
    // forms of RFC 850 and C's asctime as well. It reads the tokens of the day, the month's name, the year and the
    // time, and skips any other word, so that the dates that servers write loosely, such as with a zone of UTC, still
    // read. A two-digit year names one of 1970 through 2069.
    PrDateTimeFormat_Http,
    // A date and time without an offset, as in "2026-01-15 10:30:00".
    PrDateTimeFormat_Simple,
    // A date, as in "2026-01-15".
    PrDateTimeFormat_Date,
    // A time of day, as in "10:30:00".
    PrDateTimeFormat_Time,
};
typedef uint8_t PrDateTimeFormat;

// Appends `dateTime` in `format` to `out`. A format other than ISO 8601 writes the year as its decimal digits, which a
// year outside 0 through 9999 does not fit. An offset is written to the minute. The HTTP-date writes the time in UTC,
// and the formats without an offset write the time as the clock shows it. The weekday that a format writes is the
// date's, whatever the weekday field holds.
void prDateTimeFormat(PrDateTime dateTime, PrDateTimeFormat format, PrTextBuffer* out);

// Reads the whole of `text` as a date and time in `format`. Returns false if `text` is not one, and leaves `out`
// unchanged in that case. Otherwise sets the fields of what the format holds and leaves the others as they are: the
// date and its weekday, the time of day with its nanoseconds, which are zero if the text has no fraction, and the
// offset, which an HTTP-date holds as zero.
bool prDateTimeParse(PrStringView text, PrDateTimeFormat format, PrDateTime* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
