#include "pr/qoi.h"

int prTestQoiCApi(void);

// The public header compiled as C. An image of one pixel, coded as a run of the starting pixel, decodes.
int prTestQoiCApi(void) {
    static const uint8_t file[] = {'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0, 1};
    PrByteView bytes = {file, sizeof(file)};
    int result = !prQoiHasSignature(bytes);
    PrQoiDecoder* decoder = NULL;
    result |= prQoiDecoderCreate(bytes, NULL, bytes.size, &decoder) != PrStatus_Ok;
    if (decoder == NULL)
        return 1;
    const PrQoiInfo* info = prQoiDecoderInfo(decoder);
    result |= info->extent.x != 1 || info->extent.y != 1 || info->channels != 4 ||
        info->colorspace != PrQoiColorspace_Srgb;
    uint8_t pixel[4] = {1, 1, 1, 1};
    PrPixels pixels = {pixel, sizeof(pixel), {1, 1}};
    result |= prQoiDecoderRead(decoder, pixels) != PrStatus_Ok;
    result |= pixel[0] != 0 || pixel[3] != 255;
    prQoiDecoderDestroy(decoder);
    return result;
}
