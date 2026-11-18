// The NUL-terminated strings of the C functions of POSIX: a helper of the Unix-like platforms, which core provides on
// them for any code that calls those functions. A string that contains a NUL, which such a function would read as its
// end, does not convert.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns a NUL-terminated copy of `text`, which the caller frees with prMemoryFree. Returns null if `text` contains a
// NUL.
char* PR_NULLABLE prPosixStringCopy(PrStringView text);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
