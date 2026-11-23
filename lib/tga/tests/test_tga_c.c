#include "pr/tga.h"

int prTestTgaCApi(void);

// The public header compiled as C. A black and white image of one pixel decodes.
int prTestTgaCApi(void) {
    static const uint8_t file[] = {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 8, 0, 77};
    PrByteView bytes = {file, sizeof(file)};
    int result = !prTgaHasHeader(bytes) || prTgaHasFooter(bytes);
    PrTgaDecoder* decoder = NULL;
    PrTgaLimits limits = prTgaLimitsDefault();
    result |= prTgaDecoderCreate(bytes, &limits, bytes.size, &decoder) != PrStatus_Ok;
    if (decoder == NULL)
        return 1;
    const PrTgaInfo* info = prTgaDecoderInfo(decoder);
    result |= info->extent.x != 1 || info->channels != 1 || !info->gray || !info->fromBottom;
    uint8_t pixel = 0;
    PrPixels pixels = {&pixel, 1, {1, 1}};
    result |= prTgaDecoderRead(decoder, pixels) != PrStatus_Ok || pixel != 77;
    prTgaDecoderDestroy(decoder);
    return result;
}
