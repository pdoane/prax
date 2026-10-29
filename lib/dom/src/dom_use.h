// The shadow trees of SVG's use elements (SVG2/struct.html#UseShadowTree): a closed shadow root that each use element
// hosts, holding instances of the element that its href references.
//
// A document keeps its use elements and what they reference. A mutation notes the uses that it may reach, and the next
// synchronization brings their shadow trees up to date, before a restyle or before a document that has no presentation
// is drawn. An instance keeps its identity while it matches its original, so the original's changes reach it as changes
// to an element, which transitions and animations follow.

#pragma once

#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomUses;

// Returns true if `root` is the shadow root of a use element, or if `node` is in such a shadow tree, at any depth.
bool domIsUseShadowRoot(const PrDomNode* root);
bool domInUseShadowTree(const PrDomNode* node);

// Returns the original of `node`, an instance in the shadow tree of a use element: the node of the referenced subtree
// that it corresponds to (SVG2/struct.html#UseShadowTree). Returns null for a node that is in no use's shadow tree, and
// for one whose use no longer references what it was instanced from.
PrDomNode* PR_NULLABLE domNodeUseOriginal(const PrDomNode* node);

// Notes that a use element was connected or disconnected. A use in another use's shadow tree is synchronized with its
// host, and is not a use of its own.
void domUseConnectionChanged(PrDomElement* element);

// Note the mutations of a document that may reach its uses: an attribute of `element` in `space` named `name`, a child
// inserted into or removed from `parent`, and the data of `node`. Each does nothing in a document without uses, and for
// the mutations of a synchronization.
void domUsesAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name);
void domUsesChildrenChanged(PrDomNode* parent, const PrDomNode* child);
void domUsesDataChanged(PrDomNode* node);

// Note that an element with the ID `id` joined or left the tree whose root is `root`, and that the subtree of `node`
// moved within its tree, which may change which of the elements with an ID is first. A use's href names the first.
void domUsesIdChanged(PrDomNode* root, PrStringView id);
void domUsesMoved(PrDomNode* node);

// Notes that what every use of the document references may have changed, such as when a document that a use references
// loads.
void domUsesResolveAll(PrDomDocument* document);

// Brings the shadow trees of the document's stale uses up to date with what they reference.
void domUsesSync(PrDomDocument* document);

// Frees what the document keeps for its uses.
void domUsesDestroy(DomUses* PR_NULLABLE uses);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
