#include "pr/basis.h"
#include "pr/basis_etc1s.h"
#include "pr/basis_uastc.h"

int prTestBasisCApi(void);

// Compiles the public headers as C. A white BC1 block decodes to white, and empty global data is not valid ETC1S data.
int prTestBasisCApi(void) {
    static const uint8_t white[8] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    uint8_t texels[64];
    int result = !prBasisBlocksDecodes(PrBasisBlocks_Bc1);
    prBasisBlockDecode(PrBasisBlocks_Bc1, white, texels);
    result |= texels[0] != 255 || texels[63] != 255;
    result |= prBasisBlocksBytes(PrBasisBlocks_Bc7) != 16;
    PrBasisEtc1s* etc1s = NULL;
    PrByteView global = {white, 0};
    result |= prBasisEtc1sCreate(global, 1, &etc1s) == PrStatus_Ok;
    prBasisEtc1sDestroy(etc1s);
    result |= PrBasisEtc1sTarget_Pixels != 0;
    return result;
}
