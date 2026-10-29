#include "pr/compress_builtin.h"

int prTestCompressCApi(void);

// The public headers compiled as C. A stored Deflate block decodes through the built-in backend.
int prTestCompressCApi(void) {
    const uint8_t stored[] = {0x01, 0x02, 0x00, 0xfd, 0xff, 'h', 'i'};
    uint8_t out[2] = {0};
    PrCompressRead read;
    PrByteView input = {stored, sizeof(stored)};
    PrStatus status = prCompressCodecDecode(prCompressBuiltinCreate(), PrCompressFormat_Deflate, input,
                                            out, sizeof(out), &read);
    int result = status != PrStatus_Ok || read.progress != PrCompressProgress_Done || out[1] != 'i';
    prCompressCodecDestroy(NULL);
    prCompressDecoderDestroy(NULL);
    return result;
}
