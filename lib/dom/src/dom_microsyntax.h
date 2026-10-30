// Common microsyntaxes: numbers, dates and times
//
// html - https://html.spec.whatwg.org/multipage/common-microsyntaxes.html
//
// The parsers of the strict syntaxes, such as a valid date string, accept exactly the strings that the syntax defines.
// The rules for parsing a value, such as the rules for parsing integers, are lenient and accept a prefix.

#pragma once

#include "pr/dom.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/time_date.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Numbers

// Parses `text` as an integer and writes it to `out` (html/#rules-for-parsing-integers). Returns false if `text` has no
// digits after its leading whitespace and sign. A value beyond the range of an int64_t saturates.
bool domParseInteger(PrStringView text, int64_t* out);

// Parses `text` as a non-negative integer and writes it to `out` (html/#rules-for-parsing-non-negative-integers).
// Returns false if the rules return an error.
bool domParseNonNegativeInteger(PrStringView text, uint64_t* out);

// Parses `text` by the rules for parsing floating-point number values
// (html/#rules-for-parsing-floating-point-number-values) and writes the number to `out`. Returns false if the rules
// return an error, which they do for a number too large to be finite. Negative zero parses as zero.
bool domParseFloat(PrStringView text, double* out);

// Returns true if `text` is a valid floating-point number (html/#valid-floating-point-number).
bool domIsValidFloat(PrStringView text);

// Appends the best representation of `value` as a floating-point number
// (html/#best-representation-of-the-number-as-a-floating-point-number), which is the shortest decimal that parses back
// to it, as JavaScript's Number::toString writes it. `value` must be finite.
void domAppendFloat(TextBuffer* out, double value);

// ---------------------------------------------------------------------------------------------------------------------
// Dates and times (html/#dates-and-times)
//
// Dates are in the proleptic Gregorian calendar (pr/time_date.h), and a year is greater than zero. A month is a year
// and a month, and a week is a year and a week. A time is local, with no time zone.

// A time of day, in milliseconds since midnight, below 86400000.
typedef double DomTime;

// Parse the strings that html/#valid-month-string, html/#valid-date-string, html/#valid-week-string,
// html/#valid-time-string and html/#valid-local-date-and-time-string define. Each writes what the string represents and
// returns false for a string that is not one. A time's fraction of a second may have more digits than the valid
// syntax's three, as the parsing rules allow (html/#parse-a-time-component), and `fractionDigits` says how many it has.
bool domParseMonth(PrStringView text, int64_t* year, uint32_t* month);
bool domParseDate(PrStringView text, PrTimeDate* out);
bool domParseWeek(PrStringView text, int64_t* year, uint32_t* week);
bool domParseTime(PrStringView text, DomTime* out, uint32_t* PR_NULLABLE fractionDigits = nullptr);
bool domParseLocalDateTime(PrStringView text, PrTimeDate* date, DomTime* time, uint32_t* PR_NULLABLE fractionDigits = nullptr);

// Returns true if `text` is a valid time string, or a valid local date and time string: one that parses, with at most
// three digits of a fraction of a second.
bool domIsValidTime(PrStringView text);
bool domIsValidLocalDateTime(PrStringView text);

// Returns the number of weeks in `year`, 52 or 53 (html/#week-number-of-the-last-day).
uint32_t domWeeksInYear(int64_t year);

// Returns the number of days from 1970-01-01 to the Monday that starts week `week` of `year`.
int64_t domWeekDays(int64_t year, uint32_t week);

// Writes the year and the week of the week that holds the day `days` after 1970-01-01.
void domWeekOfDays(int64_t days, int64_t* year, uint32_t* week);

// Append a valid month string, date string, week string, time string, and normalized local date and time string
// (html/#valid-normalised-local-date-and-time-string). A time string leaves out its seconds if they and the
// milliseconds are zero, and the milliseconds if they are zero.
void domAppendMonth(TextBuffer* out, int64_t year, uint32_t month);
void domAppendDate(TextBuffer* out, PrTimeDate date);
void domAppendWeek(TextBuffer* out, int64_t year, uint32_t week);
void domAppendTime(TextBuffer* out, DomTime time);
void domAppendLocalDateTime(TextBuffer* out, PrTimeDate date, DomTime time);

// Returns the attribute `name` of `element` as an unsigned long IDL attribute that reflects it with the default
// `fallback` reads it (html/#reflecting-content-attributes-in-idl-attributes): a non-negative integer, or `fallback` if
// the attribute is missing, does not parse, is negative, or is past 2147483647.
uint32_t domReflectedUnsigned(const PrDomElement* element, PrStringView name, uint32_t fallback);

// Parses `text` by the rules for parsing dimension values (html/#rules-for-parsing-dimension-values). Returns false if
// it holds no number. Otherwise writes the number to `out`, and writes true to `percentage` if it is a percentage.
bool domParseDimension(PrStringView text, double* out, bool* percentage);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
