#include "pr/etc2.h"

int prTestEtc2CApi(void);

// The public header compiled as C. An EAC R11 block of one value decodes to it in red.
int prTestEtc2CApi(void) {
    static const uint8_t block[8] = {128, 0, 0, 0, 0, 0, 0, 0};
    uint8_t texels[64];
    int result = !prEtc2FormatDecodes(PrEtc2Format_R11);
    prEtc2Decode(PrEtc2Format_R11, block, texels);
    result |= texels[0] != 128 || texels[3] != 255;
    return result;
}
