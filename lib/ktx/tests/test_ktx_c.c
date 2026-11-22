#include "pr/ktx.h"

int prTestKtxCApi(void);

// Compiles the public header as C. For bytes that may begin a file, prKtxInfoBytes returns the size of the header. For
// other bytes it returns zero.
int prTestKtxCApi(void) {
    static const uint8_t start[4] = {0xAB, 'K', 'T', 'X'};
    static const uint8_t other[4] = {0x89, 'P', 'N', 'G'};
    int result = prKtxInfoBytes((PrByteView){start, sizeof(start)}) != kPrKtxHeaderBytes;
    result |= prKtxInfoBytes((PrByteView){other, sizeof(other)}) != 0;
    result |= prKtxHasSignature((PrByteView){start, sizeof(start)});
    PrUVec2 extent = prKtxLevelExtent((PrUVec2){12, 8}, 3);
    result |= extent.x != 1 || extent.y != 1;
    return result;
}
