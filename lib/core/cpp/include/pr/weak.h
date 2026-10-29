// Weak references: pointers that are cleared when their target is destroyed.
//
// Counted references point one way. An edge that points back against them is weak.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

struct WeakReference;

// The list of weak references to an object, stored in the object. When the object is destroyed, it clears each
// reference.
struct WeakReferences {
    WeakReference* PR_NULLABLE first;
};

// A weak reference to an object. The holder must reset it before the holder is destroyed.
struct WeakReference {
    void* PR_NULLABLE target;                        // null once the target is gone
    WeakReference* PR_NULLABLE next;                 // the next reference to the same target

    // The pointer that points to this reference: the list's `first`, or the previous reference's `next`.
    WeakReference* PR_NULLABLE* PR_NULLABLE link;
};

// Clears `reference` and removes it from the list of its target.
inline void weakReset(WeakReference* reference) {
    if (reference->link != nullptr) {
        *reference->link = reference->next;
        if (reference->next != nullptr)
            reference->next->link = reference->link;
    }
    *reference = WeakReference{};
}

// Points `reference` at `target`, whose list of weak references is `list`. A null `target` or `list` clears the
// reference.
inline void weakSet(WeakReference* reference, WeakReferences* PR_NULLABLE list,
                    void* PR_NULLABLE target) {
    weakReset(reference);
    if (target == nullptr || list == nullptr)
        return;
    reference->target = target;
    reference->next = list->first;
    reference->link = &list->first;
    if (list->first != nullptr)
        list->first->link = &reference->next;
    list->first = reference;
}

// Returns the target of `reference` as a `T`, or null if the target is gone.
template <typename T>
T* PR_NULLABLE weakGet(const WeakReference& reference) {
    return static_cast<T*>(reference.target);
}

// Clears every weak reference in `list`. The object that holds the list calls this when it is destroyed.
inline void weakClearAll(WeakReferences* list) {
    for (WeakReference* reference = list->first; reference != nullptr;) {
        WeakReference* next = reference->next;
        *reference = WeakReference{};
        reference = next;
    }
    list->first = nullptr;
}

} // namespace pr

PR_ASSUME_NONNULL_END
