#include "pr/url.h"

int prTestUrlCApi(void);

// The public header compiled as C. Components are views, and parameters are a list.
int prTestUrlCApi(void) {
    PrUrl* url = NULL;
    if (prUrlCreate(PR_SV("https://example.org:8080/p?a=1&a=2"), NULL, PrCharset_Utf8, &url) != PrStatus_Ok)
        return 1;
    int result = prUrlPort(url) != 8080 || prUrlHost(url).size != 11 || prUrlFragment(url).data != NULL;
    PrUrlSearchParams* params = prUrlSearchParams(url);
    result |= prUrlSearchParamsSize(params) != 2 || prUrlSearchParamsValueAt(params, 1).data[0] != '2';
    prUrlSearchParamsDelete(params, PR_SV("a"), (PrStringView){0});
    result |= prUrlSearchParamsSize(params) != 0 || prUrlQuery(url).data != NULL;
    prUrlDestroy(url);
    prUrlDestroy(NULL);
    prUrlSearchParamsDestroy(NULL);
    return result;
}
