#include "pr/string.h"
#include "pr/string_ops.h"

#include "apps/browser/address.h"

#include "tools/harness/test.h"

using namespace browser;

namespace {

// Text typed in the address bar and the URL that it resolves to.
struct ResolveCase {
    const char* typed;
    const char* url;
};

String resolved(const char* typed) {
    return addressResolve(typed);
}

} // namespace

TEST_CASE("text with a scheme resolves to itself") {
    const ResolveCase cases[] = {
        {"https://example.com/a?b#c", "https://example.com/a?b#c"},
        {"  http://example.com  ", "http://example.com"},
        {"HTTPS://Example.com", "HTTPS://Example.com"},
        {"about:blank", "about:blank"},
        {"data:text/html,<p>hi", "data:text/html,<p>hi"},
        {"ftp://example.com/file", "ftp://example.com/file"},
        {"prax:history", "prax:history"},
    };
    for (const ResolveCase& c : cases) {
        CAPTURE(c.typed);
        CHECK(resolved(c.typed) == c.url);
    }
}

TEST_CASE("text that names a host resolves to the host's https URL, and the local host to http") {
    const ResolveCase cases[] = {
        {"example.com", "https://example.com"},
        {"en.wikipedia.org/wiki/HTML", "https://en.wikipedia.org/wiki/HTML"},
        {"example.com:8443/path", "https://example.com:8443/path"},
        {"intranet:8080", "https://intranet:8080"},
        {"localhost", "http://localhost"},
        {"localhost:8000/index.html", "http://localhost:8000/index.html"},
        {"127.0.0.1:3000", "http://127.0.0.1:3000"},
        {"[::1]:8080/", "http://[::1]:8080/"},
    };
    for (const ResolveCase& c : cases) {
        CAPTURE(c.typed);
        CHECK(resolved(c.typed) == c.url);
    }
}

TEST_CASE("text that names no URL is a search for the text") {
    const ResolveCase cases[] = {
        {"html parser", "https://html.duckduckgo.com/html/?q=html+parser"},
        {"prax", "https://html.duckduckgo.com/html/?q=prax"},
        {"what is a.b", "https://html.duckduckgo.com/html/?q=what+is+a.b"},
        {"c++ & rust?", "https://html.duckduckgo.com/html/?q=c%2B%2B+%26+rust%3F"},
        {"example.", "https://html.duckduckgo.com/html/?q=example."},
        {".hidden", "https://html.duckduckgo.com/html/?q=.hidden"},
        {"caf\xc3\xa9", "https://html.duckduckgo.com/html/?q=caf%C3%A9"},
    };
    for (const ResolveCase& c : cases) {
        CAPTURE(c.typed);
        CHECK(resolved(c.typed) == c.url);
    }
}

TEST_CASE("whitespace resolves to nothing") {
    CHECK(resolved("") == "");
    CHECK(resolved(" \t\n") == "");
}

TEST_CASE("a path to a file that exists resolves to its file URL") {
    String url = resolved(BROWSER_TESTS_DIR "/test_address.cpp");
    CHECK(prStringStartsWith(url, "file://"));
    CHECK(prStringContains(url, "apps/browser/tests/test_address.cpp"));
    CHECK(resolved("/no/such/file.html") == "https://html.duckduckgo.com/html/?q=%2Fno%2Fsuch%2Ffile.html");
}

TEST_CASE("a blank tab shows no address, and any other shows its URL") {
    CHECK(addressIsBlank(""));
    CHECK(addressIsBlank("about:blank"));
    CHECK_FALSE(addressIsBlank("about:blank#top"));
    CHECK(addressShown("about:blank") == "");
    CHECK(addressShown("https://example.com/") == "https://example.com/");
}

TEST_CASE("the browser's own pages are the URLs of the prax scheme, in any case") {
    CHECK(addressIsBrowserPage("prax:history"));
    CHECK(addressIsBrowserPage("PRAX:history"));
    CHECK_FALSE(addressIsBrowserPage("prax"));
    CHECK_FALSE(addressIsBrowserPage("https://prax.example/"));
}

TEST_CASE("the host of a URL drops its credentials, its port and a leading www") {
    CHECK(addressHost("https://www.example.com/path") == "example.com");
    CHECK(addressHost("http://user:secret@example.com:8080/") == "example.com");
    CHECK(addressHost("https://[::1]:8080/") == "[::1]");
    CHECK(addressHost("file:///Users/me/page.html") == "");
    CHECK(addressHost("about:blank") == "");
    CHECK(addressHost("not a url") == "");
}

TEST_CASE("a match ignores ASCII case, and the empty query matches everything") {
    CHECK(addressMatches("https://Example.com/", "example"));
    CHECK(addressMatches("Hacker News", "NEWS"));
    CHECK(addressMatches("anything", ""));
    CHECK_FALSE(addressMatches("short", "longer text"));
    CHECK_FALSE(addressMatches("example", "exampel"));
}
