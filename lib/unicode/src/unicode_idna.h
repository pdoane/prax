// The IDNA Mapping Table of UTS #46, which tools/gen/gen_unicode.py generates.

#pragma once

#include "pr/unicode_idna.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The Status values of the IDNA Mapping Table.
enum {
    IdnaStatus_Valid,
    IdnaStatus_Ignored,
    IdnaStatus_Mapped,
    IdnaStatus_Deviation,       // valid, or mapped by transitional processing
    IdnaStatus_Disallowed,
};
typedef uint8_t IdnaStatus;

// A code point's row of the table.
struct UnicodeIdnaMapping {
    IdnaStatus status;
    uint32_t count;
    const uint32_t* codePoints;     // what a mapped or deviation code point maps to
};

// Returns the row of `codePoint`, which is at most kPrUtf8MaxCodePoint.
UnicodeIdnaMapping unicodeIdnaMapping(uint32_t codePoint);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
