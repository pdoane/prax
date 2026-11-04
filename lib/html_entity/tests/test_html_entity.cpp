#include "pr/html_entity.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

extern "C" int prTestEntityCApi(void);

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestEntityCApi() == 0);
}

TEST_CASE("a name expands to its characters") {
    CHECK(prHtmlEntityMatch("amp;") == "&");
    CHECK(prHtmlEntityMatch("copy;") == "\xc2\xa9");
    CHECK(prHtmlEntityMatch("NotEqualTilde;") == "\xe2\x89\x82\xcc\xb8");
    CHECK(prHtmlEntityMatch("CounterClockwiseContourIntegral;").size != 0);
}

TEST_CASE("a legacy name matches without its semicolon, and others need it") {
    CHECK(prHtmlEntityMatch("amp") == "&");
    CHECK(prHtmlEntityMatch("Aacute") == "\xc3\x81");
    CHECK(prHtmlEntityMatch("copy") == "\xc2\xa9");
    CHECK(prHtmlEntityMatch("Alpha").data == nullptr);
    CHECK(prHtmlEntityMatch("Alpha;") == "\xce\x91");
}

TEST_CASE("a lookup matches the exact spelling") {
    CHECK(prHtmlEntityMatch("AMP;") == "&");
    CHECK(prHtmlEntityMatch("Amp;").data == nullptr);
    CHECK(prHtmlEntityMatch("").data == nullptr);
    CHECK(prHtmlEntityMatch("&amp;").data == nullptr);
}
