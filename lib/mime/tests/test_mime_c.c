#include "pr/mime.h"
#include "pr/mime_sniff.h"

int prTestMimeCApi(void);

// The public header compiled as C.
int prTestMimeCApi(void) {
    int result = prMimeTypeEssence(PR_SV("text/html; charset=utf-8")).size != 9;
    PrRawString value = prRawStringNull();
    result |= !prMimeTypeParameter(PR_SV("text/html;charset=utf-8"), PR_SV("charset"), &value);
    prRawStringReset(&value);
    result |= !prMimeTypeIsJavaScript(PR_SV("text/javascript"));
    result |= !prMimeTypeIsXml(PR_SV("image/svg+xml"));
    const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    PrByteView bytes = {png, sizeof(png)};
    result |= prMimeSniff(bytes, PR_SV("")) != PrMimeFormat_Png;
    result |= prMimeFormatInfo(PrMimeFormat_Png).kind != PrMimeKind_Image;
    return result;
}
