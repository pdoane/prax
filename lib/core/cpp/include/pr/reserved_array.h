// Reserved arrays: storage with a known bound, reserved as address space (pr/vmem.h) and committed as it fills.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/vmem.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The smallest storage worth reserving. Smaller storage is allocated instead.
constexpr size_t kVmWorthwhile = 64 * 1024;

// The size of one commit, which spans many pages.
constexpr size_t kVmCommitStep = 1024 * 1024;

// An array whose capacity is set by `prepare` to a bound on the count. The count never exceeds the capacity, so the
// array never moves while it fills. A pointer into it stays valid until the next `prepare` or `reset`.
template <typename T>
struct ReservedArray {
    T* PR_NULLABLE data = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;    // the number of elements reserved
    uint32_t committed = 0;   // the number of elements committed so far
    bool mapped = false;      // reserved with prVmemReserve rather than allocated

    // Empties the array and makes room for `elements`. Storage that is already large enough is kept, with its committed
    // pages. Smaller storage is released, and new storage takes its place.
    void prepare(uint32_t elements) {
        if (elements <= capacity) {
            count = 0;
            return;
        }

        reset();
        size_t bytes = size_t(elements) * sizeof(T);
        mapped = bytes >= kVmWorthwhile;
        if (!mapped) {
            data = static_cast<T*>(prMemoryAlloc(bytes));
            committed = elements;
        } else {
            data = static_cast<T*>(prVmemReserve(bytes));
        }
        capacity = elements;
    }

    // Frees the storage and leaves the array empty.
    void reset() {
        if (mapped)
            prVmemFree(data, size_t(capacity) * sizeof(T));
        else
            prMemoryFree(data);
        data = nullptr;
        count = 0;
        capacity = 0;
        committed = 0;
        mapped = false;
    }

    // Commits the first `elements` of the reservation, regardless of the count.
    void commitTo(uint32_t elements) {
        PR_ASSERT(elements <= capacity);
        while (committed < elements)
            commitMore();
    }

    // Commits room for `elements` past `count` and returns a pointer to it, ready to be written. The caller then adds
    // the number of elements that it wrote to `count`.
    T* PR_NONNULL room(uint32_t elements) {
        PR_ASSERT(count + elements <= capacity);
        while (committed < count + elements)
            commitMore();
        return data + count;
    }

    // Appends a copy of `value` and returns its index. The array must not be full.
    uint32_t push(const T& value) {
        PR_ASSERT(count < capacity);
        if (count == committed)
            commitMore();
        uint32_t index = count++;
        memCopy(data + index, &value, sizeof(T));
        return index;
    }

    // Commits the next step of the reservation, or the rest of it if less than a step is left.
    PR_NOINLINE void commitMore() {
        uint32_t step = uint32_t(kVmCommitStep / sizeof(T));
        if (step == 0)
            step = 1;

        uint32_t wanted = capacity - committed < step ? capacity : committed + step;
        prVmemCommit(data, size_t(wanted) * sizeof(T));
        committed = wanted;
    }
};

} // namespace pr

PR_ASSUME_NONNULL_END
