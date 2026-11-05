// Page-margin boxes: what a paged presentation gives the margins of its pages.
//
// css-page-3 - https://drafts.csswg.org/css-page-3/
// css-gcpm-3 - https://drafts.csswg.org/css-gcpm-3/
//
// A page's margin boxes are boxes of the presentation's tree, children of the root's box, which layout sizes and places
// in each page's margins (pr/layout_page.h). The presentation makes them once its tree's content is laid out into
// pages, because what they show depends on the whole layout: the page and pages counters, the counters of the document
// at the start of each page, the named strings that the document sets (css-gcpm-3/#named-strings), and copies of the
// running elements on each page (css-gcpm-3/#running-elements). Their styles cascade from the @page rules, the page
// context's inheriting from the root element and each margin box's from its page context (css-page-3/#page-properties).
//
// Making the boxes reads what each page holds, and the document in tree order only if a margin box reads a document's
// counter, a named string or a running element. A page's box that would hold what it holds already is kept, so a pass
// over unchanged pages changes nothing, and each page's margin boxes cost what they hold.

#pragma once

#include "css/src/css_page.h"
#include "css/src/css_presentation.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Gives the pages of the last layout of the presentation's tree the margin boxes that the rules of `boxes` give them
// (css-page-3/#margin-boxes), in place of those it gave before. Called on the presentation's thread, while it holds the
// tree, once the content is laid out. The next update lays out the boxes that changed. Does nothing if neither the
// presentation, nor the number of pages, nor the page boxes changed since the last call, as `boxesChanged` says of the
// boxes.
void cssPresentationSetPageMargins(PrCssPresentation* presentation, const CssPageBoxes& boxes, bool boxesChanged);

// Removes the page-margin boxes from the presentation's tree, as cssPresentationSetPageMargins does for no pages.
void cssPresentationClearPageMargins(PrCssPresentation* presentation);

// Frees what the presentation keeps of its page-margin boxes, whose nodes its tree frees.
void cssPageMarginsReset(CssPageMargins& margins);

// The links inside the copies of running elements in page-margin boxes, valid until the next pass.
struct CssPageMarginLinks {
    const CssPageMarginLink* PR_NULLABLE links;
    uint32_t count;
};

// Returns the links inside the copies of running elements in the presentation's page-margin boxes.
CssPageMarginLinks cssPresentationPageMarginLinks(const PrCssPresentation* presentation);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
