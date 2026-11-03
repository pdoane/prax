#include "dom/src/dom_slot.h"

#include "pr/alloc.h"
#include "pr/hash.h"
#include "pr/string_ops.h"

#include "dom/src/dom_event.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_mutation.h"

namespace pr_impl {
namespace {

bool isSlottable(const PrDomNode* node) {
    return node->type == PrDomNodeType_Element || node->type == PrDomNodeType_Text;
}

// dom/#slotable-name: returns an element's slot attribute, or the empty string for a text node.
PrStringView slottableName(const PrDomNode* slottable) {
    const PrDomElement* element = domElementFromNode(slottable);
    return element != nullptr ? domAttributeValue(element, "slot") : "";
}

DomSlot* PR_NULLABLE slotOf(const PrDomElement* slot) {
    return slot->state != nullptr ? slot->state->slot : nullptr;
}

DomSlot* stateOf(PrDomElement* slot) {
    DomElementState* state = domElementState(slot);
    if (state->slot == nullptr)
        state->slot = allocZeroed<DomSlot>();
    return state->slot;
}

bool holds(const Array<PrDomNode* PR_NONNULL>& nodes, const PrDomNode* node) {
    for (uint32_t i = 0; i < nodes.count; ++i) {
        if (nodes.data[i] == node)
            return true;
    }
    return false;
}

// Returns the shadow root that `node` is in, or null for a node outside a shadow tree.
PrDomShadowRoot* PR_NULLABLE shadowOf(const PrDomNode* node) {
    return domNodeShadowRoot(domNodeRoot(const_cast<PrDomNode*>(node)));
}

// dom/#find-slotables
void findSlottables(const PrDomElement* slot, Array<PrDomNode* PR_NONNULL>* out) {
    PrDomShadowRoot* shadow = shadowOf(slot);
    if (shadow == nullptr)
        return;
    PrDomElement* host = shadow->host;
    if (shadow->slotAssignment == PrDomSlotAssignmentMode_Manual) {
        if (const DomSlot* state = slotOf(slot)) {
            for (uint32_t i = 0; i < state->manual.count; ++i) {
                if (state->manual.data[i]->parent == host)
                    out->push(state->manual.data[i]);
            }
        }
        return;
    }
    for (PrDomNode* child = host->first; child != nullptr; child = child->next) {
        if (isSlottable(child) && domFindSlot(child, false) == slot)
            out->push(child);
    }
}

// dom/#find-flattened-slotables: appends a slot's slottables, or its slottable children if it has none. Each slot among
// them that is in a shadow tree is replaced by its own flattened slottables.
void findFlattened(const PrDomElement* slot, Array<PrDomNode* PR_NONNULL>* out) {
    if (shadowOf(slot) == nullptr)
        return;
    Array<PrDomNode* PR_NONNULL> slottables;
    findSlottables(slot, &slottables);
    if (slottables.count == 0) {
        for (PrDomNode* child = slot->first; child != nullptr; child = child->next) {
            if (isSlottable(child))
                slottables.push(child);
        }
    }
    for (uint32_t i = 0; i < slottables.count; ++i) {
        PrDomNode* node = slottables.data[i];
        if (domIsSlot(node) && shadowOf(node) != nullptr)
            findFlattened(domElementFromNode(node), out);
        else
            out->push(node);
    }
    slottables.reset();
}

void fireSlotChange(void* data) {
    DomSlotsToken* token = static_cast<DomSlotsToken*>(data);
    PrDomDocument* document = token->document;
    prMemoryFree(token);
    if (document == nullptr || document->slots == nullptr)
        return;
    document->slots->token = nullptr;
    // dom/#notify-mutation-observers: the observers take their records before the slots fire.
    domMutationObserversDeliver(document);
    domSlotsFire(document);
}

// dom/#signal-a-slot-change
void signalSlotChange(PrDomElement* slot) {
    PrDomDocument* document = slot->document;
    DomSlots* slots = document->slots;
    for (uint32_t i = 0; i < slots->signals.count; ++i) {
        if (slots->signals.data[i] == slot)
            return;
    }
    slots->signals.push(slot);
    const DomDocumentHost* host = domDocumentMicrotaskHost(document);
    if (slots->token != nullptr || host == nullptr)
        return;
    DomSlotsToken* token = allocZeroed<DomSlotsToken>();
    token->document = document;
    if (host->queueMicrotask(host->context, fireSlotChange, token))
        slots->token = token;
    else
        prMemoryFree(token);
}

// Returns the position of `node` in the document's list of slotted nodes, or kHashMissing.
uint32_t slottedAt(const DomSlots* slots, const PrDomNode* node) {
    return slots->slottedIndex.find(prHashPointer(node, 0), [&](uint32_t i) { return slots->slotted.data[i].node == node; });
}

// Records that `slot` is the assigned slot of `node`.
void setSlotted(DomSlots* slots, PrDomNode* node, PrDomElement* slot) {
    uint32_t at = slottedAt(slots, node);
    if (at != kHashMissing) {
        slots->slotted.data[at].slot = slot;
        return;
    }
    slots->slottedIndex.insert(prHashPointer(node, 0), slots->slotted.count);
    slots->slotted.push(DomSlotted{node, slot});
    node->flags |= DomNodeFlag_Slotted;
}

// Records that `node` has no assigned slot, if `slot` is the one recorded, or any slot for a null `slot`. A node that
// moved to another slot, whose assignment was recorded first, keeps that slot.
void unsetSlotted(DomSlots* slots, PrDomNode* node, const PrDomElement* PR_NULLABLE slot) {
    uint32_t at = (node->flags & DomNodeFlag_Slotted) != 0 ? slottedAt(slots, node) : kHashMissing;
    if (at == kHashMissing || (slot != nullptr && slots->slotted.data[at].slot != slot))
        return;
    node->flags = DomNodeFlags(node->flags & ~DomNodeFlag_Slotted);
    slots->slottedIndex.erase(prHashPointer(node, 0), at);
    uint32_t last = slots->slotted.count - 1;
    if (at != last) {
        slots->slottedIndex.relocate(prHashPointer(slots->slotted.data[last].node, 0), last, at);
        slots->slotted.data[at] = slots->slotted.data[last];
    }
    slots->slotted.pop();
}

// dom/#assign-slotables. The document's render learns of each change to the slot's assigned nodes.
void assignSlottables(PrDomElement* slot) {
    Array<PrDomNode* PR_NONNULL> slottables;
    findSlottables(slot, &slottables);
    DomSlot* state = slotOf(slot);
    bool same = state != nullptr ? slottables.count == state->assigned.count : slottables.count == 0;
    for (uint32_t i = 0; same && i < slottables.count; ++i)
        same = state->assigned.data[i] == slottables.data[i];
    if (!same) {
        signalSlotChange(slot);
        state = stateOf(slot);
        Array<PrDomNode* PR_NONNULL> before = state->assigned;
        state->assigned = slottables;
        slottables = {};
        DomSlots* slots = slot->document->slots;
        for (uint32_t i = 0; i < before.count; ++i) {
            if (!holds(state->assigned, before.data[i]))
                unsetSlotted(slots, before.data[i], slot);
        }
        for (uint32_t i = 0; i < state->assigned.count; ++i)
            setSlotted(slots, state->assigned.data[i], slot);
        domNotifySlotChanged(slot, before.data, before.count);
        before.reset();
    }
    slottables.reset();
}

// dom/#assign-slotables-for-a-tree, for the slots among the inclusive descendants of `root`.
void assignForTree(PrDomNode* root) {
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (domIsSlot(node))
            assignSlottables(domElementFromNode(node));
    }
}

// dom/#assign-a-slot
void assignSlot(PrDomNode* slottable) {
    if (PrDomElement* slot = domFindSlot(slottable, false))
        assignSlottables(slot);
}

bool hasSlot(PrDomNode* root) {
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (domIsSlot(node))
            return true;
    }
    return false;
}

// Removes `node` from the list.
template <typename Node>
void forgetIn(Array<Node* PR_NONNULL>* list, const PrDomNode* node) {
    for (uint32_t i = list->count; i-- > 0;) {
        if (list->data[i] == node)
            list->erase(i);
    }
}

// Returns the shadow root that `node` hosts, or null if it is not a host.
PrDomShadowRoot* PR_NULLABLE hostedShadow(const PrDomNode* PR_NULLABLE node) {
    const PrDomElement* host = domElementFromNode(node);
    return host != nullptr ? domShadowRootOfHost(host) : nullptr;
}

// Signals a change for a slot in a shadow tree whose children changed, if the children are its fallback content. They
// are its fallback content if the slot has nothing assigned.
void fallbackChanged(PrDomNode* parent) {
    if (!domIsSlot(parent) || shadowOf(parent) == nullptr)
        return;
    const DomSlot* state = slotOf(domElementFromNode(parent));
    if (state == nullptr || state->assigned.count == 0)
        signalSlotChange(domElementFromNode(parent));
}

} // namespace

bool domIsSlot(const PrDomNode* node) {
    const PrDomElement* element = domElementFromNode(node);
    return domIsHtml(element, PrDomTag_Slot);
}

void domSlotsEnable(PrDomDocument* document) {
    if (document->slots == nullptr)
        document->slots = allocZeroed<DomSlots>();
}

void domSlotsDestroy(PrDomDocument* document) {
    DomSlots* slots = document->slots;
    if (slots == nullptr)
        return;
    if (slots->token != nullptr)
        slots->token->document = nullptr;
    slots->signals.reset();
    slots->manual.reset();
    for (uint32_t i = 0; i < slots->slotted.count; ++i) {
        PrDomNode* node = slots->slotted.data[i].node;
        node->flags = DomNodeFlags(node->flags & ~DomNodeFlag_Slotted);
    }
    slots->slotted.reset();
    slots->slottedIndex.reset();
    prMemoryFree(slots);
    document->slots = nullptr;
}

PrDomElement* PR_NULLABLE domFindSlot(const PrDomNode* slottable, bool open) {
    if (slottable->type == PrDomNodeType_Attribute)
        return nullptr;
    PrDomShadowRoot* shadow = hostedShadow(slottable->parent);
    if (shadow == nullptr || (open && shadow->mode != PrDomShadowRootMode_Open))
        return nullptr;
    bool manual = shadow->slotAssignment == PrDomSlotAssignmentMode_Manual;
    PrStringView name = manual ? PrStringView{} : slottableName(slottable);
    for (PrDomNode* node = shadow->first; node != nullptr; node = domNodeNext(shadow, node)) {
        if (!domIsSlot(node))
            continue;
        PrDomElement* slot = domElementFromNode(node);
        if (manual) {
            const DomSlot* state = slotOf(slot);
            if (state != nullptr && holds(state->manual, slottable))
                return slot;
        } else if (domAttributeValue(slot, "name") == name) {
            return slot;
        }
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE domAssignedSlot(const PrDomNode* slottable) {
    if ((slottable->flags & DomNodeFlag_Slotted) == 0)
        return nullptr;
    const DomSlots* slots = slottable->document->slots;
    uint32_t at = slottedAt(slots, slottable);
    return at != kHashMissing ? slots->slotted.data[at].slot : nullptr;
}

uint32_t domSlotAssignedCount(const PrDomElement* slot) {
    const DomSlot* state = slotOf(slot);
    return state != nullptr ? state->assigned.count : 0;
}

PrDomNode* const PR_NONNULL* PR_NULLABLE domSlotAssigned(const PrDomElement* slot) {
    const DomSlot* state = slotOf(slot);
    return state != nullptr ? state->assigned.data : nullptr;
}

void domSlotAssignedNodes(const PrDomElement* slot, bool flatten, Array<PrDomNode* PR_NONNULL>* out) {
    if (flatten) {
        findFlattened(slot, out);
        return;
    }
    if (const DomSlot* state = slotOf(slot))
        out->append(state->assigned.data, state->assigned.count);
}

bool domSlotHasSlotted(const PrDomElement* slot) {
    const DomSlot* state = slotOf(slot);
    if (state == nullptr)
        return false;
    for (uint32_t i = 0; i < state->assigned.count; ++i) {
        PrDomNode* node = state->assigned.data[i];
        if (!domIsSlot(node) || shadowOf(node) == nullptr)
            return true;
        Array<PrDomNode* PR_NONNULL> flattened;
        findFlattened(domElementFromNode(node), &flattened);
        bool any = flattened.count != 0;
        flattened.reset();
        if (any)
            return true;
    }
    return false;
}

void domSlotAssign(PrDomElement* slot, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    PrDomDocument* document = slot->document;
    domSlotsEnable(document);
    DomSlots* slots = document->slots;
    DomSlot* state = stateOf(slot);
    // html/#dom-slot-assign: each node leaves the slot that it was manually assigned to, and the slot's nodes become
    // the given nodes, each listed once.
    Array<PrDomNode* PR_NONNULL> assigned;
    for (uint32_t i = 0; i < count; ++i) {
        PrDomNode* node = nodes[i];
        if (holds(assigned, node))
            continue;
        for (uint32_t j = 0; j < slots->manual.count; ++j) {
            PrDomElement* other = slots->manual.data[j];
            if (other == slot)
                continue;
            forgetIn(&slotOf(other)->manual, node);
        }
        assigned.push(node);
    }
    state->manual.reset();
    state->manual = assigned;
    forgetIn(&slots->manual, slot);
    if (state->manual.count != 0)
        slots->manual.push(slot);
    assignForTree(domNodeRoot(slot));
}

void domSlotsInserted(PrDomNode* parent, PrDomNode* node) {
    // dom/#concept-node-insert: a slottable that a host takes is assigned if the host's shadow root assigns by name. A
    // slot that had only fallback content signals its change. The shadow tree that the node joins assigns its slots
    // again.
    PrDomShadowRoot* shadow = hostedShadow(parent);
    if (shadow != nullptr && shadow->slotAssignment == PrDomSlotAssignmentMode_Named && isSlottable(node))
        assignSlot(node);
    fallbackChanged(parent);
    if (PrDomShadowRoot* root = shadowOf(node))
        assignForTree(root);
}

void domSlotsRemoved(PrDomNode* parent, PrDomNode* node, PrDomElement* PR_NULLABLE assigned) {
    // dom/#concept-node-remove
    if (assigned != nullptr)
        assignSlottables(assigned);
    fallbackChanged(parent);
    // A slot outside a shadow tree has nothing assigned, so the slots of a node that was in no shadow tree assign
    // nothing either way.
    PrDomShadowRoot* root = shadowOf(parent);
    if (root != nullptr && hasSlot(node)) {
        assignForTree(root);
        assignForTree(node);
    }
}

void domSlotsAttributeChanged(PrDomElement* element, PrStringView name) {
    if (name == "slot") {
        // dom/#slotable-name: update the slot that the element was assigned to, and the slot that its name finds now.
        if (PrDomElement* assigned = domAssignedSlot(element))
            assignSlottables(assigned);
        assignSlot(element);
    } else if (name == "name" && domIsSlot(element)) {
        // dom/#slot-name
        if (PrDomShadowRoot* root = shadowOf(element))
            assignForTree(root);
    }
}

void domSlotsTransfer(PrDomDocument* document, PrDomNode* node) {
    DomSlots* from = node->document->slots;
    if (from == nullptr || document == node->document)
        return;
    // A slotted node moves with its host, and its slot with the host's shadow tree.
    if ((node->flags & DomNodeFlag_Slotted) != 0) {
        PrDomElement* slot = from->slotted.data[slottedAt(from, node)].slot;
        unsetSlotted(from, node, nullptr);
        domSlotsEnable(document);
        setSlotted(document->slots, node, slot);
    }
    if (!domIsSlot(node))
        return;
    forgetIn(&from->signals, node);
    forgetIn(&from->manual, node);
    const DomSlot* state = slotOf(domElementFromNode(node));
    if (state != nullptr && state->manual.count != 0) {
        domSlotsEnable(document);
        document->slots->manual.push(domElementFromNode(node));
    }
}

void domSlotsForget(PrDomNode* node) {
    DomSlots* slots = node->document->slots;
    if (slots == nullptr)
        return;
    forgetIn(&slots->signals, node);
    forgetIn(&slots->manual, node);
    for (uint32_t i = 0; i < slots->manual.count; ++i)
        forgetIn(&slotOf(slots->manual.data[i])->manual, node);
    unsetSlotted(slots, node, nullptr);
}

void domSlotsFire(PrDomDocument* document) {
    DomSlots* slots = document->slots;
    if (slots == nullptr || slots->signals.count == 0)
        return;
    Array<PrDomElement* PR_NONNULL> signals = slots->signals;
    slots->signals = {};
    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    for (uint32_t i = 0; i < signals.count; ++i)
        domEventFire(signals.data[i], "slotchange", init);
    signals.reset();
}

} // namespace pr_impl
