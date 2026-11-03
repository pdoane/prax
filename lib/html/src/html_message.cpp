// MessagePort: one end of a channel between scripts of an agent (html/#message-ports).

#include "html/src/html_message.h"

#include "pr/alloc.h"
#include "pr/memory.h"

#include "html/src/html_interface.h"
#include "html/src/html_window.h"

namespace pr_impl {

namespace {

void portRetain(PrHtmlMessagePort* port) {
    prDomEventTargetRetain(prHtmlMessagePortAsEventTarget(port));
}

void portRelease(PrHtmlMessagePort* port) {
    prDomEventTargetRelease(prHtmlMessagePortAsEventTarget(port));
}

// Ends a task's hold on its port, and reports the last scheduled delivery.
void deliveryEnded(PrHtmlMessagePort* port) {
    if (--port->scheduled == 0 && port->pending != nullptr)
        port->pending(port->context, port, false);
    portRelease(port);
}

void dropDelivery(void* data) {
    deliveryEnded(static_cast<PrHtmlMessagePort*>(data));
}

// html/#message-port-post-message-steps, in the task that those steps queue. Fires a message event at the port with the
// first message of its queue. The queue is empty if the port was transferred after the task was queued.
void deliver(void* data) {
    PrHtmlMessagePort* port = static_cast<PrHtmlMessagePort*>(data);
    if (port->queueHead != port->queue.count) {
        HtmlMessage message = port->queue.data[port->queueHead++];
        if (port->queueHead == port->queue.count) {
            port->queue.count = 0;
            port->queueHead = 0;
        }
        htmlFireMessage(prHtmlMessagePortAsEventTarget(port), &message, prRawStringEmpty(), nullptr);
    }
    deliveryEnded(port);
}

void scheduleDelivery(PrHtmlMessagePort* port) {
    portRetain(port);
    if (port->scheduled++ == 0 && port->pending != nullptr)
        port->pending(port->context, port, true);
    htmlWindowQueueTask(port->window, deliver, port, dropDelivery);
}

void disentangle(PrHtmlMessagePort* port) {
    if (port->peer != nullptr)
        port->peer->peer = nullptr;
    port->peer = nullptr;
}

void destroyPort(DomCountedTarget* target) {
    PrHtmlMessagePort* port = static_cast<PrHtmlMessagePort*>(target);
    disentangle(port);
    for (uint32_t i = port->queueHead; i < port->queue.count; ++i)
        htmlMessageReset(&port->queue.data[i]);
    port->queue.reset();
    domListenersDestroy(port->listeners);
    if (port->window != nullptr)
        prDomEventTargetRelease(prHtmlWindowAsEventTarget(port->window));
    prMemoryFree(port);
}

PrHtmlMessagePort* createPort(PrHtmlWindow* PR_NULLABLE window, PrHtmlMessagePortPendingFn PR_NULLABLE pending,
        void* PR_NULLABLE context) {
    PrHtmlMessagePort* port = allocZeroed<PrHtmlMessagePort>();
    port->type = DomTargetType_MessagePort;
    port->destroy = destroyPort;
    port->references = 1;
    port->window = window;
    port->pending = pending;
    port->context = context;
    if (window != nullptr)
        prDomEventTargetRetain(prHtmlWindowAsEventTarget(window));
    return port;
}

} // namespace

void htmlMessageReset(HtmlMessage* message) {
    prRawStringReset(&message->data);
    for (uint32_t i = 0; i < message->ports.count; ++i)
        domEventTargetRelease(message->ports.data[i]);
    message->ports.reset();
}

void htmlFireMessage(PrDomEventTarget* target, HtmlMessage* message, PrRawString origin, PrHtmlWindow* PR_NULLABLE source) {
    PrHtmlMessageEvent* event =
        domEventCreate<PrHtmlMessageEvent>("message", prDomEventInitDefault(), PrDomEventKind_Message);
    event->trusted = true;
    event->message = allocZeroed<DomMessageEventData>();
    event->message->data = message->data;
    event->message->origin = origin;
    event->message->lastEventId = prRawStringEmpty();
    event->message->source = source;
    event->message->ports = message->ports;
    *message = HtmlMessage{};
    domEventDispatch(target, event);
    domEventRelease(event);
}

} // namespace pr_impl

using namespace pr_impl;

void prHtmlMessagePortCreatePair(PrHtmlWindow* window, PrHtmlMessagePortPendingFn PR_NULLABLE pending,
        void* PR_NULLABLE context, PrHtmlMessagePort* PR_NULLABLE* PR_NONNULL first,
        PrHtmlMessagePort* PR_NULLABLE* PR_NONNULL second) {
    PrHtmlMessagePort* a = createPort(window, pending, context);
    PrHtmlMessagePort* b = createPort(window, pending, context);
    a->peer = b;
    b->peer = a;
    *first = a;
    *second = b;
}

PrHtmlMessagePort* PR_NULLABLE prHtmlMessagePortQuery(PrDomEventTarget* PR_NULLABLE object) {
    bool isPort = object != nullptr && object->type == DomTargetType_MessagePort;
    return isPort ? static_cast<PrHtmlMessagePort*>(object) : nullptr;
}

PrHtmlMessagePort* PR_NULLABLE prHtmlMessagePortPeer(PrHtmlMessagePort* port) {
    return port->peer;
}

bool prHtmlMessagePortIsDetached(const PrHtmlMessagePort* port) {
    return port->detached;
}

bool prHtmlMessagePortHasWindow(const PrHtmlMessagePort* port) {
    return port->window != nullptr;
}

void prHtmlMessagePortPostMessage(PrHtmlMessagePort* port, PrStringView serialized,
        PrHtmlMessagePort* PR_NONNULL const* PR_NULLABLE ports, uint32_t portCount) {
    PrHtmlMessagePort* peer = port->peer;
    HtmlMessage message{.data = prRawStringCopy(serialized)};
    for (uint32_t i = 0; i < portCount; ++i)
        message.ports.push(prHtmlMessagePortAsEventTarget(ports[i]));
    if (peer == nullptr) {
        htmlMessageReset(&message);
        return;
    }
    peer->queue.push(message);
    if (peer->started && peer->window != nullptr)
        scheduleDelivery(peer);
}

void prHtmlMessagePortStart(PrHtmlMessagePort* port) {
    if (port->started || port->detached)
        return;
    port->started = true;
    if (port->window == nullptr)
        return;
    for (uint32_t i = port->queueHead; i < port->queue.count; ++i)
        scheduleDelivery(port);
}

void prHtmlMessagePortClose(PrHtmlMessagePort* port) {
    disentangle(port);
}

PrHtmlMessagePort* prHtmlMessagePortTransfer(PrHtmlMessagePort* port) {
    PrHtmlMessagePort* moved = createPort(nullptr, nullptr, nullptr);
    moved->peer = port->peer;
    if (moved->peer != nullptr)
        moved->peer->peer = moved;
    port->peer = nullptr;
    port->detached = true;
    for (uint32_t i = port->queueHead; i < port->queue.count; ++i)
        moved->queue.push(port->queue.data[i]);
    port->queue.clear();
    port->queueHead = 0;
    return moved;
}

void prHtmlMessagePortAdopt(PrHtmlMessagePort* port, PrHtmlWindow* window, PrHtmlMessagePortPendingFn PR_NULLABLE pending,
        void* PR_NULLABLE context) {
    PR_ASSERT(port->window == nullptr);
    port->window = window;
    port->pending = pending;
    port->context = context;
    prDomEventTargetRetain(prHtmlWindowAsEventTarget(window));
}

uint32_t prHtmlMessageEventPortCount(const PrHtmlMessageEvent* event) {
    return event->message != nullptr ? event->message->ports.count : 0;
}

PrHtmlMessagePort* prHtmlMessageEventPort(PrHtmlMessageEvent* event, uint32_t index) {
    return prHtmlMessagePortQuery(event->message->ports.data[index]);
}
