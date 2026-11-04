#include "pr/font.h"
#include "pr/font_math.h"

int prTestFontCApi(void);

// The public headers compile as C. Bytes that are not a font make no font.
int prTestFontCApi(void) {
    static const unsigned char bytes[] = {0, 1, 2, 3};
    const PrFont* font = NULL;
    PrByteView view = {bytes, sizeof(bytes)};
    int result = prFontCreate(view, 0, NULL, &font) != PrStatus_InvalidData;
    result |= font != NULL;
    result |= prFontCount(view) != 0;
    result |= PR_FONT_TAG('w', 'g', 'h', 't') != 0x77676874u;
    prFontRelease(NULL);
    return result;
}
