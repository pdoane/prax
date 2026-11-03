// Ranges
//
// dom - https://dom.spec.whatwg.org/#ranges
//
// Live and static ranges, and the steps that keep live ranges current as their document changes.

#pragma once

#include "pr/dom.h"
#include "pr/weak.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A boundary point (dom/#concept-range-bp).
struct DomBoundary {
    PrDomNode* node;
    uint32_t offset;
};

// A range, which has a start and an end boundary point. A live range belongs to its node document, whose mutation
// algorithms keep it current. A static range is not kept current. A range holds a reference to the node of each
// boundary point, and a live range also holds a reference to its document.
struct DomRange {
    DomBoundary start;
    DomBoundary end;
    PrDomDocument* PR_NULLABLE document;        // document whose live ranges include it, or null for a static range

    // The neighbors in the document's list of live ranges.
    PrDomRange* PR_NULLABLE previous;
    PrDomRange* PR_NULLABLE next;

    uint32_t references;
    WeakReferences weak;                      // selection whose range this is, if any
};

// Creates a live range collapsed at the start of the document, with one reference that the caller holds.
PrDomRange* domRangeCreate(PrDomDocument* document);

// Creates a static range (dom/#dom-staticrange-staticrange), with one reference that the caller holds. A static range
// needs no validation beyond the types of its nodes.
PrDomRange* domStaticRangeCreate(DomBoundary start, DomBoundary end);

// Adds or releases a reference. The last release frees the range.
void domRangeRetain(PrDomRange* range);
void domRangeRelease(PrDomRange* PR_NULLABLE range);

// Returns the length of the node (dom/#concept-node-length).
uint32_t domNodeLength(const PrDomNode* node);

// Checks a boundary point that a method is given, with the first steps of dom/#dom-range-comparepoint. Returns
// WrongType for a doctype, and OutOfRange for an offset past the node's length.
PrStatus domCheckBoundaryPoint(const PrDomNode* node, uint32_t offset);

// Returns the index of the node among its siblings (dom/#concept-tree-index).
uint32_t domNodeIndex(const PrDomNode* node);

// Returns true if `ancestor` is `node` or an ancestor of it (dom/#concept-tree-inclusive-ancestor).
bool domIsInclusiveAncestor(const PrDomNode* ancestor, const PrDomNode* node);

// Returns the position of `a` relative to `b` (dom/#concept-range-bp-position): -1, 0 or 1 if `a` is before, equal to
// or after `b`. The two must share a root.
int domBoundaryCompare(DomBoundary a, DomBoundary b);

// Returns the root of the range's start node (dom/#concept-range-root).
PrDomNode* domRangeRoot(const PrDomRange* range);

// Returns true if the start and the end are the same boundary point (dom/#range-collapsed).
inline bool domRangeCollapsed(const DomRange* range) {
    return range->start.node == range->end.node && range->start.offset == range->end.offset;
}

// Set the start or the end, and collapse the range to the new boundary point if the start and end would be out of order
// or in different roots (dom/#concept-range-bp-set). Return WrongType for a doctype, and OutOfRange for an offset past
// the node's length.
PrStatus domRangeSetStart(PrDomRange* range, PrDomNode* node, uint32_t offset);
PrStatus domRangeSetEnd(PrDomRange* range, PrDomNode* node, uint32_t offset);

// Sets the range to `node` itself (dom/#concept-range-select). Returns WrongType for a node without a parent.
PrStatus domRangeSelect(PrDomRange* range, PrDomNode* node);

// Sets the range to the contents of `node` (dom/#dom-range-selectnodecontents). Returns WrongType for a doctype.
PrStatus domRangeSelectContents(PrDomRange* range, PrDomNode* node);

// Collapses the range to its start if `toStart` is set, and to its end otherwise.
void domRangeCollapse(PrDomRange* range, bool toStart);

// Return true if the range contains `node` (dom/#contained), or contains it partially (dom/#partially-contained).
bool domRangeContains(const PrDomRange* range, const PrDomNode* node);
bool domRangePartiallyContains(const PrDomRange* range, const PrDomNode* node);

// Creates a fragment of the range's contents, and removes the contents from the tree if `extract` is set
// (dom/#concept-range-extract, dom/#concept-range-clone). The caller releases the fragment's reference, whatever the
// status. Returns NotAllowed if a doctype is contained.
PrStatus domRangeExtract(PrDomRange* range, bool extract, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out);

// Removes the range's contents from the tree (dom/#dom-range-deletecontents).
void domRangeDelete(PrDomRange* range);

// Inserts `node` at the start of the range (dom/#concept-range-insert). Fails in the same cases as pre-insertion, and
// also returns NotAllowed if the start is a comment, a processing instruction, or a text node with no parent.
PrStatus domRangeInsert(PrDomRange* range, PrDomNode* node);

// Moves the range's contents into `parent`, and inserts `parent` in their place (dom/#dom-range-surroundcontents).
// Returns InvalidState if a non-text node is partially contained, and WrongType if `parent` is a document, doctype or
// fragment.
PrStatus domRangeSurround(PrDomRange* range, PrDomNode* parent);

// Returns the text within the range, from the Text nodes that it holds in whole or in part, in tree order
// (dom/#dom-range-stringifier).
PrRawString domRangeString(const PrDomRange* range);

// Run the steps that the mutation algorithms run for the live ranges of the node's document. domRangesInserted runs
// when `node` has been inserted into its parent at its index (dom/#concept-node-insert). domRangesRemoving runs when
// `node` is about to be removed from its parent (dom/#concept-node-remove). domRangesDataReplaced runs when `count`
// code units of the data at `offset` are replaced by `length` code units (dom/#concept-cd-replace). domRangesSplit runs
// when the text node `node` is split at `offset` into `created`, which is inserted after it (dom/#concept-text-split).
void domRangesInserted(PrDomNode* node);
void domRangesRemoving(PrDomNode* node);
void domRangesDataReplaced(PrDomNode* node, uint32_t offset, uint32_t count, uint32_t length);
void domRangesSplit(PrDomNode* node, uint32_t offset, PrDomNode* created);

// Runs the steps of dom/#dom-node-normalize for the live ranges, after the data of `from` has been appended to `into`
// at `offset` and before `from` is removed.
void domRangesMerged(PrDomNode* into, uint32_t offset, PrDomNode* from);

// Moves the live ranges in the node's subtree from the list of its old document to the list of its new document.
// Returns the number of ranges moved. domAdopt moves the references that they hold to the old document along with them.
uint32_t domRangesAdopted(PrDomDocument* from, PrDomNode* node);

} // namespace pr_impl

// C opaque types
struct PrDomRange : pr_impl::DomRange {};

PR_ASSUME_NONNULL_END
