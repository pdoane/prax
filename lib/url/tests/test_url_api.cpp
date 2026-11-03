#include "pr/url.h"

#include "doctest/doctest.h"

#include <string>

extern "C" int prTestUrlCApi(void);

namespace pr_impl {

namespace {

std::string text(PrStringView view) {
    return std::string(view.data != nullptr ? view.data : "", view.size);
}

std::string take(PrRawString value) {
    std::string result = text(prRawStringView(&value));
    prRawStringReset(&value);
    return result;
}

PrUrl* parse(const char* input) {
    PrUrl* url = nullptr;
    REQUIRE(prUrlCreate(input, nullptr, PrCharset_Utf8, &url) == PrStatus_Ok);
    return url;
}

std::string href(const PrUrl* url) {
    return take(prUrlSerialize(url, PrUrlSerialization_Whole));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the URL API compiles and links as C") {
    CHECK(prTestUrlCApi() == 0);
}

TEST_CASE("a URL's parameters stay live across href replacement") {
    PrUrl* url = parse("https://example.org/?a=1&a=2");
    PrUrlSearchParams* params = prUrlSearchParams(url);
    prUrlSearchParamsDelete(params, "a", "1");
    CHECK(text(prUrlQuery(url)) == "a=2");
    CHECK(prUrlSetComponent(url, PrUrlComponent_Href, "http://other.test/?b=3") == PrStatus_Ok);
    CHECK(prUrlSearchParams(url) == params);
    CHECK(text(prUrlSearchParamsNameAt(params, 0)) == "b");
    CHECK(text(prUrlSearchParamsGet(params, "b")) == "3");
    CHECK(prUrlSearchParamsGet(params, "c").data == nullptr);
    CHECK(prUrlSetComponent(url, PrUrlComponent_Href, "relative") == PrStatus_Syntax);
    CHECK(href(url) == "http://other.test/?b=3");
    prUrlSetComponent(url, PrUrlComponent_Search, "\n?query");
    prUrlSetComponent(url, PrUrlComponent_Hash, "\t#fragment");
    CHECK(text(prUrlQuery(url)) == "?query");
    CHECK(text(prUrlFragment(url)) == "#fragment");
    prUrlDestroy(url);
}

TEST_CASE("URLSearchParams sorts names by UTF-16 code units") {
    // U+E000 sorts after U+10FFFF, because U+10FFFF's surrogates come first.
    PrUrl* url = parse("https://example.org/?%EE%80%80=1&%F4%8F%BF%BF=2&%F0%9F%98%80=3&z=4");
    prUrlSearchParamsSort(prUrlSearchParams(url));
    CHECK(text(prUrlQuery(url)) == "z=4&%F0%9F%98%80=3&%F4%8F%BF%BF=2&%EE%80%80=1");
    prUrlDestroy(url);
}

TEST_CASE("URL setters run the parser from their state") {
    PrUrl* url = parse("https://example.org:8080/a");
    prUrlSetComponent(url, PrUrlComponent_Host, "other.test:99x");
    CHECK(href(url) == "https://other.test:99/a");
    prUrlSetComponent(url, PrUrlComponent_Port, "443");
    CHECK(prUrlPort(url) == -1);
    prUrlSetComponent(url, PrUrlComponent_Protocol, "foo");
    CHECK(text(prUrlScheme(url)) == "https");
    prUrlSetComponent(url, PrUrlComponent_Pathname, "b/../c d");
    CHECK(text(prUrlPath(url)) == "/c%20d");
    prUrlDestroy(url);
}

TEST_CASE("a query is percent-encoded in the charset of the document that a URL is parsed for") {
    auto parsed = [](PrStringView input, PrCharset charset) {
        PrUrl* url = nullptr;
        REQUIRE(prUrlCreate(input, nullptr, charset, &url) == PrStatus_Ok);
        PrRawString text = prUrlSerialize(url, PrUrlSerialization_Whole);
        std::string result(prRawStringView(&text).data, prRawStringView(&text).size);
        prRawStringReset(&text);
        prUrlDestroy(url);
        return result;
    };
    CHECK(parsed("https://example.test/caf\xC3\xA9?caf\xC3\xA9#caf\xC3\xA9", PrCharset_Windows1252) ==
        "https://example.test/caf%C3%A9?caf%E9#caf%C3%A9");
    // A code point that the charset cannot encode is a numeric character reference.
    CHECK(parsed("https://example.test/?\xE4\xB8\xAD", PrCharset_Windows1252) == "https://example.test/?%26%2320013%3B");
    CHECK(parsed("https://example.test/?\xE4\xB8\xAD", PrCharset_Gbk) == "https://example.test/?%D6%D0");
    // A charset without an encoder encodes as UTF-8, and so do the queries of URLs that are not special and of ws.
    CHECK(parsed("https://example.test/?\xC3\xA9", PrCharset_Utf16Le) == "https://example.test/?%C3%A9");
    CHECK(parsed("custom:x?\xC3\xA9", PrCharset_Windows1252) == "custom:x?%C3%A9");
    CHECK(parsed("ws://example.test/?\xC3\xA9", PrCharset_Windows1252) == "ws://example.test/?%C3%A9");
    CHECK(parsed("https://example.test/?\xE3\x81\x82", PrCharset_Iso2022Jp) == "https://example.test/?%1B$B$%22%1B(B");
}
