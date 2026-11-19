#include "pr/hdr.h"

int prTestHdrCApi(void);

// The public header compiled as C. A picture of one flat pixel reads and decodes.
int prTestHdrCApi(void) {
    static const char file[] = "#?RADIANCE\n\n-Y 1 +X 1\n\x80\x80\x80\x80";
    PrByteView bytes = {(const uint8_t*)file, sizeof(file) - 1};
    int result = !prHdrHasSignature(bytes);
    PrHdrDecoder* decoder = NULL;
    result |= prHdrDecoderCreate(bytes, NULL, bytes.size, &decoder) != PrStatus_Ok;
    if (decoder == NULL)
        return 1;
    const PrHdrInfo* info = prHdrDecoderInfo(decoder);
    result |= info->extent.x != 1 || info->extent.y != 1 || !info->scanlineAxis.y || !info->scanlineAxis.decreasing;
    float pixel[3];
    PrPixels pixels = {(uint8_t*)pixel, sizeof(pixel), {1, 1}};
    result |= prHdrDecoderRead(decoder, pixels) != PrStatus_Ok;
    result |= pixel[0] != 128.5f / 256;
    prHdrDecoderDestroy(decoder);
    return result;
}
