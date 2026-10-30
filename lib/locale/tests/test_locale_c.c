#include "pr/locale.h"
#include "pr/locale_common.h"
#include "pr/locale_full.h"

int prTestLocaleCApi(void);

// The public headers compiled as C. Parses a tag and reads its subtags back.
int prTestLocaleCApi(void) {
    PrLocale locale = prLocaleFromTag(PR_SV("en-US"));
    return prLocaleLanguage(locale) != PrLanguage_En || prLocaleRegion(locale) != PrRegion_US ||
        prScriptCode(PrScript_Latn).size != 4;
}
