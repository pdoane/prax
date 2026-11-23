#include "pr/bmp.h"

int prTestBmpCApi(void);

// The public header compiled as C. A file of one 24-bit pixel decodes.
int prTestBmpCApi(void) {
    static const uint8_t file[] = {
        'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0,
        4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        30, 20, 10, 0,
    };
    PrByteView bytes = {file, sizeof(file)};
    int result = !prBmpHasSignature(bytes);
    PrBmpDecoder* decoder = NULL;
    PrBmpLimits limits = prBmpLimitsDefault();
    result |= prBmpDecoderCreate(bytes, PrBmpSource_File, &limits, bytes.size, &decoder) != PrStatus_Ok;
    if (decoder == NULL)
        return 1;
    const PrBmpInfo* info = prBmpDecoderInfo(decoder);
    result |= info->extent.x != 1 || info->channels != 3 || info->bitCount != 24 || info->color != PrBmpColor_Unknown;
    uint8_t pixel[3] = {0};
    PrPixels pixels = {pixel, 3, {1, 1}};
    result |= prBmpDecoderRead(decoder, pixels) != PrStatus_Ok || pixel[0] != 10 || pixel[2] != 30;
    prBmpDecoderDestroy(decoder);
    return result;
}
