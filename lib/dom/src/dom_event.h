// Events
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_event_target.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomListeners;
struct DomEntryList;

// A standalone EventTarget, which counts the references to it.
struct DomStandaloneTarget : PrDomEventTarget {
    uint32_t references;
};

// A target of an interface that another library defines, which counts the references to it and frees itself through
// `destroy` once the last is released.
struct DomCountedTarget : PrDomEventTarget {
    void (*destroy)(DomCountedTarget* target);
    uint32_t references;
};

// A target of an interface that another library defines, which is part of the target `owner`. A reference to the part
// is a reference to its owner.
struct DomPartTarget : PrDomEventTarget {
    PrDomEventTarget* owner;
};

// A target of an interface that another library defines, which lives as long as its document.
struct DomDocumentTarget : PrDomEventTarget {
    PrDomDocument* document;
};

// The data of an event of an interface that another library defines, which frees it through `destroy`.
struct DomEventExtra {
    void (*destroy)(DomEventExtra* extra);
};

// Adds or releases a reference to an event target. A reference to a node, or to a document's FontFaceSet, keeps its
// document alive. The last release frees a standalone EventTarget, a reference-counted target (DomCountedTarget) or an
// AbortSignal. A reference to a part of a target (DomPartTarget) is a reference to its owner. These calls do nothing
// for any other target, which its owner keeps alive.
void domEventTargetRetain(PrDomEventTarget* target);
void domEventTargetRelease(PrDomEventTarget* PR_NULLABLE target);

// Points `slot` at `target`, taking a reference to `target`, and releases the reference that `slot` held.
void domEventTargetAssign(PrDomEventTarget* PR_NULLABLE* PR_NONNULL slot, PrDomEventTarget* PR_NULLABLE target);

// The data that an ErrorEvent carries beyond an event (html/#runtime-script-errors).
struct DomErrorEventData {
    PrString message;
    PrString filename;
    uint32_t lineno;
    uint32_t colno;
};

// The data that a TransitionEvent or an AnimationEvent carries beyond an event
// (css-transitions-1/#interface-transitionevent, css-animations-1/#interface-animationevent). This is the property that
// transitioned or the name of the animation, the time in seconds within the active time at which the event occurs, and
// the pseudo-element that is animated, or empty for the element itself.
struct DomAnimationEventData {
    PrString name;
    double elapsedTime;
    PrString pseudoElement;
};

// The data that a MouseEvent carries beyond a UIEvent (pointerevents4/#mouseevent-interface). The related target is
// stored in the event.
struct DomMouseEventData {
    PrDomModifiers modifiers;
    int16_t button;
    uint16_t buttons;
    double screenX;
    double screenY;
    double clientX;
    double clientY;
};

// The data that a WheelEvent carries beyond a MouseEvent (pointerevents4/#interface-wheelevent).
struct DomWheelEventData {
    double deltaX;
    double deltaY;
    double deltaZ;
    uint32_t deltaMode;
};

// The data that a KeyboardEvent carries beyond a UIEvent (uievents/#interface-keyboardevent), with the legacy codes
// (uievents/#legacy-interface-KeyboardEvent).
struct DomKeyboardEventData {
    PrString key;
    PrString code;
    uint32_t location;
    uint32_t charCode;
    uint32_t keyCode;
    PrDomModifiers modifiers;
    bool repeat;
    bool isComposing;
};

// The data that a MessageEvent carries beyond an event (html/#messageevent). The data is the bytes that a script
// serialized the message to, or the null string if the binding holds the message itself.
struct DomMessageEventData {
    PrString data;
    PrString origin;
    PrString lastEventId;
    PrDomEventTarget* PR_NULLABLE source;

    // The ports that the message transferred, each with a reference.
    Array<PrDomEventTarget* PR_NONNULL> ports;
};

// The data of a PopStateEvent or a HashChangeEvent (html/#popstateevent, html/#hashchangeevent).
struct DomHistoryEventData {
    PrString oldUrl;
    PrString newUrl;
    bool historyState;              // a popstate that carries the history state of the window it is fired at
    bool hasUAVisualTransition;
};

// The data of a SubmitEvent, a FormDataEvent, a ToggleEvent or a CommandEvent (html/#submitevent, html/#formdataevent,
// html/#toggleevent, html/#commandevent). The data holds a reference to its source and to its entry list.
struct DomControlEventData {
    PrDomEventTarget* PR_NULLABLE source;   // a SubmitEvent's submitter, or a ToggleEvent's or CommandEvent's source
    DomEntryList* PR_NULLABLE entries;      // a FormDataEvent's form data
    PrString oldState;                      // a ToggleEvent's states
    PrString newState;
    PrString command;                       // a CommandEvent's command
};

// An event (dom/#interface-event). It holds a reference to its target, its related target and the retargeted related
// target (domEventTargetAssign), which each dispatch sets. Its path holds no references, because the path is the target
// and the target's ancestors, which the target keeps alive.
struct DomEvent {
    PrString type;
    PrDomEventTarget* PR_NULLABLE target;
    PrDomEventTarget* PR_NULLABLE currentTarget;  // set while a listener is being invoked
    Array<PrDomEventTarget* PR_NONNULL> path;     // during dispatch, from the target outward (dom/#event-path)
    DomErrorEventData* PR_NULLABLE error;         // data of an ErrorEvent, or null for any other event
    DomMessageEventData* PR_NULLABLE message;     // data of a MessageEvent, or null for any other event
    DomHistoryEventData* PR_NULLABLE history;     // data of a PopStateEvent or HashChangeEvent, or null
    DomControlEventData* PR_NULLABLE control;     // data of a SubmitEvent, FormDataEvent, ToggleEvent or CommandEvent
    DomAnimationEventData* PR_NULLABLE animation; // data of a TransitionEvent or AnimationEvent, or null
    DomMouseEventData* PR_NULLABLE mouse;         // data of a MouseEvent, or null for one that createEvent made
    DomWheelEventData* PR_NULLABLE wheel;         // data of a WheelEvent, or null for any other event
    DomKeyboardEventData* PR_NULLABLE keyboard;   // data of a KeyboardEvent, or null for one that createEvent made
    PrString composition;                         // data attribute of a CompositionEvent or an InputEvent
    PrString inputType;                           // inputType attribute of an InputEvent
    PrDomDataTransfer* PR_NULLABLE transfer;      // clipboardData attribute of a ClipboardEvent
    bool isComposing;                             // isComposing attribute of an InputEvent
    PrDomEventTarget* PR_NULLABLE related;        // related target that the event was given
    PrDomEventTarget* PR_NULLABLE relatedTarget;  // `related`, retargeted the same way as the target
    PrDomEventTarget* PR_NULLABLE view;           // a UIEvent's Window

    DomEventExtra* PR_NULLABLE extra;             // data of an interface that another library defines, or null

    int32_t detail;                              // detail attribute of a UIEvent
    uint32_t which;                              // which attribute of a UIEvent, as its init gave it
    double timeStamp;                            // in prTimeMonotonicMilliseconds()
    PrDomEventKind kind;
    PrDomEventPhase phase;
    bool bubbles;
    bool cancelable;
    bool composed;
    bool trusted;                                // dom/#dom-event-istrusted
    bool initialized;                            // dom/#initialized-flag
    bool dispatching;                            // dom/#dispatch-flag
    bool stopPropagation;                        // dom/#stop-propagation-flag
    bool stopImmediatePropagation;               // dom/#stop-immediate-propagation-flag
    bool canceled;                               // dom/#canceled-flag
    bool inPassiveListener;                      // dom/#in-passive-listener-flag
    uint32_t references;                         // one for the creator, and one for each holder
};

// One registration in a target's event listener list (dom/#interface-eventtarget).
struct DomListener {
    PrString type;
    PrDomEventListener callback;
    uint64_t id;                     // order in which the listener was added, which dispatch follows

    // The signal whose abort removes the listener, which the listener holds a reference to, and the id of the abort
    // algorithm that removes it. Null if the listener has no signal.
    PrDomAbortSignal* PR_NULLABLE signal;
    uint32_t abortId;

    bool capture;
    bool passive;
    bool once;
};

// An event handler that has been set on a target (html/#event-handlers).
struct DomEventHandlerEntry {
    PrDomEventHandlerCallback value;   // a null invoke if it is null or not yet compiled

    // The text of an internal raw uncompiled handler (html/#internal-raw-uncompiled-handler), or the null string for
    // any other value.
    PrString raw;

    PrDomElement* PR_NULLABLE element;   // the element whose content attribute is `raw`
    PrDomEventHandler handler;
    bool listening;                    // true if the handler's listener is in the target's list
};

// A target's event listeners, in the order they were added, and its event handlers.
struct DomListeners {
    Array<DomListener> entries;
    Array<DomEventHandlerEntry> handlers;
    uint64_t nextId;                   // id of the next listener added
};

// Starts a new event of `kind` with the steps of createEvent() (dom/#dom-document-createevent), with one reference that
// the caller holds. The event is not initialized.
void domEventStart(PrDomEvent* event, PrDomEventKind kind);

// Initializes a started event with the steps of its constructor (dom/#concept-event-constructor). The event is
// untrusted.
void domEventConstruct(PrDomEvent* event, PrStringView type, const PrDomEventInit& init);

// Gives a TransitionEvent or an AnimationEvent its data.
void domEventSetAnimation(PrDomEvent* event, PrStringView name, double elapsedTime, PrStringView pseudoElement);

// Gives an ErrorEvent its data.
void domEventSetError(PrDomEvent* event, PrStringView message, PrStringView filename, uint32_t lineno, uint32_t colno);

// Creates an event of the interface `Event` with the steps of createEvent(). The event is not initialized.
template <typename Event = PrDomEvent>
Event* PR_NONNULL domEventCreateUninitialized(PrDomEventKind kind) {
    Event* event = allocZeroed<Event>();
    domEventStart(event, kind);
    return event;
}

// Creates an event of the interface `Event` with the steps of its constructor. The event is initialized and untrusted.
template <typename Event = PrDomEvent>
Event* PR_NONNULL domEventCreate(PrStringView type, const PrDomEventInit& init,
        PrDomEventKind kind = PrDomEventKind_Event) {
    Event* event = domEventCreateUninitialized<Event>(kind);
    domEventConstruct(event, type, init);
    return event;
}

// Adds or releases a reference. The last release frees the event.
void domEventRetain(PrDomEvent* event);
void domEventRelease(PrDomEvent* PR_NULLABLE event);

// Initializes the event (dom/#concept-event-initialize). Does nothing while the event is being dispatched.
void domEventInitialize(PrDomEvent* event, PrStringView type, bool bubbles, bool cancelable);

// Cancels the event, unless it cannot be canceled or a passive listener is running (dom/#set-the-canceled-flag).
void domEventCancel(PrDomEvent* event);

// Dispatches `event` at `target` (dom/#concept-event-dispatch). Returns true if the event was not canceled. A non-null
// `targetOverride` replaces `target` as the event's target. For example, a Window's document is the target override for
// the Window's load event.
bool domEventDispatch(PrDomEventTarget* target, PrDomEvent* event, PrDomEventTarget* PR_NULLABLE targetOverride = nullptr);

// Returns `target` retargeted against `other` (dom/#retarget): `target`, or the host of the outermost shadow root that
// holds `target` and does not hold `other`. A null `other` is in no tree.
PrDomEventTarget* PR_NULLABLE domRetarget(PrDomEventTarget* PR_NULLABLE target, PrDomEventTarget* PR_NULLABLE other);

// Fires a new trusted event of `type` at `target` (dom/#concept-event-fire). Returns true if the event was not
// canceled.
bool domEventFire(PrDomEventTarget* target, PrStringView type, const PrDomEventInit& init = {},
    PrDomEventTarget* PR_NULLABLE targetOverride = nullptr);

// Frees the listeners and handlers, and releases the references that their callbacks hold.
void domListenersDestroy(DomListeners* PR_NULLABLE listeners);

// Erases the event listeners and handlers of `target` (html/#erase-all-event-listeners-and-handlers). The lists keep
// their storage, which a dispatch may be reading.
void domListenersErase(PrDomEventTarget* target);

// Returns the default passive value of a listener for `type` on `target` (dom/#default-passive-value).
bool domDefaultPassive(const PrDomEventTarget* target, PrStringView type);

// Returns true if `target` has a listener for wheel events that is not passive, which may cancel a wheel.
bool domHasBlockingWheelListener(const PrDomEventTarget* target);

// Returns true if a wheel event fired at `target` would reach a listener that is not passive on its path.
bool domWheelPathBlocks(PrDomEventTarget* target);

// Adds `callback` as a listener for `type` (dom/#add-an-event-listener). Adds nothing if a listener with the same type,
// callback and capture already exists.
void domListenerAdd(PrDomEventTarget* target, PrStringView type, const PrDomEventListener* callback,
    const PrDomAddEventListenerOptions* PR_NULLABLE options);

// Removes the listener for `type` with `callback` and `capture`, if there is one (dom/#remove-an-event-listener).
void domListenerRemove(PrDomEventTarget* target, PrStringView type, const PrDomEventListener* callback, bool capture);

// Returns the value of the event handler `handler` of `target` (html/#getting-the-current-value-of-the-event-handler).
// Compiles an internal raw uncompiled handler through the host of the element's document. Returns a null invoke if the
// handler is null or does not compile.
PrDomEventHandlerCallback domEventHandler(PrDomEventTarget* target, PrDomEventHandler handler);

// Sets the event handler `handler` of `target` with the steps of the setter in html/#event-handler-attributes. A null
// `callback` deactivates the handler, and a non-null callback activates it if it was not active.
void domSetEventHandler(PrDomEventTarget* target, PrDomEventHandler handler,
    const PrDomEventHandlerCallback* PR_NULLABLE callback);

// Sets the event handler `handler` of `target` to an internal raw uncompiled handler of `text`, and activates it.
// `text` is the value of the content attribute of `element` (html/#event-handler-attributes).
void domSetEventHandlerRaw(PrDomEventTarget* target, PrDomEventHandler handler, PrStringView text, PrDomElement* element);

} // namespace pr_impl

// C opaque types. Every event is a DomEvent, and its interface is the one that its kind names.
struct PrDomEvent : pr_impl::DomEvent {};
struct PrDomCustomEvent : PrDomEvent {};
struct PrDomUiEvent : PrDomEvent {};
struct PrDomFocusEvent : PrDomUiEvent {};
struct PrDomMouseEvent : PrDomUiEvent {};
struct PrDomWheelEvent : PrDomMouseEvent {};
struct PrDomKeyboardEvent : PrDomUiEvent {};
struct PrDomCompositionEvent : PrDomUiEvent {};
struct PrDomInputEvent : PrDomUiEvent {};
struct PrDomClipboardEvent : PrDomEvent {};
struct PrCssAnimationEvent : PrDomEvent {};
struct PrCssTransitionEvent : PrDomEvent {};
struct PrCssFontFaceSetLoadEvent : PrDomEvent {};

PR_ASSUME_NONNULL_END
