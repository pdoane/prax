#include "pr/compress_deflate.h"
#include "pr/png.h"
#include "pr/png_text.h"

int prTestPngCApi(void);

// The public headers compiled as C. A one-pixel image encodes and decodes back.
int prTestPngCApi(void) {
    uint8_t pixel[4] = {1, 2, 3, 4};
    PrPixelView view = {pixel, 4, {1, 1}};
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* compress = prCompressCodecSetCreate(&deflate, 1);
    uint8_t file[128];
    size_t size = prPngEncode(view, 0, compress, file, sizeof(file));
    PrPngDecoder* decoder = NULL;
    PrByteView bytes = {file, size};
    int result = size > sizeof(file) || prPngDecoderCreate(bytes, compress, NULL, size, &decoder) != PrStatus_Ok;
    if (decoder != NULL) {
        uint8_t out[4] = {0};
        PrPixels pixels = {out, 4, {1, 1}};
        result |= prPngDecoderRead(decoder, 0, pixels) != PrStatus_Ok || out[3] != 4;
        result |= prPngDecoderInfo(decoder)->channels != 4;
    }
    prPngDecoderDestroy(decoder);
    prCompressCodecSetDestroy(compress);
    return result;
}
