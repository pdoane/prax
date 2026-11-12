// Arrays: contiguous growable arrays that construct, copy and destroy their elements.
//
// An array is a value: it copies its elements when it is copied, and destroys them when it goes. An implementation in
// lib uses RawArray (pr/raw_array.h) instead, whose owner resets it (lint.py, `managed-include`).

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/construct.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/span.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A growable array that owns its elements. Growth invalidates data() and references into it. An element is moved when
// the array grows, and copied as bytes if it is trivially copyable.
template <typename T>
struct Array {
    Array() = default;

    // Makes `count` elements with no arguments, which zeroes a trivial type. The count is a template parameter, which a
    // braced list cannot deduce, so that `Array<uint32_t> a({5})` is a list of one element.
    template <typename Count>
    explicit Array(Count count) requires(isIntegral<Count> && !isSame<Count, bool>) {
        PR_ASSERT(uint64_t(count) <= 0xffffffffu);
        resize(uint32_t(count));
    }

    // Makes `count` copies of `value`.
    template <typename Count>
    explicit Array(Count count, const T& value) requires(isIntegral<Count> && !isSame<Count, bool>) {
        PR_ASSERT(uint64_t(count) <= 0xffffffffu);
        reserve(uint32_t(count));
        for (uint32_t i = 0; i < uint32_t(count); ++i)
            push(value);
    }

    // Copies the elements of the list `items`, as in `Array<int> a({1, 2, 3})`.
    template <uint32_t N>
    explicit Array(const T (&items)[N]) {
        append(Span(items));
    }

    Array(const Array& other) { append(other.span()); }
    Array(Array&& other) : elements(other.elements), used(other.used), allocated(other.allocated) { other.forget(); }

    Array& operator=(const Array& other) {
        if (this != &other) {
            clear();
            append(other.span());
        }
        return *this;
    }
    Array& operator=(Array&& other) {
        if (this != &other) {
            reset();
            elements = other.elements;
            used = other.used;
            allocated = other.allocated;
            other.forget();
        }
        return *this;
    }

    ~Array() { reset(); }

    T& operator[](uint32_t index) {
        PR_ASSERT(index < used);
        return elements[index];
    }
    const T& operator[](uint32_t index) const {
        PR_ASSERT(index < used);
        return elements[index];
    }

    T* PR_NULLABLE begin() { return elements; }
    T* PR_NULLABLE end() { return elements + used; }
    const T* PR_NULLABLE begin() const { return elements; }
    const T* PR_NULLABLE end() const { return elements + used; }

    // The elements, which are changed through the functions below, and how many there are and there is room for.
    T* PR_NULLABLE data() PR_LIFETIMEBOUND { return elements; }
    const T* PR_NULLABLE data() const PR_LIFETIMEBOUND { return elements; }
    uint32_t count() const { return used; }
    uint32_t capacity() const { return allocated; }
    bool isEmpty() const { return used == 0; }
    // An array of bytes converts implicitly to a PrByteView, as a String converts to a PrStringView, so that it passes
    // wherever the C API takes bytes.
    operator PrByteView() const PR_LIFETIMEBOUND requires(isSame<T, uint8_t>) { return PrByteView{elements, used}; }

    Span<T> span() PR_LIFETIMEBOUND { return Span(elements, used); }
    Span<const T> span() const PR_LIFETIMEBOUND { return Span<const T>(elements, used); }

    // Makes an element from `arguments` at the end, and returns it.
    template <typename... Arguments>
    T& emplace(Arguments&&... arguments) {
        PR_ASSERT(used != 0xffffffffu);
        if (used == allocated)
            reserve(used + 1);
        T* made = construct<T>(elements + used, forward<Arguments>(arguments)...);
        ++used;
        return *made;
    }

    // Appends a copy of `value`, or `value` itself, moved. `value` may be an element of the array.
    void push(const T& value) {
        if (holds(&value)) {
            T copy(value);
            emplace(move(copy));
        } else {
            emplace(value);
        }
    }
    void push(T&& value) {
        if (holds(&value)) {
            T moved(move(value));
            emplace(move(moved));
        } else {
            emplace(move(value));
        }
    }

    // Append copies of the `itemsCount` elements at `items`, or of the elements of `items`, or an array of bytes the
    // bytes of a view, growing once for all of them. The elements must not be in the array.
    void append(const T* PR_NULLABLE items, uint32_t itemsCount) {
        if (itemsCount == 0)
            return;
        PR_ASSERT(itemsCount <= 0xffffffffu - used && !holds(items));
        if (itemsCount > allocated - used)
            reserve(used + itemsCount);
        for (uint32_t i = 0; i < itemsCount; ++i)
            construct<T>(elements + used + i, items[i]);
        used += itemsCount;
    }
    template <typename Item>
    void append(Span<Item> items) requires(isSame<RemoveConst<Item>, T>) {
        append(items.data(), items.count());
    }
    void append(PrByteView bytes) requires(isSame<T, uint8_t>) {
        PR_ASSERT(bytes.size <= 0xffffffffu);
        append(bytes.data, uint32_t(bytes.size));
    }

    // Inserts `value` at `index`, and moves the elements from `index` onward up by one.
    void insert(uint32_t index, T value) {
        PR_ASSERT(index <= used);
        if (index == used) {
            emplace(move(value));
            return;
        }
        // The storage grows before the last element moves into the new slot, so that it moves from where it is.
        if (used == allocated)
            reserve(used + 1);
        construct<T>(elements + used, move(elements[used - 1]));
        ++used;
        for (uint32_t i = used - 2; i > index; --i)
            elements[i] = move(elements[i - 1]);
        elements[index] = move(value);
    }

    // Inserts copies of `items` at `index`, and moves the elements from `index` onward up by their count. The items
    // must not be in the array.
    template <typename Item>
    void insert(uint32_t index, Span<Item> items) requires(isSame<RemoveConst<Item>, T>) {
        uint32_t added = items.count();
        PR_ASSERT(index <= used && added <= 0xffffffffu - used && !holds(items.data()));
        if (added == 0)
            return;
        if (added > allocated - used)
            reserve(used + added);
        // The tail moves up from its end. A slot at or past the old count holds nothing yet, so it is constructed, and
        // a slot below it holds an element that has been moved from, so it is assigned.
        for (uint32_t i = used; i-- > index;) {
            if (i + added >= used)
                construct<T>(elements + i + added, move(elements[i]));
            else
                elements[i + added] = move(elements[i]);
        }
        for (uint32_t i = 0; i < added; ++i) {
            if (index + i < used)
                elements[index + i] = items[i];
            else
                construct<T>(elements + index + i, items[i]);
        }
        used += added;
    }

    // Removes `removedCount` elements starting at `index`, and keeps the order of the rest.
    void erase(uint32_t index, uint32_t removedCount = 1) {
        PR_ASSERT(index <= used && removedCount <= used - index);
        for (uint32_t i = index; i + removedCount < used; ++i)
            elements[i] = move(elements[i + removedCount]);
        for (uint32_t i = used - removedCount; i < used; ++i)
            destroy(elements + i);
        used -= removedCount;
    }

    // Returns the index of the first element equal to `value`, or kArrayMissing if there is none.
    template <typename Value>
    uint32_t find(const Value& value) const {
        for (uint32_t i = 0; i < used; ++i) {
            if (elements[i] == value)
                return i;
        }
        return kArrayMissing;
    }

    // Returns true if an element equals `value`.
    template <typename Value>
    bool contains(const Value& value) const {
        return find(value) != kArrayMissing;
    }

    // Removes every element for which `predicate` returns true, and keeps the order of the rest. Returns how many it
    // removed.
    template <typename Predicate>
    uint32_t removeWhere(Predicate predicate) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < used; ++i) {
            if (predicate(elements[i]))
                continue;
            if (kept != i)
                elements[kept] = move(elements[i]);
            ++kept;
        }
        uint32_t removed = used - kept;
        for (uint32_t i = kept; i < used; ++i)
            destroy(elements + i);
        used = kept;
        return removed;
    }

    // Removes the first element equal to `value`, and keeps the order of the rest. Returns false if there is none.
    template <typename Value>
    bool removeFirst(const Value& value) {
        uint32_t at = find(value);
        if (at == kArrayMissing)
            return false;
        erase(at);
        return true;
    }

    // Returns the last element. The array must not be empty.
    T& last() {
        PR_ASSERT(used != 0);
        return elements[used - 1];
    }
    const T& last() const {
        PR_ASSERT(used != 0);
        return elements[used - 1];
    }

    // Removes the last element. The array must not be empty.
    void pop() {
        PR_ASSERT(used != 0);
        destroy(elements + --used);
    }

    // Sets the count to `newCount`. Elements past the old count are made with no arguments, which zeroes a trivial
    // type; elements past the new count are destroyed.
    void resize(uint32_t newCount) {
        if (newCount < used) {
            for (uint32_t i = newCount; i < used; ++i)
                destroy(elements + i);
        } else {
            if (newCount > allocated)
                reserve(newCount);
            for (uint32_t i = used; i < newCount; ++i)
                construct<T>(elements + i);
        }
        used = newCount;
    }

    // Grows the capacity to at least `minCapacity`, and keeps the elements. Growth at least doubles the capacity.
    void reserve(uint32_t minCapacity) {
        if (minCapacity <= allocated)
            return;
        PR_ASSERT(allocated < kArrayMaxGrowth);
        uint32_t doubled = allocated == 0 ? arrayInitialCapacity<T>() : allocated * 2;
        uint32_t grown = doubled > minCapacity ? doubled : minCapacity;
        if constexpr (isTriviallyCopyable<T>) {
            elements = static_cast<T*>(prMemoryRealloc(elements, size_t(grown) * sizeof(T)));
        } else {
            T* moved = static_cast<T*>(prMemoryAlloc(size_t(grown) * sizeof(T)));
            for (uint32_t i = 0; i < used; ++i) {
                construct<T>(moved + i, move(elements[i]));
                destroy(elements + i);
            }
            if (elements != nullptr)
                prMemoryFree(elements);
            elements = moved;
        }
        allocated = grown;
    }

    // Destroys the elements and keeps the storage.
    void clear() {
        if constexpr (!isTriviallyDestructible<T>) {
            for (uint32_t i = 0; i < used; ++i)
                destroy(elements + i);
        }
        used = 0;
    }

    // Destroys the elements and frees the storage.
    void reset() {
        clear();
        if (elements != nullptr)
            prMemoryFree(elements);
        forget();
    }

    // Returns the elements as a RawArray, which takes the storage, for code that manages it explicitly. The array is
    // left empty, with no storage.
    RawArray<T> take() {
        static_assert(isTriviallyCopyable<T>, "a RawArray copies its elements as bytes");
        RawArray<T> out;
        out.data = elements;
        out.count = used;
        out.capacity = allocated;
        forget();
        return out;
    }

    // Returns true if `a` and `b` hold equal elements in the same order.
    friend bool operator==(const Array& a, const Array& b) { return a.span() == b.span(); }

    // Returns true if `pointer` points into the storage.
    bool holds(const void* PR_NULLABLE pointer) const {
        uintptr_t at = uintptr_t(pointer);
        return at >= uintptr_t(elements) && at < uintptr_t(elements + allocated);
    }

private:
    T* PR_NULLABLE elements = nullptr;
    uint32_t used = 0;
    uint32_t allocated = 0;

    // Drops the storage without destroying anything, for a move that has taken it.
    void forget() {
        elements = nullptr;
        used = 0;
        allocated = 0;
    }
};

} // namespace pr

PR_ASSUME_NONNULL_END
