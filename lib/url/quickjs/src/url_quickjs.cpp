// JavaScript binding of URL and URLSearchParams.
//
// url - https://url.spec.whatwg.org/

#include "pr/url_quickjs.h"

#include "pr/count_of.h"
#include "pr/quickjs_binding.h"
#include "pr/text_buffer.h"
#include "pr/url.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {
namespace {

JSClassID urlClass;
JSClassID urlSearchParamsClass;
JSClassID urlSearchParamsIteratorClass;

PrUrl* PR_NULLABLE urlOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrUrl>(ctx, object, urlClass, "URL");
}

PrUrlSearchParams* PR_NULLABLE paramsOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrUrlSearchParams>(ctx, object, urlSearchParamsClass, "URLSearchParams");
}

// Returns `text` as a JavaScript string, or the empty string if `text` is null.
JSValue stringOf(JSContext* ctx, PrStringView text) {
    return JS_NewStringLen(ctx, text.data != nullptr ? text.data : "", text.size);
}

// url/#api-url-parser. Parses the URL that argv[0] names into `out`, resolved against argv[1] if it is given. The base
// is parsed first. Returns false for arguments that do not convert. If either URL does not parse, `*out` is null.
bool parseApi(JSContext* ctx, int argc, JSValueConst* argv, PrUrl* PR_NULLABLE* out) {
    *out = nullptr;
    StringArgument url = {ctx};
    StringArgument base = {ctx};
    bool hasBase = jsArgGiven(argc, argv, 1);
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "Not enough arguments");
        return false;
    }
    if (!url.read(argv[0]) || (hasBase && !base.read(argv[1])))
        return false;
    PrUrl* parsedBase = nullptr;
    if (!hasBase || prUrlCreate(base.value(), nullptr, PrCharset_Utf8, &parsedBase) == PrStatus_Ok)
        prUrlCreate(url.value(), parsedBase, PrCharset_Utf8, out);
    prUrlDestroy(parsedBase);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// URL (url/#url-class)

// url/#dom-url-url
JSValue construct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrUrl* url = nullptr;
    if (!parseApi(ctx, argc, argv, &url))
        return JS_EXCEPTION;
    if (url == nullptr)
        return JS_ThrowTypeError(ctx, "Invalid URL");
    JSValue object = jsWrap(ctx, urlClass, JS_UNDEFINED, url, true);
    if (JS_IsException(object))
        prUrlDestroy(url);
    return jsConstructed(ctx, newTarget, object);
}

// url/#dom-url-parse and url/#dom-url-canparse, selected by the magic.
JSValue parseStatic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    PrUrl* url = nullptr;
    if (!parseApi(ctx, argc, argv, &url))
        return JS_EXCEPTION;
    if (magic != 0) {
        prUrlDestroy(url);
        return JS_NewBool(ctx, url != nullptr);
    }
    if (url == nullptr)
        return JS_NULL;
    JSValue object = jsWrap(ctx, urlClass, JS_UNDEFINED, url, true);
    if (JS_IsException(object))
        prUrlDestroy(url);
    return object;
}

// The getter steps of url/#url-class for the component that the magic names.
JSValue componentGet(JSContext* ctx, JSValueConst urlObject, int magic) {
    PrUrl* url = urlOf(ctx, urlObject);
    return url != nullptr ? jsTakeString(ctx, prUrlComponent(url, PrUrlComponent(magic))) : JS_EXCEPTION;
}

// The setter steps of url/#url-class. href's setter throws a TypeError for a URL that does not parse.
JSValue componentSet(JSContext* ctx, JSValueConst urlObject, JSValueConst input, int magic) {
    PrUrl* url = urlOf(ctx, urlObject);
    StringArgument value = {ctx};
    if (url == nullptr || !value.read(input))
        return JS_EXCEPTION;
    if (prUrlSetComponent(url, PrUrlComponent(magic), value.value()) != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "Invalid URL");
    return JS_UNDEFINED;
}

JSValue hrefMethod(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return componentGet(ctx, object, PrUrlComponent_Href);
}

// url/#dom-url-searchparams: the URL's query object, [SameObject]. It keeps the URL alive.
JSValue searchParamsGet(JSContext* ctx, JSValueConst urlObject) {
    PrUrl* url = urlOf(ctx, urlObject);
    if (url == nullptr)
        return JS_EXCEPTION;
    JSValue held = jsSameObject(ctx, urlObject, "searchParams");
    if (!JS_IsUndefined(held))
        return held;
    JSValue params = jsWrap(ctx, urlSearchParamsClass, urlObject, prUrlSearchParams(url), false);
    if (!JS_IsException(params))
        jsSetSameObject(ctx, urlObject, "searchParams", params);
    return params;
}

void urlFinalizer(JSRuntime* runtime, JSValue object) {
    jsFinalize<PrUrl>(runtime, object, urlClass, prUrlDestroy);
}

void urlMark(JSRuntime* runtime, JSValueConst object, JS_MarkFunc* mark) {
    jsMark(runtime, object, urlClass, mark);
}

// ---------------------------------------------------------------------------------------------------------------------
// URLSearchParams (url/#interface-urlsearchparams)

// A pair from an init sequence. The scope of the call holds the strings that its views refer to.
struct Pair {
    PrStringView name;
    PrStringView value;
};

// The (sequence<sequence<USVString>> or record<USVString, USVString> or USVString) init of
// url/#dom-urlsearchparams-urlsearchparams, fully converted before any of it is used.
struct InitArgument {
    RawArray<Pair> pairs;
    PrStringView string = {};

    void reset() { pairs.reset(); }

    bool read(JSContext* ctx, JSValueConst input) {
        if (JS_IsUndefined(input))
            return true;
        if (!JS_IsObject(input))
            return jsReadString(ctx, input, &string);
        JSValue method = jsIteratorMethod(ctx, input);
        if (JS_IsException(method))
            return false;
        bool paired = true;
        bool ok;
        if (JS_IsNull(method) || JS_IsUndefined(method))
            ok = readRecord(ctx, input);
        else
            ok = jsIterate(ctx, input, method, [&](JSValue item) {
                bool read = readPair(ctx, item, &paired);
                JS_FreeValue(ctx, item);
                return read;
            });
        JS_FreeValue(ctx, method);
        // The initializer checks each pair once all are converted.
        if (ok && !paired) {
            JS_ThrowTypeError(ctx, "A pair must hold exactly two strings");
            ok = false;
        }
        return ok;
    }

    // Reads one sequence<USVString>. Clears `paired` unless it holds exactly two strings.
    bool readPair(JSContext* ctx, JSValueConst entry, bool* paired) {
        PrStringView values[2] = {};
        uint32_t count = 0;
        bool ok = jsSequence(ctx, entry, [&](JSValue item) {
            PrStringView value;
            bool read = jsReadString(ctx, item, &value);
            JS_FreeValue(ctx, item);
            if (read && count < 2)
                values[count] = value;
            ++count;
            return read;
        });
        *paired = *paired && count == 2;
        if (ok)
            pairs.push(Pair{values[0], values[1]});
        return ok;
    }

    // webidl/#js-record: the enumerable string-keyed properties. A later duplicate replaces the value of an earlier
    // one.
    bool readRecord(JSContext* ctx, JSValueConst input) {
        JSPropertyEnum* properties = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &properties, &count, input, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return false;
        bool ok = true;
        for (uint32_t i = 0; ok && i < count; ++i) {
            Pair pair = {};
            JSValue key = JS_AtomToValue(ctx, properties[i].atom);
            ok = jsReadString(ctx, key, &pair.name);
            JS_FreeValue(ctx, key);
            JSValue value = ok ? JS_GetProperty(ctx, input, properties[i].atom) : JS_UNDEFINED;
            ok = ok && !JS_IsException(value) && jsReadString(ctx, value, &pair.value);
            JS_FreeValue(ctx, value);
            uint32_t found = 0;
            while (ok && found < pairs.count && pairs.data[found].name != pair.name)
                ++found;
            if (ok && found == pairs.count)
                pairs.push(pair);
            else if (ok)
                pairs.data[found].value = pair.value;
        }
        JS_FreePropertyEnum(ctx, properties, count);
        return ok;
    }
};

// url/#dom-urlsearchparams-urlsearchparams
JSValue constructParams(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    InitArgument init = {};
    if (!init.read(ctx, argc > 0 ? argv[0] : JS_UNDEFINED)) {
        init.reset();
        return JS_EXCEPTION;
    }
    PrUrlSearchParams* params = prUrlSearchParamsCreate();
    PrStringView text = init.string;
    if (text.size != 0 && text.data[0] == '?')
        text = prStringSlice(text, 1, text.size - 1);
    prUrlSearchParamsParse(params, text);
    for (uint32_t i = 0; i < init.pairs.count; ++i)
        prUrlSearchParamsAppend(params, init.pairs.data[i].name, init.pairs.data[i].value);
    init.reset();
    JSValue object = jsWrap(ctx, urlSearchParamsClass, JS_UNDEFINED, params, true);
    if (JS_IsException(object))
        prUrlSearchParamsDestroy(params);
    return jsConstructed(ctx, newTarget, object);
}

JSValue sizeGet(JSContext* ctx, JSValueConst paramsObject) {
    PrUrlSearchParams* params = paramsOf(ctx, paramsObject);
    return params != nullptr ? JS_NewUint32(ctx, prUrlSearchParamsSize(params)) : JS_EXCEPTION;
}

// The name, and the value if the method takes one. With `optional`, an absent value is the null view.
struct NameValue {
    StringArgument name;
    StringArgument value;

    bool read(JSContext* ctx, int argc, JSValueConst* argv, int required, bool withValue) {
        if (argc < required) {
            JS_ThrowTypeError(ctx, "Not enough arguments");
            return false;
        }
        return name.read(argv[0]) && (!withValue || !jsArgGiven(argc, argv, 1) || value.read(argv[1]));
    }
};

enum {
    Method_Append,
    Method_Delete,
    Method_Get,
    Method_GetAll,
    Method_Has,
    Method_Set,
};
typedef uint8_t Method;

// The methods of URLSearchParams that take a name, selected by the magic.
JSValue nameMethod(JSContext* ctx, JSValueConst paramsObject, int argc, JSValueConst* argv, int magic) {
    PrUrlSearchParams* params = paramsOf(ctx, paramsObject);
    NameValue arguments = {{ctx}, {ctx, PrStringView{}}};
    bool pair = magic == Method_Append || magic == Method_Set;
    bool withValue = pair || magic == Method_Delete || magic == Method_Has;
    if (params == nullptr || !arguments.read(ctx, argc, argv, pair ? 2 : 1, withValue))
        return JS_EXCEPTION;
    PrStringView name = arguments.name.value();
    PrStringView value = arguments.value.value();
    switch (Method(magic)) {
        case Method_Append: prUrlSearchParamsAppend(params, name, value); break;
        case Method_Delete: prUrlSearchParamsDelete(params, name, value); break;
        case Method_Get: return jsNewStringView(ctx, prUrlSearchParamsGet(params, name));
        case Method_GetAll: {
            JSValue all = JS_NewArray(ctx);
            uint32_t count = 0;
            for (uint32_t i = 0; i < prUrlSearchParamsSize(params); ++i) {
                if (prUrlSearchParamsNameAt(params, i) == name)
                    JS_SetPropertyUint32(ctx, all, count++, stringOf(ctx, prUrlSearchParamsValueAt(params, i)));
            }
            return all;
        }
        case Method_Has: return JS_NewBool(ctx, prUrlSearchParamsHas(params, name, value));
        case Method_Set: prUrlSearchParamsSet(params, name, value); break;
    }
    return JS_UNDEFINED;
}

JSValue sortMethod(JSContext* ctx, JSValueConst paramsObject, int, JSValueConst*) {
    PrUrlSearchParams* params = paramsOf(ctx, paramsObject);
    if (params == nullptr)
        return JS_EXCEPTION;
    prUrlSearchParamsSort(params);
    return JS_UNDEFINED;
}

JSValue toStringMethod(JSContext* ctx, JSValueConst paramsObject, int, JSValueConst*) {
    PrUrlSearchParams* params = paramsOf(ctx, paramsObject);
    return params != nullptr ? jsTakeString(ctx, prUrlSearchParamsToString(params)) : JS_EXCEPTION;
}

// Returns the [name, value] pair at the index argv[0], or undefined past the end.
JSValue entryAt(JSContext* ctx, JSValueConst paramsObject, int, JSValueConst* argv) {
    PrUrlSearchParams* params = paramsOf(ctx, paramsObject);
    uint32_t index = 0;
    if (params == nullptr || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    if (index >= prUrlSearchParamsSize(params))
        return JS_UNDEFINED;
    return jsEntry(ctx, stringOf(ctx, prUrlSearchParamsNameAt(params, index)),
        stringOf(ctx, prUrlSearchParamsValueAt(params, index)));
}

// entries, keys and values, selected by the part of each entry that the magic names.
JSValue iterate(JSContext* ctx, JSValueConst params, int, JSValueConst*, int magic) {
    if (paramsOf(ctx, params) == nullptr)
        return JS_EXCEPTION;
    return jsPairIterator(ctx, urlSearchParamsIteratorClass, params, entryAt, JsMapPart(magic));
}

JSValue forEachMethod(JSContext* ctx, JSValueConst params, int argc, JSValueConst* argv) {
    if (paramsOf(ctx, params) == nullptr)
        return JS_EXCEPTION;
    return jsPairForEach(ctx, params, entryAt, argc, argv);
}

void paramsFinalizer(JSRuntime* runtime, JSValue object) {
    jsFinalize<PrUrlSearchParams>(runtime, object, urlSearchParamsClass, prUrlSearchParamsDestroy);
}

void paramsMark(JSRuntime* runtime, JSValueConst object, JS_MarkFunc* mark) {
    jsMark(runtime, object, urlSearchParamsClass, mark);
}

#define PR_COMPONENT(name, component) \
    PR_JS_MAGIC_GET_SET(name, componentGet, componentSet, component)

constexpr JSCFunctionListEntry kUrlProto[] = {
    PR_JS_TAG("URL"),
    PR_COMPONENT("href", PrUrlComponent_Href),
    PR_JS_MAGIC_GET("origin", componentGet, PrUrlComponent_Origin),
    PR_COMPONENT("protocol", PrUrlComponent_Protocol),
    PR_COMPONENT("username", PrUrlComponent_Username),
    PR_COMPONENT("password", PrUrlComponent_Password),
    PR_COMPONENT("host", PrUrlComponent_Host),
    PR_COMPONENT("hostname", PrUrlComponent_Hostname),
    PR_COMPONENT("port", PrUrlComponent_Port),
    PR_COMPONENT("pathname", PrUrlComponent_Pathname),
    PR_COMPONENT("search", PrUrlComponent_Search),
    PR_JS_GET("searchParams", searchParamsGet),
    PR_COMPONENT("hash", PrUrlComponent_Hash),
    PR_JS_METHOD("toJSON", 0, hrefMethod),
    PR_JS_METHOD("toString", 0, hrefMethod),
};

constexpr JSCFunctionListEntry kUrlStatic[] = {
    PR_JS_MAGIC("parse", 1, parseStatic, 0),
    PR_JS_MAGIC("canParse", 1, parseStatic, 1),
};

constexpr JSCFunctionListEntry kUrlSearchParamsProto[] = {
    PR_JS_TAG("URLSearchParams"),
    PR_JS_GET("size", sizeGet),
    PR_JS_MAGIC("append", 2, nameMethod, Method_Append),
    PR_JS_MAGIC("delete", 1, nameMethod, Method_Delete),
    PR_JS_MAGIC("get", 1, nameMethod, Method_Get),
    PR_JS_MAGIC("getAll", 1, nameMethod, Method_GetAll),
    PR_JS_MAGIC("has", 1, nameMethod, Method_Has),
    PR_JS_MAGIC("set", 2, nameMethod, Method_Set),
    PR_JS_METHOD("sort", 0, sortMethod),
    PR_JS_METHOD("toString", 0, toStringMethod),
    PR_JS_MAGIC("entries", 0, iterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, iterate, JsMapPart_Keys),
    PR_JS_MAGIC("values", 0, iterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, forEachMethod),
};

#undef PR_COMPONENT

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsUrlBind(JSContext* ctx) {
    JSValue url = jsDefineClass(ctx, {
        .name = "URL",
        .id = &urlClass,
        .finalizer = urlFinalizer,
        .mark = urlMark,
        .proto = kUrlProto,
        .protoCount = int(countOf(kUrlProto)),
        .statics = kUrlStatic,
        .staticCount = int(countOf(kUrlStatic)),
        .constructor = construct,
        .length = 1,
    });
    // url/#url-class: [LegacyWindowAlias=webkitURL]
    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, "webkitURL", url, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);

    JSValue params = jsDefineClass(ctx, {
        .name = "URLSearchParams",
        .id = &urlSearchParamsClass,
        .finalizer = paramsFinalizer,
        .mark = paramsMark,
        .proto = kUrlSearchParamsProto,
        .protoCount = int(countOf(kUrlSearchParamsProto)),
        .constructor = constructParams,
        .iterator = "entries",
    });
    JS_FreeValue(ctx, params);
    jsPairIteratorBind(ctx, &urlSearchParamsIteratorClass, "URLSearchParams Iterator");
}

PrUrlSearchParams* PR_NULLABLE prJsUrlSearchParamsFromValue(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrUrlSearchParams>(ctx, value, urlSearchParamsClass);
}
