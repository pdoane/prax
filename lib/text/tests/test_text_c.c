#include "pr/text.h"
#include "pr/text_estimate.h"
#include "pr/text_font.h"
#include "pr/text_itemize.h"
#include "pr/text_layout.h"
#include "pr/text_shaping.h"
#include "pr/text_transform.h"

int prTestTextCApi(void);

// The public headers compile as C. Glyphs are appended and read back.
int prTestTextCApi(void) {
    PrTextGlyphBuffer* glyphs = prTextGlyphBufferCreate();
    PrTextGlyph* g = prTextGlyphBufferAppend(glyphs, 2);
    g[0].advance.x = 1;
    g[1].advance.x = 2;
    int result = prTextGlyphBufferGlyphCount(glyphs) != 2 || prTextGlyphBufferAdvance(glyphs).x != 3;
    prTextGlyphBufferClear(glyphs);
    result |= prTextGlyphBufferGlyphCount(glyphs) != 0;
    prTextGlyphBufferDestroy(glyphs);
    prTextShaperDestroy(NULL);
    return result;
}
