#include "pr/string.h"
#include "pr/unicode_idna.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the text of `text`, which it takes, and leaves `text` null.
String taken(PrRawString* text) {
    String out = String::adopt(*text);
    *text = PrRawString{};
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the URL Standard's flags accept what its host parser does") {
    PrRawString out{};
    CHECK(prUnicodeIdnaToAscii("Bu\xcc\x88" "cher.de", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(taken(&out) == "xn--bcher-kva.de");
    CHECK(prUnicodeIdnaToAscii("a_b.\xe2\x98\x95", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(taken(&out) == "a_b.xn--53h");
    CHECK(!prUnicodeIdnaToAscii("xn--a.\xc3\x9f", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(prRawStringIsNull(&out));
}
