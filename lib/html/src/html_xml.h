// What HTML adds to XML parsing
//
// html - https://html.spec.whatwg.org/multipage/xhtml.html#parsing-xhtml-documents

#pragma once

#include "pr/dom.h"

#include "dom/src/dom_xml.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The entities of the DTDs that an XHTML doctype names, which are the HTML named character references
// (html/#parsing-xhtml-documents).
const DomXmlEntities* htmlXhtmlEntities();

// Replaces the document's contents with a parsererror element that describes `error`, in the form that
// html/#dom-domparser-parsefromstring creates. A browsing context shows an XML error the same way.
void htmlShowXmlError(PrDomDocument* document, const PrXmlError& error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
