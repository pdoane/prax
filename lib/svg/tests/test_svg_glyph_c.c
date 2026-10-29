#include "pr/svg_glyph.h"

int prTestSvgGlyphCApi(void);

// Tests that the public header compiles as C. A decoder without codecs is created and destroyed.
int prTestSvgGlyphCApi(void) {
    PrDrawGlyphDecoder* decoder = prSvgGlyphDecoderCreate(NULL, NULL);
    int result = decoder == NULL;
    prDrawGlyphDecoderDestroy(decoder);
    return result;
}
