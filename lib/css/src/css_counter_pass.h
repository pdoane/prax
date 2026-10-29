// The counter pass: the values of CSS counters over the boxes that a presentation generates, the quote depth, and the
// text that they give the content of pseudo-elements.
//
// css-lists-3 - https://drafts.csswg.org/css-lists-3/
// css-content-3 - https://drafts.csswg.org/css-content-3/
//
// Counters follow tree order over the boxes of elements and pseudo-elements (css-lists-3/#creating-counters), and so
// does the quote depth (css-content-3/#quote-values), so the pass runs once a restyle has decided which nodes have
// boxes. A participant is a node with a box that resets, sets or increments a counter, which every list item does, or a
// pseudo-element whose content reads a counter or steps the quote depth, which every marker and every quote does. Each
// element whose pseudo-elements or descendants hold a participant carries CssPresentationEntry::countsBelow, so a pass
// walks only those subtrees.

#pragma once

#include "css/src/css_presentation.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the computed display of `style` makes a list item (css-display-3/#list-items).
bool cssIsListItem(const CssComputedStyle* style);

// Returns true if an element with a box, styled by `style`, has style containment (css-contain-2/#containment-style):
// from contain, from a content-visibility other than visible, or from a container-type that sizes the element
// (css-conditional-5/#container-type). counter-increment and counter-set inside its subtree do not reach the counters
// from outside it.
bool cssContainsStyle(const CssComputedStyle* style);

// Returns true if an element that generates a box, styled by `style`, is a participant. `listItem` says whether the
// style makes a list item (cssIsListItem).
bool cssCountsElement(const PrCssPresentation* presentation, const CssComputedStyle* style, bool listItem);

// Returns true if a pseudo-element `pseudo` that generates a box, styled by `style`, is a participant.
bool cssCountsPseudo(const PrCssPresentation* presentation, const CssComputedStyle* style, CssPseudo pseudo);

// Returns true if the content of a pseudo-element styled by `style` steps the quote depth, so that its texts depend on
// the quotes of its own style and its ancestors' and on their languages.
bool cssReadsQuotes(const CssComputedStyle* style);

// Marks the ancestors of the participant `node` as holding one, so that the next pass walks to it.
void cssCountsBelow(PrCssPresentation* presentation, const PrDomNode* node);

// Notes that the participants changed at `element`, an element with an entry, so that the next pass runs and may start
// where the change follows the last pass (CssCountingResume). A change at no such element, such as a removal, is passed
// as null, and the next pass starts at the root.
void cssCountersChanged(PrCssPresentation* presentation, const PrDomNode* PR_NULLABLE element);

// Frees what the resume state holds.
void cssCounterResumeReset(CssCountingResume& resume);

// Which nodes a pass walks.
enum {
    CssCountingWalk_Participants,    // the subtrees that hold a participant, whose flags the pass brings up to date
    CssCountingWalk_Every,           // every node with a box, without reading or changing a flag, as a check does
};
typedef uint8_t CssCountingWalk;

// Called with each pseudo-element whose content reads counters or quotes, in tree order, and the texts that they give
// it: one for each counter(), counters(), open-quote and close-quote, in order. The texts are valid only during the
// call.
typedef void (*CssCountingTextsFn)(void* PR_NULLABLE context, CssPresentationPseudo* pseudo,
                                   const CssCountingTexts* texts);

// Computes the counters of the presentation's boxes in tree order, as the last restyle left them, and calls `fn` with
// the texts of each pseudo-element whose content reads them. With CssCountingWalk_Participants, `fn` is not called for
// a marker whose marker string the pass made for the same list-item value, unless a restyle of the marker cleared it
// since (CssPresentationPseudo::markerKept). Such a run starts at the deepest point of the last run (CssCountingResume)
// that every change it was told of follows, and calls `fn` only for what follows that point.
void cssCountersRun(PrCssPresentation* presentation, CssCountingWalk walk, CssCountingTextsFn fn,
                    void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
