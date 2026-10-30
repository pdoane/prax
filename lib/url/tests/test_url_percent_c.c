#include "pr/url_percent.h"

bool prTestPercentC(void);

bool prTestPercentC(void) {
    const char bytes[] = {(char)0xff, '+'};
    char encoded[4];
    size_t size = prUrlPercentEncode(PR_LITERAL(PrStringView){bytes, sizeof(bytes)},
        PrUrlPercentEncodeSet_C0Control, encoded, sizeof(encoded));
    bool ok = size == 4 && encoded[0] == '%' && encoded[1] == 'F' && encoded[2] == 'F' && encoded[3] == '+';
    size = prUrlPercentDecode(PR_LITERAL(PrStringView){encoded, sizeof(encoded)}, encoded, sizeof(encoded));
    return ok && size == 2 && (uint8_t)encoded[0] == 0xff && encoded[1] == '+';
}
