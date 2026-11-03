#include "dom/src/dom_event.h"

#include "pr/alloc.h"
#include "pr/string_ops.h"
#include "pr/time.h"

#include "dom/src/dom_abort.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_event_handler.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_slot.h"
#include "dom/src/dom_use.h"

namespace pr_impl {
namespace {

void retain(const PrDomEventListener& callback) {
    if (callback.retain != nullptr)
        callback.retain(callback.context);
}

void release(const PrDomEventListener& callback) {
    if (callback.release != nullptr)
        callback.release(callback.context);
}

void remove(DomListeners* listeners, uint32_t index) {
    DomListener entry = listeners->entries.data[index];
    listeners->entries.erase(index);
    prStringReset(&entry.type);
    release(entry.callback);
    if (entry.signal != nullptr) {
        prDomAbortSignalRemove(entry.signal, entry.abortId);
        domAbortSignalRelease(entry.signal);
    }
}

// Returns true if a listener for `type` can cancel a wheel: it listens for wheel events, and `passive` is not set.
bool blocksWheel(PrStringView type, bool passive) {
    return !passive && type == "wheel";
}

// Tells the presentation of the target's document that the target gained or lost a listener that can cancel a wheel,
// which changes where a user's wheel waits for the document.
void wheelListenersChanged(PrDomEventTarget* target) {
    if (PrDomElement* element = domElementFromNode(domNodeFromTarget(target)))
        domNotifyWheelListenersChanged(element);
}

DomEventHandlerEntry* PR_NULLABLE findHandler(const DomListeners* PR_NULLABLE listeners, PrDomEventHandler handler) {
    if (listeners == nullptr)
        return nullptr;
    for (uint32_t i = 0; i < listeners->handlers.count; ++i) {
        if (listeners->handlers.data[i].handler == handler)
            return &listeners->handlers.data[i];
    }
    return nullptr;
}

// html/#the-event-handler-processing-algorithm, run as the listener for the handlers of the target `context`. The
// handler is the one for the event's type.
void processHandler(void* PR_NULLABLE context, PrDomEvent* handle) {
    PrDomEventTarget* target = static_cast<PrDomEventTarget*>(context);
    PrDomEvent* event = reinterpret_cast<PrDomEvent*>(handle);
    DomListeners* listeners = target->listeners;
    PrDomEventHandlerCallback callback = {};
    for (uint32_t i = 0; listeners != nullptr && i < listeners->handlers.count; ++i) {
        const DomEventHandlerEntry& handler = listeners->handlers.data[i];
        if (handler.listening && domEventHandlerType(handler.handler) == prStringView(&event->type)) {
            callback = domEventHandler(target, handler.handler);
            break;
        }
    }
    if (callback.invoke == nullptr)
        return;
    bool error = event->kind == PrDomEventKind_Error && prStringView(&event->type) == "error" &&
        event->currentTarget != nullptr && event->currentTarget->type == DomTargetType_Window;
    if (callback.retain != nullptr)
        callback.retain(callback.context);
    PrDomEventHandlerResult result = callback.invoke(callback.context, handle, error);
    if (callback.release != nullptr)
        callback.release(callback.context);
    if (result == (error ? PrDomEventHandlerResult_True : PrDomEventHandlerResult_False))
        domEventCancel(event);
}

// Returns the listener for an event handler of `target`. Each of the target's handlers is for a different type, and the
// type distinguishes their listeners.
PrDomEventListener handlerListener(PrDomEventTarget* target) {
    return PrDomEventListener{processHandler, target, 0, nullptr, nullptr};
}

void releaseHandler(DomEventHandlerEntry* handler) {
    if (handler->value.release != nullptr)
        handler->value.release(handler->value.context);
    handler->value = {};
    prStringReset(&handler->raw);
    handler->element = nullptr;
}

// Returns the entry of the event handler `handler` of `target`, creating it if the target has none.
DomEventHandlerEntry* entryOf(PrDomEventTarget* target, PrDomEventHandler handler) {
    DomEventHandlerEntry* entry = findHandler(target->listeners, handler);
    if (entry != nullptr)
        return entry;
    if (target->listeners == nullptr)
        target->listeners = allocZeroed<DomListeners>();
    DomEventHandlerEntry& added = target->listeners->handlers.emplace();
    added = DomEventHandlerEntry{{}, {}, nullptr, handler, false};
    return &added;
}

// html/#activate-an-event-handler
void activate(PrDomEventTarget* target, DomEventHandlerEntry* entry) {
    if (entry->listening)
        return;
    PrDomEventListener listener = handlerListener(target);
    domListenerAdd(target, domEventHandlerType(entry->handler), &listener, nullptr);
    entry->listening = true;
}

bool matches(const DomListener& entry, PrStringView type, const PrDomEventListener* callback, bool capture) {
    return entry.capture == capture && entry.callback.invoke == callback->invoke &&
        entry.callback.identity == callback->identity && prStringView(&entry.type) == type;
}

// dom/#get-the-parent: returns a node's parent, a shadow root's host, or a document's Window, for an event of `type`
// that is composed if `composed` is set. A shadow root has no parent if the event is not composed and the shadow root
// is the root of the event's target. A document has no parent for a load event. `origin` is the target that the path
// starts from.
PrDomEventTarget* PR_NULLABLE parentOf(PrDomEventTarget* target, PrStringView type, bool composed, PrDomEventTarget* origin) {
    PrDomNode* node = domNodeFromTarget(target);
    if (node == nullptr)
        return nullptr;
    if (node->type == PrDomNodeType_Document)
        return type == "load" ? nullptr : static_cast<PrDomDocument*>(node)->global;
    if (PrDomShadowRoot* shadow = domNodeShadowRoot(node)) {
        PrDomNode* start = domNodeFromTarget(origin);
        bool own = start != nullptr && domNodeRoot(start) == shadow;
        return !composed && own ? nullptr : shadow->host;
    }
    // The parent of a slottable in the path is the slot that it is assigned to.
    if (node->document->slots != nullptr) {
        if (PrDomElement* slot = domAssignedSlot(node))
            return slot;
    }
    return domNodeParent(node);
}

// Returns true if `ancestor` is a shadow-including inclusive ancestor of `node`
// (dom/#concept-shadow-including-inclusive-ancestor).
bool shadowIncludingAncestor(const PrDomNode* ancestor, const PrDomNode* PR_NULLABLE node) {
    for (; node != nullptr; node = domShadowIncludingParent(node)) {
        if (node == ancestor)
            return true;
    }
    return false;
}

// dom/#retarget: returns `target` if it is in a tree that `other` is in or is under. Otherwise replaces `target` with
// the host of the shadow root that it is in, and repeats.
PrDomEventTarget* PR_NULLABLE retarget(PrDomEventTarget* PR_NULLABLE target, PrDomEventTarget* PR_NULLABLE other) {
    PrDomNode* within = other != nullptr ? domNodeFromTarget(other) : nullptr;
    for (;;) {
        PrDomNode* node = domNodeFromTarget(target);
        PrDomShadowRoot* shadow = node != nullptr ? domNodeShadowRoot(domNodeRoot(node)) : nullptr;
        if (shadow == nullptr || (within != nullptr && shadowIncludingAncestor(shadow, within)))
            return target;
        target = shadow->host;
    }
}

// dom/#concept-event-listener-invoke for the capture listeners, or for the other listeners. The listeners are those
// that the target has when invocation starts, in the order in which they were added. A listener that is removed during
// invocation is not called.
void invokeListeners(PrDomEventTarget* owner, PrDomEvent* event, bool capture, bool handlers);

void invoke(PrDomEventTarget* target, PrDomEvent* event, bool capture) {
    if (event->stopPropagation)
        return;
    event->currentTarget = target;
    if (target->listeners != nullptr)
        invokeListeners(target, event, capture, true);
    // An instance in a use element's shadow tree also listens with its original's listeners
    // (SVG2/struct.html#UseEventHandling). Its event handlers are its own, from the attributes that it copies.
    PrDomElement* element = domElementFromNode(domNodeFromTarget(target));
    if (element == nullptr || element->space != PrDomNamespace_Svg || event->stopImmediatePropagation)
        return;
    PrDomNode* original = domNodeShadowRoot(domNodeRoot(element)) != nullptr ? domNodeUseOriginal(element) : nullptr;
    if (original != nullptr && original->listeners != nullptr)
        invokeListeners(original, event, capture, false);
}

// Calls the listeners of `owner`, which has some, for the event's phase, in order, with the event's current target. The
// listeners of event handlers are called only if `handlers` is set.
void invokeListeners(PrDomEventTarget* owner, PrDomEvent* event, bool capture, bool handlers) {
    DomListeners* listeners = owner->listeners;
    uint64_t last = listeners->nextId;
    uint64_t previous = 0;
    // Finds the next registration after each call, because a call may add or remove registrations.
    for (;;) {
        uint32_t index = 0;
        for (; index < listeners->entries.count; ++index) {
            const DomListener& entry = listeners->entries.data[index];
            if (entry.id > previous && entry.id <= last && entry.capture == capture &&
                    prStringView(&entry.type) == prStringView(&event->type) &&
                    (handlers || entry.callback.invoke != processHandler))
                break;
        }
        if (index == listeners->entries.count)
            return;
        DomListener entry = listeners->entries.data[index];
        previous = entry.id;
        retain(entry.callback);
        if (entry.once) {
            remove(listeners, index);
            if (blocksWheel(prStringView(&event->type), entry.passive))
                wheelListenersChanged(owner);
        }
        event->inPassiveListener = entry.passive;
        entry.callback.invoke(entry.callback.context, reinterpret_cast<PrDomEvent*>(event));
        event->inPassiveListener = false;
        release(entry.callback);
        if (event->stopImmediatePropagation)
            return;
    }
}

// The listener that an abort algorithm removes. The listener's removal removes the algorithm, so the target outlives
// it.
struct ListenerAbort {
    PrDomEventTarget* target;
    uint64_t id;
};

// Removes a listener whose signal aborted (dom/#add-an-event-listener).
void removeOnAbort(void* PR_NULLABLE context, PrDomAbortSignal*) {
    ListenerAbort* abort = static_cast<ListenerAbort*>(context);
    DomListeners* listeners = abort->target->listeners;
    for (uint32_t i = 0; i < listeners->entries.count; ++i) {
        if (listeners->entries.data[i].id == abort->id) {
            const DomListener& entry = listeners->entries.data[i];
            bool wheel = blocksWheel(prStringView(&entry.type), entry.passive);
            remove(listeners, i);
            if (wheel)
                wheelListenersChanged(abort->target);
            return;
        }
    }
}

// Removes every listener and handler, and keeps the storage of the lists.
void clearListeners(DomListeners* listeners) {
    while (listeners->entries.count != 0)
        remove(listeners, listeners->entries.count - 1);
    for (uint32_t i = 0; i < listeners->handlers.count; ++i)
        releaseHandler(&listeners->handlers.data[i]);
    listeners->handlers.clear();
}

// Returns true if `item` is an element with activation behavior, according to the host of its document.
bool hasActivationBehavior(PrDomEventTarget* item) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(item));
    if (element == nullptr)
        return false;
    const DomDocumentHost* host = element->document->host;
    return host != nullptr && host->activates != nullptr && host->activates(host->context, element);
}

} // namespace

void domEventTargetRetain(PrDomEventTarget* target) {
    if (PrDomNode* node = domNodeFromTarget(target)) {
        domNodeRetain(node);
        return;
    }
    if ((target->type & DomTargetTypeFlag_Counted) != 0) {
        ++static_cast<DomCountedTarget*>(target)->references;
        return;
    }
    if ((target->type & DomTargetTypeFlag_Part) != 0) {
        domEventTargetRetain(static_cast<DomPartTarget*>(target)->owner);
        return;
    }
    switch (target->type) {
        case DomTargetType_EventTarget: ++static_cast<DomStandaloneTarget*>(target)->references; break;
        case DomTargetType_FontFaceSet: domNodeRetain(static_cast<DomDocumentTarget*>(target)->document); break;
        case DomTargetType_AbortSignal: domAbortSignalRetain(static_cast<PrDomAbortSignal*>(target)); break;
        default: break;
    }
}

void domEventTargetRelease(PrDomEventTarget* PR_NULLABLE target) {
    if (target == nullptr)
        return;
    if (PrDomNode* node = domNodeFromTarget(target)) {
        domNodeRelease(node);
        return;
    }
    if ((target->type & DomTargetTypeFlag_Counted) != 0) {
        DomCountedTarget* counted = static_cast<DomCountedTarget*>(target);
        if (--counted->references == 0)
            counted->destroy(counted);
        return;
    }
    if ((target->type & DomTargetTypeFlag_Part) != 0) {
        domEventTargetRelease(static_cast<DomPartTarget*>(target)->owner);
        return;
    }
    switch (target->type) {
        case DomTargetType_EventTarget: {
            DomStandaloneTarget* standalone = static_cast<DomStandaloneTarget*>(target);
            if (--standalone->references == 0) {
                domListenersDestroy(standalone->listeners);
                prMemoryFree(standalone);
            }
            break;
        }
        case DomTargetType_FontFaceSet: domNodeRelease(static_cast<DomDocumentTarget*>(target)->document); break;
        case DomTargetType_AbortSignal: domAbortSignalRelease(static_cast<PrDomAbortSignal*>(target)); break;
        default: break;
    }
}

void domEventTargetAssign(PrDomEventTarget* PR_NULLABLE* PR_NONNULL slot, PrDomEventTarget* PR_NULLABLE target) {
    if (*slot == target)
        return;
    if (target != nullptr)
        domEventTargetRetain(target);
    PrDomEventTarget* old = *slot;
    *slot = target;
    domEventTargetRelease(old);
}

void domEventStart(PrDomEvent* event, PrDomEventKind kind) {
    event->references = 1;
    event->type = prStringEmpty();
    event->kind = kind;
    event->timeStamp = prTimeMonotonicMilliseconds();
}

void domEventConstruct(PrDomEvent* event, PrStringView type, const PrDomEventInit& init) {
    domEventInitialize(event, type, init.bubbles, init.cancelable);
    event->composed = init.composed;
}

void domEventSetAnimation(PrDomEvent* event, PrStringView name, double elapsedTime, PrStringView pseudoElement) {
    event->animation = alloc<DomAnimationEventData>();
    *event->animation = DomAnimationEventData{prStringCopy(name), elapsedTime, prStringCopy(pseudoElement)};
}

void domEventSetError(PrDomEvent* event, PrStringView message, PrStringView filename, uint32_t lineno, uint32_t colno) {
    event->error = alloc<DomErrorEventData>();
    *event->error = DomErrorEventData{prStringCopy(message), prStringCopy(filename), lineno, colno};
}

void domEventRetain(PrDomEvent* event) {
    ++event->references;
}

void domEventRelease(PrDomEvent* PR_NULLABLE event) {
    if (event == nullptr || --event->references != 0)
        return;
    if (event->error != nullptr) {
        prStringReset(&event->error->message);
        prStringReset(&event->error->filename);
        prMemoryFree(event->error);
    }
    if (event->message != nullptr) {
        prStringReset(&event->message->data);
        prStringReset(&event->message->origin);
        prStringReset(&event->message->lastEventId);
        for (uint32_t i = 0; i < event->message->ports.count; ++i)
            domEventTargetRelease(event->message->ports.data[i]);
        event->message->ports.reset();
        prMemoryFree(event->message);
    }
    if (event->history != nullptr) {
        prStringReset(&event->history->oldUrl);
        prStringReset(&event->history->newUrl);
        prMemoryFree(event->history);
    }
    if (DomControlEventData* control = event->control) {
        domEventTargetRelease(control->source);
        domEntryListRelease(control->entries);
        prStringReset(&control->oldState);
        prStringReset(&control->newState);
        prStringReset(&control->command);
        prMemoryFree(control);
    }
    if (event->animation != nullptr) {
        prStringReset(&event->animation->name);
        prStringReset(&event->animation->pseudoElement);
        prMemoryFree(event->animation);
    }
    prMemoryFree(event->mouse);
    prMemoryFree(event->wheel);
    if (event->keyboard != nullptr) {
        prStringReset(&event->keyboard->key);
        prStringReset(&event->keyboard->code);
        prMemoryFree(event->keyboard);
    }
    prStringReset(&event->composition);
    event->path.reset();
    if (event->extra != nullptr)
        event->extra->destroy(event->extra);
    domEventTargetRelease(event->target);
    domEventTargetRelease(event->related);
    domEventTargetRelease(event->relatedTarget);
    prStringReset(&event->type);
    prMemoryFree(event);
}

void domEventInitialize(PrDomEvent* event, PrStringView type, bool bubbles, bool cancelable) {
    if (event->dispatching)
        return;
    event->initialized = true;
    event->stopPropagation = false;
    event->stopImmediatePropagation = false;
    event->canceled = false;
    event->trusted = false;
    domEventTargetAssign(&event->target, nullptr);
    prStringReset(&event->type);
    event->type = prStringCopy(type);
    event->bubbles = bubbles;
    event->cancelable = cancelable;
}

void domEventCancel(PrDomEvent* event) {
    if (event->cancelable && !event->inPassiveListener)
        event->canceled = true;
}

bool domEventDispatch(PrDomEventTarget* target, PrDomEvent* event, PrDomEventTarget* PR_NULLABLE targetOverride) {
    event->dispatching = true;
    PrDomEventTarget* origin = targetOverride != nullptr ? targetOverride : target;
    // A click that is a MouseEvent activates its target or, if the event bubbles, the nearest parent with activation
    // behavior.
    bool activation = event->kind == PrDomEventKind_Mouse && prStringView(&event->type) == "click";
    PrDomEventTarget* activationTarget = activation && hasActivationBehavior(target) ? target : nullptr;
    // An event whose related target, retargeted against the target, is the target itself is dispatched to no one.
    // Otherwise its path ends at a parent that is its related target, unless the target is already under that parent.
    PrDomEventTarget* related = event->related;
    if (related == nullptr || retarget(related, target) != target || target == related) {
        PrDomEventTarget* scope = target;
        for (PrDomEventTarget* item = target; item != nullptr;
                item = parentOf(item, prStringView(&event->type), event->composed, target)) {
            PrDomNode* node = domNodeFromTarget(item);
            PrDomNode* scopeNode = domNodeFromTarget(scope);
            bool under = node == nullptr || (scopeNode != nullptr && shadowIncludingAncestor(domNodeRoot(scopeNode), node));
            if (item != target && !under) {
                if (related != nullptr && item == retarget(related, item))
                    break;
                scope = item;
            }
            if (item != target && activation && event->bubbles && activationTarget == nullptr &&
                    hasActivationBehavior(item))
                activationTarget = item;
            event->path.push(item);
        }
    }

    // The capture listeners run from the outermost target inward, and then the other listeners run from the target
    // outward. The non-capture listeners of the target's parents run only for an event that bubbles. Each listener sees
    // the target retargeted to its own tree. The first entry of the path is at the target. So is a host if the target
    // in its shadow tree is retargeted to it.
    for (uint32_t i = event->path.count; i-- > 0;) {
        domEventTargetAssign(&event->target, retarget(origin, event->path.data[i]));
        domEventTargetAssign(&event->relatedTarget, retarget(related, event->path.data[i]));
        bool atTarget = i == 0 || event->target == event->path.data[i];
        event->phase = atTarget ? PrDomEventPhase_AtTarget : PrDomEventPhase_Capturing;
        invoke(event->path.data[i], event, true);
    }
    for (uint32_t i = 0; i < event->path.count; ++i) {
        domEventTargetAssign(&event->target, retarget(origin, event->path.data[i]));
        domEventTargetAssign(&event->relatedTarget, retarget(related, event->path.data[i]));
        bool atTarget = i == 0 || event->target == event->path.data[i];
        if (!atTarget && !event->bubbles)
            continue;
        event->phase = atTarget ? PrDomEventPhase_AtTarget : PrDomEventPhase_Bubbling;
        invoke(event->path.data[i], event, false);
    }
    // dom/#concept-event-dispatch leaves the targets retargeted to the outermost tree, and clears them if that tree is
    // still a shadow tree.
    PrDomEventTarget* outermost = event->path.count != 0 ? event->path.last() : origin;
    domEventTargetAssign(&event->target, retarget(origin, outermost));
    domEventTargetAssign(&event->relatedTarget, retarget(related, outermost));
    auto inShadow = [](PrDomEventTarget* PR_NULLABLE item) {
        PrDomNode* node = domNodeFromTarget(item);
        return node != nullptr && domNodeShadowRoot(domNodeRoot(node)) != nullptr;
    };
    if (inShadow(event->target) || inShadow(event->relatedTarget)) {
        domEventTargetAssign(&event->target, nullptr);
        domEventTargetAssign(&event->relatedTarget, nullptr);
    }

    event->phase = PrDomEventPhase_None;
    event->currentTarget = nullptr;
    event->path.clear();
    event->dispatching = false;
    event->stopPropagation = false;
    event->stopImmediatePropagation = false;
    if (activationTarget != nullptr && !event->canceled) {
        PrDomElement* element = domElementFromNode(domNodeFromTarget(activationTarget));
        const DomDocumentHost* host = element->document->host;
        domNodeRetain(element);
        if (host != nullptr && host->activate != nullptr)
            host->activate(host->context, element, event);
        domNodeRelease(element);
    }
    return !event->canceled;
}

PrDomEventTarget* PR_NULLABLE domRetarget(PrDomEventTarget* PR_NULLABLE target, PrDomEventTarget* PR_NULLABLE other) {
    return retarget(target, other);
}

bool domEventFire(PrDomEventTarget* target, PrStringView type, const PrDomEventInit& init,
        PrDomEventTarget* PR_NULLABLE targetOverride) {
    PrDomEvent* event = domEventCreate(type, init);
    event->trusted = true;
    bool proceed = domEventDispatch(target, event, targetOverride);
    domEventRelease(event);
    return proceed;
}

void domListenersDestroy(DomListeners* PR_NULLABLE listeners) {
    if (listeners == nullptr)
        return;
    clearListeners(listeners);
    listeners->entries.reset();
    listeners->handlers.reset();
    prMemoryFree(listeners);
}

void domListenersErase(PrDomEventTarget* target) {
    if (target->listeners == nullptr)
        return;
    bool wheel = domHasBlockingWheelListener(target);
    clearListeners(target->listeners);
    if (wheel)
        wheelListenersChanged(target);
}

// dom/#default-passive-value
bool domDefaultPassive(const PrDomEventTarget* target, PrStringView type) {
    if (type != "touchstart" && type != "touchmove" && type != "wheel" && type != "mousewheel")
        return false;
    if (target->type == DomTargetType_Window)
        return true;
    const PrDomNode* node = domNodeFromTarget(const_cast<PrDomEventTarget*>(target));
    if (node == nullptr)
        return false;
    PrDomDocument* document = node->document;
    return node == document || node == domDocumentElement(document) || node == domDocumentBody(document);
}

bool domHasBlockingWheelListener(const PrDomEventTarget* target) {
    const DomListeners* listeners = target->listeners;
    for (uint32_t i = 0; listeners != nullptr && i < listeners->entries.count; ++i) {
        const DomListener& entry = listeners->entries.data[i];
        if (blocksWheel(prStringView(&entry.type), entry.passive))
            return true;
    }
    return false;
}

bool domWheelPathBlocks(PrDomEventTarget* target) {
    for (PrDomEventTarget* item = target; item != nullptr; item = parentOf(item, "wheel", true, target)) {
        if (domHasBlockingWheelListener(item))
            return true;
    }
    return false;
}

void domListenerAdd(PrDomEventTarget* target, PrStringView type, const PrDomEventListener* callback,
        const PrDomAddEventListenerOptions* PR_NULLABLE options) {
    PrDomAddEventListenerOptions settings = {.passive = domDefaultPassive(target, type)};
    if (options != nullptr)
        settings = *options;
    if (settings.signal != nullptr && prDomAbortSignalAborted(settings.signal))
        return;
    if (target->listeners == nullptr)
        target->listeners = allocZeroed<DomListeners>();
    DomListeners* listeners = target->listeners;
    for (uint32_t i = 0; i < listeners->entries.count; ++i) {
        if (matches(listeners->entries.data[i], type, callback, settings.capture))
            return;
    }
    DomListener entry{
        .type = prStringCopy(type),
        .callback = *callback,
        .id = ++listeners->nextId,
        .capture = settings.capture,
        .passive = settings.passive,
        .once = settings.once,
    };
    retain(entry.callback);
    if (settings.signal != nullptr) {
        ListenerAbort* abort = alloc<ListenerAbort>();
        *abort = ListenerAbort{target, entry.id};
        PrDomAbortAlgorithm algorithm = {
            removeOnAbort,
            abort,
            nullptr,
            [](void* context) { prMemoryFree(context); },
        };
        entry.signal = settings.signal;
        entry.abortId = prDomAbortSignalAdd(settings.signal, &algorithm);
        domAbortSignalRetain(settings.signal);
    }
    listeners->entries.push(entry);
    if (blocksWheel(type, settings.passive))
        wheelListenersChanged(target);
}

void domListenerRemove(PrDomEventTarget* target, PrStringView type, const PrDomEventListener* callback, bool capture) {
    DomListeners* listeners = target->listeners;
    if (listeners == nullptr)
        return;
    for (uint32_t i = 0; i < listeners->entries.count; ++i) {
        if (matches(listeners->entries.data[i], type, callback, capture)) {
            bool wheel = blocksWheel(type, listeners->entries.data[i].passive);
            remove(listeners, i);
            if (wheel)
                wheelListenersChanged(target);
            return;
        }
    }
}

PrDomEventHandlerCallback domEventHandler(PrDomEventTarget* target, PrDomEventHandler handler) {
    DomEventHandlerEntry* entry = findHandler(target->listeners, handler);
    if (entry == nullptr)
        return PrDomEventHandlerCallback{};
    if (!prStringIsNull(&entry->raw)) {
        // A handler that does not compile becomes null, and stays null.
        PrString raw = entry->raw;
        entry->raw = {};
        PrDomElement* element = entry->element;
        entry->element = nullptr;
        const DomDocumentHost* host = element->document->host;
        PrDomEventHandlerCallback compiled = {};
        if (host != nullptr && host->compile(host->context, target, element, handler, prStringView(&raw), &compiled))
            entry->value = compiled;
        prStringReset(&raw);
    }
    return entry->value;
}

void domSetEventHandler(PrDomEventTarget* target, PrDomEventHandler handler,
        const PrDomEventHandlerCallback* PR_NULLABLE callback) {
    DomEventHandlerEntry* entry = findHandler(target->listeners, handler);
    if (callback == nullptr || callback->invoke == nullptr) {
        // html/#deactivate-an-event-handler
        if (entry == nullptr)
            return;
        releaseHandler(entry);
        if (entry->listening) {
            PrDomEventListener listener = handlerListener(target);
            domListenerRemove(target, domEventHandlerType(handler), &listener, false);
        }
        entry->listening = false;
        return;
    }
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    entry = entryOf(target, handler);
    releaseHandler(entry);
    entry->value = *callback;
    activate(target, entry);
}

void domSetEventHandlerRaw(PrDomEventTarget* target, PrDomEventHandler handler, PrStringView text, PrDomElement* element) {
    DomEventHandlerEntry* entry = entryOf(target, handler);
    releaseHandler(entry);
    entry->raw = prStringCopy(text);
    entry->element = element;
    activate(target, entry);
}

} // namespace pr_impl
