#include "pr/date_time.h"
#include "pr/float.h"
#include "pr/text_buffer.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

bool isDate(PrDateTime date, int64_t year, uint32_t month, uint32_t day) {
    return date.year == year && date.month == month && date.day == day;
}

// Returns `dateTime` in `format`.
String formatted(PrDateTime dateTime, PrDateTimeFormat format) {
    PrTextBuffer text{};
    prDateTimeFormat(dateTime, format, &text);
    String result(prTextBufferView(&text));
    prTextBufferReset(&text);
    return result;
}

// Returns the system time that `text` in `format` names, or the epoch if it names none.
uint64_t parsedTicks(PrStringView text, PrDateTimeFormat format) {
    PrDateTime dateTime{};
    return prDateTimeParse(text, format, &dateTime) ? prDateTimeToSystem(dateTime).ticks : 0;
}

// 2026-01-15 at 23:30:05 UTC, a Thursday.
constexpr int64_t kThursday = 1768519805;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a day is the date that it falls on, and the date is the day again, on either side of the epoch") {
    CHECK(isDate(prDateTimeFromDays(0), 1970, 1, 1));
    CHECK(isDate(prDateTimeFromDays(-1), 1969, 12, 31));
    CHECK(isDate(prDateTimeFromDays(11017), 2000, 3, 1));
    CHECK(isDate(prDateTimeFromDays(19782), 2024, 2, 29));
    CHECK(isDate(prDateTimeFromDays(-719528), 0, 1, 1));
    CHECK(isDate(prDateTimeFromDays(-719529), -1, 12, 31));
    CHECK(prDateTimeToDays(PrDateTime{.year = 2000, .month = 3, .day = 1}) == 11017);
    CHECK(prDateTimeToDays(PrDateTime{.year = 0, .month = 1, .day = 1}) == -719528);
    for (int64_t days = -800000; days <= 800000; days += 997) {
        PrDateTime date = prDateTimeFromDays(days);
        CHECK(prDateTimeToDays(date) == days);
        CHECK(date.weekday == prDateTimeWeekdayFromDays(days));
        CHECK(date.hour == 0);
        CHECK(date.offset == 0);
    }
}

TEST_CASE("a leap year is every fourth, except centuries not divisible by 400, and gives February 29 days") {
    CHECK(prDateTimeIsLeapYear(2024));
    CHECK(prDateTimeIsLeapYear(2000));
    CHECK(prDateTimeIsLeapYear(0));
    CHECK_FALSE(prDateTimeIsLeapYear(1900));
    CHECK_FALSE(prDateTimeIsLeapYear(2023));
    CHECK(prDateTimeDaysInMonth(2024, 2) == 29);
    CHECK(prDateTimeDaysInMonth(2023, 2) == 28);
    CHECK(prDateTimeDaysInMonth(2023, 4) == 30);
    CHECK(prDateTimeDaysInMonth(2023, 12) == 31);
}

TEST_CASE("a day's weekday counts from the epoch's Thursday, in both directions") {
    CHECK(prDateTimeWeekdayFromDays(0) == PrDateTimeWeekday_Thursday);
    CHECK(prDateTimeWeekdayFromDays(-1) == PrDateTimeWeekday_Wednesday);
    CHECK(prDateTimeWeekdayFromDays(3) == PrDateTimeWeekday_Sunday);
    CHECK(prDateTimeWeekdayFromDays(19782) == PrDateTimeWeekday_Thursday);
}

TEST_CASE("a system time is the date and time that a clock ahead of UTC by the offset shows") {
    PrTimeSystem time = prTimeSystemFromUnix(kThursday, 123456789);
    PrDateTime utc = prDateTimeFromSystem(time, 0);
    CHECK(isDate(utc, 2026, 1, 15));
    CHECK(utc.hour == 23);
    CHECK(utc.minute == 30);
    CHECK(utc.second == 5);
    CHECK(utc.nanosecond == 123456700);
    CHECK(utc.weekday == PrDateTimeWeekday_Thursday);
    CHECK(utc.offset == 0);
    PrDateTime ahead = prDateTimeFromSystem(time, 2 * 3600 + 45 * 60);
    CHECK(isDate(ahead, 2026, 1, 16));
    CHECK(ahead.hour == 2);
    CHECK(ahead.minute == 15);
    CHECK(ahead.weekday == PrDateTimeWeekday_Friday);
    CHECK(prDateTimeToSystem(utc).ticks == time.ticks);
    CHECK(prDateTimeToSystem(ahead).ticks == time.ticks);

    // The epoch is the first day of an era, a Wednesday, and a leap day ends the year of the era that holds it.
    PrDateTime epoch = prDateTimeFromSystem(PrTimeSystem{0}, 0);
    CHECK(isDate(epoch, 1600, 3, 1));
    CHECK(epoch.weekday == PrDateTimeWeekday_Wednesday);
    CHECK(isDate(prDateTimeFromSystem(prTimeSystemFromUnix(951782400, 0), 0), 2000, 2, 29));
    CHECK(isDate(prDateTimeFromSystem(prTimeSystemFromUnix(951868800, 0), 0), 2000, 3, 1));

    // A clock behind UTC at the epoch shows a time before it, which converts back to the epoch.
    PrDateTime before = prDateTimeFromSystem(PrTimeSystem{0}, -3600);
    CHECK(isDate(before, 1600, 2, 29));
    CHECK(before.hour == 23);
    CHECK(before.weekday == PrDateTimeWeekday_Tuesday);
    CHECK(prDateTimeToSystem(before).ticks == 0);
    PrDateTime early{.year = 1500, .month = 6, .day = 1};
    CHECK(prDateTimeToSystem(early).ticks == 0);

    // A leap second is the second after 59.
    PrDateTime leap = utc;
    leap.second = 60;
    CHECK(prDateTimeToSystem(leap).ticks == time.ticks + 55 * kPrTimeSystemTicksPerSecond);

    // Every day across eras agrees with the count of days from the Unix epoch.
    for (int64_t days = -135080; days <= 800000; days += 997) {
        PrTimeSystem midnight{uint64_t(days + 135080) * 86400 * kPrTimeSystemTicksPerSecond};
        PrDateTime dateTime = prDateTimeFromSystem(midnight, 0);
        CHECK(prDateTimeToDays(dateTime) == days);
        CHECK(dateTime.weekday == prDateTimeWeekdayFromDays(days));
        CHECK(prDateTimeToSystem(dateTime).ticks == midnight.ticks);
    }
}

TEST_CASE("a system time converts to and from Unix time, FILETIME and the milliseconds of JavaScript's Date") {
    CHECK(prTimeSystemFromUnix(0, 0).ticks == kPrTimeSystemUnixEpoch);
    CHECK(prTimeSystemFromUnix(-11670912000, 0).ticks == 0);
    CHECK(prTimeSystemFromUnix(-11670912001, 0).ticks == 0);
    // 1970-01-01 is 11644473600 seconds after 1601-01-01.
    CHECK(prTimeSystemFromWindowsFileTime(116444736000000000).ticks == kPrTimeSystemUnixEpoch);
    CHECK(prTimeSystemToWindowsFileTime(PrTimeSystem{kPrTimeSystemUnixEpoch}) == 116444736000000000);
    CHECK(prTimeSystemToWindowsFileTime(PrTimeSystem{0}) == 0);
    CHECK(isDate(prDateTimeFromSystem(prTimeSystemFromWindowsFileTime(0), 0), 1601, 1, 1));
    CHECK(prTimeSystemToUnixMilliseconds(PrTimeSystem{kPrTimeSystemUnixEpoch}) == 0);
    CHECK(prTimeSystemToUnixMilliseconds(PrTimeSystem{kPrTimeSystemUnixEpoch - 5000}) == -0.5);
    // The ticks within a millisecond survive the trip, at a time that has more digits than a double holds in ticks.
    PrTimeSystem precise{kPrTimeSystemUnixEpoch + 1768519805123ull * 10000 + 2500};
    CHECK(prTimeSystemToUnixMilliseconds(precise) == 1768519805123.25);
    CHECK(prTimeSystemFromUnixMilliseconds(prTimeSystemToUnixMilliseconds(precise)).ticks == precise.ticks);
    CHECK(prTimeSystemFromUnixMilliseconds(-1e300).ticks == 0);
    CHECK(prTimeSystemFromUnixMilliseconds(kPrF64Nan).ticks == 0);
    CHECK(prTimeSystemFromUnixMilliseconds(1e300).ticks == UINT64_MAX);
    CHECK(prTimeSystemMilliseconds(precise, PrTimeSystem{precise.ticks + 25000}) == 2.5);
    CHECK(prTimeSystemMilliseconds(PrTimeSystem{precise.ticks + 25000}, precise) == -2.5);
}

TEST_CASE("a date and time is written in each format, with the offset and the fraction that it has") {
    PrDateTime utc = prDateTimeFromSystem(prTimeSystemFromUnix(kThursday, 0), 0);
    PrDateTime india = prDateTimeFromSystem(prTimeSystemFromUnix(kThursday, 250000000), 5 * 3600 + 1800);
    PrDateTime pacific = prDateTimeFromSystem(prTimeSystemFromUnix(kThursday, 0), -8 * 3600);
    CHECK(formatted(utc, PrDateTimeFormat_Iso8601) == "2026-01-15T23:30:05Z");
    CHECK(formatted(india, PrDateTimeFormat_Iso8601) == "2026-01-16T05:00:05.25+05:30");
    CHECK(formatted(pacific, PrDateTimeFormat_Iso8601) == "2026-01-15T15:30:05-08:00");
    CHECK(formatted(utc, PrDateTimeFormat_Rfc5322) == "Thu, 15 Jan 2026 23:30:05 +0000");
    CHECK(formatted(india, PrDateTimeFormat_Rfc5322) == "Fri, 16 Jan 2026 05:00:05 +0530");
    CHECK(formatted(india, PrDateTimeFormat_Http) == "Thu, 15 Jan 2026 23:30:05 GMT");
    CHECK(formatted(pacific, PrDateTimeFormat_Simple) == "2026-01-15 15:30:05");
    CHECK(formatted(pacific, PrDateTimeFormat_Date) == "2026-01-15");
    CHECK(formatted(pacific, PrDateTimeFormat_Time) == "15:30:05");

    // ISO 8601 writes a year outside four digits as an expanded year, and a format reads the weekday from the date.
    PrDateTime far{.year = 275760, .month = 9, .day = 13, .weekday = PrDateTimeWeekday_Monday};
    CHECK(formatted(far, PrDateTimeFormat_Iso8601) == "+275760-09-13T00:00:00Z");
    far.year = -271821;
    far.month = 4;
    far.day = 20;
    CHECK(formatted(far, PrDateTimeFormat_Iso8601) == "-271821-04-20T00:00:00Z");
    PrDateTime nanoseconds = utc;
    nanoseconds.nanosecond = 1;
    CHECK(formatted(nanoseconds, PrDateTimeFormat_Iso8601) == "2026-01-15T23:30:05.000000001Z");
    PrDateTime stale = utc;
    stale.weekday = PrDateTimeWeekday_Monday;
    CHECK(formatted(stale, PrDateTimeFormat_Http) == "Thu, 15 Jan 2026 23:30:05 GMT");
}

TEST_CASE("each format reads what it writes, and the forms that its standard also allows") {
    uint64_t thursday = prTimeSystemFromUnix(kThursday, 0).ticks;
    CHECK(parsedTicks("2026-01-15T23:30:05Z", PrDateTimeFormat_Iso8601) == thursday);
    CHECK(parsedTicks("2026-01-15t23:30:05z", PrDateTimeFormat_Iso8601) == thursday);
    CHECK(parsedTicks("2026-01-16T05:00:05+05:30", PrDateTimeFormat_Iso8601) == thursday);
    CHECK(parsedTicks("2026-01-16T05:00:05+0530", PrDateTimeFormat_Iso8601) == thursday);
    CHECK(parsedTicks("2026-01-15T15:30:05-08:00", PrDateTimeFormat_Iso8601) == thursday);
    CHECK(parsedTicks("2026-01-15T23:30:05.25Z", PrDateTimeFormat_Iso8601) == thursday + 2500000);
    CHECK(parsedTicks("2026-01-15T23:30:05.1234567891Z", PrDateTimeFormat_Iso8601) == thursday + 1234567);
    CHECK(parsedTicks("Thu, 15 Jan 2026 23:30:05 +0000", PrDateTimeFormat_Rfc5322) == thursday);
    CHECK(parsedTicks("15 Jan 2026 15:30:05 PST", PrDateTimeFormat_Rfc5322) == thursday);
    CHECK(parsedTicks("Thu, 15 Jan 26 23:30:05 GMT", PrDateTimeFormat_Rfc5322) == thursday);
    CHECK(parsedTicks("Thu, 15 Jan 2026 23:30 +0000", PrDateTimeFormat_Rfc5322) == thursday - 5 * 10000000);
    CHECK(parsedTicks("Thu, 15 Jan 2026 23:30:05 GMT", PrDateTimeFormat_Http) == thursday);
    CHECK(parsedTicks("Thursday, 15-Jan-26 23:30:05 GMT", PrDateTimeFormat_Http) == thursday);
    CHECK(parsedTicks("Thu Jan 15 23:30:05 2026", PrDateTimeFormat_Http) == thursday);
    CHECK(parsedTicks("Thu, 15 Jan 2026 23:30:05 UTC", PrDateTimeFormat_Http) == thursday);

    // A parse sets what its format holds, and leaves the rest.
    PrDateTime dateTime = prDateTimeFromSystem(prTimeSystemFromUnix(kThursday, 0), 3600);
    REQUIRE(prDateTimeParse("1999-12-31", PrDateTimeFormat_Date, &dateTime));
    CHECK(isDate(dateTime, 1999, 12, 31));
    CHECK(dateTime.weekday == PrDateTimeWeekday_Friday);
    CHECK(dateTime.hour == 0);
    CHECK(dateTime.minute == 30);
    CHECK(dateTime.offset == 3600);
    REQUIRE(prDateTimeParse("08:09:10", PrDateTimeFormat_Time, &dateTime));
    CHECK(isDate(dateTime, 1999, 12, 31));
    CHECK(dateTime.hour == 8);
    CHECK(dateTime.second == 10);
    REQUIRE(prDateTimeParse("2000-02-29 12:00:00", PrDateTimeFormat_Simple, &dateTime));
    CHECK(isDate(dateTime, 2000, 2, 29));
    CHECK(dateTime.offset == 3600);
    REQUIRE(prDateTimeParse("2000-02-29T12:00:00Z", PrDateTimeFormat_Iso8601, &dateTime));
    CHECK(dateTime.offset == 0);
    REQUIRE(prDateTimeParse("+012345-01-02T03:04:05-01:00", PrDateTimeFormat_Iso8601, &dateTime));
    CHECK(dateTime.year == 12345);
    CHECK(dateTime.offset == -3600);
    REQUIRE(prDateTimeParse("23:59:60", PrDateTimeFormat_Time, &dateTime));
    CHECK(dateTime.second == 60);

    // Text that is not a date and time in the format leaves the date and time unchanged.
    const char* const kNotDates[] = {
        "2026-02-30", "2026-13-01", "2026-1-15", "26-01-15", "2026-01-15 ", " 2026-01-15", "",
    };
    PrDateTime unchanged = dateTime;
    for (const char* text : kNotDates)
        CHECK_FALSE(prDateTimeParse(PrStringView(text), PrDateTimeFormat_Date, &dateTime));
    CHECK_FALSE(prDateTimeParse("24:00:00", PrDateTimeFormat_Time, &dateTime));
    CHECK_FALSE(prDateTimeParse("2026-01-15T23:30:05", PrDateTimeFormat_Iso8601, &dateTime));
    CHECK_FALSE(prDateTimeParse("2026-01-15T23:30:05+24:00", PrDateTimeFormat_Iso8601, &dateTime));
    CHECK_FALSE(prDateTimeParse("2026-01-15T23:30:05.Z", PrDateTimeFormat_Iso8601, &dateTime));
    CHECK_FALSE(prDateTimeParse("Thu, 15 Jan 2026 23:30:05", PrDateTimeFormat_Rfc5322, &dateTime));
    CHECK_FALSE(prDateTimeParse("Thu, 15 Foo 2026 23:30:05 GMT", PrDateTimeFormat_Rfc5322, &dateTime));
    CHECK_FALSE(prDateTimeParse("Thu, 31 Feb 2026 23:30:05 GMT", PrDateTimeFormat_Http, &dateTime));
    CHECK_FALSE(prDateTimeParse("Thu, 15 Jan 2026 GMT", PrDateTimeFormat_Http, &dateTime));
    CHECK_FALSE(prDateTimeParse("-1", PrDateTimeFormat_Http, &dateTime));
    CHECK(prDateTimeToSystem(dateTime).ticks == prDateTimeToSystem(unchanged).ticks);

    // What each format writes, it reads back as the same time.
    for (int64_t seconds = -4000000000; seconds < 8000000000; seconds += 98765431) {
        PrDateTime written = prDateTimeFromSystem(prTimeSystemFromUnix(seconds, 0), int32_t(seconds % 50000 / 60 * 60));
        const PrDateTimeFormat kFormats[] = {PrDateTimeFormat_Iso8601, PrDateTimeFormat_Rfc5322, PrDateTimeFormat_Http};
        for (PrDateTimeFormat format : kFormats)
            CHECK(parsedTicks(formatted(written, format), format) == prDateTimeToSystem(written).ticks);
    }
}
