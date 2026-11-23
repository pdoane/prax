#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the boolean value of evaluating `source`.
bool evaluates(JSContext* ctx, const char* source) {
    JSValue result = JS_Eval(ctx, source, strlen(source), "unscopables", JS_EVAL_TYPE_GLOBAL);
    bool value = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the prototypes name their [Unscopable] members, which a with statement skips") {
    JSRuntime* runtime = jsRuntimeCreate();
    JSContext* ctx = jsContextCreate(runtime);
    prJsDomBind(ctx);

    // webidl/#interface-prototype-object: an object with no prototype that names each member, which is configurable and
    // neither writable nor enumerable on the prototype.
    CHECK(evaluates(ctx, R"JS((() => {
        const names = (proto) => Object.keys(proto[Symbol.unscopables]).sort().join();
        const descriptor = Object.getOwnPropertyDescriptor(Element.prototype, Symbol.unscopables);
        return names(Element.prototype) === "after,append,before,prepend,remove,replaceChildren,replaceWith,slot"
            && names(CharacterData.prototype) === "after,before,remove,replaceWith"
            && names(DocumentType.prototype) === "after,before,remove,replaceWith"
            && names(Document.prototype) === "append,prepend,replaceChildren"
            && names(DocumentFragment.prototype) === "append,prepend,replaceChildren"
            && Object.getPrototypeOf(descriptor.value) === null && descriptor.configurable && !descriptor.writable
            && !descriptor.enumerable;
    })())JS"));

    // A name that the prototype makes unscopable resolves past the object, as it does in an event handler's scope.
    CHECK(evaluates(ctx, R"JS((() => {
        const before = () => "outer";
        const element = new Document().createElement("a");
        with (element) {
            return before() === "outer" && typeof appendChild === "function";
        }
    })())JS"));

    JS_FreeContext(ctx);
    jsRuntimeDestroy(runtime);
}
