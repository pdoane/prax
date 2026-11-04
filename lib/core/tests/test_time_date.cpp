#include "pr/time_date.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

bool isDate(PrTimeDate date, int64_t year, uint32_t month, uint32_t day) {
    return date.year == year && date.month == month && date.day == day;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a day is the date that it falls on, and the date is the day again, on either side of the epoch") {
    CHECK(isDate(prTimeDateFromDays(0), 1970, 1, 1));
    CHECK(isDate(prTimeDateFromDays(-1), 1969, 12, 31));
    CHECK(isDate(prTimeDateFromDays(11017), 2000, 3, 1));
    CHECK(isDate(prTimeDateFromDays(19782), 2024, 2, 29));
    CHECK(isDate(prTimeDateFromDays(-719528), 0, 1, 1));
    CHECK(prTimeDateToDays(PrTimeDate{2000, 3, 1}) == 11017);
    CHECK(prTimeDateToDays(PrTimeDate{0, 1, 1}) == -719528);
    for (int64_t days = -800000; days <= 800000; days += 997)
        CHECK(prTimeDateToDays(prTimeDateFromDays(days)) == days);
}

TEST_CASE("a leap year is every fourth, except centuries not divisible by 400, and gives February 29 days") {
    CHECK(prTimeIsLeapYear(2024));
    CHECK(prTimeIsLeapYear(2000));
    CHECK(prTimeIsLeapYear(0));
    CHECK_FALSE(prTimeIsLeapYear(1900));
    CHECK_FALSE(prTimeIsLeapYear(2023));
    CHECK(prTimeDaysInMonth(2024, 2) == 29);
    CHECK(prTimeDaysInMonth(2023, 2) == 28);
    CHECK(prTimeDaysInMonth(2023, 4) == 30);
    CHECK(prTimeDaysInMonth(2023, 12) == 31);
}

TEST_CASE("a day's weekday counts from the epoch's Thursday, in both directions") {
    CHECK(prTimeWeekdayFromDays(0) == PrTimeWeekday_Thursday);
    CHECK(prTimeWeekdayFromDays(-1) == PrTimeWeekday_Wednesday);
    CHECK(prTimeWeekdayFromDays(3) == PrTimeWeekday_Sunday);
    CHECK(prTimeWeekdayFromDays(19782) == PrTimeWeekday_Thursday);
}

TEST_CASE("a time falls on the day that a clock ahead of UTC by the offset shows") {
    // 2026-01-15 at 23:30 UTC.
    double time = 1768519800000.0;
    CHECK(prTimeDaysFromMilliseconds(time, 0) == 20468);
    CHECK(prTimeDaysFromMilliseconds(time, 2 * 3600000.0) == 20469);
    CHECK(prTimeDaysFromMilliseconds(time, -24 * 3600000.0) == 20467);
    CHECK(prTimeDaysFromMilliseconds(-1, 0) == -1);
}
