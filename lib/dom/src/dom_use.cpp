// The shadow trees of SVG's use elements.

#include "dom/src/dom_use.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/string_ops.h"

#include "dom/src/dom_element_state.h"
#include "dom/src/dom_node_leaves.h"
#include "dom/src/dom_resource.h"

namespace pr_impl {

// A use element of the document, and the elements of the document that its shadow tree instances: what its href
// references, and what the uses among the instances reference.
struct DomUseRecord {
    PrDomElement* PR_NULLABLE use;      // null once the use is disconnected, until the next synchronization drops it
    const PrDomElement* PR_NULLABLE target;
    uint32_t named;                     // the hash of the ID that the href named when `target` was found
    bool resolve;                       // the element that the href names may have changed
    bool stale;
    Array<const PrDomElement*> reached;
};

// A referenced element, and the record of a use whose shadow tree instances it. The entries of one element are a list.
struct DomUseReach {
    const PrDomElement* target;
    uint32_t record;
    uint32_t next;                      // the next entry of the same element, or kArrayMissing
};

struct DomUses {
    Array<DomUseRecord> records;
    Array<DomUseReach> reaches;
    HashIndex reachIndex;               // the first entry of each referenced element, by element

    bool stale;                        // some record is stale, or needs resolving
    bool syncing;                      // the document's mutations are the synchronization's own
};

namespace {

// The most uses that are synchronized inside one another's shadow trees, and the most elements that one use of the
// document instances, with the uses inside its shadow tree.
constexpr uint32_t kMaxDepth = 16;
constexpr uint32_t kMaxInstances = 100000;

uint32_t hashOf(const PrDomElement* element) {
    return hashPointer(element, 0);
}

uint32_t hashOfId(PrStringView id) {
    return uint32_t(hashBytes(id.data, id.size, 0));
}

bool isUse(const PrDomElement* element) {
    return element->space == PrDomNamespace_Svg && element->tag == PrDomTag_Use;
}

// Returns the use's href, or its xlink:href if it has none, or the null view.
PrStringView hrefOf(const PrDomElement* use) {
    const PrDomAttr* href = domAttribute(use, PrDomNamespace_None, "href");
    if (href == nullptr)
        href = domAttribute(use, PrDomNamespace_XLink, "href");
    return href != nullptr ? prStringView(&href->value) : PrStringView{};
}

// Returns the element that the use's href or xlink:href names, by ID in the use's document or in another document. In
// another document, a URL without a fragment names the root element (SVG2/struct.html#UseElement). Writes the hash of
// the ID that the href names to `named`, if it is not null. Returns null until another document has loaded.
const PrDomElement* PR_NULLABLE targetOf(const PrDomElement* use, uint32_t* PR_NULLABLE named = nullptr) {
    PrStringView href = hrefOf(use);
    DomReference reference{};
    bool split = href.data != nullptr && domReferenceSplit(use->document, href, &reference);
    if (named != nullptr)
        *named = hashOfId(split && reference.fragment.data != nullptr ? reference.fragment : PrStringView{});
    const PrDomElement* target = split ? domReferenceTarget(use->document, reference) : nullptr;
    prStringReset(&reference.url);
    return target;
}

// Returns true if `node` is an instance of `original`: both are character data of one type, or elements of one
// namespace and name.
bool matches(const PrDomNode* node, const PrDomNode* original) {
    if (node->type != original->type)
        return false;
    if (node->type != PrDomNodeType_Element)
        return true;
    const PrDomElement* a = static_cast<const PrDomElement*>(node);
    const PrDomElement* b = static_cast<const PrDomElement*>(original);
    return a->space == b->space && domElementName(a) == domElementName(b);
}

// Returns true if an original is instanced: a script is inert in a use's shadow tree (SVG2/struct.html#UseShadowTree),
// and is left out.
bool instanced(const PrDomNode* original) {
    const PrDomElement* e = domElementFromNode(original);
    return e == nullptr || e->tag != PrDomTag_Script
           || (e->space != PrDomNamespace_Html && e->space != PrDomNamespace_Svg);
}

// The state of one use's synchronization: the uses being synchronized, outermost first, and what it has instanced.
struct Sync {
    DomUseRecord* record;
    const PrDomElement* stack[kMaxDepth];
    uint32_t depth;
    uint32_t instances;
};

void syncUse(Sync& s, PrDomElement* use, const PrDomElement* PR_NULLABLE target);

// Makes `node` take the attributes of `original`, in place.
void syncAttributes(PrDomElement* node, const PrDomElement* original) {
    for (const PrDomAttr* a = original->firstAttribute; a != nullptr; a = static_cast<const PrDomAttr*>(a->next)) {
        const PrDomAttr* own = domAttribute(node, a->space, prStringView(&a->name));
        if (own == nullptr || !(prStringView(&own->value) == prStringView(&a->value)))
            domSetAttributeNs(node, a->space, domAttributePrefix(a), prStringView(&a->name), prStringView(&a->value));
    }
    for (PrDomAttr* a = node->firstAttribute; a != nullptr;) {
        PrDomAttr* next = static_cast<PrDomAttr*>(a->next);
        if (domAttribute(original, a->space, prStringView(&a->name)) == nullptr)
            domRemoveAttributeNode(node, a);
        a = next;
    }
}

void syncChildren(Sync& s, PrDomNode* node, const PrDomNode* original);

// Makes `node`, an instance of `original`, match it: its attributes or its data, and its children.
void syncNode(Sync& s, PrDomNode* node, const PrDomNode* original) {
    ++s.instances;
    if (PrDomElement* e = domElementFromNode(node)) {
        syncAttributes(e, static_cast<const PrDomElement*>(original));
        syncChildren(s, node, original);
        if (isUse(e))
            syncUse(s, e, targetOf(e));
        return;
    }
    const PrDomCharacterData* data = static_cast<const PrDomCharacterData*>(original);
    PrDomCharacterData* own = static_cast<PrDomCharacterData*>(node);
    if (!(prStringView(&own->data) == prStringView(&data->data)))
        domSetData(own, prStringView(&data->data));
}

// Makes the children of `node` instances of the children of `original`, in order. A child that matches its original
// keeps its identity. A child that does not is removed if the child after it matches, as when the original lost a
// child, and otherwise an instance of the original is inserted before it.
void syncChildren(Sync& s, PrDomNode* node, const PrDomNode* original) {
    PrDomNode* child = node->first;
    for (const PrDomNode* o = original->first; o != nullptr; o = o->next) {
        if (!instanced(o))
            continue;
        if (s.instances >= kMaxInstances)
            break;
        if (child != nullptr && !matches(child, o) && child->next != nullptr && matches(child->next, o)) {
            PrDomNode* removed = child;
            child = child->next;
            domRemove(removed);
        }
        if (child == nullptr || !matches(child, o)) {
            PrDomNode* copy = domClone(o, node->document, false);
            if (copy == nullptr)
                continue;
            domInsert(node, copy, child);
            child = copy;
        }
        syncNode(s, child, o);
        child = child->next;
    }
    while (child != nullptr) {
        PrDomNode* removed = child;
        child = child->next;
        domRemove(removed);
    }
}

// Brings the shadow tree of `use` up to date with `target`, the element that its href names, or empties it if the
// reference is in error: it names nothing, or an element outside the SVG namespace, or makes a cycle through the use's
// ancestors or the uses being synchronized.
void syncUse(Sync& s, PrDomElement* use, const PrDomElement* PR_NULLABLE target) {
    bool valid = target != nullptr && target->space == PrDomNamespace_Svg && s.depth < kMaxDepth;
    for (uint32_t i = 0; valid && i < s.depth; ++i)
        valid = s.stack[i] != target;
    for (const PrDomNode* n = use; valid && n != nullptr; n = n->parent)
        valid = n != target;
    PrDomShadowRoot* shadow = domShadowRootOfHost(use);
    if (shadow == nullptr) {
        if (!valid)
            return;
        shadow = domAttachUseShadow(use);
    }
    if (!valid) {
        while (shadow->first != nullptr)
            domRemove(shadow->first);
        return;
    }
    s.record->reached.push(target);
    s.stack[s.depth++] = target;
    // The shadow root's one child is the instance root (SVG2/struct.html#UseShadowTree).
    PrDomNode* root = shadow->first;
    while (root != nullptr && root->next != nullptr)
        domRemove(root->next);
    if (root != nullptr && !matches(root, target)) {
        domRemove(root);
        root = nullptr;
    }
    if (root == nullptr) {
        root = domClone(target, use->document, false);
        domInsert(shadow, root, nullptr);
    }
    syncNode(s, root, target);
    --s.depth;
}

// Lists each record under the elements that it reached, so that a mutation finds the records that it reaches.
void index(DomUses* uses) {
    uses->reaches.clear();
    uses->reachIndex.clear();
    for (uint32_t r = 0; r < uses->records.count; ++r) {
        const DomUseRecord& record = uses->records.data[r];
        for (uint32_t i = 0; i < record.reached.count; ++i) {
            const PrDomElement* target = record.reached.data[i];
            uint32_t hash = hashOf(target);
            uint32_t head =
                uses->reachIndex.find(hash, [&](uint32_t at) { return uses->reaches.data[at].target == target; });
            uint32_t at = uses->reaches.count;
            if (head == kHashMissing) {
                uses->reaches.push(DomUseReach{target, r, kArrayMissing});
                uses->reachIndex.insert(hash, at);
            } else {
                uses->reaches.push(DomUseReach{target, r, uses->reaches.data[head].next});
                uses->reaches.data[head].next = at;
            }
        }
    }
}

// Marks stale the records of the uses that instance `node` or one of its ancestors.
void markReached(DomUses* uses, const PrDomNode* node) {
    if (uses->reaches.count == 0)
        return;
    for (const PrDomNode* n = node; n != nullptr; n = n->parent) {
        const PrDomElement* e = domElementFromNode(n);
        if (e == nullptr)
            continue;
        uint32_t at = uses->reachIndex.find(hashOf(e), [&](uint32_t k) { return uses->reaches.data[k].target == e; });
        for (; at != kArrayMissing && at != kHashMissing; at = uses->reaches.data[at].next) {
            uses->records.data[uses->reaches.data[at].record].stale = true;
            uses->stale = true;
        }
    }
}

// Returns the document's uses if a mutation of the document may reach them, or null.
DomUses* PR_NULLABLE watching(const PrDomNode* node) {
    DomUses* uses = node->document->uses;
    return uses != nullptr && !uses->syncing ? uses : nullptr;
}

} // namespace

bool domIsUseShadowRoot(const PrDomNode* root) {
    const PrDomShadowRoot* shadow = domNodeShadowRoot(root);
    return shadow != nullptr && isUse(shadow->host);
}

bool domInUseShadowTree(const PrDomNode* node) {
    for (const PrDomNode* root = domNodeRoot(node); domNodeShadowRoot(root) != nullptr;
         root = domNodeRoot(domNodeShadowRoot(root)->host)) {
        if (domIsUseShadowRoot(root))
            return true;
    }
    return false;
}

PrDomNode* PR_NULLABLE domNodeUseOriginal(const PrDomNode* node) {
    // The positions of the node and its ancestors among their siblings, up to the instance root, which is the shadow
    // root's one child. An instance's children correspond to its original's children in order, except the originals
    // that are not instanced.
    Array<uint32_t> positions;
    const PrDomNode* at = node;
    const PrDomNode* parent = at->parent;
    while (parent != nullptr && !domIsUseShadowRoot(parent)) {
        uint32_t position = 0;
        for (const PrDomNode* sibling = parent->first; sibling != at; sibling = sibling->next)
            ++position;
        positions.push(position);
        at = parent;
        parent = at->parent;
    }
    const PrDomNode* original = parent != nullptr ? targetOf(domNodeShadowRoot(parent)->host) : nullptr;
    if (original != nullptr && !matches(at, original))
        original = nullptr;
    for (uint32_t i = positions.count; original != nullptr && i-- != 0;) {
        uint32_t position = positions.data[i];
        const PrDomNode* child = original->first;
        while (child != nullptr && (!instanced(child) || position-- != 0))
            child = child->next;
        original = child;
    }
    positions.reset();
    return const_cast<PrDomNode*>(original);
}

void domUseConnectionChanged(PrDomElement* element) {
    if (!isUse(element) || domInUseShadowTree(element))
        return;
    PrDomDocument* document = element->document;
    if (domIsConnected(element)) {
        if (document->uses == nullptr)
            document->uses = allocZeroed<DomUses>();
        document->uses->records.push(DomUseRecord{.use = element, .resolve = true, .stale = true});
        document->uses->stale = true;
        return;
    }
    DomUses* uses = document->uses;
    for (uint32_t i = 0; uses != nullptr && i < uses->records.count; ++i) {
        if (uses->records.data[i].use == element)
            uses->records.data[i].use = nullptr;
    }
}

void domUsesAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name) {
    DomUses* uses = watching(element);
    if (uses == nullptr)
        return;
    // What an href names changes with its own href, and with the elements that have its ID (domUsesIdChanged).
    if (isUse(element) && name == "href" && (space == PrDomNamespace_None || space == PrDomNamespace_XLink)) {
        for (uint32_t i = 0; i < uses->records.count; ++i) {
            if (uses->records.data[i].use == element)
                uses->records.data[i].resolve = uses->stale = true;
        }
    }
    markReached(uses, element);
}

void domUsesChildrenChanged(PrDomNode* parent, const PrDomNode* /*child*/) {
    if (DomUses* uses = watching(parent))
        markReached(uses, parent);
}

void domUsesDataChanged(PrDomNode* node) {
    if (DomUses* uses = watching(node))
        markReached(uses, node);
}

void domUsesIdChanged(PrDomNode* root, PrStringView id) {
    DomUses* uses = root->type == PrDomNodeType_Document ? watching(root) : nullptr;
    if (uses == nullptr)
        return;
    uint32_t hash = hashOfId(id);
    for (uint32_t i = 0; i < uses->records.count; ++i) {
        if (uses->records.data[i].named == hash)
            uses->records.data[i].resolve = uses->stale = true;
    }
}

void domUsesMoved(PrDomNode* node) {
    PrDomNode* root = domNodeRoot(node);
    if (root->type != PrDomNodeType_Document || watching(root) == nullptr)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (element != nullptr && (element->flags & DomNodeFlag_Id) != 0)
            domUsesIdChanged(root, domElementId(element));
    }
}

void domUsesResolveAll(PrDomDocument* document) {
    DomUses* uses = document->uses;
    if (uses == nullptr)
        return;
    for (uint32_t i = 0; i < uses->records.count; ++i)
        uses->records.data[i].resolve = true;
    uses->stale = true;
}

void domUsesSync(PrDomDocument* document) {
    DomUses* uses = document->uses;
    if (uses == nullptr || !uses->stale || uses->syncing)
        return;
    uses->syncing = true;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < uses->records.count; ++i) {
        DomUseRecord& record = uses->records.data[i];
        if (record.use == nullptr) {
            record.reached.reset();
            continue;
        }
        if (record.resolve) {
            const PrDomElement* target = targetOf(record.use, &record.named);
            record.stale = record.stale || target != record.target;
            record.target = target;
            record.resolve = false;
        }
        uses->records.data[kept++] = record;
    }
    uses->records.count = kept;
    for (uint32_t i = 0; i < uses->records.count; ++i) {
        DomUseRecord& record = uses->records.data[i];
        if (!record.stale)
            continue;
        record.stale = false;
        record.reached.clear();
        Sync s{.record = &record};
        // The record's use is held: a synchronization runs no script, which is what could free it.
        syncUse(s, record.use, record.target);
    }
    index(uses);
    uses->stale = false;
    uses->syncing = false;
}

void domUsesDestroy(DomUses* PR_NULLABLE uses) {
    if (uses == nullptr)
        return;
    for (uint32_t i = 0; i < uses->records.count; ++i)
        uses->records.data[i].reached.reset();
    uses->records.reset();
    uses->reaches.reset();
    uses->reachIndex.reset();
    prMemoryFree(uses);
}

} // namespace pr_impl
