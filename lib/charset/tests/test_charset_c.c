#include "pr/charset.h"

bool prTestCharsetC(void);

bool prTestCharsetC(void) {
    PrCharset charset = prCharsetFromLabel(PR_SV("latin1"));
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    const char bytes[] = {'c', 'a', 'f', (char)0xe9};
    char out[32];
    size_t size = 0;
    bool ok = prCharsetDecodeMaxSize(&decoder, sizeof(bytes)) <= sizeof(out)
              && prCharsetDecode(&decoder, PR_LITERAL(PrStringView){bytes, sizeof(bytes)}, PrCharsetInput_Final, out,
                                 &size);
    return ok && charset == PrCharset_Windows1252 && size == 5 && (uint8_t)out[3] == 0xc3 && (uint8_t)out[4] == 0xa9;
}
