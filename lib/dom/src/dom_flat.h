// The flat tree
//
// css-shadow-1 - https://drafts.csswg.org/css-shadow-1/#flattening

#pragma once

#include "pr/array.h"
#include "pr/dom.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The children of a node in the flat tree (css-shadow-1/#flattening), which a presentation lays out. A shadow host's
// are its shadow root's children, a slot's are its assigned nodes, or its children if it has none, and any other node's
// are its children. A slot assigned to a slot is a child of that slot, with its own assigned nodes as its children. The
// iterator reads the siblings or the slot's assigned nodes, which must not change while it is used.
struct DomFlatChildren {
    PrDomNode* PR_NULLABLE next;                    // the next child, if the children are siblings
    PrDomNode* const PR_NONNULL* PR_NULLABLE list;  // the slot's assigned nodes, if the children are those
    uint32_t count;
    uint32_t at;
};

// The flat tree of a document that has had a shadow root, which DomDocument::slots marks, apart from its light tree.
DomFlatChildren domFlatChildrenShadowed(const PrDomNode* node);
PrDomNode* PR_NULLABLE domFlatParentShadowed(const PrDomNode* node);

// Starts an iteration over the children of `node` in the flat tree. A document that has never had a shadow root has the
// light tree as its flat tree, which costs nothing more to walk.
PR_FORCE_INLINE DomFlatChildren domFlatChildren(const PrDomNode* node) {
    if (node->document->slots == nullptr)
        return DomFlatChildren{node->first, nullptr, 0, 0};
    return domFlatChildrenShadowed(node);
}

// Returns the next child, or null after the last.
PR_FORCE_INLINE PrDomNode* PR_NULLABLE domFlatNext(DomFlatChildren& children) {
    if (children.list != nullptr)
        return children.at < children.count ? children.list[children.at++] : nullptr;
    PrDomNode* child = children.next;
    if (child != nullptr)
        children.next = child->next;
    return child;
}

// Calls `visit` with each child of `node` in the flat tree, in order. The loop over a light tree's children is the loop
// over `next` that a walk of the light tree makes, if `visit` is inlined (PR_FORCE_INLINE_LAMBDA).
template <typename Visit>
PR_FORCE_INLINE void domFlatEach(const PrDomNode* node, Visit visit) {
    DomFlatChildren children = domFlatChildren(node);
    if (children.list == nullptr) {
        for (PrDomNode* child = children.next; child != nullptr; child = child->next)
            visit(child);
        return;
    }
    for (uint32_t i = 0; i < children.count; ++i)
        visit(children.list[i]);
}

// Starts an iteration over the children of `parent` in the flat tree that come after its child `child`. Costs the
// number of children before `child` if they are a slot's assigned nodes.
DomFlatChildren domFlatChildrenAfter(const PrDomNode* parent, const PrDomNode* child);

// Returns true if `node` has a child in the flat tree.
inline bool domFlatHasChildren(const PrDomNode* node) {
    DomFlatChildren children = domFlatChildren(node);
    return children.list != nullptr ? children.count != 0 : children.next != nullptr;
}

// Returns the parent of `node` in the flat tree: its assigned slot, or the host of a shadow root that is its parent, or
// its parent. A presentation's pseudo-element has its originating element as its parent. Returns null for a node
// outside the flat tree, a child of a shadow host that no slot takes or a slot's child that its slot's assigned nodes
// replace, and for a root.
PR_FORCE_INLINE PrDomNode* PR_NULLABLE domFlatParent(const PrDomNode* node) {
    if (node->document->slots == nullptr)
        return node->parent;
    return domFlatParentShadowed(node);
}

// Returns true if an element is an outermost svg element: an svg element whose flat parent is not an element of SVG
// content, which is an element in the SVG namespace other than a foreignObject (SVG2/struct.html#SVGElement). The flat
// parent puts an svg that a use element instances inside the use (SVG2/struct.html#UseShadowTree). CSS lays it out, and
// lib/svg draws its content.
inline bool domIsOuterSvg(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Svg || element->tag != PrDomTag_Svg)
        return false;
    const PrDomNode* flat = domFlatParent(element);
    const PrDomElement* parent = flat != nullptr ? domElementFromNode(flat) : nullptr;
    return parent == nullptr || parent->space != PrDomNamespace_Svg || parent->tag == PrDomTag_ForeignObject;
}

// Returns the node before `node` in flat tree order: the last flat descendant of its previous flat sibling, or else its
// flat parent. Returns null for a node without a flat parent.
PrDomNode* PR_NULLABLE domFlatPrevious(const PrDomNode* node);

// Returns the node after `node` and its flat descendants in flat tree order: its next flat sibling, or else that of its
// nearest flat ancestor that has one. Returns null if there is none.
PrDomNode* PR_NULLABLE domFlatFollowing(const PrDomNode* node);

// A walk of the descendants of a node in the flat tree, in tree order.
struct DomFlatWalk {
    Array<DomFlatChildren> open;    // the children of each node on the path from the root, still to be walked
};

// The walk of a document that has had a shadow root, apart from its light tree.
PrDomNode* PR_NULLABLE domFlatWalkNextShadowed(DomFlatWalk& walk, const PrDomNode* root, const PrDomNode* node, bool skip);

// Returns the next descendant of `root` after `node`, which is `root` itself or a descendant that the walk returned
// last, in tree order. The descendants of `node` are skipped if `skip` is true. Returns null after the last.
PR_FORCE_INLINE PrDomNode* PR_NULLABLE domFlatWalkNext(DomFlatWalk& walk, const PrDomNode* root,
        const PrDomNode* node, bool skip = false) {
    if (root->document->slots == nullptr)
        return const_cast<PrDomNode*>(skip ? domNodeNextSkipping(root, node) : domNodeNext(root, node));
    return domFlatWalkNextShadowed(walk, root, node, skip);
}

// Frees what the walk holds.
inline void domFlatWalkReset(DomFlatWalk& walk) {
    walk.open.reset();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
