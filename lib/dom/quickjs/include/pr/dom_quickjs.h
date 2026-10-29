// The QuickJS layer of the Document Object Model
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines the classes of this layer on the global object of `ctx`: Web IDL's, geometry's, the DOM's and the CSSOM's.
// Adds the classifiers that choose an object's most specific class.
void prJsDomBind(JSContext* ctx);

// The classes of the module. A module that defines classes derived from them refers to these.
extern JSClassID prJsDomEventClass;
extern JSClassID prJsDomCustomEventClass;
extern JSClassID prJsDomUiEventClass;
extern JSClassID prJsDomFocusEventClass;
extern JSClassID prJsDomMouseEventClass;
extern JSClassID prJsDomWheelEventClass;
extern JSClassID prJsDomKeyboardEventClass;
extern JSClassID prJsDomCompositionEventClass;
extern JSClassID prJsDomEventTargetClass;
extern JSClassID prJsDomAbortSignalClass;
extern JSClassID prJsDomNodeClass;
extern JSClassID prJsDomDocumentClass;
extern JSClassID prJsDomDocumentFragmentClass;
extern JSClassID prJsDomShadowRootClass;
extern JSClassID prJsDomElementClass;
extern JSClassID prJsDomCharacterDataClass;
extern JSClassID prJsDomTextClass;
extern JSClassID prJsDomCommentClass;
extern JSClassID prJsDomProcessingInstructionClass;
extern JSClassID prJsDomHtmlCollectionClass;
extern JSClassID prJsDomNodeListClass;
extern JSClassID prJsDomTokenListClass;
extern JSClassID prJsDomAbstractRangeClass;
extern JSClassID prJsDomRangeClass;
extern JSClassID prJsDomSelectionClass;

// How the runtime finds a global object's associated Document (html/#concept-document-window). Node constructors create
// their nodes in that document.
typedef struct PrJsDomGlobalDocument {
    PrDomDocument* PR_NULLABLE (*PR_NONNULL document)(JSContext* ctx);
} PrJsDomGlobalDocument;

// Sets how the runtime finds its global's associated Document. A module whose global has an associated Document calls
// this.
void prJsDomSetGlobalDocument(JSRuntime* runtime, const PrJsDomGlobalDocument* source);

// Sets the agent whose event loop runs the runtime's scripts, or clears it if `agent` is null. The [CEReactions]
// members push onto that agent's reactions stack (html/#cereactions). A runtime runs one agent.
void prJsDomSetAgent(JSRuntime* runtime, PrDomAgent* PR_NULLABLE agent);

// Returns the agent that prJsDomSetAgent set, or null if there is none.
PrDomAgent* PR_NULLABLE prJsDomAgent(JSRuntime* runtime);

// Returns the global object's associated Document, or null with a TypeError thrown if it has none.
PrDomDocument* PR_NULLABLE prJsDomGlobalDocument(JSContext* ctx);

// Returns the reason of the abort of the AbortSignal object `signal`, or undefined if it is not aborted.
JSValue prJsDomAbortSignalReason(JSContext* ctx, JSValueConst signal);

// Returns `target` as a JS value. This is the global object if `target` belongs to the global, and otherwise the
// target's object, which keeps `owner` alive.
JSValue prJsDomWrapEventTarget(JSContext* ctx, JSValueConst owner,
                               PrDomEventTarget* PR_NULLABLE target);

// Returns the object of `document`, which has no owner.
JSValue prJsDomWrapDocument(JSContext* ctx, PrDomDocument* document);

// Returns the object of `range`, which keeps `owner` alive. The object is a Range if the range is live, and a
// StaticRange otherwise. The caller's reference passes to the object, or is released.
JSValue prJsDomWrapRange(JSContext* ctx, JSValueConst owner, PrDomRange* range);

// Reads the members of a present EventInit dictionary (dom/#dictdef-eventinit). A dictionary that inherits from it
// reads these before its own.
bool prJsDomEventInitRead(JSContext* ctx, JSValueConst dictionary, PrDomEventInit* out);

// Calls the listener whose callback is `context` (jsCallbackCreate) with the event, with the event's current target as
// the receiver. It is the function of every listener that script adds, by addEventListener or by a binding's own member
// such as addListener, so that a callback added either way is the same listener.
void prJsDomInvokeListener(void* PR_NULLABLE context, PrDomEvent* event);

// The event handlers of the GlobalEventHandlers mixin (html/#globaleventhandlers), including those that other specs add
// to it.
#define PR_JS_DOM_GLOBAL_EVENT_HANDLERS(X) \
    X(onabort) X(onanimationcancel) X(onanimationend) X(onanimationiteration) X(onanimationstart) \
    X(onauxclick) X(onbeforeinput) X(onbeforematch) X(onbeforetoggle) X(onblur) X(oncancel) \
    X(oncanplay) X(oncanplaythrough) X(onchange) X(onclick) X(onclose) X(oncommand) \
    X(oncontextlost) X(oncontextmenu) X(oncontextrestored) X(oncopy) X(oncuechange) X(oncut) \
    X(ondblclick) X(ondrag) X(ondragend) X(ondragenter) X(ondragleave) X(ondragover) \
    X(ondragstart) X(ondrop) X(ondurationchange) X(onemptied) X(onended) X(onerror) X(onfocus) \
    X(onformdata) X(ongotpointercapture) X(oninput) X(oninvalid) X(onkeydown) X(onkeypress) \
    X(onkeyup) X(onload) X(onloadeddata) X(onloadedmetadata) X(onloadstart) \
    X(onlostpointercapture) X(onmousedown) X(onmouseenter) X(onmouseleave) X(onmousemove) \
    X(onmouseout) X(onmouseover) X(onmouseup) X(onpaste) X(onpause) X(onplay) X(onplaying) \
    X(onpointercancel) X(onpointerdown) X(onpointerenter) X(onpointerleave) X(onpointermove) \
    X(onpointerout) X(onpointerover) X(onpointerrawupdate) X(onpointerup) X(onprogress) \
    X(onratechange) X(onreset) X(onresize) X(onscroll) X(onscrollend) X(onsecuritypolicyviolation) \
    X(onseeked) X(onseeking) X(onselect) X(onselectionchange) X(onselectstart) X(onslotchange) \
    X(onstalled) X(onsubmit) X(onsuspend) X(ontimeupdate) X(ontoggle) X(ontransitioncancel) \
    X(ontransitionend) X(ontransitionrun) X(ontransitionstart) X(onvolumechange) X(onwaiting) \
    X(onwebkitanimationend) X(onwebkitanimationiteration) X(onwebkitanimationstart) \
    X(onwebkittransitionend) X(onwheel)

// The event handlers of the WindowEventHandlers mixin (html/#windoweventhandlers).
#define PR_JS_DOM_WINDOW_EVENT_HANDLERS(X) \
    X(onafterprint) X(onbeforeprint) X(onbeforeunload) X(onhashchange) X(onlanguagechange) \
    X(onmessage) X(onmessageerror) X(onoffline) X(ononline) X(onpagehide) X(onpagereveal) \
    X(onpageshow) X(onpageswap) X(onpopstate) X(onrejectionhandled) X(onstorage) \
    X(onunhandledrejection) X(onunload)

// Creates the callback of an event handler that calls `function`. The callback belongs to `owner`, which is the object
// of the handler's target (jsCallbackCreate). `out` holds one reference to the callback's context. Returns false with
// an exception pending.
bool prJsDomEventHandlerCallback(JSContext* ctx, JSValueConst function, JSValueConst owner,
                                 PrDomEventHandlerCallback* out);

// The getter and setter of an event handler IDL attribute (html/#event-handler-idl-attributes). The magic is the
// attribute's PrDomEventHandler.
JSValue prJsDomEventHandler(JSContext* ctx, JSValueConst self, int magic);
JSValue prJsDomSetEventHandler(JSContext* ctx, JSValueConst self, JSValueConst input, int magic);

// The entry of a prototype's member list that is the event handler IDL attribute `name`.
#define PR_JS_DOM_EVENT_HANDLER(name)                                                                  \
    PR_JS_MAGIC_GET_SET(#name, prJsDomEventHandler, prJsDomSetEventHandler,                         \
                        PrDomEventHandler_##name),

// Reads a position argument of the insertAdjacent methods, matched ASCII case-insensitively (dom/#insert-adjacent).
// Returns false with an exception pending, which is a SyntaxError for a string that names no position.
bool prJsDomInsertPositionRead(JSContext* ctx, JSValueConst value, PrDomInsertPosition* out);

// Throws the DOMException that a DOM status stands for, and returns JS_EXCEPTION.
JSValue prJsDomThrow(JSContext* ctx, PrStatus status);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
