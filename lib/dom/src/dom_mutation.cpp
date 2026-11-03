#include "dom/src/dom_mutation.h"

#include "pr/alloc.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"

#include "dom/src/dom_node.h"
#include "dom/src/dom_slot.h"

namespace pr_impl {
namespace {

void resetRegistration(DomObserverRegistration* registration) {
    stringsReset(&registration->attributeFilter);
    domNodeRelease(registration->node);
}

DomMutationObservers* observersOf(PrDomDocument* document) {
    if (document->observers == nullptr)
        document->observers = allocZeroed<DomMutationObservers>();
    return document->observers;
}

// Adds the observer to the document's list, if it is not on it yet.
void list(PrDomMutationObserver* observer, PrDomDocument* document) {
    if (observer->documents.find(document) != kArrayMissing)
        return;
    domDocumentRetain(document);
    observer->documents.push(document);
    observersOf(document)->observers.push(observer);
}

// Removes the observer from every list that it is on, and drops its registrations and records. Then releases the
// documents, which may destroy them.
void clear(PrDomMutationObserver* observer) {
    for (uint32_t i = 0; i < observer->documents.count; ++i)
        observer->documents.data[i]->observers->observers.removeFirst(observer);
    for (uint32_t i = 0; i < observer->registrations.count; ++i)
        resetRegistration(&observer->registrations.data[i]);
    observer->registrations.clear();
    for (uint32_t i = 0; i < observer->records.count; ++i)
        domMutationRecordDestroy(observer->records.data[i]);
    observer->records.clear();
    RawArray<PrDomDocument* PR_NONNULL> documents = observer->documents;
    observer->documents = {};
    for (uint32_t i = 0; i < documents.count; ++i)
        domDocumentRelease(documents.data[i]);
    documents.reset();
}

// dom/#notify-mutation-observers, for one observer: passes its records to its callback, which takes them. Then the
// slots of its documents fire, as dom/#notify-mutation-observers requires.
void notify(void* data) {
    PrDomMutationObserver* observer = static_cast<PrDomMutationObserver*>(data);
    observer->pending = false;
    if (observer->records.count != 0)
        observer->callback.invoke(observer->callback.context, reinterpret_cast<PrDomMutationObserver*>(observer));
    for (uint32_t i = 0; i < observer->documents.count; ++i)
        domSlotsFire(observer->documents.data[i]);
    if (observer->callback.pending != nullptr)
        observer->callback.pending(observer->callback.context, reinterpret_cast<PrDomMutationObserver*>(observer), false);
    domMutationObserverRelease(observer);
}

// An observer that is interested in a record, and whether the record takes the old value
// (dom/#queue-a-mutation-record).
struct Interested {
    PrDomMutationObserver* observer;
    bool oldValue;
};

// Returns the observer's registration on `node`, or null if it has none.
const DomObserverRegistration* PR_NULLABLE registrationOn(const PrDomMutationObserver* observer, const PrDomNode* node) {
    for (uint32_t i = 0; i < observer->registrations.count; ++i) {
        if (observer->registrations.data[i].node == node)
            return &observer->registrations.data[i];
    }
    return nullptr;
}

PrDomNode* PR_NULLABLE retained(PrDomNode* PR_NULLABLE node) {
    if (node != nullptr)
        domNodeRetain(node);
    return node;
}

void queueRecord(PrDomNode* target, PrDomMutationType type, PrStringView name, PrStringView space,
        PrStringView oldValue, PrDomNode* const PR_NONNULL* PR_NULLABLE added,
        uint32_t addedCount, PrDomNode* const PR_NONNULL* PR_NULLABLE removed,
        uint32_t removedCount, PrDomNode* PR_NULLABLE previous, PrDomNode* PR_NULLABLE next) {
    PrDomDocument* document = target->document;
    DomMutationObservers* observers = document->observers;
    if (observers == nullptr || observers->suppressed != 0)
        return;
    RawArray<Interested> interested;
    for (PrDomNode* node = target; node != nullptr; node = domNodeParent(node)) {
        for (uint32_t k = 0; k < observers->observers.count; ++k) {
            PrDomMutationObserver* observer = observers->observers.data[k];
            const DomObserverRegistration* registration = registrationOn(observer, node);
            if (registration == nullptr)
                continue;
            const PrDomMutationObserverInit& options = registration->options;
            if (node != target && !options.subtree)
                continue;
            if (type == PrDomMutationType_Attributes) {
                if (!options.attributes)
                    continue;
                bool filtered = registration->attributeFilter.count != 0 || options.hasAttributeFilter;
                bool listed = false;
                for (uint32_t j = 0; j < registration->attributeFilter.count; ++j)
                    listed |= prRawStringView(&registration->attributeFilter.data[j]) == name;
                if (filtered && (!listed || space.size != 0))
                    continue;
            }
            if ((type == PrDomMutationType_CharacterData && !options.characterData) ||
                    (type == PrDomMutationType_ChildList && !options.childList))
                continue;
            bool old = (type == PrDomMutationType_Attributes && options.attributeOldValue) ||
                (type == PrDomMutationType_CharacterData && options.characterDataOldValue);
            uint32_t at = 0;
            while (at < interested.count && interested.data[at].observer != observer)
                ++at;
            if (at == interested.count)
                interested.push(Interested{observer, old});
            else
                interested.data[at].oldValue |= old;
        }
    }
    for (uint32_t i = 0; i < interested.count; ++i) {
        PrDomMutationObserver* observer = interested.data[i].observer;
        PrDomMutationRecord* record = allocZeroed<PrDomMutationRecord>();
        record->type = type;
        record->target = retained(target);
        for (uint32_t j = 0; j < addedCount; ++j)
            record->added.push(retained(added[j]));
        for (uint32_t j = 0; j < removedCount; ++j)
            record->removed.push(retained(removed[j]));
        record->previous = retained(previous);
        record->next = retained(next);
        record->attributeName = prRawStringCopy(name);
        record->attributeNamespace = space.size != 0 ? prRawStringCopy(space) : prRawStringNull();
        record->oldValue = interested.data[i].oldValue ? prRawStringCopy(oldValue) : prRawStringNull();
        observer->records.push(record);
        // dom/#queue-a-mutation-observer-compound-microtask
        const DomDocumentHost* host = domDocumentMicrotaskHost(document);
        if (!observer->pending && host != nullptr && host->queueMicrotask(host->context, notify, observer)) {
            observer->pending = true;
            domMutationObserverRetain(observer);
            if (observer->callback.pending != nullptr)
                observer->callback.pending(observer->callback.context,
                    reinterpret_cast<PrDomMutationObserver*>(observer), true);
        }
    }
    interested.reset();
}

} // namespace

PrDomMutationObserver* domMutationObserverCreate(const PrDomMutationCallback* callback) {
    PrDomMutationObserver* observer = allocZeroed<PrDomMutationObserver>();
    observer->callback = *callback;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    observer->references = 1;
    return observer;
}

void domMutationObserverRetain(PrDomMutationObserver* observer) {
    ++observer->references;
}

void domMutationObserverRelease(PrDomMutationObserver* PR_NULLABLE observer) {
    if (observer == nullptr || --observer->references != 0)
        return;
    clear(observer);
    observer->registrations.reset();
    observer->records.reset();
    if (observer->callback.release != nullptr)
        observer->callback.release(observer->callback.context);
    prMemoryFree(observer);
}

PrStatus domMutationObserverObserve(PrDomMutationObserver* observer, PrDomNode* target,
        const PrDomMutationObserverInit* options, const PrStringView* PR_NULLABLE filter, uint32_t filterCount) {
    // dom/#dom-mutationobserver-observe: an old value or a filter implies the kind of change that it applies to.
    PrDomMutationObserverInit effective = *options;
    if (!effective.hasAttributes && (effective.attributeOldValue || effective.hasAttributeFilter))
        effective.attributes = true;
    if (!effective.hasCharacterData && effective.characterDataOldValue)
        effective.characterData = true;
    if ((!effective.childList && !effective.attributes && !effective.characterData) ||
            (effective.attributeOldValue && !effective.attributes) ||
            (effective.hasAttributeFilter && !effective.attributes) ||
            (effective.characterDataOldValue && !effective.characterData))
        return PrStatus_InvalidData;
    for (uint32_t i = 0; i < observer->registrations.count; ++i) {
        DomObserverRegistration* registration = &observer->registrations.data[i];
        if (registration->node == target) {
            stringsClear(&registration->attributeFilter);
            for (uint32_t j = 0; j < filterCount; ++j)
                registration->attributeFilter.push(prRawStringCopy(filter[j]));
            registration->options = effective;
            return PrStatus_Ok;
        }
    }
    DomObserverRegistration registration = {target, effective, {}};
    for (uint32_t j = 0; j < filterCount; ++j)
        registration.attributeFilter.push(prRawStringCopy(filter[j]));
    domNodeRetain(target);
    observer->registrations.push(registration);
    list(observer, target->document);
    return PrStatus_Ok;
}

void domMutationObserverDisconnect(PrDomMutationObserver* observer) {
    domMutationObserverRetain(observer);
    clear(observer);
    domMutationObserverRelease(observer);
}

bool domMutationObserverTake(PrDomMutationObserver* observer, PrDomMutationRecord* record) {
    if (observer->records.count == 0)
        return false;
    // The queued record gives its contents to `record`, and takes what `record` held with it when it is destroyed.
    PrDomMutationRecord* queued = observer->records.data[0];
    observer->records.erase(0, 1);
    DomMutationRecord held = *record;
    *static_cast<DomMutationRecord*>(record) = *queued;
    *static_cast<DomMutationRecord*>(queued) = held;
    domMutationRecordDestroy(queued);
    return true;
}

void domMutationRecordDestroy(PrDomMutationRecord* PR_NULLABLE record) {
    if (record == nullptr)
        return;
    domNodeRelease(record->target);
    for (uint32_t i = 0; i < record->added.count; ++i)
        domNodeRelease(record->added.data[i]);
    for (uint32_t i = 0; i < record->removed.count; ++i)
        domNodeRelease(record->removed.data[i]);
    domNodeRelease(record->previous);
    domNodeRelease(record->next);
    record->added.reset();
    record->removed.reset();
    prRawStringReset(&record->attributeName);
    prRawStringReset(&record->attributeNamespace);
    prRawStringReset(&record->oldValue);
    prMemoryFree(record);
}

void domMutationObserversDeliver(PrDomDocument* document) {
    DomMutationObservers* observers = document->observers;
    if (observers == nullptr)
        return;
    RawArray<PrDomMutationObserver* PR_NONNULL> delivering;
    for (uint32_t i = 0; i < observers->observers.count; ++i) {
        PrDomMutationObserver* observer = observers->observers.data[i];
        if (observer->records.count != 0) {
            domMutationObserverRetain(observer);
            delivering.push(observer);
        }
    }
    for (uint32_t i = 0; i < delivering.count; ++i) {
        PrDomMutationObserver* observer = delivering.data[i];
        if (observer->records.count != 0)
            observer->callback.invoke(observer->callback.context, reinterpret_cast<PrDomMutationObserver*>(observer));
        domMutationObserverRelease(observer);
    }
    delivering.reset();
}

void domMutationObserversDestroy(PrDomDocument* document) {
    DomMutationObservers* observers = document->observers;
    if (observers == nullptr)
        return;
    PR_ASSERT(observers->observers.count == 0);
    observers->observers.reset();
    prMemoryFree(observers);
    document->observers = nullptr;
}

void domMutationObserversAdopted(PrDomDocument* from, PrDomDocument* to) {
    if (from->observers == nullptr)
        return;
    for (uint32_t i = 0; i < from->observers->observers.count; ++i) {
        PrDomMutationObserver* observer = from->observers->observers.data[i];
        for (uint32_t j = 0; j < observer->registrations.count; ++j) {
            if (observer->registrations.data[j].node->document == to) {
                list(observer, to);
                break;
            }
        }
    }
}

void domQueueAttributeRecord(PrDomNode* target, PrStringView name, PrStringView space, PrStringView oldValue) {
    queueRecord(target, PrDomMutationType_Attributes, name, space, oldValue, nullptr, 0, nullptr, 0, nullptr, nullptr);
}

void domQueueCharacterDataRecord(PrDomNode* target, PrStringView oldValue) {
    queueRecord(target, PrDomMutationType_CharacterData, PrStringView{}, PrStringView{}, oldValue,
        nullptr, 0, nullptr, 0, nullptr, nullptr);
}

void domQueueTreeRecord(PrDomNode* target, PrDomNode* const PR_NONNULL* PR_NULLABLE added,
        uint32_t addedCount, PrDomNode* const PR_NONNULL* PR_NULLABLE removed,
        uint32_t removedCount, PrDomNode* PR_NULLABLE previous, PrDomNode* PR_NULLABLE next) {
    if (addedCount == 0 && removedCount == 0)
        return;
    queueRecord(target, PrDomMutationType_ChildList, PrStringView{}, PrStringView{}, PrStringView{},
        added, addedCount, removed, removedCount, previous, next);
}

bool domMutationsSuppressed(const PrDomDocument* document) {
    return document->observers != nullptr && document->observers->suppressed != 0;
}

void domSuppressMutations(PrDomDocument* document, bool suppress) {
    DomMutationObservers* observers = observersOf(document);
    if (suppress)
        ++observers->suppressed;
    else
        --observers->suppressed;
}

} // namespace pr_impl
