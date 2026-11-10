// The conversions between UTF-8 and the UTF-16 of Windows' wide-character functions, which the Windows backends of
// every library share. A backend links prax::core_windows for them.
//
// Each conversion returns how many units the whole conversion has, and writes them to `out` if `capacity` holds them
// all, so that a call with no capacity measures and a second call fills. Neither writes a NUL.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

#include <stddef.h>

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a string is, which decides what a conversion does with ill-formed input: UTF-8 that does not decode, or UTF-16
// with a lone surrogate.
enum {
    PrCoreWindowsString_Exact,      // a name that must round-trip, such as a path, which fails if ill-formed
    PrCoreWindowsString_Substitute, // text for display, whose ill-formed sequences each become U+FFFD
};
typedef uint8_t PrCoreWindowsString;

// What a conversion of an Exact string returns for ill-formed input.
enum { kPrCoreWindowsStringInvalid = 0xffffffff };

// Converts `text` to UTF-16.
uint32_t prCoreWindowsWiden(PrStringView text, wchar_t* PR_NULLABLE out, uint32_t capacity, PrCoreWindowsString string);

// Converts `count` UTF-16 units to UTF-8.
uint32_t prCoreWindowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity,
    PrCoreWindowsString string);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
