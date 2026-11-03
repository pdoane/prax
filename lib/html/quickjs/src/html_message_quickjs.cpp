// JavaScript binding of channel messaging (html/#message-channels).

#include "html/quickjs/src/html_message_quickjs.h"

#include "pr/alloc.h"
#include "pr/dom_quickjs.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"
#include "html/quickjs/src/html_clone_quickjs.h"

namespace pr_impl {

JSClassID htmlMessagePortClass;

namespace {

JSClassID htmlMessageChannelClass;

// The slot in which a port's object holds the object of its peer. The collector traces the pair as one, so that a
// listener on either port lasts as long as script can reach the other, and an unreachable pair is freed.
constexpr uint32_t kPeerSlot = 0;

// Pins a port's object while a message waits for its listeners.
void portPending(void* PR_NULLABLE context, PrHtmlMessagePort* port, bool pending) {
    jsPin(static_cast<JSRuntime*>(context), prJsDomEventTargetClass, prHtmlMessagePortAsEventTarget(port), pending);
}

constexpr JsReferences kPortReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    true,
};

void releasePort(PrHtmlMessagePort* PR_NULLABLE port) {
    prDomEventTargetRelease(prHtmlMessagePortAsEventTarget(port));
}

// postMessage(message, transfer) and postMessage(message, options) (html/#dom-messageport-postmessage).
JSValue portPostMessage(JSContext* ctx, JSValueConst portObject, int argc, JSValueConst* argv) {
    PrHtmlMessagePort* port = jsThis<PrHtmlMessagePort>(ctx, portObject, htmlMessagePortClass);
    if (port == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    Array<JSValue> transfer;
    bool ok = true;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue iterator = jsIteratorMethod(ctx, argv[1]);
        ok = !JS_IsException(iterator);
        if (ok && !JS_IsUndefined(iterator) && !JS_IsNull(iterator)) {
            ok = jsHtmlTransferRead(ctx, argv[1], &transfer);
        } else if (ok) {
            ok = jsHtmlTransferOption(ctx, argv[1], &transfer);
        }
        JS_FreeValue(ctx, iterator);
    } else if (argc > 1 && !JS_IsUndefined(argv[1])) {
        ok = false;
        JS_ThrowTypeError(ctx, "The second argument is not an object");
    }
    JsHtmlSerialized message{};
    ok = ok && jsHtmlSerialize(ctx, argv[0], &transfer, port, &message);
    jsHtmlTransferReset(ctx, &transfer);
    if (!ok)
        return JS_EXCEPTION;
    prHtmlMessagePortPostMessage(port, prStringView(&message.bytes), message.ports.data, message.ports.count);
    prStringReset(&message.bytes);
    message.ports.reset();
    return JS_UNDEFINED;
}

JSValue portStart(JSContext* ctx, JSValueConst portObject, int, JSValueConst*) {
    PrHtmlMessagePort* port = jsThis<PrHtmlMessagePort>(ctx, portObject, htmlMessagePortClass);
    if (port == nullptr)
        return JS_EXCEPTION;
    prHtmlMessagePortStart(port);
    return JS_UNDEFINED;
}

JSValue portClose(JSContext* ctx, JSValueConst portObject, int, JSValueConst*) {
    PrHtmlMessagePort* port = jsThis<PrHtmlMessagePort>(ctx, portObject, htmlMessagePortClass);
    if (port == nullptr)
        return JS_EXCEPTION;
    prHtmlMessagePortClose(port);
    return JS_UNDEFINED;
}

// Setting onmessage starts the port (html/#handler-messageport-onmessage).
JSValue portSetOnMessage(JSContext* ctx, JSValueConst resultObject, JSValueConst input, int magic) {
    JSValue result = prJsDomSetEventHandler(ctx, resultObject, input, magic);
    PrHtmlMessagePort* port = jsThis<PrHtmlMessagePort>(ctx, resultObject, htmlMessagePortClass);
    if (!JS_IsException(result) && port != nullptr)
        prHtmlMessagePortStart(port);
    return result;
}

constexpr JSCFunctionListEntry kPortProto[] = {
    PR_JS_TAG("MessagePort"),
    PR_JS_METHOD("postMessage", 1, portPostMessage),
    PR_JS_METHOD("start", 0, portStart),
    PR_JS_METHOD("close", 0, portClose),
    PR_JS_MAGIC_GET_SET("onmessage", prJsDomEventHandler, portSetOnMessage, PrDomEventHandler_onmessage),
    PR_JS_DOM_EVENT_HANDLER(onmessageerror),
};

// The slots of a channel's object, which hold its ports' objects.
enum {
    ChannelSlot_Port1,
    ChannelSlot_Port2,
};
typedef uint8_t ChannelSlot;

JSValue portToJs(JSContext* ctx, PrHtmlMessagePort* port) {
    JSValue object = jsWrap(ctx, htmlMessagePortClass, JS_UNDEFINED, prHtmlMessagePortAsEventTarget(port), true);
    if (JS_IsException(object))
        releasePort(port);
    return object;
}

// A channel's object has no native value of its own, so its handle only marks it as a channel.
JSValue channelConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "MessageChannel needs a window");
    PrHtmlMessagePort* first = nullptr;
    PrHtmlMessagePort* second = nullptr;
    prHtmlMessagePortCreatePair(window, portPending, JS_GetRuntime(ctx), &first, &second);
    JSValue firstObject = portToJs(ctx, first);
    JSValue secondObject = portToJs(ctx, second);
    JSValue channel = jsWrap(ctx, htmlMessageChannelClass, JS_UNDEFINED, allocZeroed<uint8_t>(), true);
    if (JS_IsException(firstObject) || JS_IsException(secondObject) || JS_IsException(channel)) {
        JS_FreeValue(ctx, firstObject);
        JS_FreeValue(ctx, secondObject);
        JS_FreeValue(ctx, channel);
        return JS_EXCEPTION;
    }
    jsSetReference(ctx, firstObject, kPeerSlot, secondObject);
    jsSetReference(ctx, secondObject, kPeerSlot, firstObject);
    jsSetReference(ctx, channel, ChannelSlot_Port1, firstObject);
    jsSetReference(ctx, channel, ChannelSlot_Port2, secondObject);
    JS_FreeValue(ctx, firstObject);
    JS_FreeValue(ctx, secondObject);
    return jsConstructed(ctx, newTarget, channel);
}

// port1 and port2, selected by the magic.
JSValue channelPort(JSContext* ctx, JSValueConst object, int magic) {
    if (jsThis<uint8_t>(ctx, object, htmlMessageChannelClass) == nullptr)
        return JS_EXCEPTION;
    return jsReference(ctx, object, uint32_t(magic));
}

void freeChannel(uint8_t* PR_NULLABLE handle) {
    prMemoryFree(handle);
}

constexpr JSCFunctionListEntry kChannelProto[] = {
    PR_JS_TAG("MessageChannel"),
    PR_JS_MAGIC_GET("port1", channelPort, ChannelSlot_Port1),
    PR_JS_MAGIC_GET("port2", channelPort, ChannelSlot_Port2),
};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("MessagePort", htmlMessagePortClass, (jsFinalizer<&htmlMessagePortClass, releasePort>), kPortProto),
        .base = &prJsDomEventTargetClass,
        .references = &kPortReferences,
    },
    {
        PR_JS_CLASS("MessageChannel", htmlMessageChannelClass, (jsFinalizer<&htmlMessageChannelClass, freeChannel>),
            kChannelProto),
        .constructor = channelConstruct,
    },
};

} // namespace

PrHtmlMessagePort* PR_NULLABLE jsHtmlMessagePortOf(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrHtmlMessagePort>(ctx, value, htmlMessagePortClass);
}

JSValue jsHtmlMessagePortsAdopt(JSContext* ctx, PrHtmlMessagePort* PR_NONNULL const* PR_NULLABLE ports, uint32_t count,
        PrHtmlWindow* window) {
    JSValue objects = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        PrHtmlMessagePort* port = ports[i];
        if (!prHtmlMessagePortHasWindow(port))
            prHtmlMessagePortAdopt(port, window, portPending, JS_GetRuntime(ctx));
        JSValue object = jsWrap(ctx, htmlMessagePortClass, JS_UNDEFINED, prHtmlMessagePortAsEventTarget(port), false);
        PrHtmlMessagePort* peer = prHtmlMessagePortPeer(port);
        if (peer != nullptr && prHtmlMessagePortHasWindow(peer)) {
            JSValue peerObject = jsWrap(ctx, htmlMessagePortClass, JS_UNDEFINED, prHtmlMessagePortAsEventTarget(peer), false);
            jsSetReference(ctx, object, kPeerSlot, peerObject);
            jsSetReference(ctx, peerObject, kPeerSlot, object);
            JS_FreeValue(ctx, peerObject);
        }
        JS_SetPropertyUint32(ctx, objects, i, object);
    }
    JS_FreezeObject(ctx, objects);
    return objects;
}

void jsHtmlMessageBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

} // namespace pr_impl
