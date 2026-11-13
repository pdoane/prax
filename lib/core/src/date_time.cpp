// Dates and times of the proleptic Gregorian calendar.

#include "pr/date_time.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// The days of an era of the Gregorian calendar, which repeats every 400 years.
constexpr int64_t kEraDays = 146097;

// The day of 0000-03-01, which starts the first era of the count below.
constexpr int64_t kMarchFirstOfYearZero = -719468;

// The day of 1600-03-01, the epoch of the system time, four eras later.
constexpr int64_t kSystemEpochDay = kMarchFirstOfYearZero + 4 * kEraDays;

constexpr int64_t kDaySeconds = 86400;
constexpr uint64_t kTicksPerSecond = kPrTimeSystemTicksPerSecond;

static_assert(kPrTimeSystemUnixEpoch == uint64_t(-kSystemEpochDay * kDaySeconds) * kTicksPerSecond);

constexpr const char* kWeekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kWeekdayLongNames[] = {
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
};
constexpr const char* kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int64_t floorDiv(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

int64_t floorMod(int64_t value, int64_t divisor) {
    int64_t remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

// Sets the date of `out` to the day `dayOfEra` of the era that starts on March 1 of `eraYear`. The era's years count
// from March, so that a leap day ends its year (H. Hinnant, "chrono-Compatible Low-Level Date Algorithms").
void setDateOfEra(int64_t eraYear, uint32_t dayOfEra, PrDateTime* out) {
    uint32_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    uint32_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    uint32_t monthFromMarch = (5 * dayOfYear + 2) / 153;
    out->day = uint8_t(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    out->month = uint8_t(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);
    out->year = eraYear + yearOfEra + (out->month <= 2 ? 1 : 0);
}

// Sets the date and weekday of `out` to the day `days`.
void setDate(int64_t days, PrDateTime* out) {
    int64_t shifted = days - kMarchFirstOfYearZero;
    int64_t era = floorDiv(shifted, kEraDays);
    setDateOfEra(era * 400, uint32_t(shifted - era * kEraDays), out);
    out->weekday = prDateTimeWeekdayFromDays(days);
}

// Sets the time of day of `out` to the second `seconds` of a day.
void setTimeOfDay(uint32_t seconds, PrDateTime* out) {
    out->hour = uint8_t(seconds / 3600);
    out->minute = uint8_t(seconds / 60 % 60);
    out->second = uint8_t(seconds % 60);
}

// Returns the seconds of `dateTime` since the Unix epoch, in UTC.
int64_t unixSeconds(PrDateTime dateTime) {
    return prDateTimeToDays(dateTime) * kDaySeconds + int64_t(dateTime.hour) * 3600 + int64_t(dateTime.minute) * 60 +
        int64_t(dateTime.second) - dateTime.offset;
}

// Returns `dateTime` as the same moment in UTC.
PrDateTime toUtc(PrDateTime dateTime) {
    int64_t seconds = unixSeconds(dateTime);
    PrDateTime utc = prDateTimeFromDays(floorDiv(seconds, kDaySeconds));
    setTimeOfDay(uint32_t(floorMod(seconds, kDaySeconds)), &utc);
    utc.nanosecond = dateTime.nanosecond;
    return utc;
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing

void appendPadded(PrTextBuffer* out, uint64_t value, uint32_t width) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (uint32_t i = count; i < width; ++i)
        prTextBufferAppendChar(out, '0');
    while (count != 0)
        prTextBufferAppendChar(out, digits[--count]);
}

// Appends the year's decimal digits, at least `width` of them, after a minus sign if it is negative.
void appendYear(PrTextBuffer* out, int64_t year, uint32_t width) {
    if (year < 0)
        prTextBufferAppendChar(out, '-');
    appendPadded(out, year < 0 ? 0 - uint64_t(year) : uint64_t(year), width);
}

void appendDate(PrTextBuffer* out, PrDateTime dateTime) {
    if (dateTime.year >= 0 && dateTime.year <= 9999) {
        appendPadded(out, uint64_t(dateTime.year), 4);
    } else {
        if (dateTime.year > 0)
            prTextBufferAppendChar(out, '+');
        appendYear(out, dateTime.year, 6);
    }
    prTextBufferAppendChar(out, '-');
    appendPadded(out, dateTime.month, 2);
    prTextBufferAppendChar(out, '-');
    appendPadded(out, dateTime.day, 2);
}

void appendTime(PrTextBuffer* out, PrDateTime dateTime) {
    appendPadded(out, dateTime.hour, 2);
    prTextBufferAppendChar(out, ':');
    appendPadded(out, dateTime.minute, 2);
    prTextBufferAppendChar(out, ':');
    appendPadded(out, dateTime.second, 2);
}

// Appends the offset as a sign and its hours and minutes, with `separator` between them unless it is zero.
void appendOffset(PrTextBuffer* out, int32_t offset, char separator) {
    prTextBufferAppendChar(out, offset < 0 ? '-' : '+');
    uint32_t minutes = uint32_t(offset < 0 ? -int64_t(offset) : int64_t(offset)) / 60;
    appendPadded(out, minutes / 60, 2);
    if (separator != 0)
        prTextBufferAppendChar(out, separator);
    appendPadded(out, minutes % 60, 2);
}

// Appends the weekday, day, month and year, as in "Thu, 15 Jan 2026".
void appendMailDate(PrTextBuffer* out, PrDateTime dateTime) {
    prTextBufferAppend(out, PrStringView(kWeekdayNames[prDateTimeWeekdayFromDays(prDateTimeToDays(dateTime))]));
    prTextBufferAppend(out, PR_SV(", "));
    appendPadded(out, dateTime.day, 2);
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppend(out, PrStringView(kMonthNames[dateTime.month - 1]));
    prTextBufferAppendChar(out, ' ');
    appendYear(out, dateTime.year, 4);
    prTextBufferAppendChar(out, ' ');
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading

// A position in the text that a parse reads.
struct Reader {
    PrStringView text;
    size_t at = 0;

    bool done() const { return at == text.size; }

    bool take(char c) {
        if (at == text.size || text.data[at] != c)
            return false;
        ++at;
        return true;
    }

    bool takeFold(char c) {
        if (at == text.size || prAsciiLower(text.data[at]) != c)
            return false;
        ++at;
        return true;
    }

    // Reads from `least` through `most` digits, as many as there are.
    bool digits(uint32_t least, uint32_t most, uint64_t* out, uint32_t* PR_NULLABLE count = nullptr) {
        uint64_t value = 0;
        uint32_t taken = 0;
        while (taken < most && at < text.size && prAsciiIsDigit(text.data[at])) {
            value = value * 10 + uint64_t(text.data[at] - '0');
            ++at;
            ++taken;
        }
        if (taken < least)
            return false;
        *out = value;
        if (count != nullptr)
            *count = taken;
        return true;
    }

    bool exactly(uint32_t count, uint64_t* out) { return digits(count, count, out); }

    // Skips spaces and tabs. Returns true if there was one.
    bool spaces() {
        size_t start = at;
        while (at < text.size && prAsciiIsTabOrSpace(text.data[at]))
            ++at;
        return at != start;
    }

    // Reads a run of ASCII letters.
    PrStringView word() {
        size_t start = at;
        while (at < text.size && prAsciiIsLetter(text.data[at]))
            ++at;
        return prStringSlice(text, start, at - start);
    }
};

// Returns the month 1 through 12 that the three letters of `name` abbreviate, ignoring case, or zero.
uint8_t monthOfName(PrStringView name) {
    for (uint8_t i = 0; i < 12; ++i) {
        if (prAsciiEqualsFold(name, PrStringView(kMonthNames[i])))
            return uint8_t(i + 1);
    }
    return 0;
}

bool isWeekdayName(PrStringView name) {
    for (uint32_t i = 0; i < 7; ++i) {
        if (prAsciiEqualsFold(name, PrStringView(kWeekdayNames[i])) ||
                prAsciiEqualsFold(name, PrStringView(kWeekdayLongNames[i])))
            return true;
    }
    return false;
}

// The fields that a parse reads, before it checks their ranges and writes them.
struct Fields {
    bool hasDate = false;
    bool hasTime = false;
    bool hasOffset = false;
    int64_t year = 0;
    uint64_t month = 0;
    uint64_t day = 0;
    uint64_t hour = 0;
    uint64_t minute = 0;
    uint64_t second = 0;
    uint32_t nanosecond = 0;
    int32_t offset = 0;
};

// Returns true if the fields are in range, and writes them to `out`.
bool store(const Fields& fields, PrDateTime* out) {
    if (fields.hasDate && (fields.month < 1 || fields.month > 12 || fields.day < 1 ||
            fields.day > prDateTimeDaysInMonth(fields.year, uint32_t(fields.month))))
        return false;
    if (fields.hasTime && (fields.hour > 23 || fields.minute > 59 || fields.second > 60))
        return false;
    if (fields.hasDate) {
        out->year = fields.year;
        out->month = uint8_t(fields.month);
        out->day = uint8_t(fields.day);
        out->weekday = prDateTimeWeekdayFromDays(prDateTimeToDays(*out));
    }
    if (fields.hasTime) {
        out->hour = uint8_t(fields.hour);
        out->minute = uint8_t(fields.minute);
        out->second = uint8_t(fields.second);
        out->nanosecond = fields.nanosecond;
    }
    if (fields.hasOffset)
        out->offset = fields.offset;
    return true;
}

// Reads "YYYY-MM-DD", or with `expanded` ISO 8601's signed years of six or more digits as well.
bool readDate(Reader& r, bool expanded, Fields* out) {
    bool negative = false;
    uint64_t year = 0;
    if (expanded && (r.take('+') || (negative = r.take('-')))) {
        // Eighteen digits keep the year in range.
        if (!r.digits(6, 18, &year))
            return false;
    } else if (!r.exactly(4, &year)) {
        return false;
    }
    out->hasDate = true;
    out->year = negative ? -int64_t(year) : int64_t(year);
    return r.take('-') && r.exactly(2, &out->month) && r.take('-') && r.exactly(2, &out->day);
}

// Reads "HH:MM:SS", or with `fraction` a fraction of a second after it as well.
bool readTime(Reader& r, bool fraction, Fields* out) {
    out->hasTime = true;
    if (!r.exactly(2, &out->hour) || !r.take(':') || !r.exactly(2, &out->minute) || !r.take(':') ||
            !r.exactly(2, &out->second))
        return false;
    if (!fraction || !r.take('.'))
        return true;
    // The digits past nanoseconds are read and dropped.
    uint64_t value = 0;
    uint32_t count = 0;
    if (!r.digits(1, 9, &value, &count))
        return false;
    for (uint32_t i = count; i < 9; ++i)
        value *= 10;
    out->nanosecond = uint32_t(value);
    uint64_t rest = 0;
    r.digits(0, UINT32_MAX, &rest);
    return true;
}

// Reads a sign, two digits of hours, an optional `separator` and two digits of minutes.
bool readOffset(Reader& r, char separator, Fields* out) {
    bool negative = r.take('-');
    if (!negative && !r.take('+'))
        return false;
    uint64_t hours = 0;
    uint64_t minutes = 0;
    if (!r.exactly(2, &hours))
        return false;
    if (separator != 0)
        r.take(separator);
    if (!r.exactly(2, &minutes) || hours > 23 || minutes > 59)
        return false;
    out->hasOffset = true;
    out->offset = int32_t(hours * 3600 + minutes * 60) * (negative ? -1 : 1);
    return true;
}

bool parseIso8601(Reader& r, Fields* out) {
    if (!readDate(r, true, out) || !r.takeFold('t') || !readTime(r, true, out))
        return false;
    if (r.takeFold('z')) {
        out->hasOffset = true;
        return true;
    }
    return readOffset(r, ':', out);
}

// Reads the zone of RFC 5322: an offset of four digits, or a name that it makes obsolete.
bool readMailZone(Reader& r, Fields* out) {
    if (r.at < r.text.size && (r.text.data[r.at] == '+' || r.text.data[r.at] == '-'))
        return readOffset(r, 0, out);
    static const char* const kZoneNames[] = {"UT", "GMT", "EST", "EDT", "CST", "CDT", "MST", "MDT", "PST", "PDT"};
    static const int32_t kZoneHours[] = {0, 0, -5, -4, -6, -5, -7, -6, -8, -7};
    PrStringView name = r.word();
    for (size_t i = 0; i < countOf(kZoneHours); ++i) {
        if (prAsciiEqualsFold(name, PrStringView(kZoneNames[i]))) {
            out->hasOffset = true;
            out->offset = kZoneHours[i] * 3600;
            return true;
        }
    }
    return false;
}

// RFC 5322 section 3.3, with the obsolete years of section 4.3.
bool parseRfc5322(Reader& r, Fields* out) {
    r.spaces();
    size_t start = r.at;
    PrStringView weekday = r.word();
    if (weekday.size != 0) {
        r.spaces();
        if (!isWeekdayName(weekday) || !r.take(','))
            return false;
    } else {
        r.at = start;
    }
    r.spaces();
    uint64_t year = 0;
    uint32_t yearDigits = 0;
    out->hasDate = true;
    if (!r.digits(1, 2, &out->day) || !r.spaces() || (out->month = monthOfName(r.word())) == 0 || !r.spaces() ||
            !r.digits(2, 18, &year, &yearDigits) || !r.spaces())
        return false;
    // A two-digit year below 50 is in the 2000s, and any other of two or three digits counts from 1900.
    out->year = int64_t(yearDigits == 2 && year < 50 ? 2000 + year : yearDigits <= 3 ? 1900 + year : year);
    out->hasTime = true;
    if (!r.exactly(2, &out->hour) || !r.take(':') || !r.exactly(2, &out->minute))
        return false;
    if (r.take(':') && !r.exactly(2, &out->second))
        return false;
    if (!r.spaces() || !readMailZone(r, out))
        return false;
    r.spaces();
    return true;
}

// RFC 9110 section 5.6.7: an IMF-fixdate, or the obsolete forms of RFC 850 and asctime, read token by token so that the
// dates that servers write loosely still read. The tokens are between spaces, commas and hyphens: the day, the month's
// name, the year and the time are read, and any other word, such as the weekday or the zone, is skipped.
bool parseHttp(Reader& r, Fields* out) {
    int64_t day = -1;
    int64_t month = -1;
    int64_t year = -1;
    PrStringView text = r.text;
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && (text.data[at] == ' ' || text.data[at] == ',' || text.data[at] == '-'))
            ++at;
        size_t end = at;
        while (end < text.size && text.data[end] != ' ' && text.data[end] != ',' && text.data[end] != '-')
            ++end;
        PrStringView token = prStringSlice(text, at, end - at);
        at = end;
        if (token.size == 0)
            continue;
        if (token.size == 8 && token.data[2] == ':' && token.data[5] == ':') {
            Reader time{token};
            if (out->hasTime || !readTime(time, false, out))
                return false;
        } else if (prAsciiIsDigit(token.data[0])) {
            Reader number{token};
            uint64_t value = 0;
            if (!number.digits(1, 18, &value) || !number.done())
                return false;
            if (day < 0 && token.size <= 2)
                day = int64_t(value);
            else if (year < 0)
                year = token.size <= 2 ? int64_t(value < 70 ? 2000 + value : 1900 + value) : int64_t(value);
            else
                return false;
        } else if (uint8_t named = monthOfName(token); named != 0) {
            month = named;
        }
    }
    r.at = text.size;
    if (day < 0 || month < 0 || year < 0 || !out->hasTime)
        return false;
    out->hasDate = true;
    out->hasOffset = true;
    out->year = year;
    out->month = uint64_t(month);
    out->day = uint64_t(day);
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prDateTimeIsLeapYear(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint32_t prDateTimeDaysInMonth(int64_t year, uint32_t month) {
    PR_ASSERT(month >= 1 && month <= 12);
    constexpr uint32_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && prDateTimeIsLeapYear(year) ? 29 : kDays[month - 1];
}

PrDateTimeWeekday prDateTimeWeekdayFromDays(int64_t days) {
    // 1970-01-01 was a Thursday.
    return PrDateTimeWeekday(floorMod(days + PrDateTimeWeekday_Thursday, 7));
}

PrDateTime prDateTimeFromDays(int64_t days) {
    PrDateTime dateTime{};
    setDate(days, &dateTime);
    return dateTime;
}

// The conversions count days from 0000-03-01 in eras of 400 years.
int64_t prDateTimeToDays(PrDateTime dateTime) {
    PR_ASSERT(dateTime.month >= 1 && dateTime.month <= 12 && dateTime.day >= 1 && dateTime.day <= 31);
    int64_t year = dateTime.year - (dateTime.month <= 2 ? 1 : 0);
    int64_t era = floorDiv(year, 400);
    int64_t yearOfEra = year - era * 400;
    int64_t dayOfYear = (153 * (dateTime.month > 2 ? dateTime.month - 3 : dateTime.month + 9) + 2) / 5 + dateTime.day - 1;
    int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * kEraDays + dayOfEra + kMarchFirstOfYearZero;
}

// The system time counts from the start of an era, so that a time it shows on or after the epoch is a day of an era
// with unsigned divisions alone.
PrDateTime prDateTimeFromSystem(PrTimeSystem time, int32_t offset) {
    int64_t seconds = int64_t(time.ticks / kTicksPerSecond) + offset;
    PrDateTime dateTime{};
    if (seconds >= 0) {
        uint64_t days = uint64_t(seconds) / kDaySeconds;
        setDateOfEra(1600 + int64_t(days / kEraDays) * 400, uint32_t(days % kEraDays), &dateTime);
        dateTime.weekday = PrDateTimeWeekday((days + PrDateTimeWeekday_Wednesday) % 7);  // 1600-03-01 was a Wednesday
        setTimeOfDay(uint32_t(uint64_t(seconds) % kDaySeconds), &dateTime);
    } else {
        setDate(floorDiv(seconds, kDaySeconds) + kSystemEpochDay, &dateTime);
        setTimeOfDay(uint32_t(floorMod(seconds, kDaySeconds)), &dateTime);
    }
    dateTime.nanosecond = uint32_t(time.ticks % kTicksPerSecond * 100);
    dateTime.offset = offset;
    return dateTime;
}

PrTimeSystem prDateTimeToSystem(PrDateTime dateTime) {
    PR_ASSERT(dateTime.hour < 24 && dateTime.minute < 60 && dateTime.second <= 60 && dateTime.nanosecond < 1000000000);
    int64_t seconds = unixSeconds(dateTime) - kSystemEpochDay * kDaySeconds;
    if (seconds < 0)
        return PrTimeSystem{0};
    if (uint64_t(seconds) >= UINT64_MAX / kTicksPerSecond)
        return PrTimeSystem{UINT64_MAX};
    return PrTimeSystem{uint64_t(seconds) * kTicksPerSecond + dateTime.nanosecond / 100};
}

void prDateTimeFormat(PrDateTime dateTime, PrDateTimeFormat format, PrTextBuffer* out) {
    switch (format) {
        case PrDateTimeFormat_Iso8601:
            appendDate(out, dateTime);
            prTextBufferAppendChar(out, 'T');
            appendTime(out, dateTime);
            if (dateTime.nanosecond != 0) {
                uint32_t fraction = dateTime.nanosecond;
                uint32_t width = 9;
                while (fraction % 10 == 0) {
                    fraction /= 10;
                    --width;
                }
                prTextBufferAppendChar(out, '.');
                appendPadded(out, fraction, width);
            }
            if (dateTime.offset == 0)
                prTextBufferAppendChar(out, 'Z');
            else
                appendOffset(out, dateTime.offset, ':');
            break;
        case PrDateTimeFormat_Rfc5322:
            appendMailDate(out, dateTime);
            appendTime(out, dateTime);
            prTextBufferAppendChar(out, ' ');
            appendOffset(out, dateTime.offset, 0);
            break;
        case PrDateTimeFormat_Http: {
            PrDateTime utc = dateTime.offset != 0 ? toUtc(dateTime) : dateTime;
            appendMailDate(out, utc);
            appendTime(out, utc);
            prTextBufferAppend(out, PR_SV(" GMT"));
            break;
        }
        case PrDateTimeFormat_Simple:
            appendDate(out, dateTime);
            prTextBufferAppendChar(out, ' ');
            appendTime(out, dateTime);
            break;
        case PrDateTimeFormat_Date:
            appendDate(out, dateTime);
            break;
        case PrDateTimeFormat_Time:
            appendTime(out, dateTime);
            break;
        default:
            PR_ASSERT(false);
    }
}

bool prDateTimeParse(PrStringView text, PrDateTimeFormat format, PrDateTime* out) {
    Reader r{text};
    Fields fields;
    bool read = false;
    switch (format) {
        case PrDateTimeFormat_Iso8601:
            read = parseIso8601(r, &fields);
            break;
        case PrDateTimeFormat_Rfc5322:
            read = parseRfc5322(r, &fields);
            break;
        case PrDateTimeFormat_Http:
            read = parseHttp(r, &fields);
            break;
        case PrDateTimeFormat_Simple:
            read = readDate(r, false, &fields) && r.take(' ') && readTime(r, false, &fields);
            break;
        case PrDateTimeFormat_Date:
            read = readDate(r, false, &fields);
            break;
        case PrDateTimeFormat_Time:
            read = readTime(r, false, &fields);
            break;
        default:
            PR_ASSERT(false);
    }
    return read && r.done() && store(fields, out);
}
