#include "pr/unicode_normalization.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

std::string normalize(const std::string& text, PrUnicodeNormalForm form) {
    PrRawString normalized = prUnicodeNormalize(text, form);
    PrStringView view = prRawStringView(&normalized);
    std::string out(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&normalized);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ill-formed UTF-8 normalizes to U+FFFD and is in no form") {
    CHECK(normalize("a\xff", PrUnicodeNormalForm_Nfc) == "a\xef\xbf\xbd");
    CHECK(!prUnicodeIsNormalized("a\xff", PrUnicodeNormalForm_Nfc));
    CHECK(prUnicodeIsNormalized("", PrUnicodeNormalForm_Nfd));
}
