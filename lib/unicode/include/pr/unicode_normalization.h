// Unicode normalization (UAX #15, https://www.unicode.org/reports/tr15/): the canonical normalization forms.
//
// tools/gen/gen_unicode.py takes the decompositions and composition exclusions from the Unicode Character Database.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A canonical normalization form.
enum {
    PrUnicodeNormalForm_Nfd,      // canonical decomposition
    PrUnicodeNormalForm_Nfc,      // canonical decomposition, then canonical composition
};
typedef uint8_t PrUnicodeNormalForm;

// Returns `text` in `form`. `text` is UTF-8 that may be ill-formed, and each ill-formed sequence becomes U+FFFD. The
// caller releases the result with prStringReset.
PrString prUnicodeNormalize(PrStringView text, PrUnicodeNormalForm form);

// Returns true if `text` is well-formed UTF-8 that is in `form`.
bool prUnicodeIsNormalized(PrStringView text, PrUnicodeNormalForm form);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
