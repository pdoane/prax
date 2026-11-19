#include "pr/exr.h"

int prTestExrCApi(void);

// The public header compiled as C. Bytes that are not an OpenEXR file are refused, and the limits have their defaults.
int prTestExrCApi(void) {
    static const uint8_t bytes[8] = {0x76, 0x2f, 0x31, 0x02, 2, 0, 0, 0};
    PrByteView file = {bytes, sizeof(bytes)};
    int result = prExrHasSignature(file);
    PrExrLimits limits = prExrLimitsDefault();
    result |= limits.parts != 4096;
    PrExrImage* image = NULL;
    result |= prExrImageCreate(file, NULL, &limits, 0, &image) != PrStatus_NotSupported;
    result |= image != NULL;
    prExrImageDestroy(image);
    return result;
}
