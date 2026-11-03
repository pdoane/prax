// Raw arrays: contiguous growable arrays of elements that are copied as bytes, with no constructor or destructor run,
// which their owner resets.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The largest capacity that an array can grow from. An array at this capacity can no longer grow.
constexpr uint32_t kArrayMaxGrowth = 0x80000000u;

// The capacity of an array when it first grows: as many elements as fit in kArrayInitialBytes, and at least
// kArrayInitialCount.
constexpr uint32_t kArrayInitialBytes = 64;
constexpr uint32_t kArrayInitialCount = 4;
template <typename T>
uint32_t arrayInitialCapacity() {
    uint32_t bytes = kArrayInitialBytes / uint32_t(sizeof(T));
    return bytes > kArrayInitialCount ? bytes : kArrayInitialCount;
}

// The index that RawArray::find returns when no element equals the value.
constexpr uint32_t kArrayMissing = 0xffffffffu;

// A growable array that owns its storage. Elements are copied as bytes, so they must be trivially copyable, and none is
// constructed or destroyed. The owner frees the storage with `reset`. Growth invalidates `data` and references into it,
// so a value that `push`, `insert` or `append` copies must not be stored in the array itself.
template <typename T>
struct RawArray {
    T* PR_NULLABLE data = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;

    // Appends an uninitialized element and returns it.
    T& emplace() {
        PR_ASSERT(count != 0xffffffffu);
        if (count == capacity)
            reserve(count + 1);
        return data[count++];
    }

    // Appends a copy of `value` at the index that was `count`.
    void push(const T& value) {
        PR_ASSERT(!holds(&value));
        prMemCopy(&emplace(), &value, sizeof(T));
    }

    // Inserts a copy of `value` at `index`, and moves the elements from `index` onward up by one.
    void insert(uint32_t index, const T& value) {
        PR_ASSERT(index <= count && !holds(&value));
        emplace();
        prMemMove(data + index + 1, data + index, size_t(count - index - 1) * sizeof(T));
        prMemCopy(data + index, &value, sizeof(T));
    }

    // Removes `removedCount` elements starting at `index`, and keeps the order of the rest.
    void erase(uint32_t index, uint32_t removedCount = 1) {
        PR_ASSERT(index <= count && removedCount <= count - index);
        if (removedCount == 0)
            return;
        prMemMove(data + index, data + index + removedCount, size_t(count - index - removedCount) * sizeof(T));
        count -= removedCount;
    }

    // Appends copies of the `itemsCount` elements at `items`, growing once for all of them.
    void append(const T* PR_NONNULL items, uint32_t itemsCount) {
        if (itemsCount == 0)
            return;
        PR_ASSERT(itemsCount <= 0xffffffffu - count && !holds(items));
        if (itemsCount > capacity - count)
            reserve(count + itemsCount);
        prMemCopy(data + count, items, size_t(itemsCount) * sizeof(T));
        count += itemsCount;
    }

    // Grows the capacity to at least `minCapacity`, and keeps the elements. Growth at least doubles the capacity.
    void reserve(uint32_t minCapacity);

    // Sets the count to `newCount`, growing once if it is larger than the capacity. Elements past the old count are
    // uninitialized.
    void resize(uint32_t newCount) {
        if (newCount > capacity)
            reserve(newCount);
        count = newCount;
    }

    // Gives an array with no storage `newCount` elements of zeroed bytes. A large array that is touched sparsely costs
    // only what it touches.
    void resizeZeroed(uint32_t newCount) {
        static_assert(isTriviallyCopyable<T>, "a RawArray copies its elements as bytes");
        PR_ASSERT(data == nullptr);
        data = static_cast<T*>(prMemoryAllocZeroed(size_t(newCount) * sizeof(T)));
        count = newCount;
        capacity = newCount;
    }

    // Returns the index of the first element equal to `value`, or kArrayMissing if there is none.
    uint32_t find(const T& value) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (data[i] == value)
                return i;
        }
        return kArrayMissing;
    }

    // Removes the first element equal to `value`, and keeps the order of the rest. Returns false if there is none.
    bool removeFirst(const T& value) {
        uint32_t at = find(value);
        if (at == kArrayMissing)
            return false;
        erase(at);
        return true;
    }

    // Returns the last element. The array must not be empty.
    T& last() {
        PR_ASSERT(count != 0);
        return data[count - 1];
    }
    const T& last() const {
        PR_ASSERT(count != 0);
        return data[count - 1];
    }

    // Removes the last element. The array must not be empty.
    void pop() {
        PR_ASSERT(count != 0);
        --count;
    }

    // Empties the array and keeps its storage.
    void clear() { count = 0; }

    // Frees the storage and leaves an empty array behind.
    void reset() {
        if (data != nullptr)
            prMemoryFree(data);
        data = nullptr;
        count = 0;
        capacity = 0;
    }

    // Returns true if `pointer` points into the storage.
    bool holds(const void* pointer) const {
        uintptr_t at = uintptr_t(pointer);
        return at >= uintptr_t(data) && at < uintptr_t(data + capacity);
    }
};

// The out-of-line definition of RawArray::reserve.
template <typename T>
PR_NOINLINE void RawArray<T>::reserve(uint32_t minCapacity) {
    // Checked where storage is first made, by which point `T` is complete, as it need not be where an array is
    // declared.
    static_assert(isTriviallyCopyable<T>, "a RawArray copies its elements as bytes");
    if (minCapacity <= capacity)
        return;
    PR_ASSERT(capacity < kArrayMaxGrowth);

    uint32_t doubled = capacity == 0 ? arrayInitialCapacity<T>() : capacity * 2;
    uint32_t grown = doubled > minCapacity ? doubled : minCapacity;
    data = static_cast<T*>(prMemoryRealloc(data, size_t(grown) * sizeof(T)));
    capacity = grown;
}

} // namespace pr

PR_ASSUME_NONNULL_END
