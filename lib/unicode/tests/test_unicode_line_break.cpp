#include "pr/unicode_line_break.h"

#include "tools/harness/test.h"

TEST_CASE("a hard line break is mandatory, and a combining mark never breaks from its base") {
    const uint32_t text[] = {'a', ' ', 'b', '\n', 'c', 0x0301, ' ', 0x0301};
    PrUnicodeLineBreak breaks[8];
    prUnicodeLineBreaks(text, 8, breaks);
    CHECK(breaks[0] == PrUnicodeLineBreak_Prohibited);
    CHECK(breaks[1] == PrUnicodeLineBreak_Allowed);
    CHECK(breaks[3] == PrUnicodeLineBreak_Mandatory);
    CHECK(breaks[4] == PrUnicodeLineBreak_Prohibited);
    // A mark after a space has no base, and is treated as an alphabetic character on its own.
    CHECK(breaks[6] == PrUnicodeLineBreak_Allowed);
    CHECK(breaks[7] == PrUnicodeLineBreak_Mandatory);
}
