#include "pr/font_coretext.h"

int prTestFontCoreTextCApi(void);

// The public headers compile as C. Installing into a set adds no fewer faces than the set then holds.
int prTestFontCoreTextCApi(void) {
    PrFontSet* set = prFontSetCreate(NULL);
    uint32_t added = prFontCoreTextInstall(set);
    int result = added != prFontSetFaceCount(set);
    prFontSetDestroy(set);
    return result;
}
