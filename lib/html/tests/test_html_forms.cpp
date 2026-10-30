#include "pr/text_buffer.h"

#include "dom/src/dom_node.h"
#include "html/src/html_forms.h"
#include "html/src/html_hints.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

// Returns the value of a color input after the color well's steps, given `text`.
std::string well(const PrDomElement* input, PrStringView text) {
    TextBuffer out = {};
    htmlColorWellValue(input, text, &out);
    std::string value(textView(&out).data, textView(&out).size);
    textReset(&out);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a color input's value is a serialized color") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* input = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Input, "input");
    CHECK(well(input, PrStringView{}) == "#000000");
    CHECK(well(input, "#FFF") == "#ffffff");
    CHECK(well(input, "rgb(255 0 0 / 0.5)") == "#ff0000");
    domSetAttribute(input, PrDomNamespace_None, "alpha", "");
    CHECK(well(input, "rgb(255 0 0 / 0.5)") == "color(srgb 1 0 0 / 0.5)");
    // The element has no references: the document holds it while it is detached, and frees it with itself.
    domNodeRelease(document);
}

TEST_CASE("a legacy color value parses as HTML's rules say, even from text that is no color") {
    auto parsed = [](PrStringView text) {
        PrColor8 color{};
        if (!htmlParseLegacyColor(text, &color))
            return std::string("none");
        char out[8];
        snprintf(out, sizeof(out), "%02x%02x%02x", color.r, color.g, color.b);
        return std::string(out);
    };
    CHECK(parsed("#009900") == "009900");
    CHECK(parsed(" Red ") == "ff0000");
    CHECK(parsed("#0f8") == "00ff88");
    CHECK(parsed("chucknorris") == "c00000");
    CHECK(parsed("009900") == "009900");
    CHECK(parsed("#1234567890") == "125690");
    CHECK(parsed("ab") == "0a0b00");
    CHECK(parsed("transparent") == "none");
    CHECK(parsed("") == "none");
}
