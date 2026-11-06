#include "pr/font_coretext.h"

int prTestFontCoreTextCApi(void);

// The public headers compile as C. A set that asks the system matches the system's families.
int prTestFontCoreTextCApi(void) {
    PrFontSystem* system = prFontCoreTextCreate();
    PrFontSet* set = prFontSetCreate(NULL);
    prFontSetSetSystem(set, system);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, prFontFamilyFromName(PR_SV("Helvetica")), &style, match);
    int result = prFontMatchFaceCount(match) == 0;
    prFontMatchDestroy(match);
    prFontSetDestroy(set);
    prFontSystemDestroy(system);
    return result;
}
