#include "url/src/url_record.h"
#include "url/src/url_scheme.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

std::string take(PrRawString value) {
    PrStringView text = prRawStringView(&value);
    std::string result(text.data != nullptr ? text.data : "", text.size);
    prRawStringReset(&value);
    return result;
}
std::string parse(const char* text, const char* base = nullptr,
        PrUrlSerialization serialization = PrUrlSerialization_Whole) {
    PrUrl* parent = nullptr;
    if (base != nullptr) REQUIRE(urlParse(base, nullptr, PrCharset_Utf8, &parent) == PrStatus_Ok);
    PrUrl* result = nullptr;
    PrStatus status = urlParse(text, parent, PrCharset_Utf8, &result);
    urlDestroy(parent);
    if (status != PrStatus_Ok) return take(prRawStringCopy(prStatusName(status)));
    std::string serialized = take(urlSerialize(result, serialization));
    Url* copy = urlClone(result);
    urlDestroy(result);
    CHECK(take(urlSerialize(copy, serialization)) == serialized);
    urlDestroy(copy);
    return serialized;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("URL parsing resolves references without filesystem semantics") {
    CHECK(parse("../m.js?x#y", "https://EXAMPLE.org:443/a/b/main.js") == "https://example.org/a/m.js?x#y");
    CHECK(parse("/a/%2e%2e/b", "https://example.org/c") == "https://example.org/b");
    CHECK(parse("//other.test/x", "https://example.org/c") == "https://other.test/x");
    CHECK(parse("https:next", "https://example.org/a/main") == "https://example.org/a/next");
    CHECK(parse("", "https://example.org/a?q#old") == "https://example.org/a?q");
    CHECK(parse("?", "https://example.org/a?q#old") == "https://example.org/a?");
    CHECK(parse("#", "https://example.org/a?q#old") == "https://example.org/a?q#");
    CHECK(parse("a%2fb", "https://example.org/") == "https://example.org/a%2fb");
    CHECK(parse("  ht\ntps:\\example.org\\a b  ") == "https://example.org/a%20b");
}

TEST_CASE("URL hosts and credentials are normalized independently") {
    CHECK(parse("https://u@x:p@EXAMPLE.org:443/a?q#f") == "https://u%40x:p@example.org/a?q#f");
    CHECK(parse("https://u:p@example.org:8443/a?q#f", nullptr, PrUrlSerialization_ExcludeFragment) ==
        "https://u:p@example.org:8443/a?q");
    CHECK(parse("http://0x7f.1/") == "http://127.0.0.1/");
    CHECK(parse("http://2130706433/") == "http://127.0.0.1/");
    CHECK(parse("http://[2001:0db8:0:0:0:0:0:1]/") == "http://[2001:db8::1]/");
    CHECK(parse("http://[::ffff:192.0.2.1]/") == "http://[::ffff:c000:201]/");
    CHECK(parse("http://[::]/") == "http://[::]/");
    CHECK(parse("other://EXAMPLE.org:80") == "other://EXAMPLE.org:80");
    CHECK(parse("http://example.org:65536") == "Syntax");
    CHECK(parse("http://[:::1]") == "Syntax");
    CHECK(parse("http://0x100000000/") == "Syntax");
    CHECK(parse("http://example.999999999999999") == "Syntax");
    CHECK(parse("http://a%23b/") == "Syntax");
    CHECK(parse("http://é.test/") == "http://xn--9ca.test/");
    CHECK(parse("http://xn--nxasmq6b/") == "http://xn--nxasmq6b/");
    CHECK(parse("http://XN--A.example/") == "http://xn--a.example/");
    CHECK(parse("http://xn--a.é/") == "Syntax");
    CHECK(parse("http://\xef\xbc\xa1\xe3\x80\x82" "com/") == "http://a.com/");
    CHECK(parse("http://%ff.test/") == "Syntax");
    CHECK(parse("http://é^.test/") == "Syntax");
}

TEST_CASE("file URLs retain URL drive and authority rules") {
    CHECK(parse("file:///tmp/a b.js") == "file:///tmp/a%20b.js");
    CHECK(parse("file://localhost/tmp/x") == "file:///tmp/x");
    CHECK(parse("C|/x", "file://server/share/main") == "file://server/C:/x");
    CHECK(parse("file://server/share/x") == "file://server/share/x");
    CHECK(parse("../x", "file:///tmp/a/main.js") == "file:///tmp/x");
    CHECK(parse("file:C|/a/../b") == "file:///C:/b");
    CHECK(parse("/next", "file:///C:/a/main.js") == "file:///C:/next");
    CHECK(parse("../../../../x", "file:///C:/a/main.js") == "file:///C:/x");
    CHECK(parse("file://u:p@server/x") == "Syntax");
    CHECK(parse("file://server:80/x") == "Syntax");
}

TEST_CASE("opaque URLs and absent components remain distinct") {
    CHECK(parse("custom:opaque  ?q") == "custom:opaque %20?q");
    CHECK(parse("mailto:a@example.org?q#f") == "mailto:a@example.org?q#f");
    CHECK(parse("#next", "data:text/plain,hello") == "data:text/plain,hello#next");
    CHECK(parse("x", "data:text/plain,hello") == "Syntax");
    CHECK(parse("custom:/a/..//b") == "custom:/.//b");
    PrUrl* url = nullptr;
    REQUIRE(urlParse("https://example.org/?#", nullptr, PrCharset_Utf8, &url) == PrStatus_Ok);
    CHECK(!prRawStringIsNull(&url->query));
    CHECK(!prRawStringIsNull(&url->fragment));
    CHECK(prRawStringIsEmpty(&url->query));
    CHECK(!url->hasPort);
    urlDestroy(url);
    REQUIRE(urlParse("custom:/a", nullptr, PrCharset_Utf8, &url) == PrStatus_Ok);
    CHECK(prRawStringIsNull(&url->host));
    CHECK(prRawStringIsNull(&url->query));
    CHECK(prRawStringIsNull(&url->fragment));
    urlDestroy(url);
    CHECK(urlParse("relative", nullptr, PrCharset_Utf8, &url) == PrStatus_Syntax);
    CHECK(url == nullptr);
    urlDestroy(nullptr);
}

TEST_CASE("URL special schemes use their specified default ports") {
    CHECK(urlScheme("file") == UrlScheme_File);
    CHECK(kUrlSchemePort[UrlScheme_File] == -1);
    CHECK(urlScheme("http") == UrlScheme_Http);
    CHECK(kUrlSchemePort[UrlScheme_Http] == 80);
    CHECK(kUrlSchemePort[UrlScheme_Https] == 443);
    CHECK(kUrlSchemePort[UrlScheme_Ftp] == 21);
    CHECK(kUrlSchemePort[UrlScheme_Ws] == 80);
    CHECK(kUrlSchemePort[UrlScheme_Wss] == 443);
    CHECK(urlScheme("custom") == UrlScheme_None);
    CHECK(urlScheme("") == UrlScheme_None);
    CHECK(urlScheme("https-extra") == UrlScheme_None);
}
