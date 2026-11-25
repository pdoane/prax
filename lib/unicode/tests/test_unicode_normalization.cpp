#include "pr/string.h"
#include "pr/unicode_normalization.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

String normalize(PrStringView text, PrUnicodeNormalForm form) {
    return String::adopt(prUnicodeNormalize(text, form));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ill-formed UTF-8 normalizes to U+FFFD and is in no form") {
    CHECK(normalize("a\xff", PrUnicodeNormalForm_Nfc) == "a\xef\xbf\xbd");
    CHECK(!prUnicodeIsNormalized("a\xff", PrUnicodeNormalForm_Nfc));
    CHECK(prUnicodeIsNormalized("", PrUnicodeNormalForm_Nfd));
}

TEST_CASE("a lone surrogate in WTF-8 stays itself, and a split pair is joined") {
    CHECK(normalize("e\xCC\x81\xED\xA0\x80", PrUnicodeNormalForm_Nfc) == "\xC3\xA9\xED\xA0\x80");
    CHECK(normalize("\xED\xA0\xBD\xED\xB8\x80", PrUnicodeNormalForm_Nfd) == "\xF0\x9F\x98\x80");
    CHECK(prUnicodeIsNormalized("a\xED\xA0\x80", PrUnicodeNormalForm_Nfc));
    CHECK(!prUnicodeIsNormalized("\xED\xA0\xBD\xED\xB8\x80", PrUnicodeNormalForm_Nfc));
}
