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
