// Mutation observers
//
// dom - https://dom.spec.whatwg.org/#mutation-observers

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomMutationObserver;

// A mutation record (dom/#mutationrecord). It holds a reference to each node it names.
struct DomMutationRecord {
    PrDomMutationType type;
    PrDomNode* target;
    Array<PrDomNode* PR_NONNULL> added;
    Array<PrDomNode* PR_NONNULL> removed;
    PrDomNode* PR_NULLABLE previous;
    PrDomNode* PR_NULLABLE next;
    PrString attributeName;       // the null string for a record of another type
    PrString attributeNamespace;  // the null string for no namespace
    PrString oldValue;            // the null string if the observer did not ask for it
};

// The registration of an observer on a node (dom/#registered-observer). The registration holds a reference to the node.
struct DomObserverRegistration {
    PrDomNode* node;
    PrDomMutationObserverInit options;
    Array<PrString> attributeFilter;
};

// The observers registered on a document's nodes. The document holds no references to them. Each observer holds a
// reference to the documents that list it, and removes itself from each list before releasing the reference.
struct DomMutationObservers {
    Array<PrDomMutationObserver* PR_NONNULL> observers;
    uint32_t suppressed;                    // number of mutation algorithms running with their records suppressed
};

// A mutation observer (dom/#mutationobserver). It is reference counted. A script and a queued delivery each hold a
// reference.
struct DomMutationObserver {
    PrDomMutationCallback callback;
    Array<DomObserverRegistration> registrations;
    Array<PrDomMutationRecord* PR_NONNULL> records;
    Array<PrDomDocument* PR_NONNULL> documents;   // documents that list the observer, with a reference to each
    uint32_t references;
    bool pending;                                // true if a microtask is queued to deliver the records
};

// Creates an observer that calls `callback`, with one reference that the caller holds. The observer keeps a copy of the
// callback.
PrDomMutationObserver* domMutationObserverCreate(const PrDomMutationCallback* callback);

// Adds or releases a reference. The last release frees the observer and its records.
void domMutationObserverRetain(PrDomMutationObserver* observer);
void domMutationObserverRelease(PrDomMutationObserver* PR_NULLABLE observer);

// Registers the observer on `target` with `options` and the attribute filter `filter`
// (dom/#dom-mutationobserver-observe). Returns InvalidData for options that ask for nothing, or for old values or a
// filter of a kind that the options do not observe.
PrStatus domMutationObserverObserve(PrDomMutationObserver* observer, PrDomNode* target,
                                    const PrDomMutationObserverInit* options,
                                    const PrStringView* PR_NULLABLE filter, uint32_t filterCount);

// Removes the observer's registrations and drops its records (dom/#dom-mutationobserver-disconnect).
void domMutationObserverDisconnect(PrDomMutationObserver* observer);

// Takes the observer's oldest record into `record`, in place of what it held. Returns false, and leaves `record`
// unchanged, if the observer holds no records.
bool domMutationObserverTake(PrDomMutationObserver* observer, PrDomMutationRecord* record);

// Frees the record, and releases the nodes that it names.
void domMutationRecordDestroy(PrDomMutationRecord* PR_NULLABLE record);

// Notifies the observers registered on the document's nodes (dom/#notify-mutation-observers). Each observer with
// records takes them, in registration order.
void domMutationObserversDeliver(PrDomDocument* document);

// Frees the document's list when the document is destroyed. No observer may be on it.
void domMutationObserversDestroy(PrDomDocument* document);

// Lists in `to` the observers of `from` that are registered on a node now in `to`. domAdopt calls this when it moves a
// subtree between documents.
void domMutationObserversAdopted(PrDomDocument* from, PrDomDocument* to);

// Queues a mutation record for an attribute of `target`, before the attribute changes from `oldValue`
// (dom/#queue-a-mutation-record).
void domQueueAttributeRecord(PrDomNode* target, PrStringView name, PrStringView space,
                             PrStringView oldValue);

// Queues a mutation record for the data of `target`, before the data changes from `oldValue` (dom/#concept-cd-replace).
void domQueueCharacterDataRecord(PrDomNode* target, PrStringView oldValue);

// Queues a mutation record for a change to the children of `target` (dom/#queue-a-tree-mutation-record). `added` and
// `removed` are the nodes that the change added and removed, between `previous` and `next`.
void domQueueTreeRecord(PrDomNode* target, PrDomNode* const PR_NONNULL* PR_NULLABLE added,
                        uint32_t addedCount, PrDomNode* const PR_NONNULL* PR_NULLABLE removed,
                        uint32_t removedCount, PrDomNode* PR_NULLABLE previous,
                        PrDomNode* PR_NULLABLE next);

// Returns true if a record of a change to the document's nodes could reach an observer, which means that some node of
// the document is observed and records are not suppressed. A mutation algorithm checks this before it gathers the
// contents of a record.
inline bool domMutationsObserved(const PrDomDocument* document) {
    return document->observers != nullptr && document->observers->observers.count != 0
        && document->observers->suppressed == 0;
}

// Returns true if records of the document's changes are suppressed. A mutation algorithm that is built from other
// algorithms suppresses their records and queues one of its own (dom/#concept-node-replace-all).
bool domMutationsSuppressed(const PrDomDocument* document);

// Begins suppressing records if `suppress` is set, and otherwise ends the suppression that the matching call began.
// Suppressions nest.
void domSuppressMutations(PrDomDocument* document, bool suppress);

} // namespace pr_impl

// C opaque types
struct PrDomMutationRecord : pr_impl::DomMutationRecord {};
struct PrDomMutationObserver : pr_impl::DomMutationObserver {};

PR_ASSUME_NONNULL_END
