#include "pr/compress_builtin.h"
#include "pr/image_builtin.h"

int prTestImageBuiltinCApi(void);

// The public header compiled as C. An encoded PNG decodes back through the built-in codec.
int prTestImageBuiltinCApi(void) {
    const uint8_t pixel[4] = {10, 20, 30, 255};
    uint8_t png[128];
    PrImageCodec* codec = prImageBuiltinCreate(prCompressBuiltinCreate());
    size_t size = 0;
    if (prImageCodecEncode(codec, PR_SV("image/png"), (PrPixelView){pixel, 4, {1, 1}}, png, sizeof(png), &size)
            != PrStatus_Ok
        || size > sizeof(png))
        return 1;
    PrImageSource source = {0};
    source.memory.data = png;
    source.memory.size = size;
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    int result = prImageDecoderOpen(decoder, source, NULL) != PrStatus_Ok;
    uint8_t out[4] = {0};
    if (result == 0)
        result |= prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
                                          (PrPixels){out, 4, {1, 1}}) != PrStatus_Ok
                  || out[1] != 20;
    prImageDecoderDestroy(decoder);
    prImageCodecDestroy(codec);
    return result;
}
