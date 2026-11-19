#include "pr/webp.h"

int prTestWebpCApi(void);

// The public header compiled as C. Bytes that are not a WebP file are refused, and the limits have their defaults.
int prTestWebpCApi(void) {
    static const uint8_t bytes[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'};
    PrByteView file = {bytes, sizeof(bytes)};
    int result = prWebpHasSignature(file);
    PrWebpLimits limits = prWebpLimitsDefault();
    result |= limits.frames != 4096;
    PrWebpDecoder* decoder = NULL;
    result |= prWebpDecoderCreate(file, &limits, 0, &decoder) != PrStatus_NotSupported;
    result |= decoder != NULL;
    prWebpDecoderDestroy(decoder);
    return result;
}
