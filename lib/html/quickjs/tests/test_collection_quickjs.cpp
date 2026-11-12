#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "tools/harness/test.h"

extern "C" int prTestHtmlQuickJsCApi(void);

namespace pr_impl {

namespace {

// A runtime with the DOM bound, and a probe object whose finalizations it counts.
struct Lifetime {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    int finalized = 0;

    Lifetime() {
        prJsDomBind(ctx);
        prJsCssBind(ctx);
        jsHtmlBind(ctx);
    }

    ~Lifetime() {
        JS_FreeContext(ctx);
        jsRuntimeDestroy(runtime);
    }

    JSValue probe() {
        static JSClassID probeClass = 0;
        JSClassDef definition = {};
        definition.class_name = "LifetimeProbe";
        definition.finalizer = [](JSRuntime*, JSValue value) {
            JSClassID id = 0;
            ++*static_cast<int*>(JS_GetAnyOpaque(value, &id));
        };
        jsRegisterClass(runtime, &probeClass, definition);
        JSValue object = JS_NewObjectClass(ctx, int(probeClass));
        JS_SetOpaque(object, &finalized);
        return object;
    }

    // Calls the function that `source` evaluates to with `argument`, and returns its result.
    JSValue call(const char* source, JSValueConst argument) {
        JSValue function = JS_Eval(ctx, source, PrStringView(source).size, "lifetime", JS_EVAL_TYPE_GLOBAL);
        REQUIRE(!JS_IsException(function));
        JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 1, &argument);
        if (JS_IsException(result))
            jsReportException(ctx);
        JS_FreeValue(ctx, function);
        return result;
    }

    void collect() {
        JS_RunGC(runtime);
        JS_RunGC(runtime);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a collection or an element a query returns keeps its document alive") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    jsHtmlBind(ctx);

    // A native listener retains this probe until the document is destroyed.
    int finalized = 0;
    static JSClassID probeClass = 0;
    JSClassDef definition = {};
    definition.class_name = "LifetimeProbe";
    definition.finalizer = [](JSRuntime*, JSValue value) {
        JSClassID id = 0;
        ++*static_cast<int*>(JS_GetAnyOpaque(value, &id));
    };
    jsRegisterClass(runtime, &probeClass, definition);
    JSValue probe = JS_NewObjectClass(ctx, int(probeClass));
    JS_SetOpaque(probe, &finalized);
    const char source[] = R"JS((function(probe) {
        const doc = new DOMParser().parseFromString('<p id="first"></p><p name="second"></p>', 'text/html');
        doc.addEventListener('lifetime', probe);
        const list = doc.getElementsByTagName('P');
        if (list.length !== 2 || list.item(2) !== null || list[2] !== undefined ||
            list.namedItem('first').nodeType !== 1 || list.namedItem('second').nodeType !== 1 ||
            list.namedItem('absent') !== null) throw new Error('collection access');
        return list;
    }))JS";
    JSValue function = JS_Eval(ctx, source, sizeof(source) - 1, "collection", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(function));
    JSValue collection = JS_Call(ctx, function, JS_UNDEFINED, 1, &probe);
    REQUIRE(!JS_IsException(collection));
    JS_FreeValue(ctx, function);
    JS_FreeValue(ctx, probe);
    JS_RunGC(runtime);
    CHECK(finalized == 0);

    JSValue element = JS_GetPropertyUint32(ctx, collection, 0);
    REQUIRE(JS_IsObject(element));
    JS_FreeValue(ctx, collection);
    JS_RunGC(runtime);
    CHECK(finalized == 0);
    JSValue type = JS_GetPropertyStr(ctx, element, "nodeType");
    int32_t number = 0;
    CHECK(JS_ToInt32(ctx, &number, type) == 0);
    CHECK(number == PrDomNodeType_Element);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, element);
    // The document and the objects of its nodes form a cycle that the collector frees. The objects that the document's
    // listeners held are released as the document is freed, for the next collection to free.
    JS_RunGC(runtime);
    JS_RunGC(runtime);
    CHECK(finalized == 1);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("a listener that reaches its target is freed with the document the target is in") {
    Lifetime lifetime;
    JSValue probe = lifetime.probe();
    JS_FreeValue(lifetime.ctx, lifetime.call(R"JS((function(probe) {
        const doc = new DOMParser().parseFromString('<p>text</p>', 'text/html');
        doc.body.addEventListener('cycle', () => doc.body);
        doc.body.onclick = () => doc;
        doc.addEventListener('lifetime', probe);
    }))JS", probe));
    JS_FreeValue(lifetime.ctx, probe);
    lifetime.collect();
    CHECK(lifetime.finalized == 1);
}

TEST_CASE("a node's listeners last while script reaches the node through what holds it") {
    Lifetime lifetime;
    JSValue probe = lifetime.probe();
    // A static range that the global owns holds its container's document.
    JSValue range = lifetime.call(R"JS((function(probe) {
        const doc = new DOMParser().parseFromString('<p>text</p>', 'text/html');
        doc.body.addEventListener('ping', () => { globalThis.heard = true; });
        doc.addEventListener('lifetime', probe);
        return new StaticRange({startContainer: doc.body, startOffset: 0, endContainer: doc.body, endOffset: 0});
    }))JS",
        probe);
    JS_FreeValue(lifetime.ctx, probe);
    lifetime.collect();
    CHECK(lifetime.finalized == 0);
    JSValue heard = lifetime.call(R"JS((function(range) {
        range.startContainer.dispatchEvent(new Event('ping'));
        return globalThis.heard === true;
    }))JS", range);
    CHECK(JS_ToBool(lifetime.ctx, heard) == 1);
    JS_FreeValue(lifetime.ctx, range);
    lifetime.collect();
    CHECK(lifetime.finalized == 1);
}

TEST_CASE("a node's object and its listeners move with the node to the document that adopts it") {
    Lifetime lifetime;
    JSValue probe = lifetime.probe();
    JSValue adopter = lifetime.call(R"JS((function(probe) {
        const from = new DOMParser().parseFromString('<p>text</p>', 'text/html');
        const to = new DOMParser().parseFromString('', 'text/html');
        const p = from.querySelector('p');
        p.expando = 'kept';
        p.addEventListener('ping', () => { globalThis.heard = true; });
        from.addEventListener('lifetime', probe);
        to.body.appendChild(p);
        return to;
    }))JS", probe);
    JS_FreeValue(lifetime.ctx, probe);
    lifetime.collect();
    // The document that the node left is freed. The node's object is freed with the document that adopted it.
    CHECK(lifetime.finalized == 1);
    JSValue heard = lifetime.call(R"JS((function(to) {
        const p = to.querySelector('p');
        p.dispatchEvent(new Event('ping'));
        return globalThis.heard === true && p.expando === 'kept';
    }))JS", adopter);
    CHECK(JS_ToBool(lifetime.ctx, heard) == 1);
    JS_FreeValue(lifetime.ctx, adopter);
}

TEST_CASE("the public headers of the QuickJS layers compile as C") {
    CHECK(prTestHtmlQuickJsCApi() == 0);
}
