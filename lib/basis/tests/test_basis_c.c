#include "pr/basis_etc1s.h"
#include "pr/basis_uastc.h"

int prTestBasisCApi(void);

// Compiles the public headers as C. Empty global data is not valid ETC1S data.
int prTestBasisCApi(void) {
    static const uint8_t nothing[1] = {0};
    PrBasisEtc1s* etc1s = NULL;
    PrByteView global = {nothing, 0};
    int result = prBasisEtc1sCreate(global, 1, &etc1s) == PrStatus_Ok;
    prBasisEtc1sDestroy(etc1s);
    result |= PrBasisEtc1sTarget_Pixels != 0;
    return result;
}
