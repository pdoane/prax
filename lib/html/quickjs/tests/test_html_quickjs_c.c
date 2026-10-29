// Checks that the public headers of the QuickJS layers compile and link as C.

#include "pr/canvas_quickjs.h"
#include "pr/charset_quickjs.h"
#include "pr/css_quickjs.h"
#include "pr/css_typed_om_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/fetch_quickjs.h"
#include "pr/html_console_quickjs.h"
#include "pr/html_quickjs.h"
#include "pr/mathml_quickjs.h"
#include "pr/svg_quickjs.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

int prTestHtmlQuickJsCApi(void);

int prTestHtmlQuickJsCApi(void) {
    PrJsHtmlBind* const binds[] = {prJsCharsetBind, prJsUrlBind, prJsXhrBind, prJsSvgBind, prJsMathMlBind};
    PrJsHtmlEngineDesc desc = {.binds = binds, .bindCount = 5};
    PrJsHtmlEngine* engine = prJsHtmlEngineCreate(&desc);
    JSContext* ctx = prJsHtmlEngineContext(engine);
    int failures = prJsHtmlEngineRun(ctx, PR_SV("new URL('https://example.com/')"), PR_SV("c-api"), 1, 1) ? 0 : 1;
    failures += prJsHtmlEngineHostContext(ctx) == NULL ? 0 : 1;
    prJsHtmlEngineDestroy(engine);
    return failures;
}
