#include "pr/url_data.h"

bool prTestDataC(void);

bool prTestDataC(void) {
    PrString body = {0};
    PrString type = {0};
    bool ok = prUrlProcessData(PR_SV("data:,x"), &body, &type) == PrStatus_Ok && prStringView(&body).size == 1 &&
        prStringView(&body).data[0] == 'x';
    prStringReset(&body);
    prStringReset(&type);
    return ok;
}
