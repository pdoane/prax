#include "pr/mime.h"

#include "doctest/doctest.h"

#include <string>

extern "C" int prTestMimeCApi(void);

namespace pr_impl {

namespace {

// Returns the value of the parameter `name` of `type`, or "<none>" if it has none.
std::string parameterOf(PrStringView type, PrStringView name) {
    PrString value = prStringNull();
    if (!prMimeTypeParameter(type, name, &value))
        return "<none>";
    PrStringView view = prStringView(&value);
    std::string result(view.data, view.size);
    prStringReset(&value);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestMimeCApi() == 0);
}

TEST_CASE("a MIME type's essence is its type and subtype, in their case, without parameters or whitespace") {
    CHECK(prMimeTypeEssence("text/html") == "text/html");
    CHECK(prMimeTypeEssence(" Text/HTML ; charset=utf-8") == "Text/HTML");
    CHECK(prMimeTypeEssence("") == "");
}

TEST_CASE("a parameter is the first of its name, unquoted, and only in a type that parses") {
    CHECK(parameterOf("text/html; charset=utf-8", "charset") == "utf-8");
    CHECK(parameterOf("text/html;CHARSET=\"utf-8\"", "charset") == "utf-8");
    CHECK(parameterOf("text/markdown; variant=\"GF\\M\"", "variant") == "GFM");
    // The first of a name wins, and an empty unquoted value is no value.
    CHECK(parameterOf("text/plain; charset=; charset=a; charset=b", "charset") == "a");
    CHECK(parameterOf("text/plain; charset", "charset") == "<none>");
    CHECK(parameterOf("text/plain; other=x", "charset") == "<none>");
    // A type without a subtype, or whose type is not a token, does not parse.
    CHECK(parameterOf("text; charset=utf-8", "charset") == "<none>");
    CHECK(parameterOf("te xt/plain; charset=utf-8", "charset") == "<none>");
}

TEST_CASE("an essence matches the JavaScript MIME types, and a type is XML by its essence or a +xml suffix") {
    CHECK(prMimeTypeIsJavaScript("text/javascript"));
    CHECK(prMimeTypeIsJavaScript("Application/X-JavaScript"));
    CHECK(!prMimeTypeIsJavaScript("text/javascript; charset=utf-8"));
    CHECK(!prMimeTypeIsJavaScript("module"));
    CHECK(prMimeTypeIsXml("application/xml"));
    CHECK(prMimeTypeIsXml("Text/XML; charset=utf-8"));
    CHECK(prMimeTypeIsXml("image/svg+xml"));
    CHECK(!prMimeTypeIsXml("text/html"));
}
