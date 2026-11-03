// The node tree
//
// dom - https://dom.spec.whatwg.org/

#include "dom/src/dom_node.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/name_table.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"
#include "pr/utf8.h"

#include "dom/src/dom_control.h"
#include "dom/src/dom_custom.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event_handler.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_mutation.h"
#include "dom/src/dom_range.h"
#include "dom/src/dom_resource.h"
#include "dom/src/dom_select.h"
#include "dom/src/dom_selection.h"
#include "dom/src/dom_slot.h"
#include "dom/src/dom_traversal.h"
#include "dom/src/dom_use.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Local names

// Returns the tag for an element of the document whose local name `name` no tag names. The tag is an atom of the
// document's local names.
DomTagId internLocalName(PrDomDocument* document, PrStringView name) {
    if (document->localNames == nullptr)
        document->localNames = allocZeroed<AtomTable>();
    return PrDomTag_Count + atomTableAdd(document->localNames, name);
}

// Returns the atom of `text` in `*table`, and adds `text` if the table lacks it and holds fewer than `limit` atoms.
// Returns kAtomNone if the table is full. The first atom creates the table.
Atom internAtom(AtomTable* PR_NULLABLE* table, PrStringView text, uint32_t limit) {
    Atom atom = *table != nullptr ? atomTableFind(*table, text) : kAtomNone;
    if (atom != kAtomNone || (*table != nullptr && (*table)->atoms.count == limit))
        return atom;
    if (*table == nullptr)
        *table = allocZeroed<AtomTable>();
    return atomTableAdd(*table, text);
}

// Frees a table that internAtom created.
void atomsDestroy(AtomTable* PR_NULLABLE table) {
    if (table != nullptr)
        atomTableReset(table);
    prMemoryFree(table);
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

// Sets the element's classes to the words of `value`, which is the value of its class attribute. The null view clears
// them (dom/#concept-class).
void setClasses(PrDomElement* element, PrStringView value) {
    element->classes = domClassesAdd(element->document, value);
}

// Moves the element's classes from the table of `from` to the table of the element's document.
void transferClasses(PrDomElement* element, PrDomDocument* from) {
    if (element->classes != kDomClassesNone)
        element->classes = domClassesAdd(element->document, domClassesText(from, element->classes));
}

// ---------------------------------------------------------------------------------------------------------------------
// Elements by ID

constexpr uint64_t kDomIdSeed = 0x2545f4914f6cdd1dull;

// Returns the slot in which the tree whose root is `root` keeps its elements by ID, or null for a tree that keeps none.
DomIds* PR_NULLABLE* PR_NULLABLE idsOf(PrDomNode* root) {
    if (root->type == PrDomNodeType_Document)
        return &static_cast<PrDomDocument*>(root)->ids;
    if (PrDomShadowRoot* shadow = domNodeShadowRoot(root))
        return &shadow->ids;
    return nullptr;
}

uint32_t idHash(PrStringView id) {
    return uint32_t(prHashBytes(id.data, id.size, kDomIdSeed));
}

uint32_t findId(const DomIds* ids, PrStringView id, uint32_t hash) {
    return ids->index.find(hash, [&](uint32_t at) {
        return prRawStringView(&ids->entries.data[at].id) == id;
    });
}

// Returns true if `entry` knows the first of its elements. A move within the tree does not reach a lone element's
// entry, since the element stays the first wherever it moves, and makes what the entry knew of several elements out of
// date.
bool firstKnown(const DomIds* ids, const DomIdEntry* entry) {
    return entry->element != nullptr && (entry->count == 1 || entry->moves == ids->moves);
}

// Returns true if every other node of the tree precedes `element`, which is where a parser adds an element.
bool isLastInTree(const PrDomElement* element) {
    if (element->first != nullptr)
        return false;
    for (const PrDomNode* node = element; node != nullptr; node = node->parent) {
        if (node->next != nullptr)
            return false;
    }
    return true;
}

void idsDestroy(DomIds* PR_NULLABLE ids) {
    if (ids == nullptr)
        return;
    for (uint32_t i = 0; i < ids->entries.count; ++i)
        prRawStringReset(&ids->entries.data[i].id);
    ids->entries.reset();
    ids->index.reset();
    prMemoryFree(ids);
}

// Counts `element` among the elements with the ID `id` in the tree whose root is `root`.
PR_NOINLINE void addId(PrDomNode* root, PrDomElement* element, PrStringView id) {
    DomIds** slot = idsOf(root);
    if (id.size == 0 || slot == nullptr)
        return;
    domUsesIdChanged(root, id);
    if (*slot == nullptr)
        *slot = allocZeroed<DomIds>();
    DomIds* ids = *slot;
    uint32_t hash = idHash(id);
    uint32_t at = findId(ids, id, hash);
    if (at != kHashMissing) {
        // An element added after every other leaves the first as it was. Any other may be the new first.
        DomIdEntry* entry = &ids->entries.data[at];
        bool stays = firstKnown(ids, entry) && isLastInTree(element);
        ++entry->count;
        entry->moves = ids->moves;
        if (!stays)
            entry->element = nullptr;
        return;
    }
    ids->index.insert(hash, ids->entries.count);
    ids->entries.push(DomIdEntry{prRawStringCopy(id), hash, 1, ids->moves, element});
}

// Removes one element from those with the ID `id` in the tree whose root is `root`.
PR_NOINLINE void removeId(PrDomNode* root, PrStringView id) {
    DomIds** slot = idsOf(root);
    if (id.size == 0 || slot == nullptr || *slot == nullptr)
        return;
    domUsesIdChanged(root, id);
    DomIds* ids = *slot;
    uint32_t hash = idHash(id);
    uint32_t at = findId(ids, id, hash);
    PR_ASSERT(at != kHashMissing);
    DomIdEntry* entry = &ids->entries.data[at];
    // If two elements had the ID, finding the first of those left takes a walk, which the next lookup performs.
    entry->element = nullptr;
    if (--entry->count != 0)
        return;
    prRawStringReset(&entry->id);
    ids->index.erase(hash, at);
    uint32_t last = ids->entries.count - 1;
    if (at != last) {
        ids->index.erase(ids->entries.data[last].hash, last);
        ids->entries.data[at] = ids->entries.data[last];
        ids->index.insert(ids->entries.data[at].hash, at);
    }
    ids->entries.count = last;
    if (last == 0) {
        idsDestroy(ids);
        *slot = nullptr;
    }
}

// Moves the elements with an ID in the subtree of `node` from the tree whose root is `from` to the tree whose root is
// `to`. A shadow tree inside the subtree keeps its own elements.
PR_NOINLINE void moveIds(PrDomNode* node, PrDomNode* from, PrDomNode* to) {
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element == nullptr || (element->flags & DomNodeFlag_Id) == 0)
            continue;
        PrStringView id = domElementId(element);
        removeId(from, id);
        addId(to, element, id);
    }
}

void addDetached(PrDomNode* node) {
    PrDomDocument* document = node->document;
    node->parent = nullptr;
    node->previous = nullptr;
    node->next = document->detached;
    if (node->next != nullptr)
        node->next->previous = node;
    document->detached = node;
}

void removeDetached(PrDomNode* node) {
    PR_ASSERT(node->parent == nullptr);
    if (node->previous != nullptr) {
        node->previous->next = node->next;
    } else {
        PR_ASSERT(node->document->detached == node);
        node->document->detached = node->next;
    }
    if (node->next != nullptr)
        node->next->previous = node->previous;
    node->previous = nullptr;
    node->next = nullptr;
}

// Creates a node of `document` as the root of its own tree, with no references.
template <typename T>
T* make(PrDomDocument* document, PrDomNodeType type) {
    T* object = allocZeroed<T>();
    PrDomNode* node = object;
    node->type = type;
    node->document = document;
    addDetached(node);
    return object;
}

// Creates a fragment in `document` that `host` owns, which is its template contents or the shadow root that it hosts.
template <typename T>
T* makeOwned(PrDomDocument* document, PrDomElement* host) {
    T* fragment = make<T>(document, PrDomNodeType_DocumentFragment);
    removeDetached(fragment);
    fragment->host = host;
    return fragment;
}

// Releases payloads and registrations, and leaves the allocation intact.
void releaseNode(PrDomNode* node) {
    domListenersDestroy(node->listeners);
    if (node->document->slots != nullptr)
        domSlotsForget(node);
    switch (node->type) {
        case PrDomNodeType_Element: {
            PrDomElement* element = static_cast<PrDomElement*>(node);
            domNotifyElementDestroyed(element);
            // An element with no state may still be on the document's list of elements with a form attribute.
            if (element->state != nullptr || element->document->formAttributed.count != 0)
                domControlDestroy(element);
            if (element->state != nullptr) {
                if (element->state->forgetEmbedded != nullptr)
                    element->state->forgetEmbedded(element->state->embedded);
                domCustomDestroy(element->state->custom);
                prRawStringReset(&element->state->prefix);
                prRawStringReset(&element->state->returnValue);
                prRawStringReset(&element->state->imageUrl);
                prDrawImageRelease(element->state->picture);
                prDrawImageRelease(element->state->storedPicture);
                prLayoutPainterRelease(element->state->vector);
                if (DomSlot* slot = element->state->slot) {
                    slot->assigned.reset();
                    slot->manual.reset();
                    prMemoryFree(slot);
                }
                prMemoryFree(element->state);
            }
            return;
        }

        case PrDomNodeType_Attribute: {
            PrDomAttr* attribute = static_cast<PrDomAttr*>(node);
            prRawStringReset(&attribute->name);
            prRawStringReset(&attribute->value);
            return;
        }

        case PrDomNodeType_ProcessingInstruction: {
            PrDomProcessingInstruction* instruction = static_cast<PrDomProcessingInstruction*>(node);
            prRawStringReset(&instruction->target);
            prRawStringReset(&instruction->data);
            return;
        }

        case PrDomNodeType_Text:
        case PrDomNodeType_Comment:
            prRawStringReset(&static_cast<PrDomCharacterData*>(node)->data);
            return;

        case PrDomNodeType_DocumentType: {
            PrDomDocumentType* doctype = static_cast<PrDomDocumentType*>(node);
            prRawStringReset(&doctype->name);
            prRawStringReset(&doctype->publicId);
            prRawStringReset(&doctype->systemId);
            return;
        }

        case PrDomNodeType_DocumentFragment:
            if (PrDomShadowRoot* shadow = domNodeShadowRoot(node)) {
                domNotifyShadowRootDestroyed(shadow);
                idsDestroy(shadow->ids);
            }
            return;

        default:
            return;
    }
}

// Appends the fragments that the element owns: its template contents and the shadow root that it hosts.
void pushOwnedFragments(const PrDomElement* element, RawArray<PrDomNode* PR_NONNULL>* roots) {
    if (domElementContents(element) != nullptr && domNodeOwner(domElementContents(element)) == element)
        roots->push(domElementContents(element));
    if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
        roots->push(shadow);
}

// Destroys the tree iteratively, so that tree depth does not grow the C++ stack. Attributes and owned fragments are
// reached through their element.
void destroyTree(PrDomNode* root) {
    RawArray<PrDomNode* PR_NONNULL> roots;
    for (PrDomNode* top = root;; top = roots.data[--roots.count]) {
        PrDomNode* node = top;
        for (;;) {
            while (node->first != nullptr) {
                PrDomNode* child = node->first;
                node->first = nullptr;
                node = child;
            }
            PrDomNode* next = node->next;
            PrDomNode* parent = node->parent;
            bool done = node == top;
            if (PrDomElement* element = domElementFromNode(node)) {
                PrDomAttr* attribute = element->firstAttribute;
                while (attribute != nullptr) {
                    PrDomAttr* following = static_cast<PrDomAttr*>(attribute->next);
                    releaseNode(attribute);
                    prMemoryFree(attribute);
                    attribute = following;
                }
                pushOwnedFragments(element, &roots);
            }
            releaseNode(node);
            prMemoryFree(node);
            if (done)
                break;
            node = next != nullptr ? next : parent;
        }
        if (roots.count == 0)
            break;
    }
    roots.reset();
}

bool hasConnectionSteps(const PrDomElement* element);

// Sets or clears the connected flag of `root` and its shadow-including descendants. Adds those with insertion or
// removing steps to `stepped`, if given, in shadow-including tree order. `under` is the node that `root` is or was
// linked under, and its tree keeps its elements by ID. The root of `under` is found only for an element with an ID.
void setConnected(PrDomNode* root, bool connected, RawArray<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped, PrDomNode* under) {
    PrDomNode* scope = nullptr;
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (connected)
            node->flags |= DomNodeFlag_Connected;
        else
            node->flags &= DomNodeFlags(~DomNodeFlag_Connected);
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        if (domIsHtml(element, PrDomTag_Base)) {
            if (connected)
                ++element->document->baseElements;
            else
                --element->document->baseElements;
        }
        if (domIsHtml(element, PrDomTag_Meta))
            domNotifyMetaChanged(element->document);
        if ((element->flags & DomNodeFlag_Id) != 0) {
            if (scope == nullptr)
                scope = domNodeRoot(under);
            if (connected)
                addId(scope, element, domElementId(element));
            else
                removeId(scope, domElementId(element));
        }
        if (stepped != nullptr && hasConnectionSteps(element))
            stepped->push(element);
        if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
            setConnected(shadow, connected, stepped, shadow);
    }
}

// Adds `count` references to the tree whose root is `root`. A tree other than the document's own tree holds a reference
// to the document while the tree has any references.
void treeAdd(PrDomNode* root, uint32_t count) {
    if (count == 0)
        return;
    if (root->type != PrDomNodeType_Document && root->retained == 0)
        domDocumentRetain(root->document);
    root->retained += count;
}

// Removes `count` references from the tree whose root is `root`. Returns true if the tree is left with none.
bool treeTake(PrDomNode* root, uint32_t count) {
    PR_ASSERT(root->retained >= count);
    root->retained -= count;
    return count != 0 && root->retained == 0;
}

void destroyDocument(PrDomDocument* document);

// Adds `count` to the counts of `node` and each node that owns it, up to its root. Returns the root.
PrDomNode* countUp(PrDomNode* node, uint32_t count) {
    PrDomNode* at = node;
    for (;;) {
        at->retained += count;
        PrDomNode* owner = domNodeOwner(at);
        if (owner == nullptr)
            return at;
        at = owner;
    }
}

// Subtracts `count` from the counts of `node` and each node that owns it, up to its root. Returns the root.
PrDomNode* countDown(PrDomNode* node, uint32_t count) {
    PrDomNode* at = node;
    for (;;) {
        PR_ASSERT(at->retained >= count);
        at->retained -= count;
        PrDomNode* owner = domNodeOwner(at);
        if (owner == nullptr)
            return at;
        at = owner;
    }
}

// Called after an owner has taken `node`, which was a root. The node's references now count toward the owner's tree.
// That tree holds the document while it has any references, because the node no longer does.
void joinOwner(PrDomNode* node) {
    uint32_t count = node->retained;
    if (count == 0)
        return;
    PrDomNode* root = countUp(domNodeOwner(node), count);
    if (root->type != PrDomNodeType_Document && root->retained == count)
        domDocumentRetain(root->document);
    domDocumentRelease(node->document);
}

// Makes `node`, which `owner` has just released, the root of its own tree. The new tree takes its references from the
// owner's tree. Neither tree is freed.
void leaveOwner(PrDomNode* node, PrDomNode* owner) {
    addDetached(node);
    uint32_t count = node->retained;
    if (count == 0)
        return;
    domDocumentRetain(node->document);
    PrDomNode* root = countDown(owner, count);
    if (root->retained == 0 && root->type != PrDomNodeType_Document)
        domDocumentRelease(root->document);
}

// Called after the tree whose root is `root` has lost its last reference. A document is destroyed. Any other root is
// freed with its tree, and releases its document.
void treeEmptied(PrDomNode* root) {
    if (root->type == PrDomNodeType_Document) {
        destroyDocument(static_cast<PrDomDocument*>(root));
        return;
    }
    PrDomDocument* document = root->document;
    domFreeTree(root);
    domDocumentRelease(document);
}

// A node on the line of owners at which countEach is counting, and the number of references that it has yet to add to
// or subtract from the counts of the node and its owners.
struct Pending {
    PrDomNode* node;
    uint32_t count;
};

// Adds the pending count of the entry at the top of `line` to its node, or subtracts it if `up` is false, and passes
// the count to the owner below. The root at the bottom takes the steps for its tree. A tree other than the document's
// own tree holds the document while it has references, and a tree left with none is freed.
void flushPending(RawArray<Pending>* line, bool up) {
    Pending top = line->data[--line->count];
    top.node->flags &= DomNodeFlags(~DomNodeFlag_Counting);
    if (up)
        top.node->retained += top.count;
    else
        top.node->retained -= top.count;
    if (line->count != 0) {
        line->data[line->count - 1].count += top.count;
        return;
    }
    PrDomNode* root = top.node;
    if (up && root->type != PrDomNodeType_Document && root->retained == top.count)
        domDocumentRetain(root->document);
    if (!up && root->retained == 0)
        treeEmptied(root);
}

// domNodeRetainEach and domNodeReleaseEach. A node counts itself immediately. The line of owners of the last node
// counted stays on a stack, and each owner keeps the count that it has yet to apply. A node therefore shares the part
// of the line that it is under, and each owner is written once, as the walk leaves it. A root is its own line, so the
// steps for its tree are taken as the walk leaves it.
void countEach(PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count, bool up) {
    RawArray<Pending> line;
    RawArray<PrDomNode* PR_NONNULL> above;   // owners between a node and the line, nearest first
    for (uint32_t i = 0; i < count; ++i) {
        PrDomNode* node = nodes[i];
        PR_ASSERT(up || node->references != 0);
        node->references += up ? 1u : ~0u;
        PrDomNode* owner = domNodeOwner(node);
        PrDomNode* start = node;
        if (owner != nullptr) {
            node->retained += up ? 1u : ~0u;
            if (line.count != 0 && line.last().node == owner) {
                line.last().count += 1;
                continue;
            }
            start = owner;
        }
        above.count = 0;
        PrDomNode* at = start;
        while (at != nullptr && (at->flags & DomNodeFlag_Counting) == 0) {
            above.push(at);
            at = domNodeOwner(at);
        }
        // Pops the line down to the owner that `start` is under, or pops all of it for another tree.
        while (line.count != 0 && line.last().node != at)
            flushPending(&line, up);
        for (uint32_t j = above.count; j-- > 0;) {
            above.data[j]->flags |= DomNodeFlag_Counting;
            line.push(Pending{above.data[j], 0});
        }
        line.last().count += 1;
    }
    while (line.count != 0)
        flushPending(&line, up);
    line.reset();
    above.reset();
}

// Tells the document's tree callback that `node` has a new owner or a new document, if the node's binding has an object
// for it. The node's tree is held while the callback runs, so a release during the callback does not free it.
void treeMoved(PrDomNode* node) {
    const PrDomTreeCallback& callback = node->document->treeCallback;
    if (callback.moved == nullptr || node->binding == nullptr)
        return;
    PrDomNode* root = domTreeRoot(node);
    treeAdd(root, 1);
    callback.moved(callback.context, node);
    if (treeTake(root, 1)) {
        if (root->type == PrDomNodeType_Document)
            destroyDocument(static_cast<PrDomDocument*>(root));
        else
            domDocumentRelease(root->document);
    }
}

// Moves the document's sequential focus navigation starting point out of `child`, which is about to leave its parent,
// to the position where `child` was: just after the element that precedes `child` in flat tree order, which is the
// order that sequential navigation follows through slots. A child that no slot takes is placed by its light tree. The
// starting point is unset if no element precedes it.
void startingPointRemoving(PrDomNode* child) {
    PrDomDocument* document = child->document;
    if (document->navigationStart == nullptr || !domShadowIncludingContains(child, document->navigationStart))
        return;
    PrDomNode* node = child;
    while (domFlatParent(node) == nullptr && node->parent != nullptr)
        node = node->parent;
    PrDomElement* preceding = nullptr;
    while (preceding == nullptr && node != nullptr) {
        node = domFlatPrevious(node);
        preceding = node != nullptr ? domElementFromNode(node) : nullptr;
    }
    document->navigationStart = preceding;
    document->navigationStartAfter = preceding != nullptr;
}

// Removes the elements of `child`, which is about to leave its parent, from the document's autofocus candidates.
void autofocusRemoving(PrDomNode* child) {
    RawArray<PrDomElement* PR_NONNULL>& candidates = child->document->autofocusCandidates;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < candidates.count; ++i) {
        if (!domShadowIncludingContains(child, candidates.data[i]))
            candidates.data[kept++] = candidates.data[i];
    }
    candidates.count = kept;
}

// Unlinks `child` from its parent and siblings, running the range, slot and focus steps of the change.
void unlink(PrDomNode* child) {
    startingPointRemoving(child);
    autofocusRemoving(child);
    domNotifyRemoving(child);
    PrDomNode* parent = child->parent;
    domUsesChildrenChanged(parent, child);
    if (child->document->ranges != nullptr)
        domRangesRemoving(child);
    if (child->document->iterators != nullptr)
        domNodeIteratorsRemoving(child);
    bool slotted = child->document->slots != nullptr;
    PrDomElement* assigned = slotted ? domAssignedSlot(child) : nullptr;

    ++parent->document->treeVersion;
    if (child->previous != nullptr)
        child->previous->next = child->next;
    else
        parent->first = child->next;

    if (child->next != nullptr)
        child->next->previous = child->previous;
    else
        parent->last = child->previous;

    child->parent = nullptr;
    child->previous = nullptr;
    child->next = nullptr;
    if (slotted)
        domSlotsRemoved(parent, child, assigned);
    domSelectUnlinked(child, parent);
    domControlsUnlinked(child, parent);
}

// Links `child`, which has no parent, into the children of `parent` before `before`, running the range and slot steps
// of the change.
void link(PrDomNode* parent, PrDomNode* child, PrDomNode* PR_NULLABLE before,
        RawArray<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped) {
    ++parent->document->treeVersion;
    child->parent = parent;
    child->next = before;
    child->previous = before != nullptr ? before->previous : parent->last;

    if (child->previous != nullptr)
        child->previous->next = child;
    else
        parent->first = child;

    if (child->next != nullptr)
        child->next->previous = child;
    else
        parent->last = child;
    if (domIsConnected(parent) != domIsConnected(child))
        setConnected(child, domIsConnected(parent), stepped, parent);
    domNotifyInserted(child);
    domUsesChildrenChanged(parent, child);
    if (parent->document->ranges != nullptr)
        domRangesInserted(child);
    if (parent->document->slots != nullptr)
        domSlotsInserted(parent, child);
    domSelectLinked(child);
    domControlsLinked(child);
}

// Returns the number, starting from 1, that `document` assigns to `prefix`. Returns 0 if the document cannot hold
// another prefix.
uint8_t internPrefix(PrDomDocument* document, PrStringView prefix) {
    Atom atom = internAtom(&document->prefixes, prefix, 0xff);
    return atom != kAtomNone ? uint8_t(atom + 1) : 0;
}

// Maps a namespace of the node's document to the same namespace in `document`, which holds it from then on.
DomNamespaceId transferNamespace(PrDomDocument* document, const PrDomNode* node, DomNamespaceId space) {
    if (space < PrDomNamespace_Other || node->document == document)
        return space;
    DomNamespaceId moved = PrDomNamespace_None;
    domNamespaceFromUrl(document, domNamespaceUrl(node->document, space), &moved);
    return moved;
}

void adoptTree(PrDomDocument* document, PrDomNode* node, uint32_t* moved);

// Moves `node` and the nodes that it owns to `document`, and adds the number of live ranges moved with them to `moved`.
void adoptContents(PrDomDocument* document, PrDomNode* contents);

void transferDocument(PrDomDocument* document, PrDomNode* node, uint32_t* moved) {
    node->space = transferNamespace(document, node, node->space);
    if (node->type == PrDomNodeType_Attribute) {
        PrDomAttr* attribute = static_cast<PrDomAttr*>(node);
        if (attribute->prefix != 0)
            attribute->prefix = internPrefix(document, domAttributePrefix(attribute));
    }
    domSlotsTransfer(document, node);
    PrDomDocument* from = node->document;
    node->document = document;
    if (PrDomElement* element = domElementFromNode(node)) {
        if (domTagIsAtom(element->tag))
            element->tag = internLocalName(document, atomTableText(from->localNames, element->tag - PrDomTag_Count));
        transferClasses(element, from);
        domCustomAdopted(element, from);
        if (domIsHtml(element, PrDomTag_Option) || domIsHtml(element, PrDomTag_Selectedcontent))
            document->options = true;
        domControlAdopted(element, from);
        for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                attribute = static_cast<PrDomAttr*>(attribute->next)) {
            attribute->space = transferNamespace(document, attribute, attribute->space);
            if (attribute->prefix != 0)
                attribute->prefix = internPrefix(document, domAttributePrefix(attribute));
            attribute->document = document;
        }
        if (domElementContents(element) != nullptr && domNodeOwner(domElementContents(element)) == element)
            adoptContents(domDocumentTemplateDocument(document), domElementContents(element));
        if (PrDomShadowRoot* shadow = domShadowRootOfHost(element)) {
            domSlotsEnable(document);
            adoptTree(document, shadow, moved);
        }
    }
    for (PrDomNode* child = node->first; child != nullptr; child = child->next)
        transferDocument(document, child, moved);
}

// Moves the tree of `node`, which is a root or an owned fragment, to `document`. Adds the number of live ranges in the
// tree to `moved`. The outermost adoption moves the references of those ranges.
void adoptTree(PrDomDocument* document, PrDomNode* node, uint32_t* moved) {
    PrDomDocument* from = node->document;
    transferDocument(document, node, moved);
    if (from->ranges != nullptr)
        *moved += domRangesAdopted(from, node);
    if (from->iterators != nullptr)
        domNodeIteratorsAdopted(from, node);
}

// Moves what `from` records about the nodes that just moved to `document`: their holds and their observers.
void moveRecords(PrDomDocument* from, PrDomDocument* document) {
    RawArray<PrDomNode* PR_NONNULL>& holds = from->holds;
    for (uint32_t i = holds.count; i-- != 0;) {
        if (holds.data[i]->document == document) {
            document->holds.push(holds.data[i]);
            holds.data[i] = holds.data[--holds.count];
        }
    }
    domMutationObserversAdopted(from, document);
}

// Moves the contents of a template to the inert document of the document that its template moved to
// (html/#template-adopting-steps). The live ranges in the contents hold the inert document that they are in.
void adoptContents(PrDomDocument* document, PrDomNode* contents) {
    PrDomDocument* from = contents->document;
    if (from == document)
        return;
    uint32_t moved = 0;
    adoptTree(document, contents, &moved);
    moveRecords(from, document);
    document->retained += moved;
    from->retained -= moved;
}

// Runs the host's adopting steps of the embedded content in `node` and its shadow-including descendants, once the whole
// tree has moved.
void embedsAdopted(const DomDocumentHost* host, PrDomNode* node) {
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element == nullptr)
            continue;
        if (domEmbeds(element))
            host->embedAdopted(host->context, element);
        if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
            embedsAdopted(host, shadow);
    }
}

// Returns the attribute of `element` in `space` named `name` or, if `lowered` is set, named the ASCII lowercase of
// `name`. `hash` is domAttributeHash(name).
PrDomAttr* PR_NULLABLE findAttribute(const PrDomElement* element, DomNamespaceId space,
        PrStringView name, uint32_t hash, bool lowered) {
    for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<PrDomAttr*>(attribute->next)) {
        if (attribute->hash != hash || attribute->space != space)
            continue;
        PrStringView held = prRawStringView(&attribute->name);
        if (lowered ? prAsciiEqualsLowered(held, name) : held == name)
            return attribute;
    }
    return nullptr;
}

// Returns the first HTML child of the document's html element whose tag is `first` or `second`.
PrDomElement* PR_NULLABLE htmlChild(PrDomDocument* document, PrDomTag first, PrDomTag second) {
    PrDomElement* html = domDocumentElement(document);
    if (!domIsHtml(html, PrDomTag_Html))
        return nullptr;
    for (PrDomNode* child = html->first; child != nullptr; child = child->next) {
        PrDomElement* element = domElementFromNode(child);
        if (domIsHtml(element, first) || domIsHtml(element, second))
            return element;
    }
    return nullptr;
}

// The classes of a byte in an element or attribute local name (dom/#valid-element-local-name,
// dom/#valid-attribute-local-name). A byte above ASCII is part of a code point above U+007F.
enum {
    NameClass_ElementStop = 1 << 0,     // ASCII whitespace, NUL, `/`, `>`
    NameClass_AttributeStop = 1 << 1,   // those and `=`
    NameClass_Start = 1 << 2,           // `:`, `_`, and above ASCII
    NameClass_Rest = 1 << 3,            // ASCII alphanumerics, `-`, `.`, `:`, `_`, and above ASCII
};
typedef uint8_t NameClass;

constexpr struct NameClasses {
    NameClass of[256];

    constexpr NameClasses() : of() {
        for (uint32_t c = 0; c < 256; ++c) {
            bool stop = c == 0 || c == '/' || c == '>' || prAsciiIsSpace(char(c));
            bool start = c == ':' || c == '_' || c >= 0x80;
            bool rest = start || c == '-' || c == '.' || prAsciiIsDigit(char(c)) || prAsciiIsLetter(char(c));
            of[c] = NameClass((stop ? NameClass_ElementStop | NameClass_AttributeStop : 0) |
                (c == '=' ? NameClass_AttributeStop : 0) | (start ? NameClass_Start : 0) | (rest ? NameClass_Rest : 0));
        }
    }
} kNameClasses;

// Returns true if every byte of `name` from `from` on has every class in `required` and none in `refused`.
bool bytesAre(PrStringView name, size_t from, NameClass required, NameClass refused) {
    NameClass all = NameClass(~0u);
    NameClass any = 0;
    for (size_t at = from; at < name.size; ++at) {
        NameClass c = kNameClasses.of[uint8_t(name.data[at])];
        all &= c;
        any |= c;
    }
    return (all & required) == required && (any & refused) == 0;
}

// Returns true if the element has an autofocus attribute that makes it an autofocus candidate when it is inserted
// (html/#the-autofocus-attribute). HTML, SVG and MathML elements take the attribute.
bool hasAutofocus(const PrDomElement* element) {
    return element->firstAttribute != nullptr &&
        (element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg ||
            element->space == PrDomNamespace_MathMl) && domAttribute(element, PrDomNamespace_None, "autofocus") != nullptr;
}

// Returns true if the element has insertion or removing steps of its own. Most elements have none, and this function
// decides that without calling out for each element.
bool hasConnectionSteps(const PrDomElement* element) {
    const PrDomDocument* document = element->document;
    if (element->state != nullptr || document->focused == element || document->hovered == element ||
            document->active == element || document->target == element ||
            element->customState == PrDomCustomElementState_Undefined ||
            element->customState == PrDomCustomElementState_Custom || hasAutofocus(element))
        return true;
    if (element->tag == PrDomTag_Style)
        return element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg;
    if (element->tag == PrDomTag_Use)
        return element->space == PrDomNamespace_Svg;
    if (domIsScript(element) || domEmbeds(element))
        return true;
    return element->space == PrDomNamespace_Html && (element->tag == PrDomTag_Link || element->tag == PrDomTag_Meta ||
        element->tag == PrDomTag_Selectedcontent);
}

// Runs the insertion or removing steps (dom/#concept-node-insert-ext, dom/#concept-node-remove-ext) of the elements
// that setConnected found to have them, and empties `stepped`. Each element is held while the steps run, because a step
// can reach the embedder and script. A connected script element that is not parser-inserted is appended to `scripts`,
// held, for its post-connection steps (runPostConnectionSteps).
void runConnectionSteps(RawArray<PrDomElement* PR_NONNULL>* stepped,
        RawArray<PrDomElement* PR_NONNULL>* PR_NULLABLE scripts = nullptr) {
    for (uint32_t i = 0; i < stepped->count; ++i)
        domNodeRetain(stepped->data[i]);
    for (uint32_t i = 0; i < stepped->count; ++i) {
        PrDomElement* element = stepped->data[i];
        domNotifyStyleBlockUpdate(element);
        domUseConnectionChanged(element);
        domNotifyLinkUpdate(element);
        domMetaConnected(element);
        domEmbedChanged(element, {});
        domCustomConnectionChanged(element);
        domSelectedContentConnected(element);
        if (scripts != nullptr && domIsScript(element) && domIsConnected(element) &&
                (domScriptFlags(element) & DomScriptFlag_ParserInserted) == 0) {
            domNodeRetain(element);
            scripts->push(element);
        }
        // html/#the-dialog-element: a removed dialog is no longer modal. The focus moves from a removed element to the
        // viewport (html/#html-element-removing-steps). A removed element is no longer designated or activated.
        if (element->state != nullptr && !domIsConnected(element))
            element->state->modal = false;
        PrDomDocument* document = element->document;
        if (!domIsConnected(element)) {
            document->focused = document->focused == element ? nullptr : document->focused;
            document->hovered = document->hovered == element ? nullptr : document->hovered;
            document->active = document->active == element ? nullptr : document->active;
            document->target = document->target == element ? nullptr : document->target;
        } else {
            domAutofocusInserted(element);
        }
    }
    for (uint32_t i = 0; i < stepped->count; ++i)
        domNodeRelease(stepped->data[i]);
    stepped->count = 0;
}

// Runs the post-connection steps of a script element that is not parser-inserted (html/#script-processing-model): the
// host prepares it if it is still connected.
void scriptPostConnection(PrDomElement* element) {
    const DomDocumentHost* host = element->document->host;
    if (host != nullptr && host->prepareScript != nullptr && domIsConnected(element) &&
            (domScriptFlags(element) & DomScriptFlag_ParserInserted) == 0)
        host->prepareScript(host->context, element);
}

// Runs the post-connection steps (dom/#concept-node-post-connection-ext) of the script elements that runConnectionSteps
// collected, once the insertion has ended, in tree order, and releases and empties them. A step can run script, which
// can disconnect a script that has yet to take its step.
void runPostConnectionSteps(RawArray<PrDomElement* PR_NONNULL>* scripts) {
    for (uint32_t i = 0; i < scripts->count; ++i) {
        scriptPostConnection(scripts->data[i]);
        domNodeRelease(scripts->data[i]);
    }
    scripts->reset();
}

// dom/#concept-node-children-changed-ext. A removal runs no script's children changed steps
// (html/#script-processing-model), so that no script starts on a child's removal.
void childrenChanged(PrDomNode* parent, bool removal = false) {
    PrDomElement* element = domElementFromNode(parent);
    if (element == nullptr)
        return;
    domNotifyStyleBlockUpdate(element);
    if (domIsScript(element) && !removal)
        scriptPostConnection(element);
    if (element->document->controls)
        domTextareaChildrenChanged(element);
}

// Returns true if a node of `type` comes after `child` among its siblings.
bool followedBy(const PrDomNode* child, PrDomNodeType type) {
    for (const PrDomNode* node = child->next; node != nullptr; node = node->next) {
        if (node->type == type)
            return true;
    }
    return false;
}

// Returns true if a node of `type` comes before `child` among the children of `parent`. A null `child` means the end of
// the children.
bool precededBy(const PrDomNode* parent, const PrDomNode* PR_NULLABLE child, PrDomNodeType type) {
    for (const PrDomNode* node = parent->first; node != child; node = node->next) {
        if (node->type == type)
            return true;
    }
    return false;
}

// Returns the number of element children of a fragment, as none, one, or more than one.
uint32_t elementChildren(const PrDomNode* fragment) {
    uint32_t count = 0;
    for (const PrDomNode* child = fragment->first; child != nullptr && count < 2; child = child->next)
        count += child->type == PrDomNodeType_Element;
    return count;
}

// dom/#concept-node-ensure-pre-insertion-validity, plus the checks that dom/#concept-node-replace makes when `replaced`
// is the child being replaced.
PrStatus validateInsertion(const PrDomNode* parent, const PrDomNode* node, const PrDomNode* PR_NULLABLE child,
        bool replaced) {
    if (parent->type != PrDomNodeType_Document && parent->type != PrDomNodeType_DocumentFragment &&
            parent->type != PrDomNodeType_Element)
        return PrStatus_NotAllowed;
    if (domIsHostIncludingInclusiveAncestor(node, parent))
        return PrStatus_NotAllowed;
    if (child != nullptr && child->parent != parent)
        return PrStatus_NotFound;

    switch (node->type) {
        case PrDomNodeType_DocumentFragment:
        case PrDomNodeType_DocumentType:
        case PrDomNodeType_Element:
        case PrDomNodeType_Comment:
        case PrDomNodeType_ProcessingInstruction:
            break;
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
            if (parent->type == PrDomNodeType_Document)
                return PrStatus_NotAllowed;
            break;
        default:
            return PrStatus_NotAllowed;
    }
    if (node->type == PrDomNodeType_DocumentType && parent->type != PrDomNodeType_Document)
        return PrStatus_NotAllowed;
    if (parent->type != PrDomNodeType_Document)
        return PrStatus_Ok;

    // A document holds at most one element and one doctype, with the doctype first. When replacing `child`, `child`
    // does not count against either.
    auto elementOtherThan = [&](const PrDomNode* PR_NULLABLE skipped) {
        for (const PrDomNode* each = parent->first; each != nullptr; each = each->next) {
            if (each->type == PrDomNodeType_Element && each != skipped)
                return true;
        }
        return false;
    };
    const PrDomNode* skipped = replaced ? child : nullptr;
    bool doctypeAfter = child != nullptr && followedBy(child, PrDomNodeType_DocumentType);
    bool childIsDoctype = child != nullptr && child->type == PrDomNodeType_DocumentType;
    switch (node->type) {
        case PrDomNodeType_DocumentFragment: {
            uint32_t elements = elementChildren(node);
            bool text = false;
            for (const PrDomNode* each = node->first; each != nullptr; each = each->next)
                text |= domIsText(each);
            if (elements > 1 || text)
                return PrStatus_NotAllowed;
            if (elements == 1 && (elementOtherThan(skipped) || (!replaced && childIsDoctype) || doctypeAfter))
                return PrStatus_NotAllowed;
            return PrStatus_Ok;
        }
        case PrDomNodeType_Element:
            if (elementOtherThan(skipped) || (!replaced && childIsDoctype) || doctypeAfter)
                return PrStatus_NotAllowed;
            return PrStatus_Ok;
        case PrDomNodeType_DocumentType: {
            for (const PrDomNode* each = parent->first; each != nullptr; each = each->next) {
                if (each->type == PrDomNodeType_DocumentType && each != skipped)
                    return PrStatus_NotAllowed;
            }
            if (precededBy(parent, child, PrDomNodeType_Element))
                return PrStatus_NotAllowed;
            return PrStatus_Ok;
        }
        default:
            return PrStatus_Ok;
    }
}

// Returns true if `node` is one of the nodes in `nodes`.
bool among(const PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].node == node)
            return true;
    }
    return false;
}

// dom/#convert-nodes-into-a-node
PrStatus nodesIntoNode(PrDomDocument* document, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count, PrDomNode** out) {
    auto nodeAt = [&](uint32_t i) -> PrDomNode* {
        PrDomNode* node = nodes[i].node;
        return node != nullptr ? node : domTextCreate(document, nodes[i].string);
    };
    if (count == 1) {
        *out = nodeAt(0);
        return PrStatus_Ok;
    }
    PrDomNode* fragment = domDocumentFragmentCreate(document);
    for (uint32_t i = 0; i < count; ++i) {
        PrStatus status = domPreInsert(fragment, nodeAt(i), nullptr);
        if (status != PrStatus_Ok)
            return status;
    }
    *out = fragment;
    return PrStatus_Ok;
}

// Appends the code units of `data` for which `keep` returns true, and inserts `insert` before the unit `at`, or at the
// end. A code point is kept whole only if both its units are kept.
template <typename Keep>
void copyUnits(PrTextBuffer* out, PrStringView data, Keep keep, PrStringView insert = {}, uint32_t at = UINT32_MAX) {
    uint32_t unit = 0;
    bool inserted = insert.data == nullptr;
    for (size_t offset = 0; offset < data.size;) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(data, offset, &codePoint);
        uint32_t units = codePoint > 0xffff ? 2 : 1;
        bool whole = keep(unit) && (units == 1 || keep(unit + 1)) && (units == 1 || at != unit + 1);
        if (!inserted && at == unit) {
            prTextBufferAppend(out, insert);
            inserted = true;
        }
        if (whole) {
            prTextBufferAppend(out, PrStringView{data.data + offset, size});
        } else {
            for (uint32_t half = 0; half < units; ++half) {
                if (!inserted && at == unit + half) {
                    prTextBufferAppend(out, insert);
                    inserted = true;
                }
                if (keep(unit + half))
                    prTextBufferAppendCodePoint(out, kPrUtf8Replacement);
            }
        }
        unit += units;
        offset += size;
    }
    if (!inserted)
        prTextBufferAppend(out, insert);
}

// dom/#valid-shadow-host-name
bool isShadowHost(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Article:
        case PrDomTag_Aside:
        case PrDomTag_Blockquote:
        case PrDomTag_Body:
        case PrDomTag_Div:
        case PrDomTag_Footer:
        case PrDomTag_H1:
        case PrDomTag_H2:
        case PrDomTag_H3:
        case PrDomTag_H4:
        case PrDomTag_H5:
        case PrDomTag_H6:
        case PrDomTag_Header:
        case PrDomTag_Main:
        case PrDomTag_Nav:
        case PrDomTag_P:
        case PrDomTag_Section:
        case PrDomTag_Span:
            return true;
        default:
            return domIsValidCustomElementName(domElementName(element));
    }
}

// html/#window-reflecting-body-element-event-handler-set
bool reflectsWindow(PrDomEventHandler handler) {
    switch (handler) {
        case PrDomEventHandler_onblur: case PrDomEventHandler_onerror: case PrDomEventHandler_onfocus:
        case PrDomEventHandler_onload: case PrDomEventHandler_onresize: case PrDomEventHandler_onscroll:
            return true;
        default:
            return false;
    }
}

// html/#event-handler-attributes, run when the attribute `name` of `element` changed to `value`, or was removed if
// `value` is null. The content attribute of an HTML element's event handler sets the handler on the target that
// domEventHandlerTarget decides.
void eventHandlerAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
        const PrRawString* PR_NULLABLE value) {
    // HTML, SVG and MathML elements have event handler content attributes (html/#globaleventhandlers).
    bool handlers = element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg ||
        element->space == PrDomNamespace_MathMl;
    if (!handlers || space != PrDomNamespace_None || !prStringStartsWith(name, "on"))
        return;
    PrDomEventHandler handler = domEventHandlerFromName(name);
    if (handler == PrDomEventHandler_Count)
        return;
    // Only a handler of GlobalEventHandlers has a content attribute, except that on a body or frameset element a
    // handler of WindowEventHandlers has one too.
    DomEventHandlerMixins mixins = domEventHandlerMixins(handler);
    bool body = element->tag == PrDomTag_Body || element->tag == PrDomTag_Frameset;
    if ((mixins & DomEventHandlerMixin_Global) == 0 && !(body && (mixins & DomEventHandlerMixin_Window) != 0))
        return;
    PrDomEventTarget* target = domEventHandlerTarget(element, handler);
    if (target == nullptr)
        return;
    if (value == nullptr)
        domSetEventHandler(target, handler, nullptr);
    else
        domSetEventHandlerRaw(target, handler, prRawStringView(value), element);
}

// Appends the nodes that inserting `node` adds, as a record names them: a fragment's children, or the node itself.
void insertedNodes(PrDomNode* node, RawArray<PrDomNode* PR_NONNULL>* out) {
    if (node->type != PrDomNodeType_DocumentFragment) {
        out->push(node);
        return;
    }
    for (PrDomNode* child = node->first; child != nullptr; child = child->next)
        out->push(child);
}

// dom/#handle-attribute-changes up to the mutation record, for `attribute` of `element`, which is about to change from
// `oldValue`. The null view means no value. Queues the record.
void attributeWillChange(PrDomElement* element, const PrDomAttr* attribute, PrStringView oldValue) {
    if (domMutationsObserved(element->document))
        domQueueAttributeRecord(element, prRawStringView(&attribute->name),
            domNamespaceUrl(element->document, attribute->space), oldValue);
}

// dom/#handle-attribute-changes after the mutation record: runs a custom element's callback, and the attribute change
// steps (dom/#concept-element-attributes-change-ext) of the attributes that have them. Those are the attributes of a
// slot, a slottable and a sheet owner, and event handler attributes, which are the only kind whose name starts with
// "on". The old value is the null view for none. The attribute change steps of a script element
// (html/#script-processing-model): an async attribute added makes its force async false, and a source set while it is
// connected runs its post-connection steps. An SVG script's source is its href, or its XLink href
// (SVG2/interact.html#ScriptElement).
void scriptAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
        const PrRawString* PR_NULLABLE value) {
    if (value == nullptr)
        return;
    bool html = element->space == PrDomNamespace_Html;
    if (html && space == PrDomNamespace_None && name == "async")
        domScriptSetFlags(element, domScriptFlags(element) | DomScriptFlag_NotForceAsync);
    bool source;
    if (html)
        source = space == PrDomNamespace_None && name == "src";
    else
        source = name == "href" && (space == PrDomNamespace_None || space == PrDomNamespace_XLink);
    if (source)
        scriptPostConnection(element);
}

void attributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView oldValue, const PrRawString* PR_NULLABLE value) {
    ++element->document->treeVersion;
    PrStringView newValue = value != nullptr ? prRawStringView(value) : PrStringView{};
    domNotifyAttributeChanged(element, space, name, oldValue, newValue);
    domUsesAttributeChanged(element, space, name);
    if (space == PrDomNamespace_None && name == "class")
        setClasses(element, newValue);
    if (space == PrDomNamespace_None && name == "id") {
        PrStringView id = newValue;
        if (id.size != 0)
            element->flags |= DomNodeFlag_Id;
        else
            element->flags &= DomNodeFlags(~DomNodeFlag_Id);
        if (domIsConnected(element)) {
            PrDomNode* root = domNodeRoot(element);
            removeId(root, oldValue);
            addId(root, element, id);
            domControlsIdChanged(element);
        }
    }
    domCustomAttributeChanged(element, space, name, oldValue, newValue);
    if (space == PrDomNamespace_None && element->document->slots != nullptr)
        domSlotsAttributeChanged(element, name);
    domNotifyOwnerAttributeChanged(element, space, name, value != nullptr ? prRawStringView(value) : PrStringView{});
    if (name.size > 2 && name.data[0] == 'o' && name.data[1] == 'n')
        eventHandlerAttributeChanged(element, space, name, value);
    if (domIsScript(element))
        scriptAttributeChanged(element, space, name, value);
    // An SVG element's href may be in the XLink namespace.
    if (element->space == PrDomNamespace_Svg && (space == PrDomNamespace_None || space == PrDomNamespace_XLink))
        domEmbedChanged(element, name);
    if (element->space != PrDomNamespace_Html || space != PrDomNamespace_None)
        return;
    if (element->tag == PrDomTag_Option && name == "selected")
        domOptionSelectedAttributeChanged(element, value != nullptr);
    if (element->document->controls)
        domControlAttributeChanged(element, name, oldValue);
    if (element->tag == PrDomTag_Meta && (name == "name" || name == "content") && domIsConnected(element))
        domNotifyMetaChanged(element->document);
    domEmbedChanged(element, name);
}

// Tells what depends on the node's data that the data changed.
void dataChanged(PrDomCharacterData* node) {
    domNotifyDataChanged(node);
    domUsesDataChanged(node);
    if (node->parent != nullptr)
        childrenChanged(node->parent);
}

// Replaces the node's data and queues its mutation record. This is the part of dom/#concept-cd-replace after the live
// ranges are updated.
void writeData(PrDomCharacterData* node, PrStringView value) {
    if (domMutationsObserved(node->document))
        domQueueCharacterDataRecord(node, prRawStringView(&node->data));
    prRawStringReplace(&node->data, value.data != nullptr ? value : "");
    dataChanged(node);
}

// Returns the length of UTF-8 text in UTF-16 code units.
uint32_t utf16Length(PrStringView text) {
    uint32_t units = 0;
    for (size_t at = 0; at < text.size;) {
        uint32_t c = 0;
        at += prUtf8Decode(text, at, &c);
        units += c >= 0x10000 ? 2 : 1;
    }
    return units;
}

void destroyDocument(PrDomDocument* document) {
    // No live range remains, because each holds a reference.
    PR_ASSERT(document->ranges == nullptr);
    domSelectionDestroy(document->selection);
    document->selection = nullptr;
    prUrlDestroy(document->url);
    prRawStringReset(&document->contentType);
    prRawStringReset(&document->languagePragma);
    prRawStringReset(&document->languageHeader);
    domMutationObserversDestroy(document);
    domSlotsDestroy(document);
    domUsesDestroy(document->uses);
    document->uses = nullptr;
    domResourcesDestroy(document->resources);
    document->resources = nullptr;
    atomsDestroy(document->namespaces);
    document->namespaces = nullptr;
    atomsDestroy(document->prefixes);
    document->prefixes = nullptr;
    document->formAttributed.reset();
    // A hold is a reference, so none remains.
    document->holds.reset();
    document->autofocusCandidates.reset();
    domNotifyDocumentDestroyed(document);
    domListenersDestroy(document->listeners);
    PrDomNode* node = document->first;
    while (node != nullptr) {
        PrDomNode* next = node->next;
        destroyTree(node);
        node = next;
    }
    // A tree with references holds its document, so the remaining trees have none.
    while (document->detached != nullptr)
        domFreeTree(document->detached);
    idsDestroy(document->ids);
    domClassesDestroy(document->classes);
    if (document->localNames != nullptr)
        atomTableReset(document->localNames);
    prMemoryFree(document->localNames);
    // The inert document outlives the contents of the templates that were just destroyed.
    PrDomDocument* inert = document->templateDocument;
    prMemoryFree(document);
    if (inert != nullptr && inert != document) {
        inert->templateOwner = nullptr;
        domDocumentRelease(inert);
    }
}

// dom/#concept-element-attributes-append for a new attribute, created on the element rather than first as its own root.
// Nothing references the attribute and nothing has an object for it, so it takes none of the steps of an adopted
// attribute. `hash` is domAttributeHash(name).
PrDomAttr* appendNew(PrDomElement* element, DomNamespaceId space, uint8_t prefix, PrStringView name,
        PrStringView value, uint32_t hash) {
    PrDomAttr* attribute = allocZeroed<PrDomAttr>();
    attribute->type = PrDomNodeType_Attribute;
    attribute->document = element->document;
    attribute->space = space;
    attribute->name = prRawStringCopy(name);
    attribute->value = prRawStringCopy(value);
    attribute->hash = hash;
    attribute->prefix = prefix;
    attributeWillChange(element, attribute, PrStringView{});
    attribute->parent = element;
    attribute->previous = element->lastAttribute;
    if (element->lastAttribute != nullptr)
        element->lastAttribute->next = attribute;
    else
        element->firstAttribute = attribute;
    element->lastAttribute = attribute;
    attributeChanged(element, space, prRawStringView(&attribute->name), PrStringView{}, &attribute->value);
    return attribute;
}

// Copies `attribute` into `document`, with its namespace, prefix, name and value (dom/#concept-node-clone).
PrDomAttr* cloneAttribute(const PrDomAttr* attribute, PrDomDocument* document) {
    return domAttrCreate(document, transferNamespace(document, attribute, attribute->space),
        domAttributePrefix(attribute), prRawStringView(&attribute->name), prRawStringView(&attribute->value));
}

void tellHeld(PrDomNode* node, bool held) {
    const PrDomTreeCallback& callback = node->document->treeCallback;
    if (callback.held != nullptr)
        callback.held(callback.context, node, held);
}

} // namespace

PrStringView domNamespaceUrl(const PrDomDocument* document, DomNamespaceId space) {
    if (space >= PrDomNamespace_Other)
        return atomTableText(document->namespaces, space - PrDomNamespace_Other);
    return domNamedNamespaceUrl(space);
}

PrStringView domNamedNamespaceUrl(DomNamespaceId space) {
    switch (space) {
        case PrDomNamespace_Html:   return "http://www.w3.org/1999/xhtml";
        case PrDomNamespace_MathMl: return "http://www.w3.org/1998/Math/MathML";
        case PrDomNamespace_Svg:    return "http://www.w3.org/2000/svg";
        case PrDomNamespace_XLink:  return "http://www.w3.org/1999/xlink";
        case PrDomNamespace_Xml:    return "http://www.w3.org/XML/1998/namespace";
        case PrDomNamespace_XmlNs:  return "http://www.w3.org/2000/xmlns/";
        default:                  return PrStringView{};
    }
}

bool domNamespaceFromUrl(PrDomDocument* document, PrStringView url, DomNamespaceId* out) {
    *out = PrDomNamespace_None;
    if (url.size == 0)
        return true;
    for (DomNamespaceId space = PrDomNamespace_None + 1; space < PrDomNamespace_Other; ++space) {
        if (domNamespaceUrl(document, space) == url) {
            *out = space;
            return true;
        }
    }
    Atom atom = internAtom(&document->namespaces, url, 0x100 - PrDomNamespace_Other);
    if (atom == kAtomNone)
        return false;
    *out = DomNamespaceId(PrDomNamespace_Other + atom);
    return true;
}

PrDomDocument* domDocumentCreate(DomDocumentKind kind) {
    PrDomDocument* document = allocZeroed<PrDomDocument>();
    document->type = PrDomNodeType_Document;
    document->document = document;
    document->flags = DomNodeFlag_Connected;
    document->references = 1;
    document->retained = 1;
    document->kind = kind;
    document->contentType = prRawStringExternal(kind == DomDocumentKind_Html ? "text/html" : "application/xml");
    document->charset = PrCharset_Utf8;
    prUrlCreate("about:blank", nullptr, PrCharset_Utf8, &document->url);
    return document;
}

PrDomDocument* domDocumentTemplateDocument(PrDomDocument* document) {
    if (document->templateDocument == nullptr) {
        PrDomDocument* inert = domDocumentCreate(document->kind);
        inert->templateDocument = inert;
        inert->templateOwner = document;
        domNotifyDocumentCreated(document, inert);
        document->templateDocument = inert;
    }
    return document->templateDocument;
}

const DomDocumentHost* PR_NULLABLE domDocumentMicrotaskHost(const PrDomDocument* document) {
    return document->templateOwner != nullptr ? document->templateOwner->host : document->host;
}

void domNodeRetain(PrDomNode* node) {
    domNodeRetainBy(node, 1);
}

void domNodeRelease(PrDomNode* PR_NULLABLE node) {
    if (node != nullptr)
        domNodeReleaseBy(node, 1);
}

void domNodeRetainBy(PrDomNode* node, uint32_t count) {
    if (count == 0)
        return;
    node->references += count;
    PrDomNode* root = countUp(node, count);
    if (root->type != PrDomNodeType_Document && root->retained == count)
        domDocumentRetain(root->document);
}

void domNodeReleaseBy(PrDomNode* node, uint32_t count) {
    PR_ASSERT(node->references >= count);
    if (count == 0)
        return;
    node->references -= count;
    PrDomNode* root = countDown(node, count);
    if (root->retained == 0)
        treeEmptied(root);
}

void domNodeRetainEach(PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    countEach(nodes, count, true);
}

void domNodeReleaseEach(PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    countEach(nodes, count, false);
}

void domNodeHold(PrDomNode* node) {
    domNodeRetain(node);
    if (node->holds++ != 0)
        return;
    node->document->holds.push(node);
    tellHeld(node, true);
}

void domNodeUnhold(PrDomNode* PR_NULLABLE node) {
    if (node == nullptr)
        return;
    PR_ASSERT(node->holds != 0);
    if (--node->holds == 0) {
        // Holds are mostly released in the reverse of the order in which they were taken.
        RawArray<PrDomNode* PR_NONNULL>& holds = node->document->holds;
        for (uint32_t i = holds.count; i-- != 0;) {
            if (holds.data[i] == node) {
                holds.data[i] = holds.data[--holds.count];
                break;
            }
        }
        tellHeld(node, false);
    }
    domNodeRelease(node);
}

void domDocumentSetTreeCallback(PrDomDocument* document, const PrDomTreeCallback& callback) {
    document->treeCallback = callback;
    for (uint32_t i = 0; i < document->holds.count; ++i)
        tellHeld(document->holds.data[i], true);
}

bool domIsHostIncludingInclusiveAncestor(const PrDomNode* ancestor, const PrDomNode* node) {
    // An attribute is not an ancestor of any node.
    if (node->type == PrDomNodeType_Attribute)
        return ancestor == node;
    for (const PrDomNode* at = node; at != nullptr; at = domNodeOwner(at)) {
        if (at == ancestor)
            return true;
    }
    return false;
}

PrDomNode* domTreeRoot(PrDomNode* node) {
    for (;;) {
        if (domIsConnected(node))
            return node->document;
        PrDomNode* owner = domNodeOwner(node);
        if (owner == nullptr)
            return node;
        node = owner;
    }
}

void domFreeTree(PrDomNode* root) {
    PR_ASSERT(root->retained == 0 && domNodeOwner(root) == nullptr && root->type != PrDomNodeType_Document);
    removeDetached(root);
    destroyTree(root);
}

void domTemplateNameShadow(PrDomElement* element, PrDomShadowRoot* shadow) {
    PrDomNode* contents = domElementContents(element);
    PR_ASSERT(contents != nullptr && domNodeOwner(contents) == element && contents->retained == 0);
    destroyTree(contents);
    domElementState(element)->contents = shadow;
}

PrDomEventTarget* PR_NULLABLE domEventHandlerTarget(PrDomEventTarget* target, PrDomEventHandler handler) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(target));
    if (!domIsHtml(element, PrDomTag_Body) && !domIsHtml(element, PrDomTag_Frameset))
        return target;
    if ((domEventHandlerMixins(handler) & DomEventHandlerMixin_Window) == 0 && !reflectsWindow(handler))
        return target;
    return element->document->global;
}

void domVisitSubtree(PrDomNode* root, void (*visit)(void* context, PrDomNode* node), void* context) {
    RawArray<PrDomNode* PR_NONNULL> roots;
    for (PrDomNode* top = root;; top = roots.data[--roots.count]) {
        for (PrDomNode* node = top; node != nullptr;) {
            visit(context, node);
            if (PrDomElement* element = domElementFromNode(node)) {
                for (PrDomNode* attribute = element->firstAttribute; attribute != nullptr; attribute = attribute->next)
                    visit(context, attribute);
                if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
                    roots.push(shadow);
            }
            if (node->first != nullptr) {
                node = node->first;
                continue;
            }
            while (node != top && node->next == nullptr)
                node = node->parent;
            node = node != top ? node->next : nullptr;
        }
        if (roots.count == 0)
            break;
    }
    roots.reset();
}

void domDocumentRetain(PrDomDocument* document) {
    ++document->retained;
}

void domDocumentRelease(PrDomDocument* PR_NULLABLE document) {
    if (document != nullptr && treeTake(document, 1))
        destroyDocument(document);
}

PrUrl* domDocumentBaseUrl(const PrDomDocument* document) {
    // html/#document-base-url
    if (document->baseElements == 0)
        return prUrlClone(document->url);
    for (const PrDomNode* node = domNodeNext(document, document); node != nullptr; node = domNodeNext(document, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (!domIsHtml(element, PrDomTag_Base))
            continue;
        const PrDomAttr* href = domAttribute(element, PrDomNamespace_None, "href");
        if (href == nullptr)
            continue;
        PrUrl* base = nullptr;
        if (prUrlCreate(prRawStringView(&href->value), document->url, document->charset, &base) == PrStatus_Ok) {
            PrStringView scheme = prUrlScheme(base);
            if (scheme != "data" && scheme != "javascript")
                return base;
            prUrlDestroy(base);
        }
        break;
    }
    return prUrlClone(document->url);
}

PrStatus domDocumentParseUrl(const PrDomDocument* document, PrStringView text, PrUrl* PR_NULLABLE* PR_NONNULL out) {
    PrUrl* base = domDocumentBaseUrl(document);
    PrStatus status = prUrlCreate(text, base, document->charset, out);
    prUrlDestroy(base);
    return status;
}

PrDomElement* domElementCreate(PrDomDocument* document, DomNamespaceId space, PrDomTag tag, PrStringView name) {
    PrDomElement* element = make<PrDomElement>(document, PrDomNodeType_Element);
    element->space = space;
    // A name has one tag in a namespace. The tag is the table's tag if the table names elements of that namespace, and
    // the name's atom otherwise.
    bool named = space == PrDomNamespace_Html || space == PrDomNamespace_Svg || space == PrDomNamespace_MathMl;
    if (tag == PrDomTag_Unknown && named)
        tag = domTagFromText(name);
    element->tag = tag != PrDomTag_Unknown ? DomTagId(tag) : internLocalName(document, name);
    if (space == PrDomNamespace_Html && (tag == PrDomTag_Option || tag == PrDomTag_Selectedcontent))
        document->options = true;
    domControlCreated(element);
    if (space == PrDomNamespace_Html && tag == PrDomTag_Template)
        domElementState(element)->contents = makeOwned<PrDomDocumentFragment>(domDocumentTemplateDocument(document), element);
    return element;
}

PrDomElement* domElementCreateNs(PrDomDocument* document, DomNamespaceId space, PrStringView prefix, PrStringView local) {
    PrDomElement* element = domElementCreate(document, space, PrDomTag_Unknown, local);
    if (prefix.data != nullptr)
        domElementState(element)->prefix = prRawStringCopy(prefix);
    return element;
}

PrDomText* domTextCreate(PrDomDocument* document, PrStringView data) {
    PrDomText* text = make<PrDomText>(document, PrDomNodeType_Text);
    text->data = prRawStringCopy(data);
    return text;
}

PrDomText* domTextCreateCdataSection(PrDomDocument* document, PrStringView data) {
    PrDomText* section = make<PrDomText>(document, PrDomNodeType_CdataSection);
    section->data = prRawStringCopy(data);
    return section;
}

PrDomComment* domCommentCreate(PrDomDocument* document, PrStringView data) {
    PrDomComment* comment = make<PrDomComment>(document, PrDomNodeType_Comment);
    comment->data = prRawStringCopy(data);
    return comment;
}

PrDomProcessingInstruction* domProcessingInstructionCreate(PrDomDocument* document, PrStringView target, PrStringView data) {
    PrDomProcessingInstruction* instruction =
        make<PrDomProcessingInstruction>(document, PrDomNodeType_ProcessingInstruction);
    instruction->target = prRawStringCopy(target);
    instruction->data = prRawStringCopy(data);
    return instruction;
}

PrDomDocumentType* domDocumentTypeCreate(PrDomDocument* document, PrStringView name, PrStringView publicId,
        PrStringView systemId) {
    PrDomDocumentType* doctype = make<PrDomDocumentType>(document, PrDomNodeType_DocumentType);
    doctype->name = prRawStringCopy(name);
    doctype->publicId = prRawStringCopy(publicId);
    doctype->systemId = prRawStringCopy(systemId);
    return doctype;
}

PrDomDocumentFragment* domDocumentFragmentCreate(PrDomDocument* document) {
    return make<PrDomDocumentFragment>(document, PrDomNodeType_DocumentFragment);
}

PrStatus domAttachShadow(PrDomElement* host, const PrDomShadowRootInit* init, PrDomShadowRoot* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!isShadowHost(host) || domShadowRootOfHost(host) != nullptr)
        return PrStatus_NotSupported;
    PrDomShadowRoot* shadow = makeOwned<PrDomShadowRoot>(host->document, host);
    shadow->flags = DomNodeFlag_ShadowRoot;
    if (domIsConnected(host))
        shadow->flags |= DomNodeFlag_Connected;
    shadow->mode = init->mode;
    shadow->delegatesFocus = init->delegatesFocus;
    shadow->slotAssignment = init->slotAssignment;
    shadow->clonable = init->clonable;
    shadow->serializable = init->serializable;
    domElementState(host)->shadowRoot = shadow;
    domSlotsEnable(host->document);
    host->document->slots->scopes = true;
    domNotifyShadowAttached(host);
    *out = shadow;
    return PrStatus_Ok;
}

PrDomShadowRoot* domAttachUseShadow(PrDomElement* use) {
    PR_ASSERT(use->space == PrDomNamespace_Svg && use->tag == PrDomTag_Use && domShadowRootOfHost(use) == nullptr);
    PrDomShadowRoot* shadow = makeOwned<PrDomShadowRoot>(use->document, use);
    shadow->flags = DomNodeFlag_ShadowRoot;
    if (domIsConnected(use))
        shadow->flags |= DomNodeFlag_Connected;
    shadow->mode = PrDomShadowRootMode_Closed;
    domElementState(use)->shadowRoot = shadow;
    domSlotsEnable(use->document);
    domNotifyShadowAttached(use);
    return shadow;
}

bool domEmbeds(const PrDomElement* element) {
    if (element->space == PrDomNamespace_Svg)
        return element->tag == PrDomTag_Image || element->tag == PrDomTag_FeImage;
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Audio:
        case PrDomTag_Canvas:
        case PrDomTag_Embed:
        case PrDomTag_Iframe:
        case PrDomTag_Img:
        case PrDomTag_Input:
        case PrDomTag_Object:
        case PrDomTag_Picture:
        case PrDomTag_Source:
        case PrDomTag_Video:
            return true;
        default:
            return false;
    }
}

void domEmbedChanged(PrDomElement* element, PrStringView attribute) {
    if (!domEmbeds(element))
        return;
    const DomDocumentHost* host = element->document->host;
    if (host != nullptr)
        host->embedChanged(host->context, element, attribute);
}

void domEmbedRemoved(PrDomNode* node, PrDomNode* oldParent, PrDomNode* PR_NULLABLE oldNext) {
    PrDomElement* element = domElementFromNode(node);
    if (element == nullptr || !domEmbeds(element))
        return;
    const DomDocumentHost* host = element->document->host;
    if (host != nullptr && host->embedRemoved != nullptr)
        host->embedRemoved(host->context, element, oldParent, oldNext);
}

bool domIsScript(const PrDomElement* element) {
    return element->tag == PrDomTag_Script &&
        (element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg);
}

DomScriptFlags domScriptFlags(const PrDomElement* element) {
    return element->state != nullptr ? element->state->script : 0;
}

void domScriptSetFlags(PrDomElement* element, DomScriptFlags flags) {
    if (flags != domScriptFlags(element))
        domElementState(element)->script = flags;
}

void domScriptParserCreated(PrDomElement* element, bool fragment) {
    if (!domIsScript(element))
        return;
    DomScriptFlags flags = DomScriptFlag_ParserInserted | DomScriptFlag_NotForceAsync;
    if (fragment)
        flags = DomScriptFlag_Started;
    domScriptSetFlags(element, flags);
}

DomElementState* domElementState(PrDomElement* element) {
    if (element->state == nullptr) {
        element->state = allocZeroed<DomElementState>();
        element->state->prefix = prRawStringNull();
        element->state->returnValue = prRawStringEmpty();
        element->state->imageUrl = prRawStringNull();
    }
    return element->state;
}

PrDomNode* PR_NULLABLE domClone(const PrDomNode* node, PrDomDocument* document, bool deep) {
    PrDomNode* copy = nullptr;
    switch (node->type) {
        case PrDomNodeType_Element: {
            const PrDomElement* element = static_cast<const PrDomElement*>(node);
            // dom/#concept-node-clone creates the copy with the element's is value, and not synchronously.
            const DomCustomElement* custom = element->state != nullptr ? element->state->custom : nullptr;
            bool held = false;
            PrDomElement* clone;
            if (element->space == PrDomNamespace_Html) {
                bool customized = custom != nullptr && !prRawStringIsNull(&custom->is);
                PrStringView is = customized ? prRawStringView(&custom->is) : PrStringView{};
                clone = domElementCreateHtml(document, domElementTag(element), domElementName(element), is, false, &held);
            } else {
                clone = domElementCreate(document, element->space,
                    domTagIsAtom(element->tag) ? PrDomTag_Unknown : PrDomTag(element->tag), domElementName(element));
            }
            for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                    attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                uint8_t prefix;
                if (attribute->prefix == 0 || attribute->document == document)
                    prefix = attribute->prefix;
                else
                    prefix = internPrefix(document, domAttributePrefix(attribute));
                appendNew(clone, transferNamespace(document, attribute, attribute->space), prefix,
                    prRawStringView(&attribute->name), prRawStringView(&attribute->value), attribute->hash);
            }
            // html/#script-processing-model: the cloning steps of a script copy whether it already started.
            if (domIsScript(element))
                domScriptSetFlags(clone, domScriptFlags(element) & DomScriptFlag_Started);
            domControlCloned(element, clone);
            if (deep && domElementContents(element) != nullptr && domElementContents(clone) != nullptr) {
                PrDomNode* contents = domElementContents(clone);
                for (const PrDomNode* child = domElementContents(element)->first; child != nullptr; child = child->next)
                    domAttach(contents, domClone(child, contents->document, true), nullptr);
            }
            copy = clone;
            break;
        }
        case PrDomNodeType_Attribute:
            copy = cloneAttribute(static_cast<const PrDomAttr*>(node), document);
            break;
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
            copy = domTextCreate(document, prRawStringView(&static_cast<const PrDomCharacterData*>(node)->data));
            copy->type = node->type;
            break;
        case PrDomNodeType_Comment:
            copy = domCommentCreate(document, prRawStringView(&static_cast<const PrDomCharacterData*>(node)->data));
            break;
        case PrDomNodeType_ProcessingInstruction: {
            const PrDomProcessingInstruction* instruction = static_cast<const PrDomProcessingInstruction*>(node);
            copy = domProcessingInstructionCreate(document, prRawStringView(&instruction->target),
                prRawStringView(&instruction->data));
            break;
        }
        case PrDomNodeType_DocumentType: {
            const PrDomDocumentType* doctype = static_cast<const PrDomDocumentType*>(node);
            copy = domDocumentTypeCreate(document, prRawStringView(&doctype->name),
                prRawStringView(&doctype->publicId), prRawStringView(&doctype->systemId));
            break;
        }
        case PrDomNodeType_DocumentFragment:
            copy = domDocumentFragmentCreate(document);
            break;
        default:
            return nullptr;
    }
    if (deep) {
        for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
            domAttach(copy, domClone(child, document, true), nullptr);
    }
    return copy;
}

PrRawString domNodeTextContent(const PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_Text:
        case PrDomNodeType_Comment:
        case PrDomNodeType_ProcessingInstruction:
            return prRawStringClone(&static_cast<const PrDomCharacterData*>(node)->data);
        case PrDomNodeType_Attribute:
            return prRawStringClone(&static_cast<const PrDomAttr*>(node)->value);
        case PrDomNodeType_Element:
        case PrDomNodeType_DocumentFragment:
            break;
        default:
            return prRawStringNull();
    }

    PrRawString text = prRawStringEmpty();
    for (const PrDomNode* at = domNodeNext(node, node); at != nullptr; at = domNodeNext(node, at)) {
        if (domIsText(at))
            prRawStringAppend(&text, prRawStringView(&static_cast<const PrDomText*>(at)->data));
    }
    return text;
}

bool domNodeEquals(const PrDomNode* a, const PrDomNode* b) {
    auto same = [](const PrRawString& x, const PrRawString& y) {
        return prRawStringView(&x) == prRawStringView(&y);
    };
    if (a->type != b->type)
        return false;
    switch (a->type) {
        case PrDomNodeType_DocumentType: {
            const PrDomDocumentType* x = static_cast<const PrDomDocumentType*>(a);
            const PrDomDocumentType* y = static_cast<const PrDomDocumentType*>(b);
            if (!same(x->name, y->name) || !same(x->publicId, y->publicId) || !same(x->systemId, y->systemId))
                return false;
            break;
        }
        case PrDomNodeType_Element: {
            const PrDomElement* x = static_cast<const PrDomElement*>(a);
            const PrDomElement* y = static_cast<const PrDomElement*>(b);
            if (x->space != y->space || domElementName(x) != domElementName(y) ||
                    domElementAttributeCount(x) != domElementAttributeCount(y))
                return false;
            for (const PrDomAttr* attribute = x->firstAttribute; attribute != nullptr;
                    attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                const PrDomAttr* other =
                    domAttribute(y, DomNamespaceId(attribute->space), prRawStringView(&attribute->name));
                if (other == nullptr || !same(attribute->value, other->value))
                    return false;
            }
            break;
        }
        case PrDomNodeType_Attribute: {
            const PrDomAttr* x = static_cast<const PrDomAttr*>(a);
            const PrDomAttr* y = static_cast<const PrDomAttr*>(b);
            if (x->space != y->space || !same(x->name, y->name) || !same(x->value, y->value))
                return false;
            break;
        }
        case PrDomNodeType_ProcessingInstruction:
            if (!same(static_cast<const PrDomProcessingInstruction*>(a)->target,
                    static_cast<const PrDomProcessingInstruction*>(b)->target))
                return false;
            [[fallthrough]];
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_Comment:
            if (!same(static_cast<const PrDomCharacterData*>(a)->data, static_cast<const PrDomCharacterData*>(b)->data))
                return false;
            break;
        default:
            break;
    }
    const PrDomNode* x = a->first;
    const PrDomNode* y = b->first;
    for (; x != nullptr && y != nullptr; x = x->next, y = y->next) {
        if (!domNodeEquals(x, y))
            return false;
    }
    return x == nullptr && y == nullptr;
}

const PrDomNode* domNodeRoot(const PrDomNode* node) {
    if (node->type == PrDomNodeType_Attribute)
        return node;
    while (node->parent != nullptr)
        node = node->parent;
    return node;
}

const PrDomNode* domShadowIncludingRoot(const PrDomNode* node) {
    const PrDomNode* root = domNodeRoot(node);
    while (const PrDomShadowRoot* shadow = domNodeShadowRoot(root))
        root = domNodeRoot(shadow->host);
    return root;
}

PrDomNode* PR_NULLABLE domShadowIncludingNext(const PrDomNode* root, const PrDomNode* node) {
    if (const PrDomElement* element = domElementFromNode(node)) {
        if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
            return shadow;
    }
    if (node->first != nullptr)
        return node->first;
    for (const PrDomNode* at = node; at != root;) {
        // A host's children follow its shadow tree.
        if (const PrDomShadowRoot* shadow = domNodeShadowRoot(at)) {
            if (shadow->host->first != nullptr)
                return shadow->host->first;
            at = shadow->host;
            continue;
        }
        if (at->next != nullptr)
            return at->next;
        at = at->parent;
        if (at == nullptr)
            return nullptr;
    }
    return nullptr;
}

uint32_t domNodeDepth(const PrDomNode* node) {
    uint32_t result = 0;
    for (; node->parent != nullptr; node = node->parent)
        ++result;
    return result;
}

// Both nodes belong to the same tree. Compares ancestors at equal depths, then the two branches beneath their common
// parent. Descendants are not visited.
bool domNodePrecedes(const PrDomNode* a, const PrDomNode* b) {
    uint32_t aDepth = domNodeDepth(a);
    uint32_t bDepth = domNodeDepth(b);
    bool aShallower = aDepth < bDepth;
    while (aDepth > bDepth) {
        a = a->parent;
        --aDepth;
    }
    while (bDepth > aDepth) {
        b = b->parent;
        --bDepth;
    }
    if (a == b)
        return aShallower;
    while (a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }
    PR_ASSERT(a->parent != nullptr);
    for (const PrDomNode* next = a->next; next != nullptr; next = next->next) {
        if (next == b)
            return true;
    }
    return false;
}

const PrDomElement* PR_NULLABLE domDocumentElement(const PrDomDocument* document) {
    for (const PrDomNode* child = document->first; child != nullptr; child = child->next) {
        if (const PrDomElement* element = domElementFromNode(child))
            return element;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE domDocumentHead(PrDomDocument* document) {
    return htmlChild(document, PrDomTag_Head, PrDomTag_Head);
}

PrDomElement* PR_NULLABLE domDocumentBody(PrDomDocument* document) {
    return htmlChild(document, PrDomTag_Body, PrDomTag_Frameset);
}

PrDomElement* PR_NULLABLE domElementById(PrDomNode* root, PrStringView id) {
    if (id.size == 0)
        return nullptr;
    if (domKeepsIds(root)) {
        PrDomElement* first = nullptr;
        domElementsById(root, id, &first);
        return first;
    }
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domElementId(element) == id)
            return element;
    }
    return nullptr;
}

bool domKeepsIds(const PrDomNode* root) {
    return root->type == PrDomNodeType_Document || (domNodeShadowRoot(root) != nullptr && domIsConnected(root));
}

uint32_t domElementsById(PrDomNode* root, PrStringView id, PrDomElement* PR_NULLABLE* PR_NONNULL first) {
    PR_ASSERT(domKeepsIds(root));
    *first = nullptr;
    DomIds* ids = *idsOf(root);
    uint32_t at = ids != nullptr && id.size != 0 ? findId(ids, id, idHash(id)) : kHashMissing;
    if (at == kHashMissing)
        return 0;
    DomIdEntry* entry = &ids->entries.data[at];
    if (!firstKnown(ids, entry)) {
        PrDomElement* found = nullptr;
        for (PrDomNode* node = domNodeNext(root, root); node != nullptr && found == nullptr; node = domNodeNext(root, node)) {
            PrDomElement* element = domElementFromNode(node);
            if (element != nullptr && domElementId(element) == id)
                found = element;
        }
        PR_ASSERT(found != nullptr);
        entry->element = found;
        entry->moves = ids->moves;
    }
    *first = entry->element;
    return entry->count;
}

void domResolveIds(PrDomNode* root) {
    PR_ASSERT(domKeepsIds(root));
    DomIds* ids = *idsOf(root);
    if (ids == nullptr)
        return;
    // One walk finds every first that is missing, since it meets the elements of each ID in tree order.
    uint32_t missing = 0;
    for (uint32_t i = 0; i < ids->entries.count; ++i) {
        DomIdEntry* entry = &ids->entries.data[i];
        if (!firstKnown(ids, entry)) {
            entry->element = nullptr;
            entry->moves = ids->moves;
            ++missing;
        }
    }
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr && missing != 0; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || (element->flags & DomNodeFlag_Id) == 0)
            continue;
        PrStringView id = domElementId(element);
        uint32_t at = findId(ids, id, idHash(id));
        if (at != kHashMissing && ids->entries.data[at].element == nullptr) {
            ids->entries.data[at].element = element;
            --missing;
        }
    }
    PR_ASSERT(missing == 0);
}

void domAutofocusInserted(PrDomElement* element) {
    if (!domIsConnected(element) || !hasAutofocus(element))
        return;
    RawArray<PrDomElement* PR_NONNULL>& candidates = element->document->autofocusCandidates;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < candidates.count; ++i) {
        if (candidates.data[i] != element)
            candidates.data[kept++] = candidates.data[i];
    }
    candidates.count = kept;
    candidates.push(element);
}

void domAttach(PrDomNode* parent, PrDomNode* child, PrDomNode* PR_NULLABLE before,
        RawArray<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped) {
    // An attribute hangs off its element rather than being one of its children.
    PR_ASSERT(child->type != PrDomNodeType_Attribute);
    PR_ASSERT(before == nullptr || before->parent == parent);
    if (child->parent != nullptr) {
        // A move within one tree moves the child's references from one line of ancestors to another. The root's count,
        // on which the tree's lifetime depends, is unchanged. The contents of a template count toward the tree of their
        // host, but are in another document, so a move out of them adopts the child.
        if (domTreeRoot(child) == domTreeRoot(parent) && child->document == parent->document) {
            uint32_t count = child->retained;
            PrDomNode* previous = child->parent;
            PrDomNode* from = domNodeRoot(previous);
            unlink(child);
            if (count != 0)
                countDown(previous, count);
            link(parent, child, before, stepped);
            if (count != 0)
                countUp(parent, count);
            // Moving a connected subtree between a document's tree and a shadow tree changes which tree keeps its
            // elements by ID.
            PrDomNode* to = domNodeRoot(parent);
            if (from != to && domIsConnected(child))
                moveIds(child, from, to);
            else if (domIsConnected(child)) {
                if (DomIds** ids = idsOf(to); ids != nullptr && *ids != nullptr)
                    ++(*ids)->moves;
                domUsesMoved(child);
            }
            treeMoved(child);
            return;
        }
        domDetach(child);
    }

    domAdopt(parent->document, child);
    removeDetached(child);
    link(parent, child, before, stepped);
    joinOwner(child);
    treeMoved(child);
}

void domDetach(PrDomNode* child, RawArray<PrDomElement* PR_NONNULL>* PR_NULLABLE stepped) {
    PR_ASSERT(child->type != PrDomNodeType_Attribute);
    PrDomNode* parent = child->parent;
    if (parent == nullptr)
        return;
    unlink(child);
    if (domIsConnected(child)) {
        setConnected(child, false, stepped, parent);
        domControlsDisconnected(child);
    }
    leaveOwner(child, parent);
    treeMoved(child);
}

void domAdopt(PrDomDocument* document, PrDomNode* node) {
    PR_ASSERT(node->type != PrDomNodeType_Document && node->parent == nullptr);
    PrDomDocument* from = node->document;
    if (from == document)
        return;
    removeDetached(node);
    uint32_t moved = 0;
    adoptTree(document, node, &moved);
    addDetached(node);
    moveRecords(from, document);
    // The live ranges in the tree, and the tree itself if anything references it, hold the document.
    if (node->retained != 0)
        ++moved;
    document->retained += moved;
    treeMoved(node);
    const DomDocumentHost* host = document->host;
    if (host != nullptr && host->embedAdopted != nullptr && node->type != PrDomNodeType_Attribute)
        embedsAdopted(host, node);
    from->retained -= moved;
    if (from->retained == 0)
        destroyDocument(from);
}

PrStatus domPreInsert(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child) {
    PrStatus status = validateInsertion(parent, node, child, false);
    if (status != PrStatus_Ok)
        return status;
    domInsert(parent, node, child == node ? node->next : child);
    return PrStatus_Ok;
}

void domInsert(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child) {
    bool connected = domIsConnected(parent);
    bool observed = domMutationsObserved(parent->document);
    PrDomNode* previous = child != nullptr ? child->previous : parent->last;
    RawArray<PrDomNode* PR_NONNULL> added;
    RawArray<PrDomElement* PR_NONNULL> stepped;
    RawArray<PrDomElement* PR_NONNULL> scripts;
    if (observed)
        insertedNodes(node, &added);
    if (node->type == PrDomNodeType_DocumentFragment) {
        if (node->first == nullptr) {
            added.reset();
            return;
        }
        // dom/#concept-node-insert: the fragment's children leave it with a record of their own. The fragment is held
        // through the steps that run as each child is connected, and is freed afterwards if nothing else holds it.
        domNodeRetain(node);
        if (observed)
            domQueueTreeRecord(node, nullptr, 0, added.data, added.count, nullptr, nullptr);
        while (PrDomNode* moved = node->first) {
            domAttach(parent, moved, child, connected ? &stepped : nullptr);
            runConnectionSteps(&stepped, &scripts);
        }
        childrenChanged(node);
        domNodeRelease(node);
    } else {
        // Removing the node from its old place would free it if nothing else held it.
        bool moving = node->parent != nullptr;
        if (moving) {
            domNodeRetain(node);
            domRemove(node);
        }
        domAttach(parent, node, child, connected ? &stepped : nullptr);
        runConnectionSteps(&stepped, &scripts);
        if (moving)
            domNodeRelease(node);
    }
    stepped.reset();
    if (observed) {
        domQueueTreeRecord(parent, added.data, added.count, nullptr, 0, previous, child);
        added.reset();
    }
    childrenChanged(parent);
    runPostConnectionSteps(&scripts);
}

PrStatus domReplace(PrDomNode* parent, PrDomNode* node, PrDomNode* child) {
    PrStatus status = validateInsertion(parent, node, child, true);
    if (status != PrStatus_Ok)
        return status;
    PrDomNode* reference = child->next == node ? node->next : child->next;
    PrDomNode* previous = child->previous == node ? node->previous : child->previous;
    // The parent is held through the change, the node is held until it is inserted, and the child is held until its
    // record is queued.
    domNodeRetain(parent);
    domNodeRetain(node);
    domNodeRetain(child);
    // dom/#concept-node-replace: one record for the child and the nodes that replace it.
    if (node->parent != nullptr)
        domRemove(node);
    bool observed = domMutationsObserved(parent->document);
    RawArray<PrDomNode* PR_NONNULL> added;
    if (observed)
        insertedNodes(node, &added);
    PrDomDocument* document = parent->document;
    // If the node is the child, taking it from its parent already removed it.
    bool removing = child->parent != nullptr;
    if (observed)
        domSuppressMutations(document, true);
    if (removing)
        domRemove(child);
    domInsert(parent, node, reference);
    if (observed)
        domSuppressMutations(document, false);
    if (observed) {
        PrDomNode* removed[] = {child};
        domQueueTreeRecord(parent, added.data, added.count, removed, removing ? 1 : 0, previous, reference);
        added.reset();
    }
    domNodeRelease(node);
    domNodeRelease(child);
    domNodeRelease(parent);
    return PrStatus_Ok;
}

void domReplaceAll(PrDomNode* parent, PrDomNode* PR_NULLABLE node) {
    // The parent is held through the change, the node is held until it is inserted, and the removed children are held
    // until their record is queued.
    domNodeRetain(parent);
    if (node != nullptr)
        domNodeRetain(node);
    if (node != nullptr && node->parent != nullptr)
        domRemove(node);
    // dom/#concept-node-replace-all: one record for the children and the nodes that replace them.
    bool observed = domMutationsObserved(parent->document);
    RawArray<PrDomNode* PR_NONNULL> removed;
    RawArray<PrDomNode* PR_NONNULL> added;
    for (PrDomNode* child = parent->first; observed && child != nullptr; child = child->next) {
        domNodeRetain(child);
        removed.push(child);
    }
    if (observed && node != nullptr)
        insertedNodes(node, &added);
    PrDomDocument* document = parent->document;
    if (observed)
        domSuppressMutations(document, true);
    // The children are removed in tree order, so that each has no earlier sibling left for its removal to reach.
    while (parent->first != nullptr)
        domRemove(parent->first);
    if (node != nullptr)
        domInsert(parent, node, nullptr);
    if (observed) {
        domSuppressMutations(document, false);
        domQueueTreeRecord(parent, added.data, added.count, removed.data, removed.count, nullptr, nullptr);
        for (uint32_t i = 0; i < removed.count; ++i)
            domNodeRelease(removed.data[i]);
        removed.reset();
        added.reset();
    }
    domNodeRelease(node);
    domNodeRelease(parent);
}

void domStringReplaceAll(PrDomNode* parent, PrStringView string) {
    domReplaceAll(parent, string.size != 0 ? domTextCreate(parent->document, string) : nullptr);
}

PrStatus domPreRemove(PrDomNode* parent, PrDomNode* child) {
    if (child->parent != parent)
        return PrStatus_NotFound;
    domRemove(child);
    return PrStatus_Ok;
}

void domRemove(PrDomNode* node) {
    PrDomNode* parent = node->parent;
    PR_ASSERT(parent != nullptr);
    // The parent is held through the steps that follow. Its tree is freed if the node held its last reference.
    domNodeRetain(parent);
    bool connected = domIsConnected(parent);
    if (domMutationsObserved(parent->document)) {
        PrDomNode* removed[] = {node};
        domQueueTreeRecord(parent, nullptr, 0, removed, 1, node->previous, node->next);
    }
    RawArray<PrDomElement* PR_NONNULL> stepped;
    PrDomNode* next = node->next;
    domDetach(node, connected ? &stepped : nullptr);
    PrDomElement* select = domSelectedContentRemoved(node, parent);
    // If steps run, the node is held through them, and releasing it frees it if nothing else holds it.
    bool embeds = domElementFromNode(node) != nullptr && domEmbeds(domElementFromNode(node));
    bool held = stepped.count != 0 || select != nullptr || embeds;
    if (held) {
        domNodeRetain(node);
        runConnectionSteps(&stepped);
        domEmbedRemoved(node, parent, next);
    }
    stepped.reset();
    if (select != nullptr)
        domSelectUpdateSelectedContent(select);
    childrenChanged(parent, true);
    if (held)
        domNodeRelease(node);
    else if (node->retained == 0 && domNodeOwner(node) == nullptr)
        domFreeTree(node);
    domNodeRelease(parent);
}

PrStatus domPrepend(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* node = nullptr;
    PrStatus status = nodesIntoNode(parent->document, nodes, count, &node);
    return status == PrStatus_Ok ? domPreInsert(parent, node, parent->first) : status;
}

PrStatus domAppend(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* node = nullptr;
    PrStatus status = nodesIntoNode(parent->document, nodes, count, &node);
    return status == PrStatus_Ok ? domPreInsert(parent, node, nullptr) : status;
}

PrStatus domReplaceChildren(PrDomNode* parent, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* node = nullptr;
    PrStatus status = nodesIntoNode(parent->document, nodes, count, &node);
    if (status == PrStatus_Ok)
        status = validateInsertion(parent, node, nullptr, false);
    if (status == PrStatus_Ok)
        domReplaceAll(parent, node);
    return status;
}

PrStatus domBefore(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_Ok;
    PrDomNode* viable = node->previous;
    while (viable != nullptr && among(viable, nodes, count))
        viable = viable->previous;
    PrDomNode* inserted = nullptr;
    PrStatus status = nodesIntoNode(node->document, nodes, count, &inserted);
    if (status != PrStatus_Ok)
        return status;
    return domPreInsert(parent, inserted, viable != nullptr ? viable->next : parent->first);
}

PrStatus domAfter(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_Ok;
    PrDomNode* viable = node->next;
    while (viable != nullptr && among(viable, nodes, count))
        viable = viable->next;
    PrDomNode* inserted = nullptr;
    PrStatus status = nodesIntoNode(node->document, nodes, count, &inserted);
    return status == PrStatus_Ok ? domPreInsert(parent, inserted, viable) : status;
}

PrStatus domReplaceWith(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_Ok;
    PrDomNode* viable = node->next;
    while (viable != nullptr && among(viable, nodes, count))
        viable = viable->next;
    PrDomNode* inserted = nullptr;
    PrStatus status = nodesIntoNode(node->document, nodes, count, &inserted);
    if (status != PrStatus_Ok)
        return status;
    // Converting can move `node` into the fragment, which gives it another parent.
    if (node->parent == parent)
        return domReplace(parent, inserted, node);
    return domPreInsert(parent, inserted, viable);
}

void domRemoveFromParent(PrDomNode* node) {
    if (domNodeParent(node) != nullptr)
        domRemove(node);
}

void domSetTextContent(PrDomNode* node, PrStringView value) {
    switch (node->type) {
        case PrDomNodeType_Element:
        case PrDomNodeType_DocumentFragment:
            domStringReplaceAll(node, value);
            return;
        case PrDomNodeType_Attribute:
            // dom/#set-an-existing-attribute-value
            domSetAttributeValue(static_cast<PrDomAttr*>(node), value);
            return;
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_Comment:
        case PrDomNodeType_ProcessingInstruction:
            domSetData(static_cast<PrDomCharacterData*>(node), value);
            return;
        default:
            return;
    }
}

PrRawString domChildTextContent(const PrDomNode* node) {
    PrRawString text = prRawStringEmpty();
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next) {
        if (domIsText(child))
            prRawStringAppend(&text, prRawStringView(&static_cast<const PrDomCharacterData*>(child)->data));
    }
    return text;
}

void domSetData(PrDomCharacterData* node, PrStringView value) {
    if (node->document->ranges != nullptr)
        domRangesDataReplaced(node, 0, domCharacterDataLength(node), utf16Length(value));
    writeData(node, value);
}

void domAppendData(PrDomCharacterData* node, PrStringView data) {
    if (data.size == 0)
        return;
    if (node->document->ranges != nullptr)
        domRangesDataReplaced(node, domCharacterDataLength(node), 0, utf16Length(data));
    if (domMutationsObserved(node->document))
        domQueueCharacterDataRecord(node, prRawStringView(&node->data));
    prRawStringAppend(&node->data, data);
    dataChanged(node);
}

const PrDomAttr* PR_NULLABLE domAttribute(const PrDomElement* element, DomNamespaceId space, PrStringView name) {
    return domAttributeHashed(element, space, name, domAttributeHash(name));
}

const PrDomAttr* PR_NULLABLE domAttributeHashed(const PrDomElement* element, DomNamespaceId space, PrStringView name,
        uint32_t hash) {
    // dom/#concept-element-attributes-get-by-name: HTML names are looked up in ASCII lowercase.
    bool lowered = domIsHtmlInHtmlDocument(element) && space == PrDomNamespace_None;
    return findAttribute(element, space, name, hash, lowered);
}

PrStringView domAttributeValue(const PrDomElement* element, PrStringView name) {
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, name);
    return attribute != nullptr ? prRawStringView(&attribute->value) : PrStringView{};
}

void domSetAttribute(PrDomElement* element, DomNamespaceId space, PrStringView name, PrStringView value) {
    domSetAttributeNs(element, space, PrStringView{}, name, value);
}

void domSetAttributeNs(PrDomElement* element, DomNamespaceId space, PrStringView prefix, PrStringView name, PrStringView value) {
    uint32_t hash = domAttributeHash(name);
    if (PrDomAttr* existing = findAttribute(element, space, name, hash, false))
        domSetAttributeValue(existing, value);
    else
        appendNew(element, space, prefix.data != nullptr ? internPrefix(element->document, prefix) : 0, name, value, hash);
}

PrDomAttr* domAttrCreate(PrDomDocument* document, DomNamespaceId space, PrStringView prefix,
        PrStringView name, PrStringView value) {
    PrDomAttr* attribute = make<PrDomAttr>(document, PrDomNodeType_Attribute);
    attribute->space = space;
    attribute->name = prRawStringCopy(name);
    attribute->value = prRawStringCopy(value);
    attribute->hash = domAttributeHash(name);
    if (prefix.data != nullptr)
        attribute->prefix = internPrefix(document, prefix);
    return attribute;
}

void domAppendAttribute(PrDomElement* element, PrDomAttr* attribute) {
    PR_ASSERT(attribute->parent == nullptr);
    if (attribute->document != element->document)
        domAdopt(element->document, attribute);
    attributeWillChange(element, attribute, PrStringView{});
    removeDetached(attribute);
    attribute->parent = element;
    attribute->previous = element->lastAttribute;
    attribute->next = nullptr;
    if (element->lastAttribute != nullptr)
        element->lastAttribute->next = attribute;
    else
        element->firstAttribute = attribute;
    element->lastAttribute = attribute;
    joinOwner(attribute);
    treeMoved(attribute);
    attributeChanged(element, attribute->space, prRawStringView(&attribute->name), {}, &attribute->value);
}

void domReplaceAttribute(PrDomElement* element, PrDomAttr* old, PrDomAttr* attribute) {
    PR_ASSERT(old->parent == element && attribute->parent == nullptr);
    if (attribute->document != element->document)
        domAdopt(element->document, attribute);
    attributeWillChange(element, old, prRawStringView(&old->value));
    removeDetached(attribute);
    attribute->parent = element;
    attribute->previous = old->previous;
    attribute->next = old->next;
    if (old->previous != nullptr)
        old->previous->next = attribute;
    else
        element->firstAttribute = attribute;
    if (old->next != nullptr)
        old->next->previous = attribute;
    else
        element->lastAttribute = attribute;
    old->parent = nullptr;
    joinOwner(attribute);
    leaveOwner(old, element);
    treeMoved(attribute);
    treeMoved(old);
    attributeChanged(element, old->space, prRawStringView(&old->name), prRawStringView(&old->value), &attribute->value);
    if (old->retained == 0)
        domFreeTree(old);
}

void domSetAttributeValue(PrDomAttr* attribute, PrStringView value) {
    PrDomElement* element = domElementFromNode(attribute->parent);
    if (element == nullptr) {
        prRawStringReplace(&attribute->value, value.data != nullptr ? value : "");
        return;
    }
    attributeWillChange(element, attribute, prRawStringView(&attribute->value));
    PrRawString old = attribute->value;
    attribute->value = prRawStringCopy(value);
    PrStringView name = prRawStringView(&attribute->name);
    attributeChanged(element, attribute->space, name, prRawStringView(&old), &attribute->value);
    prRawStringReset(&old);
}

PrStringView domAttributePrefix(const PrDomAttr* attribute) {
    if (attribute->prefix == 0)
        return PrStringView{};
    return atomTableText(attribute->document->prefixes, attribute->prefix - 1);
}

PrDomAttr* PR_NULLABLE domAttributeByName(const PrDomElement* element, PrStringView qualifiedName) {
    bool lowered = domIsHtmlInHtmlDocument(element);
    for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<PrDomAttr*>(attribute->next)) {
        PrStringView prefix = domAttributePrefix(attribute);
        PrStringView name = prRawStringView(&attribute->name);
        PrStringView local = qualifiedName;
        if (prefix.data != nullptr) {
            if (qualifiedName.size != prefix.size + 1 + name.size || !prStringStartsWith(qualifiedName, prefix) ||
                    qualifiedName.data[prefix.size] != ':')
                continue;
            local = prStringSlice(qualifiedName, prefix.size + 1, name.size);
        }
        if (lowered && attribute->space == PrDomNamespace_None ? prAsciiEqualsLowered(name, local) : name == local)
            return attribute;
    }
    return nullptr;
}

void domRemoveAttribute(PrDomElement* element, DomNamespaceId space, PrStringView name) {
    PrDomAttr* attribute = const_cast<PrDomAttr*>(domAttribute(element, space, name));
    if (attribute != nullptr)
        domRemoveAttributeNode(element, attribute);
}

void domRemoveAttributeNode(PrDomElement* element, PrDomAttr* attribute) {
    DomNamespaceId space = attribute->space;
    attributeWillChange(element, attribute, prRawStringView(&attribute->value));
    if (attribute->previous != nullptr)
        attribute->previous->next = attribute->next;
    else
        element->firstAttribute = static_cast<PrDomAttr*>(attribute->next);
    if (attribute->next != nullptr)
        attribute->next->previous = attribute->previous;
    else
        element->lastAttribute = static_cast<PrDomAttr*>(attribute->previous);
    attribute->parent = nullptr;
    leaveOwner(attribute, element);
    treeMoved(attribute);
    attributeChanged(element, space, prRawStringView(&attribute->name), prRawStringView(&attribute->value), nullptr);
    if (attribute->retained == 0)
        domFreeTree(attribute);
}

bool domIsValidElementLocalName(PrStringView name) {
    if (name.size == 0)
        return false;
    char first = name.data[0];
    if (prAsciiIsLetter(first))
        return bytesAre(name, 1, 0, NameClass_ElementStop);
    return (kNameClasses.of[uint8_t(first)] & NameClass_Start) != 0 && bytesAre(name, 1, NameClass_Rest, 0);
}

bool domIsValidAttributeLocalName(PrStringView name) {
    return name.size != 0 && bytesAre(name, 0, 0, NameClass_AttributeStop);
}

PrStringView domElementId(const PrDomElement* element) {
    constexpr uint32_t kHash = domAttributeHash("id");
    const PrDomAttr* id = domAttributeHashed(element, PrDomNamespace_None, "id", kHash);
    return id != nullptr ? prRawStringView(&id->value) : PrStringView{};
}

PrRawString domElementQualifiedName(const PrDomElement* element) {
    bool prefixed = element->state != nullptr && !prRawStringIsNull(&element->state->prefix);
    if (!prefixed && !domTagIsAtom(element->tag))
        return prRawStringExternal(domTagName(PrDomTag(element->tag)));
    PrTextBuffer name = {};
    if (prefixed) {
        prTextBufferAppend(&name, prRawStringView(&element->state->prefix));
        prTextBufferAppendChar(&name, ':');
    }
    prTextBufferAppend(&name, domElementName(element));
    return prTextBufferTake(&name);
}

PrRawString domElementTagName(const PrDomElement* element) {
    bool prefixed = element->state != nullptr && !prRawStringIsNull(&element->state->prefix);
    if (!prefixed && !domTagIsAtom(element->tag) && domIsHtmlInHtmlDocument(element))
        return prRawStringExternal(domTagUpperName(PrDomTag(element->tag)));
    PrRawString name = domElementQualifiedName(element);
    if (!domIsHtmlInHtmlDocument(element))
        return name;
    PrTextBuffer upper = {};
    prTextBufferAppend(&upper, prRawStringView(&name));
    prRawStringReset(&name);
    prTextBufferAsciiUpper(&upper);
    return prTextBufferTake(&upper);
}

uint32_t domCharacterDataLength(const PrDomCharacterData* node) {
    return utf16Length(prRawStringView(&node->data));
}

PrStatus domSubstringData(const PrDomCharacterData* node, uint32_t offset, uint32_t count, PrRawString* out) {
    uint32_t length = domCharacterDataLength(node);
    if (offset > length)
        return PrStatus_OutOfRange;
    uint32_t end = count > length - offset ? length : offset + count;
    PrTextBuffer buffer = {};
    copyUnits(&buffer, prRawStringView(&node->data), [&](uint32_t unit) { return unit >= offset && unit < end; });
    *out = prTextBufferTake(&buffer);
    return PrStatus_Ok;
}

PrStatus domReplaceData(PrDomCharacterData* node, uint32_t offset, uint32_t count, PrStringView data) {
    uint32_t length = domCharacterDataLength(node);
    if (offset > length)
        return PrStatus_OutOfRange;
    uint32_t end = count > length - offset ? length : offset + count;
    PrTextBuffer buffer = {};
    copyUnits(&buffer, prRawStringView(&node->data), [&](uint32_t unit) { return unit < offset || unit >= end; },
        data.data != nullptr ? data : "", offset);
    if (node->document->ranges != nullptr)
        domRangesDataReplaced(node, offset, end - offset, utf16Length(data));
    writeData(node, prTextBufferView(&buffer));
    prTextBufferReset(&buffer);
    return PrStatus_Ok;
}

PrDomText* PR_NULLABLE domSplitText(PrDomText* node, uint32_t offset) {
    uint32_t length = domCharacterDataLength(node);
    if (offset > length)
        return nullptr;
    PrRawString data = {};
    domSubstringData(node, offset, length - offset, &data);
    PrDomText* created = domTextCreate(node->document, prRawStringView(&data));
    prRawStringReset(&data);
    if (node->parent != nullptr)
        domInsert(node->parent, created, node->next);
    if (node->document->ranges != nullptr)
        domRangesSplit(node, offset, created);
    domReplaceData(node, offset, length - offset, "");
    return created;
}

void domNormalize(PrDomNode* node) {
    // Each exclusive Text node of the subtree takes the data of the Text nodes that follow it, which are removed. An
    // empty one is itself removed.
    auto exclusive = [](const PrDomNode* at) {
        return at != nullptr && at->type == PrDomNodeType_Text;
    };
    for (PrDomNode* at = node->first; at != nullptr;) {
        PrDomNode* next = domNodeNext(node, at);
        if (!exclusive(at)) {
            at = next;
            continue;
        }
        PrDomText* text = static_cast<PrDomText*>(at);
        uint32_t length = domCharacterDataLength(text);
        if (length == 0) {
            next = const_cast<PrDomNode*>(domNodeNextSkipping(node, at));
            domRemove(at);
            at = next;
            continue;
        }
        PrTextBuffer data = {};
        for (PrDomNode* sibling = at->next; exclusive(sibling); sibling = sibling->next)
            prTextBufferAppend(&data, prRawStringView(&static_cast<PrDomText*>(sibling)->data));
        domReplaceData(text, length, 0, prTextBufferView(&data));
        prTextBufferReset(&data);
        for (PrDomNode* sibling = at->next; exclusive(sibling); sibling = sibling->next) {
            if (node->document->ranges != nullptr)
                domRangesMerged(text, length, sibling);
            length += domCharacterDataLength(static_cast<PrDomText*>(sibling));
        }
        while (exclusive(at->next))
            domRemove(at->next);
        at = domNodeNext(node, at);
    }
}

PrStatus domValidateInsertion(const PrDomNode* parent, const PrDomNode* node, const PrDomNode* PR_NULLABLE child) {
    return validateInsertion(parent, node, child, false);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
