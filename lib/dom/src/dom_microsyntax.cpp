// Common microsyntaxes: numbers, dates and times
//
// html - https://html.spec.whatwg.org/multipage/common-microsyntaxes.html

#include "dom/src/dom_microsyntax.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/integer.h"

#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Numbers

// Returns the number of ASCII digits at `at` in `text`.
size_t digitsAt(PrStringView text, size_t at) {
    size_t end = at;
    while (end < text.size && prAsciiIsDigit(text.data[end]))
        ++end;
    return end - at;
}

// ---------------------------------------------------------------------------------------------------------------------
// Dates and times

// The most digits of a year that the parsers accept, which keeps every date's days and milliseconds exact.
constexpr size_t kMaxYearDigits = 12;

// Returns the day of the week of the day `days` after 1970-01-01, with Monday as 0.
uint32_t isoWeekday(int64_t days) {
    return (uint32_t(prDateTimeWeekdayFromDays(days)) + 6) % 7;
}

// Reads exactly `count` digits at `*at` and advances past them.
bool readDigits(PrStringView text, size_t* at, size_t count, uint32_t* out) {
    if (*at + count > text.size || digitsAt(text, *at) < count)
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = value * 10 + uint32_t(text.data[*at + i] - '0');
    *at += count;
    *out = value;
    return true;
}

bool readChar(PrStringView text, size_t* at, char c) {
    if (*at >= text.size || text.data[*at] != c)
        return false;
    ++*at;
    return true;
}

// html/#parse-a-month-component: four or more digits of a year above zero, a hyphen, and two digits of a month.
bool readMonth(PrStringView text, size_t* at, int64_t* year, uint32_t* month) {
    size_t digits = digitsAt(text, *at);
    if (digits < 4 || digits > kMaxYearDigits)
        return false;
    uint64_t value = 0;
    prU64ParsePrefix(text.data + *at, digits, &value, 10);
    *at += digits;
    if (value == 0 || !readChar(text, at, '-') || !readDigits(text, at, 2, month) || *month < 1 || *month > 12)
        return false;
    *year = int64_t(value);
    return true;
}

// html/#parse-a-date-component
bool readDate(PrStringView text, size_t* at, PrDateTime* out) {
    int64_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
    if (!readMonth(text, at, &year, &month) || !readChar(text, at, '-') || !readDigits(text, at, 2, &day) || day < 1 ||
            day > prDateTimeDaysInMonth(year, month))
        return false;
    *out = PrDateTime{.year = year, .month = uint8_t(month), .day = uint8_t(day)};
    return true;
}

// html/#parse-a-time-component: hours and minutes, then optional seconds with an optional fraction of any length.
bool readTime(PrStringView text, size_t* at, DomTime* out, uint32_t* fractionDigits) {
    uint32_t hour = 0;
    uint32_t minute = 0;
    if (!readDigits(text, at, 2, &hour) || hour > 23 || !readChar(text, at, ':') || !readDigits(text, at, 2, &minute) ||
            minute > 59)
        return false;
    double milliseconds = double(hour * 3600000 + minute * 60000);
    *fractionDigits = 0;
    if (readChar(text, at, ':')) {
        uint32_t second = 0;
        if (!readDigits(text, at, 2, &second) || second > 59)
            return false;
        milliseconds += second * 1000;
        if (readChar(text, at, '.')) {
            size_t digits = digitsAt(text, *at);
            if (digits == 0)
                return false;
            // The fraction keeps its first three digits, a whole number of milliseconds.
            double scale = 100;
            for (size_t i = 0; i < digits && i < 3; ++i, scale /= 10)
                milliseconds += (text.data[*at + i] - '0') * scale;
            *at += digits;
            *fractionDigits = uint32_t(digits);
        }
    }
    *out = milliseconds;
    return true;
}

// Appends `value` in decimal, padded with zeros to `width` digits.
void appendPadded(PrTextBuffer* out, uint64_t value, uint32_t width) {
    uint32_t digits = 1;
    for (uint64_t rest = value / 10; rest != 0; rest /= 10)
        ++digits;
    for (; digits < width; ++digits)
        prTextBufferAppendChar(out, '0');
    prTextBufferAppendU64(out, value, 10);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Numbers

bool domParseInteger(PrStringView text, int64_t* out) {
    size_t at = 0;
    while (at < text.size && prAsciiIsSpace(text.data[at]))
        ++at;
    bool negative = false;
    if (at < text.size && (text.data[at] == '-' || text.data[at] == '+'))
        negative = text.data[at++] == '-';
    uint64_t magnitude = 0;
    if (prU64ParsePrefix(text.data + at, text.size - at, &magnitude, 10) == 0)
        return false;
    uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
    magnitude = magnitude < limit ? magnitude : limit;
    *out = negative ? int64_t(0 - magnitude) : int64_t(magnitude);
    return true;
}

bool domParseNonNegativeInteger(PrStringView text, uint64_t* out) {
    int64_t value = 0;
    if (!domParseInteger(text, &value) || value < 0)
        return false;
    *out = uint64_t(value);
    return true;
}

// The rules read a sign, digits, a fraction and an exponent, each of which may end the number early. The bytes they
// read are a decimal that prF64Parse rounds correctly, as the rules' conversion step asks.
bool domParseFloat(PrStringView text, double* out) {
    size_t at = 0;
    while (at < text.size && prAsciiIsSpace(text.data[at]))
        ++at;
    if (at == text.size)
        return false;
    size_t start = at;
    if (text.data[at] == '-' || text.data[at] == '+') {
        ++at;
        if (at == text.size)
            return false;
    }
    // A leading '+' is skipped, and prF64Parse reads only a '-'.
    if (text.data[start] == '+')
        start = at;
    size_t whole = digitsAt(text, at);
    bool fraction = at + 1 < text.size && text.data[at] == '.' && prAsciiIsDigit(text.data[at + 1]);
    if (whole == 0 && !fraction)
        return false;
    at += whole;
    // A point without digits after it ends the number, and an exponent after it is not read.
    if (at + 1 < text.size && text.data[at] == '.' && prAsciiIsDigit(text.data[at + 1]))
        at += 1 + digitsAt(text, at + 1);
    size_t end = at;
    if (at < text.size && (text.data[at] == 'e' || text.data[at] == 'E')) {
        size_t exponent = at + 1;
        if (exponent < text.size && (text.data[exponent] == '-' || text.data[exponent] == '+'))
            ++exponent;
        size_t digits = digitsAt(text, exponent);
        if (digits != 0)
            end = exponent + digits;
    }
    // prF64Parse needs a digit before a fraction, so a number such as ".5" reads as "0.5".
    PrStringView number{text.data + start, end - start};
    double value = 0;
    if (whole == 0) {
        bool negative = number.data[0] == '-';
        PrTextBuffer prefixed = {};
        prTextBufferAppend(&prefixed, negative ? "-0" : "0");
        prTextBufferAppend(&prefixed, PrStringView{number.data + (negative ? 1 : 0), number.size - (negative ? 1 : 0)});
        PrStringView view = prTextBufferView(&prefixed);
        value = prF64Parse(view.data, view.size);
        prTextBufferReset(&prefixed);
    } else {
        value = prF64Parse(number.data, number.size);
    }
    if (!prF64IsFinite(value))
        return false;
    *out = value == 0 ? 0 : value;
    return true;
}

bool domIsValidFloat(PrStringView text) {
    size_t at = 0;
    if (at < text.size && text.data[at] == '-')
        ++at;
    size_t whole = digitsAt(text, at);
    at += whole;
    size_t fraction = 0;
    if (at < text.size && text.data[at] == '.') {
        fraction = digitsAt(text, at + 1);
        if (fraction == 0)
            return false;
        at += 1 + fraction;
    }
    if (whole == 0 && fraction == 0)
        return false;
    if (at < text.size && (text.data[at] == 'e' || text.data[at] == 'E')) {
        ++at;
        if (at < text.size && (text.data[at] == '-' || text.data[at] == '+'))
            ++at;
        size_t exponent = digitsAt(text, at);
        if (exponent == 0)
            return false;
        at += exponent;
    }
    return at == text.size;
}

void domAppendFloat(PrTextBuffer* out, double value) {
    PR_ASSERT(prF64IsFinite(value));
    char buffer[kPrF64TextMaxBytes];
    prTextBufferAppend(out, PrStringView{buffer, uint32_t(prF64Format(value, buffer))});
}

// ---------------------------------------------------------------------------------------------------------------------
// Dates and times

bool domParseMonth(PrStringView text, int64_t* year, uint32_t* month) {
    size_t at = 0;
    int64_t readYear = 0;
    uint32_t readMonthOfYear = 0;
    if (!readMonth(text, &at, &readYear, &readMonthOfYear) || at != text.size)
        return false;
    *year = readYear;
    *month = readMonthOfYear;
    return true;
}

bool domParseDate(PrStringView text, PrDateTime* out) {
    size_t at = 0;
    return readDate(text, &at, out) && at == text.size;
}

bool domParseWeek(PrStringView text, int64_t* year, uint32_t* week) {
    size_t at = 0;
    size_t digits = digitsAt(text, at);
    if (digits < 4 || digits > kMaxYearDigits)
        return false;
    uint64_t value = 0;
    prU64ParsePrefix(text.data, digits, &value, 10);
    at = digits;
    uint32_t number = 0;
    if (value == 0 || !readChar(text, &at, '-') || !readChar(text, &at, 'W') || !readDigits(text, &at, 2, &number) ||
            at != text.size || number < 1 || number > domWeeksInYear(int64_t(value)))
        return false;
    *year = int64_t(value);
    *week = number;
    return true;
}

bool domParseTime(PrStringView text, DomTime* out, uint32_t* PR_NULLABLE fractionDigits) {
    size_t at = 0;
    uint32_t digits = 0;
    if (!readTime(text, &at, out, &digits) || at != text.size)
        return false;
    if (fractionDigits != nullptr)
        *fractionDigits = digits;
    return true;
}

bool domParseLocalDateTime(PrStringView text, PrDateTime* date, DomTime* time, uint32_t* PR_NULLABLE fractionDigits) {
    size_t at = 0;
    uint32_t digits = 0;
    if (!readDate(text, &at, date) || at >= text.size || (text.data[at] != 'T' && text.data[at] != ' '))
        return false;
    ++at;
    if (!readTime(text, &at, time, &digits) || at != text.size)
        return false;
    if (fractionDigits != nullptr)
        *fractionDigits = digits;
    return true;
}

bool domIsValidTime(PrStringView text) {
    DomTime time = 0;
    uint32_t digits = 0;
    return domParseTime(text, &time, &digits) && digits <= 3;
}

bool domIsValidLocalDateTime(PrStringView text) {
    PrDateTime date{};
    DomTime time = 0;
    uint32_t digits = 0;
    return domParseLocalDateTime(text, &date, &time, &digits) && digits <= 3;
}

// A year has 53 weeks if it starts on a Thursday, or if it is a leap year that starts on a Wednesday.
uint32_t domWeeksInYear(int64_t year) {
    uint32_t first = isoWeekday(prDateTimeToDays(PrDateTime{.year = year, .month = 1, .day = 1}));
    return first == 3 || (first == 2 && prDateTimeIsLeapYear(year)) ? 53 : 52;
}

// Week 1 is the week that holds the year's first Thursday, and so its January 4.
int64_t domWeekDays(int64_t year, uint32_t week) {
    int64_t fourth = prDateTimeToDays(PrDateTime{.year = year, .month = 1, .day = 4});
    return fourth - isoWeekday(fourth) + int64_t(week - 1) * 7;
}

void domWeekOfDays(int64_t days, int64_t* year, uint32_t* week) {
    int64_t at = prDateTimeFromDays(days).year;
    if (days >= domWeekDays(at + 1, 1))
        ++at;
    else if (days < domWeekDays(at, 1))
        --at;
    *year = at;
    *week = uint32_t((days - domWeekDays(at, 1)) / 7 + 1);
}

void domAppendMonth(PrTextBuffer* out, int64_t year, uint32_t month) {
    appendPadded(out, uint64_t(year), 4);
    prTextBufferAppendChar(out, '-');
    appendPadded(out, month, 2);
}

void domAppendDate(PrTextBuffer* out, PrDateTime date) {
    domAppendMonth(out, date.year, date.month);
    prTextBufferAppendChar(out, '-');
    appendPadded(out, date.day, 2);
}

void domAppendWeek(PrTextBuffer* out, int64_t year, uint32_t week) {
    appendPadded(out, uint64_t(year), 4);
    prTextBufferAppend(out, "-W");
    appendPadded(out, week, 2);
}

void domAppendTime(PrTextBuffer* out, DomTime time) {
    uint64_t milliseconds = uint64_t(time);
    appendPadded(out, milliseconds / 3600000, 2);
    prTextBufferAppendChar(out, ':');
    appendPadded(out, milliseconds / 60000 % 60, 2);
    uint64_t second = milliseconds / 1000 % 60;
    uint64_t fraction = milliseconds % 1000;
    if (second == 0 && fraction == 0)
        return;
    prTextBufferAppendChar(out, ':');
    appendPadded(out, second, 2);
    if (fraction == 0)
        return;
    prTextBufferAppendChar(out, '.');
    appendPadded(out, fraction, 3);
}

void domAppendLocalDateTime(PrTextBuffer* out, PrDateTime date, DomTime time) {
    domAppendDate(out, date);
    prTextBufferAppendChar(out, 'T');
    domAppendTime(out, time);
}

uint32_t domReflectedUnsigned(const PrDomElement* element, PrStringView name, uint32_t fallback) {
    int64_t value = 0;
    if (!domParseInteger(domAttributeValue(element, name), &value) || value < 0 || value > 2147483647)
        return fallback;
    return uint32_t(value);
}

bool domParseDimension(PrStringView text, double* out, bool* percentage) {
    size_t at = 0;
    while (at < text.size && prAsciiIsSpace(text.data[at]))
        ++at;
    uint64_t whole = 0;
    size_t digits = prU64ParsePrefix(text.data + at, text.size - at, &whole, 10);
    if (digits == 0)
        return false;
    at += digits;
    double value = double(whole);
    if (at + 1 < text.size && text.data[at] == '.' && prAsciiIsDigit(text.data[at + 1])) {
        double scale = 0.1;
        for (++at; at < text.size && prAsciiIsDigit(text.data[at]); ++at, scale /= 10)
            value += (text.data[at] - '0') * scale;
    }
    *out = value;
    *percentage = at < text.size && text.data[at] == '%';
    return true;
}

} // namespace pr_impl
