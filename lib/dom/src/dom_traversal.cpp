// Traversal: NodeFilter, NodeIterator and TreeWalker (dom/#traversal).

#include "dom/src/dom_traversal.h"

#include "pr/alloc.h"

#include "dom/src/dom_node.h"
#include "dom/src/dom_range.h"

namespace pr_impl {

namespace {

// These report no tree links for an attribute, whose links hold its element and its neighbors in the attribute list.
PrDomNode* PR_NULLABLE firstChild(PrDomNode* node) {
    return node->type == PrDomNodeType_Attribute ? nullptr : node->first;
}

PrDomNode* PR_NULLABLE lastChild(PrDomNode* node) {
    return node->type == PrDomNodeType_Attribute ? nullptr : node->last;
}

PrDomNode* PR_NULLABLE nextSibling(PrDomNode* node) {
    return node->type == PrDomNodeType_Attribute ? nullptr : node->next;
}

PrDomNode* PR_NULLABLE previousSibling(PrDomNode* node) {
    return node->type == PrDomNodeType_Attribute ? nullptr : node->previous;
}

// Returns the first node after `node` in tree order that is not its descendant and is in `root`, or null. A filter can
// move `node` out of `root`, so the search also ends at the top of the tree of `node`.
PrDomNode* PR_NULLABLE nextSkipping(PrDomNode* root, PrDomNode* node) {
    for (PrDomNode* at = node; at != nullptr && at != root; at = domNodeParent(at)) {
        if (PrDomNode* sibling = nextSibling(at))
            return sibling;
    }
    return nullptr;
}

PrDomNode* lastInclusiveDescendant(PrDomNode* node) {
    while (PrDomNode* child = lastChild(node))
        node = child;
    return node;
}

// Sets a slot that holds a reference. The new node is retained first, because the old reference can be the last one
// that keeps the tree of the new node alive.
void setHeld(PrDomNode** slot, PrDomNode* node) {
    domNodeRetain(node);
    PrDomNode* previous = *slot;
    *slot = node;
    domNodeRelease(previous);
}

void traversalInit(DomTraversal* traversal, PrDomNode* root, uint32_t whatToShow, const PrDomNodeFilter* PR_NULLABLE filter) {
    domNodeRetain(root);
    traversal->root = root;
    traversal->whatToShow = whatToShow;
    if (filter == nullptr)
        return;
    traversal->hasFilter = true;
    traversal->filter = *filter;
    if (filter->retain != nullptr)
        filter->retain(filter->context);
}

void traversalReset(DomTraversal* traversal) {
    if (traversal->hasFilter && traversal->filter.release != nullptr)
        traversal->filter.release(traversal->filter.context);
    domNodeRelease(traversal->held);
    domNodeRelease(traversal->root);
}

const PrDomNodeFilter* PR_NULLABLE traversalFilter(const DomTraversal* traversal) {
    return traversal->hasFilter ? &traversal->filter : nullptr;
}

// Moves a node pointer out of `removed`, which is about to be removed (dom/#nodeiterator-pre-removing-steps).
void retarget(PrDomNode* root, PrDomNode* removed, PrDomNode** position, bool* pointerBefore) {
    if (!domIsInclusiveAncestor(removed, *position))
        return;
    if (*pointerBefore) {
        if (PrDomNode* following = nextSkipping(root, removed)) {
            setHeld(position, following);
            return;
        }
        *pointerBefore = false;
    }
    PrDomNode* before = previousSibling(removed);
    setHeld(position, before != nullptr ? lastInclusiveDescendant(before) : domNodeParent(removed));
}

// Filters `node` (dom/#concept-node-filter). Returns InvalidState if the filter is already running, and NotAllowed if
// the filter abandons the traversal.
PrStatus filterNode(DomTraversal* traversal, PrDomNode* node, PrDomNodeFilterResult* out) {
    if (traversal->active)
        return PrStatus_InvalidState;
    if (((uint64_t(1) << (uint32_t(node->type) - 1)) & traversal->whatToShow) == 0) {
        *out = PrDomNodeFilterResult_Skip;
        return PrStatus_Ok;
    }
    if (!traversal->hasFilter) {
        *out = PrDomNodeFilterResult_Accept;
        return PrStatus_Ok;
    }
    setHeld(&traversal->held, node);
    traversal->active = true;
    *out = traversal->filter.invoke(traversal->filter.context, node);
    traversal->active = false;
    return *out == PrDomNodeFilterResult_Abandon ? PrStatus_NotAllowed : PrStatus_Ok;
}

// Clears the candidate reference, and returns `status`.
PrStatus candidateEnded(DomNodeIterator* iterator, PrStatus status) {
    domNodeRelease(iterator->candidate);
    iterator->candidate = nullptr;
    return status;
}

// dom/#concept-nodeiterator-traverse
PrStatus iteratorTraverse(DomNodeIterator* iterator, bool forward, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDomNode* node = iterator->reference;
    bool before = iterator->pointerBeforeReference;
    for (;;) {
        if (forward == before) {
            before = !before;
        } else if (forward) {
            PrDomNode* child = firstChild(node);
            node = child != nullptr ? child : nextSkipping(iterator->root, node);
        } else if (node == iterator->root) {
            node = nullptr;
        } else if (PrDomNode* sibling = previousSibling(node)) {
            node = lastInclusiveDescendant(sibling);
        } else {
            node = domNodeParent(node);
        }
        if (node == nullptr)
            return candidateEnded(iterator, PrStatus_Ok);
        PrDomNode* filtered = node;
        if (iterator->hasFilter) {
            setHeld(&iterator->candidate, node);
            iterator->candidateBefore = before;
        }
        PrDomNodeFilterResult result = 0;
        PrStatus status = filterNode(iterator, node, &result);
        if (status != PrStatus_Ok)
            return candidateEnded(iterator, status);
        if (iterator->hasFilter) {
            node = iterator->candidate;
            before = iterator->candidateBefore;
        }
        if (result == PrDomNodeFilterResult_Accept) {
            setHeld(&iterator->reference, node);
            iterator->pointerBeforeReference = before;
            *out = filtered;
            return candidateEnded(iterator, PrStatus_Ok);
        }
    }
}

PrStatus walkerArrive(DomTreeWalker* walker, PrDomNode* node, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    setHeld(&walker->current, node);
    *out = node;
    return PrStatus_Ok;
}

// dom/#dom-treewalker-parentnode
PrStatus walkerParent(DomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* node = walker->current;
    while (node != nullptr && node != walker->root) {
        node = domNodeParent(node);
        if (node == nullptr)
            break;
        PrDomNodeFilterResult result = 0;
        PrStatus status = filterNode(walker, node, &result);
        if (status != PrStatus_Ok)
            return status;
        if (result == PrDomNodeFilterResult_Accept)
            return walkerArrive(walker, node, out);
    }
    return PrStatus_Ok;
}

// dom/#concept-traverse-children
PrStatus walkerChildren(DomTreeWalker* walker, bool first, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* node = first ? firstChild(walker->current) : lastChild(walker->current);
    while (node != nullptr) {
        PrDomNodeFilterResult result = 0;
        PrStatus status = filterNode(walker, node, &result);
        if (status != PrStatus_Ok)
            return status;
        if (result == PrDomNodeFilterResult_Accept)
            return walkerArrive(walker, node, out);
        if (result == PrDomNodeFilterResult_Skip) {
            PrDomNode* child = first ? firstChild(node) : lastChild(node);
            if (child != nullptr) {
                node = child;
                continue;
            }
        }
        for (;;) {
            PrDomNode* sibling = first ? nextSibling(node) : previousSibling(node);
            if (sibling != nullptr) {
                node = sibling;
                break;
            }
            PrDomNode* parent = domNodeParent(node);
            if (parent == nullptr || parent == walker->root || parent == walker->current)
                return PrStatus_Ok;
            node = parent;
        }
    }
    return PrStatus_Ok;
}

// dom/#concept-traverse-siblings
PrStatus walkerSiblings(DomTreeWalker* walker, bool next, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* node = walker->current;
    if (node == walker->root)
        return PrStatus_Ok;
    for (;;) {
        PrDomNode* sibling = next ? nextSibling(node) : previousSibling(node);
        while (sibling != nullptr) {
            node = sibling;
            PrDomNodeFilterResult result = 0;
            PrStatus status = filterNode(walker, node, &result);
            if (status != PrStatus_Ok)
                return status;
            if (result == PrDomNodeFilterResult_Accept)
                return walkerArrive(walker, node, out);
            sibling = next ? firstChild(node) : lastChild(node);
            if (result == PrDomNodeFilterResult_Reject || sibling == nullptr)
                sibling = next ? nextSibling(node) : previousSibling(node);
        }
        node = domNodeParent(node);
        if (node == nullptr || node == walker->root)
            return PrStatus_Ok;
        PrDomNodeFilterResult result = 0;
        PrStatus status = filterNode(walker, node, &result);
        if (status != PrStatus_Ok)
            return status;
        if (result == PrDomNodeFilterResult_Accept)
            return PrStatus_Ok;
    }
}

// dom/#dom-treewalker-previousnode
PrStatus walkerPrevious(DomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* node = walker->current;
    while (node != walker->root) {
        PrDomNode* sibling = previousSibling(node);
        while (sibling != nullptr) {
            node = sibling;
            PrDomNodeFilterResult result = 0;
            PrStatus status = filterNode(walker, node, &result);
            if (status != PrStatus_Ok)
                return status;
            while (result != PrDomNodeFilterResult_Reject && lastChild(node) != nullptr) {
                node = lastChild(node);
                status = filterNode(walker, node, &result);
                if (status != PrStatus_Ok)
                    return status;
            }
            if (result == PrDomNodeFilterResult_Accept)
                return walkerArrive(walker, node, out);
            sibling = previousSibling(node);
        }
        if (node == walker->root || domNodeParent(node) == nullptr)
            return PrStatus_Ok;
        node = domNodeParent(node);
        PrDomNodeFilterResult result = 0;
        PrStatus status = filterNode(walker, node, &result);
        if (status != PrStatus_Ok)
            return status;
        if (result == PrDomNodeFilterResult_Accept)
            return walkerArrive(walker, node, out);
    }
    return PrStatus_Ok;
}

// dom/#dom-treewalker-nextnode
PrStatus walkerNext(DomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* node = walker->current;
    PrDomNodeFilterResult result = PrDomNodeFilterResult_Accept;
    for (;;) {
        while (result != PrDomNodeFilterResult_Reject && firstChild(node) != nullptr) {
            node = firstChild(node);
            PrStatus status = filterNode(walker, node, &result);
            if (status != PrStatus_Ok)
                return status;
            if (result == PrDomNodeFilterResult_Accept)
                return walkerArrive(walker, node, out);
        }
        // The current node can be outside the root, and then the walk ends at the top of its tree.
        node = nextSkipping(walker->root, node);
        if (node == nullptr)
            return PrStatus_Ok;
        PrStatus status = filterNode(walker, node, &result);
        if (status != PrStatus_Ok)
            return status;
        if (result == PrDomNodeFilterResult_Accept)
            return walkerArrive(walker, node, out);
    }
}

void link(PrDomNodeIterator* iterator, PrDomDocument* document) {
    iterator->previous = nullptr;
    iterator->next = document->iterators;
    if (iterator->next != nullptr)
        iterator->next->previous = iterator;
    document->iterators = iterator;
}

void unlink(PrDomNodeIterator* iterator, PrDomDocument* document) {
    if (iterator->previous != nullptr)
        iterator->previous->next = iterator->next;
    else
        document->iterators = iterator->next;
    if (iterator->next != nullptr)
        iterator->next->previous = iterator->previous;
}

} // namespace

void domNodeIteratorsRemoving(PrDomNode* node) {
    for (PrDomNodeIterator* iterator = node->document->iterators; iterator != nullptr; iterator = iterator->next) {
        // Removing the root or an ancestor of it leaves the root's descendants in place.
        if (domIsInclusiveAncestor(node, iterator->root))
            continue;
        retarget(iterator->root, node, &iterator->reference, &iterator->pointerBeforeReference);
        if (iterator->candidate != nullptr)
            retarget(iterator->root, node, &iterator->candidate, &iterator->candidateBefore);
    }
}

void domNodeIteratorsAdopted(PrDomDocument* from, PrDomNode* node) {
    for (PrDomNodeIterator* iterator = from->iterators; iterator != nullptr;) {
        PrDomNodeIterator* next = iterator->next;
        if (domIsInclusiveAncestor(node, iterator->root)) {
            unlink(iterator, from);
            link(iterator, node->document);
        }
        iterator = next;
    }
}

} // namespace pr_impl

using namespace pr_impl;

PrDomNodeIterator* prDomNodeIteratorCreate(PrDomNode* root, uint32_t whatToShow, const PrDomNodeFilter* PR_NULLABLE filter) {
    PrDomNodeIterator* iterator = allocZeroed<PrDomNodeIterator>();
    traversalInit(iterator, root, whatToShow, filter);
    domNodeRetain(root);
    iterator->reference = root;
    iterator->pointerBeforeReference = true;
    link(iterator, root->document);
    return iterator;
}

void prDomNodeIteratorDestroy(PrDomNodeIterator* PR_NULLABLE iterator) {
    if (iterator == nullptr)
        return;
    unlink(iterator, iterator->root->document);
    domNodeRelease(iterator->reference);
    traversalReset(iterator);
    prMemoryFree(iterator);
}

PrDomNode* prDomNodeIteratorRoot(PrDomNodeIterator* iterator) {
    return iterator->root;
}

PrDomNode* prDomNodeIteratorReferenceNode(PrDomNodeIterator* iterator) {
    return iterator->reference;
}

bool prDomNodeIteratorPointerBeforeReferenceNode(const PrDomNodeIterator* iterator) {
    return iterator->pointerBeforeReference;
}

uint32_t prDomNodeIteratorWhatToShow(const PrDomNodeIterator* iterator) {
    return iterator->whatToShow;
}

const PrDomNodeFilter* PR_NULLABLE prDomNodeIteratorFilter(const PrDomNodeIterator* iterator) {
    return traversalFilter(iterator);
}

PrStatus prDomNodeIteratorNextNode(PrDomNodeIterator* iterator, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    return iteratorTraverse(iterator, true, out);
}

PrStatus prDomNodeIteratorPreviousNode(PrDomNodeIterator* iterator, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    return iteratorTraverse(iterator, false, out);
}

PrDomTreeWalker* prDomTreeWalkerCreate(PrDomNode* root, uint32_t whatToShow, const PrDomNodeFilter* PR_NULLABLE filter) {
    PrDomTreeWalker* walker = allocZeroed<PrDomTreeWalker>();
    traversalInit(walker, root, whatToShow, filter);
    domNodeRetain(root);
    walker->current = root;
    return walker;
}

void prDomTreeWalkerDestroy(PrDomTreeWalker* PR_NULLABLE walker) {
    if (walker == nullptr)
        return;
    domNodeRelease(walker->current);
    traversalReset(walker);
    prMemoryFree(walker);
}

PrDomNode* prDomTreeWalkerRoot(PrDomTreeWalker* walker) {
    return walker->root;
}

uint32_t prDomTreeWalkerWhatToShow(const PrDomTreeWalker* walker) {
    return walker->whatToShow;
}

const PrDomNodeFilter* PR_NULLABLE prDomTreeWalkerFilter(const PrDomTreeWalker* walker) {
    return traversalFilter(walker);
}

PrDomNode* prDomTreeWalkerCurrentNode(PrDomTreeWalker* walker) {
    return walker->current;
}

void prDomTreeWalkerSetCurrentNode(PrDomTreeWalker* walker, PrDomNode* node) {
    setHeld(&walker->current, node);
}

PrStatus prDomTreeWalkerParentNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerParent(walker, out);
}

PrStatus prDomTreeWalkerFirstChild(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerChildren(walker, true, out);
}

PrStatus prDomTreeWalkerLastChild(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerChildren(walker, false, out);
}

PrStatus prDomTreeWalkerPreviousSibling(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerSiblings(walker, false, out);
}

PrStatus prDomTreeWalkerNextSibling(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerSiblings(walker, true, out);
}

PrStatus prDomTreeWalkerPreviousNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerPrevious(walker, out);
}

PrStatus prDomTreeWalkerNextNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    return walkerNext(walker, out);
}
