// Page boxes: what a document's @page rules give each page (css-page-3).
//
// css-page-3 - https://drafts.csswg.org/css-page-3/
//
// Layout asks for each page's box as it reaches the page (pr/layout_page.h), on the thread that lays the tree out. A
// document's page boxes are therefore resolved ahead, on the document's thread, for every kind of page that its @page
// selectors tell apart: each page name that they use, or none, and whether the page is the first, a left or a right
// page, and blank. A page of a name that no selector uses takes the boxes of no name.

#pragma once

#include "pr/dom.h"
#include "pr/layout_page.h"
#include "pr/raw_array.h"

#include "css/src/css_media.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The box of one kind of page, and the @page rules that apply to it in the order of the cascade
// (css-page-3/#cascading-and-page-context), each of which the box holds. The rules give the page context's declarations
// and those of its margin boxes.
struct CssPageBox {
    PrLayoutName name;
    bool first;
    bool blank;
    PrLayoutPageSide side;
    PrLayoutPageBox box;
    RawArray<CssRuleData*> rules;
};

// The page boxes of a document, by kind of page.
struct CssPageBoxes {
    RawArray<CssPageBox> boxes;
};

// Resolves the page boxes of `document` from its @page rules (css-page-3/#page-properties), evaluating their media in
// `media`, over `initial`, the box that a page has before any rule applies. A rule's page names are interned in `tree`.
// Replaces what `out` held.
void cssPageBoxesResolve(PrDomDocument* document, const CssMediaEnvironment* media, PrLayoutTree* tree,
    const PrLayoutPageBox& initial, CssPageBoxes& out);

// Returns the index among the boxes of the kind of page that `page` describes, or UINT32_MAX for none.
uint32_t cssPageBoxesKind(const CssPageBoxes& boxes, const PrLayoutPageContext& page);

// Returns the box of the page that `page` describes, or `initial` if the boxes hold none for it.
PrLayoutPageBox cssPageBoxesFind(const CssPageBoxes& boxes, const PrLayoutPageContext& page, const PrLayoutPageBox& initial);

// Returns true if two sets of boxes give every kind of page the same box. The rules that apply to them may differ.
bool cssPageBoxesEqual(const CssPageBoxes& a, const CssPageBoxes& b);

// Frees the boxes' storage.
void cssPageBoxesReset(CssPageBoxes& boxes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
