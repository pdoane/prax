// Views of bytes: the type that the C API passes binary data as.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

// A view of bytes with no implied encoding or contents. The storage must stay valid while the view is used.
//
// In C++, a view converts implicitly from an array of bytes and from a contiguous run of bytes with data() and size(),
// such as std::vector<uint8_t>, and compares with `==` by its bytes.
typedef struct PrByteView {
    const uint8_t* PR_NULLABLE data;
    size_t size;
#if defined(__cplusplus)
    PrByteView() = default;
    constexpr PrByteView(const uint8_t* PR_NULLABLE start, size_t count) : data(start), size(count) {}

    // Views every byte of the array.
    template <size_t Count>
    constexpr PrByteView(const uint8_t (&bytes)[Count]) : data(bytes), size(Count) {}

    // Views a contiguous run of bytes that has data() and size(), such as std::vector<uint8_t>. data() must convert
    // implicitly to the byte pointer, and size() to a size without narrowing.
    template <typename Bytes>
    requires requires(const Bytes& bytes, const uint8_t* PR_NULLABLE& pointer) {
        pointer = bytes.data();
        size_t{bytes.size()};
    }
    constexpr PrByteView(const Bytes& bytes) : data(bytes.data()), size(bytes.size()) {}

    constexpr const uint8_t* PR_NULLABLE begin() const { return data; }
    constexpr const uint8_t* PR_NULLABLE end() const { return data + size; }

    // The null view equals the empty view.
    friend constexpr bool operator==(PrByteView a, PrByteView b) {
        return a.size == b.size && (a.size == 0 || __builtin_memcmp(a.data, b.data, a.size) == 0);
    }
#endif
} PrByteView;

PR_ASSUME_NONNULL_END
