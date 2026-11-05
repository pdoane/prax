#include "pr/font_subset.h"

int prTestFontSubsetCApi(void);

// Tests that the public header compiles as C.
int prTestFontSubsetCApi(void) {
    PrFontSubsetFormat format = PrFontSubsetFormat_TrueType;
    return format == PrFontSubsetFormat_None;
}
