#include "pr/bc.h"

int prTestBcCApi(void);

// The public header compiled as C. A white BC1 block decodes to white.
int prTestBcCApi(void) {
    static const uint8_t white[8] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    uint8_t texels[64];
    int result = !prBcFormatDecodes(PrBcFormat_Bc1);
    prBcDecode(PrBcFormat_Bc1, white, texels);
    result |= texels[0] != 255 || texels[63] != 255;
    PrBcPartition partition = prBcPartition(2, 0);
    result |= partition.anchors[1] != 15;
    return result;
}
