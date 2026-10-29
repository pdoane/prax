#include "pr/font_directwrite.h"

int prTestFontDirectWriteCApi(void);

// The public headers compile as C. Installing into a set adds no fewer faces than the set then holds.
int prTestFontDirectWriteCApi(void) {
    PrFontSet* set = prFontSetCreate(NULL);
    uint32_t added = prFontDirectWriteInstall(set);
    int result = added != prFontSetFaceCount(set);
    prFontSetDestroy(set);
    return result;
}
