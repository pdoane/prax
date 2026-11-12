#include "pr/base64.h"
#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/fetch.h"
#include "pr/fetch_intercept.h"
#include "pr/html_page.h"
#include "pr/mathml_quickjs.h"
#include "pr/net.h"
#include "pr/net_null.h"
#include "pr/quickjs_binding.h"
#include "pr/svg_quickjs.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Holds every request that an intercepting fetcher receives, unanswered, so that what a fetch loads is still loading
// for as long as the test needs.
void hold(void* PR_NULLABLE context, PrFetchInterceptRequest* request) {
    static_cast<std::vector<PrFetchInterceptRequest*>*>(context)->push_back(request);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a script reads and changes a style element's sheet through document.styleSheets") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<style>foo { color: blue; }</style><style></style>", "about:blank", nullptr) ==
        PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    const char source[] = R"JS((function() {
        const list = document.styleSheets;
        if (list.length !== 2 || list.item(2) !== null || list[2] !== undefined)
            throw new Error('stylesheet list access');
        const rules = list[0].cssRules;
        if (rules.length !== 1 || rules[0].selectorText !== 'foo' || list[1].cssRules.length !== 0)
            throw new Error('inline CSS parsing and order');
        rules[0].selectorText = 'bar';
        if (document.styleSheets.item(0).cssRules[0].selectorText !== 'bar')
            throw new Error('stylesheet modification was lost');
    })())JS";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "stylesheets", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("a script reaches the rule interfaces, their media lists and their maps") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<style media=print>@media screen {} @layer a, b;"
        "@font-feature-values F { @swash { x: 1 } }</style>", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    const char source[] = R"JS((function() {
        const sheet = document.styleSheets[0];
        const rules = sheet.cssRules;
        if (String(sheet.media) !== 'print' || !(rules[0] instanceof CSSMediaRule))
            throw new Error('media lists');
        // webidl/#SameObject: whatever a script stores on the object stays with it.
        rules[0].media.marker = 1;
        rules[0].media = 'speech';
        if (rules[0].media.marker !== 1 || rules[0].media.mediaText !== 'speech')
            throw new Error('[SameObject] and [PutForwards]');
        const names = rules[1].nameList;
        if (!Object.isFrozen(names) || names.join() !== 'a,b')
            throw new Error('FrozenArray');
        const swash = rules[2].swash;
        swash.set('y', [2]);
        if (swash.size !== 2 || swash.get('y')[0] !== 2 || swash.get('z') !== undefined
            || [...swash.keys()].join() !== 'x,y' || rules[2].swash !== swash)
            throw new Error('maplike');
        if ([...rules].length !== 3 || Object.prototype.toString.call(rules) !== '[object CSSRuleList]')
            throw new Error('iteration and @@toStringTag');
    })())JS";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "rules", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("a script matches media queries against the environment the host sets") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<style>@media (min-width: 1000px) {}</style>", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(prHtmlPageSetMediaFeature(page, PrCssDescriptor_MediaWidth, "1024px"));
    CHECK_FALSE(prHtmlPageSetMediaFeature(page, PrCssDescriptor_MediaWidth, "wide"));
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    const char source[] = R"JS((function() {
        if (!document.styleSheets[0].cssRules[0].matches)
            throw new Error('CSSMediaRule.matches');
        const list = matchMedia(' (MIN-WIDTH: 1000px) , (scripting)');
        if (!(list instanceof MediaQueryList) || !(list instanceof EventTarget))
            throw new Error('MediaQueryList');
        if (list.media !== '(min-width: 1000px), (scripting)' || !list.matches)
            throw new Error('media and matches');
        if (matchMedia('(scripting)').matches || matchMedia('(orientation: portrait)').matches)
            throw new Error('the default environment');
        const listener = () => {};
        list.addListener(listener);
        list.removeListener(listener);
        list.addListener(null);
    })())JS";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "media", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("devicePixelRatio is the display's pixel ratio, not a resolution a query sees, and replaceable") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<p>", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    auto run = [&](const char* source) {
        JSValue result = JS_Eval(ctx, source, strlen(source), "ratio", JS_EVAL_TYPE_GLOBAL);
        CHECK(!JS_IsException(result));
        JS_FreeValue(ctx, result);
    };
    run("if (devicePixelRatio !== 1) throw new Error('the default');");
    CHECK(prHtmlPageSetMediaFeature(page, PrCssDescriptor_MediaResolution, "150dpi"));
    run("if (devicePixelRatio !== 1) throw new Error('the resolution');");
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = 2;
    prHtmlPageSetDisplay(page, &display);
    run("if (window.devicePixelRatio !== 2) throw new Error('the display');devicePixelRatio = 'x';"
        "if (devicePixelRatio !== 'x') throw new Error('replaceable');");
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("the viewport that the host sets is the one that scripts measure, and the screen's orientation follows it") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<link id=l hreflang=fr-CA imagesrcset='a.png 2x'>", "about:blank", nullptr) ==
        PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    auto run = [&](const char* source) {
        JSValue result = JS_Eval(ctx, source, strlen(source), "viewport", JS_EVAL_TYPE_GLOBAL);
        CHECK(!JS_IsException(result));
        JS_FreeValue(ctx, result);
    };
    run("if (innerWidth !== 800 || innerHeight !== 600) throw new Error('the default viewport');"
        "const o = screen.orientation;"
        "if (!(o instanceof ScreenOrientation) || !(o instanceof EventTarget) || o !== screen.orientation)"
        "    throw new Error('[SameObject] orientation');"
        "if (o.type !== 'landscape-primary' || o.angle !== 0) throw new Error('type and angle');o.unlock();"
        "globalThis.lockError = null;o.lock('portrait').catch(e => { globalThis.lockError = e.name; });"
        "if (navigator.javaEnabled() !== false) throw new Error('javaEnabled');"
        "if (l.hreflang !== 'fr-CA' || l.imageSrcset !== 'a.png 2x') throw new Error('link reflections');");
    prHtmlPageSetViewport(page, PrVec2{1280, 900});
    JSContext* pending = nullptr;
    while (JS_ExecutePendingJob(runtime, &pending) > 0) {
    }
    run("if (innerWidth !== 1280 || innerHeight !== 900 || !matchMedia('(width: 1280px)').matches)"
        "    throw new Error('the host viewport');if (lockError !== 'NotSupportedError') throw new Error('lock rejects');");
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("a face's object made while it loads is held until the load settles its promise") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    std::string path = std::string(PR_FONTS_DIR) + "/noto_handwriting-cff2_colr_1.otf";
    std::string bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    char buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.append(buffer, read);
    std::fclose(file);
    std::string encoded(prBase64EncodedSize(bytes.size()), '\0');
    prBase64Encode(bytes, encoded.data(), encoded.size());
    std::string html = "<style>@font-face { font-family: A; src: url(data:font/otf;base64," + encoded + ") }</style>";
    // FontFaceSet.load starts loading the face, which settles only in a task.
    CHECK(prHtmlPageLoad(page, html, "about:blank", nullptr) == PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    const char source[] = R"JS(document.fonts.load('10px A').then(faces => { globalThis.loaded = faces.length; }))JS";
    JSValue result = JS_Eval(ctx, source, sizeof(source) - 1, "fonts", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
    JS_RunGC(runtime);
    while (prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == 0)
        prDomAgentRunTask(prHtmlPageAgent(page));
    JSContext* pending = nullptr;
    while (JS_ExecutePendingJob(runtime, &pending) > 0) {
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue loaded = JS_GetPropertyStr(ctx, global, "loaded");
    int32_t count = 0;
    CHECK(JS_ToInt32(ctx, &count, loaded) == 0);
    CHECK(count == 1);
    JS_FreeValue(ctx, loaded);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
}

TEST_CASE("the ready promise taken while the last loading face loads resolves when the face is deleted") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    prJsSvgBind(ctx);
    prJsMathMlBind(ctx);
    jsHtmlBind(ctx);
    // The fetcher holds every fetch, so that the face is still loading when it is deleted.
    PrNet* net = prNetNullCreate();
    std::vector<PrFetchInterceptRequest*> held;
    PrFetchInterceptDesc fetcherDesc{.net = net, .request = hold, .context = &held};
    PrFetchNetwork* fetcher = prFetchInterceptCreate(&fetcherDesc);
    PrHtmlHost host = prHtmlHostDefault();
    host.fetcher = fetcher;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    CHECK(prHtmlPageLoad(page, "<p>", "https://fonts.test/", nullptr) == PrStatus_Ok);
    CHECK(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    auto settle = [&] {
        for (bool ran = true; ran;) {
            ran = prNetNullRun(net);
            while (prDomAgentRunTask(prHtmlPageAgent(page)))
                ran = true;
        }
        JSContext* pending = nullptr;
        while (JS_ExecutePendingJob(runtime, &pending) > 0) {
        }
    };
    auto run = [&](const char* source) {
        JSValue result = JS_Eval(ctx, source, strlen(source), "fonts", JS_EVAL_TYPE_GLOBAL);
        CHECK(!JS_IsException(result));
        JS_FreeValue(ctx, result);
    };
    run("globalThis.face = new FontFace('B', 'url(b.ttf)');document.fonts.add(face);face.load();");
    settle();
    REQUIRE(held.size() == 1);
    run("if (document.fonts.status !== 'loading') throw new Error('loading');globalThis.ready = false;"
        "document.fonts.ready.then(() => { globalThis.ready = true; });document.fonts.delete(face);");
    settle();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ready = JS_GetPropertyStr(ctx, global, "ready");
    CHECK(JS_ToBool(ctx, ready) == 1);
    JS_FreeValue(ctx, ready);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
    prHtmlPageDestroy(page);
    prNetNullRun(net);
    for (PrFetchInterceptRequest* request : held)
        prFetchInterceptRequestFail(request, PrNetError_Reset);
    prFetchNetworkDestroy(fetcher);
    prNetDestroy(net);
}
