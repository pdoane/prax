#include "pr/webview.h"

int prTestWebViewCApi(void);

// The public header compiled as C. Checks the structures that a view is created with and reports.
int prTestWebViewCApi(void) {
    PrWebViewDesc desc = {.viewport = {40, 20}, .pixelRatio = 2};
    PrWebViewProcess process = {.role = PrWebViewProcessRole_Gpu, .id = 7};
    prWebViewDestroy(NULL);
    return desc.viewport.x != 40 || process.role != PrWebViewProcessRole_Gpu ||
        prWebViewBackendName(PrWebViewBackend_WebView2).size != 8;
}
