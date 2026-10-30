// The bitmap glyph decoder, which draws the images of glyphs in bitmap strikes, decoded by an image codec.

#include "pr/draw_glyph_bitmap.h"

#include "pr/alloc.h"

namespace pr_impl {

namespace {

struct BitmapDecoder : PrDrawGlyphDecoder {
    PrImageCodec* codec;
};

void destroyDecoder(PrDrawGlyphDecoder* decoder) {
    prMemoryFree(decoder);
}

bool decode(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE, PrColor, PrDrawList* list) {
    PrFontGlyphSvg svg{};
    if (prFontGlyphSvg(font, glyph, &svg))
        return false;
    BitmapDecoder* bitmap = static_cast<BitmapDecoder*>(decoder);
    return prDrawGlyphImage(bitmap->codec, font, glyph, pixelsPerEm, list);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawGlyphDecoder* prDrawGlyphBitmapCreate(PrImageCodec* codec) {
    BitmapDecoder* decoder = allocZeroed<BitmapDecoder>();
    decoder->destroy = destroyDecoder;
    decoder->decode = decode;
    decoder->codec = codec;
    return decoder;
}
