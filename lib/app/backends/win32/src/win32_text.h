// The conversions between UTF-8 and the UTF-16 of Windows that the Win32 backend makes.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Writes `text` to `out` as NUL-terminated UTF-16. Ill-formed UTF-8 becomes U+FFFD.
void win32Widen(PrStringView text, Array<wchar_t>& out);

// The length in UTF-8 of `count` UTF-16 units.
uint32_t win32Utf8Length(const wchar_t* units, int count);

// Writes `count` UTF-16 units to `out` as UTF-8, and returns the view of them, which is valid until `out` changes. A
// lone surrogate becomes U+FFFD.
PrStringView win32Narrow(const wchar_t* units, int count, Array<char>& out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
