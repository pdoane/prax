// The scripts of HTML documents, run by QuickJS.
//
// html - https://html.spec.whatwg.org/multipage/webappapis.html

#pragma once

#include "pr/html_console_quickjs.h"
#include "pr/html_page.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines a module's classes in a realm, as prJsSvgBind, prJsMathMlBind and prJsUrlBind do.
typedef void PrJsHtmlBind(JSContext* ctx);

// What an engine runs scripts with. The callbacks receive `context`.
typedef struct PrJsHtmlEngineDesc {
    // The bindings of the modules beyond DOM and HTML that scripts see, defined in each realm in this order. DOM's
    // bindings are defined before them, and HTML's after.
    PrJsHtmlBind* PR_NONNULL const* PR_NULLABLE binds;
    uint32_t bindCount;

    // Where the console prints. Null prints nothing.
    const PrJsHtmlConsolePrinter* PR_NULLABLE console;

    void* PR_NULLABLE context;

    // Called once for each realm, with its bindings defined, to add what the application gives scripts. A browsing
    // context's realm is opened once its global is the browsing context's window, before its document loads. The
    // engine's own realm (prJsHtmlEngineContext) is opened with a null browsing context.
    void (*PR_NULLABLE realmOpened)(void* PR_NULLABLE context, JSContext* ctx, PrHtmlPage* PR_NULLABLE page);

    // Waits for what a module's evaluation waits on before its document can finish loading: `delay` milliseconds for a
    // timer, or, while the browsing context fetches, until a task is posted to the agent (prDomAgentPost). The delay is
    // negative if no timer is due. Null sleeps for a timer, and polls for a fetch every millisecond.
    void (*PR_NULLABLE wait)(void* PR_NULLABLE context, double delay);
} PrJsHtmlEngineDesc;

// Whether a browsing context, and each of its iframes, runs scripts.
enum {
    PrJsHtmlScripting_Enabled,
    PrJsHtmlScripting_Disabled,
};
typedef uint8_t PrJsHtmlScripting;

// One runtime, with a realm for each browsing context that it runs scripts for.
typedef struct PrJsHtmlEngine PrJsHtmlEngine;

// Creates an engine. The runtime runs the browsing contexts of one agent.
PrJsHtmlEngine* prJsHtmlEngineCreate(const PrJsHtmlEngineDesc* desc);

// Measures the engine's stack limit from the calling thread, which runs scripts from now on. A host that creates the
// engine on another thread calls this first. The limit is an address on one thread's stack, and a script's deep
// recursion would otherwise overflow the stack instead of throwing a RangeError.
void prJsHtmlEngineAttachThread(PrJsHtmlEngine* engine);

// Destroys the engine's realms and runtime. The browsing contexts that it runs scripts for are destroyed after it,
// since its objects' finalizers reach their windows. Their hosts run no scripts once the engine is gone.
void prJsHtmlEngineDestroy(PrJsHtmlEngine* PR_NULLABLE engine);

// Returns `host` with the engine running its browsing context's scripts in a realm of their own. The result's execute,
// parseModule, compile and microtask run scripts, and its browsing context fetches the modules that they import. Its
// settle waits for module scripts, its opened sets the realm's global, and its frame gives an iframe's browsing context
// a realm of its own if `host` has a frame; each then calls `host`'s own. The result's other members are `host`'s. If
// `scripting` is Disabled, the result's execute is null, which disables scripting, and so is that of each iframe's
// host.
PrHtmlHost prJsHtmlEngineHost(PrJsHtmlEngine* engine, const PrHtmlHost* host, PrJsHtmlScripting scripting);

// Performs a microtask checkpoint (html/#perform-a-microtask-checkpoint): runs the queued jobs of every realm, then
// notifies each window of the rejections that the jobs left unhandled, whose listeners may queue more jobs. Each
// exception is reported. Returns false if a job threw outside any window.
bool prJsHtmlEngineCheckpoint(PrJsHtmlEngine* engine);

// Returns the engine's own realm, which has the bindings and no window, for scripts that the application runs outside
// any document.
JSContext* prJsHtmlEngineContext(PrJsHtmlEngine* engine);

// Reads `serialized`, a message's data as a script of the engine's serialized it (prHtmlMessageEventSerializedData),
// and writes the value as JSON to `out` (JSON.stringify), which the caller releases with prRawStringReset. This is how
// a host that runs no script of its own reads what a document's scripts post to it. Returns false, and writes the null
// string, if the bytes do not deserialize, a port or a value with none has no JSON (as undefined has none), or the
// serialization throws.
bool prJsHtmlEngineMessageJson(PrJsHtmlEngine* engine, PrStringView serialized, PrRawString* out);

// Returns the realm of a browsing context, or null if the engine does not run its scripts.
JSContext* PR_NULLABLE prJsHtmlEngineRealm(PrJsHtmlEngine* engine, PrHtmlPage* page);

// Returns the context of the host that the realm's browsing context was created with (prJsHtmlEngineHost), or null for
// the engine's own realm.
void* PR_NULLABLE prJsHtmlEngineHostContext(JSContext* ctx);

// Runs `source` in `ctx` as a classic script from `url`, whose text starts at `line` and `column`, counting from one.
// Reports an exception it throws. Returns false if it threw.
bool prJsHtmlEngineRun(JSContext* ctx, PrStringView source, PrStringView url, uint32_t line, uint32_t column);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
