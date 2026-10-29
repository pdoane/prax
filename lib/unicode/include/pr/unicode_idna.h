// Unicode IDNA Compatibility Processing (UTS #46, https://www.unicode.org/reports/tr46/): the conversion of a domain
// name between its Unicode form and its ASCII form, in which each label that is not ASCII is Punycode.
//
// tools/gen/gen_unicode.py takes the IDNA Mapping Table from the Unicode Character Database.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The input flags of UTS #46 processing. Each flag that is absent is false.
enum {
    PrUnicodeIdnaFlag_CheckHyphens = 1u << 0,          // reject hyphens in the third and fourth places or at either end
    PrUnicodeIdnaFlag_CheckBidi = 1u << 1,             // apply RFC 5893's rules to a name with a right-to-left label
    PrUnicodeIdnaFlag_CheckJoiners = 1u << 2,          // apply RFC 5892's ContextJ rules to ZWNJ and ZWJ
    PrUnicodeIdnaFlag_UseStd3AsciiRules = 1u << 3,     // allow only lowercase letters, digits and hyphens among ASCII
    PrUnicodeIdnaFlag_Transitional = 1u << 4,          // map the deviation characters, as IDNA2003 does
    PrUnicodeIdnaFlag_VerifyDnsLength = 1u << 5,       // limit a name to 253 bytes and each label to 1 to 63
    PrUnicodeIdnaFlag_IgnoreInvalidPunycode = 1u << 6, // keep a label whose Punycode does not decode instead of failing

    // Every check, as UTS #46's conformance tests and the URL Standard's strict domain parser set them.
    PrUnicodeIdnaFlag_Strict = PrUnicodeIdnaFlag_CheckHyphens | PrUnicodeIdnaFlag_CheckBidi
                               | PrUnicodeIdnaFlag_CheckJoiners | PrUnicodeIdnaFlag_UseStd3AsciiRules
                               | PrUnicodeIdnaFlag_VerifyDnsLength,
};
typedef uint8_t PrUnicodeIdnaFlags;

// Converts `domain` to its ASCII form (UTS #46 ToASCII). Returns false if the conversion records an error, and writes
// the null string to `*out` in that case. `domain` is UTF-8, and an ill-formed sequence is an error. The caller
// releases the result with prStringReset.
bool prUnicodeIdnaToAscii(PrStringView domain, PrUnicodeIdnaFlags flags, PrString* out);

// Converts `domain` to its Unicode form (UTS #46 ToUnicode). Returns false if the conversion records an error. The
// result is written to `*out` either way. The VerifyDnsLength flag does not apply. The caller releases the result with
// prStringReset.
bool prUnicodeIdnaToUnicode(PrStringView domain, PrUnicodeIdnaFlags flags, PrString* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
