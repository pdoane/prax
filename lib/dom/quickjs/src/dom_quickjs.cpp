// JavaScript binding of the Document Object Model.
//
// dom - https://dom.spec.whatwg.org/

#include "pr/dom_quickjs.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/dom_abort.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

JSClassID domAbortControllerClass;
JSClassID domDocumentTypeClass;
JSClassID domAttrClass;
JSClassID domNamedNodeMapClass;
JSClassID domImplementationClass;
JSClassID domMutationObserverClass;
JSClassID domMutationRecordClass;
JSClassID domStaticRangeClass;

namespace {

// The key under which the runtime stores its PrJsDomGlobalDocument.
constexpr char kGlobalDocumentKey = 0;

// The key under which the runtime stores its agent (prJsDomSetAgent).
constexpr char kAgentKey = 0;

// html/#cereactions: every member of the bindings runs the custom element reactions of the runtime's agent when the
// member returns. This does nothing if no reactions were queued.
constexpr JsMemberScope kReactions = {
    [](void* agent) { prDomAgentReactionsPush(static_cast<PrDomAgent*>(agent)); },
    [](void* agent) { prDomAgentReactionsPop(static_cast<PrDomAgent*>(agent)); },
};

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

// An event's object holds a reference to the event. A listener that keeps the event after dispatch needs this
// reference.
constexpr JsReferences kEventReferences = {
    [](void* handle) { prDomEventRetain(static_cast<PrDomEvent*>(handle)); },
    [](void* handle) { prDomEventRelease(static_cast<PrDomEvent*>(handle)); },
    false,
};

// A node's object holds a reference to the node, which keeps its tree alive. A node's object is pinned while the engine
// holds the node (nodeHeld), which also keeps the objects of its tree alive (place).
constexpr JsReferences kNodeReferences = {
    [](void* handle) { prDomNodeRetain(static_cast<PrDomNode*>(handle)); },
    [](void* handle) { prDomNodeRelease(static_cast<PrDomNode*>(handle)); },
    true,
};

JSValue nodeToJs(JSContext* ctx, JSValueConst owner, PrDomNode* PR_NULLABLE node) {
    return jsWrapAs(ctx, prJsDomNodeClass, owner, node, false);
}

// Returns the object for a node whose reference a call returned. The object replaces that reference.
JSValue madeToJs(JSContext* ctx, JSValueConst owner, PrDomNode* PR_NULLABLE node) {
    JSValue object = nodeToJs(ctx, owner, node);
    prDomNodeRelease(node);
    return object;
}

// Joins a node's object to the object of its tree parent (jsGroupJoin). The parent has an object whenever a node that
// it holds has one. As a result, whatever a script stores on an object, the listeners that it adds, and the prototype
// that an upgrade gives a custom element last as long as script can reach any node of the tree. A move changes only the
// membership of the moved node, and the memberships of the groups above it. The object of a root other than a document
// keeps its document's object alive, in the same way as the tree keeps the document alive.
void place(JSContext* ctx, JSValueConst object, PrDomNode* node) {
    if (PrDomNode* parent = prDomNodeTreeParent(node)) {
        // Ancestors without an object get one, from the top down, and each joins the object above it.
        Array<PrDomNode* PR_NONNULL> missing;
        JSValue group = jsWrapperOf(ctx, prJsDomNodeClass, parent);
        for (PrDomNode* at = parent; JS_IsUndefined(group);) {
            missing.push(at);
            at = prDomNodeTreeParent(at);
            if (at == nullptr)
                break;
            group = jsWrapperOf(ctx, prJsDomNodeClass, at);
        }
        for (uint32_t i = missing.count; i-- != 0;) {
            JS_FreeValue(ctx, group);
            group = nodeToJs(ctx, JS_NULL, missing.data[i]);
            if (JS_IsException(group))
                break;
        }
        missing.reset();
        if (JS_IsObject(group))
            jsGroupJoin(ctx, group, object);
        JS_FreeValue(ctx, group);
        return;
    }
    PrDomDocument* document = prDomNodeOwnerDocument(node);
    if (document == nullptr) {
        jsSetOwner(ctx, object, JS_UNDEFINED);
        return;
    }
    JSValue owner = prJsDomWrapDocument(ctx, document);
    if (!JS_IsException(owner))
        jsSetOwner(ctx, object, owner);
    JS_FreeValue(ctx, owner);
}

JSValue rangeToJs(JSContext* ctx, JSValueConst owner, PrDomRange* range);

// Makes an object whose handle holds references to nodes hold the object of their document. The object therefore keeps
// alive what a script stored on the nodes, whichever document the nodes are in when script receives them.
void holdDocumentOf(JSContext* ctx, JSValueConst object, PrDomNode* PR_NULLABLE node) {
    if (node == nullptr || !JS_IsObject(object))
        return;
    PrDomDocument* document = prDomNodeOwnerDocument(node);
    if (document == nullptr)
        document = prDomDocumentQuery(prDomNodeAsEventTarget(node));
    JSValue owner = prJsDomWrapDocument(ctx, document);
    if (!JS_IsException(owner))
        jsHold(ctx, object, "[[Documents]]", owner, true);
    JS_FreeValue(ctx, owner);
}

// Applies holdDocumentOf to the event's targets that are nodes.
void holdEventDocuments(JSContext* ctx, JSValueConst object, PrDomEvent* event) {
    holdDocumentOf(ctx, object, prDomNodeQuery(prDomEventTarget(event)));
    if (PrDomFocusEvent* focus = prDomFocusEventQuery(event))
        holdDocumentOf(ctx, object, prDomNodeQuery(prDomFocusEventRelatedTarget(focus)));
    if (PrDomMouseEvent* mouse = prDomMouseEventQuery(event))
        holdDocumentOf(ctx, object, prDomNodeQuery(prDomMouseEventMembers(mouse).relatedTarget));
}

// Moves a node's object to follow the node to its new tree parent. A node without an object has no descendant with one,
// because its object would have been created together with theirs.
void nodeMoved(void* PR_NULLABLE context, PrDomNode* node) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = jsWrapperOf(ctx, prJsDomNodeClass, node);
    if (JS_IsObject(object))
        place(ctx, object, node);
    JS_FreeValue(ctx, object);
}

// Called when the engine starts holding a node for pending work, or releases it.
void nodeHeld(void* PR_NULLABLE context, PrDomNode* node, bool held) {
    jsPin(JS_GetRuntime(static_cast<JSContext*>(context)), prJsDomNodeClass, node, held);
}

// Finalizes a document's object, and clears the document's tree callback, which refers to the runtime that is releasing
// it.
void documentFinalizer(JSRuntime* runtime, JSValue object) {
    if (JsWrapper* wrapper = static_cast<JsWrapper*>(JS_GetOpaque(object, prJsDomDocumentClass)))
        prDomDocumentSetTreeCallback(static_cast<PrDomDocument*>(wrapper->handle), nullptr);
    jsFinalize<void>(runtime, object, prJsDomDocumentClass, nullptr);
}

void nodeWrapped(JSContext* ctx, JSValueConst object, void* handle) {
    PrDomNode* node = static_cast<PrDomNode*>(handle);
    if (PrDomDocument* document = prDomDocumentQuery(prDomNodeAsEventTarget(node))) {
        const PrDomTreeCallback callback = {nodeMoved, nodeHeld, ctx};
        prDomDocumentSetTreeCallback(document, &callback);
    }
    // A document's tree lives as long as its document, and script may request its nodes again.
    if (prDomNodeIsConnected(node))
        jsGroupKeep(ctx, object);
    place(ctx, object, node);
}

// A name from a closed set uses the runtime's single string for that name. Only other names are created when read.

JSValue namespaceUrl(JSContext* ctx, PrDomNamespace space, PrStringView url) {
    if (space == PrDomNamespace_Other)
        return jsNewStringView(ctx, url);
    return url.data != nullptr ? jsFixedString(ctx, url) : JS_NULL;
}

JSValue tagNameOf(JSContext* ctx, const PrDomElement* element) {
    bool uppercase = false;
    PrDomTag tag = prDomElementTagNameTag(element, &uppercase);
    if (tag != PrDomTag_Unknown)
        return jsFixedString(ctx, uppercase ? prDomTagUpperName(tag) : prDomTagName(tag));
    return jsTakeString(ctx, prDomElementTagName(element));
}

JSValue nodeName(JSContext* ctx, JSValueConst nodeObject) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    if (PrDomElement* element = prDomElementQuery(prDomNodeAsEventTarget(node)))
        return tagNameOf(ctx, element);
    PrStringView named = prDomNodeTypeName(prDomNodeType(node));
    if (named.data != nullptr)
        return jsFixedString(ctx, named);
    return jsTakeString(ctx, prDomNodeName(node));
}

JSValue localName(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomTag tag = prDomElementTag(element);
    if (tag != PrDomTag_Unknown)
        return jsFixedString(ctx, prDomTagName(tag));
    return jsNewStringView(ctx, prDomElementLocalName(element));
}

JSValue tagName(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    return element != nullptr ? tagNameOf(ctx, element) : JS_EXCEPTION;
}

JSValue elementNamespace(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return namespaceUrl(ctx, prDomElementNamespace(element), prDomElementNamespaceUri(element));
}

JSValue attrNamespace(JSContext* ctx, JSValueConst attributeObject) {
    PrDomAttr* attribute = jsThis<PrDomAttr>(ctx, attributeObject, domAttrClass);
    if (attribute == nullptr)
        return JS_EXCEPTION;
    return namespaceUrl(ctx, prDomAttrNamespace(attribute), prDomAttrNamespaceUri(attribute));
}

// Returns a node argument, or null with a TypeError thrown.
PrDomNode* PR_NULLABLE nodeArgument(JSContext* ctx, JSValueConst value) {
    PrDomNode* node = jsHandleAs<PrDomNode>(ctx, value, prJsDomNodeClass);
    if (node == nullptr)
        JS_ThrowTypeError(ctx, "Expected a Node");
    return node;
}

// Returns the document in which the global's constructors create nodes.
PrDomDocument* PR_NULLABLE globalDocument(JSContext* ctx) {
    const PrJsDomGlobalDocument* source = static_cast<const PrJsDomGlobalDocument*>(
        jsRuntimeData(JS_GetRuntime(ctx), &kGlobalDocumentKey));
    PrDomDocument* document = source != nullptr ? source->document(ctx) : nullptr;
    if (document == nullptr)
        JS_ThrowTypeError(ctx, "The global object has no document");
    return document;
}

// Returns the object for a node that a constructor made in the global's document. The object replaces the node's
// reference.
JSValue constructed(JSContext* ctx, JSValueConst newTarget, PrDomNode* node) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue object = madeToJs(ctx, global, node);
    JS_FreeValue(ctx, global);
    return jsConstructed(ctx, newTarget, object);
}

// Returns a result that the caller owns as an object of `id` that keeps `jsObject` alive. `destroy` frees the result if
// the object cannot be created.
template <typename T>
JSValue ownedToJs(JSContext* ctx, JSClassID id, JSValueConst jsObject, T* handle, void (*destroy)(T* PR_NULLABLE)) {
    JSValue object = jsWrapAs(ctx, id, jsObject, handle, true);
    if (JS_IsException(object))
        destroy(handle);
    return object;
}

// The (Node or DOMString)... argument of the ParentNode and ChildNode methods (dom/#convert-nodes-into-a-node). The
// call's scope holds the values and their strings.
struct NodesArgument {
    Array<PrDomNodeOrString> values;

    bool read(JSContext* ctx, int argc, JSValueConst* argv) {
        bool read = true;
        for (int i = 0; read && i < argc; ++i) {
            PrDomNodeOrString value = {jsHandleAs<PrDomNode>(ctx, argv[i], prJsDomNodeClass), PrStringView{}};
            read = value.node != nullptr || jsReadString(ctx, argv[i], &value.string);
            values.push(value);
        }
        jsHoldBytes(ctx, values.data);
        return read;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Classifiers, which give a handle the most specific class of the module that applies to it

JSClassID classifyEventTarget(void* handle) {
    PrDomEventTarget* target = static_cast<PrDomEventTarget*>(handle);
    if (prDomNodeQuery(target) != nullptr)
        return prJsDomNodeClass;
    if (prDomAbortSignalQuery(target) != nullptr)
        return prJsDomAbortSignalClass;
    return prJsDomEventTargetClass;
}

JSClassID classifyEvent(void* handle) {
    switch (prDomEventKind(static_cast<PrDomEvent*>(handle))) {
        case PrDomEventKind_Custom: return prJsDomCustomEventClass;
        case PrDomEventKind_Focus: return prJsDomFocusEventClass;
        case PrDomEventKind_Mouse: return prJsDomMouseEventClass;
        case PrDomEventKind_Wheel: return prJsDomWheelEventClass;
        case PrDomEventKind_Keyboard: return prJsDomKeyboardEventClass;
        case PrDomEventKind_Composition: return prJsDomCompositionEventClass;
        case PrDomEventKind_Ui: return prJsDomUiEventClass;
        default: return prJsDomEventClass;
    }
}

JSClassID classifyNode(void* handle) {
    switch (prDomNodeType(static_cast<PrDomNode*>(handle))) {
        case PrDomNodeType_Element: return prJsDomElementClass;
        case PrDomNodeType_Attribute: return domAttrClass;
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection: return prJsDomTextClass;
        case PrDomNodeType_Comment: return prJsDomCommentClass;
        case PrDomNodeType_ProcessingInstruction: return prJsDomProcessingInstructionClass;
        case PrDomNodeType_Document: return prJsDomDocumentClass;
        case PrDomNodeType_DocumentType: return domDocumentTypeClass;
        case PrDomNodeType_DocumentFragment:
            if (prDomShadowRootQuery(prDomNodeAsEventTarget(static_cast<PrDomNode*>(handle))) != nullptr)
                return prJsDomShadowRootClass;
            return prJsDomDocumentFragmentClass;
        default: return prJsDomNodeClass;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Event (dom/#interface-event)

// Implements the constructors of Event and CustomEvent, selected by the magic. CustomEvent's init also holds a detail
// (dom/#dictdef-customeventinit), which the event's object holds.
JSValue constructEvent(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int magic) {
    StringArgument type = {ctx};
    bool present = false;
    PrDomEventInit init = prDomEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue detail = JS_NULL;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            (present && !prJsDomEventInitRead(ctx, dictionary, &init)))
        return JS_EXCEPTION;
    if (present && magic == 1) {
        detail = JS_GetPropertyStr(ctx, dictionary, "detail");
        if (JS_IsException(detail))
            return detail;
        if (JS_IsUndefined(detail))
            detail = JS_NULL;
    }
    JSValue object;
    if (magic == 0)
        object =
            ownedToJs(ctx, prJsDomEventClass, JS_UNDEFINED, prDomEventCreate(type.value(), &init), prDomEventRelease);
    else
        object = ownedToJs(ctx, prJsDomCustomEventClass, JS_UNDEFINED,
            prDomCustomEventAsEvent(prDomCustomEventCreate(type.value(), &init)), prDomEventRelease);
    if (magic == 1 && !JS_IsException(object))
        jsSetReference(ctx, object, 0, detail);
    JS_FreeValue(ctx, detail);
    return jsConstructed(ctx, newTarget, object);
}

JSValue eventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return constructEvent(ctx, newTarget, argc, argv, 0);
}

JSValue customEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return constructEvent(ctx, newTarget, argc, argv, 1);
}

JSValue eventTarget(JSContext* ctx, JSValueConst eventObject, int magic) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, prJsDomEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    return prJsDomWrapEventTarget(ctx, eventObject,
        magic == 0 ? prDomEventTarget(event) : prDomEventCurrentTarget(event));
}

JSValue composedPath(JSContext* ctx, JSValueConst eventObject, int, JSValueConst*) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, prJsDomEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    Array<PrDomEventTarget*> items;
    items.reserve(prDomEventComposedPath(event, nullptr, 0));
    items.count = prDomEventComposedPath(event, items.data, items.capacity);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < items.count; ++i)
        JS_SetPropertyUint32(ctx, array, i, prJsDomWrapEventTarget(ctx, eventObject, items.data[i]));
    items.reset();
    return array;
}

// Implements stopPropagation, stopImmediatePropagation and preventDefault, selected by the magic.
JSValue eventControl(JSContext* ctx, JSValueConst eventObject, int, JSValueConst*, int magic) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, prJsDomEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    constexpr void (*kControls[])(PrDomEvent*) = {
        prDomEventStopPropagation, prDomEventStopImmediatePropagation, prDomEventPreventDefault,
    };
    kControls[magic](event);
    return JS_UNDEFINED;
}

// Implements the setter of a boolean attribute.
template <void (*Set)(PrDomEvent*, bool)>
JSValue setEventFlag(JSContext* ctx, JSValueConst eventObject, JSValueConst input) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, prJsDomEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    Set(event, JS_ToBool(ctx, input) != 0);
    return JS_UNDEFINED;
}

JSValue timeStamp(JSContext* ctx, JSValueConst eventObject) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, prJsDomEventClass);
    return event != nullptr ? JS_NewFloat64(ctx, prDomEventTimeStamp(event)) : JS_EXCEPTION;
}

// Implements initEvent and initCustomEvent, selected by the magic. Their flags default to false, and the detail
// defaults to null.
JSValue initEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv, int magic) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, eventObject, magic == 0 ? prJsDomEventClass : prJsDomCustomEventClass);
    StringArgument type = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    bool bubbles = argc > 1 && JS_ToBool(ctx, argv[1]) != 0;
    bool cancelable = argc > 2 && JS_ToBool(ctx, argv[2]) != 0;
    if (magic == 0) {
        prDomEventInitEvent(event, type.value(), bubbles, cancelable);
    } else if (prDomCustomEventInitCustomEvent(prDomCustomEventQuery(event), type.value(), bubbles, cancelable)) {
        JSValueConst detail = jsArgGiven(argc, argv, 3) ? argv[3] : JS_NULL;
        jsSetReference(ctx, eventObject, 0, detail);
    }
    return JS_UNDEFINED;
}

JSValue customDetail(JSContext* ctx, JSValueConst detailObject) {
    if (jsThis<PrDomCustomEvent>(ctx, detailObject, prJsDomCustomEventClass) == nullptr)
        return JS_EXCEPTION;
    JSValue detail = jsReference(ctx, detailObject, 0);
    return JS_IsUndefined(detail) ? JS_NULL : detail;
}

// ---------------------------------------------------------------------------------------------------------------------
// UIEvent and FocusEvent (uievents/#interface-uievent, uievents/#interface-focusevent)

// Reads an EventTarget member of a dictionary, which is null if it is not given or is null.
bool dictTarget(JSContext* ctx, JSValueConst dictionary, const char* name, PrDomEventTarget* PR_NULLABLE* out) {
    JSValue value = JS_GetPropertyStr(ctx, dictionary, name);
    if (JS_IsException(value))
        return false;
    bool ok = true;
    if (JS_IsUndefined(value) || JS_IsNull(value))
        *out = nullptr;
    else if (jsIsGlobal(ctx, value))
        *out = static_cast<PrDomEventTarget*>(jsGlobalHandleIn(ctx, prJsDomEventTargetClass));
    else if ((*out = jsHandleAs<PrDomEventTarget>(ctx, value, prJsDomEventTargetClass)) == nullptr) {
        JS_ThrowTypeError(ctx, "%s is not an EventTarget", name);
        ok = false;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// Converts a number in the same way as a long (webidl/#js-long). Short and unsigned short values wrap from the result.
int32_t toLong(JSContext* ctx, double value) {
    JSValue number = JS_NewFloat64(ctx, value);
    int32_t out = 0;
    JS_ToInt32(ctx, &out, number);
    JS_FreeValue(ctx, number);
    return out;
}

// Reads the members of UIEventInit (uievents/#dictdef-uieventinit), after those of EventInit, if the dictionary is
// present.
bool uiInitRead(JSContext* ctx, JSValueConst dictionary, bool present, PrDomUiEventInit* init) {
    if (!present)
        return true;
    double detail = 0;
    if (!prJsDomEventInitRead(ctx, dictionary, &init->event) || !jsDictNumber(ctx, dictionary, "detail", &detail) ||
            !dictTarget(ctx, dictionary, "view", &init->view) || !jsDictUint32(ctx, dictionary, "which", &init->which))
        return false;
    init->detail = toLong(ctx, detail);
    return true;
}

// Reads an argument that is a Window or null, as the legacy initializers take their view.
bool viewArgument(JSContext* ctx, JSValueConst value, PrDomEventTarget* PR_NULLABLE* out) {
    if (JS_IsNull(value) || JS_IsUndefined(value))
        *out = nullptr;
    else if (jsIsGlobal(ctx, value))
        *out = static_cast<PrDomEventTarget*>(jsGlobalHandleIn(ctx, prJsDomEventTargetClass));
    else if ((*out = jsHandleAs<PrDomEventTarget>(ctx, value, prJsDomEventTargetClass)) == nullptr) {
        JS_ThrowTypeError(ctx, "view is not a Window");
        return false;
    }
    return true;
}

// Implements the constructors of UIEvent and FocusEvent, selected by the magic. Each init dictionary extends the one
// before (uievents/#dictdef-uieventinit, uievents/#dictdef-focuseventinit).
JSValue constructUiEvent(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int magic) {
    StringArgument type = {ctx};
    bool present = false;
    PrDomFocusEventInit init = prDomFocusEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !uiInitRead(ctx, dictionary, present, &init.ui) ||
            (present && magic == 1 && !dictTarget(ctx, dictionary, "relatedTarget", &init.relatedTarget)))
        return JS_EXCEPTION;
    JSValue object;
    if (magic == 0)
        object = ownedToJs(ctx, prJsDomUiEventClass, JS_UNDEFINED,
            prDomUiEventAsEvent(prDomUiEventCreate(type.value(), &init.ui)), prDomEventRelease);
    else
        object = ownedToJs(ctx, prJsDomFocusEventClass, JS_UNDEFINED,
            prDomFocusEventAsEvent(prDomFocusEventCreate(type.value(), &init)), prDomEventRelease);
    return jsConstructed(ctx, newTarget, object);
}

JSValue uiEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return constructUiEvent(ctx, newTarget, argc, argv, 0);
}

JSValue focusEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return constructUiEvent(ctx, newTarget, argc, argv, 1);
}

// uievents/#dom-uievent-inituievent. The arguments after the type default to false, null or zero.
JSValue uiInitUiEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomUiEvent* event = jsThis<PrDomUiEvent>(ctx, eventObject, prJsDomUiEventClass);
    StringArgument type = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    auto arg = [&](int i) { return i < argc ? argv[i] : JS_UNDEFINED; };
    PrDomUiEventInit init = prDomUiEventInitDefault();
    init.event.bubbles = JS_ToBool(ctx, arg(1)) != 0;
    init.event.cancelable = JS_ToBool(ctx, arg(2)) != 0;
    double detail = 0;
    if (!viewArgument(ctx, arg(3), &init.view) || JS_ToFloat64(ctx, &detail, arg(4)) < 0)
        return JS_EXCEPTION;
    init.detail = toLong(ctx, detail);
    prDomUiEventInitUiEvent(event, type.value(), &init);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// MouseEvent (pointerevents4/#mouseevent-interface, cssom-view-1/#extensions-to-the-mouseevent-interface) and
// WheelEvent (pointerevents4/#interface-wheelevent)

// The boolean members of EventModifierInit (uievents/#dictdef-eventmodifierinit), in the order in which a dictionary is
// read.
struct ModifierMember {
    const char* member;
    PrDomModifiers modifier;
};
constexpr ModifierMember kModifierMembers[] = {
    {"altKey", PrDomModifier_Alt},
    {"ctrlKey", PrDomModifier_Control},
    {"metaKey", PrDomModifier_Meta},
    {"modifierAltGraph", PrDomModifier_AltGraph},
    {"modifierCapsLock", PrDomModifier_CapsLock},
    {"modifierFn", PrDomModifier_Fn},
    {"modifierFnLock", PrDomModifier_FnLock},
    {"modifierHyper", PrDomModifier_Hyper},
    {"modifierNumLock", PrDomModifier_NumLock},
    {"modifierScrollLock", PrDomModifier_ScrollLock},
    {"modifierSuper", PrDomModifier_Super},
    {"modifierSymbol", PrDomModifier_Symbol},
    {"modifierSymbolLock", PrDomModifier_SymbolLock},
    {"shiftKey", PrDomModifier_Shift},
};

// Reads the members of EventModifierInit (uievents/#dictdef-eventmodifierinit) that follow those of UIEventInit.
bool modifiersRead(JSContext* ctx, JSValueConst dictionary, PrDomModifiers* modifiers) {
    for (const ModifierMember& entry : kModifierMembers) {
        bool on = false;
        if (!jsDictBool(ctx, dictionary, entry.member, &on))
            return false;
        *modifiers = PrDomModifiers(on ? *modifiers | entry.modifier : *modifiers);
    }
    return true;
}

// Reads the members of MouseEventInit (pointerevents4/#dom-mouseeventinit) and of the dictionaries that it extends, if
// the dictionary is present.
bool mouseInitRead(JSContext* ctx, JSValueConst dictionary, bool present, PrDomMouseEventInit* init) {
    if (!present)
        return true;
    double button = 0;
    double buttons = 0;
    if (!uiInitRead(ctx, dictionary, present, &init->ui) || !modifiersRead(ctx, dictionary, &init->modifiers) ||
            !jsDictNumber(ctx, dictionary, "button", &button) || !jsDictNumber(ctx, dictionary, "buttons", &buttons) ||
            !jsDictNumber(ctx, dictionary, "clientX", &init->clientX) ||
            !jsDictNumber(ctx, dictionary, "clientY", &init->clientY) ||
            !dictTarget(ctx, dictionary, "relatedTarget", &init->relatedTarget) ||
            !jsDictNumber(ctx, dictionary, "screenX", &init->screenX) ||
            !jsDictNumber(ctx, dictionary, "screenY", &init->screenY))
        return false;
    init->button = int16_t(toLong(ctx, button));
    init->buttons = uint16_t(toLong(ctx, buttons));
    return true;
}

JSValue mouseEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    bool present = false;
    PrDomMouseEventInit init = prDomMouseEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !mouseInitRead(ctx, dictionary, present, &init))
        return JS_EXCEPTION;
    JSValue object = ownedToJs(ctx, prJsDomMouseEventClass, JS_UNDEFINED,
        prDomMouseEventAsEvent(prDomMouseEventCreate(type.value(), &init)), prDomEventRelease);
    return jsConstructed(ctx, newTarget, object);
}

// Implements the MouseEvent's members, selected by the magic.
enum {
    MouseMember_ScreenX,
    MouseMember_ScreenY,
    MouseMember_ClientX,
    MouseMember_ClientY,
    MouseMember_Button,
    MouseMember_Buttons,
    MouseMember_Control,
    MouseMember_Shift,
    MouseMember_Alt,
    MouseMember_Meta,
};
typedef uint8_t MouseMember;

JSValue mouseMember(JSContext* ctx, JSValueConst eventObject, int magic) {
    PrDomMouseEvent* event = jsThis<PrDomMouseEvent>(ctx, eventObject, prJsDomMouseEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    PrDomMouseEventInit members = prDomMouseEventMembers(event);
    switch (magic) {
        case MouseMember_ScreenX: return JS_NewFloat64(ctx, members.screenX);
        case MouseMember_ScreenY: return JS_NewFloat64(ctx, members.screenY);
        case MouseMember_ClientX: return JS_NewFloat64(ctx, members.clientX);
        case MouseMember_ClientY: return JS_NewFloat64(ctx, members.clientY);
        case MouseMember_Button: return JS_NewInt32(ctx, members.button);
        case MouseMember_Buttons: return JS_NewInt32(ctx, members.buttons);
        case MouseMember_Control: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Control) != 0);
        case MouseMember_Shift: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Shift) != 0);
        case MouseMember_Alt: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Alt) != 0);
        default: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Meta) != 0);
    }
}

JSValue mouseRelatedTarget(JSContext* ctx, JSValueConst eventObject) {
    PrDomMouseEvent* event = jsThis<PrDomMouseEvent>(ctx, eventObject, prJsDomMouseEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    return prJsDomWrapEventTarget(ctx, eventObject, prDomMouseEventMembers(event).relatedTarget);
}

// pointerevents4/#dom-mouseevent-getmodifierstate
JSValue mouseGetModifierState(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomMouseEvent* event = jsThis<PrDomMouseEvent>(ctx, eventObject, prJsDomMouseEventClass);
    StringArgument key = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !key.read(argv[0]))
        return JS_EXCEPTION;
    PrDomModifiers modifier = prDomModifiersFromKey(key.value());
    return JS_NewBool(ctx, modifier != 0 && (prDomMouseEventMembers(event).modifiers & modifier) != 0);
}

// pointerevents4/#dom-mouseevent-initmouseevent. The arguments after the type default to false, null or zero.
JSValue mouseInitMouseEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomMouseEvent* event = jsThis<PrDomMouseEvent>(ctx, eventObject, prJsDomMouseEventClass);
    StringArgument type = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    auto arg = [&](int i) { return i < argc ? argv[i] : JS_UNDEFINED; };
    PrDomMouseEventInit init = prDomMouseEventInitDefault();
    init.ui.event.bubbles = JS_ToBool(ctx, arg(1)) != 0;
    init.ui.event.cancelable = JS_ToBool(ctx, arg(2)) != 0;
    if (!viewArgument(ctx, arg(3), &init.ui.view))
        return JS_EXCEPTION;
    double numbers[5] = {};
    for (int i = 0; i < 5; ++i) {
        if (JS_ToFloat64(ctx, &numbers[i], arg(4 + i)) < 0)
            return JS_EXCEPTION;
    }
    init.ui.detail = toLong(ctx, numbers[0]);
    init.screenX = double(toLong(ctx, numbers[1]));
    init.screenY = double(toLong(ctx, numbers[2]));
    init.clientX = double(toLong(ctx, numbers[3]));
    init.clientY = double(toLong(ctx, numbers[4]));
    constexpr PrDomModifiers kKeys[] = {
        PrDomModifier_Control, PrDomModifier_Alt, PrDomModifier_Shift,
        PrDomModifier_Meta,
    };
    for (int i = 0; i < 4; ++i)
        init.modifiers = PrDomModifiers(JS_ToBool(ctx, arg(9 + i)) != 0 ? init.modifiers | kKeys[i] : init.modifiers);
    double button = 0;
    if (JS_ToFloat64(ctx, &button, arg(13)) < 0)
        return JS_EXCEPTION;
    init.button = int16_t(toLong(ctx, button));
    JSValueConst related = arg(14);
    if (!JS_IsNull(related) && !JS_IsUndefined(related) &&
            (init.relatedTarget = jsHandleAs<PrDomEventTarget>(ctx, related, prJsDomEventTargetClass)) == nullptr)
        return JS_ThrowTypeError(ctx, "relatedTarget is not an EventTarget");
    prDomMouseEventInitMouseEvent(event, type.value(), &init);
    return JS_UNDEFINED;
}

JSValue wheelEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    bool present = false;
    PrDomWheelEventInit init = prDomWheelEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !mouseInitRead(ctx, dictionary, present, &init.mouse))
        return JS_EXCEPTION;
    if (present && (!jsDictUint32(ctx, dictionary, "deltaMode", &init.deltaMode) ||
            !jsDictNumber(ctx, dictionary, "deltaX", &init.deltaX) ||
            !jsDictNumber(ctx, dictionary, "deltaY", &init.deltaY) ||
            !jsDictNumber(ctx, dictionary, "deltaZ", &init.deltaZ)))
        return JS_EXCEPTION;
    JSValue object = ownedToJs(ctx, prJsDomWheelEventClass, JS_UNDEFINED,
        prDomWheelEventAsEvent(prDomWheelEventCreate(type.value(), &init)), prDomEventRelease);
    return jsConstructed(ctx, newTarget, object);
}

// Implements the WheelEvent's members, selected by the magic.
enum {
    WheelMember_DeltaX,
    WheelMember_DeltaY,
    WheelMember_DeltaZ,
    WheelMember_DeltaMode,
};
typedef uint8_t WheelMember;

JSValue wheelMember(JSContext* ctx, JSValueConst eventObject, int magic) {
    PrDomWheelEvent* event = jsThis<PrDomWheelEvent>(ctx, eventObject, prJsDomWheelEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    PrDomWheelEventInit members = prDomWheelEventMembers(event);
    switch (magic) {
        case WheelMember_DeltaX: return JS_NewFloat64(ctx, members.deltaX);
        case WheelMember_DeltaY: return JS_NewFloat64(ctx, members.deltaY);
        case WheelMember_DeltaZ: return JS_NewFloat64(ctx, members.deltaZ);
        default: return JS_NewUint32(ctx, members.deltaMode);
    }
}

constexpr JsConstant kWheelEventConstants[] = {
    {"DOM_DELTA_PIXEL", PrDomDeltaMode_Pixel},
    {"DOM_DELTA_LINE", PrDomDeltaMode_Line},
    {"DOM_DELTA_PAGE", PrDomDeltaMode_Page},
};

// ---------------------------------------------------------------------------------------------------------------------
// KeyboardEvent (uievents/#interface-keyboardevent) and CompositionEvent (uievents/#interface-compositionevent)

JSValue keyboardEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument key = {ctx};
    StringArgument code = {ctx};
    bool present = false;
    PrDomKeyboardEventInit init = prDomKeyboardEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !uiInitRead(ctx, dictionary, present, &init.ui))
        return JS_EXCEPTION;
    // The members of KeyboardEventInit and of its legacy partial dictionary are read together, in lexicographic order.
    if (present && (!modifiersRead(ctx, dictionary, &init.modifiers) ||
            !jsDictUint32(ctx, dictionary, "charCode", &init.charCode) || !jsDictString(ctx, dictionary, "code", &code) ||
            !jsDictBool(ctx, dictionary, "isComposing", &init.isComposing) ||
            !jsDictString(ctx, dictionary, "key", &key) || !jsDictUint32(ctx, dictionary, "keyCode", &init.keyCode) ||
            !jsDictUint32(ctx, dictionary, "location", &init.location) ||
            !jsDictBool(ctx, dictionary, "repeat", &init.repeat)))
        return JS_EXCEPTION;
    init.key = key.value();
    init.code = code.value();
    JSValue object = ownedToJs(ctx, prJsDomKeyboardEventClass, JS_UNDEFINED,
        prDomKeyboardEventAsEvent(prDomKeyboardEventCreate(type.value(), &init)), prDomEventRelease);
    return jsConstructed(ctx, newTarget, object);
}

// Implements the KeyboardEvent's members, selected by the magic.
enum {
    KeyboardMember_Key,
    KeyboardMember_Code,
    KeyboardMember_Location,
    KeyboardMember_Control,
    KeyboardMember_Shift,
    KeyboardMember_Alt,
    KeyboardMember_Meta,
    KeyboardMember_Repeat,
    KeyboardMember_IsComposing,
    KeyboardMember_CharCode,
    KeyboardMember_KeyCode,
};
typedef uint8_t KeyboardMember;

JSValue keyboardMember(JSContext* ctx, JSValueConst eventObject, int magic) {
    PrDomKeyboardEvent* event = jsThis<PrDomKeyboardEvent>(ctx, eventObject, prJsDomKeyboardEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    PrDomKeyboardEventInit members = prDomKeyboardEventMembers(event);
    switch (magic) {
        case KeyboardMember_Key: return jsNewStringView(ctx, members.key);
        case KeyboardMember_Code: return jsNewStringView(ctx, members.code);
        case KeyboardMember_Location: return JS_NewUint32(ctx, members.location);
        case KeyboardMember_Control: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Control) != 0);
        case KeyboardMember_Shift: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Shift) != 0);
        case KeyboardMember_Alt: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Alt) != 0);
        case KeyboardMember_Meta: return JS_NewBool(ctx, (members.modifiers & PrDomModifier_Meta) != 0);
        case KeyboardMember_Repeat: return JS_NewBool(ctx, members.repeat);
        case KeyboardMember_IsComposing: return JS_NewBool(ctx, members.isComposing);
        case KeyboardMember_CharCode: return JS_NewUint32(ctx, members.charCode);
        default: return JS_NewUint32(ctx, members.keyCode);
    }
}

// uievents/#dom-keyboardevent-getmodifierstate
JSValue keyboardGetModifierState(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomKeyboardEvent* event = jsThis<PrDomKeyboardEvent>(ctx, eventObject, prJsDomKeyboardEventClass);
    StringArgument key = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !key.read(argv[0]))
        return JS_EXCEPTION;
    PrDomModifiers modifier = prDomModifiersFromKey(key.value());
    return JS_NewBool(ctx, modifier != 0 && (prDomKeyboardEventMembers(event).modifiers & modifier) != 0);
}

// uievents/#dom-keyboardevent-initkeyboardevent. The arguments after the type default to false, null, empty or zero.
JSValue keyboardInitKeyboardEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomKeyboardEvent* event = jsThis<PrDomKeyboardEvent>(ctx, eventObject, prJsDomKeyboardEventClass);
    StringArgument type = {ctx};
    StringArgument key = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    auto arg = [&](int i) { return i < argc ? argv[i] : JS_UNDEFINED; };
    PrDomKeyboardEventInit init = prDomKeyboardEventInitDefault();
    init.ui.event.bubbles = JS_ToBool(ctx, arg(1)) != 0;
    init.ui.event.cancelable = JS_ToBool(ctx, arg(2)) != 0;
    if (!viewArgument(ctx, arg(3), &init.ui.view) || (argc > 4 && !JS_IsUndefined(argv[4]) && !key.read(argv[4])) ||
            JS_ToUint32(ctx, &init.location, arg(5)) < 0)
        return JS_EXCEPTION;
    init.key = key.value();
    constexpr PrDomModifiers kKeys[] = {
        PrDomModifier_Control, PrDomModifier_Alt, PrDomModifier_Shift,
        PrDomModifier_Meta,
    };
    for (int i = 0; i < 4; ++i)
        init.modifiers = PrDomModifiers(JS_ToBool(ctx, arg(6 + i)) != 0 ? init.modifiers | kKeys[i] : init.modifiers);
    prDomKeyboardEventInitKeyboardEvent(event, type.value(), &init);
    return JS_UNDEFINED;
}

constexpr JsConstant kKeyboardEventConstants[] = {
    {"DOM_KEY_LOCATION_STANDARD", PrDomKeyLocation_Standard},
    {"DOM_KEY_LOCATION_LEFT", PrDomKeyLocation_Left},
    {"DOM_KEY_LOCATION_RIGHT", PrDomKeyLocation_Right},
    {"DOM_KEY_LOCATION_NUMPAD", PrDomKeyLocation_Numpad},
};

JSValue compositionEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument data = {ctx};
    bool present = false;
    PrDomUiEventInit init = prDomUiEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !uiInitRead(ctx, dictionary, present, &init) || (present && !jsDictString(ctx, dictionary, "data", &data)))
        return JS_EXCEPTION;
    JSValue object = ownedToJs(ctx, prJsDomCompositionEventClass, JS_UNDEFINED,
        prDomCompositionEventAsEvent(prDomCompositionEventCreate(type.value(), &init, data.value())),
        prDomEventRelease);
    return jsConstructed(ctx, newTarget, object);
}

JSValue compositionData(JSContext* ctx, JSValueConst eventObject) {
    PrDomCompositionEvent* event = jsThis<PrDomCompositionEvent>(ctx, eventObject, prJsDomCompositionEventClass);
    return event != nullptr ? jsNewStringView(ctx, prDomCompositionEventData(event)) : JS_EXCEPTION;
}

// uievents/#dom-compositionevent-initcompositionevent. The arguments after the type default to false, null or empty.
JSValue compositionInitCompositionEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrDomCompositionEvent* event = jsThis<PrDomCompositionEvent>(ctx, eventObject, prJsDomCompositionEventClass);
    StringArgument type = {ctx};
    StringArgument data = {ctx};
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    auto arg = [&](int i) { return i < argc ? argv[i] : JS_UNDEFINED; };
    PrDomUiEventInit init = prDomUiEventInitDefault();
    init.event.bubbles = JS_ToBool(ctx, arg(1)) != 0;
    init.event.cancelable = JS_ToBool(ctx, arg(2)) != 0;
    if (!viewArgument(ctx, arg(3), &init.view) || (argc > 4 && !JS_IsUndefined(argv[4]) && !data.read(argv[4])))
        return JS_EXCEPTION;
    prDomCompositionEventInitCompositionEvent(event, type.value(), &init, data.value());
    return JS_UNDEFINED;
}

JSValue uiEventView(JSContext* ctx, JSValueConst eventObject) {
    PrDomUiEvent* event = jsThis<PrDomUiEvent>(ctx, eventObject, prJsDomUiEventClass);
    return event != nullptr ? prJsDomWrapEventTarget(ctx, eventObject, prDomUiEventView(event)) : JS_EXCEPTION;
}

JSValue focusRelatedTarget(JSContext* ctx, JSValueConst eventObject) {
    PrDomFocusEvent* event = jsThis<PrDomFocusEvent>(ctx, eventObject, prJsDomFocusEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    return prJsDomWrapEventTarget(ctx, eventObject, prDomFocusEventRelatedTarget(event));
}

constexpr JsConstant kEventConstants[] = {
    {"NONE", PrDomEventPhase_None},
    {"CAPTURING_PHASE", PrDomEventPhase_Capturing},
    {"AT_TARGET", PrDomEventPhase_AtTarget},
    {"BUBBLING_PHASE", PrDomEventPhase_Bubbling},
};

// ---------------------------------------------------------------------------------------------------------------------
// EventTarget (dom/#interface-eventtarget)

JSValue targetConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomEventTarget* target = prDomEventTargetCreate();
    return jsConstructed(ctx, newTarget,
        ownedToJs(ctx, prJsDomEventTargetClass, JS_UNDEFINED, target, prDomEventTargetRelease));
}

// Reads the options of addEventListener (dom/#event-flatten-more), or of removeEventListener, whose options have only
// capture (dom/#concept-flatten-options). The boolean form sets only capture. The arguments hold the signal.
bool readListenerOptions(JSContext* ctx, JSValueConst value, bool add, PrDomAddEventListenerOptions* out) {
    if (!JS_IsObject(value)) {
        out->capture = JS_ToBool(ctx, value) != 0;
        return true;
    }
    if (!jsDictBool(ctx, value, "capture", &out->capture))
        return false;
    if (!add)
        return true;
    if (!jsDictBool(ctx, value, "once", &out->once) || !jsDictBool(ctx, value, "passive", &out->passive))
        return false;
    JSValue signal = JS_GetPropertyStr(ctx, value, "signal");
    if (JS_IsException(signal))
        return false;
    if (JS_IsUndefined(signal))
        return true;
    out->signal = jsValueOf<PrDomAbortSignal>(ctx, signal, prJsDomAbortSignalClass, "an AbortSignal");
    jsHold(ctx, JsHeld{.kind = JsHeldKind_Value, .value = signal});
    return out->signal != nullptr;
}

// Implements addEventListener and removeEventListener, selected by the magic.
JSValue listenerMethod(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv, int magic) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, targetObject, prJsDomEventTargetClass);
    if (target == nullptr)
        return JS_EXCEPTION;
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    StringArgument type = {ctx};
    CallbackArgument<PrDomEventListener> callback = {ctx, targetObject};
    if (!type.read(argv[0]))
        return JS_EXCEPTION;
    PrDomAddEventListenerOptions options = prDomAddEventListenerOptionsDefault(target, type.value());
    if (!callback.read(argv[1], prJsDomInvokeListener, true) ||
            !readListenerOptions(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, magic == 0, &options))
        return JS_EXCEPTION;
    if (magic == 0) {
        prDomEventTargetAddEventListener(target, type.value(), callback.get(), &options);
    } else {
        PrDomEventListenerOptions capture = {options.capture};
        prDomEventTargetRemoveEventListener(target, type.value(), callback.get(), &capture);
    }
    return JS_UNDEFINED;
}

JSValue dispatchEvent(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, targetObject, prJsDomEventTargetClass);
    if (target == nullptr)
        return JS_EXCEPTION;
    PrDomEvent* event = argc > 0 ? jsHandleAs<PrDomEvent>(ctx, argv[0], prJsDomEventClass) : nullptr;
    if (event == nullptr)
        return JS_ThrowTypeError(ctx, "Expected an Event");
    bool result = false;
    PrStatus status = prDomEventTargetDispatchEvent(target, event, &result);
    holdEventDocuments(ctx, argv[0], event);
    return status == PrStatus_Ok ? JS_NewBool(ctx, result) : prJsDomThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// EventHandler (html/#event-handler-idl-attributes)

// Calls an event handler's callback with the current target as the receiver, following
// html/#the-event-handler-processing-algorithm.
PrDomEventHandlerResult invokeHandler(void* PR_NULLABLE context, PrDomEvent* event, bool error) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue object = jsWrapAs(ctx, prJsDomEventClass, JS_NULL, event, false);
    holdEventDocuments(ctx, object, event);
    JSValue receiver = prJsDomWrapEventTarget(ctx, JS_NULL, prDomEventCurrentTarget(event));
    JSValue args[5] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    int argc = 1;
    bool ok = !JS_IsException(object) && !JS_IsException(receiver);
    if (ok && error) {
        // An OnErrorEventHandler receives the ErrorEvent's members instead of the event.
        constexpr const char* kMembers[] = {"message", "filename", "lineno", "colno", "error"};
        for (int i = 0; ok && i < 5; ++i) {
            args[i] = JS_GetPropertyStr(ctx, object, kMembers[i]);
            ok = !JS_IsException(args[i]);
        }
        argc = 5;
    } else if (ok) {
        args[0] = JS_DupValue(ctx, object);
    }
    JSValue result = ok ? jsCallbackCall(callback, nullptr, receiver, argc, args) : JS_EXCEPTION;
    for (JSValue arg : args)
        JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, object);
    JS_FreeValue(ctx, receiver);
    PrDomEventHandlerResult answer = PrDomEventHandlerResult_Other;
    if (JS_IsException(result))
        jsReportException(ctx);
    else if (JS_IsBool(result))
        answer = JS_ToBool(ctx, result) ? PrDomEventHandlerResult_True : PrDomEventHandlerResult_False;
    JS_FreeValue(ctx, result);
    return answer;
}

// ---------------------------------------------------------------------------------------------------------------------
// AbortController and AbortSignal (dom/#aborting-ongoing-activities)

// A signal's object holds a reference to the signal.
constexpr JsReferences kAbortSignalReferences = {
    [](void* handle) { prDomAbortSignalRetain(static_cast<PrDomAbortSignal*>(handle)); },
    [](void* handle) { prDomAbortSignalRelease(static_cast<PrDomAbortSignal*>(handle)); },
    false,
};

// The reference slot in which a signal's object keeps its abort reason, which the collector traces with the object. A
// signal passes the reason of an abort to the objects of the signals that it reaches and keeps none itself, so a reason
// that refers back to its signal is a cycle that the collector frees.
constexpr uint32_t kAbortReasonSlot = 0;

// Keeps the reason of the signal's abort in the signal's object (PrDomAbortReasonFn). `reason` is a JSValue.
void keepAbortReason(void* PR_NULLABLE context, PrDomAbortSignal* signal, const void* PR_NULLABLE reason) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = jsWrapperOf(ctx, prJsDomEventTargetClass, prDomAbortSignalAsEventTarget(signal));
    if (!JS_IsUndefined(object) && reason != nullptr)
        jsSetReference(ctx, object, kAbortReasonSlot, *static_cast<const JSValue*>(reason));
    JS_FreeValue(ctx, object);
}

// A signal's object is given the reason of each abort of its signal.
void abortSignalWrapped(JSContext* ctx, JSValueConst, void* handle) {
    prDomAbortSignalSetReasonHandler(static_cast<PrDomAbortSignal*>(handle), keepAbortReason, ctx);
}

// A signal's object stops taking its reasons when it is finalized, since the signal may outlive it.
void abortSignalFinalize(JSRuntime* runtime, JSValue object) {
    if (JsWrapper* wrapper = static_cast<JsWrapper*>(JS_GetOpaque(object, prJsDomAbortSignalClass)))
        prDomAbortSignalSetReasonHandler(static_cast<PrDomAbortSignal*>(wrapper->handle), nullptr, nullptr);
    jsFinalize<PrDomAbortSignal>(runtime, object, prJsDomAbortSignalClass,
        [](PrDomAbortSignal* signal) { prDomAbortSignalRelease(signal); });
}

// Returns the optional reason of an abort: the argument, or a new AbortError DOMException if it is not given, or an
// exception. The caller frees it.
JSValue abortReasonOf(JSContext* ctx, int argc, JSValueConst* argv) {
    return jsArgGiven(argc, argv, 0) ? JS_DupValue(ctx, argv[0]) : jsDomExceptionNew(ctx, "AbortError");
}

JSValue controllerConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    return jsConstructed(ctx, newTarget,
        ownedToJs(ctx, domAbortControllerClass, JS_UNDEFINED, prDomAbortControllerCreate(),
            prDomAbortControllerDestroy));
}

// The controller's signal's object, which the controller's object keeps as its [SameObject] signal.
JSValue controllerSignalObject(JSContext* ctx, JSValueConst object, PrDomAbortController* controller) {
    return jsSameObject(ctx, object, "signal", [&] {
        return jsWrapAs(ctx, prJsDomAbortSignalClass, object, prDomAbortControllerSignal(controller), false);
    });
}

// The signal's object is made before the abort, so that it is there to keep the reason.
JSValue controllerAbort(JSContext* ctx, JSValueConst controllerObject, int argc, JSValueConst* argv) {
    PrDomAbortController* controller = jsThis<PrDomAbortController>(ctx, controllerObject, domAbortControllerClass);
    if (controller == nullptr)
        return JS_EXCEPTION;
    JSValue signal = controllerSignalObject(ctx, controllerObject, controller);
    if (JS_IsException(signal))
        return signal;
    JS_FreeValue(ctx, signal);
    JSValue reason = abortReasonOf(ctx, argc, argv);
    if (JS_IsException(reason))
        return reason;
    prDomAbortControllerAbort(controller, &reason);
    JS_FreeValue(ctx, reason);
    return JS_UNDEFINED;
}

JSValue controllerSignal(JSContext* ctx, JSValueConst controllerObject) {
    PrDomAbortController* controller = jsThis<PrDomAbortController>(ctx, controllerObject, domAbortControllerClass);
    return controller != nullptr ? controllerSignalObject(ctx, controllerObject, controller) : JS_EXCEPTION;
}

JSValue signalAbort(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue reason = abortReasonOf(ctx, argc, argv);
    if (JS_IsException(reason))
        return reason;
    JSValue object = ownedToJs(ctx, prJsDomAbortSignalClass, JS_UNDEFINED, prDomAbortSignalCreateAborted(),
        prDomAbortSignalRelease);
    if (!JS_IsException(object))
        jsSetReference(ctx, object, kAbortReasonSlot, reason);
    JS_FreeValue(ctx, reason);
    return object;
}

// dom/#dom-abortsignal-any. The object of a dependent signal lives as long as the objects of the signals that it was
// made from, any of which may abort it, so that its listeners hear the abort.
JSValue signalAny(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    Array<PrDomAbortSignal*> signals;
    Array<JSValue> objects;
    bool ok = jsSequence(ctx, argv[0], [&](JSValue item) {
        PrDomAbortSignal* signal = jsHandleAs<PrDomAbortSignal>(ctx, item, prJsDomAbortSignalClass);
        if (signal == nullptr) {
            JS_FreeValue(ctx, item);
            JS_ThrowTypeError(ctx, "Expected an AbortSignal");
            return false;
        }
        signals.push(signal);
        objects.push(item);
        return true;
    });
    JSValue result = JS_EXCEPTION;
    if (ok) {
        PrDomAbortSignal* signal = prDomAbortSignalCreateDependent(signals.data, signals.count);
        bool aborted = prDomAbortSignalAborted(signal);
        result = ownedToJs(ctx, prJsDomAbortSignalClass, JS_UNDEFINED, signal, prDomAbortSignalRelease);
        // A signal made aborted takes the reason of the first of its signals that is aborted.
        for (uint32_t i = 0; aborted && !JS_IsException(result) && i < signals.count; ++i) {
            if (prDomAbortSignalAborted(signals.data[i])) {
                JSValue reason = jsReference(ctx, objects.data[i], kAbortReasonSlot);
                jsSetReference(ctx, result, kAbortReasonSlot, reason);
                JS_FreeValue(ctx, reason);
                break;
            }
        }
        for (uint32_t i = 0; !aborted && !JS_IsException(result) && i < objects.count; ++i) {
            if (!jsHold(ctx, objects.data[i], "dependentSignals", result, true)) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            }
        }
    }
    for (uint32_t i = 0; i < objects.count; ++i)
        JS_FreeValue(ctx, objects.data[i]);
    objects.reset();
    signals.reset();
    return result;
}

JSValue signalReason(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrDomAbortSignal>(ctx, object, prJsDomAbortSignalClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomAbortSignalReason(ctx, object);
}

JSValue throwIfAborted(JSContext* ctx, JSValueConst signalObject, int, JSValueConst*) {
    PrDomAbortSignal* signal = jsThis<PrDomAbortSignal>(ctx, signalObject, prJsDomAbortSignalClass);
    if (signal == nullptr)
        return JS_EXCEPTION;
    if (!prDomAbortSignalAborted(signal))
        return JS_UNDEFINED;
    return JS_Throw(ctx, jsReference(ctx, signalObject, kAbortReasonSlot));
}

// ---------------------------------------------------------------------------------------------------------------------
// Node (dom/#interface-node)

JSValue hasChildNodes(JSContext* ctx, JSValueConst nodeObject, int, JSValueConst*) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    return node != nullptr ? JS_NewBool(ctx, prDomNodeHasChildNodes(node)) : JS_EXCEPTION;
}

// Reads an optional Node? argument, which is null if it is null or not given. Throws a TypeError if it is anything
// else.
bool nullableNode(JSContext* ctx, int argc, JSValueConst* argv, int index, PrDomNode* PR_NULLABLE* out) {
    *out = nullptr;
    if (index >= argc || JS_IsNull(argv[index]) || JS_IsUndefined(argv[index]))
        return true;
    *out = nodeArgument(ctx, argv[index]);
    return *out != nullptr;
}

// Implements contains, isEqualNode and isSameNode, selected by the magic.
JSValue nodeRelation(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv, int magic) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    PrDomNode* other = nullptr;
    if (node == nullptr || !jsArgCount(ctx, argc, 1) || !nullableNode(ctx, argc, argv, 0, &other))
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return JS_NewBool(ctx, prDomNodeContains(node, other));
        case 1: return JS_NewBool(ctx, prDomNodeIsEqualNode(node, other));
        default: return JS_NewBool(ctx, node == other);
    }
}

JSValue compareDocumentPosition(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* other = nodeArgument(ctx, argv[0]);
    return other != nullptr ? JS_NewUint32(ctx, prDomNodeCompareDocumentPosition(node, other)) : JS_EXCEPTION;
}

JSValue normalize(JSContext* ctx, JSValueConst nodeObject, int, JSValueConst*) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    prDomNodeNormalize(node);
    return JS_UNDEFINED;
}

JSValue cloneNode(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    PrDomNode* clone = nullptr;
    PrStatus status = prDomNodeCloneNode(node, argc > 0 && JS_ToBool(ctx, argv[0]), &clone);
    return status == PrStatus_Ok ? madeToJs(ctx, nodeObject, clone) : prJsDomThrow(ctx, status);
}

JSValue getRootNode(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    bool present = false;
    bool composed = false;
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (node == nullptr || !jsDictionary(ctx, options, &present) ||
            (present && !jsDictBool(ctx, options, "composed", &composed)))
        return JS_EXCEPTION;
    return nodeToJs(ctx, nodeObject, prDomNodeGetRootNode(node, composed));
}

// Implements the [SameObject] children, a live collection that the node keeps.
JSValue children(JSContext* ctx, JSValueConst nodeObject) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, nodeObject, "children", [&] {
        return ownedToJs(ctx, prJsDomHtmlCollectionClass, nodeObject, prDomHtmlCollectionCreateChildren(node),
            prDomHtmlCollectionDestroy);
    });
}

// Implements getElementById (dom/#dom-nonelementparentnode-getelementbyid), for a document or a fragment.
JSValue getElementById(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    StringArgument id = {ctx};
    if (node == nullptr || !jsArgCount(ctx, argc, 1) || !id.read(argv[0]))
        return JS_EXCEPTION;
    return jsWrapAs(ctx, prJsDomElementClass, nodeObject, prDomNodeGetElementById(node, id.value()), false);
}

// Implements the [SameObject] childNodes, a live list that the node keeps.
JSValue childNodes(JSContext* ctx, JSValueConst nodeObject) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, nodeObject, "childNodes", [&] {
        return ownedToJs(ctx, prJsDomNodeListClass, nodeObject, prDomNodeListCreateChildNodes(node),
            prDomNodeListDestroy);
    });
}

enum {
    Insertion_InsertBefore,
    Insertion_AppendChild,
    Insertion_ReplaceChild,
    Insertion_RemoveChild,
};
typedef uint8_t Insertion;

// Implements the Node methods that insert and remove children (dom/#concept-node-pre-insert), selected by the magic.
// Each returns the node it inserted or removed.
JSValue insertion(JSContext* ctx, JSValueConst parentObject, int argc, JSValueConst* argv, int magic) {
    PrDomNode* parent = jsThis<PrDomNode>(ctx, parentObject, prJsDomNodeClass);
    if (parent == nullptr)
        return JS_EXCEPTION;
    int required = magic == Insertion_InsertBefore || magic == Insertion_ReplaceChild ? 2 : 1;
    if (!jsArgCount(ctx, argc, required))
        return JS_EXCEPTION;
    PrDomNode* node = nodeArgument(ctx, argv[0]);
    if (node == nullptr)
        return JS_EXCEPTION;
    // insertBefore's child may be null, and replaceChild's may not.
    PrDomNode* second = nullptr;
    if (magic == Insertion_ReplaceChild || (magic == Insertion_InsertBefore && !JS_IsNull(argv[1]))) {
        second = nodeArgument(ctx, argv[1]);
        if (second == nullptr)
            return JS_EXCEPTION;
    }
    PrStatus status = PrStatus_Ok;
    switch (Insertion(magic)) {
        case Insertion_InsertBefore: status = prDomNodeInsertBefore(parent, node, second); break;
        case Insertion_AppendChild: status = prDomNodeAppendChild(parent, node); break;
        case Insertion_ReplaceChild: status = prDomNodeReplaceChild(parent, node, second); break;
        case Insertion_RemoveChild: status = prDomNodeRemoveChild(parent, node); break;
    }
    if (status != PrStatus_Ok)
        return prJsDomThrow(ctx, status);
    // replaceChild returns the child that it replaced, and the others return the node that they were given.
    return JS_DupValue(ctx, argv[magic == Insertion_ReplaceChild ? 1 : 0]);
}

enum {
    NodesMethod_Prepend,
    NodesMethod_Append,
    NodesMethod_ReplaceChildren,
    NodesMethod_Before,
    NodesMethod_After,
    NodesMethod_ReplaceWith,
};
typedef uint8_t NodesMethod;

// Implements the ParentNode and ChildNode methods that take nodes and strings, selected by the magic.
JSValue nodesMethod(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv, int magic) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    NodesArgument nodes = {};
    if (node == nullptr || !nodes.read(ctx, argc, argv))
        return JS_EXCEPTION;
    constexpr PrStatus (*kMethods[])(PrDomNode*, const PrDomNodeOrString* PR_NULLABLE, uint32_t) = {
        prDomNodePrepend, prDomNodeAppend, prDomNodeReplaceChildren,
        prDomNodeBefore,  prDomNodeAfter,  prDomNodeReplaceWith,
    };
    PrStatus status = kMethods[magic](node, nodes.values.data, nodes.values.count);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue removeMethod(JSContext* ctx, JSValueConst nodeObject, int, JSValueConst*) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    prDomNodeRemove(node);
    return JS_UNDEFINED;
}

constexpr JsConstant kNodeConstants[] = {
    {"ELEMENT_NODE", PrDomNodeType_Element},
    {"ATTRIBUTE_NODE", PrDomNodeType_Attribute},
    {"TEXT_NODE", PrDomNodeType_Text},
    {"CDATA_SECTION_NODE", PrDomNodeType_CdataSection},
    {"ENTITY_REFERENCE_NODE", PrDomNodeType_EntityReference},
    {"ENTITY_NODE", PrDomNodeType_Entity},
    {"PROCESSING_INSTRUCTION_NODE", PrDomNodeType_ProcessingInstruction},
    {"COMMENT_NODE", PrDomNodeType_Comment},
    {"DOCUMENT_NODE", PrDomNodeType_Document},
    {"DOCUMENT_TYPE_NODE", PrDomNodeType_DocumentType},
    {"DOCUMENT_FRAGMENT_NODE", PrDomNodeType_DocumentFragment},
    {"NOTATION_NODE", PrDomNodeType_Notation},
    {"DOCUMENT_POSITION_DISCONNECTED", PrDomNodePosition_Disconnected},
    {"DOCUMENT_POSITION_PRECEDING", PrDomNodePosition_Preceding},
    {"DOCUMENT_POSITION_FOLLOWING", PrDomNodePosition_Following},
    {"DOCUMENT_POSITION_CONTAINS", PrDomNodePosition_Contains},
    {"DOCUMENT_POSITION_CONTAINED_BY", PrDomNodePosition_ContainedBy},
    {"DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", PrDomNodePosition_ImplementationSpecific},
};

// ---------------------------------------------------------------------------------------------------------------------
// Document (dom/#interface-document)

JSValue documentConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomDocument* document = prDomDocumentCreate();
    return jsConstructed(ctx, newTarget,
        ownedToJs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, prDomDocumentRelease));
}

// Implements the Document methods that take one string, selected by the magic.
enum {
    DocumentMethod_GetElementsByTagName,
    DocumentMethod_GetElementsByClassName,
    DocumentMethod_CreateElement,
    DocumentMethod_CreateTextNode,
    DocumentMethod_CreateComment,
    DocumentMethod_CreateEvent,
};
typedef uint8_t DocumentMethod;

JSValue documentMethod(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    StringArgument text = {ctx};
    if (document == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    switch (DocumentMethod(magic)) {
        case DocumentMethod_GetElementsByTagName:
            return ownedToJs(ctx, prJsDomHtmlCollectionClass, documentObject,
                prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(document), text.value()),
                prDomHtmlCollectionDestroy);
        case DocumentMethod_GetElementsByClassName:
            return ownedToJs(ctx, prJsDomHtmlCollectionClass, documentObject,
                prDomHtmlCollectionCreateByClassName(prDomDocumentAsNode(document), text.value()),
                prDomHtmlCollectionDestroy);
        case DocumentMethod_CreateElement: {
            PrDomElement* element = nullptr;
            PrStatus status = prDomElementCreate(document, text.value(), nullptr, &element);
            if (status != PrStatus_Ok)
                return prJsDomThrow(ctx, status);
            return madeToJs(ctx, documentObject, prDomElementAsNode(element));
        }
        case DocumentMethod_CreateTextNode:
            return madeToJs(ctx, documentObject, prDomTextAsNode(prDomTextCreate(document, text.value())));
        case DocumentMethod_CreateComment:
            return madeToJs(ctx, documentObject, prDomCommentAsNode(prDomCommentCreate(document, text.value())));
        case DocumentMethod_CreateEvent: {
            PrDomEvent* event = nullptr;
            PrDomEventKind kind = PrDomEventKind_Event;
            PrStatus status = PrStatus_NotSupported;
            if (prDomEventKindFromInterface(text.value(), &kind))
                status = prDomEventCreateUninitialized(kind, &event);
            if (status == PrStatus_Ok)
                return ownedToJs(ctx, prJsDomEventClass, JS_UNDEFINED, event, prDomEventRelease);
            return prJsDomThrow(ctx, status);
        }
    }
    return JS_UNDEFINED;
}

JSValue createElementNs(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    StringArgument space = {ctx, PrStringView{}};
    StringArgument name = {ctx};
    if (document == nullptr || !jsArgCount(ctx, argc, 2) || !space.readNullable(argv[0]) || !name.read(argv[1]))
        return JS_EXCEPTION;
    PrDomElement* element = nullptr;
    switch (prDomElementCreateNs(document, space.value(), name.value(), nullptr, &element)) {
        case PrStatus_Ok: return madeToJs(ctx, documentObject, prDomElementAsNode(element));
        case PrStatus_InvalidData: return jsDomExceptionThrow(ctx, "InvalidCharacterError");
        case PrStatus_NotAllowed: return jsDomExceptionThrow(ctx, "NamespaceError");
        default: return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
}

// The options argument is a boolean that says whether the clone is deep, or an ImportNodeOptions whose selfOnly says
// whether it is shallow (dom/#dom-document-importnode).
JSValue importNode(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = jsHandleAs<PrDomNode>(ctx, argv[0], prJsDomNodeClass);
    if (node == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Node");
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool deep = false;
    if (JS_IsObject(options)) {
        bool present = false;
        bool selfOnly = false;
        if (!jsDictionary(ctx, options, &present) || !jsDictBool(ctx, options, "selfOnly", &selfOnly))
            return JS_EXCEPTION;
        deep = !selfOnly;
    } else {
        deep = JS_ToBool(ctx, options) != 0;
    }
    PrDomNode* clone = nullptr;
    PrStatus status = prDomDocumentImportNode(document, node, deep, &clone);
    return status == PrStatus_Ok ? madeToJs(ctx, documentObject, clone) : prJsDomThrow(ctx, status);
}

JSValue createDocumentFragment(JSContext* ctx, JSValueConst documentObject, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return madeToJs(ctx, documentObject, prDomDocumentFragmentAsNode(prDomDocumentFragmentCreate(document)));
}

JSValue createRange(JSContext* ctx, JSValueConst documentObject, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return rangeToJs(ctx, documentObject, prDomRangeCreate(document));
}

JSValue createCdataSection(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    StringArgument data = {ctx};
    if (document == nullptr || !jsArgCount(ctx, argc, 1) || !data.read(argv[0]))
        return JS_EXCEPTION;
    PrDomText* section = nullptr;
    PrStatus status = prDomTextCreateCdataSection(document, data.value(), &section);
    return status == PrStatus_Ok ? madeToJs(ctx, documentObject, prDomTextAsNode(section)) : prJsDomThrow(ctx, status);
}

JSValue createProcessingInstruction(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    StringArgument target = {ctx};
    StringArgument data = {ctx};
    if (document == nullptr || !jsArgCount(ctx, argc, 2) || !target.read(argv[0]) || !data.read(argv[1]))
        return JS_EXCEPTION;
    PrDomProcessingInstruction* instruction = nullptr;
    PrStatus status = prDomProcessingInstructionCreate(document, target.value(), data.value(), &instruction);
    if (status == PrStatus_Ok)
        return madeToJs(ctx, documentObject, prDomProcessingInstructionAsNode(instruction));
    return prJsDomThrow(ctx, status);
}

JSValue setBody(JSContext* ctx, JSValueConst documentObject, JSValueConst input) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrDomElement* body = JS_IsNull(input) ? nullptr : jsHandleAs<PrDomElement>(ctx, input, prJsDomElementClass);
    if (body == nullptr && !JS_IsNull(input))
        return JS_ThrowTypeError(ctx, "Expected an Element");
    PrStatus status = prDomDocumentSetBody(document, body);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue compatMode(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prDomDocumentQuirks(document) ? "BackCompat" : "CSS1Compat");
}

// Implements characterSet and its legacy aliases (dom/#dom-document-characterset). A charset's name is one of a fixed
// set.
JSValue characterSet(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    return document != nullptr ? jsFixedString(ctx, prDomDocumentCharacterSet(document)) : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMImplementation (dom/#interface-domimplementation)

JSValue createDocumentType(JSContext* ctx, JSValueConst implementationObject, int argc, JSValueConst* argv) {
    PrDomImplementation* implementation =
        jsThis<PrDomImplementation>(ctx, implementationObject, domImplementationClass);
    StringArgument name = {ctx};
    StringArgument publicId = {ctx};
    StringArgument systemId = {ctx};
    if (implementation == nullptr || !jsArgCount(ctx, argc, 3) || !name.read(argv[0]) ||
            !publicId.read(argv[1]) || !systemId.read(argv[2]))
        return JS_EXCEPTION;
    PrDomDocumentType* doctype = nullptr;
    PrStatus status = prDomDocumentTypeCreate(implementation, name.value(), publicId.value(), systemId.value(), &doctype);
    if (status != PrStatus_Ok)
        return prJsDomThrow(ctx, status);
    return madeToJs(ctx, implementationObject, prDomDocumentTypeAsNode(doctype));
}

// Implements createDocument(namespace, qualifiedName, doctype). Its namespace is nullable, and its qualifiedName takes
// null as the empty string ([LegacyNullToEmptyString]).
JSValue createDocument(JSContext* ctx, JSValueConst implementationObject, int argc, JSValueConst* argv) {
    PrDomImplementation* implementation =
        jsThis<PrDomImplementation>(ctx, implementationObject, domImplementationClass);
    StringArgument space = {ctx, PrStringView{}};
    StringArgument name = {ctx, ""};
    if (implementation == nullptr || !jsArgCount(ctx, argc, 2) || !space.readNullable(argv[0]) ||
            !name.readNullable(argv[1]))
        return JS_EXCEPTION;
    PrDomDocumentType* doctype = nullptr;
    if (jsArgGiven(argc, argv, 2) && !JS_IsNull(argv[2])) {
        PrDomNode* node = jsHandleAs<PrDomNode>(ctx, argv[2], domDocumentTypeClass);
        if (node == nullptr)
            return JS_ThrowTypeError(ctx, "doctype is not a DocumentType");
        doctype = prDomDocumentTypeQuery(prDomNodeAsEventTarget(node));
    }
    PrDomDocument* document = nullptr;
    PrStatus status = prDomDocumentCreateXml(implementation, space.value(), name.value(), doctype, &document);
    if (status != PrStatus_Ok)
        return prJsDomThrow(ctx, status);
    return ownedToJs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, prDomDocumentRelease);
}

JSValue createHtmlDocument(JSContext* ctx, JSValueConst implementationObject, int argc, JSValueConst* argv) {
    PrDomImplementation* implementation =
        jsThis<PrDomImplementation>(ctx, implementationObject, domImplementationClass);
    StringArgument title = {ctx, PrStringView{}};
    if (implementation == nullptr || (jsArgGiven(argc, argv, 0) && !title.read(argv[0])))
        return JS_EXCEPTION;
    PrDomDocument* document = prDomDocumentCreateHtml(implementation, title.value());
    return ownedToJs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, prDomDocumentRelease);
}

// dom/#dom-domimplementation-hasfeature, which is useless and returns true.
JSValue hasFeature(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return jsThis<PrDomImplementation>(ctx, object, domImplementationClass) != nullptr ? JS_TRUE : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// ShadowRoot (dom/#interface-shadowroot)

JSValue shadowMode(JSContext* ctx, JSValueConst shadowObject) {
    PrDomShadowRoot* shadow = jsThis<PrDomShadowRoot>(ctx, shadowObject, prJsDomShadowRootClass);
    if (shadow == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prDomShadowRootMode(shadow) == PrDomShadowRootMode_Open ? "open" : "closed");
}

JSValue slotAssignment(JSContext* ctx, JSValueConst shadowObject) {
    PrDomShadowRoot* shadow = jsThis<PrDomShadowRoot>(ctx, shadowObject, prJsDomShadowRootClass);
    if (shadow == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prDomShadowRootSlotAssignment(shadow) == PrDomSlotAssignmentMode_Named ? "named" : "manual");
}

// Reads a value of the ShadowRootMode or SlotAssignmentMode enumeration (webidl/#js-enumeration).
bool readEnumeration(JSContext* ctx, JSValueConst value, const PrStringView* names, uint32_t count, uint8_t* out) {
    StringArgument name = {ctx};
    if (!name.read(value))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (name.value() == names[i]) {
            *out = uint8_t(i);
            return true;
        }
    }
    JS_ThrowTypeError(ctx, "Invalid enumeration value");
    return false;
}

JSValue attachShadow(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    constexpr PrStringView kModes[] = {"open", "closed"};
    constexpr PrStringView kAssignments[] = {"named", "manual"};
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    bool present = false;
    // The members of ShadowRootInit in lexicographic order (dom/#dictdef-shadowrootinit). mode is required.
    if (!jsDictionary(ctx, argv[0], &present) || !jsDictBool(ctx, argv[0], "clonable", &init.clonable) ||
            !jsDictBool(ctx, argv[0], "delegatesFocus", &init.delegatesFocus))
        return JS_EXCEPTION;
    JSValue mode = present ? JS_GetPropertyStr(ctx, argv[0], "mode") : JS_UNDEFINED;
    if (JS_IsException(mode))
        return mode;
    if (JS_IsUndefined(mode))
        return JS_ThrowTypeError(ctx, "ShadowRootInit needs a mode");
    bool read = readEnumeration(ctx, mode, kModes, 2, &init.mode);
    JS_FreeValue(ctx, mode);
    if (!read || !jsDictBool(ctx, argv[0], "serializable", &init.serializable))
        return JS_EXCEPTION;
    JSValue assignment = JS_GetPropertyStr(ctx, argv[0], "slotAssignment");
    if (JS_IsException(assignment))
        return assignment;
    read = JS_IsUndefined(assignment) || readEnumeration(ctx, assignment, kAssignments, 2, &init.slotAssignment);
    JS_FreeValue(ctx, assignment);
    if (!read)
        return JS_EXCEPTION;
    PrDomShadowRoot* shadow = nullptr;
    PrStatus status = prDomElementAttachShadow(element, &init, &shadow);
    return status == PrStatus_Ok ? nodeToJs(ctx, elementObject, prDomShadowRootAsNode(shadow)) : prJsDomThrow(ctx, status);
}

JSValue insertAdjacentElement(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    PrDomInsertPosition where = PrDomInsertPosition_BeforeBegin;
    if (element == nullptr || !jsArgCount(ctx, argc, 2) || !prJsDomInsertPositionRead(ctx, argv[0], &where))
        return JS_EXCEPTION;
    PrDomElement* inserted = jsHandleAs<PrDomElement>(ctx, argv[1], prJsDomElementClass);
    if (inserted == nullptr)
        return JS_ThrowTypeError(ctx, "Expected an Element");
    PrDomElement* result = nullptr;
    PrStatus status = prDomElementInsertAdjacentElement(element, where, inserted, &result);
    return status == PrStatus_Ok ? jsWrapAs(ctx, prJsDomElementClass, elementObject, result, false) : prJsDomThrow(ctx, status);
}

JSValue insertAdjacentText(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    PrDomInsertPosition where = PrDomInsertPosition_BeforeBegin;
    StringArgument data = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 2) ||
            !prJsDomInsertPositionRead(ctx, argv[0], &where) || !data.read(argv[1]))
        return JS_EXCEPTION;
    PrStatus status = prDomElementInsertAdjacentText(element, where, data.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue shadowRoot(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomShadowRoot* shadow = prDomElementShadowRoot(element);
    return nodeToJs(ctx, elementObject, shadow != nullptr ? prDomShadowRootAsNode(shadow) : nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// DocumentFragment, Text, Comment and ProcessingInstruction

JSValue fragmentConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomDocument* document = globalDocument(ctx);
    if (document == nullptr)
        return JS_EXCEPTION;
    return constructed(ctx, newTarget, prDomDocumentFragmentAsNode(prDomDocumentFragmentCreate(document)));
}

// Implements the constructors of Text and Comment, selected by the magic: a node with the given data, or the empty
// string.
JSValue dataConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int magic) {
    StringArgument data = {ctx};
    PrDomDocument* document = globalDocument(ctx);
    if (document == nullptr || (jsArgGiven(argc, argv, 0) && !data.read(argv[0])))
        return JS_EXCEPTION;
    PrDomNode* node;
    if (magic == 0)
        node = prDomTextAsNode(prDomTextCreate(document, data.value()));
    else
        node = prDomCommentAsNode(prDomCommentCreate(document, data.value()));
    return constructed(ctx, newTarget, node);
}

JSValue textConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return dataConstruct(ctx, newTarget, argc, argv, 0);
}

JSValue commentConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return dataConstruct(ctx, newTarget, argc, argv, 1);
}

JSValue splitText(JSContext* ctx, JSValueConst textObject, int argc, JSValueConst* argv) {
    PrDomText* text = jsThis<PrDomText>(ctx, textObject, prJsDomTextClass);
    uint32_t offset = 0;
    if (text == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &offset, argv[0]) < 0)
        return JS_EXCEPTION;
    PrDomText* split = nullptr;
    PrStatus status = prDomTextSplitText(text, offset, &split);
    return status == PrStatus_Ok ? madeToJs(ctx, textObject, prDomTextAsNode(split)) : prJsDomThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// Element (dom/#interface-element)

JSValue hasAttributes(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    return element != nullptr ? JS_NewBool(ctx, prDomElementHasAttributes(element)) : JS_EXCEPTION;
}

// Implements the Element methods that take a name or selectors, selected by the magic.
enum {
    ElementMethod_GetAttribute,
    ElementMethod_RemoveAttribute,
    ElementMethod_HasAttribute,
    ElementMethod_GetElementsByTagName,
    ElementMethod_GetElementsByClassName,
};
typedef uint8_t ElementMethod;

JSValue elementMethod(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument text = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    switch (ElementMethod(magic)) {
        case ElementMethod_GetAttribute:
            return jsNewStringView(ctx, prDomElementGetAttribute(element, text.value()));
        case ElementMethod_RemoveAttribute:
            prDomElementRemoveAttribute(element, text.value());
            return JS_UNDEFINED;
        case ElementMethod_HasAttribute:
            return JS_NewBool(ctx, prDomElementHasAttribute(element, text.value()));
        case ElementMethod_GetElementsByTagName:
            return ownedToJs(ctx, prJsDomHtmlCollectionClass, elementObject,
                prDomHtmlCollectionCreateByTagName(prDomElementAsNode(element), text.value()),
                prDomHtmlCollectionDestroy);
        case ElementMethod_GetElementsByClassName:
            return ownedToJs(ctx, prJsDomHtmlCollectionClass, elementObject,
                prDomHtmlCollectionCreateByClassName(prDomElementAsNode(element), text.value()),
                prDomHtmlCollectionDestroy);
    }
    return JS_UNDEFINED;
}

JSValue setAttribute(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument name = {ctx};
    StringArgument value = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 2) || !name.read(argv[0]) || !value.read(argv[1]))
        return JS_EXCEPTION;
    PrStatus status = prDomElementSetAttribute(element, name.value(), value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// Implements getAttributeNS, removeAttributeNS and hasAttributeNS, selected by the magic. Their namespace is nullable,
// and null means no namespace.
enum {
    AttributeNs_Get,
    AttributeNs_Remove,
    AttributeNs_Has,
};
typedef uint8_t AttributeNs;

JSValue attributeNs(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument space = {ctx, ""};
    StringArgument name = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 2) || !space.readNullable(argv[0]) || !name.read(argv[1]))
        return JS_EXCEPTION;
    switch (magic) {
        case AttributeNs_Get:
            return jsNewStringView(ctx, prDomElementGetAttributeNs(element, space.value(), name.value()));
        case AttributeNs_Remove:
            prDomElementRemoveAttributeNs(element, space.value(), name.value());
            return JS_UNDEFINED;
        default:
            return JS_NewBool(ctx, prDomElementHasAttributeNs(element, space.value(), name.value()));
    }
}

JSValue setAttributeNs(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument space = {ctx, ""};
    StringArgument name = {ctx};
    StringArgument value = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 3) || !space.readNullable(argv[0]) ||
            !name.read(argv[1]) || !value.read(argv[2]))
        return JS_EXCEPTION;
    PrStatus status = prDomElementSetAttributeNs(element, space.value(), name.value(), value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// Implements assignedSlot, for an element or a text node (dom/#dom-slotable-assignedslot).
JSValue assignedSlot(JSContext* ctx, JSValueConst nodeObject) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prDomSlottableAssignedSlot(node), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attr (dom/#interface-attr) and NamedNodeMap (dom/#interface-namednodemap), whose handle is its element's handle

JSValue attrToJs(JSContext* ctx, JSValueConst owner, PrDomAttr* PR_NULLABLE attr) {
    return nodeToJs(ctx, owner, attr != nullptr ? prDomAttrAsNode(attr) : nullptr);
}

// Returns the Attr argument at `index`, or null with a TypeError thrown.
PrDomAttr* PR_NULLABLE attrArgument(JSContext* ctx, int argc, JSValueConst* argv, int index) {
    PrDomAttr* attr = index < argc ? jsHandleAs<PrDomAttr>(ctx, argv[index], domAttrClass) : nullptr;
    if (attr == nullptr)
        JS_ThrowTypeError(ctx, "Expected an Attr");
    return attr;
}

JSValue attrSpecified(JSContext* ctx, JSValueConst object) {
    return jsThis<PrDomAttr>(ctx, object, domAttrClass) != nullptr ? JS_TRUE : JS_EXCEPTION;
}

uint32_t attributeMapLength(void* handle) {
    return prDomElementAttributeCount(static_cast<PrDomElement*>(handle));
}

JSValue attributeMapItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    return attrToJs(ctx, object, prDomElementAttributeAt(static_cast<PrDomElement*>(handle), index));
}

bool attributeMapNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    return jsNameAtoms(ctx, static_cast<const PrDomElement*>(handle), prDomElementNamedAttributeNames, out);
}

JSValue attributeMapNamed(JSContext* ctx, JSValueConst object, void* handle, PrStringView name) {
    PrDomAttr* attr = prDomElementGetAttributeNode(static_cast<PrDomElement*>(handle), name);
    return attr != nullptr ? attrToJs(ctx, object, attr) : JS_UNDEFINED;
}

constexpr JsIndexed kAttributeMapIndexed = {attributeMapLength, attributeMapItem, nullptr};

// [LegacyUnenumerableNamedProperties], with no named setter or deleter.
constexpr JsNamed kAttributeMapNamed = {
    attributeMapNames, attributeMapNamed, nullptr, nullptr,
    JsNamedFlag_Unenumerable,
};

// Implements the [SameObject] attributes (dom/#dom-element-attributes).
JSValue attributes(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, "attributes",
        [&] { return jsWrap(ctx, domNamedNodeMapClass, elementObject, element, false); });
}

enum {
    AttributeLookup_ByName,
    AttributeLookup_ByNamespace,
};
typedef uint8_t AttributeLookup;

// Implements getNamedItem and getNamedItemNS of a NamedNodeMap, and getAttributeNode and getAttributeNodeNS of an
// element. The magic's low bit selects the NS form, and its next bit selects the map.
JSValue getAttributeNode(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    bool map = (magic & 2) != 0;
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, map ? domNamedNodeMapClass : prJsDomElementClass);
    bool ns = (magic & 1) == AttributeLookup_ByNamespace;
    StringArgument space = {ctx, ""};
    StringArgument name = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, ns ? 2 : 1) ||
            (ns && !space.readNullable(argv[0])) || !name.read(argv[ns ? 1 : 0]))
        return JS_EXCEPTION;
    PrDomAttr* attr;
    if (ns)
        attr = prDomElementGetAttributeNodeNs(element, space.value(), name.value());
    else
        attr = prDomElementGetAttributeNode(element, name.value());
    return attrToJs(ctx, elementObject, attr);
}

// Implements setNamedItem and setNamedItemNS of a NamedNodeMap, and setAttributeNode and setAttributeNodeNS of an
// element. The magic selects the map.
JSValue setAttributeNode(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element =
        jsThis<PrDomElement>(ctx, elementObject, magic != 0 ? domNamedNodeMapClass : prJsDomElementClass);
    PrDomAttr* attr = element != nullptr ? attrArgument(ctx, argc, argv, 0) : nullptr;
    if (attr == nullptr)
        return JS_EXCEPTION;
    PrDomAttr* old = nullptr;
    if (prDomElementSetAttributeNode(element, attr, &old) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "InUseAttributeError");
    return madeToJs(ctx, elementObject, old != nullptr ? prDomAttrAsNode(old) : nullptr);
}

// Implements removeNamedItem and removeNamedItemNS of a NamedNodeMap, selected by the magic.
JSValue removeNamedItem(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, domNamedNodeMapClass);
    bool ns = magic == AttributeLookup_ByNamespace;
    StringArgument space = {ctx, ""};
    StringArgument name = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, ns ? 2 : 1) ||
            (ns && !space.readNullable(argv[0])) || !name.read(argv[ns ? 1 : 0]))
        return JS_EXCEPTION;
    PrDomAttr* removed = nullptr;
    PrStatus status;
    if (ns)
        status = prDomElementRemoveNamedAttributeNs(element, space.value(), name.value(), &removed);
    else
        status = prDomElementRemoveNamedAttribute(element, name.value(), &removed);
    return status == PrStatus_Ok ? madeToJs(ctx, elementObject, prDomAttrAsNode(removed)) : prJsDomThrow(ctx, status);
}

JSValue removeAttributeNode(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    PrDomAttr* attr = element != nullptr ? attrArgument(ctx, argc, argv, 0) : nullptr;
    if (attr == nullptr)
        return JS_EXCEPTION;
    PrStatus status = prDomElementRemoveAttributeNode(element, attr);
    return status == PrStatus_Ok ? attrToJs(ctx, elementObject, attr) : prJsDomThrow(ctx, status);
}

// dom/#dom-element-toggleattribute
JSValue toggleAttribute(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument name = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    bool force = jsArgGiven(argc, argv, 1) && JS_ToBool(ctx, argv[1]);
    bool present = false;
    PrStatus status = prDomElementToggleAttribute(element, name.value(), jsArgGiven(argc, argv, 1) ? &force : nullptr, &present);
    return status == PrStatus_Ok ? JS_NewBool(ctx, present) : prJsDomThrow(ctx, status);
}

// Implements createAttribute and createAttributeNS, selected by the magic.
JSValue createAttribute(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    bool ns = magic != 0;
    StringArgument space = {ctx, PrStringView{}};
    StringArgument name = {ctx};
    if (document == nullptr || !jsArgCount(ctx, argc, ns ? 2 : 1) ||
            (ns && !space.readNullable(argv[0])) || !name.read(argv[ns ? 1 : 0]))
        return JS_EXCEPTION;
    PrDomAttr* attr = nullptr;
    PrStatus status;
    if (ns)
        status = prDomAttrCreateNs(document, space.value(), name.value(), &attr);
    else
        status = prDomAttrCreate(document, name.value(), &attr);
    switch (status) {
        case PrStatus_Ok: return madeToJs(ctx, documentObject, prDomAttrAsNode(attr));
        case PrStatus_InvalidData: return jsDomExceptionThrow(ctx, "InvalidCharacterError");
        case PrStatus_NotAllowed: return jsDomExceptionThrow(ctx, "NamespaceError");
        default: return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
}

// Implements the [SameObject, PutForwards=value] classList (dom/#dom-element-classlist).
JSValue classList(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, "classList", [&] {
        return ownedToJs(ctx, prJsDomTokenListClass, elementObject, prDomTokenListCreate(element, "class"),
            prDomTokenListDestroy);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMTokenList (dom/#interface-domtokenlist)

uint32_t tokenListLength(void* handle) {
    return prDomTokenListLength(static_cast<PrDomTokenList*>(handle));
}

JSValue tokenListItem(JSContext* ctx, JSValueConst, void* handle, uint32_t index) {
    return jsNewStringView(ctx, prDomTokenListItem(static_cast<PrDomTokenList*>(handle), index));
}

constexpr JsIndexed kTokenListIndexed = {tokenListLength, tokenListItem, nullptr};

JSValue tokenContains(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv) {
    PrDomTokenList* list = jsThis<PrDomTokenList>(ctx, listObject, prJsDomTokenListClass);
    StringArgument token = {ctx};
    if (list == nullptr || !jsArgCount(ctx, argc, 1) || !token.read(argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prDomTokenListContains(list, token.value()));
}

// Implements add and remove, selected by the magic.
JSValue tokenChange(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv, int magic) {
    PrDomTokenList* list = jsThis<PrDomTokenList>(ctx, listObject, prJsDomTokenListClass);
    if (list == nullptr)
        return JS_EXCEPTION;
    Array<PrStringView> tokens;
    bool read = true;
    for (int i = 0; read && i < argc; ++i) {
        PrStringView token = {};
        read = jsReadString(ctx, argv[i], &token);
        tokens.push(token);
    }
    PrStatus status = PrStatus_Ok;
    if (read && magic == 0)
        status = prDomTokenListAdd(list, tokens.data, tokens.count);
    else if (read)
        status = prDomTokenListRemove(list, tokens.data, tokens.count);
    tokens.reset();
    if (!read)
        return JS_EXCEPTION;
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue tokenToggle(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv) {
    PrDomTokenList* list = jsThis<PrDomTokenList>(ctx, listObject, prJsDomTokenListClass);
    StringArgument token = {ctx};
    if (list == nullptr || !jsArgCount(ctx, argc, 1) || !token.read(argv[0]))
        return JS_EXCEPTION;
    bool force = jsArgGiven(argc, argv, 1) && JS_ToBool(ctx, argv[1]);
    bool present = false;
    PrStatus status = prDomTokenListToggle(list, token.value(), jsArgGiven(argc, argv, 1) ? &force : nullptr, &present);
    return status == PrStatus_Ok ? JS_NewBool(ctx, present) : prJsDomThrow(ctx, status);
}

JSValue tokenReplace(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv) {
    PrDomTokenList* list = jsThis<PrDomTokenList>(ctx, listObject, prJsDomTokenListClass);
    StringArgument token = {ctx};
    StringArgument newToken = {ctx};
    if (list == nullptr || !jsArgCount(ctx, argc, 2) || !token.read(argv[0]) || !newToken.read(argv[1]))
        return JS_EXCEPTION;
    bool replaced = false;
    PrStatus status = prDomTokenListReplace(list, token.value(), newToken.value(), &replaced);
    return status == PrStatus_Ok ? JS_NewBool(ctx, replaced) : prJsDomThrow(ctx, status);
}

// The stringifier, which returns value.
JSValue tokenToString(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return jsStringGetter<&prJsDomTokenListClass, prDomTokenListValue>(ctx, object);
}

// dom/#dom-domtokenlist-supports, for a list whose attribute defines no supported tokens, as class does.
JSValue tokenSupports(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    StringArgument token = {ctx};
    if (jsThis<PrDomTokenList>(ctx, object, prJsDomTokenListClass) == nullptr || !jsArgCount(ctx, argc, 1) ||
            !token.read(argv[0]))
        return JS_EXCEPTION;
    return JS_ThrowTypeError(ctx, "The attribute defines no supported tokens");
}

// ---------------------------------------------------------------------------------------------------------------------
// AbstractRange, StaticRange and Range (dom/#ranges), and Selection (selection-api/#selection-interface)

// Makes a range's object hold the objects of the documents of its containers (holdDocumentOf).
void holdRangeDocuments(JSContext* ctx, JSValueConst object) {
    PrDomRange* range = jsHandleAs<PrDomRange>(ctx, object, prJsDomAbstractRangeClass);
    if (range == nullptr)
        return;
    holdDocumentOf(ctx, object, prDomRangeStartContainer(range));
    holdDocumentOf(ctx, object, prDomRangeEndContainer(range));
}

// Returns a range of the C API as an object that keeps `owner` alive: a Range if it is live, and a StaticRange
// otherwise. The caller's reference passes to the object, or is released.
JSValue rangeToJs(JSContext* ctx, JSValueConst owner, PrDomRange* range) {
    JSValue object = ownedToJs(ctx, prDomRangeIsLive(range) ? prJsDomRangeClass : domStaticRangeClass,
        owner, range, prDomRangeRelease);
    holdRangeDocuments(ctx, object);
    return object;
}

// Returns the object for a constructor's range, which keeps the global alive.
JSValue constructedRange(JSContext* ctx, JSValueConst newTarget, PrDomRange* range) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue object = rangeToJs(ctx, global, range);
    JS_FreeValue(ctx, global);
    return jsConstructed(ctx, newTarget, object);
}

// Reads a node and offset argument pair, at argv[0] and argv[1].
bool boundaryArgument(JSContext* ctx, JSValueConst* argv, PrDomNode** node, uint32_t* offset) {
    *node = nodeArgument(ctx, argv[0]);
    return *node != nullptr && JS_ToUint32(ctx, offset, argv[1]) >= 0;
}

// Implements the container and offset attributes, selected by the magic: startContainer, startOffset, endContainer,
// endOffset and collapsed.
JSValue rangeBoundary(JSContext* ctx, JSValueConst rangeObject, int magic) {
    PrDomRange* range = jsThis<PrDomRange>(ctx, rangeObject, prJsDomAbstractRangeClass);
    if (range == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return nodeToJs(ctx, rangeObject, prDomRangeStartContainer(range));
        case 1: return JS_NewUint32(ctx, prDomRangeStartOffset(range));
        case 2: return nodeToJs(ctx, rangeObject, prDomRangeEndContainer(range));
        case 3: return JS_NewUint32(ctx, prDomRangeEndOffset(range));
        default: return JS_NewBool(ctx, prDomRangeCollapsed(range));
    }
}

// dom/#dom-staticrange-staticrange, from a StaticRangeInit, whose members are all required.
JSValue staticRangeConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    bool present = false;
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !jsDictionary(ctx, init, &present))
        return JS_EXCEPTION;
    PrDomNode* containers[2] = {};
    uint32_t offsets[2] = {};
    const char* kContainers[] = {"startContainer", "endContainer"};
    const char* kOffsets[] = {"startOffset", "endOffset"};
    for (int i = 0; i < 2; ++i) {
        JSValue container = JS_GetPropertyStr(ctx, init, kContainers[i]);
        if (JS_IsException(container))
            return container;
        if (JS_IsUndefined(container)) {
            JS_FreeValue(ctx, container);
            return JS_ThrowTypeError(ctx, "%s is required", kContainers[i]);
        }
        containers[i] = nodeArgument(ctx, container);
        JS_FreeValue(ctx, container);
        if (containers[i] == nullptr)
            return JS_EXCEPTION;
    }
    for (int i = 0; i < 2; ++i) {
        JSValue offset = JS_GetPropertyStr(ctx, init, kOffsets[i]);
        if (JS_IsException(offset))
            return offset;
        bool missing = JS_IsUndefined(offset);
        int converted = missing ? 0 : JS_ToUint32(ctx, &offsets[i], offset);
        JS_FreeValue(ctx, offset);
        if (missing)
            return JS_ThrowTypeError(ctx, "%s is required", kOffsets[i]);
        if (converted < 0)
            return JS_EXCEPTION;
    }
    PrDomRange* range = nullptr;
    PrStatus status = prDomRangeCreateStatic(containers[0], offsets[0], containers[1], offsets[1], &range);
    return status == PrStatus_Ok ? constructedRange(ctx, newTarget, range) : prJsDomThrow(ctx, status);
}

// dom/#dom-range-range, for the global's document.
JSValue rangeConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrDomDocument* document = globalDocument(ctx);
    return document != nullptr ? constructedRange(ctx, newTarget, prDomRangeCreate(document)) : JS_EXCEPTION;
}

PrDomRange* PR_NULLABLE liveRange(JSContext* ctx, JSValueConst object) {
    return jsThis<PrDomRange>(ctx, object, prJsDomRangeClass);
}

JSValue rangeCommonAncestor(JSContext* ctx, JSValueConst rangeObject) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    return range != nullptr ? nodeToJs(ctx, rangeObject, prDomRangeCommonAncestorContainer(range)) : JS_EXCEPTION;
}

// Implements setStart and setEnd, selected by the magic.
JSValue rangeSet(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv, int magic) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    PrDomNode* node = nullptr;
    uint32_t offset = 0;
    if (range == nullptr || !jsArgCount(ctx, argc, 2) || !boundaryArgument(ctx, argv, &node, &offset))
        return JS_EXCEPTION;
    PrStatus status = magic == 0 ? prDomRangeSetStart(range, node, offset) : prDomRangeSetEnd(range, node, offset);
    holdRangeDocuments(ctx, rangeObject);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// Implements setStartBefore, setStartAfter, setEndBefore and setEndAfter, selected by the magic, which is the
// PrDomRangeSide. The magic values after those select selectNode, selectNodeContents, insertNode and surroundContents.
JSValue rangeNodeMethod(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv, int magic) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    if (range == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = nodeArgument(ctx, argv[0]);
    if (node == nullptr)
        return JS_EXCEPTION;
    PrStatus status;
    switch (magic) {
        case 4: status = prDomRangeSelectNode(range, node); break;
        case 5: status = prDomRangeSelectNodeContents(range, node); break;
        case 6: status = prDomRangeInsertNode(range, node); break;
        case 7: status = prDomRangeSurroundContents(range, node); break;
        default: status = prDomRangeSetBeside(range, PrDomRangeSide(magic), node); break;
    }
    holdRangeDocuments(ctx, rangeObject);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue rangeCollapse(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    if (range == nullptr)
        return JS_EXCEPTION;
    prDomRangeCollapse(range, argc > 0 && JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

JSValue rangeCompareBoundaryPoints(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    uint32_t how = 0;
    if (range == nullptr || !jsArgCount(ctx, argc, 2) || JS_ToUint32(ctx, &how, argv[0]) < 0)
        return JS_EXCEPTION;
    PrDomRange* source = jsHandleAs<PrDomRange>(ctx, argv[1], prJsDomRangeClass);
    if (source == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Range");
    // The unsigned short conversion (webidl/#js-unsigned-short), then the check of `how` in
    // dom/#dom-range-compareboundarypoints.
    if (uint16_t(how) > PrDomRangeHow_EndToStart)
        return prJsDomThrow(ctx, PrStatus_NotSupported);
    int8_t order = 0;
    PrStatus status = prDomRangeCompareBoundaryPoints(range, PrDomRangeHow(how), source, &order);
    return status == PrStatus_Ok ? JS_NewInt32(ctx, order) : prJsDomThrow(ctx, status);
}

// Implements deleteContents, extractContents, cloneContents, cloneRange, detach and toString, selected by the magic.
JSValue rangeContents(JSContext* ctx, JSValueConst rangeObject, int, JSValueConst*, int magic) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    if (range == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0:
            prDomRangeDeleteContents(range);
            return JS_UNDEFINED;
        case 1:
        case 2: {
            PrDomDocumentFragment* fragment = nullptr;
            PrStatus status =
                magic == 1 ? prDomRangeExtractContents(range, &fragment) : prDomRangeCloneContents(range, &fragment);
            if (status != PrStatus_Ok)
                return prJsDomThrow(ctx, status);
            return madeToJs(ctx, rangeObject, prDomDocumentFragmentAsNode(fragment));
        }
        case 3:
            return rangeToJs(ctx, rangeObject, prDomRangeCloneRange(range));
        case 4:
            return JS_UNDEFINED;   // dom/#dom-range-detach does nothing
        default:
            return jsTakeString(ctx, prDomRangeToString(range));
    }
}

// Implements isPointInRange and comparePoint, selected by the magic.
JSValue rangePoint(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv, int magic) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    PrDomNode* node = nullptr;
    uint32_t offset = 0;
    if (range == nullptr || !jsArgCount(ctx, argc, 2) || !boundaryArgument(ctx, argv, &node, &offset))
        return JS_EXCEPTION;
    if (magic == 0) {
        bool inside = false;
        PrStatus status = prDomRangeIsPointInRange(range, node, offset, &inside);
        return status == PrStatus_Ok ? JS_NewBool(ctx, inside) : prJsDomThrow(ctx, status);
    }
    int8_t order = 0;
    PrStatus status = prDomRangeComparePoint(range, node, offset, &order);
    return status == PrStatus_Ok ? JS_NewInt32(ctx, order) : prJsDomThrow(ctx, status);
}

JSValue rangeIntersectsNode(JSContext* ctx, JSValueConst rangeObject, int argc, JSValueConst* argv) {
    PrDomRange* range = liveRange(ctx, rangeObject);
    if (range == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = nodeArgument(ctx, argv[0]);
    return node != nullptr ? JS_NewBool(ctx, prDomRangeIntersectsNode(range, node)) : JS_EXCEPTION;
}

constexpr JsConstant kRangeConstants[] = {
    {"START_TO_START", PrDomRangeHow_StartToStart},
    {"START_TO_END", PrDomRangeHow_StartToEnd},
    {"END_TO_END", PrDomRangeHow_EndToEnd},
    {"END_TO_START", PrDomRangeHow_EndToStart},
};

PrDomSelection* PR_NULLABLE selectionThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrDomSelection>(ctx, object, prJsDomSelectionClass);
}

// Implements anchorNode, anchorOffset, focusNode, focusOffset, isCollapsed, rangeCount, type and direction, selected by
// the magic.
JSValue selectionAttribute(JSContext* ctx, JSValueConst selectionObject, int magic) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return nodeToJs(ctx, selectionObject, prDomSelectionAnchorNode(selection));
        case 1: return JS_NewUint32(ctx, prDomSelectionAnchorOffset(selection));
        case 2: return nodeToJs(ctx, selectionObject, prDomSelectionFocusNode(selection));
        case 3: return JS_NewUint32(ctx, prDomSelectionFocusOffset(selection));
        case 4: return JS_NewBool(ctx, prDomSelectionIsCollapsed(selection));
        case 5: return JS_NewUint32(ctx, prDomSelectionRangeCount(selection));
        case 6: {
            constexpr PrStringView kTypes[] = {"None", "Caret", "Range"};
            return jsFixedString(ctx, kTypes[prDomSelectionType(selection)]);
        }
        default: {
            constexpr PrStringView kDirections[] = {"none", "forward", "backward"};
            return jsFixedString(ctx, kDirections[prDomSelectionDirection(selection)]);
        }
    }
}

// Makes the selection's object hold the object of its range, which keeps the range alive while it is the selection's
// range (prDomSelectionRange). Every range that a selection has is owned by a script object: one that was added, or one
// created for a call that gives the selection a new range.
constexpr uint32_t kSelectionRangeSlot = 0;

void holdSelectionRange(JSContext* ctx, JSValueConst jsObject, PrDomSelection* selection) {
    PrDomRange* range = prDomSelectionRange(selection);
    JSValue object = range != nullptr ? jsWrap(ctx, prJsDomRangeClass, jsObject, range, false) : JS_UNDEFINED;
    jsSetReference(ctx, jsObject, kSelectionRangeSlot, object);
    JS_FreeValue(ctx, object);
}

// Applies `change`, passing a new range that it may make the selection's range. Then holds the range that the selection
// has afterward.
template <typename Change>
JSValue changeSelection(JSContext* ctx, JSValueConst freshObject, PrDomSelection* selection, Change change) {
    JSValue fresh = rangeToJs(ctx, freshObject, prDomRangeCreate(prDomSelectionDocument(selection)));
    PrDomRange* range = jsHandleAs<PrDomRange>(ctx, fresh, prJsDomRangeClass);
    if (range == nullptr)
        return JS_EXCEPTION;
    PrStatus status = change(range);
    holdSelectionRange(ctx, freshObject, selection);
    JS_FreeValue(ctx, fresh);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue selectionGetRangeAt(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    uint32_t index = 0;
    if (selection == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    PrDomRange* range = nullptr;
    PrStatus status = prDomSelectionGetRangeAt(selection, index, &range);
    if (status != PrStatus_Ok)
        return prJsDomThrow(ctx, status);
    return jsWrap(ctx, prJsDomRangeClass, selectionObject, range, false);
}

// Implements addRange and removeRange, selected by the magic.
JSValue selectionRange(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv, int magic) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomRange* range = jsHandleAs<PrDomRange>(ctx, argv[0], prJsDomRangeClass);
    if (range == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Range");
    PrStatus status = PrStatus_Ok;
    if (magic == 0)
        prDomSelectionAddRange(selection, range);
    else
        status = prDomSelectionRemoveRange(selection, range);
    holdSelectionRange(ctx, selectionObject, selection);
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// Implements removeAllRanges, empty, collapseToStart, collapseToEnd and deleteFromDocument, selected by the magic.
JSValue selectionAction(JSContext* ctx, JSValueConst selectionObject, int, JSValueConst*, int magic) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr)
        return JS_EXCEPTION;
    return changeSelection(ctx, selectionObject, selection, [&](PrDomRange* range) -> PrStatus {
        switch (magic) {
            case 0: prDomSelectionRemoveAllRanges(selection); return PrStatus_Ok;
            case 1: return prDomSelectionCollapseToStart(selection, range);
            case 2: return prDomSelectionCollapseToEnd(selection, range);
            default: prDomSelectionDeleteFromDocument(selection); return PrStatus_Ok;
        }
    });
}

// Implements collapse and setPosition, whose node may be null, and extend, selected by the magic.
JSValue selectionPoint(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv, int magic) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    uint32_t offset = 0;
    if (selection == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = nullptr;
    if (magic == 0 ? !nullableNode(ctx, argc, argv, 0, &node) : (node = nodeArgument(ctx, argv[0])) == nullptr)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToUint32(ctx, &offset, argv[1]) < 0)
        return JS_EXCEPTION;
    return changeSelection(ctx, selectionObject, selection, [&](PrDomRange* range) {
        if (magic == 0)
            return prDomSelectionCollapse(selection, node, offset, range);
        return prDomSelectionExtend(selection, node, offset, range);
    });
}

JSValue selectionSetBaseAndExtent(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    PrDomNode* anchor = nullptr;
    PrDomNode* focus = nullptr;
    uint32_t anchorOffset = 0;
    uint32_t focusOffset = 0;
    if (selection == nullptr || !jsArgCount(ctx, argc, 4) || !boundaryArgument(ctx, argv, &anchor, &anchorOffset) ||
            !boundaryArgument(ctx, argv + 2, &focus, &focusOffset))
        return JS_EXCEPTION;
    return changeSelection(ctx, selectionObject, selection, [&](PrDomRange* range) {
        return prDomSelectionSetBaseAndExtent(selection, anchor, anchorOffset, focus, focusOffset, range);
    });
}

JSValue selectionSelectAllChildren(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = nodeArgument(ctx, argv[0]);
    if (node == nullptr)
        return JS_EXCEPTION;
    return changeSelection(ctx, selectionObject, selection, [&](PrDomRange* range) {
        return prDomSelectionSelectAllChildren(selection, node, range);
    });
}

// selection-api/#dom-selection-modify moves by the rendered layout, which the engine does not have. It returns the
// result of the aborting steps.
JSValue selectionModify(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    if (selectionThis(ctx, object) == nullptr)
        return JS_EXCEPTION;
    for (int i = 0; i < argc && i < 3; ++i) {
        StringArgument text = {ctx};
        if (!text.read(argv[i]))
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

JSValue selectionContainsNode(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* node = nodeArgument(ctx, argv[0]);
    if (node == nullptr)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prDomSelectionContainsNode(selection, node, argc > 1 && JS_ToBool(ctx, argv[1])));
}

JSValue selectionToString(JSContext* ctx, JSValueConst selectionObject, int, JSValueConst*) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prDomSelectionToString(selection));
}

// selection-api/#dom-selection-getcomposedranges, taking a GetComposedRangesOptions dictionary, or shadow roots given
// as separate arguments.
JSValue selectionGetComposedRanges(JSContext* ctx, JSValueConst selectionObject, int argc, JSValueConst* argv) {
    PrDomSelection* selection = selectionThis(ctx, selectionObject);
    if (selection == nullptr)
        return JS_EXCEPTION;
    Array<PrDomShadowRoot*> roots;
    bool ok = true;
    auto addRoot = [&](JSValueConst value) {
        PrDomShadowRoot* root = jsHandleAs<PrDomShadowRoot>(ctx, value, prJsDomShadowRootClass);
        if (root == nullptr) {
            JS_ThrowTypeError(ctx, "Expected a ShadowRoot");
            return false;
        }
        roots.push(root);
        return true;
    };
    if (argc > 0 && jsHandleAs<PrDomShadowRoot>(ctx, argv[0], prJsDomShadowRootClass) != nullptr) {
        for (int i = 0; ok && i < argc; ++i)
            ok = addRoot(argv[i]);
    } else {
        bool present = false;
        JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
        ok = jsDictionary(ctx, options, &present);
        JSValue list = ok && present ? JS_GetPropertyStr(ctx, options, "shadowRoots") : JS_UNDEFINED;
        ok = ok && !JS_IsException(list);
        if (ok && !JS_IsUndefined(list)) {
            ok = jsSequence(ctx, list, [&](JSValue item) {
                bool added = addRoot(item);
                JS_FreeValue(ctx, item);
                return added;
            });
        }
        JS_FreeValue(ctx, list);
    }
    if (!ok) {
        roots.reset();
        return JS_EXCEPTION;
    }
    PrDomRange* range = nullptr;
    uint32_t count = prDomSelectionComposedRanges(selection, roots.data, roots.count, &range);
    roots.reset();
    JSValue array = JS_NewArray(ctx);
    if (count != 0)
        JS_SetPropertyUint32(ctx, array, 0, rangeToJs(ctx, selectionObject, range));
    return array;
}

// ---------------------------------------------------------------------------------------------------------------------
// MutationObserver and MutationRecord (dom/#mutation-observers)

void retainObserver(void* handle) {
    prDomMutationObserverRetain(static_cast<PrDomMutationObserver*>(handle));
}

// Takes the observer's records and returns them as an array of MutationRecord objects.
JSValue takeRecords(JSContext* ctx, PrDomMutationObserver* observer) {
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; prDomMutationObserverRecordCount(observer) != 0; ++i) {
        // Each record is its own script object, so each takes into a record of its own.
        PrDomMutationRecord* taken = prDomMutationRecordCreate();
        prDomMutationObserverTakeRecord(observer, taken);
        PrDomNode* target = prDomMutationRecordTarget(taken);
        JSValue record = ownedToJs(ctx, domMutationRecordClass, JS_UNDEFINED, taken, prDomMutationRecordDestroy);
        holdDocumentOf(ctx, record, target);
        JS_SetPropertyUint32(ctx, array, i, record);
    }
    return array;
}

// Calls the observer's callback with its records and the observer, which is also the receiver
// (dom/#notify-mutation-observers). The observer's object, and the callback it owns, stay alive while a delivery is
// pending, even if script cannot reach them.
void invokeObserver(void* PR_NULLABLE context, PrDomMutationObserver* observer) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue object = jsWrap(ctx, domMutationObserverClass, JS_UNDEFINED, observer, false, retainObserver);
    JSValue args[] = {takeRecords(ctx, observer), object};
    JSValue result = JS_IsException(object) ? JS_EXCEPTION : jsCallbackCall(callback, nullptr, object, 2, args);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, object);
}

JSValue observerConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    CallbackArgument<PrDomMutationCallback> callback = {ctx, JS_UNDEFINED};
    if (!jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeObserver))
        return JS_EXCEPTION;
    PrDomMutationCallback value = *callback.get();
    value.pending = [](void* PR_NULLABLE context, PrDomMutationObserver* observer, bool pending) {
        jsPin(JS_GetRuntime(static_cast<JsCallback*>(context)->ctx), domMutationObserverClass, observer, pending);
    };
    PrDomMutationObserver* observer = prDomMutationObserverCreate(&value);
    JSValue object = ownedToJs(ctx, domMutationObserverClass, JS_UNDEFINED, observer, prDomMutationObserverRelease);
    // The callback belongs to the observer's object, which is created after the callback.
    if (!JS_IsException(object))
        jsCallbackOwn(callback.callback, object);
    return jsConstructed(ctx, newTarget, object);
}

JSValue observe(JSContext* ctx, JSValueConst observerObject, int argc, JSValueConst* argv) {
    PrDomMutationObserver* observer = jsThis<PrDomMutationObserver>(ctx, observerObject, domMutationObserverClass);
    if (observer == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomNode* target = nodeArgument(ctx, argv[0]);
    if (target == nullptr)
        return JS_EXCEPTION;
    PrDomMutationObserverInit init = prDomMutationObserverInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    Array<PrStringView> filter;
    // MutationObserverInit's members in lexicographic order (dom/#dictdef-mutationobserverinit).
    bool read = jsDictionary(ctx, dictionary, &present);
    if (read && present) {
        JSValue value = JS_GetPropertyStr(ctx, dictionary, "attributeFilter");
        read = !JS_IsException(value);
        if (read && !JS_IsUndefined(value)) {
            init.hasAttributeFilter = true;
            read = jsSequence(ctx, value, [&](JSValue item) {
                PrStringView name = {};
                bool ok = jsReadString(ctx, item, &name);
                JS_FreeValue(ctx, item);
                if (ok)
                    filter.push(name);
                return ok;
            });
        }
        JS_FreeValue(ctx, value);
        read = read && jsDictBool(ctx, dictionary, "attributeOldValue", &init.attributeOldValue) &&
            jsDictBool(ctx, dictionary, "attributes", &init.attributes, &init.hasAttributes) &&
            jsDictBool(ctx, dictionary, "characterData", &init.characterData, &init.hasCharacterData) &&
            jsDictBool(ctx, dictionary, "characterDataOldValue", &init.characterDataOldValue) &&
            jsDictBool(ctx, dictionary, "childList", &init.childList) &&
            jsDictBool(ctx, dictionary, "subtree", &init.subtree);
    }
    PrStatus status =
        read ? prDomMutationObserverObserve(observer, target, &init, filter.data, filter.count) : PrStatus_Ok;
    filter.reset();
    if (!read)
        return JS_EXCEPTION;
    if (status != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "The options observe nothing, or ask for what they do not observe");
    // The node's object holds each observer registered on the node, because the registration in C does not hold it. An
    // observer lives while a script can reach a node that it observes (dom/#registered-observer-list).
    return jsHold(ctx, argv[0], "[[Observers]]", observerObject, true) ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue disconnect(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrDomMutationObserver* observer = jsThis<PrDomMutationObserver>(ctx, observerObject, domMutationObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    prDomMutationObserverDisconnect(observer);
    return JS_UNDEFINED;
}

JSValue observerTakeRecords(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrDomMutationObserver* observer = jsThis<PrDomMutationObserver>(ctx, observerObject, domMutationObserverClass);
    return observer != nullptr ? takeRecords(ctx, observer) : JS_EXCEPTION;
}

JSValue recordType(JSContext* ctx, JSValueConst recordObject) {
    constexpr PrStringView kTypes[] = {"attributes", "characterData", "childList"};
    PrDomMutationRecord* record = jsThis<PrDomMutationRecord>(ctx, recordObject, domMutationRecordClass);
    return record != nullptr ? jsFixedString(ctx, kTypes[prDomMutationRecordType(record)]) : JS_EXCEPTION;
}

// Implements the [SameObject] addedNodes and removedNodes, selected by the magic.
JSValue recordNodes(JSContext* ctx, JSValueConst recordObject, int magic) {
    PrDomMutationRecord* record = jsThis<PrDomMutationRecord>(ctx, recordObject, domMutationRecordClass);
    if (record == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, recordObject, magic == 0 ? "addedNodes" : "removedNodes", [&] {
        PrDomNodeList* list = prDomNodeListCreate();
        if (magic == 0)
            prDomMutationRecordAddedNodes(record, list);
        else
            prDomMutationRecordRemovedNodes(record, list);
        return ownedToJs(ctx, prJsDomNodeListClass, recordObject, list, prDomNodeListDestroy);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// CharacterData (dom/#interface-characterdata)

// Implements substringData, insertData, deleteData and replaceData, selected by the magic. They take an offset, a
// count, data, or several of these, in the order in which the interface declares them.
enum {
    DataMethod_Substring,
    DataMethod_Insert,
    DataMethod_Delete,
    DataMethod_Replace,
};
typedef uint8_t DataMethod;

JSValue dataMethod(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv, int magic) {
    PrDomCharacterData* node = jsThis<PrDomCharacterData>(ctx, nodeObject, prJsDomCharacterDataClass);
    if (node == nullptr)
        return JS_EXCEPTION;
    constexpr int kRequired[] = {2, 2, 2, 3};
    if (!jsArgCount(ctx, argc, kRequired[magic]))
        return JS_EXCEPTION;
    uint32_t offset = 0;
    uint32_t count = 0;
    StringArgument data = {ctx};
    bool counted = magic != DataMethod_Insert;
    int dataAt = magic == DataMethod_Insert ? 1 : magic == DataMethod_Replace ? 2 : -1;
    if (JS_ToUint32(ctx, &offset, argv[0]) < 0 || (counted && JS_ToUint32(ctx, &count, argv[1]) < 0) ||
            (dataAt >= 0 && !data.read(argv[dataAt])))
        return JS_EXCEPTION;
    PrStatus status = PrStatus_Ok;
    switch (DataMethod(magic)) {
        case DataMethod_Substring: {
            PrString text = {};
            status = prDomCharacterDataSubstringData(node, offset, count, &text);
            if (status != PrStatus_Ok)
                break;
            return jsTakeString(ctx, text);
        }
        case DataMethod_Insert:
            status = prDomCharacterDataInsertData(node, offset, data.value());
            break;
        case DataMethod_Delete: status = prDomCharacterDataDeleteData(node, offset, count); break;
        case DataMethod_Replace:
            status = prDomCharacterDataReplaceData(node, offset, count, data.value());
            break;
    }
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

JSValue appendData(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv) {
    PrDomCharacterData* node = jsThis<PrDomCharacterData>(ctx, nodeObject, prJsDomCharacterDataClass);
    StringArgument data = {ctx};
    if (node == nullptr || !jsArgCount(ctx, argc, 1) || !data.read(argv[0]))
        return JS_EXCEPTION;
    prDomCharacterDataAppendData(node, data.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLCollection and NodeList (dom/#interface-htmlcollection, dom/#interface-nodelist)

uint32_t collectionLength(void* handle) {
    return prDomHtmlCollectionLength(static_cast<PrDomHtmlCollection*>(handle));
}

JSValue collectionItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    PrDomHtmlCollection* collection = static_cast<PrDomHtmlCollection*>(handle);
    return jsWrapAs(ctx, prJsDomElementClass, object, prDomHtmlCollectionElementAt(collection, index), false);
}

uint32_t listLength(void* handle) {
    return prDomNodeListLength(static_cast<PrDomNodeList*>(handle));
}

JSValue listItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    return nodeToJs(ctx, object, prDomNodeListNodeAt(static_cast<PrDomNodeList*>(handle), index));
}

bool collectionNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    return jsNameAtoms(ctx, static_cast<const PrDomHtmlCollection*>(handle), prDomHtmlCollectionNames, out);
}

JSValue collectionNamed(JSContext* ctx, JSValueConst object, void* handle, PrStringView name) {
    PrDomElement* element = prDomHtmlCollectionNamedItem(static_cast<PrDomHtmlCollection*>(handle), name);
    return element != nullptr ? jsWrapAs(ctx, prJsDomElementClass, object, element, false) : JS_UNDEFINED;
}

constexpr JsIndexed kCollectionIndexed = {collectionLength, collectionItem, nullptr};

// [LegacyUnenumerableNamedProperties], with no named setter or deleter.
constexpr JsNamed kCollectionNamed = {collectionNames, collectionNamed, nullptr, nullptr, JsNamedFlag_Unenumerable};
constexpr JsIndexed kListIndexed = {listLength, listItem, nullptr};

JSValue namedItem(JSContext* ctx, JSValueConst collectionObject, int argc, JSValueConst* argv) {
    PrDomHtmlCollection* collection = jsThis<PrDomHtmlCollection>(ctx, collectionObject, prJsDomHtmlCollectionClass);
    StringArgument name = {ctx};
    if (collection == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    return jsWrapAs(ctx, prJsDomElementClass, collectionObject, prDomHtmlCollectionNamedItem(collection, name.value()), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

// The members of the ParentNode mixin, which Document, DocumentFragment and Element include.
#define PR_PARENT_NODE                                                    \
    PR_JS_MAGIC("prepend", 0, nodesMethod, NodesMethod_Prepend),                   \
    PR_JS_MAGIC("append", 0, nodesMethod, NodesMethod_Append),                     \
    PR_JS_MAGIC("replaceChildren", 0, nodesMethod, NodesMethod_ReplaceChildren),   \
    PR_JS_GET("children", children),                                         \
    PR_JS_GET("firstElementChild",                                           \
              (jsObjectGetter<&prJsDomNodeClass, prDomNodeFirstElementChild, &prJsDomElementClass>)), \
    PR_JS_GET("lastElementChild",                                            \
              (jsObjectGetter<&prJsDomNodeClass, prDomNodeLastElementChild, &prJsDomElementClass>)), \
    PR_JS_GET("childElementCount", (jsNumberGetter<&prJsDomNodeClass, prDomNodeChildElementCount>))

// The members of the ChildNode mixin, which Element and CharacterData include.
#define PR_CHILD_NODE                                                     \
    PR_JS_MAGIC("before", 0, nodesMethod, NodesMethod_Before),                     \
    PR_JS_MAGIC("after", 0, nodesMethod, NodesMethod_After),                       \
    PR_JS_MAGIC("replaceWith", 0, nodesMethod, NodesMethod_ReplaceWith),           \
    PR_JS_METHOD("remove", 0, removeMethod)

// The members of the NonDocumentTypeChildNode mixin, which Element and CharacterData include.
#define PR_NON_DOCUMENT_TYPE_CHILD_NODE                                   \
    PR_JS_GET("previousElementSibling",                                      \
              (jsObjectGetter<&prJsDomNodeClass, prDomNodePreviousElementSibling, &prJsDomElementClass>)), \
    PR_JS_GET("nextElementSibling",                                          \
              (jsObjectGetter<&prJsDomNodeClass, prDomNodeNextElementSibling, &prJsDomElementClass>))

constexpr JSCFunctionListEntry kEventProto[] = {
    PR_JS_TAG("Event"),
    PR_JS_GET("type", (jsStringGetter<&prJsDomEventClass, prDomEventType>)),
    PR_JS_MAGIC_GET("target", eventTarget, 0),
    // dom/#dom-event-srcelement, a legacy alias of target.
    PR_JS_MAGIC_GET("srcElement", eventTarget, 0),
    PR_JS_MAGIC_GET("currentTarget", eventTarget, 1),
    PR_JS_METHOD("composedPath", 0, composedPath),
    PR_JS_GET("eventPhase", (jsNumberGetter<&prJsDomEventClass, prDomEventPhase>)),
    PR_JS_MAGIC("stopPropagation", 0, eventControl, 0),
    PR_JS_GET_SET("cancelBubble", (jsBoolGetter<&prJsDomEventClass, prDomEventCancelBubble>),
        setEventFlag<prDomEventSetCancelBubble>),
    PR_JS_MAGIC("stopImmediatePropagation", 0, eventControl, 1),
    PR_JS_GET("bubbles", (jsBoolGetter<&prJsDomEventClass, prDomEventBubbles>)),
    PR_JS_GET("cancelable", (jsBoolGetter<&prJsDomEventClass, prDomEventCancelable>)),
    PR_JS_GET_SET("returnValue", (jsBoolGetter<&prJsDomEventClass, prDomEventReturnValue>),
        setEventFlag<prDomEventSetReturnValue>),
    PR_JS_MAGIC("preventDefault", 0, eventControl, 2),
    PR_JS_GET("defaultPrevented", (jsBoolGetter<&prJsDomEventClass, prDomEventDefaultPrevented>)),
    PR_JS_GET("composed", (jsBoolGetter<&prJsDomEventClass, prDomEventComposed>)),
    PR_JS_GET("timeStamp", timeStamp),
    PR_JS_MAGIC("initEvent", 1, initEvent, 0),
};

// dom/#dom-event-istrusted
constexpr JSCFunctionListEntry kEventUnforgeable[] = {
    PR_JS_UNFORGEABLE_GET("isTrusted", (jsBoolGetter<&prJsDomEventClass, prDomEventIsTrusted>)),
};

constexpr JSCFunctionListEntry kCustomEventProto[] = {
    PR_JS_TAG("CustomEvent"),
    PR_JS_GET("detail", customDetail),
    PR_JS_MAGIC("initCustomEvent", 1, initEvent, 1),
};

constexpr JSCFunctionListEntry kUiEventProto[] = {
    PR_JS_TAG("UIEvent"),
    PR_JS_GET("view", uiEventView),
    PR_JS_GET("detail", (jsNumberGetter<&prJsDomUiEventClass, prDomUiEventDetail>)),
    PR_JS_METHOD("initUIEvent", 1, uiInitUiEvent),
    PR_JS_GET("which", (jsNumberGetter<&prJsDomUiEventClass, prDomUiEventWhich>)),
};

constexpr JSCFunctionListEntry kFocusEventProto[] = {
    PR_JS_TAG("FocusEvent"),
    PR_JS_GET("relatedTarget", focusRelatedTarget),
};

constexpr JSCFunctionListEntry kMouseEventProto[] = {
    PR_JS_TAG("MouseEvent"),
    PR_JS_MAGIC_GET("screenX", mouseMember, MouseMember_ScreenX),
    PR_JS_MAGIC_GET("screenY", mouseMember, MouseMember_ScreenY),
    PR_JS_MAGIC_GET("clientX", mouseMember, MouseMember_ClientX),
    PR_JS_MAGIC_GET("clientY", mouseMember, MouseMember_ClientY),
    PR_JS_MAGIC_GET("x", mouseMember, MouseMember_ClientX),
    PR_JS_MAGIC_GET("y", mouseMember, MouseMember_ClientY),
    PR_JS_MAGIC_GET("ctrlKey", mouseMember, MouseMember_Control),
    PR_JS_MAGIC_GET("shiftKey", mouseMember, MouseMember_Shift),
    PR_JS_MAGIC_GET("altKey", mouseMember, MouseMember_Alt),
    PR_JS_MAGIC_GET("metaKey", mouseMember, MouseMember_Meta),
    PR_JS_MAGIC_GET("button", mouseMember, MouseMember_Button),
    PR_JS_MAGIC_GET("buttons", mouseMember, MouseMember_Buttons),
    PR_JS_GET("relatedTarget", mouseRelatedTarget),
    PR_JS_METHOD("getModifierState", 1, mouseGetModifierState),
    PR_JS_METHOD("initMouseEvent", 1, mouseInitMouseEvent),
};

constexpr JSCFunctionListEntry kWheelEventProto[] = {
    PR_JS_TAG("WheelEvent"),
    PR_JS_MAGIC_GET("deltaX", wheelMember, WheelMember_DeltaX),
    PR_JS_MAGIC_GET("deltaY", wheelMember, WheelMember_DeltaY),
    PR_JS_MAGIC_GET("deltaZ", wheelMember, WheelMember_DeltaZ),
    PR_JS_MAGIC_GET("deltaMode", wheelMember, WheelMember_DeltaMode),
};

constexpr JSCFunctionListEntry kKeyboardEventProto[] = {
    PR_JS_TAG("KeyboardEvent"),
    PR_JS_MAGIC_GET("key", keyboardMember, KeyboardMember_Key),
    PR_JS_MAGIC_GET("code", keyboardMember, KeyboardMember_Code),
    PR_JS_MAGIC_GET("location", keyboardMember, KeyboardMember_Location),
    PR_JS_MAGIC_GET("ctrlKey", keyboardMember, KeyboardMember_Control),
    PR_JS_MAGIC_GET("shiftKey", keyboardMember, KeyboardMember_Shift),
    PR_JS_MAGIC_GET("altKey", keyboardMember, KeyboardMember_Alt),
    PR_JS_MAGIC_GET("metaKey", keyboardMember, KeyboardMember_Meta),
    PR_JS_MAGIC_GET("repeat", keyboardMember, KeyboardMember_Repeat),
    PR_JS_MAGIC_GET("isComposing", keyboardMember, KeyboardMember_IsComposing),
    PR_JS_MAGIC_GET("charCode", keyboardMember, KeyboardMember_CharCode),
    PR_JS_MAGIC_GET("keyCode", keyboardMember, KeyboardMember_KeyCode),
    PR_JS_METHOD("getModifierState", 1, keyboardGetModifierState),
    PR_JS_METHOD("initKeyboardEvent", 1, keyboardInitKeyboardEvent),
};

constexpr JSCFunctionListEntry kCompositionEventProto[] = {
    PR_JS_TAG("CompositionEvent"),
    PR_JS_GET("data", compositionData),
    PR_JS_METHOD("initCompositionEvent", 1, compositionInitCompositionEvent),
};

constexpr JSCFunctionListEntry kEventTargetProto[] = {
    PR_JS_TAG("EventTarget"),
    PR_JS_MAGIC("addEventListener", 2, listenerMethod, 0),
    PR_JS_MAGIC("removeEventListener", 2, listenerMethod, 1),
    PR_JS_METHOD("dispatchEvent", 1, dispatchEvent),
};

constexpr JSCFunctionListEntry kAbortControllerProto[] = {
    PR_JS_TAG("AbortController"),
    PR_JS_GET("signal", controllerSignal),
    PR_JS_METHOD("abort", 0, controllerAbort),
};

constexpr JSCFunctionListEntry kAbortSignalStatic[] = {
    PR_JS_METHOD("abort", 0, signalAbort),
    PR_JS_METHOD("any", 1, signalAny),
};

constexpr JSCFunctionListEntry kAbortSignalProto[] = {
    PR_JS_TAG("AbortSignal"),
    PR_JS_GET("aborted", (jsBoolGetter<&prJsDomAbortSignalClass, prDomAbortSignalAborted>)),
    PR_JS_GET("reason", signalReason),
    PR_JS_METHOD("throwIfAborted", 0, throwIfAborted),
    PR_JS_DOM_EVENT_HANDLER(onabort),
};

constexpr JSCFunctionListEntry kNodeProto[] = {
    PR_JS_TAG("Node"),
    PR_JS_GET("nodeType", (jsNumberGetter<&prJsDomNodeClass, prDomNodeType>)),
    PR_JS_GET("baseURI", (jsStringGetter<&prJsDomNodeClass, prDomNodeBaseUri>)),
    PR_JS_GET("isConnected", (jsBoolGetter<&prJsDomNodeClass, prDomNodeIsConnected>)),
    PR_JS_GET("ownerDocument", (jsObjectGetter<&prJsDomNodeClass, prDomNodeOwnerDocument, &prJsDomDocumentClass>)),
    PR_JS_GET("parentNode", (jsObjectGetter<&prJsDomNodeClass, prDomNodeParentNode, &prJsDomNodeClass>)),
    PR_JS_GET("parentElement", (jsObjectGetter<&prJsDomNodeClass, prDomNodeParentElement, &prJsDomElementClass>)),
    PR_JS_METHOD("hasChildNodes", 0, hasChildNodes),
    PR_JS_GET("childNodes", childNodes),
    PR_JS_GET("firstChild", (jsObjectGetter<&prJsDomNodeClass, prDomNodeFirstChild, &prJsDomNodeClass>)),
    PR_JS_GET("lastChild", (jsObjectGetter<&prJsDomNodeClass, prDomNodeLastChild, &prJsDomNodeClass>)),
    PR_JS_GET("previousSibling", (jsObjectGetter<&prJsDomNodeClass, prDomNodePreviousSibling, &prJsDomNodeClass>)),
    PR_JS_GET("nextSibling", (jsObjectGetter<&prJsDomNodeClass, prDomNodeNextSibling, &prJsDomNodeClass>)),
    PR_JS_GET_SET("textContent", (jsStringGetter<&prJsDomNodeClass, prDomNodeTextContent>),
        (jsStringSetter<&prJsDomNodeClass, prDomNodeSetTextContent, true>)),
    PR_JS_MAGIC("insertBefore", 2, insertion, Insertion_InsertBefore),
    PR_JS_MAGIC("appendChild", 1, insertion, Insertion_AppendChild),
    PR_JS_MAGIC("replaceChild", 2, insertion, Insertion_ReplaceChild),
    PR_JS_MAGIC("removeChild", 1, insertion, Insertion_RemoveChild),
    PR_JS_GET("nodeName", nodeName),
    PR_JS_GET_SET("nodeValue", (jsStringGetter<&prJsDomNodeClass, prDomNodeValue>),
        (jsStringSetter<&prJsDomNodeClass, prDomNodeSetValue, true>)),
    PR_JS_METHOD("getRootNode", 0, getRootNode),
    PR_JS_METHOD("cloneNode", 0, cloneNode),
    PR_JS_MAGIC("contains", 1, nodeRelation, 0),
    PR_JS_METHOD("compareDocumentPosition", 1, compareDocumentPosition),
    PR_JS_METHOD("normalize", 0, normalize),
    PR_JS_MAGIC("isEqualNode", 1, nodeRelation, 1),
    PR_JS_MAGIC("isSameNode", 1, nodeRelation, 2),
};

constexpr JSCFunctionListEntry kDocumentProto[] = {
    PR_JS_TAG("Document"),
    PR_JS_GET("URL", (jsStringGetter<&prJsDomDocumentClass, prDomDocumentUrl>)),
    PR_JS_GET("documentURI", (jsStringGetter<&prJsDomDocumentClass, prDomDocumentUrl>)),
    PR_JS_GET("contentType", (jsStringGetter<&prJsDomDocumentClass, prDomDocumentContentType>)),
    PR_JS_GET("documentElement", (jsObjectGetter<&prJsDomDocumentClass, prDomDocumentElement, &prJsDomElementClass>)),
    PR_JS_MAGIC("getElementsByTagName", 1, documentMethod, DocumentMethod_GetElementsByTagName),
    PR_JS_MAGIC("getElementsByClassName", 1, documentMethod, DocumentMethod_GetElementsByClassName),
    PR_JS_MAGIC("createElement", 1, documentMethod, DocumentMethod_CreateElement),
    PR_JS_METHOD("createElementNS", 2, createElementNs),
    PR_JS_METHOD("importNode", 1, importNode),
    PR_JS_METHOD("createDocumentFragment", 0, createDocumentFragment),
    PR_JS_MAGIC("createAttribute", 1, createAttribute, 0),
    PR_JS_MAGIC("createAttributeNS", 2, createAttribute, 1),
    PR_JS_METHOD("createRange", 0, createRange),
    PR_JS_MAGIC("createTextNode", 1, documentMethod, DocumentMethod_CreateTextNode),
    PR_JS_MAGIC("createComment", 1, documentMethod, DocumentMethod_CreateComment),
    PR_JS_METHOD("createCDATASection", 1, createCdataSection),
    PR_JS_METHOD("createProcessingInstruction", 2, createProcessingInstruction),
    PR_JS_MAGIC("createEvent", 1, documentMethod, DocumentMethod_CreateEvent),
    PR_JS_GET_SET("title", (jsStringGetter<&prJsDomDocumentClass, prDomDocumentTitle>),
        (jsStringSetter<&prJsDomDocumentClass, prDomDocumentSetTitle>)),
    PR_JS_GET_SET("body", (jsObjectGetter<&prJsDomDocumentClass, prDomDocumentBody, &prJsDomElementClass>), setBody),
    PR_JS_GET("head", (jsObjectGetter<&prJsDomDocumentClass, prDomDocumentHead, &prJsDomElementClass>)),
    PR_JS_METHOD("getElementById", 1, getElementById),
    PR_JS_GET("implementation", (jsSameObjectGetter<&prJsDomDocumentClass, prDomDocumentImplementation,
        &domImplementationClass, "implementation">)),
    PR_JS_GET("doctype", (jsObjectGetter<&prJsDomDocumentClass, prDomDocumentDoctype, &prJsDomNodeClass>)),
    PR_JS_GET("compatMode", compatMode),
    PR_JS_GET("characterSet", characterSet),
    PR_JS_GET("charset", characterSet),
    PR_JS_GET("inputEncoding", characterSet),
    PR_JS_DOM_GLOBAL_EVENT_HANDLER_MEMBERS PR_PARENT_NODE,
};

constexpr JSCFunctionListEntry kDocumentFragmentProto[] = {
    PR_JS_TAG("DocumentFragment"),
    PR_JS_METHOD("getElementById", 1, getElementById),
    PR_PARENT_NODE,
};

constexpr JSCFunctionListEntry kDocumentTypeProto[] = {
    PR_JS_TAG("DocumentType"),
    PR_JS_GET("name", (jsStringGetter<&domDocumentTypeClass, prDomDocumentTypeName>)),
    PR_JS_GET("publicId", (jsStringGetter<&domDocumentTypeClass, prDomDocumentTypePublicId>)),
    PR_JS_GET("systemId", (jsStringGetter<&domDocumentTypeClass, prDomDocumentTypeSystemId>)),
    PR_CHILD_NODE,
};

constexpr JSCFunctionListEntry kImplementationProto[] = {
    PR_JS_TAG("DOMImplementation"),
    PR_JS_METHOD("createDocumentType", 3, createDocumentType),
    PR_JS_METHOD("createDocument", 2, createDocument),
    PR_JS_METHOD("createHTMLDocument", 0, createHtmlDocument),
    PR_JS_METHOD("hasFeature", 0, hasFeature),
};

constexpr JSCFunctionListEntry kShadowRootProto[] = {
    PR_JS_TAG("ShadowRoot"),
    PR_JS_GET("mode", shadowMode),
    PR_JS_GET("delegatesFocus", (jsBoolGetter<&prJsDomShadowRootClass, prDomShadowRootDelegatesFocus>)),
    PR_JS_GET("slotAssignment", slotAssignment),
    PR_JS_GET("clonable", (jsBoolGetter<&prJsDomShadowRootClass, prDomShadowRootClonable>)),
    PR_JS_GET("serializable", (jsBoolGetter<&prJsDomShadowRootClass, prDomShadowRootSerializable>)),
    PR_JS_GET("host", (jsObjectGetter<&prJsDomShadowRootClass, prDomShadowRootHost, &prJsDomElementClass>)),
    PR_JS_DOM_EVENT_HANDLER(onslotchange),
};

constexpr JSCFunctionListEntry kElementProto[] = {
    PR_JS_TAG("Element"),
    PR_JS_METHOD("attachShadow", 1, attachShadow),
    PR_JS_METHOD("insertAdjacentElement", 2, insertAdjacentElement),
    PR_JS_METHOD("insertAdjacentText", 2, insertAdjacentText),
    PR_JS_GET("shadowRoot", shadowRoot),
    PR_JS_GET("localName", localName),
    PR_JS_GET("namespaceURI", elementNamespace),
    PR_JS_GET("prefix", (jsStringGetter<&prJsDomElementClass, prDomElementPrefix>)),
    PR_JS_GET("tagName", tagName),
    PR_JS_GET_SET("id", (jsStringGetter<&prJsDomElementClass, prDomElementId>),
        (jsStringSetter<&prJsDomElementClass, prDomElementSetId>)),
    PR_JS_GET_SET("slot", (jsStringGetter<&prJsDomElementClass, prDomElementSlot>),
        (jsStringSetter<&prJsDomElementClass, prDomElementSetSlot>)),
    PR_JS_GET("assignedSlot", assignedSlot),
    PR_JS_MAGIC("getAttribute", 1, elementMethod, ElementMethod_GetAttribute),
    PR_JS_METHOD("setAttribute", 2, setAttribute),
    PR_JS_MAGIC("removeAttribute", 1, elementMethod, ElementMethod_RemoveAttribute),
    PR_JS_MAGIC("hasAttribute", 1, elementMethod, ElementMethod_HasAttribute),
    PR_JS_METHOD("hasAttributes", 0, hasAttributes),
    PR_JS_GET("attributes", attributes),
    PR_JS_METHOD("getAttributeNames", 0, (jsStringsMethod<&prJsDomElementClass, prDomElementGetAttributeNames>)),
    PR_JS_METHOD("toggleAttribute", 1, toggleAttribute),
    PR_JS_MAGIC("getAttributeNode", 1, getAttributeNode, AttributeLookup_ByName),
    PR_JS_MAGIC("getAttributeNodeNS", 2, getAttributeNode, AttributeLookup_ByNamespace),
    PR_JS_MAGIC("setAttributeNode", 1, setAttributeNode, 0),
    PR_JS_MAGIC("setAttributeNodeNS", 1, setAttributeNode, 0),
    PR_JS_METHOD("removeAttributeNode", 1, removeAttributeNode),
    PR_JS_MAGIC("getAttributeNS", 2, attributeNs, AttributeNs_Get),
    PR_JS_METHOD("setAttributeNS", 3, setAttributeNs),
    PR_JS_MAGIC("removeAttributeNS", 2, attributeNs, AttributeNs_Remove),
    PR_JS_MAGIC("hasAttributeNS", 2, attributeNs, AttributeNs_Has),
    PR_JS_GET_SET("className", (jsStringGetter<&prJsDomElementClass, prDomElementClassName>),
        (jsStringSetter<&prJsDomElementClass, prDomElementSetClassName>)),
    PR_JS_GET_SET("classList", classList, (jsPutForwards<"classList", "value">)),
    PR_JS_MAGIC("getElementsByTagName", 1, elementMethod, ElementMethod_GetElementsByTagName),
    PR_JS_MAGIC("getElementsByClassName", 1, elementMethod, ElementMethod_GetElementsByClassName),
    PR_PARENT_NODE,
    PR_CHILD_NODE,
    PR_NON_DOCUMENT_TYPE_CHILD_NODE,
};

constexpr JSCFunctionListEntry kAttrProto[] = {
    PR_JS_TAG("Attr"),
    PR_JS_GET("namespaceURI", attrNamespace),
    PR_JS_GET("prefix", (jsStringGetter<&domAttrClass, prDomAttrPrefix>)),
    PR_JS_GET("localName", (jsStringGetter<&domAttrClass, prDomAttrLocalName>)),
    PR_JS_GET("name", (jsStringGetter<&domAttrClass, prDomAttrName>)),
    PR_JS_GET_SET("value", (jsStringGetter<&domAttrClass, prDomAttrValue>),
        (jsStringSetter<&domAttrClass, prDomAttrSetValue>)),
    PR_JS_GET("ownerElement", (jsObjectGetter<&domAttrClass, prDomAttrOwnerElement, &prJsDomElementClass>)),
    PR_JS_GET("specified", attrSpecified),
};

constexpr JSCFunctionListEntry kNamedNodeMapProto[] = {
    PR_JS_TAG("NamedNodeMap"),
    PR_JS_GET("length", (jsNumberGetter<&domNamedNodeMapClass, prDomElementAttributeCount>)),
    PR_JS_METHOD("item", 1, (jsItemMethod<&domNamedNodeMapClass, &kAttributeMapIndexed>)),
    PR_JS_MAGIC("getNamedItem", 1, getAttributeNode, AttributeLookup_ByName | 2),
    PR_JS_MAGIC("getNamedItemNS", 2, getAttributeNode, AttributeLookup_ByNamespace | 2),
    PR_JS_MAGIC("setNamedItem", 1, setAttributeNode, 1),
    PR_JS_MAGIC("setNamedItemNS", 1, setAttributeNode, 1),
    PR_JS_MAGIC("removeNamedItem", 1, removeNamedItem, AttributeLookup_ByName),
    PR_JS_MAGIC("removeNamedItemNS", 2, removeNamedItem, AttributeLookup_ByNamespace),
};

constexpr JSCFunctionListEntry kCharacterDataProto[] = {
    PR_JS_TAG("CharacterData"),
    PR_JS_GET_SET("data", (jsStringGetter<&prJsDomCharacterDataClass, prDomCharacterDataData>),
        (jsStringSetter<&prJsDomCharacterDataClass, prDomCharacterDataSetData, true>)),
    PR_JS_GET("length", (jsNumberGetter<&prJsDomCharacterDataClass, prDomCharacterDataLength>)),
    PR_JS_MAGIC("substringData", 2, dataMethod, DataMethod_Substring),
    PR_JS_METHOD("appendData", 1, appendData),
    PR_JS_MAGIC("insertData", 2, dataMethod, DataMethod_Insert),
    PR_JS_MAGIC("deleteData", 2, dataMethod, DataMethod_Delete),
    PR_JS_MAGIC("replaceData", 3, dataMethod, DataMethod_Replace),
    PR_CHILD_NODE,
    PR_NON_DOCUMENT_TYPE_CHILD_NODE,
};

constexpr JSCFunctionListEntry kTextProto[] = {
    PR_JS_TAG("Text"),
    PR_JS_METHOD("splitText", 1, splitText),
    PR_JS_GET("wholeText", (jsStringGetter<&prJsDomTextClass, prDomTextWholeText>)),
    PR_JS_GET("assignedSlot", assignedSlot),
};

constexpr JSCFunctionListEntry kCommentProto[] = {PR_JS_TAG("Comment")};

constexpr JSCFunctionListEntry kProcessingInstructionProto[] = {
    PR_JS_TAG("ProcessingInstruction"),
    PR_JS_GET("target", (jsStringGetter<&prJsDomProcessingInstructionClass, prDomProcessingInstructionTarget>)),
};

constexpr JSCFunctionListEntry kHtmlCollectionProto[] = {
    PR_JS_TAG("HTMLCollection"),
    PR_JS_GET("length", (jsNumberGetter<&prJsDomHtmlCollectionClass, prDomHtmlCollectionLength>)),
    PR_JS_METHOD("item", 1, (jsItemMethod<&prJsDomHtmlCollectionClass, &kCollectionIndexed>)),
    PR_JS_METHOD("namedItem", 1, namedItem),
};

constexpr JSCFunctionListEntry kNodeListProto[] = {
    PR_JS_TAG("NodeList"),
    PR_JS_GET("length", (jsNumberGetter<&prJsDomNodeListClass, prDomNodeListLength>)),
    PR_JS_METHOD("item", 1, (jsItemMethod<&prJsDomNodeListClass, &kListIndexed>)),
};

constexpr JSCFunctionListEntry kAbstractRangeProto[] = {
    PR_JS_TAG("AbstractRange"),
    PR_JS_MAGIC_GET("startContainer", rangeBoundary, 0),
    PR_JS_MAGIC_GET("startOffset", rangeBoundary, 1),
    PR_JS_MAGIC_GET("endContainer", rangeBoundary, 2),
    PR_JS_MAGIC_GET("endOffset", rangeBoundary, 3),
    PR_JS_MAGIC_GET("collapsed", rangeBoundary, 4),
};

constexpr JSCFunctionListEntry kStaticRangeProto[] = {PR_JS_TAG("StaticRange")};

constexpr JSCFunctionListEntry kRangeProto[] = {
    PR_JS_TAG("Range"),
    PR_JS_GET("commonAncestorContainer", rangeCommonAncestor),
    PR_JS_MAGIC("setStart", 2, rangeSet, 0),
    PR_JS_MAGIC("setEnd", 2, rangeSet, 1),
    PR_JS_MAGIC("setStartBefore", 1, rangeNodeMethod, PrDomRangeSide_StartBefore),
    PR_JS_MAGIC("setStartAfter", 1, rangeNodeMethod, PrDomRangeSide_StartAfter),
    PR_JS_MAGIC("setEndBefore", 1, rangeNodeMethod, PrDomRangeSide_EndBefore),
    PR_JS_MAGIC("setEndAfter", 1, rangeNodeMethod, PrDomRangeSide_EndAfter),
    PR_JS_METHOD("collapse", 0, rangeCollapse),
    PR_JS_MAGIC("selectNode", 1, rangeNodeMethod, 4),
    PR_JS_MAGIC("selectNodeContents", 1, rangeNodeMethod, 5),
    PR_JS_METHOD("compareBoundaryPoints", 2, rangeCompareBoundaryPoints),
    PR_JS_MAGIC("deleteContents", 0, rangeContents, 0),
    PR_JS_MAGIC("extractContents", 0, rangeContents, 1),
    PR_JS_MAGIC("cloneContents", 0, rangeContents, 2),
    PR_JS_MAGIC("insertNode", 1, rangeNodeMethod, 6),
    PR_JS_MAGIC("surroundContents", 1, rangeNodeMethod, 7),
    PR_JS_MAGIC("cloneRange", 0, rangeContents, 3),
    PR_JS_MAGIC("detach", 0, rangeContents, 4),
    PR_JS_MAGIC("isPointInRange", 2, rangePoint, 0),
    PR_JS_MAGIC("comparePoint", 2, rangePoint, 1),
    PR_JS_METHOD("intersectsNode", 1, rangeIntersectsNode),
    PR_JS_MAGIC("toString", 0, rangeContents, 5),
};

constexpr JSCFunctionListEntry kSelectionProto[] = {
    PR_JS_TAG("Selection"),
    PR_JS_MAGIC_GET("anchorNode", selectionAttribute, 0),
    PR_JS_MAGIC_GET("anchorOffset", selectionAttribute, 1),
    PR_JS_MAGIC_GET("focusNode", selectionAttribute, 2),
    PR_JS_MAGIC_GET("focusOffset", selectionAttribute, 3),
    PR_JS_MAGIC_GET("isCollapsed", selectionAttribute, 4),
    PR_JS_MAGIC_GET("rangeCount", selectionAttribute, 5),
    PR_JS_MAGIC_GET("type", selectionAttribute, 6),
    PR_JS_MAGIC_GET("direction", selectionAttribute, 7),
    PR_JS_METHOD("getRangeAt", 1, selectionGetRangeAt),
    PR_JS_MAGIC("addRange", 1, selectionRange, 0),
    PR_JS_MAGIC("removeRange", 1, selectionRange, 1),
    PR_JS_MAGIC("removeAllRanges", 0, selectionAction, 0),
    PR_JS_MAGIC("empty", 0, selectionAction, 0),
    PR_JS_METHOD("getComposedRanges", 0, selectionGetComposedRanges),
    PR_JS_MAGIC("collapse", 1, selectionPoint, 0),
    PR_JS_MAGIC("setPosition", 1, selectionPoint, 0),
    PR_JS_MAGIC("collapseToStart", 0, selectionAction, 1),
    PR_JS_MAGIC("collapseToEnd", 0, selectionAction, 2),
    PR_JS_MAGIC("extend", 1, selectionPoint, 1),
    PR_JS_METHOD("setBaseAndExtent", 4, selectionSetBaseAndExtent),
    PR_JS_METHOD("selectAllChildren", 1, selectionSelectAllChildren),
    PR_JS_METHOD("modify", 0, selectionModify),
    PR_JS_MAGIC("deleteFromDocument", 0, selectionAction, 3),
    PR_JS_METHOD("containsNode", 1, selectionContainsNode),
    PR_JS_METHOD("toString", 0, selectionToString),
};

constexpr JSCFunctionListEntry kMutationObserverProto[] = {
    PR_JS_TAG("MutationObserver"),
    PR_JS_METHOD("observe", 1, observe),
    PR_JS_METHOD("disconnect", 0, disconnect),
    PR_JS_METHOD("takeRecords", 0, observerTakeRecords),
};

constexpr JSCFunctionListEntry kMutationRecordProto[] = {
    PR_JS_TAG("MutationRecord"),
    PR_JS_GET("type", recordType),
    PR_JS_GET("target", (jsObjectGetter<&domMutationRecordClass, prDomMutationRecordTarget, &prJsDomNodeClass>)),
    PR_JS_MAGIC_GET("addedNodes", recordNodes, 0),
    PR_JS_MAGIC_GET("removedNodes", recordNodes, 1),
    PR_JS_GET("previousSibling", (jsObjectGetter<&domMutationRecordClass, prDomMutationRecordPreviousSibling,
        &prJsDomNodeClass>)),
    PR_JS_GET("nextSibling",
        (jsObjectGetter<&domMutationRecordClass, prDomMutationRecordNextSibling, &prJsDomNodeClass>)),
    PR_JS_GET("attributeName", (jsStringGetter<&domMutationRecordClass, prDomMutationRecordAttributeName>)),
    PR_JS_GET("attributeNamespace", (jsStringGetter<&domMutationRecordClass, prDomMutationRecordAttributeNamespace>)),
    PR_JS_GET("oldValue", (jsStringGetter<&domMutationRecordClass, prDomMutationRecordOldValue>)),
};

constexpr JSCFunctionListEntry kTokenListProto[] = {
    PR_JS_TAG("DOMTokenList"),
    PR_JS_GET("length", (jsNumberGetter<&prJsDomTokenListClass, prDomTokenListLength>)),
    PR_JS_METHOD("item", 1, (jsItemMethod<&prJsDomTokenListClass, &kTokenListIndexed>)),
    PR_JS_METHOD("contains", 1, tokenContains),
    PR_JS_MAGIC("add", 0, tokenChange, 0),
    PR_JS_MAGIC("remove", 0, tokenChange, 1),
    PR_JS_METHOD("toggle", 1, tokenToggle),
    PR_JS_METHOD("replace", 2, tokenReplace),
    PR_JS_METHOD("supports", 1, tokenSupports),
    PR_JS_GET_SET("value", (jsStringGetter<&prJsDomTokenListClass, prDomTokenListValue>),
        (jsStringSetter<&prJsDomTokenListClass, prDomTokenListSetValue>)),
    PR_JS_METHOD("toString", 0, tokenToString),
};

#undef PR_PARENT_NODE
#undef PR_CHILD_NODE
#undef PR_NON_DOCUMENT_TYPE_CHILD_NODE

// The module's classes, each after its base.
constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("Event", prJsDomEventClass, jsViewFinalizer<&prJsDomEventClass>, kEventProto),
        PR_JS_CONSTANTS(kEventConstants),
        .constructor = eventConstruct,
        .length = 1,
        .references = &kEventReferences,
        .unforgeable = kEventUnforgeable,
        .unforgeableCount = int(countOf(kEventUnforgeable)),
    },
    {
        PR_JS_CLASS("CustomEvent", prJsDomCustomEventClass, jsViewFinalizer<&prJsDomCustomEventClass>,
            kCustomEventProto),
        .constructor = customEventConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("UIEvent", prJsDomUiEventClass, jsViewFinalizer<&prJsDomUiEventClass>, kUiEventProto),
        .constructor = uiEventConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("FocusEvent", prJsDomFocusEventClass, jsViewFinalizer<&prJsDomFocusEventClass>, kFocusEventProto),
        .constructor = focusEventConstruct,
        .length = 1,
        .base = &prJsDomUiEventClass,
    },
    {
        PR_JS_CLASS("MouseEvent", prJsDomMouseEventClass, jsViewFinalizer<&prJsDomMouseEventClass>, kMouseEventProto),
        .constructor = mouseEventConstruct,
        .length = 1,
        .base = &prJsDomUiEventClass,
    },
    {
        PR_JS_CLASS("WheelEvent", prJsDomWheelEventClass, jsViewFinalizer<&prJsDomWheelEventClass>, kWheelEventProto),
        PR_JS_CONSTANTS(kWheelEventConstants),
        .constructor = wheelEventConstruct,
        .length = 1,
        .base = &prJsDomMouseEventClass,
    },
    {
        PR_JS_CLASS("KeyboardEvent", prJsDomKeyboardEventClass, jsViewFinalizer<&prJsDomKeyboardEventClass>,
            kKeyboardEventProto),
        PR_JS_CONSTANTS(kKeyboardEventConstants),
        .constructor = keyboardEventConstruct,
        .length = 1,
        .base = &prJsDomUiEventClass,
    },
    {
        PR_JS_CLASS("CompositionEvent", prJsDomCompositionEventClass, jsViewFinalizer<&prJsDomCompositionEventClass>,
            kCompositionEventProto),
        .constructor = compositionEventConstruct,
        .length = 1,
        .base = &prJsDomUiEventClass,
    },
    {
        PR_JS_CLASS("EventTarget", prJsDomEventTargetClass,
            (jsFinalizer<&prJsDomEventTargetClass, prDomEventTargetRelease>), kEventTargetProto),
        .constructor = targetConstruct,
        .slot = [](void* handle) {
            return prDomEventTargetBinding(static_cast<PrDomEventTarget*>(handle));
        },
    },
    {
        PR_JS_CLASS("AbortController", domAbortControllerClass,
            (jsFinalizer<&domAbortControllerClass, prDomAbortControllerDestroy>), kAbortControllerProto),
        .constructor = controllerConstruct,
    },
    {
        PR_JS_CLASS("AbortSignal", prJsDomAbortSignalClass, abortSignalFinalize, kAbortSignalProto),
        .statics = kAbortSignalStatic,
        .staticCount = int(countOf(kAbortSignalStatic)),
        .base = &prJsDomEventTargetClass,
        .references = &kAbortSignalReferences,
        .wrapped = abortSignalWrapped,
    },
    {
        PR_JS_CLASS("Node", prJsDomNodeClass, jsViewFinalizer<&prJsDomNodeClass>, kNodeProto),
        PR_JS_CONSTANTS(kNodeConstants),
        .base = &prJsDomEventTargetClass,
        .references = &kNodeReferences,
        .wrapped = nodeWrapped,
    },
    {
        PR_JS_CLASS("Document", prJsDomDocumentClass, documentFinalizer, kDocumentProto),
        .constructor = documentConstruct,
        .base = &prJsDomNodeClass,
    },
    {
        PR_JS_CLASS("DocumentType", domDocumentTypeClass, jsViewFinalizer<&domDocumentTypeClass>, kDocumentTypeProto),
        .base = &prJsDomNodeClass,
    },
    {
        PR_JS_CLASS("DocumentFragment", prJsDomDocumentFragmentClass,
            jsViewFinalizer<&prJsDomDocumentFragmentClass>, kDocumentFragmentProto),
        .constructor = fragmentConstruct,
        .base = &prJsDomNodeClass,
    },
    {
        PR_JS_CLASS("ShadowRoot", prJsDomShadowRootClass, jsViewFinalizer<&prJsDomShadowRootClass>, kShadowRootProto),
        .base = &prJsDomDocumentFragmentClass,
    },
    {
        PR_JS_CLASS("Element", prJsDomElementClass, jsViewFinalizer<&prJsDomElementClass>, kElementProto),
        .base = &prJsDomNodeClass,
    },
    {PR_JS_CLASS("Attr", domAttrClass, jsViewFinalizer<&domAttrClass>, kAttrProto), .base = &prJsDomNodeClass},
    {
        PR_JS_CLASS("NamedNodeMap", domNamedNodeMapClass, jsViewFinalizer<&domNamedNodeMapClass>, kNamedNodeMapProto),
        .indexed = &kAttributeMapIndexed,
        .named = &kAttributeMapNamed,
    },
    {
        PR_JS_CLASS("CharacterData", prJsDomCharacterDataClass, jsViewFinalizer<&prJsDomCharacterDataClass>,
            kCharacterDataProto),
        .base = &prJsDomNodeClass,
    },
    {
        PR_JS_CLASS("Text", prJsDomTextClass, jsViewFinalizer<&prJsDomTextClass>, kTextProto),
        .constructor = textConstruct,
        .base = &prJsDomCharacterDataClass,
    },
    {
        PR_JS_CLASS("Comment", prJsDomCommentClass, jsViewFinalizer<&prJsDomCommentClass>, kCommentProto),
        .constructor = commentConstruct,
        .base = &prJsDomCharacterDataClass,
    },
    {
        PR_JS_CLASS("ProcessingInstruction", prJsDomProcessingInstructionClass,
            jsViewFinalizer<&prJsDomProcessingInstructionClass>, kProcessingInstructionProto),
        .base = &prJsDomCharacterDataClass,
    },
    {
        PR_JS_CLASS("HTMLCollection", prJsDomHtmlCollectionClass,
            (jsFinalizer<&prJsDomHtmlCollectionClass, prDomHtmlCollectionDestroy>), kHtmlCollectionProto),
        .indexed = &kCollectionIndexed,
        .named = &kCollectionNamed,
    },
    {
        PR_JS_CLASS("NodeList", prJsDomNodeListClass, (jsFinalizer<&prJsDomNodeListClass, prDomNodeListDestroy>),
            kNodeListProto),
        .indexed = &kListIndexed,
        .iterable = true,
    },
    {
        PR_JS_CLASS("DOMImplementation", domImplementationClass,
            jsViewFinalizer<&domImplementationClass>, kImplementationProto)
    },
    {
        PR_JS_CLASS("AbstractRange", prJsDomAbstractRangeClass,
            (jsFinalizer<&prJsDomAbstractRangeClass, prDomRangeRelease>), kAbstractRangeProto)
    },
    {
        PR_JS_CLASS("StaticRange", domStaticRangeClass,
            (jsFinalizer<&domStaticRangeClass, prDomRangeRelease>), kStaticRangeProto),
        .constructor = staticRangeConstruct,
        .length = 1,
        .base = &prJsDomAbstractRangeClass,
    },
    {
        PR_JS_CLASS("Range", prJsDomRangeClass, (jsFinalizer<&prJsDomRangeClass, prDomRangeRelease>), kRangeProto),
        PR_JS_CONSTANTS(kRangeConstants),
        .constructor = rangeConstruct,
        .base = &prJsDomAbstractRangeClass,
    },
    {PR_JS_CLASS("Selection", prJsDomSelectionClass, jsViewFinalizer<&prJsDomSelectionClass>, kSelectionProto)},
    {
        PR_JS_CLASS("MutationObserver", domMutationObserverClass,
            (jsFinalizer<&domMutationObserverClass, prDomMutationObserverRelease>), kMutationObserverProto),
        .constructor = observerConstruct,
        .length = 1,
    },
    {
        PR_JS_CLASS("MutationRecord", domMutationRecordClass,
            (jsFinalizer<&domMutationRecordClass, prDomMutationRecordDestroy>), kMutationRecordProto),
    },
    {
        PR_JS_CLASS("DOMTokenList", prJsDomTokenListClass,
            (jsFinalizer<&prJsDomTokenListClass, prDomTokenListDestroy>), kTokenListProto),
        .indexed = &kTokenListIndexed,
        .iterable = true,
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsDomEventClass;
JSClassID prJsDomCustomEventClass;
JSClassID prJsDomUiEventClass;
JSClassID prJsDomFocusEventClass;
JSClassID prJsDomMouseEventClass;
JSClassID prJsDomWheelEventClass;
JSClassID prJsDomKeyboardEventClass;
JSClassID prJsDomCompositionEventClass;
JSClassID prJsDomEventTargetClass;
JSClassID prJsDomAbortSignalClass;
JSClassID prJsDomNodeClass;
JSClassID prJsDomDocumentClass;
JSClassID prJsDomDocumentFragmentClass;
JSClassID prJsDomShadowRootClass;
JSClassID prJsDomElementClass;
JSClassID prJsDomCharacterDataClass;
JSClassID prJsDomTextClass;
JSClassID prJsDomCommentClass;
JSClassID prJsDomProcessingInstructionClass;
JSClassID prJsDomHtmlCollectionClass;
JSClassID prJsDomNodeListClass;
JSClassID prJsDomTokenListClass;
JSClassID prJsDomAbstractRangeClass;
JSClassID prJsDomRangeClass;
JSClassID prJsDomSelectionClass;

void prJsDomBind(JSContext* ctx) {
    PR_ASSERT(jsIndexAtomsTagged(ctx));
    jsWebidlBind(ctx);
    prJsDomGeometryBind(ctx);
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    JSRuntime* runtime = JS_GetRuntime(ctx);
    jsAddClassifier(runtime, prJsDomEventTargetClass, classifyEventTarget);
    jsAddClassifier(runtime, prJsDomNodeClass, classifyNode);
    jsAddClassifier(runtime, prJsDomEventClass, classifyEvent);
}

void prJsDomSetGlobalDocument(JSRuntime* runtime, const PrJsDomGlobalDocument* source) {
    jsSetRuntimeData(runtime, &kGlobalDocumentKey, source);
}

void prJsDomSetAgent(JSRuntime* runtime, PrDomAgent* PR_NULLABLE agent) {
    jsSetRuntimeData(runtime, &kAgentKey, agent);
    jsSetMemberScope(runtime, agent != nullptr ? &kReactions : nullptr, agent);
}

PrDomAgent* PR_NULLABLE prJsDomAgent(JSRuntime* runtime) {
    return static_cast<PrDomAgent*>(const_cast<void*>(jsRuntimeData(runtime, &kAgentKey)));
}

PrDomDocument* PR_NULLABLE prJsDomGlobalDocument(JSContext* ctx) {
    return globalDocument(ctx);
}

JSValue prJsDomAbortSignalReason(JSContext* ctx, JSValueConst signal) {
    PrDomAbortSignal* handle = jsHandleAs<PrDomAbortSignal>(ctx, signal, prJsDomAbortSignalClass);
    if (handle == nullptr || !prDomAbortSignalAborted(handle))
        return JS_UNDEFINED;
    return jsReference(ctx, signal, kAbortReasonSlot);
}

JSValue prJsDomWrapEventTarget(JSContext* ctx, JSValueConst owner, PrDomEventTarget* PR_NULLABLE target) {
    if (target != nullptr && jsGlobalHandleIn(ctx, prJsDomEventTargetClass) == target)
        return JS_GetGlobalObject(ctx);
    return jsWrapAs(ctx, prJsDomEventTargetClass, owner, target, false);
}

JSValue prJsDomWrapDocument(JSContext* ctx, PrDomDocument* document) {
    return jsWrapAs(ctx, prJsDomNodeClass, JS_NULL, prDomDocumentAsNode(document), false);
}

JSValue prJsDomWrapRange(JSContext* ctx, JSValueConst owner, PrDomRange* range) {
    return rangeToJs(ctx, owner, range);
}

bool prJsDomEventInitRead(JSContext* ctx, JSValueConst dictionary, PrDomEventInit* out) {
    return jsDictBool(ctx, dictionary, "bubbles", &out->bubbles) &&
        jsDictBool(ctx, dictionary, "cancelable", &out->cancelable) &&
        jsDictBool(ctx, dictionary, "composed", &out->composed);
}

// Calls a listener with the event, with its current target as the receiver.
void prJsDomInvokeListener(void* PR_NULLABLE context, PrDomEvent* event) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue args[] = {jsWrapAs(ctx, prJsDomEventClass, JS_NULL, event, false)};
    holdEventDocuments(ctx, args[0], event);
    JSValue receiver = prJsDomWrapEventTarget(ctx, JS_NULL, prDomEventCurrentTarget(event));
    JSValue result;
    if (JS_IsException(receiver) || JS_IsException(args[0]))
        result = JS_EXCEPTION;
    else
        result = jsCallbackCall(callback, "handleEvent", receiver, 1, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, receiver);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
}

bool prJsDomEventHandlerCallback(JSContext* ctx, JSValueConst function, JSValueConst owner, PrDomEventHandlerCallback* out) {
    JsCallback* callback = jsCallbackCreate(ctx, function, owner);
    if (callback == nullptr)
        return false;
    *out = PrDomEventHandlerCallback{invokeHandler, callback, jsCallbackRetain, jsCallbackRelease};
    return true;
}

// The getter returns the callback that a script set, or null for any other value.
JSValue prJsDomEventHandler(JSContext* ctx, JSValueConst targetObject, int magic) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, targetObject, prJsDomEventTargetClass);
    if (target == nullptr)
        return JS_EXCEPTION;
    PrDomEventHandlerCallback handler = prDomEventTargetEventHandler(target, PrDomEventHandler(magic));
    if (handler.invoke != invokeHandler)
        return JS_NULL;
    return JS_DupValue(ctx, static_cast<JsCallback*>(handler.context)->value);
}

// An EventHandler is [LegacyTreatNonObjectAsNull] (webidl/#LegacyTreatNonObjectAsNull), so the setter treats any object
// as a callback, and anything else as null.
JSValue prJsDomSetEventHandler(JSContext* ctx, JSValueConst targetObject, JSValueConst input, int magic) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, targetObject, prJsDomEventTargetClass);
    if (target == nullptr)
        return JS_EXCEPTION;
    if (!JS_IsObject(input)) {
        prDomEventTargetSetEventHandler(target, PrDomEventHandler(magic), nullptr);
        return JS_UNDEFINED;
    }
    PrDomEventHandlerCallback handler;
    if (!prJsDomEventHandlerCallback(ctx, input, targetObject, &handler))
        return JS_EXCEPTION;
    prDomEventTargetSetEventHandler(target, PrDomEventHandler(magic), &handler);
    jsCallbackRelease(handler.context);
    return JS_UNDEFINED;
}

bool prJsDomInsertPositionRead(JSContext* ctx, JSValueConst value, PrDomInsertPosition* out) {
    constexpr PrStringView kPositions[] = {"beforebegin", "afterbegin", "beforeend", "afterend"};
    StringArgument name = {ctx};
    if (!name.read(value))
        return false;
    for (uint32_t i = 0; i < countOf(kPositions); ++i) {
        if (asciiEqualsFold(name.value(), kPositions[i])) {
            *out = PrDomInsertPosition(i);
            return true;
        }
    }
    jsDomExceptionThrow(ctx, "SyntaxError");
    return false;
}

JSValue prJsDomThrow(JSContext* ctx, PrStatus status) {
    switch (status) {
        case PrStatus_NotAllowed: return jsDomExceptionThrow(ctx, "HierarchyRequestError");
        case PrStatus_NotFound: return jsDomExceptionThrow(ctx, "NotFoundError");
        case PrStatus_InvalidData: return jsDomExceptionThrow(ctx, "InvalidCharacterError");
        case PrStatus_Syntax: return jsDomExceptionThrow(ctx, "SyntaxError");
        case PrStatus_OutOfRange: return jsDomExceptionThrow(ctx, "IndexSizeError");
        case PrStatus_InvalidState: return jsDomExceptionThrow(ctx, "InvalidStateError");
        case PrStatus_NotSupported: return jsDomExceptionThrow(ctx, "NotSupportedError");
        case PrStatus_WrongType: return jsDomExceptionThrow(ctx, "InvalidNodeTypeError");
        case PrStatus_Foreign: return jsDomExceptionThrow(ctx, "WrongDocumentError");
        default: PR_FATAL("a DOM call answered a status the binding does not map");
    }
}
