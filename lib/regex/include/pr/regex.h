// Regular expressions in RE2's syntax (https://github.com/google/re2/wiki/Syntax), in the subset that has no
// backreferences, matched by running every thread of the pattern at once over the text. A match costs at most the
// product of the sizes of the pattern and the text, so no pattern can make a match slow.
//
// The syntax: literals, `.`, classes such as `[a-z]` and `[^0-9]`, alternation, groups `(...)` that record what they
// match and groups `(?:...)` that do not, the anchors `^` and `$`, the quantifiers `*`, `+`, `?`, `{n}`, `{n,}` and
// `{n,m}`, each followed by `?` to prefer fewer repetitions, the classes `\d`, `\D`, `\w`, `\W`, `\s` and `\S`, and the
// escapes `\n`, `\t`, `\r`, `\f`, `\v`, `\0` and of a punctuation character. `^` and `$` match at the ends of the text,
// and `.` matches any code point but a line feed. Patterns and texts are UTF-8, a match steps by code points, and
// offsets count bytes.
//
// A regex is immutable once made, so threads may match with it at once.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A compiled regular expression.
typedef struct PrRegex PrRegex;

// The bytes of the text that a group matched. Group 0 is the whole match, and group n is the nth group that records
// what it matches, counted by its `(`.
typedef struct PrRegexGroup {
    uint32_t start;
    uint32_t end;
} PrRegexGroup;

// The start and the end of a group that took no part in a match.
static const uint32_t kPrRegexGroupNone = 0xffffffffu;

// Compiles `pattern`, or frees a regex. Returns Syntax if the pattern does not parse, and Limit if its groups nest more
// than 256 deep, a repetition counts past 1000, or its program would pass a million steps. On failure, sets `*out` to
// null.
PrStatus prRegexCreate(PrStringView pattern, PrRegex* PR_NULLABLE* PR_NONNULL out);
void prRegexDestroy(PrRegex* PR_NULLABLE regex);

// The number of groups, with group 0.
uint32_t prRegexGroupCount(const PrRegex* regex);

// Returns true if the regex matches part of `text`, which is less than 4 GiB. Of the matches, the first to start wins,
// and of those, the one that the pattern prefers: alternatives in order, and quantifiers as many or as few as they ask.
// Writes the groups of the match to `out`, up to `capacity`, if it matched.
bool prRegexSearch(const PrRegex* regex, PrStringView text, PrRegexGroup* PR_NULLABLE out, uint32_t capacity);

// Returns true if the regex matches the whole of `text`, and writes its groups as prRegexSearch does.
bool prRegexMatch(const PrRegex* regex, PrStringView text, PrRegexGroup* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
