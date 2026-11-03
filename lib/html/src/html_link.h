// The link element's style sheets, preloads and icons
//
// html - https://html.spec.whatwg.org/multipage/semantics.html#the-link-element

#pragma once

#include "pr/dom.h"
#include "pr/image.h"
#include "pr/raw_string.h"
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

// Returns the URL of the icon that the document's links name (html/#rel-icon): that of the last HTML link element in
// tree order whose rel holds icon and whose href parses. A link whose type is one that `codec` cannot decode names no
// icon, as the user agent may skip it. A null `codec` skips none. Returns the null string if no link names an icon. The
// caller releases the string with prRawStringReset.
PrRawString htmlLinkIconUrl(const PrDomDocument* document, PrImageCodec* PR_NULLABLE codec);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
