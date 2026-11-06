// File URLs: the URLs of paths, and the paths that URLs name on this machine.

#include "pr/environment.h"
#include "pr/url_file.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

std::string urlOf(PrStringView path) {
    PrRawString url = prUrlFromFilePath(path);
    std::string out(prRawStringView(&url).data, prRawStringView(&url).size);
    prRawStringReset(&url);
    return out;
}

// The path that `url` names, or "-" for none.
std::string pathOf(PrStringView url) {
    PrRawString path = prRawStringNull();
    if (!prUrlToFilePath(url, &path))
        return "-";
    std::string out(prRawStringView(&path).data, prRawStringView(&path).size);
    prRawStringReset(&path);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("url file: a path's URL escapes what a URL's path escapes, and the percent sign") {
    CHECK(urlOf("/tmp/notes.txt") == "file:///tmp/notes.txt");
    CHECK(urlOf("/a b/100%/x#y?z") == "file:///a%20b/100%25/x%23y%3Fz");
    CHECK(urlOf("/caf\xc3\xa9") == "file:///caf%C3%A9");
#if defined(_WIN32)
    CHECK(urlOf("C:\\dir\\a.txt") == "file:///C:/dir/a.txt");
#else
    CHECK(urlOf("/a\\b") == "file:///a%5Cb");
#endif
}

TEST_CASE("url file: a URL's path is decoded, for an empty host, localhost and this machine's name") {
    CHECK(pathOf("file:///tmp/a%20b.txt") == "/tmp/a b.txt");
    CHECK(pathOf("file://localhost/tmp/x") == "/tmp/x");
    PrRawString name = prEnvironmentHostName();
    std::string host(prRawStringView(&name).data, prRawStringView(&name).size);
    prRawStringReset(&name);
    if (!host.empty()) {
        CHECK(pathOf("file://" + host + "/tmp/x") == "/tmp/x");
        std::string label = host.substr(0, host.find('.'));
        CHECK(pathOf("file://" + label + "/tmp/x") == "/tmp/x");
    }
}

TEST_CASE("url file: another scheme, another machine and a path that is not text name no path") {
    CHECK(pathOf("https://example.com/tmp/x") == "-");
    CHECK(pathOf("file://some-other-machine.invalid/tmp/x") == "-");
    CHECK(pathOf("file:///tmp/%00x") == "-");
    CHECK(pathOf("file:///tmp/%FF") == "-");
    CHECK(pathOf("") == "-");
}

TEST_CASE("url file: a path's URL names the path") {
    for (const char* path : {"/tmp/a b/c%d.txt", "/x/#?{}"})
        CHECK(pathOf(urlOf(path)) == path);
}
