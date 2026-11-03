// The owners of style sheets: the style and link elements of HTML and SVG, and xml-stylesheet processing instructions
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
// html - https://html.spec.whatwg.org/multipage/semantics.html#the-style-element

#pragma once

#include "pr/charset.h"
#include "pr/dom.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/url.h"

#include "css/src/css_object.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The data of a style element's sheet, kept for reuse by the document's other style elements with the same text. The
// entry holds a reference to the data.
struct CssSheetCacheEntry {
    uint64_t hash;
    PrRawString text;
    CssSheetData* data;
};

// The maximum number of sheets that a document keeps for reuse.
constexpr uint32_t kCssSheetCacheSize = 16;

// Returns the sheet of a style or link element, or null if it has none.
PrCssStyleSheet* PR_NULLABLE cssElementSheet(const PrDomElement* element);

// Replaces the sheet of an HTML or SVG style element with one parsed from its text (html/#update-a-style-block), and
// fetches the sheets that it imports. The element's load or error event fires once they arrive. Does nothing for any
// other element.
void cssStyleBlockUpdate(PrDomElement* element, DomInsertedBy by = DomInsertedBy_Script);

// What the owner of a sheet does once its sheet and the sheets that it imports have arrived or failed: `loaded` is
// false if any failed. Runs only in a document with a host.
typedef void CssOwnerSettledFn(PrDomElement* element, bool loaded);

// Starts an update of the sheet of a style or link element: removes the sheet from its tree's list, drops the fetches
// that it waits for, and takes the element out of its document's blocking sheets.
void cssOwnerReset(PrDomElement* element);

// Adds the element to its document's blocking sheets, or removes it. Its sheets then block the document's scripts
// (html/#script-blocking-style-sheet-set), and its rendering too if the document has no body yet
// (html/#render-blocking-element-set). The element leaves the set once its sheets settle.
void cssOwnerSetBlocking(PrDomElement* element, bool blocking);

// Fetches the sheet that `url` names for the element, with `cors`, and the sheets that it imports, in a document with a
// host. The sheets decode in `environment` unless their bytes or responses name another charset. Once the sheet
// arrives, it joins its tree's list with the element as its owner, starting disabled if `alternate` and it has a title
// (cssom-1/#css-style-sheet-set). Once every sheet has arrived or failed, `settled` runs.
void cssOwnerFetch(PrDomElement* element, const PrUrl* url, DomCorsSettings cors, PrCharset environment,
    bool alternate, CssOwnerSettledFn* settled);

// Returns true if the element's type attribute names CSS, or if the element has no type attribute or an empty one.
bool cssOwnerTypeIsCss(const PrDomElement* element);

// The media attribute of a sheet's owner took `value`, the null view for none. The sheet's media changes in place.
void cssOwnerMediaChanged(PrDomElement* element, PrStringView value);

// Fetches the sheet of an xml-stylesheet processing instruction in the prolog of a document with a host
// (cssom-1/#requirements-on-user-agents-implementing-the-xml-stylesheet-processing-instruction): the sheet that its
// href pseudo-attribute names, if its type is CSS or absent, and its alternate is not yes. Once the sheet arrives, it
// joins the document's sheets in tree order, with the title and media of its pseudo-attributes, until the instruction
// leaves the document. Does nothing for any other node.
void cssXmlStylesheetInserted(PrDomNode* node);

// cssDocumentHasBlockingSheet returns true if the document's parser inserted a style or link element whose sheets the
// document still fetches, which blocks its scripts. cssDocumentBlocksRendering returns true if such an element also
// blocks its rendering, which is the case if the parser inserted it before the document had a body.
bool cssDocumentHasBlockingSheet(const PrDomDocument* document);
bool cssDocumentBlocksRendering(const PrDomDocument* document);

// Removes the sheet of an xml-stylesheet processing instruction that leaves its document. Does nothing for any other
// node.
void cssInstructionRemoving(PrDomNode* node);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
