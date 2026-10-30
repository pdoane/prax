#include "pr/media.h"

int prTestMediaCApi(void);

// The public header compiled as C.
int prTestMediaCApi(void) {
    int result = prMediaCodecBackendFromName(PR_SV("default")) != PrMediaCodecBackend_Default;
    result |= prMediaCodecBackendName(PrMediaCodecBackend_Builtin).size != 7;
    PrMediaFrame frame = {0};
    result |= frame.serial != 0;
    prMediaDecoderDestroy(NULL);
    prMediaCodecDestroy(NULL);
    return result;
}
