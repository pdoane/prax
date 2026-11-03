#include "pr/html_quickjs.h"

#include "pr/array.h"
#include "pr/css_quickjs.h"
#include "pr/dom_abort.h"
#include "pr/dom_quickjs.h"
#include "pr/fetch_quickjs.h"
#include "pr/html.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"
#include "pr/url.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

namespace pr_impl {

// A module of a realm (html/#module-script), by the name that imports resolve to: its URL, or an inline module script's
// own name. A module that an import names is recorded before it is parsed, and is undefined until then.
struct JsHtmlModule {
    PrString name;
    PrString base;                  // the URL that its imports resolve against, which is its import.meta.url
    JSValue module;                 // compiled, with its imports unresolved until its graph is linked; or undefined
    JSValue error;                  // html/#concept-script-parse-error, or undefined
    Array<uint32_t> requests;       // the modules that it imports, by index in the realm's modules
};

// A classic script that ran in a realm, by its name, which is the URL that it came from (html/#classic-script). Its
// import() calls resolve against its base URL (html/#concept-script-base-url).
struct JsHtmlClassicScript {
    PrString name;
    PrString base;
};

struct JsHtmlEngine;
struct JsHtmlRealm;

// An import() whose module graph the browsing context fetches. Its promise is settled once the graph is fetched. The
// import outlives the engine, which frees its values, until the browsing context reports it.
struct JsHtmlImport {
    JsHtmlRealm* realm;
    JSValue resolvingFuncs[2];
    PrString base;                  // the name of the script or module that imports
    PrString specifier;
    PrString url;                   // the URL that the specifier resolved to, the name of the graph's root
};

// A fetch() whose response the browsing context fetches. Its promise is settled once the response arrives, or once its
// signal aborts, which also cancels the fetch. The fetch outlives the engine, which frees its values, until the
// browsing context answers it.
struct JsHtmlFetch {
    JsHtmlRealm* realm;
    JSValue resolvingFuncs[2];
    JSValue signal;                 // the request's AbortSignal object, or undefined
    uint32_t abortAlgorithm;        // the id of the abort steps that it added to its signal, or zero
};

// A realm, and the browsing context whose scripts it runs. The engine's own realm has no browsing context or host. A
// realm whose browsing context is open when the engine is destroyed lives on without its context until it closes, since
// the browsing context still calls its host.
struct JsHtmlRealm {
    JsHtmlEngine* PR_NULLABLE engine;                   // null once the engine is destroyed
    PrHtmlHost host;                                    // the host that the application gave
    JSContext* PR_NULLABLE ctx;                         // null once the engine is destroyed
    PrHtmlPage* PR_NULLABLE page;                       // null until opened, and once closed
    Array<JsHtmlModule> modules;
    Array<JsHtmlClassicScript> classicScripts;          // the latest base of each name that a classic script ran as
    Array<JsHtmlImport*> imports;                       // the import() calls whose graphs are being fetched
    Array<JsHtmlFetch*> fetches;                        // the fetch() calls whose responses are being fetched
    Array<JSValue> evaluations;                         // the promises of module evaluations not yet settled
    uint32_t inlineModules;
    bool scripting;                                     // whether the browsing context and its iframes run scripts
    bool opened;
    bool closed;
};

// An agent that the engine's realms run on, with the number of them. The engine's checkpoint follows each of the
// agent's tasks while the number is above zero (prDomAgentAddCheckpoint).
struct JsHtmlAgent {
    PrDomAgent* agent;
    uint32_t realms;
};

struct JsHtmlEngine {
    Array<JsHtmlAgent> agents;
    Array<PrJsHtmlBind*> binds;
    PrJsHtmlEngineDesc desc;
    JSRuntime* runtime;
    JsHtmlRealm* own;
    Array<JsHtmlRealm*> realms;   // every realm, the engine's own first
};

namespace {

bool engineCheckpoint(JsHtmlEngine* engine);

JsHtmlRealm* realmOf(JSContext* ctx) {
    return static_cast<JsHtmlRealm*>(JS_GetContextOpaque(ctx));
}

// Removes the fetch's abort steps from its signal, and frees its values.
void releaseFetchValues(JSContext* ctx, JsHtmlFetch* fetch) {
    if (fetch->abortAlgorithm != 0)
        prDomAbortSignalRemove(jsHandleAs<PrDomAbortSignal>(ctx, fetch->signal, prJsDomAbortSignalClass),
            fetch->abortAlgorithm);
    fetch->abortAlgorithm = 0;
    JS_FreeValue(ctx, fetch->signal);
    JS_FreeValue(ctx, fetch->resolvingFuncs[0]);
    JS_FreeValue(ctx, fetch->resolvingFuncs[1]);
}

// Called once the browsing context has the response to a fetch(), or failed to fetch it. Resolves the fetch's promise
// with a Response, or rejects it with a TypeError for a network error. A fetch whose document is gone is dropped
// unsettled.
void fetched(void* PR_NULLABLE context, const PrFetchResponse* response) {
    JsHtmlFetch* fetch = static_cast<JsHtmlFetch*>(context);
    JsHtmlRealm* realm = fetch->realm;
    if (realm->engine == nullptr) {
        prMemoryFree(fetch);
        return;
    }
    realm->fetches.removeFirst(fetch);
    JSContext* ctx = realm->ctx;
    bool lost = response->status == PrStatus_Lost;
    if (!lost) {
        JSValue result = JS_UNDEFINED;
        if (response->status != PrStatus_Ok) {
            result = JS_ThrowTypeError(ctx, "Failed to fetch");
        } else {
            PrJsFetchResponseDesc desc{
                .type = response->type,
                .status = response->responseStatus,
                .url = response->url,
                .headers = response->headers,
                .headerCount = response->headerCount,
                .body = response->body,
                .signal = fetch->signal,
            };
            result = prJsFetchResponseNew(ctx, &desc);
        }
        bool rejected = JS_IsException(result);
        if (rejected)
            result = JS_GetException(ctx);
        JSValue settled = JS_Call(ctx, fetch->resolvingFuncs[rejected ? 1 : 0], JS_UNDEFINED, 1, &result);
        JS_FreeValue(ctx, settled);
        JS_FreeValue(ctx, result);
    }
    releaseFetchValues(ctx, fetch);
    prMemoryFree(fetch);
    if (!lost)
        engineCheckpoint(realm->engine);
}

// Rejects the promise of a fetch() with the reason of its signal's abort, and stops the fetch (fetch/#abort-fetch). The
// response that arrived before the abort has the signal, and errors its own body.
void abortFetch(void* PR_NULLABLE context, PrDomAbortSignal*) {
    JsHtmlFetch* fetch = static_cast<JsHtmlFetch*>(context);
    JsHtmlRealm* realm = fetch->realm;
    JSContext* ctx = realm->ctx;
    realm->fetches.removeFirst(fetch);
    if (realm->page != nullptr)
        prHtmlPageCancelFetch(realm->page, fetch);
    // The signal runs its abort steps once, and lets them go.
    fetch->abortAlgorithm = 0;
    JSValue reason = prJsDomAbortSignalReason(ctx, fetch->signal);
    JSValue settled = JS_Call(ctx, fetch->resolvingFuncs[1], JS_UNDEFINED, 1, &reason);
    JS_FreeValue(ctx, settled);
    JS_FreeValue(ctx, reason);
    releaseFetchValues(ctx, fetch);
    prMemoryFree(fetch);
}

// fetch/#dom-global-fetch: fetches through the realm's browsing context. A request that its arguments cannot make
// rejects the promise, as does a realm without a browsing context.
JSValue fetchMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JsHtmlRealm* realm = realmOf(ctx);
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise))
        return promise;
    PrJsFetchRequest request = {.signal = JS_UNDEFINED};
    bool ok = jsArgCount(ctx, argc, 1);
    if (ok && realm->page == nullptr) {
        JS_ThrowTypeError(ctx, "fetch() needs a window");
        ok = false;
    }
    if (ok) {
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(realm->page));
        PrString base = prDomNodeBaseUri(prDomDocumentAsNode(document));
        ok = prJsFetchRequestRead(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, prStringView(&base), &request);
        prStringReset(&base);
    }
    // A request whose signal is aborted already is not fetched, and rejects with the signal's reason.
    JSValue aborted = ok ? prJsDomAbortSignalReason(ctx, request.signal) : JS_UNDEFINED;
    if (!JS_IsUndefined(aborted)) {
        JS_Throw(ctx, aborted);
        ok = false;
    }
    if (ok) {
        JsHtmlFetch* fetch = allocZeroed<JsHtmlFetch>();
        *fetch = JsHtmlFetch{
            .realm = realm,
            .resolvingFuncs = {functions[0], functions[1]},
            .signal = JS_DupValue(ctx, request.signal),
        };
        PrFetchClientRequest desc{
            .url = request.url,
            .method = request.method,
            .headers = request.headers,
            .headerCount = request.headerCount,
            .body = request.body,
            .mode = request.mode,
            .credentials = request.credentials,
            .redirect = request.redirect,
        };
        if (prHtmlPageFetch(realm->page, &desc, fetched, fetch) == PrStatus_Ok) {
            realm->fetches.push(fetch);
            if (PrDomAbortSignal* signal = jsHandleAs<PrDomAbortSignal>(ctx, fetch->signal, prJsDomAbortSignalClass)) {
                PrDomAbortAlgorithm steps{.invoke = abortFetch, .context = fetch};
                fetch->abortAlgorithm = prDomAbortSignalAdd(signal, &steps);
            }
        } else {
            JS_FreeValue(ctx, fetch->signal);
            prMemoryFree(fetch);
            JS_ThrowTypeError(ctx, "Invalid URL");
            ok = false;
        }
    }
    prJsFetchRequestReset(&request);
    if (!ok) {
        JSValue error = JS_GetException(ctx);
        JSValue settled = JS_Call(ctx, functions[1], JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, settled);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, functions[0]);
        JS_FreeValue(ctx, functions[1]);
    }
    return promise;
}

// Defines fetch() on the global object, where a [Global] interface's operations are (webidl/#es-operations).
void bindFetch(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue function = JS_NewCFunction2(ctx, JsCallScoped<fetchMethod>::call, "fetch", 1, JS_CFUNC_generic, 0);
    JS_DefinePropertyValueStr(ctx, global, "fetch", function,
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx, global);
}

// Gives the realm a new context, with the engine's bindings.
void bindContext(JsHtmlRealm* realm) {
    JsHtmlEngine* engine = realm->engine;
    realm->ctx = JS_NewContext(engine->runtime);
    JS_SetContextOpaque(realm->ctx, realm);
    prJsDomBind(realm->ctx);
    prJsCssBind(realm->ctx);
    for (uint32_t i = 0; i < engine->binds.count; ++i)
        engine->binds.data[i](realm->ctx);
    prJsFetchBind(realm->ctx);
    jsHtmlBind(realm->ctx);
    bindFetch(realm->ctx);
}

JsHtmlRealm* createRealm(JsHtmlEngine* engine) {
    JsHtmlRealm* realm = allocZeroed<JsHtmlRealm>();
    realm->engine = engine;
    bindContext(realm);
    engine->realms.push(realm);
    return realm;
}

// Frees what the realm's scripts hold, and its context.
void freeContext(JsHtmlRealm* realm) {
    for (uint32_t i = 0; i < realm->modules.count; ++i) {
        JsHtmlModule& module = realm->modules.data[i];
        prStringReset(&module.name);
        prStringReset(&module.base);
        JS_FreeValue(realm->ctx, module.module);
        JS_FreeValue(realm->ctx, module.error);
        module.requests.reset();
    }
    realm->modules.reset();
    for (uint32_t i = 0; i < realm->classicScripts.count; ++i) {
        prStringReset(&realm->classicScripts.data[i].name);
        prStringReset(&realm->classicScripts.data[i].base);
    }
    realm->classicScripts.reset();
    for (uint32_t i = 0; i < realm->imports.count; ++i) {
        JsHtmlImport* import = realm->imports.data[i];
        JS_FreeValue(realm->ctx, import->resolvingFuncs[0]);
        JS_FreeValue(realm->ctx, import->resolvingFuncs[1]);
    }
    realm->imports.reset();
    for (uint32_t i = 0; i < realm->fetches.count; ++i)
        releaseFetchValues(realm->ctx, realm->fetches.data[i]);
    realm->fetches.reset();
    for (uint32_t i = 0; i < realm->evaluations.count; ++i)
        JS_FreeValue(realm->ctx, realm->evaluations.data[i]);
    realm->evaluations.reset();
    JS_FreeContext(realm->ctx);
    realm->ctx = nullptr;
}

// Frees the realm's scripts and its context, which the runtime's destruction requires.
void freeScripts(JsHtmlRealm* realm) {
    freeContext(realm);
    realm->engine = nullptr;
}

// Copies `text` into `out` with a NUL after it, as JS_Eval requires, behind `column - 1` spaces so that the engine's
// columns are the source's.
void evalText(PrStringView text, uint32_t column, TextBuffer* out) {
    for (uint32_t i = 1; i < column; ++i)
        textAppendChar(out, ' ');
    textAppend(out, text);
    textAppendChar(out, '\0');
}

// Returns the index of the module named `name`, or kArrayMissing.
uint32_t findModule(const JsHtmlRealm* realm, PrStringView name) {
    for (uint32_t i = 0; i < realm->modules.count; ++i) {
        if (prStringView(&realm->modules.data[i].name) == name)
            return i;
    }
    return kArrayMissing;
}

// Returns the index of the module named `name`, which is recorded unparsed if the realm has none by that name.
uint32_t recordModule(JsHtmlRealm* realm, PrStringView name) {
    uint32_t index = findModule(realm, name);
    if (index != kArrayMissing)
        return index;
    realm->modules.push(JsHtmlModule{prStringCopy(name), prStringCopy(name), JS_UNDEFINED, JS_UNDEFINED, {}});
    return realm->modules.count - 1;
}

// Resolves the module specifier `specifier` against `base` (html/#resolve-a-module-specifier), and writes the resolved
// URL, serialized, to `out`. A specifier that is not a URL resolves against the base if it starts with "/", "./" or
// "../". Returns false for any other specifier, which an import map would have to name.
bool resolveSpecifier(PrStringView specifier, PrStringView base, PrString* out) {
    PrUrl* parsed = nullptr;
    PrStatus status = PrStatus_InvalidData;
    if (stringStartsWith(specifier, "/") || stringStartsWith(specifier, "./") || stringStartsWith(specifier, "../")) {
        PrUrl* baseUrl = nullptr;
        status = prUrlCreate(base, nullptr, PrCharset_Utf8, &baseUrl);
        if (status == PrStatus_Ok)
            status = prUrlCreate(specifier, baseUrl, PrCharset_Utf8, &parsed);
        prUrlDestroy(baseUrl);
    } else {
        status = prUrlCreate(specifier, nullptr, PrCharset_Utf8, &parsed);
    }
    if (status == PrStatus_Ok)
        *out = prUrlSerialize(parsed, PrUrlSerialization_Whole);
    prUrlDestroy(parsed);
    return status == PrStatus_Ok;
}

// Returns the base URL of the script or module named `name`: a classic script's, which a script that ran records before
// anything it runs can import, or else a module's, or else the name itself. A classic script and a module that came
// from one URL share its name. The classic script's is taken first, so that the base does not depend on whether the
// module has been recorded yet.
PrStringView baseOf(const JsHtmlRealm* realm, PrStringView name) {
    for (uint32_t i = 0; i < realm->classicScripts.count; ++i) {
        if (prStringView(&realm->classicScripts.data[i].name) == name)
            return prStringView(&realm->classicScripts.data[i].base);
    }
    uint32_t module = findModule(realm, name);
    return module != kArrayMissing ? prStringView(&realm->modules.data[module].base) : name;
}

// Resolves the specifier `name` that the script or module `baseName` imports to the name of a module, which QuickJS
// frees. It resolves against the importer's base URL (baseOf).
char* PR_NULLABLE normalizeModule(JSContext* ctx, const char* baseName, const char* name, void*) {
    JsHtmlRealm* realm = realmOf(ctx);
    if (realm == nullptr || realm->page == nullptr) {
        JS_ThrowTypeError(ctx, "Module loading requires an HTML document");
        return nullptr;
    }
    PrString resolved = {};
    if (!resolveSpecifier(name, baseOf(realm, baseName), &resolved)) {
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier '%s' from '%s'", name, baseName);
        return nullptr;
    }
    PrStringView text = prStringView(&resolved);
    char* result = static_cast<char*>(js_malloc(ctx, text.size + 1));
    if (result != nullptr) {
        memCopy(result, text.data, text.size);
        result[text.size] = '\0';
    }
    prStringReset(&resolved);
    return result;
}

// Returns the module's imports resolved against its base URL, or false, with the TypeError thrown, if a specifier does
// not resolve.
bool resolveRequests(JsHtmlRealm* realm, JSModuleDef* definition, PrStringView base, Array<PrString>* out) {
    int count = JS_GetModuleRequestCount(definition);
    for (int i = 0; i < count; ++i) {
        JSAtom atom = JS_GetModuleRequest(realm->ctx, definition, i);
        const char* specifier = JS_AtomToCString(realm->ctx, atom);
        JS_FreeAtom(realm->ctx, atom);
        if (specifier == nullptr)
            return false;
        PrString resolved = {};
        bool ok = resolveSpecifier(specifier, base, &resolved);
        if (ok)
            out->push(resolved);
        else
            JS_ThrowTypeError(realm->ctx, "Failed to resolve module specifier '%s'", specifier);
        JS_FreeCString(realm->ctx, specifier);
        if (!ok)
            return false;
    }
    return true;
}

// The host's parseModule. Compiles the module without loading what it imports, records the modules that it imports, and
// reports each to the browsing context, which fetches the graph before the module is linked. A module that the realm
// already parsed by the same name is not parsed again. An error is kept with the module, for the execution or the
// import() that reaches it.
uint32_t parseModule(void* context, PrHtmlPage*, const PrHtmlScript* script, PrHtmlModuleRequestFn request,
        void* PR_NULLABLE requestContext) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    if (realm->engine == nullptr)
        return 0;
    JSContext* ctx = realm->ctx;
    TextBuffer name;
    if (!script->external) {
        textAppend(&name, "[inline ");
        textAppendU64(&name, ++realm->inlineModules);
        textAppend(&name, "] ");
    }
    textAppend(&name, script->url);
    uint32_t index = recordModule(realm, textView(&name));
    JsHtmlModule* module = &realm->modules.data[index];
    if (JS_IsUndefined(module->module) && JS_IsUndefined(module->error)) {
        prStringReplace(&module->base, script->baseUrl);
        TextBuffer text;
        evalText(script->source, script->column, &text);
        textAppendChar(&name, '\0');
        JSEvalOptions options{
            JS_EVAL_OPTIONS_VERSION,
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE,
            textView(&name).data, int(script->line),
        };
        PrStringView eval = textView(&text);
        JSValue compiled = JS_Eval2(ctx, eval.data, eval.size - 1, &options);
        textReset(&text);
        Array<PrString> requests;
        bool parsed = !JS_IsException(compiled);
        if (parsed) {
            JSModuleDef* definition = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
            JSValue meta = JS_GetImportMeta(ctx, definition);
            parsed = !JS_IsException(meta) &&
                JS_DefinePropertyValueStr(ctx, meta, "url", jsNewStringView(ctx, script->baseUrl),
                    JS_PROP_C_W_E) >= 0 && resolveRequests(realm, definition, script->baseUrl, &requests);
            JS_FreeValue(ctx, meta);
        }
        // Recording an import may move the modules.
        for (uint32_t i = 0; parsed && i < requests.count; ++i) {
            uint32_t imported = recordModule(realm, prStringView(&requests.data[i]));
            realm->modules.data[index].requests.push(imported);
        }
        for (uint32_t i = 0; i < requests.count; ++i)
            prStringReset(&requests.data[i]);
        requests.reset();
        module = &realm->modules.data[index];
        module->module = JS_IsException(compiled) ? JS_UNDEFINED : compiled;
        if (!parsed)
            module->error = JS_GetException(ctx);
    }
    textReset(&name);
    for (uint32_t i = 0; i < realm->modules.data[index].requests.count; ++i) {
        uint32_t imported = realm->modules.data[index].requests.data[i];
        request(requestContext, prStringView(&realm->modules.data[imported].name));
    }
    return index;
}

// Returns the first parse error of the graph of the module at `index`, with a reference, or undefined if it has none
// (html/#concept-script-parse-error).
JSValue firstParseError(JsHtmlRealm* realm, uint32_t index) {
    Array<bool> reached;
    reached.resize(realm->modules.count);
    for (uint32_t i = 0; i < reached.count; ++i)
        reached.data[i] = false;
    Array<uint32_t> pending;
    pending.push(index);
    reached.data[index] = true;
    JSValue error = JS_UNDEFINED;
    for (uint32_t next = 0; next < pending.count && JS_IsUndefined(error); ++next) {
        const JsHtmlModule& module = realm->modules.data[pending.data[next]];
        if (!JS_IsUndefined(module.error)) {
            error = JS_DupValue(realm->ctx, module.error);
            continue;
        }
        for (uint32_t i = 0; i < module.requests.count; ++i) {
            uint32_t imported = module.requests.data[i];
            if (!reached.data[imported]) {
                reached.data[imported] = true;
                pending.push(imported);
            }
        }
    }
    reached.reset();
    pending.reset();
    return error;
}

// Links and evaluates the module at `index`, whose graph the browsing context fetched, and returns the promise of its
// evaluation. Throws the graph's first parse error instead if it has one. Every module of a graph without one is
// compiled, so linking finds each in the realm and loads nothing.
JSValue linkAndEvaluate(JsHtmlRealm* realm, uint32_t index) {
    JSValue error = firstParseError(realm, index);
    if (!JS_IsUndefined(error))
        return JS_Throw(realm->ctx, error);
    JSValue module = JS_DupValue(realm->ctx, realm->modules.data[index].module);
    jsEnterRealm(realm->ctx);
    JSValue evaluation = JS_ResolveModule(realm->ctx, module) < 0 ? JS_EXCEPTION : JS_EvalFunction(realm->ctx, module);
    jsLeaveRealm(realm->ctx);
    return evaluation;
}

// QuickJS's loader, which links a module to the modules that it imports. The browsing context fetched and parsed each
// before the graph is linked, so a module that the realm lacks is one that was not fetched.
JSModuleDef* PR_NULLABLE loadModule(JSContext* ctx, const char* name, void*) {
    JS_ThrowTypeError(ctx, "Module '%s' was not fetched", name);
    return nullptr;
}

// Copies `text` into `out` with a NUL after it.
const char* cStringOf(PrStringView text, TextBuffer* out) {
    textAppend(out, text);
    textAppendChar(out, '\0');
    return textView(out).data;
}

void freeImport(JsHtmlImport* import) {
    prStringReset(&import->base);
    prStringReset(&import->specifier);
    prStringReset(&import->url);
    prMemoryFree(import);
}

// Called once the browsing context has fetched an import()'s graph, or failed to. Settles the import's promise with the
// module's namespace once the graph is linked and evaluated, or with the error that stopped it. An import whose
// document is gone is dropped unsettled.
void imported(void* PR_NULLABLE context, PrStatus status) {
    JsHtmlImport* import = static_cast<JsHtmlImport*>(context);
    JsHtmlRealm* realm = import->realm;
    if (realm->engine == nullptr) {
        freeImport(import);
        return;
    }
    realm->imports.removeFirst(import);
    JSContext* ctx = realm->ctx;
    if (status == PrStatus_Lost) {
        JS_FreeValue(ctx, import->resolvingFuncs[0]);
        JS_FreeValue(ctx, import->resolvingFuncs[1]);
        freeImport(import);
        return;
    }
    TextBuffer url;
    cStringOf(prStringView(&import->url), &url);
    JSValue result = JS_UNDEFINED;
    if (status != PrStatus_Ok) {
        result = JS_ThrowTypeError(ctx, "Failed to fetch module '%s'", textView(&url).data);
    } else {
        JSValue error = firstParseError(realm, findModule(realm, prStringView(&import->url)));
        if (!JS_IsUndefined(error)) {
            result = JS_Throw(ctx, error);
        } else {
            TextBuffer base;
            TextBuffer specifier;
            jsEnterRealm(ctx);
            result = JS_LoadModule(ctx, cStringOf(prStringView(&import->base), &base),
                cStringOf(prStringView(&import->specifier), &specifier));
            jsLeaveRealm(ctx);
            textReset(&base);
            textReset(&specifier);
        }
    }
    textReset(&url);
    bool rejected = JS_IsException(result);
    if (rejected)
        result = JS_GetException(ctx);
    JSValue settled = JS_Call(ctx, import->resolvingFuncs[rejected ? 1 : 0], JS_UNDEFINED, 1, &result);
    JS_FreeValue(ctx, settled);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, import->resolvingFuncs[0]);
    JS_FreeValue(ctx, import->resolvingFuncs[1]);
    freeImport(import);
    engineCheckpoint(realm->engine);
}

// QuickJS's import(), which asks the browsing context to fetch the graph of the module that `specifier` names
// (html/#hostloadimportedmodule). A specifier that does not resolve rejects the import's promise.
void importModule(JSContext* ctx, const char* baseName, const char* specifier, JSValueConst* resolvingFuncs, void*) {
    JsHtmlRealm* realm = realmOf(ctx);
    char* url = normalizeModule(ctx, baseName, specifier, nullptr);
    if (url == nullptr) {
        JSValue error = JS_GetException(ctx);
        JSValue settled = JS_Call(ctx, resolvingFuncs[1], JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, settled);
        JS_FreeValue(ctx, error);
        return;
    }
    JsHtmlImport* import = allocZeroed<JsHtmlImport>();
    *import = JsHtmlImport{
        .realm = realm,
        .resolvingFuncs = {JS_DupValue(ctx, resolvingFuncs[0]), JS_DupValue(ctx, resolvingFuncs[1])},
        .base = prStringCopy(baseName),
        .specifier = prStringCopy(specifier),
        .url = prStringCopy(url),
    };
    js_free(ctx, url);
    realm->imports.push(import);
    prHtmlPageImportModule(realm->page, prStringView(&import->url), imported, import);
}

// The members of the host that prJsHtmlEngineHost returns. Each is called with the realm.

// Records the base URL of the classic script named `name`, replacing the base that an earlier script of that name had.
void recordClassicScript(JsHtmlRealm* realm, PrStringView name, PrStringView base) {
    for (uint32_t i = 0; i < realm->classicScripts.count; ++i) {
        if (prStringView(&realm->classicScripts.data[i].name) == name) {
            prStringReplace(&realm->classicScripts.data[i].base, base);
            return;
        }
    }
    realm->classicScripts.push(JsHtmlClassicScript{prStringCopy(name), prStringCopy(base)});
}

PrStatus execute(void* context, PrHtmlPage*, const PrHtmlScript* script) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    if (realm->engine == nullptr)
        return PrStatus_Ok;
    if (script->kind == PrHtmlScriptKind_Module) {
        JSValue evaluation = linkAndEvaluate(realm, script->module);
        if (JS_IsException(evaluation))
            jsReportException(realm->ctx);
        else
            realm->evaluations.push(evaluation);
    } else {
        recordClassicScript(realm, script->url, script->baseUrl);
        // An author's exception is reported to the window. It does not abort parsing.
        prJsHtmlEngineRun(realm->ctx, script->source, script->url, script->line, script->column);
    }
    engineCheckpoint(realm->engine);
    return PrStatus_Ok;
}

// Waits for the evaluations of the realm's module scripts to settle, running the agent's tasks and timers that they
// wait on, then runs the host's own settle.
PrStatus settle(void* context, PrHtmlPage* page) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    PrDomAgent* agent = prHtmlWindowAgent(prHtmlPageWindow(page));
    while (realm->engine != nullptr) {
        engineCheckpoint(realm->engine);
        uint32_t pending = 0;
        for (uint32_t i = 0; i < realm->evaluations.count; ++i) {
            JSValue promise = realm->evaluations.data[i];
            JSPromiseStateEnum state = JS_PromiseState(realm->ctx, promise);
            if (state == JS_PROMISE_PENDING) {
                realm->evaluations.data[pending++] = promise;
                continue;
            }
            if (state == JS_PROMISE_REJECTED) {
                JS_Throw(realm->ctx, JS_PromiseResult(realm->ctx, promise));
                jsReportException(realm->ctx);
            }
            JS_FreeValue(realm->ctx, promise);
        }
        realm->evaluations.count = pending;
        if (pending == 0)
            break;
        // An evaluation may wait for an import()'s graph, whose fetches the network posts to the agent.
        double delay = prDomAgentNextTaskDelay(agent);
        bool fetching = prHtmlPageIsFetching(page);
        if (delay < 0 && !fetching) {
            JS_ThrowInternalError(realm->ctx, "module evaluation is pending without a runnable job, timer or fetch");
            jsReportException(realm->ctx);
            return PrStatus_InvalidState;
        }
        if (delay != 0) {
            const PrJsHtmlEngineDesc& desc = realm->engine->desc;
            if (desc.wait != nullptr)
                desc.wait(desc.context, delay);
            else
                prThreadSleep(fetching ? 1 : delay);
        }
        // A load that ends in a task, as a streamed load does, waits for its modules as a task that spins the event
        // loop waits.
        prDomAgentSpinTask(agent);
    }
    return realm->host.settle != nullptr ? realm->host.settle(realm->host.context, page) : PrStatus_Ok;
}

bool compile(void* context, PrHtmlPage*, PrDomEventTarget* target, PrDomElement* element,
        PrDomEventHandler handler, PrStringView body, PrDomEventHandlerCallback* out) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    return realm->engine != nullptr && jsHtmlCompileEventHandler(realm->ctx, target, element, handler, body, out);
}

void microtask(void* context, PrHtmlPage*, void (*run)(void* data), void* data) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    if (realm->engine != nullptr)
        jsEnqueueNative(realm->ctx, run, data);
}

PrHtmlHost engineHost(JsHtmlEngine* engine, const PrHtmlHost* host, bool scripting);

void frame(void* context, PrHtmlPage* parent, PrHtmlHost* child) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    realm->host.frame(realm->host.context, parent, child);
    if (realm->engine != nullptr)
        *child = engineHost(realm->engine, child, realm->scripting);
}

// The engine's checkpoint, after each task of an agent that its realms run on.
void checkpointHook(void* PR_NULLABLE context) {
    engineCheckpoint(static_cast<JsHtmlEngine*>(context));
}

// Counts a realm that runs on `agent`, and adds the engine's checkpoint to the agent for its first realm.
void addAgent(JsHtmlEngine* engine, PrDomAgent* agent) {
    for (uint32_t i = 0; i < engine->agents.count; ++i) {
        if (engine->agents.data[i].agent == agent) {
            ++engine->agents.data[i].realms;
            return;
        }
    }
    engine->agents.push(JsHtmlAgent{agent, 1});
    prDomAgentAddCheckpoint(agent, checkpointHook, engine);
}

// Stops counting a realm that ran on `agent`, and removes the engine's checkpoint from the agent with its last realm.
void removeAgent(JsHtmlEngine* engine, PrDomAgent* agent) {
    for (uint32_t i = 0; i < engine->agents.count; ++i) {
        JsHtmlAgent& counted = engine->agents.data[i];
        if (counted.agent != agent)
            continue;
        if (--counted.realms == 0) {
            prDomAgentRemoveCheckpoint(agent, checkpointHook, engine);
            engine->agents.erase(i);
        }
        return;
    }
}

// A window of the realm's page opens. The page's first window takes the realm's context, and each window after it a new
// context, since each window is the global of a realm of its own.
void windowOpened(void* context, PrHtmlPage* page) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    realm->opened = true;
    if (realm->engine == nullptr) {
        if (realm->host.windowOpened != nullptr)
            realm->host.windowOpened(realm->host.context, page);
        return;
    }
    if (realm->page != nullptr) {
        freeContext(realm);
        bindContext(realm);
    } else {
        addAgent(realm->engine, prHtmlPageAgent(page));
    }
    realm->page = page;
    if (!jsHtmlSetGlobal(realm->ctx, prHtmlPageWindow(page)))
        jsReportException(realm->ctx);
    const PrJsHtmlEngineDesc& desc = realm->engine->desc;
    if (desc.realmOpened != nullptr)
        desc.realmOpened(desc.context, realm->ctx, page);
    if (realm->host.windowOpened != nullptr)
        realm->host.windowOpened(realm->host.context, page);
}

void windowClosed(void* context, PrHtmlPage* page) {
    const PrHtmlHost& host = static_cast<JsHtmlRealm*>(context)->host;
    if (host.windowClosed != nullptr)
        host.windowClosed(host.context, page);
}

void closed(void* context, PrHtmlPage* page) {
    JsHtmlRealm* realm = static_cast<JsHtmlRealm*>(context);
    if (realm->engine != nullptr && realm->page != nullptr)
        removeAgent(realm->engine, prHtmlPageAgent(page));
    realm->page = nullptr;
    realm->closed = true;
    PrHtmlHost host = realm->host;
    if (realm->engine == nullptr)
        prMemoryFree(realm);
    if (host.closed != nullptr)
        host.closed(host.context, page);
}

// The host's members that the engine passes on, with the host's own context.

PrDomHostState elementState(void* context, PrHtmlPage* page, const PrDomElement* element, PrDomHostState wanted) {
    const PrHtmlHost& host = static_cast<JsHtmlRealm*>(context)->host;
    return host.elementState(host.context, page, element, wanted);
}

void loaded(void* context, PrHtmlPage* page, PrStatus status) {
    const PrHtmlHost& host = static_cast<JsHtmlRealm*>(context)->host;
    host.loaded(host.context, page, status);
}

double now(void* context) {
    const PrHtmlHost& host = static_cast<JsHtmlRealm*>(context)->host;
    return host.now(host.context);
}

void rendered(void* context, PrHtmlPage* page) {
    const PrHtmlHost& host = static_cast<JsHtmlRealm*>(context)->host;
    host.rendered(host.context, page);
}

// Returns `host` with the engine running its browsing context's scripts (prJsHtmlEngineHost).
PrHtmlHost engineHost(JsHtmlEngine* engine, const PrHtmlHost* host, bool scripting) {
    JsHtmlRealm* realm = createRealm(engine);
    realm->host = *host;
    realm->scripting = scripting;
    PrHtmlHost result = *host;
    result.context = realm;
    result.execute = realm->scripting ? execute : nullptr;
    result.parseModule = realm->scripting ? parseModule : nullptr;
    result.settle = settle;
    result.compile = compile;
    result.microtask = microtask;
    result.frame = host->frame != nullptr ? frame : nullptr;
    result.windowOpened = windowOpened;
    result.windowClosed = windowClosed;
    result.closed = closed;
    result.loaded = host->loaded != nullptr ? loaded : nullptr;
    result.elementState = host->elementState != nullptr ? elementState : nullptr;
    result.now = host->now != nullptr ? now : nullptr;
    result.rendered = host->rendered != nullptr ? rendered : nullptr;
    return result;
}

// Performs a microtask checkpoint across the engine's realms (prJsHtmlEngineCheckpoint).
bool engineCheckpoint(JsHtmlEngine* engine) {
    bool ok = true;
    for (;;) {
        JSContext* pending = nullptr;
        int status = JS_ExecutePendingJob(engine->runtime, &pending);
        if (status < 0) {
            jsReportException(pending);
            ok = false;
        }
        if (status != 0)
            continue;
        bool notified = false;
        for (uint32_t i = 0; i < engine->realms.count; ++i) {
            if (engine->realms.data[i]->page != nullptr)
                notified |= jsHtmlNotifyRejectedPromises(engine->realms.data[i]->ctx);
        }
        if (!notified)
            return ok;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrJsHtmlEngine : JsHtmlEngine {};

PrJsHtmlEngine* prJsHtmlEngineCreate(const PrJsHtmlEngineDesc* desc) {
    PrJsHtmlEngine* engine = allocZeroed<PrJsHtmlEngine>();
    engine->desc = *desc;
    for (uint32_t i = 0; i < desc->bindCount; ++i)
        engine->binds.push(desc->binds[i]);
    engine->desc.binds = nullptr;
    engine->desc.bindCount = 0;
    engine->runtime = jsRuntimeCreate();
    prJsHtmlConsoleSetPrinter(engine->runtime, desc->console);
    JS_SetModuleLoaderFunc(engine->runtime, normalizeModule, loadModule, nullptr);
    JS_SetModuleImportFunc(engine->runtime, importModule);
    engine->own = createRealm(engine);
    JSValue global = JS_GetGlobalObject(engine->own->ctx);
    JS_SetPropertyStr(engine->own->ctx, global, "self", JS_DupValue(engine->own->ctx, global));
    JS_FreeValue(engine->own->ctx, global);
    if (desc->realmOpened != nullptr)
        desc->realmOpened(desc->context, engine->own->ctx, nullptr);
    return engine;
}

void prJsHtmlEngineAttachThread(PrJsHtmlEngine* engine) {
    JS_UpdateStackTop(engine->runtime);
}

void prJsHtmlEngineDestroy(PrJsHtmlEngine* PR_NULLABLE engine) {
    if (engine == nullptr)
        return;
    for (uint32_t i = 0; i < engine->agents.count; ++i)
        prDomAgentRemoveCheckpoint(engine->agents.data[i].agent, checkpointHook, engine);
    engine->agents.reset();
    for (uint32_t i = 0; i < engine->realms.count; ++i)
        freeScripts(engine->realms.data[i]);
    jsRuntimeDestroy(engine->runtime);
    // A realm whose browsing context is open is freed when the context closes.
    for (uint32_t i = 0; i < engine->realms.count; ++i) {
        JsHtmlRealm* realm = engine->realms.data[i];
        if (!realm->opened || realm->closed || realm == engine->own)
            prMemoryFree(realm);
    }
    engine->realms.reset();
    engine->binds.reset();
    prMemoryFree(engine);
}

PrHtmlHost prJsHtmlEngineHost(PrJsHtmlEngine* engine, const PrHtmlHost* host, PrJsHtmlScripting scripting) {
    return engineHost(engine, host, scripting == PrJsHtmlScripting_Enabled);
}

bool prJsHtmlEngineCheckpoint(PrJsHtmlEngine* engine) {
    return engineCheckpoint(engine);
}

JSContext* prJsHtmlEngineContext(PrJsHtmlEngine* engine) {
    return engine->own->ctx;
}

JSContext* PR_NULLABLE prJsHtmlEngineRealm(PrJsHtmlEngine* engine, PrHtmlPage* page) {
    for (uint32_t i = 0; i < engine->realms.count; ++i) {
        if (engine->realms.data[i]->page == page)
            return engine->realms.data[i]->ctx;
    }
    return nullptr;
}

void* PR_NULLABLE prJsHtmlEngineHostContext(JSContext* ctx) {
    JsHtmlRealm* realm = realmOf(ctx);
    return realm != nullptr && realm != realm->engine->own ? realm->host.context : nullptr;
}

bool prJsHtmlEngineRun(JSContext* ctx, PrStringView source, PrStringView url, uint32_t line, uint32_t column) {
    TextBuffer text;
    evalText(source, column, &text);
    TextBuffer name;
    textAppend(&name, url);
    textAppendChar(&name, '\0');
    PrStringView eval = textView(&text);
    JSEvalOptions options{JS_EVAL_OPTIONS_VERSION, JS_EVAL_TYPE_GLOBAL, textView(&name).data, int(line)};
    jsEnterRealm(ctx);
    JSValue result = JS_Eval2(ctx, eval.data, eval.size - 1, &options);
    jsLeaveRealm(ctx);
    textReset(&text);
    textReset(&name);
    bool ok = !JS_IsException(result);
    if (!ok)
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    return ok;
}
