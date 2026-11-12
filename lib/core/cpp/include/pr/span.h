// Spans: views of contiguous elements that something else owns.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A pointer and a count of the elements that follow it. A span owns nothing, and is valid while its elements are. A
// span of `const T` reads them, and a span of `T` may change them. An array converts to a span implicitly, so that a
// function that reads a list takes a span and its caller passes a local array, and so does a span of `T` to a span of
// `const T`. A container gives its elements explicitly, with span().
template <typename T>
struct Span {
    constexpr Span() = default;
    constexpr Span(T* PR_NULLABLE data, uint32_t count) : elements(data), used(count) {}

    template <uint32_t N>
    constexpr Span(T (&items)[N]) : elements(items), used(N) {}

    constexpr operator Span<const T>() const requires(!isSame<T, const T>) { return Span<const T>(elements, used); }

    constexpr T* PR_NULLABLE data() const { return elements; }
    constexpr uint32_t count() const { return used; }
    constexpr bool isEmpty() const { return used == 0; }
    constexpr Span<const T> asConst() const { return Span<const T>(elements, used); }

    constexpr T& operator[](uint32_t index) const {
        PR_ASSERT(index < used);
        return elements[index];
    }

    constexpr T* PR_NULLABLE begin() const { return elements; }
    constexpr T* PR_NULLABLE end() const { return elements + used; }

    // Return the first or last element. The span must not be empty.
    constexpr T& first() const {
        PR_ASSERT(used != 0);
        return elements[0];
    }
    constexpr T& last() const {
        PR_ASSERT(used != 0);
        return elements[used - 1];
    }

    // Returns the `sliceCount` elements from `start`, which must lie within the span.
    constexpr Span slice(uint32_t start, uint32_t sliceCount) const {
        PR_ASSERT(start <= used && sliceCount <= used - start);
        return Span(elements + start, sliceCount);
    }

    // Returns the elements from `start` to the end.
    constexpr Span from(uint32_t start) const {
        PR_ASSERT(start <= used);
        return Span(elements + start, used - start);
    }

    friend constexpr bool operator==(Span a, Span b) {
        if (a.used != b.used)
            return false;
        for (uint32_t i = 0; i < a.used; ++i) {
            if (!(a.elements[i] == b.elements[i]))
                return false;
        }
        return true;
    }

private:
    T* PR_NULLABLE elements = nullptr;
    uint32_t used = 0;
};

// A span made from an array or from a pointer and a count is of the elements' type.
template <typename T, uint32_t N>
Span(T (&)[N]) -> Span<T>;
template <typename T>
Span(T* PR_NULLABLE, uint32_t) -> Span<T>;

} // namespace pr

PR_ASSUME_NONNULL_END
