// The render-blocking mechanism and the expect link type
//
// html - https://html.spec.whatwg.org/multipage/dom.html#render-blocking-mechanism
// html - https://html.spec.whatwg.org/multipage/links.html#link-type-expect
//
// A document's render-blocking element set is kept by its window (PrHtmlWindow::renderBlocking), and the window does
// not render while it holds an element (htmlWindowRenderBlocked). Elements join it only while the document allows
// adding render-blocking elements, as it does until its parser inserts the body, and leave it once what they wait for
// is done, or once they are disconnected or no longer potentially render-blocking. The documents that no window shows
// keep no set.

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

struct PrHtmlWindow;

namespace pr_impl {

// Returns true if the element is potentially render-blocking (html/#potentially-render-blocking): its blocking
// attribute holds render, or it is `implicitly` so, as a style sheet that its document's parser created is.
bool htmlPotentiallyRenderBlocking(const PrDomElement* element, bool implicitly = false);

// Adds the element to its document's render-blocking element set if the document allows adding render-blocking elements
// (html/#block-rendering), and removes it (html/#unblock-rendering).
void htmlBlockRendering(PrDomElement* element);
void htmlUnblockRendering(PrDomElement* element);

// Returns true if the element is in its document's render-blocking element set.
bool htmlBlocksRendering(const PrDomElement* element);

// Processes an expect link (html/#process-internal-resource-link): it blocks rendering while its document loads and the
// element that its href indicates has yet to be fully parsed. Does nothing to an element that is not an HTML link.
void htmlProcessInternalResourceLink(PrDomElement* link);

// Processes every expect link of the document's render-blocking element set (html/#process-internal-resource-links), as
// an element's parse ends, an element is inserted, an ID changes or the document's readiness changes.
void htmlProcessInternalResourceLinks(PrDomDocument* document);

// The document's hooks for the mechanism: an element that script inserts, or whose parse ends, may be the one that an
// expect link waits for (html/#insert-an-element-at-the-adjusted-insertion-location, html/#stack-of-open-elements); a
// node about to be removed unblocks the elements of the set that it holds; and an attribute change runs the steps of
// html/#render-blocking-mechanism and of the expect link type.
void htmlRenderBlockingInserted(PrDomNode* node);
void htmlRenderBlockingRemoving(PrDomNode* node);
void htmlRenderBlockingAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name);

// Empties the window's set, as its document closes.
void htmlRenderBlockingReset(PrHtmlWindow* window);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
