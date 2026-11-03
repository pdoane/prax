// JavaScript binding of HTML.

#pragma once

#include "pr/html.h"
#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Binds the classes of HTML, the custom elements and the console, and adds HTML's members to the classes of other
// modules. The bindings of those modules must be bound first.
void jsHtmlBind(JSContext* ctx);

// The class of HTMLElement, which classes of other modules derive from.
extern JSClassID htmlElementClass;

// Returns the window of the context's global, or null if it has none.
PrHtmlWindow* PR_NULLABLE jsHtmlGlobalWindow(JSContext* ctx);

// Makes `handle` the window of the context's global, with the window's custom element registry. Returns false, with an
// exception pending, if the runtime already runs the windows of another agent. The host owns the window, and must keep
// it alive until the runtime is destroyed. The runtime's finalizers access the window.
bool jsHtmlSetGlobal(JSContext* ctx, PrHtmlWindow* handle);

// html/#notify-about-rejected-promises for the global's window. Fires a PromiseRejectionEvent at the window for each
// promise that was rejected without a handler since the previous call, and reports each rejection that no listener
// canceled. Returns true if any event fired, since the listeners may have queued jobs. A host calls this at the end of
// each microtask checkpoint.
bool jsHtmlNotifyRejectedPromises(JSContext* ctx);

// Compiles `body` into the callback of the event handler `handler` of `target`
// (html/#getting-the-current-value-of-the-event-handler). `body` is the text of the event handler content attribute of
// `element`. Writes the callback to `out` with one reference. If `body` does not compile, reports the error and returns
// false. A host's compile callback (PrHtmlHost) returns this result.
bool jsHtmlCompileEventHandler(JSContext* ctx, PrDomEventTarget* target, PrDomElement* element,
    PrDomEventHandler handler, PrStringView body, PrDomEventHandlerCallback* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
