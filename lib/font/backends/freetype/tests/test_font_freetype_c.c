#include "pr/font_freetype.h"

int prTestFontFreeTypeCApi(void);

// The public headers compile as C. A hinter is made and freed.
int prTestFontFreeTypeCApi(void) {
    PrFontHinter* hinter = prFontFreeTypeCreate();
    int result = hinter == NULL;
    prFontHinterDestroy(hinter);
    return result;
}
