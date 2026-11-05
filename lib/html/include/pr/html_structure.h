// Document structure: what a document's content means, as a structure tree (pr/draw_structure.h) whose keys are the
// document's nodes, which the marks of its painted content carry (prHtmlViewSetMarking).

#pragma once

#include "pr/base.h"
#include "pr/draw_structure.h"
#include "pr/html_page.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Appends the structure of the content of the page's document to `structure`, under its root, and gives the root the
// document's language. Each node's role is its ARIA role, or the role that HTML-AAM's element role mappings give its
// element. Reading order is the order of the document, with the elements that aria-owns names moved to their owner.
// Content that is not rendered, or that aria-hidden hides, is left out. A text node's key is the node, and so is a
// replaced element's, as the marks of a paint carry them. The page's presentation must be up to date, as after a paint
// (prHtmlViewPaint).
void prHtmlStructureAppend(PrHtmlPage* page, PrDrawStructure* structure);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
