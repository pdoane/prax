#include "html/src/html_xml.h"

#include "pr/html_entity.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// The public identifiers whose DTDs hold the HTML named character references.
constexpr PrStringView kXhtmlPublicIds[] = {
    "-//W3C//DTD XHTML 1.0 Transitional//EN",
    "-//W3C//DTD XHTML 1.1//EN",
    "-//W3C//DTD XHTML 1.0 Strict//EN",
    "-//W3C//DTD XHTML 1.0 Frameset//EN",
    "-//W3C//DTD XHTML Basic 1.0//EN",
    "-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN",
    "-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN",
    "-//W3C//DTD MathML 2.0//EN",
    "-//WAPFORUM//DTD XHTML Mobile 1.0//EN",
};

PrStringView xhtmlEntity(void*, PrStringView publicId, PrStringView name) {
    bool xhtml = false;
    for (PrStringView id : kXhtmlPublicIds)
        xhtml |= publicId == id;
    if (!xhtml)
        return PrStringView{};
    // A reference is looked up with its semicolon.
    TextBuffer reference;
    textAppend(&reference, name);
    textAppendChar(&reference, ';');
    PrStringView expansion = prHtmlEntityMatch(textView(&reference));
    textReset(&reference);
    return expansion;
}

constexpr DomXmlEntities kXhtmlEntities = {nullptr, xhtmlEntity};

} // namespace

const DomXmlEntities* htmlXhtmlEntities() {
    return &kXhtmlEntities;
}

void htmlShowXmlError(PrDomDocument* document, const PrXmlError& error) {
    while (document->first != nullptr)
        domDetach(document->first);
    DomNamespaceId space = PrDomNamespace_None;
    domNamespaceFromUrl(document, "http://www.mozilla.org/newlayout/xml/parsererror.xml", &space);
    PrDomElement* element = domElementCreateNs(document, space, PrStringView{}, "parsererror");
    TextBuffer text;
    textAppend(&text, "XML Parsing Error: ");
    textAppend(&text, error.message);
    textAppend(&text, "\nLine Number ");
    textAppendU64(&text, error.line);
    textAppend(&text, ", Column ");
    textAppendU64(&text, error.column);
    textAppendChar(&text, ':');
    domAttach(element, domTextCreate(document, textView(&text)), nullptr);
    textReset(&text);
    domAttach(document, element, nullptr);
}

} // namespace pr_impl
