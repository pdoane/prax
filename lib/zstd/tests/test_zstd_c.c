#include "pr/zstd.h"

int prTestZstdCApi(void);

// The public header compiled as C. A frame that the encoder writes decodes back to its input.
int prTestZstdCApi(void) {
    static const uint8_t input[] = {'z', 's', 't', 'd', 'z', 's', 't', 'd'};
    uint8_t frame[64];
    int result = prZstdEncodeBound(sizeof(input)) > sizeof(frame);
    size_t size = prZstdEncode(PrZstdLevel_Default, (PrByteView){input, sizeof(input)}, frame);
    result |= prZstdContentSize((PrByteView){frame, size}) != (int64_t)sizeof(input);
    PrZstdDecoder* decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    uint8_t output[16];
    PrZstdRead read = {0};
    result |= prZstdDecoderRead(decoder, (PrByteView){frame, size}, true, output, sizeof(output), &read) != PrStatus_Ok;
    result |= read.progress != PrZstdProgress_Done || read.consumed != size || read.written != sizeof(input);
    prZstdDecoderDestroy(decoder);
    return result;
}
