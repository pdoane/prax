#include "html/quickjs/src/custom_elements_quickjs.h"

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/dom_quickjs.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

namespace pr_impl {

namespace {

JSClassID registryClass;
JSClassID internalsClass;
JSClassID stateSetClass;

struct Registry;

// html/#custom-element-definition. Its values live in an object that the registry holds: its constructor, its
// prototype, each lifecycle callback by name, and its construction stack. An entry of the stack for an element that has
// already been constructed is null.
struct Definition {
    PrString name;              // its name, which is its local name, as for an autonomous custom element
    Array<PrString> observed;   // html/#concept-custom-element-definition-observed-attributes
    uint32_t index;             // the index of its object among the registry's objects
    bool callbacks[4];          // true for each lifecycle callback that it has, indexed by PrDomCustomCallback
    bool disableInternals;
    bool disableShadow;
    bool formAssociated;
};

// html/#customelementregistry. Its object holds the values of its definitions, as an array, and the promises that
// whenDefined created, keyed by name.
struct Registry {
    PrDomCustomElementRegistry hooks;
    JSContext* ctx;
    JSValue object;             // the registry's object, which it does not keep alive
    PrHtmlWindow* window;
    Array<Definition*> definitions;
    bool running;               // html/#element-definition-is-running
};

constexpr uint32_t kDefinitionsSlot = 0;
constexpr uint32_t kPromisesSlot = 1;

// The names of the lifecycle callbacks, indexed by PrDomCustomCallback.
constexpr const char* kCallbackNames[] = {
    "connectedCallback", "disconnectedCallback", "adoptedCallback", "attributeChangedCallback",
};

// The callbacks of a form-associated custom element (html/#custom-elements-face-example).
constexpr const char* kFormCallbackNames[] = {
    "formAssociatedCallback",
    "formResetCallback",
    "formDisabledCallback",
    "formStateRestoreCallback",
};

void definitionDestroy(Definition* definition) {
    prStringReset(&definition->name);
    stringsReset(&definition->observed);
    prMemoryFree(definition);
}

void registryDestroy(Registry* PR_NULLABLE registry) {
    if (registry == nullptr)
        return;
    for (uint32_t i = 0; i < registry->definitions.count; ++i)
        definitionDestroy(registry->definitions.data[i]);
    registry->definitions.reset();
    prMemoryFree(registry);
}

// Returns the object that holds the definition's values.
JSValue definitionObject(const Registry* registry, const Definition* definition) {
    JSValue definitions = jsReference(registry->ctx, registry->object, kDefinitionsSlot);
    JSValue object = JS_GetPropertyUint32(registry->ctx, definitions, definition->index);
    JS_FreeValue(registry->ctx, definitions);
    return object;
}

JSValue definitionValue(const Registry* registry, const Definition* definition, const char* name) {
    JSValue object = definitionObject(registry, definition);
    JSValue value = JS_GetPropertyStr(registry->ctx, object, name);
    JS_FreeValue(registry->ctx, object);
    return value;
}

JSValue elementToJs(JSContext* ctx, PrDomElement* element) {
    return prJsDomWrapEventTarget(ctx, JS_UNDEFINED, prDomElementAsEventTarget(element));
}

// Returns the registry of the current global's window, or null if it has none.
Registry* PR_NULLABLE currentRegistry(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue window = JS_GetPrototype(ctx, global);
    JSValue object = JS_IsObject(window) ? jsSameObject(ctx, window, "customElements") : JS_UNDEFINED;
    Registry* registry = static_cast<Registry*>(jsHandleIn(ctx, object, registryClass));
    JS_FreeValue(ctx, object);
    JS_FreeValue(ctx, window);
    JS_FreeValue(ctx, global);
    return registry;
}

Definition* PR_NULLABLE definitionNamed(const Registry* registry, PrStringView name) {
    for (uint32_t i = 0; i < registry->definitions.count; ++i) {
        if (prStringView(&registry->definitions.data[i]->name) == name)
            return registry->definitions.data[i];
    }
    return nullptr;
}

Definition* PR_NULLABLE definitionOf(const Registry* registry, JSValueConst constructor) {
    for (uint32_t i = 0; i < registry->definitions.count; ++i) {
        JSValue value = definitionValue(registry, registry->definitions.data[i], "constructor");
        bool same = JS_IsSameValue(registry->ctx, value, constructor);
        JS_FreeValue(registry->ctx, value);
        if (same)
            return registry->definitions.data[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// The registry's hooks

void* PR_NULLABLE lookup(void* PR_NULLABLE context, PrStringView localName, PrStringView) {
    // Customized built-in elements are not supported, so a definition is identified by its local name alone.
    return definitionNamed(static_cast<Registry*>(context), localName);
}

bool observes(void* PR_NULLABLE, void* definition, PrDomCustomCallback callback, PrStringView name) {
    Definition* held = static_cast<Definition*>(definition);
    if (!held->callbacks[callback])
        return false;
    if (callback != PrDomCustomCallback_AttributeChanged)
        return true;
    for (uint32_t i = 0; i < held->observed.count; ++i) {
        if (prStringView(&held->observed.data[i]) == name)
            return true;
    }
    return false;
}

// The construction step of html/#concept-upgrade-an-element. While the constructor runs, the element's object is the
// last entry of the construction stack, which the HTML element constructor returns.
bool upgrade(void* PR_NULLABLE context, PrDomElement* element, void* definition) {
    Registry* registry = static_cast<Registry*>(context);
    Definition* held = static_cast<Definition*>(definition);
    JSContext* ctx = registry->ctx;
    JSValue object = elementToJs(ctx, element);
    JSValue stack = definitionValue(registry, held, "stack");
    int64_t depth = 0;
    JS_GetLength(ctx, stack, &depth);
    JS_SetPropertyUint32(ctx, stack, uint32_t(depth), JS_DupValue(ctx, object));
    bool ok = true;
    if (held->disableShadow && prDomElementHostedShadowRoot(element) != nullptr) {
        jsDomExceptionThrow(ctx, "NotSupportedError");
        ok = false;
    }
    if (ok) {
        JSValue constructor = definitionValue(registry, held, "constructor");
        JSValue result = JS_CallConstructor(ctx, constructor, 0, nullptr);
        JS_FreeValue(ctx, constructor);
        ok = !JS_IsException(result);
        if (ok && !JS_IsSameValue(ctx, result, object)) {
            JS_ThrowTypeError(ctx, "The constructor did not answer the element being upgraded");
            ok = false;
        }
        JS_FreeValue(ctx, result);
    }
    if (!ok)
        jsReportException(ctx);
    JS_SetPropertyStr(ctx, stack, "length", JS_NewInt64(ctx, depth));
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, object);
    return ok;
}

// Calls the definition's constructor synchronously, as dom/#concept-create-element specifies, and returns the element
// that it produces.
PrDomElement* PR_NULLABLE construct(void* PR_NULLABLE context, void* definition) {
    Registry* registry = static_cast<Registry*>(context);
    JSContext* ctx = registry->ctx;
    JSValue constructor = definitionValue(registry, static_cast<Definition*>(definition), "constructor");
    JSValue result = JS_CallConstructor(ctx, constructor, 0, nullptr);
    JS_FreeValue(ctx, constructor);
    PrDomElement* element = nullptr;
    if (!JS_IsException(result)) {
        element = jsHandleAs<PrDomElement>(ctx, result, prJsDomElementClass);
        if (element == nullptr)
            JS_ThrowTypeError(ctx, "The constructor did not answer an element");
        else
            prDomNodeRetain(prDomElementAsNode(element));
    }
    JS_FreeValue(ctx, result);
    if (element == nullptr)
        jsReportException(ctx);
    return element;
}

// Reports an exception that the DOM throws, at the registry's global.
void report(void* PR_NULLABLE context, PrStringView name) {
    JSContext* ctx = static_cast<Registry*>(context)->ctx;
    if (name == "TypeError")
        JS_ThrowTypeError(ctx, "The constructor did not answer an HTML element");
    else
        jsDomExceptionThrow(ctx, name);
    jsReportException(ctx);
}

JSValue stringOrNull(JSContext* ctx, PrStringView text) {
    return text.data != nullptr ? jsNewStringView(ctx, text) : JS_NULL;
}

// Invokes a callback reaction with its arguments (html/#concept-custom-element-definition-lifecycle-callbacks), with
// the element as this.
void invoke(void* PR_NULLABLE context, PrDomElement* element, void* definition, const PrDomCustomReaction* reaction) {
    Registry* registry = static_cast<Registry*>(context);
    JSContext* ctx = registry->ctx;
    JSValue callback = definitionValue(registry, static_cast<Definition*>(definition),
        kCallbackNames[reaction->callback]);
    JSValue object = elementToJs(ctx, element);
    JSValue args[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    int argc = 0;
    if (reaction->callback == PrDomCustomCallback_Adopted) {
        args[0] = prJsDomWrapDocument(ctx, reaction->oldDocument);
        args[1] = prJsDomWrapDocument(ctx, reaction->newDocument);
        argc = 2;
    } else if (reaction->callback == PrDomCustomCallback_AttributeChanged) {
        args[0] = stringOrNull(ctx, reaction->name);
        args[1] = stringOrNull(ctx, reaction->oldValue);
        args[2] = stringOrNull(ctx, reaction->value);
        args[3] = stringOrNull(ctx, reaction->namespaceUrl);
        argc = 4;
    }
    JSValue result = JS_Call(ctx, callback, object, argc, args);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    for (JSValue& arg : args)
        JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, object);
    JS_FreeValue(ctx, callback);
}

void queueMicrotask(void* PR_NULLABLE context, void (*run)(void* data), void* data) {
    jsEnqueueNative(static_cast<Registry*>(context)->ctx, run, data);
}

// ---------------------------------------------------------------------------------------------------------------------
// CustomElementRegistry (html/#customelementregistry)

// Reads the strings of a sequence<DOMString> argument. Returns false, with the exception pending, on failure.
bool stringSequence(JSContext* ctx, JSValueConst value, Array<PrString>* out) {
    return jsSequence(ctx, value, [&](JSValue item) {
        StringArgument text = {ctx};
        bool read = text.read(item);
        JS_FreeValue(ctx, item);
        if (read)
            out->push(prStringCopy(text.value()));
        return read;
    });
}

// Reads a callback from the prototype, which is undefined or a function (webidl/#dfn-callback-function).
bool callbackOf(JSContext* ctx, JSValueConst prototype, JSValueConst holder, const char* name, bool* present) {
    JSValue value = JS_GetPropertyStr(ctx, prototype, name);
    if (JS_IsException(value))
        return false;
    *present = !JS_IsUndefined(value);
    if (*present && !JS_IsFunction(ctx, value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "%s is not a function", name);
        return false;
    }
    JS_SetPropertyStr(ctx, holder, name, value);
    return true;
}

// html/#dom-customelementregistry-define
JSValue define(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    Registry* registry = jsThis<Registry>(ctx, self, registryClass);
    StringArgument name = {ctx};
    if (registry == nullptr || !jsArgCount(ctx, argc, 2) || !name.read(argv[0]))
        return JS_EXCEPTION;
    JSValueConst constructor = argv[1];
    if (!JS_IsConstructor(ctx, constructor))
        return JS_ThrowTypeError(ctx, "The constructor is not a constructor");
    if (!prDomIsValidCustomElementName(name.value()))
        return jsDomExceptionThrow(ctx, "SyntaxError");
    if (definitionNamed(registry, name.value()) != nullptr || definitionOf(registry, constructor) != nullptr)
        return jsDomExceptionThrow(ctx, "NotSupportedError");
    bool present = false;
    JSValueConst options = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (!jsDictionary(ctx, options, &present))
        return JS_EXCEPTION;
    if (present) {
        JSValue extends = JS_GetPropertyStr(ctx, options, "extends");
        if (JS_IsException(extends))
            return extends;
        bool customized = !JS_IsUndefined(extends);
        JS_FreeValue(ctx, extends);
        // Customized built-in elements are not supported.
        if (customized)
            return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
    if (registry->running)
        return jsDomExceptionThrow(ctx, "NotSupportedError");

    registry->running = true;
    Definition* definition = allocZeroed<Definition>();
    JSValue holder = JS_NewObjectProto(ctx, JS_NULL);
    JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
    bool ok = !JS_IsException(prototype);
    if (ok && !JS_IsObject(prototype)) {
        JS_ThrowTypeError(ctx, "The constructor's prototype is not an object");
        ok = false;
    }
    for (uint32_t i = 0; ok && i < countOf(kCallbackNames); ++i)
        ok = callbackOf(ctx, prototype, holder, kCallbackNames[i], &definition->callbacks[i]);
    if (ok && definition->callbacks[PrDomCustomCallback_AttributeChanged]) {
        JSValue observed = JS_GetPropertyStr(ctx, constructor, "observedAttributes");
        ok = !JS_IsException(observed) &&
            (JS_IsUndefined(observed) || stringSequence(ctx, observed, &definition->observed));
        JS_FreeValue(ctx, observed);
    }
    if (ok) {
        Array<PrString> disabled;
        JSValue features = JS_GetPropertyStr(ctx, constructor, "disabledFeatures");
        ok = !JS_IsException(features) && (JS_IsUndefined(features) || stringSequence(ctx, features, &disabled));
        JS_FreeValue(ctx, features);
        for (uint32_t i = 0; i < disabled.count; ++i) {
            definition->disableInternals |= prStringView(&disabled.data[i]) == "internals";
            definition->disableShadow |= prStringView(&disabled.data[i]) == "shadow";
        }
        stringsReset(&disabled);
    }
    if (ok) {
        JSValue formAssociated = JS_GetPropertyStr(ctx, constructor, "formAssociated");
        ok = !JS_IsException(formAssociated);
        definition->formAssociated = ok && JS_ToBool(ctx, formAssociated) == 1;
        JS_FreeValue(ctx, formAssociated);
        for (uint32_t i = 0; ok && definition->formAssociated && i < countOf(kFormCallbackNames); ++i) {
            bool has = false;
            ok = callbackOf(ctx, prototype, holder, kFormCallbackNames[i], &has);
        }
    }
    registry->running = false;
    if (!ok) {
        JS_FreeValue(ctx, prototype);
        JS_FreeValue(ctx, holder);
        definitionDestroy(definition);
        return JS_EXCEPTION;
    }

    JS_SetPropertyStr(ctx, holder, "constructor", JS_DupValue(ctx, constructor));
    JS_SetPropertyStr(ctx, holder, "prototype", prototype);
    JS_SetPropertyStr(ctx, holder, "stack", JS_NewArray(ctx));
    definition->name = prStringCopy(name.value());
    definition->index = registry->definitions.count;
    JSValue definitions = jsReference(ctx, self, kDefinitionsSlot);
    JS_SetPropertyUint32(ctx, definitions, definition->index, holder);
    JS_FreeValue(ctx, definitions);
    registry->definitions.push(definition);

    // The definition upgrades the elements that it defines in the window's document, and resolves the promise that
    // whenDefined returned for the name.
    PrDomDocument* document = prHtmlWindowDocument(registry->window);
    prDomDocumentUpgradeCandidates(document, name.value(), definition);
    JSValue promises = jsReference(ctx, self, kPromisesSlot);
    JSAtom key = JS_NewAtomLen(ctx, name.value().data, name.value().size);
    JSValue pending = JS_GetProperty(ctx, promises, key);
    if (JS_IsObject(pending)) {
        JSValue resolve = JS_GetPropertyUint32(ctx, pending, 1);
        JSValue result = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &constructor);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, resolve);
        JS_DeleteProperty(ctx, promises, key, 0);
    }
    JS_FreeValue(ctx, pending);
    JS_FreeAtom(ctx, key);
    JS_FreeValue(ctx, promises);
    return JS_UNDEFINED;
}

// html/#dom-customelementregistry-get
JSValue get(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    Registry* registry = jsThis<Registry>(ctx, self, registryClass);
    StringArgument name = {ctx};
    if (registry == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    Definition* definition = definitionNamed(registry, name.value());
    return definition != nullptr ? definitionValue(registry, definition, "constructor") : JS_UNDEFINED;
}

// html/#dom-customelementregistry-getname
JSValue getName(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    Registry* registry = jsThis<Registry>(ctx, self, registryClass);
    if (registry == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Expected a constructor");
    Definition* definition = definitionOf(registry, argv[0]);
    return definition != nullptr ? jsNewString(ctx, &definition->name) : JS_NULL;
}

// html/#dom-customelementregistry-whendefined
JSValue whenDefined(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    Registry* registry = jsThis<Registry>(ctx, self, registryClass);
    StringArgument name = {ctx};
    if (registry == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    if (!prDomIsValidCustomElementName(name.value())) {
        JSValue error = jsDomExceptionNew(ctx, "SyntaxError");
        JSValue promise = jsSettledPromise(ctx, error, true);
        JS_FreeValue(ctx, error);
        return promise;
    }
    if (Definition* definition = definitionNamed(registry, name.value())) {
        JSValue constructor = definitionValue(registry, definition, "constructor");
        JSValue promise = jsSettledPromise(ctx, constructor, false);
        JS_FreeValue(ctx, constructor);
        return promise;
    }
    JSValue promises = jsReference(ctx, self, kPromisesSlot);
    JSAtom key = JS_NewAtomLen(ctx, name.value().data, name.value().size);
    JSValue pending = JS_GetProperty(ctx, promises, key);
    if (!JS_IsObject(pending)) {
        JS_FreeValue(ctx, pending);
        JSValue functions[2];
        JSValue promise = JS_NewPromiseCapability(ctx, functions);
        pending = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pending, 0, promise);
        JS_SetPropertyUint32(ctx, pending, 1, functions[0]);
        JS_FreeValue(ctx, functions[1]);
        JS_SetProperty(ctx, promises, key, JS_DupValue(ctx, pending));
    }
    JSValue promise = JS_GetPropertyUint32(ctx, pending, 0);
    JS_FreeValue(ctx, pending);
    JS_FreeAtom(ctx, key);
    JS_FreeValue(ctx, promises);
    return promise;
}

// html/#dom-customelementregistry-upgrade
JSValue upgradeTree(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    if (jsThis<Registry>(ctx, self, registryClass) == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* root = jsHandleAs<PrDomNode>(ctx, argv[0], prJsDomNodeClass);
    if (root == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Node");
    prDomNodeUpgrade(root);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// ElementInternals and CustomStateSet (html/#elementinternals, html/#customstateset)

JSValue internalsShadowRoot(JSContext* ctx, JSValueConst self) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, self, internalsClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomShadowRoot* shadow = prDomElementHostedShadowRoot(element);
    return shadow != nullptr ? prJsDomWrapEventTarget(ctx, self, prDomNodeAsEventTarget(prDomShadowRootAsNode(shadow))) : JS_NULL;
}

// html/#dom-elementinternals-states. The set belongs to the internals object, and mirrors each change to the element so
// that :state() matches.
JSValue internalsStates(JSContext* ctx, JSValueConst self) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, self, internalsClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "states", [&] {
        JSValue set = jsWrap(ctx, stateSetClass, self, element, false);
        if (!JS_IsException(set)) {
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue constructor = JS_GetPropertyStr(ctx, global, "Set");
            JSValue states = JS_CallConstructor(ctx, constructor, 0, nullptr);
            jsSetReference(ctx, set, 0, states);
            JS_FreeValue(ctx, states);
            JS_FreeValue(ctx, constructor);
            JS_FreeValue(ctx, global);
        }
        return set;
    });
}

// The members of a form-associated custom element's internals (html/#form-associated-custom-elements). A custom element
// that is not form-associated lacks them, so they throw NotSupportedError.
JSValue internalsFormMember(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    if (jsThis<PrDomElement>(ctx, self, internalsClass) == nullptr)
        return JS_EXCEPTION;
    return jsDomExceptionThrow(ctx, "NotSupportedError");
}

JSValue internalsFormGetter(JSContext* ctx, JSValueConst self) {
    return internalsFormMember(ctx, self, 0, nullptr);
}

// Returns the set that a CustomStateSet holds.
JSValue stateSet(JSContext* ctx, JSValueConst self, PrDomElement** element) {
    *element = jsThis<PrDomElement>(ctx, self, stateSetClass);
    return *element != nullptr ? jsReference(ctx, self, 0) : JS_EXCEPTION;
}

// Calls the set's Set method `name` with the arguments.
JSValue callSet(JSContext* ctx, JSValueConst set, const char* name, int argc, JSValueConst* argv) {
    JSValue method = JS_GetPropertyStr(ctx, set, name);
    JSValue result = JS_Call(ctx, method, set, argc, argv);
    JS_FreeValue(ctx, method);
    return result;
}

// add, delete and clear, selected by the magic. They change the element's states as they change the set.
JSValue stateChange(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = nullptr;
    JSValue set = stateSet(ctx, self, &element);
    if (JS_IsException(set))
        return set;
    StringArgument state = {ctx};
    if (magic != 2 && (!jsArgCount(ctx, argc, 1) || !state.read(argv[0]))) {
        JS_FreeValue(ctx, set);
        return JS_EXCEPTION;
    }
    JSValue value = magic != 2 ? jsNewStringView(ctx, state.value()) : JS_UNDEFINED;
    constexpr const char* kMethods[] = {"add", "delete", "clear"};
    JSValue result = callSet(ctx, set, kMethods[magic], magic != 2 ? 1 : 0, &value);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, set);
    if (magic == 2)
        prDomElementClearCustomStates(element);
    else
        prDomElementSetCustomState(element, state.value(), magic == 0);
    if (magic == 0 && !JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        return JS_DupValue(ctx, self);
    }
    return result;
}

// has, forEach, entries, keys, values and size, selected by the name that the magic gives. The set's own methods
// implement them.
JSValue stateRead(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = nullptr;
    JSValue set = stateSet(ctx, self, &element);
    if (JS_IsException(set))
        return set;
    constexpr const char* kMethods[] = {"has", "forEach", "entries", "keys", "values"};
    JSValue result = callSet(ctx, set, kMethods[magic], argc, argv);
    JS_FreeValue(ctx, set);
    return result;
}

JSValue stateSize(JSContext* ctx, JSValueConst self) {
    PrDomElement* element = nullptr;
    JSValue set = stateSet(ctx, self, &element);
    if (JS_IsException(set))
        return set;
    JSValue size = JS_GetPropertyStr(ctx, set, "size");
    JS_FreeValue(ctx, set);
    return size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

constexpr JSCFunctionListEntry kRegistryProto[] = {
    PR_JS_TAG("CustomElementRegistry"),
    PR_JS_METHOD("define", 2, define),
    PR_JS_METHOD("get", 1, get),
    PR_JS_METHOD("getName", 1, getName),
    PR_JS_METHOD("whenDefined", 1, whenDefined),
    PR_JS_METHOD("upgrade", 1, upgradeTree),
};

constexpr JSCFunctionListEntry kInternalsProto[] = {
    PR_JS_TAG("ElementInternals"),
    PR_JS_GET("shadowRoot", internalsShadowRoot),
    PR_JS_GET("states", internalsStates),
    PR_JS_METHOD("setFormValue", 1, internalsFormMember),
    PR_JS_GET("form", internalsFormGetter),
    PR_JS_METHOD("setValidity", 0, internalsFormMember),
    PR_JS_GET("willValidate", internalsFormGetter),
    PR_JS_GET("validity", internalsFormGetter),
    PR_JS_GET("validationMessage", internalsFormGetter),
    PR_JS_METHOD("checkValidity", 0, internalsFormMember),
    PR_JS_METHOD("reportValidity", 0, internalsFormMember),
    PR_JS_GET("labels", internalsFormGetter),
};

constexpr JSCFunctionListEntry kStateSetProto[] = {
    PR_JS_TAG("CustomStateSet"),
    PR_JS_MAGIC("add", 1, stateChange, 0),
    PR_JS_MAGIC("delete", 1, stateChange, 1),
    PR_JS_MAGIC("clear", 0, stateChange, 2),
    PR_JS_MAGIC("has", 1, stateRead, 0),
    PR_JS_MAGIC("forEach", 1, stateRead, 1),
    PR_JS_MAGIC("entries", 0, stateRead, 2),
    PR_JS_MAGIC("keys", 0, stateRead, 3),
    PR_JS_MAGIC("values", 0, stateRead, 4),
    PR_JS_GET("size", stateSize),
};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("CustomElementRegistry", registryClass, (jsFinalizer<&registryClass, registryDestroy>),
            kRegistryProto)
    },
    {PR_JS_CLASS("ElementInternals", internalsClass, jsViewFinalizer<&internalsClass>, kInternalsProto)},
    // webidl/#dfn-setlike: a set's @@iterator is values.
    {
        PR_JS_CLASS("CustomStateSet", stateSetClass, jsViewFinalizer<&stateSetClass>, kStateSetProto),
        .iterator = "values",
    },
};

} // namespace

void jsCustomElementsBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

bool jsCustomElementsSetGlobal(JSContext* ctx, JSValueConst object, PrHtmlWindow* window) {
    Registry* registry = allocZeroed<Registry>();
    registry->hooks = PrDomCustomElementRegistry{
        registry,      prHtmlWindowAgent(window),
        lookup,        observes,
        upgrade,       construct,
        report,        invoke,
        queueMicrotask,
    };
    registry->ctx = ctx;
    registry->window = window;
    JSValue value = jsWrap(ctx, registryClass, object, registry, true);
    if (JS_IsException(value)) {
        registryDestroy(registry);
        return false;
    }
    registry->object = value;
    JSValue definitions = JS_NewArray(ctx);
    JSValue promises = JS_NewObjectProto(ctx, JS_NULL);
    jsSetReference(ctx, value, kDefinitionsSlot, definitions);
    jsSetReference(ctx, value, kPromisesSlot, promises);
    JS_FreeValue(ctx, promises);
    JS_FreeValue(ctx, definitions);
    jsSetSameObject(ctx, object, "customElements", value);
    JS_FreeValue(ctx, value);
    prHtmlWindowSetCustomElementRegistry(window, &registry->hooks);
    return true;
}

JSValue jsCustomElements(JSContext* ctx, JSValueConst object) {
    return jsSameObject(ctx, object, "customElements");
}

JSValue jsCustomElementsConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    Registry* registry = currentRegistry(ctx);
    if (registry == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    // The HTML element constructor itself constructs nothing.
    JSValue elementProto = JS_GetClassProto(ctx, htmlElementClass);
    JSValue own = JS_GetPropertyStr(ctx, elementProto, "constructor");
    bool itself = JS_IsSameValue(ctx, own, newTarget);
    JS_FreeValue(ctx, own);
    Definition* definition = itself ? nullptr : definitionOf(registry, newTarget);
    if (definition == nullptr) {
        JS_FreeValue(ctx, elementProto);
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    }
    JSValue prototype = JS_GetPropertyStr(ctx, newTarget, "prototype");
    if (JS_IsException(prototype)) {
        JS_FreeValue(ctx, elementProto);
        return prototype;
    }
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_DupValue(ctx, elementProto);
    }
    JS_FreeValue(ctx, elementProto);

    JSValue stack = definitionValue(registry, definition, "stack");
    int64_t depth = 0;
    JS_GetLength(ctx, stack, &depth);
    JSValue element;
    if (depth == 0) {
        PrDomDocument* document = prHtmlWindowDocument(registry->window);
        PrDomElement* made = prDomElementCreateCustom(document, prStringView(&definition->name), definition);
        element = elementToJs(ctx, made);
        prDomNodeRelease(prDomElementAsNode(made));
    } else {
        element = JS_GetPropertyUint32(ctx, stack, uint32_t(depth - 1));
        if (JS_IsNull(element)) {
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, prototype);
            return JS_ThrowTypeError(ctx, "The element is already constructed");
        }
        JS_SetPropertyUint32(ctx, stack, uint32_t(depth - 1), JS_NULL);
    }
    JS_FreeValue(ctx, stack);
    if (!JS_IsException(element))
        JS_SetPrototype(ctx, element, prototype);
    JS_FreeValue(ctx, prototype);
    return element;
}

JSValue jsCustomElementsAttachInternals(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, self, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    Registry* registry = currentRegistry(ctx);
    Definition* definition = registry != nullptr ? definitionNamed(registry, prDomElementLocalName(element)) : nullptr;
    JSValue attached = jsSameObject(ctx, self, "[[Internals]]");
    bool already = !JS_IsUndefined(attached);
    JS_FreeValue(ctx, attached);
    PrDomCustomElementState state = prDomElementCustomElementState(element);
    if (definition == nullptr || definition->disableInternals || already ||
            (state != PrDomCustomElementState_Precustomized && state != PrDomCustomElementState_Custom))
        return jsDomExceptionThrow(ctx, "NotSupportedError");
    JSValue internals = jsWrap(ctx, internalsClass, self, element, false);
    if (!JS_IsException(internals))
        jsSetSameObject(ctx, self, "[[Internals]]", internals);
    return internals;
}

} // namespace pr_impl
