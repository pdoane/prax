// Traversal
//
// dom - https://dom.spec.whatwg.org/#traversal
//
// The state of traversals, and the steps that keep node iterators valid as their documents change.

#pragma once

#include "pr/dom_traversal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state that iterators and walkers share.
struct DomTraversal {
    PrDomNode* root;
    uint32_t whatToShow;
    bool hasFilter;
    bool active;                        // true while the filter runs
    PrDomNodeFilter filter;

    // The node most recently passed to the filter. This reference keeps the node alive if the filter removes it and
    // drops every other reference, because the move still returns it.
    PrDomNode* PR_NULLABLE held;
};

// A position between two nodes among the inclusive descendants of the root (dom/#interface-nodeiterator).
struct DomNodeIterator : DomTraversal {
    PrDomNode* reference;
    bool pointerBeforeReference;

    // The candidate reference while a filter runs, or null (dom/#concept-nodeiterator-traverse). A removal adjusts it
    // as it adjusts the reference.
    PrDomNode* PR_NULLABLE candidate;
    bool candidateBefore;

    // The links in the document's list of iterators.
    PrDomNodeIterator* PR_NULLABLE previous;
    PrDomNodeIterator* PR_NULLABLE next;
};

// A walk over the inclusive descendants of the root, which moves from a current node (dom/#interface-treewalker).
struct DomTreeWalker : DomTraversal {
    PrDomNode* current;
};

// Runs the iterators' pre-remove steps (dom/#nodeiterator-pre-removing-steps) before `node` is removed from its parent.
void domNodeIteratorsRemoving(PrDomNode* node);

// Moves each iterator whose root is in the subtree of `node` from the list of `from` to the list of the node's
// document.
void domNodeIteratorsAdopted(PrDomDocument* from, PrDomNode* node);

} // namespace pr_impl

// C opaque types
struct PrDomNodeIterator : pr_impl::DomNodeIterator {};
struct PrDomTreeWalker : pr_impl::DomTreeWalker {};

PR_ASSUME_NONNULL_END
