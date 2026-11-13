// The conversions between UTF-8 and the UTF-16 of Windows' wide-character functions: a helper of the Windows platform,
// which core provides on Windows for any code that calls those functions. A string that is ill-formed, UTF-8 that does
// not decode or UTF-16 with a lone surrogate, does not convert.
//
// prWindowsWiden and prWindowsNarrow return how many units the whole conversion has, and write them to `out` if
// `capacity` holds them all, so that a call with no capacity measures and a second call fills. Neither writes a NUL.
// The other forms allocate what they return. In C++, windowsWiden and windowsNarrow convert into the growable arrays of
// core's C++ layer, which a caller keeps to convert again without allocating.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include <stddef.h>

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What prWindowsWiden and prWindowsNarrow return for a string that is ill-formed.
enum { kPrWindowsStringInvalid = 0xffffffff };

// Converts `text` to UTF-16.
uint32_t prWindowsWiden(PrStringView text, wchar_t* PR_NULLABLE out, uint32_t capacity);

// Converts `count` UTF-16 units to UTF-8.
uint32_t prWindowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity);

// Returns `text` as NUL-terminated UTF-16, which the caller frees with prMemoryFree, and writes the count of its units
// before the NUL to `*outCount`. Returns null if `text` is not UTF-8.
wchar_t* PR_NULLABLE prWindowsWidenCopy(PrStringView text, uint32_t* PR_NULLABLE outCount);

// Returns `count` UTF-16 units as UTF-8: the empty string for no units, and the null string if they are ill-formed.
PrRawString prWindowsNarrowToRawString(const wchar_t* PR_NULLABLE units, uint32_t count);

#if defined(__cplusplus)
}

namespace pr {

// The growable array of core's C++ layer (pr/raw_array.h).
template <typename T>
struct RawArray;

// Writes `text` to `out` as NUL-terminated UTF-16. Returns false, with `out` holding only the NUL, if `text` is not
// UTF-8.
bool windowsWiden(PrStringView text, RawArray<wchar_t>& out);

// Writes `count` UTF-16 units to `out` as UTF-8, and returns the view of them, which is valid until `out` changes.
// Returns the empty view if they are ill-formed.
PrStringView windowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, RawArray<char>& out);

} // namespace pr
#endif

PR_ASSUME_NONNULL_END
