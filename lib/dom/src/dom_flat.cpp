#include "dom/src/dom_flat.h"

#include "dom/src/dom_slot.h"

namespace pr_impl {

DomFlatChildren domFlatChildrenShadowed(const PrDomNode* node) {
    DomFlatChildren children{};
    const PrDomElement* element = domElementFromNode(node);
    if (element != nullptr) {
        if (const PrDomShadowRoot* shadow = domShadowRootOfHost(element)) {
            children.next = shadow->first;
            return children;
        }
        if (uint32_t count = domIsSlot(element) ? domSlotAssignedCount(element) : 0) {
            children.list = domSlotAssigned(element);
            children.count = count;
            return children;
        }
    }
    children.next = node->first;
    return children;
}

PrDomNode* PR_NULLABLE domFlatParentShadowed(const PrDomNode* node) {
    if (node->type == DomTargetType_PseudoElement)
        return node->parent;
    if ((node->flags & DomNodeFlag_Slotted) != 0)
        return domAssignedSlot(node);
    PrDomNode* parent = node->parent;
    if (parent == nullptr)
        return nullptr;
    if ((parent->flags & DomNodeFlag_ShadowRoot) != 0)
        return static_cast<PrDomShadowRoot*>(parent)->host;
    const PrDomElement* element = domElementFromNode(parent);
    if (element != nullptr && (domShadowRootOfHost(element) != nullptr ||
            (domIsSlot(element) && domSlotAssignedCount(element) != 0)))
        return nullptr;
    return parent;
}

DomFlatChildren domFlatChildrenAfter(const PrDomNode* parent, const PrDomNode* child) {
    DomFlatChildren children = domFlatChildren(parent);
    if (children.list == nullptr) {
        children.next = child->next;
        return children;
    }
    while (children.at < children.count && children.list[children.at] != child)
        ++children.at;
    children.at = children.at < children.count ? children.at + 1 : children.count;
    return children;
}

PrDomNode* PR_NULLABLE domFlatPrevious(const PrDomNode* node) {
    PrDomNode* parent = domFlatParent(node);
    if (parent == nullptr)
        return nullptr;
    PrDomNode* previous = nullptr;
    DomFlatChildren children = domFlatChildren(parent);
    for (PrDomNode* child = domFlatNext(children); child != nullptr && child != node; child = domFlatNext(children))
        previous = child;
    if (previous == nullptr)
        return parent;
    for (;;) {
        PrDomNode* last = nullptr;
        domFlatEach(previous, [&](PrDomNode* child) { last = child; });
        if (last == nullptr)
            return previous;
        previous = last;
    }
}

PrDomNode* PR_NULLABLE domFlatFollowing(const PrDomNode* node) {
    for (const PrDomNode* at = node; at != nullptr;) {
        PrDomNode* parent = domFlatParent(at);
        if (parent == nullptr)
            return nullptr;
        DomFlatChildren after = domFlatChildrenAfter(parent, at);
        if (PrDomNode* next = domFlatNext(after))
            return next;
        at = parent;
    }
    return nullptr;
}

PrDomNode* PR_NULLABLE domFlatWalkNextShadowed(DomFlatWalk& walk, const PrDomNode* root, const PrDomNode* node, bool skip) {
    if (node == root)
        walk.open.clear();
    if (!skip)
        walk.open.push(domFlatChildren(node));
    while (walk.open.count != 0) {
        if (PrDomNode* next = domFlatNext(walk.open.last()))
            return next;
        walk.open.pop();
    }
    return nullptr;
}

} // namespace pr_impl
