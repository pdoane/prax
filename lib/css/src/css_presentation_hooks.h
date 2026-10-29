// The changes to a document that its presentation hears about
//
// For its lifetime, a presentation subscribes to the document's changes (dom_hooks.h) and gives the document's CSS
// state the functions through which CSS reaches it (CssPresentationHooks). Each change reaches the presentation
// directly.

#pragma once

#include "pr/css_presentation.h"
#include "pr/dom.h"

#include "css/src/css_pseudo.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Attaches the presentation to the document, or detaches it. Attaching gives the document CSS first (prCssEnable),
// which hears each change before the presentation.
void cssPresentationAttach(PrCssPresentation* presentation, PrDomDocument* document);
void cssPresentationDetach(PrCssPresentation* presentation, PrDomDocument* document);

// Returns the document's presentation, or null if it has none.
PrCssPresentation* PR_NULLABLE cssPresentationOf(const PrDomDocument* document);

// Tells the presentation of the element's document that the content of the replaced element changed: its natural size,
// or its image. Does nothing for an element that is not in its document.
void cssPresentationElementContentChanged(PrDomElement* element);

// Tells the presentation about the elements whose `states` changed as the element that the user points at or focuses
// moves from `from` to `to`. `states` are pseudo-classes that selectors read but attributes do not decide. The elements
// are those on one line of shadow-including ancestors and not the other.
void cssPresentationStateMoved(PrDomElement* PR_NULLABLE from, PrDomElement* PR_NULLABLE to,
                               const CssPseudoClass* states,
                               uint32_t count);

// Tells the presentation of the document that its media environment changed.
void cssPresentationDocumentMediaChanged(const PrDomDocument* PR_NULLABLE document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
