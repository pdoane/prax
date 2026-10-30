// Views of text: the type that the C API passes text as.

#pragma once

#include "pr/base.h"

// C under MSVC has no __builtin_strlen, which PR_SV uses.
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
size_t __cdecl strlen(const char* text);
#define PR_STRLEN_BUILTIN strlen
#else
#define PR_STRLEN_BUILTIN __builtin_strlen
#endif

PR_ASSUME_NONNULL_BEGIN

// A view of text. The text is well-formed UTF-8 unless the declaration that takes or returns the view says otherwise.
// It holds only Unicode scalar values, so no surrogates, and may contain U+0000. It need not be NUL terminated. The
// storage must stay valid while the view is used.
//
// A null `data` with a zero `size` is the null view. Any other `data` with a zero `size` is the empty string.
//
// In C++, a view converts implicitly from a NUL-terminated string and from a contiguous run of chars with data() and
// size(), such as std::string, and compares with `==` by its bytes.
typedef struct PrStringView {
    const char* PR_NULLABLE data;
    size_t size;
#if defined(__cplusplus)
    PrStringView() = default;
    constexpr PrStringView(const char* PR_NULLABLE start, size_t count) : data(start), size(count) {}

    // A null `text` is the null view.
    constexpr PrStringView(const char* PR_NULLABLE text) : data(text), size(text ? __builtin_strlen(text) : 0) {}

    // Views a contiguous run of chars that has data() and size(), such as std::string or std::string_view. data() must
    // convert implicitly to the text pointer, and size() to a size without narrowing.
    template <typename Text>
    requires requires(const Text& text, const char* PR_NULLABLE& pointer) {
        pointer = text.data();
        size_t{text.size()};
    }
    constexpr PrStringView(const Text& text) : data(text.data()), size(text.size()) {}

    constexpr const char* PR_NULLABLE begin() const { return data; }
    constexpr const char* PR_NULLABLE end() const { return data + size; }

    // The null view equals the empty string.
    friend constexpr bool operator==(PrStringView a, PrStringView b) {
        return a.size == b.size && (a.size == 0 || __builtin_memcmp(a.data, b.data, a.size) == 0);
    }
#endif
} PrStringView;

// Returns a view of the NUL-terminated `text`, without the terminator, for C. C++ converts the string implicitly.
// `text` must not be null, and the macro evaluates it twice.
#define PR_SV(text)                    \
    PR_LITERAL(PrStringView) {         \
        (text), PR_STRLEN_BUILTIN(text) \
    }

PR_ASSUME_NONNULL_END
