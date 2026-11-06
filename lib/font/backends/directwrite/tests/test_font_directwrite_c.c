#include "pr/font_directwrite.h"

int prTestFontDirectWriteCApi(void);

// The public headers compile as C. A set that asks the system matches the system's families.
int prTestFontDirectWriteCApi(void) {
    PrFontSystem* system = prFontDirectWriteCreate();
    if (system == NULL)
        return 1;
    PrFontSet* set = prFontSetCreate(NULL);
    prFontSetSetSystem(set, system);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, prFontFamilyFromName(PR_SV("Segoe UI")), &style, match);
    int result = prFontMatchFaceCount(match) == 0;
    prFontMatchDestroy(match);
    prFontSetDestroy(set);
    prFontSystemDestroy(system);
    return result;
}
