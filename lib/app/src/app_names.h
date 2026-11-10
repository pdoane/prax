// The names of lib/app's enumerations: what the name functions of its modules share.

#pragma once

#include "pr/base.h"
#include "pr/integer.h"
#include "pr/string_view.h"

#include <stddef.h>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the name of `value` in `names`, or the null view past the last.
template <size_t Count>
PrStringView appNameOf(const char* PR_NONNULL const (&names)[Count], uint32_t value) {
    return value < Count ? PrStringView(names[value]) : PrStringView();
}

// Returns the name of `flag` in `names`, by the flag's bit, or the null view unless it is exactly one of their bits.
template <size_t Count>
PrStringView appFlagNameOf(const char* PR_NONNULL const (&names)[Count], uint32_t flag) {
    return prU64IsPowerOfTwo(flag) ? appNameOf(names, prU64LowestBit(flag)) : PrStringView();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
