// The link element's style sheets and preloads
//
// html - https://html.spec.whatwg.org/multipage/semantics.html#the-link-element

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_hooks.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Updates an HTML link element's sheet or preload (html/#link-type-stylesheet, html/#link-type-preload). If the element
// is a stylesheet link connected in a document with a host, fetches its sheet and the sheets that it imports. Once they
// arrive, gives the element the sheet and fires load or error at it. Does nothing for any other element.
void htmlLinkUpdate(PrDomElement* element, DomInsertedBy by = DomInsertedBy_Script);

// Runs a link's attribute change steps: the attributes that name its resource fetch it again
// (html/#link-type-stylesheet).
void htmlLinkAttributeChanged(PrDomElement* element, PrStringView name);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
