// Calendar dates: days counted from the Unix epoch, and the dates of the proleptic Gregorian calendar that they name.

#include "pr/time_date.h"

#include "pr/assert.h"
#include "pr/float.h"

namespace pr_impl {

namespace {

// The days of an era of the Gregorian calendar, which repeats every 400 years.
constexpr int64_t kEraDays = 146097;

// The day of 0000-03-01, which starts the first era of the count below.
constexpr int64_t kMarchFirstOfYearZero = -719468;

constexpr double kDayMilliseconds = 86400000;

// Returns `value` modulo `divisor`, in [0, divisor).
int64_t floorMod(int64_t value, int64_t divisor) {
    int64_t remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prTimeIsLeapYear(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint32_t prTimeDaysInMonth(int64_t year, uint32_t month) {
    PR_ASSERT(month >= 1 && month <= 12);
    constexpr uint32_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && prTimeIsLeapYear(year) ? 29 : kDays[month - 1];
}

// The conversions count days from 0000-03-01 in eras of 400 years, and years from March, so that a leap day ends its
// year (H. Hinnant, "chrono-Compatible Low-Level Date Algorithms").
PrTimeDate prTimeDateFromDays(int64_t days) {
    int64_t shifted = days - kMarchFirstOfYearZero;
    int64_t era = (shifted >= 0 ? shifted : shifted - (kEraDays - 1)) / kEraDays;
    int64_t dayOfEra = shifted - era * kEraDays;
    int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    int64_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    PrTimeDate date{};
    date.day = uint32_t(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    date.month = uint32_t(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);
    date.year = yearOfEra + era * 400 + (date.month <= 2 ? 1 : 0);
    return date;
}

int64_t prTimeDateToDays(PrTimeDate date) {
    PR_ASSERT(date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= 31);
    int64_t year = date.year - (date.month <= 2 ? 1 : 0);
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yearOfEra = year - era * 400;
    int64_t dayOfYear = (153 * (date.month > 2 ? date.month - 3 : date.month + 9) + 2) / 5 + date.day - 1;
    int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * kEraDays + dayOfEra + kMarchFirstOfYearZero;
}

PrTimeWeekday prTimeWeekdayFromDays(int64_t days) {
    // 1970-01-01 was a Thursday.
    return PrTimeWeekday(floorMod(days + PrTimeWeekday_Thursday, 7));
}

int64_t prTimeDaysFromMilliseconds(double milliseconds, double offset) {
    return int64_t(floorF64((milliseconds + offset) / kDayMilliseconds));
}
