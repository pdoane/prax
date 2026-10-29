#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

bool evaluates(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, strlen(source), "typed-om", JS_EVAL_TYPE_GLOBAL);
    bool value = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, error);
        INFO((message != nullptr ? message : "exception"));
        CHECK(!JS_IsException(result));
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, result);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Typed OM bindings preserve child identity and lifetime across mutation and GC") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        const fallback = new CSSUnparsedValue(['red']);
        const variable = new CSSVariableReferenceValue('--x', fallback);
        variable.marker = {id: 7};
        fallback.marker = {id: 9};
        const onlyChild = new CSSVariableReferenceValue('--only', new CSSUnparsedValue(['blue']));
        onlyChild.marker = 17;
        globalThis.parent = new CSSUnparsedValue([onlyChild]);
        const value = new CSSUnparsedValue(['', variable, '']);
        if (variable.fallback !== fallback || value[1] !== variable ||
            !(value instanceof CSSStyleValue) || String(value) !== 'var(--x,red)') return false;
        globalThis.saved = value[1];
        value[1] = 'green';
        return String(value) === 'green';
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(ctx, "parent[0].marker === 17 && String(parent[0].fallback) === 'blue'"));
    CHECK(evaluates(ctx, "saved.variable === '--x' && String(saved.fallback) === 'red'"));
    CHECK(evaluates(ctx, R"JS((() => {
        const value = new CSSUnparsedValue([]);
        const ref = new CSSVariableReferenceValue('--cycle', value);
        value[0] = ref;
        try { String(value); return false; } catch (e) { if (!(e instanceof RangeError)) return false; }
        const orphan = new CSSUnparsedValue([]);
        const orphanRef = new CSSVariableReferenceValue('--orphan', orphan);
        orphan[0] = orphanRef;
        globalThis.cycle = ref;
        return true;
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(ctx, R"JS((() => {
        if (cycle.fallback[0] !== cycle) return false;
        cycle.fallback[0] = 'fixed';
        return String(cycle.fallback) === 'fixed';
    })())JS"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM indexed setters, iteration, and Web IDL conversions") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        class Keyword extends CSSKeywordValue {}
        const keyword = new Keyword('foo');
        if (!(keyword instanceof CSSStyleValue) || String(keyword) !== 'foo') return false;
        try { keyword.value = ''; return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        if (String(keyword) !== 'foo') return false;
        if (new CSSVariableReferenceValue('--x', undefined).fallback !== null) return false;
        const value = new CSSUnparsedValue(new Set(['a', 'b']));
        value[1] = 12;
        value[2] = null;
        if ([...value].join('|') !== 'a|12|null' || value[3] !== undefined) return false;
        try { value[4] = 'x'; return false; } catch (e) { if (!(e instanceof RangeError)) return false; }
        const keys = Object.keys(value);
        if (keys.join() !== '0,1,2') return false;
        try { value[2147483648] = 'x'; return false; } catch (e) { if (!(e instanceof RangeError)) return false; }
        if (Reflect.deleteProperty(value, '0')) return false;
        value.extra = 1;
        if (!Reflect.deleteProperty(value, 'extra') || value.extra !== undefined) return false;
        const entries = [...value.entries()];
        if (entries[1][0] !== 1 || entries[1][1] !== '12') return false;
        let visited = '';
        value.forEach((v, i, list) => { if (list !== value) throw Error('identity'); visited += i + v; });
        return visited === '0a1122null';
    })())JS"));
    CHECK(evaluates(ctx, R"JS((() => {
        let closed = false;
        function* items() { try { yield 'ok'; yield Symbol(); } finally { closed = true; } }
        try { new CSSUnparsedValue(items()); return false; }
        catch (e) { return e instanceof TypeError && closed; }
    })())JS"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM numeric constructors, conversion and nested serialization") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        const unit = new CSSUnitValue(2, 'CM');
        const sum = new CSSMathSum(unit, CSS.mm(10));
        if (!(unit instanceof CSSNumericValue) || !(sum instanceof CSSMathValue)) return false;
        if (unit.unit !== 'cm' || Math.abs(unit.to('mm').value - 20) > 1e-12 || sum.to('cm').value !== 3) return false;
        if (sum.operator !== 'sum' || sum.values[0] !== unit || sum.values.length !== 2) return false;
        if (String(new CSSMathSum(sum, CSS.cm(1))) !== 'calc((2cm + 10mm) + 1cm)') return false;
        if (String(new CSSMathSum(1, 2)) !== 'calc(1 + 2)') return false;
        const mixed = new CSSMathSum(CSS.px(1), CSS.percent(2));
        if (String(mixed) !== 'calc(1px + 2%)') return false;
        if (JSON.stringify(mixed.type()) !== '{"length":1,"percentHint":"length"}') return false;
        // A power that cancels to zero is not an entry of the type.
        if (JSON.stringify(new CSSMathProduct(CSS.px(2), new CSSMathInvert(CSS.px(1))).type()) !== '{}') return false;
        for (const invoke of [() => new CSSUnitValue(1, 'no-unit'),
                              () => new CSSUnitValue(Infinity, 'px'),
                              () => new CSSMathSum(CSS.px(1), CSS.deg(2))]) {
            try { invoke(); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        }
        try { new CSSMathSum(); return false; } catch (e) { if (e.name !== 'SyntaxError') return false; }
        unit.marker = 7;
        globalThis.numericArray = sum.values;
        return true;
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(ctx, "numericArray[0].marker === 7 && String(numericArray[1]) === '10mm'"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM values are checked a level at a time as they are made, whatever their depth") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        // A math value's type is the type of its children, however deep they go.
        let length = CSS.px(1);
        for (let i = 0; i < 2000; ++i)
            length = new CSSMathNegate(length);
        try { new CSSMathSum(length, CSS.s(1)); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        if (!(new CSSTranslate(length, CSS.px(0)) instanceof CSSTranslate)) return false;
        try { new CSSRotate(length); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        // A value deeper than a typed value may be can be created, and serializing it throws a RangeError.
        try { String(length); return false; } catch (e) { if (!(e instanceof RangeError)) return false; }
        // A value that C returns carries its type, in the same way as a value from a constructor.
        const parsed = CSSNumericValue.parse('calc(1px + 2em)');
        return new CSSMathMax(parsed, CSS.percent(5)).toString() === 'max(1px + 2em, 5%)';
    })())JS"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM transforms retain children and serialize their dimensions") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        const x = CSS.px(10), y = CSS.px(20);
        const translate = new CSSTranslate(x, y);
        if (translate.x !== x || !translate.is2D || String(translate) !== 'translate(10px, 20px)') return false;
        translate.z.marker = 41;
        const sum = new CSSMathSum(1, 2);
        sum.values[0].marker = 42;
        const scale = new CSSScale(2, 3, 4);
        const rotate = new CSSRotate(0, 0, 1, CSS.deg(90));
        if (scale.is2D || rotate.is2D || String(scale) !== 'scale3d(2, 3, 4)') return false;
        if (String(new CSSScale(2, 2)) !== 'scale(2)' || String(new CSSRotate(CSS.deg(30))) !== 'rotate(30deg)') return false;
        const skew = new CSSSkew(CSS.deg(10), CSS.deg(0));
        skew.is2D = false;
        if (!skew.is2D || String(skew) !== 'skew(10deg)') return false;
        if (String(new CSSPerspective(CSS.px(-1))) !== 'perspective(calc(-1px))') return false;
        const list = new CSSTransformValue([translate, scale]);
        if (list[0] !== translate || list.is2D) return false;
        const m = list.toMatrix();
        if (m.m11 !== 2 || m.m22 !== 3 || m.m33 !== 4 || m.m41 !== 10 || m.m42 !== 20) return false;
        for (const invoke of [() => new CSSScale(CSS.px(1), 1),
                              () => translate.z = CSS.percent(2),
                              () => new CSSTranslate(CSS.px(1), CSS.px(1), null),
                              () => new CSSTransformValue([]),
                              () => new CSSTranslate(CSS.em(1), CSS.px(0)).toMatrix(),
                              () => CSS.px(Infinity)]) {
            try { invoke(); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        }
        translate.x = CSS.px(7);
        translate.x.marker = 43;
        globalThis.transform = translate;
        globalThis.sum = sum;
        return true;
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(
        ctx,
        "transform.z.marker === 41 && transform.x.marker === 43 && sum.values[0].marker === 42"));
    CHECK(evaluates(ctx, R"JS((() => {
        const t = new CSSTranslate(CSS.px(1), CSS.px(2));
        t.z.owner = t;
        return true;
    })())JS"));
    JS_RunGC(runtime);
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM matrices parse CSS, expose mutable elements and preserve identity") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        const m = new DOMMatrixReadOnly([1, 0, 0, 1, 2, 3]);
        if (String(m) !== 'matrix(1, 0, 0, 1, 2, 3)' || !m.is2D || m.isIdentity) return false;
        const parsed = new DOMMatrix('translate(2px, 3px) scale(2)');
        if (parsed.m11 !== 2 || parsed.m41 !== 2 || parsed.m42 !== 3) return false;
        const component = new CSSMatrixComponent(m);
        component.matrix.a = 4;
        if (m.a !== 1 || component.matrix.a !== 4) return false;
        component.matrix = parsed;
        if (component.matrix !== parsed) return false;
        parsed.marker = 8;
        globalThis.matrixComponent = component;
        try { new DOMMatrix([]); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        try { new DOMMatrix('translate(1em)'); return false; } catch (e) { if (e.name !== 'SyntaxError') return false; }
        const bad = new DOMMatrix([NaN, 0, 0, 1, 0, 0]);
        try { String(bad); return false; } catch (e) { if (e.name !== 'InvalidStateError') return false; }
        const badTransform = new CSSTransformValue([new CSSMatrixComponent(bad)]);
        try { String(badTransform); return false; } catch (e) { if (e.name !== 'InvalidStateError') return false; }
        return true;
    })())JS"));
    JS_RunGC(runtime);
    CHECK(evaluates(ctx, "matrixComponent.matrix.marker === 8"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}

TEST_CASE("Typed OM colors parse as written, rectify their channels and serialize their function") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = JS_NewContext(runtime);
    prJsDomBind(ctx);
    prJsCssBind(ctx);
    CHECK(evaluates(ctx, R"JS((() => {
        const lab = CSSColorValue.parse('lab(50 25% none / 0.5)');
        if (!(lab instanceof CSSLab) || String(lab.l) !== '50%' || String(lab.a) !== '31.25' ||
            String(lab.b) !== 'none' || String(lab.alpha) !== '50%') return false;
        if (String(lab) !== 'lab(50% 31.25 none / 50%)') return false;
        const lch = CSSColorValue.parse('lch(40% 75 0.5turn)');
        if (!(lch instanceof CSSLCH) || String(lch.c) !== '50%' || String(lch.h) !== '0.5turn') return false;
        const rgb = CSSColorValue.parse('rgb(10% 20 calc(30))');
        if (String(rgb) !== 'rgb(10% 20 calc(30))') return false;
        if (CSSColorValue.parse('color-mix(in srgb, red, blue)').constructor !== CSSStyleValue) return false;
        if (CSSColorValue.parse('currentColor').constructor !== CSSKeywordValue) return false;
        return true;
    })())JS"));
    CHECK(evaluates(ctx, R"JS((() => {
        const color = CSSColorValue.parse('color(display-p3 0.5 50% none)');
        if (!(color instanceof CSSColor) || String(color.colorSpace) !== 'display-p3') return false;
        const channels = color.channels;
        if (!Array.isArray(channels) || channels.length !== 3 || String(channels[0]) !== '50%') return false;
        if (color.channels !== channels) return false;
        channels.push(0.25);
        if (String(color) !== 'color(display-p3 50% 50% none 25%)') return false;
        try { channels[0] = CSS.number(1); return false; } catch (e) { if (e.name !== 'SyntaxError') return false; }
        color.channels = [0, 1];
        color.alpha = 0.5;
        if (String(color) !== 'color(display-p3 0% 100% / 50%)' || color.channels !== channels) return false;
        const made = new CSSColor('xyz', [CSS.percent(1)]);
        return String(made) === 'color(xyz 1%)';
    })())JS"));
    CHECK(evaluates(ctx, R"JS((() => {
        const hsl = new CSSHSL(90, 0.5, CSS.percent(25));
        hsl.h = CSS.rad(1);
        if (String(hsl) !== 'hsl(1rad 50% 25%)') return false;
        try { hsl.s = CSS.number(1); return false; } catch (e) { if (e.name !== 'SyntaxError') return false; }
        try { new CSSHWB(90, 0, 0); return false; } catch (e) { if (!(e instanceof TypeError)) return false; }
        return String(new CSSOKLab(0.5, -0.1, 0.1, 0)) === 'oklab(50% -0.1 0.1 / 0%)';
    })())JS"));
    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
