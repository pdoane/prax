#include "pr/url_data.h"

#include "tools/harness/test.h"

extern "C" bool prTestDataC(void);

namespace pr_impl {

namespace {

// The body and the MIME type of a data: URL, or "error" for a URL that does not decode.
String processed(PrStringView url) {
    PrRawString body{};
    PrRawString type{};
    PrStatus status = prUrlProcessData(url, &body, &type);
    String result;
    if (status != PrStatus_Ok)
        result = "error";
    else
        result = format("{}|{}", prRawStringView(&type), prRawStringView(&body));
    prRawStringReset(&body);
    prRawStringReset(&type);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a data: URL's bytes are percent-decoded, and base64-decoded if its type says so") {
    CHECK(processed("data:text/plain,a%20b") == "text/plain|a b");
    CHECK(processed("data:image/svg+xml;base64,PHN2Zy8+") == "image/svg+xml|<svg/>");
    CHECK(processed("DATA:;Base64 ,YQ==") == "text/plain;charset=US-ASCII|a");
}

TEST_CASE("a data: URL's fragment is not part of its bytes") {
    CHECK(processed("data:text/plain,abc#frag") == "text/plain|abc");
    CHECK(processed("data:text/plain,a%23b") == "text/plain|a#b");
}

TEST_CASE("a URL that is not data:, has no comma, or holds bad base64 is an error") {
    CHECK(processed("https://example.org/a,b") == "error");
    CHECK(processed("data:text/plain") == "error");
    CHECK(processed("data:;base64,a") == "error");
}

TEST_CASE("the data: URL processor compiles as C") {
    CHECK(prTestDataC());
}
