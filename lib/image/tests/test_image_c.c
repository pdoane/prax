#include "pr/image.h"
#include "pr/image_png.h"
#include "pr/image_process.h"

int prTestImageCApi(void);

// The public header compiled as C. Pixel formats are sized through the interface.
int prTestImageCApi(void) {
    PrImageFormat format = {0};
    format.model = PrColorModel_Rgb;
    format.sample = PrImageSample_U8;
    format.alpha = PrImageAlpha_Straight;
    format.color.encoding = PrImageColorEncoding_Named;
    format.color.space = PrColorSpace_Srgb;
    int result = prImageFormatSize(&format) != 4;
    PrImageLimits limits = prImageLimitsDefault();
    result |= limits.frames == 0;
    float value = 1;
    PrPixels pixels = {(uint8_t*)&value, sizeof(float), {1, 1}};
    prImageBlur(pixels, PrPixelFormat_RF32, (PrVec2){0, 0}, PrImageEdgeMode_None);
    result |= value != 1;
    size_t size = 1;
    result |= prImagePngSetText((PrByteView){NULL, 0}, PR_SV("Key"), PR_SV("text"), NULL, 0, &size)
              != PrStatus_InvalidData;
    prImageDecoderDestroy(NULL);
    prImageCodecDestroy(NULL);
    return result;
}
