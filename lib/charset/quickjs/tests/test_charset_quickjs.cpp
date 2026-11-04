#include "pr/charset_quickjs.h"
#include "pr/quickjs_binding.h"

#include "tools/harness/test.h"

#include <cstring>

namespace pr_impl {

namespace {

void evaluate(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, std::strlen(source), "charset", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, error);
        INFO((text ? text : "exception"));
        CHECK(false);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, error);
    }
    CHECK(!JS_IsException(result));
    JS_FreeValue(ctx, result);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("TextDecoder decodes buffers and views in its charset, in parts when it streams") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsCharsetBind(ctx);
    evaluate(ctx, R"JS(
        function check(value, what) { if (!value) throw new Error('check failed: ' + what); }
        var decoder = new TextDecoder();
        check(decoder.encoding === 'utf-8' && !decoder.fatal && !decoder.ignoreBOM, 'defaults');
        check(decoder.decode() === '', 'no input');
        check(decoder.decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0xe2, 0x82, 0xac])) === 'a€', 'view');
        check(decoder.decode(new Uint8Array([0x61, 0x62]).buffer) === 'ab', 'buffer');
        check(decoder.decode(new DataView(new Uint8Array([0x78, 0x79, 0x7a]).buffer, 1)) === 'yz', 'DataView');
        check(decoder.decode(new Uint8Array([0xe2, 0x82]), {stream: true}) === '', 'held');
        check(decoder.decode(new Uint8Array([0xac])) === '€', 'completed');
        check(decoder.decode(new Uint8Array([0xe2, 0x82])) === '�', 'flushed');
        var legacy = new TextDecoder(' Latin1 ', {ignoreBOM: true});
        check(legacy.encoding === 'windows-1252' && legacy.ignoreBOM, 'label');
        check(legacy.decode(new Uint8Array([0x63, 0x61, 0x66, 0xe9])) === 'café', 'legacy');
        var fatal = new TextDecoder('utf-16le', {fatal: true});
        check(fatal.decode(new Uint8Array([0x3d, 0xd8, 0x00, 0xde])) === '😀', 'pair');
        var threw = false;
        try { fatal.decode(new Uint8Array([0x00, 0xd8])); } catch (e) { threw = e instanceof TypeError; }
        check(threw, 'fatal');
        threw = false;
        try { new TextDecoder('iso-2022-kr'); } catch (e) { threw = e instanceof RangeError; }
        check(threw, 'replacement');
        threw = false;
        try { decoder.decode('text'); } catch (e) { threw = e instanceof TypeError; }
        check(threw, 'not a buffer');
    )JS");
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("TextEncoder encodes scalar values as UTF-8, and encodes into a buffer as much as fits") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsCharsetBind(ctx);
    evaluate(ctx, R"JS(
        function check(value, what) { if (!value) throw new Error('check failed: ' + what); }
        var encoder = new TextEncoder();
        check(encoder.encoding === 'utf-8', 'encoding');
        check(new TextEncoder() !== encoder, 'objects');
        check(encoder.encode().length === 0, 'empty');
        check(encoder.encode('a€').join() === '97,226,130,172', 'encode');
        check(encoder.encode('\ud800').join() === '239,191,189', 'lone surrogate');
        var out = new Uint8Array(5);
        var result = encoder.encodeInto('a😀b', out);
        check(result.read === 3 && result.written === 5, 'encodeInto ' + result.read + ' ' + result.written);
        check(Array.prototype.join.call(out) === '97,240,159,152,128', 'written');
        result = encoder.encodeInto('€', new Uint8Array(2));
        check(result.read === 0 && result.written === 0, 'no room');
    )JS");
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
