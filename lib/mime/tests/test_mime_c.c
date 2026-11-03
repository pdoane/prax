#include "pr/mime.h"

int prTestMimeCApi(void);

// The public header compiled as C.
int prTestMimeCApi(void) {
    int result = prMimeTypeEssence(PR_SV("text/html; charset=utf-8")).size != 9;
    PrRawString value = prRawStringNull();
    result |= !prMimeTypeParameter(PR_SV("text/html;charset=utf-8"), PR_SV("charset"), &value);
    prRawStringReset(&value);
    result |= !prMimeTypeIsJavaScript(PR_SV("text/javascript"));
    result |= !prMimeTypeIsXml(PR_SV("image/svg+xml"));
    return result;
}
