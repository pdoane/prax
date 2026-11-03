// Slots
//
// dom - https://dom.spec.whatwg.org/#shadow-tree-slots

#pragma once

#include "pr/dom.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state of a slot (dom/#concept-slot), which holds its assigned nodes. The functions below keep them current. If
// the slot's shadow root assigns manually, the slot also lists the nodes that a script assigned to it, without holding
// references to them.
struct DomSlot {
    RawArray<PrDomNode* PR_NONNULL> assigned;   // dom/#slot-assigned-nodes
    RawArray<PrDomNode* PR_NONNULL> manual;     // html/#manually-assigned-nodes
};

// The link from a queued slotchange microtask to the document whose slots the microtask fires at. The document clears
// `document` when it is destroyed.
struct DomSlotsToken {
    PrDomDocument* PR_NULLABLE document;
};

// A slottable and the slot whose assigned nodes include it.
struct DomSlotted {
    PrDomNode* node;
    PrDomElement* slot;
};

// The state that a document keeps about its slots once it has a shadow root. Before that, none of its nodes is slotted,
// and the mutation algorithms skip the slot steps.
struct DomSlots {
    RawArray<PrDomElement* PR_NONNULL> signals;   // dom/#signal-slot-list, in the order signaled
    RawArray<PrDomElement* PR_NONNULL> manual;    // slots with manually assigned nodes
    DomSlotsToken* PR_NULLABLE token;             // link of the microtask queued to fire slotchange at the slots

    // The assigned slot of each node that has one, which the presentation reads for every node it walks or marks. A
    // node is listed while it has DomNodeFlag_Slotted.
    RawArray<DomSlotted> slotted;
    HashIndex slottedIndex;                    // `slotted` by prHashPointer of the node

    // The document has a shadow root whose sheets style its tree alone, which a rule set tests scopes for. A use
    // element's shadow tree matches the rules of its host's tree (dom_use.h), and makes no scope.
    bool scopes;
};

// Returns true if the node is a slot, which is an HTML slot element (dom/#concept-slot).
bool domIsSlot(const PrDomNode* node);

// Starts keeping the document's slots. Called when the document gets its first shadow root.
void domSlotsEnable(PrDomDocument* document);

// Frees the state that the document keeps about its slots.
void domSlotsDestroy(PrDomDocument* document);

// Returns the slot that the shadow root of the parent of `slottable` assigns it to (dom/#find-a-slot). If `open` is
// set, returns null for a closed shadow root.
PrDomElement* PR_NULLABLE domFindSlot(const PrDomNode* slottable, bool open);

// Returns the slot whose assigned nodes include `slottable` (dom/#slotable-assigned-slot), or null if there is none.
PrDomElement* PR_NULLABLE domAssignedSlot(const PrDomNode* slottable);

// The slot's assigned nodes (dom/#slot-assigned-nodes), unflattened, valid until its assignment changes.
uint32_t domSlotAssignedCount(const PrDomElement* slot);
PrDomNode* const PR_NONNULL* PR_NULLABLE domSlotAssigned(const PrDomElement* slot);

// Appends the slot's assigned nodes (dom/#slot-assigned-nodes) to `out`, flattened if `flatten` is set
// (dom/#find-flattened-slotables).
void domSlotAssignedNodes(const PrDomElement* slot, bool flatten, RawArray<PrDomNode* PR_NONNULL>* out);

// Returns true if the slot's assigned nodes flatten to at least one node (css-shadow-1/#selectordef-has-slotted).
bool domSlotHasSlotted(const PrDomElement* slot);

// Sets the slot's manually assigned nodes to `nodes` (html/#dom-slot-assign). Each node is removed from the assigned
// nodes of any other slot.
void domSlotAssign(PrDomElement* slot, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count);

// Each function runs the slot steps of a mutation algorithm, if the document has a shadow root. domSlotsInserted runs
// the steps of dom/#concept-node-insert once `node` is a child of `parent`. domSlotsRemoved runs those of
// dom/#concept-node-remove once it no longer is. domSlotsAttributeChanged runs those for a change of the slot or name
// attribute of `element` (dom/#slotable-name, dom/#slot-name). `assigned` is the slot that `node` was assigned to.
void domSlotsInserted(PrDomNode* parent, PrDomNode* node);
void domSlotsRemoved(PrDomNode* parent, PrDomNode* node, PrDomElement* PR_NULLABLE assigned);
void domSlotsAttributeChanged(PrDomElement* element, PrStringView name);

// Moves `node`, if it is a slot, from the slot lists of its document to those of `document`, when the node is adopted
// into `document`.
void domSlotsTransfer(PrDomDocument* document, PrDomNode* node);

// Forgets a node that the document destroys. A slot may list the node as manually assigned, or the node may be a slot.
void domSlotsForget(PrDomNode* node);

// Fires a slotchange event at each of the document's slots signaled since the last call
// (dom/#notify-mutation-observers).
void domSlotsFire(PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
