#include "pr/compress_builtin.h"
#include "pr/draw_glyph_bitmap.h"
#include "pr/image_builtin.h"

int prTestDrawGlyphBitmapCApi(void);

// Tests that the public header compiles as C. A decoder over the built-in image codec is created and destroyed.
int prTestDrawGlyphBitmapCApi(void) {
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrDrawGlyphDecoder* decoder = prDrawGlyphBitmapCreate(imageCodec);
    int result = decoder == NULL;
    prDrawGlyphDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    return result;
}
