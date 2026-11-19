#include "pr/lz4.h"

int prTestLz4CApi(void);

// The public header compiled as C. A block of three literals decodes whole, and through a decoder.
int prTestLz4CApi(void) {
    static const uint8_t block[] = {0x30, 'a', 'b', 'c'};
    uint8_t output[8];
    PrLz4Read read = {0};
    int result = prLz4DecodeBlock((PrByteView){block, sizeof(block)}, output, sizeof(output), &read) != PrStatus_Ok;
    result |= read.progress != PrLz4Progress_Done || read.written != 3;
    PrLz4Decoder* decoder = prLz4DecoderCreate();
    result |= prLz4DecoderRead(decoder, (PrByteView){block, sizeof(block)}, true, output, sizeof(output), &read) !=
        PrStatus_Ok;
    result |= read.progress != PrLz4Progress_Done || read.consumed != sizeof(block) || read.written != 3;
    prLz4DecoderDestroy(decoder);
    return result;
}
