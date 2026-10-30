// Selection
//
// selection-api - https://w3c.github.io/selection-api/
//
// A document's selection, and the selectionchange events that its changes schedule.

#pragma once

#include "pr/dom.h"

#include "dom/src/dom_range.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A document's selection, which has a range and a direction. The range is a live range of the document, which scripts
// can also reach and change through the Range API. The range holds a reference to the document, so the selection's
// reference to the range is weak. The code that made the range the selection's range keeps it alive.
struct DomSelection {
    PrDomDocument* document;
    WeakReference range;                      // a DomRange, or null for none
    PrDomSelectionDirection direction;
    bool scheduled;                           // selection-api/#dfn-has-scheduled-selectionchange-event
};

// Returns the document's selection, creating it on first access.
PrDomSelection* domDocumentSelection(PrDomDocument* document);

// Frees the selection. Its range is left as it is.
void domSelectionDestroy(PrDomSelection* PR_NULLABLE selection);

// Returns the selection's range, or null if it has none.
PrDomRange* PR_NULLABLE domSelectionRange(const PrDomSelection* selection);

// Makes `range`, which may be null, the selection's range. Schedules a selectionchange event if the range changes
// (selection-api/#scheduling-selectionchange-event).
void domSelectionSetRange(PrDomSelection* selection, PrDomRange* PR_NULLABLE range, PrDomSelectionDirection direction);

// Reports a change that a script made to the range. If the range is the range of its document's selection, schedules a
// selectionchange event.
void domSelectionRangeChanged(PrDomRange* range);

// Write the selection's anchor or focus (selection-api/#dfn-anchor). Return false, and write nothing, unless the
// selection has a range whose boundary points are in the document tree.
bool domSelectionAnchor(const PrDomSelection* selection, DomBoundary* out);
bool domSelectionFocus(const PrDomSelection* selection, DomBoundary* out);

} // namespace pr_impl

// C opaque types
struct PrDomSelection : pr_impl::DomSelection {};

PR_ASSUME_NONNULL_END
