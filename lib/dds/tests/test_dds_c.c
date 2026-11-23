#include "pr/dds.h"
#include "pr/dds_texture_source.h"

int prTestDdsCApi(void);

// The public headers compiled as C. Bytes without the magic do not open.
int prTestDdsCApi(void) {
    static const uint8_t bytes[4] = {'D', 'D', 'S', '!'};
    PrByteView view = {bytes, sizeof(bytes)};
    PrDdsTexture* texture = NULL;
    int result = prDdsHasSignature(view) || prDdsTextureCreate(view, &texture) != PrStatus_InvalidData;
    result |= texture != NULL;
    PrUVec2 extent = prDdsLevelExtent((PrUVec2){5, 1}, 1);
    result |= extent.x != 2 || extent.y != 1;
    result |= kPrDdsDx10HeaderBytes != 148;
    return result;
}
