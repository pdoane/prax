// HTML's subscription to a document
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Subscribes HTML to the changes of the document that HTML's elements follow (dom_hooks.h), which a document that the
// DOM creates from it inherits. The links that the document already holds take their sheets as if the parser had just
// inserted them. Does nothing for a document that HTML follows.
void htmlEnable(PrDomDocument* document);

struct HtmlParser;

// Returns the script-created parser of a document that no browsing context shows (html/#opening-the-input-stream),
// which runs no scripts: the one that document.open() last made, while it is active, or null. With `start`, it starts a
// parser in place of any active one, empty and open for input. The document owns the parser, which HTML follows the
// document to free. Returns null for a document that HTML does not follow.
HtmlParser* PR_NULLABLE htmlDocumentParser(PrDomDocument* document, bool start = false);

// Frees the document's script-created parser (htmlDocumentParser), as its parse ends.
void htmlDocumentEndParser(PrDomDocument* document);

// The Window whose associated Document the document was before its browsing context discarded it, or null: the window
// that document.open() still erases the listeners of (html/#document-open-steps). The document does not hold the
// window, which clears this as it is freed.
PrDomEventTarget* PR_NULLABLE htmlDocumentFormerWindow(const PrDomDocument* document);
void htmlDocumentSetFormerWindow(PrDomDocument* document, PrDomEventTarget* PR_NULLABLE window);

// Whether a navigation of the document's browsing context replaced the document with another, and notes that it did.
bool htmlDocumentReplaced(const PrDomDocument* document);
void htmlDocumentSetReplaced(PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
