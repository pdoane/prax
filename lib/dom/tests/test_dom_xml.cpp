// XML documents as the builder makes them from the parser's calls.

#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "dom/src/dom_node.h"
#include "dom/src/dom_xml.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {
namespace {

// Returns the text of a view. The null view reads as the empty string.
std::string textOf(PrStringView text) {
    return std::string(text.size != 0 ? text.data : "", text.size);
}

std::string take(PrString value) {
    PrStringView view = prStringView(&value);
    std::string text(view.data != nullptr ? view.data : "", view.size);
    prStringReset(&value);
    return text;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a parsed XML document holds its elements, attributes, text and doctype") {
    PrXmlError error = {};
    PrDomDocument* document = prDomParseXml("<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"x\">"
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:x='urn:x' x:a='1'>&#65;b</svg>", "image/svg+xml", &error);
    CHECK(error.message.data == nullptr);
    CHECK(textOf(prDomDocumentContentType(document)) == "image/svg+xml");
    PrDomDocumentType* doctype = prDomDocumentDoctype(document);
    REQUIRE(doctype != nullptr);
    CHECK(textOf(prDomDocumentTypeName(doctype)) == "svg");
    CHECK(textOf(prDomDocumentTypePublicId(doctype)) == "-//W3C//DTD SVG 1.1//EN");
    PrDomElement* svg = prDomDocumentElement(document);
    REQUIRE(svg != nullptr);
    CHECK(textOf(prDomElementNamespaceUri(svg)) == "http://www.w3.org/2000/svg");
    CHECK(textOf(prDomElementLocalName(svg)) == "svg");
    CHECK(textOf(prDomElementGetAttributeNs(svg, "urn:x", "a")) == "1");
    CHECK(prDomElementAttributeCount(svg) == 3);
    CHECK(take(prDomNodeTextContent(prDomElementAsNode(svg))) == "Ab");
    prDomDocumentRelease(document);
}

TEST_CASE("a document that is not well-formed keeps what came before the error") {
    PrXmlError error = {};
    PrDomDocument* document = prDomParseXml("<a><b/><c></a>", "application/xml", &error);
    CHECK(error.line == 1);
    CHECK(error.column == 15);
    PrDomElement* a = prDomDocumentElement(document);
    REQUIRE(a != nullptr);
    CHECK(textOf(prDomElementLocalName(a)) == "a");
    CHECK(prDomNodeFirstChild(prDomElementAsNode(a)) != nullptr);
    prDomDocumentRelease(document);
}

