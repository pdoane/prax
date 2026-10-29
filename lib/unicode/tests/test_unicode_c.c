#include "pr/locale_common.h"
#include "pr/unicode.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/unicode_idna.h"
#include "pr/unicode_line_break.h"
#include "pr/unicode_normalization.h"

int prTestUnicodeCApi(void);

// The public headers compiled as C. Reads a property from each algorithm.
int prTestUnicodeCApi(void) {
    PrUnicodeLineBreakFlags flags = 0;
    PrStringView text = PR_SV("ab");
    uint32_t folded[3];
    return prUnicodeBidiClass('a') != PrUnicodeBidiClass_L
        || prUnicodeLineBreakClass(' ', &flags) != PrUnicodeLineBreakClass_SP || prUnicodeGraphemeNext(text, 0) != 1
        || prUnicodeScript('a') != PrScript_Latn || kPrUnicodeBidiMaxDepth != 125
        || !prUnicodeIsNormalized(text, PrUnicodeNormalForm_Nfc) || prUnicodeCombiningClass(0x300) != 230
        || PrUnicodeIdnaFlag_CheckBidi == 0
        || prUnicodeGeneralCategory('a') != PrUnicodeGeneralCategory_LowercaseLetter || prUnicodeFold('A', folded) != 1;
}
