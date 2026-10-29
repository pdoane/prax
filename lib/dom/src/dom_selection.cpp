#include "dom/src/dom_selection.h"

#include "pr/alloc.h"

namespace pr_impl {

namespace {

// selection-api/#firing-selectionchange-event, at the document. The task held a reference to the document.
void fireSelectionChange(void* data) {
    PrDomDocument* document = static_cast<PrDomDocument*>(data);
    document->selection->scheduled = false;
    domEventFire(document, "selectionchange");
    domNodeRelease(document);
}

void schedule(PrDomSelection* selection) {
    PrDomDocument* document = selection->document;
    const DomDocumentHost* host = document->host;
    if (selection->scheduled || host == nullptr)
        return;
    selection->scheduled = true;
    domNodeRetain(document);
    host->queueTask(host->context, fireSelectionChange, document,
                    [](void* data) { domNodeRelease(static_cast<PrDomDocument*>(data)); });
}

// Returns the selection's range if both its boundary points are in the document tree (dom/#in-a-document-tree), or null
// otherwise.
const PrDomRange* PR_NULLABLE rangeInTree(const PrDomSelection* selection) {
    const PrDomRange* range = domSelectionRange(selection);
    if (range == nullptr || domNodeRoot(range->start.node) != selection->document
        || domNodeRoot(range->end.node) != selection->document)
        return nullptr;
    return range;
}

} // namespace

PrDomSelection* domDocumentSelection(PrDomDocument* document) {
    if (document->selection == nullptr) {
        document->selection = allocZeroed<PrDomSelection>();
        document->selection->document = document;
    }
    return document->selection;
}

void domSelectionDestroy(PrDomSelection* PR_NULLABLE selection) {
    if (selection == nullptr)
        return;
    weakReset(&selection->range);
    prMemoryFree(selection);
}

PrDomRange* PR_NULLABLE domSelectionRange(const PrDomSelection* selection) {
    return weakGet<PrDomRange>(selection->range);
}

void domSelectionSetRange(PrDomSelection* selection, PrDomRange* PR_NULLABLE range,
                          PrDomSelectionDirection direction) {
    bool changed = range != domSelectionRange(selection) || direction != selection->direction;
    weakSet(&selection->range, range != nullptr ? &range->weak : nullptr, range);
    selection->direction = direction;
    if (changed)
        schedule(selection);
}

void domSelectionRangeChanged(PrDomRange* range) {
    PrDomDocument* document = range->document;
    if (document != nullptr && document->selection != nullptr
        && domSelectionRange(document->selection) == range)
        schedule(document->selection);
}

bool domSelectionAnchor(const PrDomSelection* selection, DomBoundary* out) {
    const PrDomRange* range = rangeInTree(selection);
    if (range == nullptr)
        return false;
    *out = selection->direction == PrDomSelectionDirection_Backwards ? range->end : range->start;
    return true;
}

bool domSelectionFocus(const PrDomSelection* selection, DomBoundary* out) {
    const PrDomRange* range = rangeInTree(selection);
    if (range == nullptr)
        return false;
    *out = selection->direction == PrDomSelectionDirection_Backwards ? range->start : range->end;
    return true;
}

} // namespace pr_impl
