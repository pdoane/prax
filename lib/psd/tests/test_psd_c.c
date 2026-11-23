#include "pr/psd.h"

int prTestPsdCApi(void);

// The public header compiled as C. Bytes of another signature do not open.
int prTestPsdCApi(void) {
    static const uint8_t bytes[6] = {'8', 'B', 'P', 'X', 0, 1};
    PrByteView view = {bytes, sizeof(bytes)};
    PrPsdDecoder* decoder = NULL;
    PrPsdLimits limits = prPsdLimitsDefault();
    int result = prPsdHasSignature(view) || prPsdDecoderCreate(view, &limits, sizeof(bytes), &decoder) !=
        PrStatus_InvalidData;
    result |= decoder != NULL || PrPsdMode_Lab != 9;
    return result;
}
