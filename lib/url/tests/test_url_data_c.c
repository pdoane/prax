#include "pr/url_data.h"

bool prTestDataC(void);

bool prTestDataC(void) {
    PrRawString body = {0};
    PrRawString type = {0};
    bool ok = prUrlProcessData(PR_SV("data:,x"), &body, &type) == PrStatus_Ok && prRawStringView(&body).size == 1 &&
        prRawStringView(&body).data[0] == 'x';
    prRawStringReset(&body);
    prRawStringReset(&type);
    return ok;
}
