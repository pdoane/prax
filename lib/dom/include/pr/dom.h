// Document Object Model
//
// dom - https://dom.spec.whatwg.org/
// html - https://html.spec.whatwg.org/
// uievents - https://w3c.github.io/uievents/
// selection-api - https://w3c.github.io/selection-api/
// css-font-loading-3 - https://drafts.csswg.org/css-font-loading-3/
//
// The node tree and the interfaces that work on it.
//
// A document, and everything reachable from its tree, is used from its agent's thread, and nothing synchronizes access
// to it. A const call changes nothing that its caller can observe, though a cache the object owns may change.

#pragma once

#include "pr/base.h"
#include "pr/dom_event_handler.h"
#include "pr/dom_input_type.h"
#include "pr/dom_tag.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/task.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The handles of the objects that this header declares. Each section below describes its own handles.
typedef struct PrDomEvent PrDomEvent;
typedef struct PrDomCustomEvent PrDomCustomEvent;
typedef struct PrDomUiEvent PrDomUiEvent;
typedef struct PrDomFocusEvent PrDomFocusEvent;
typedef struct PrDomMouseEvent PrDomMouseEvent;
typedef struct PrDomWheelEvent PrDomWheelEvent;
typedef struct PrDomKeyboardEvent PrDomKeyboardEvent;
typedef struct PrDomCompositionEvent PrDomCompositionEvent;
typedef struct PrDomInputEvent PrDomInputEvent;
typedef struct PrDomClipboardEvent PrDomClipboardEvent;
typedef struct PrDomDataTransfer PrDomDataTransfer;
typedef struct PrDomEventListener PrDomEventListener;
typedef struct PrDomEventTarget PrDomEventTarget;
typedef struct PrDomAbortSignal PrDomAbortSignal;
typedef struct PrDomNode PrDomNode;
typedef struct PrDomAgent PrDomAgent;
typedef struct PrDomDocument PrDomDocument;
typedef struct PrDomDocumentType PrDomDocumentType;
typedef struct PrDomImplementation PrDomImplementation;
typedef struct PrDomDocumentFragment PrDomDocumentFragment;
typedef struct PrDomShadowRoot PrDomShadowRoot;
typedef struct PrDomElement PrDomElement;
typedef struct PrDomAttr PrDomAttr;
typedef struct PrDomCharacterData PrDomCharacterData;
typedef struct PrDomText PrDomText;
typedef struct PrDomComment PrDomComment;
typedef struct PrDomProcessingInstruction PrDomProcessingInstruction;
typedef struct PrDomHtmlCollection PrDomHtmlCollection;
typedef struct PrDomNodeList PrDomNodeList;
typedef struct PrDomTokenList PrDomTokenList;
typedef struct PrDomMutationObserver PrDomMutationObserver;
typedef struct PrDomMutationRecord PrDomMutationRecord;
typedef struct PrDomRange PrDomRange;
typedef struct PrDomSelection PrDomSelection;

// ---------------------------------------------------------------------------------------------------------------------
// Event (dom)
//
// Every event is reference counted through Event, whatever interface it implements. An event that a caller or a script
// creates is untrusted. The events that the engine fires are trusted.

// The phase of an event's dispatch (dom/#dom-event-eventphase).
enum {
    PrDomEventPhase_None      = 0,
    PrDomEventPhase_Capturing = 1,
    PrDomEventPhase_AtTarget  = 2,
    PrDomEventPhase_Bubbling  = 3,
};
typedef uint8_t PrDomEventPhase;

// The members of EventInit (dom/#dictdef-eventinit).
typedef struct PrDomEventInit {
    bool bubbles;
    bool cancelable;
    bool composed;
} PrDomEventInit;

// Returns an init with every member false.
static inline PrDomEventInit prDomEventInitDefault(void) {
    PrDomEventInit value;
    value.bubbles = false;
    value.cancelable = false;
    value.composed = false;
    return value;
}

// The most specific interface that an event implements beyond Event.
enum {
    PrDomEventKind_Event,
    PrDomEventKind_Custom,             // dom/#interface-customevent
    PrDomEventKind_Error,              // html/#errorevent
    PrDomEventKind_PromiseRejection,   // html/#promiserejectionevent
    PrDomEventKind_Message,            // html/#messageevent
    PrDomEventKind_Ui,                 // uievents/#uievent
    PrDomEventKind_Focus,              // uievents/#focusevent
    PrDomEventKind_PageReveal,         // html/#pagerevealevent
    PrDomEventKind_FontFaceSetLoad,    // css-font-loading-3/#fontfacesetloadevent
    PrDomEventKind_Transition,         // css-transitions-1/#interface-transitionevent
    PrDomEventKind_Animation,          // css-animations-1/#interface-animationevent
    PrDomEventKind_Mouse,              // pointerevents4/#mouseevent-interface
    PrDomEventKind_Wheel,              // pointerevents4/#interface-wheelevent
    PrDomEventKind_Keyboard,           // uievents/#interface-keyboardevent
    PrDomEventKind_Composition,        // uievents/#interface-compositionevent
    PrDomEventKind_Input,              // uievents/#interface-inputevent
    PrDomEventKind_Clipboard,          // clipboard-apis/#clipboardevent
    PrDomEventKind_PopState,           // html/#popstateevent
    PrDomEventKind_HashChange,         // html/#hashchangeevent
    PrDomEventKind_Submit,             // html/#submitevent
    PrDomEventKind_FormData,           // html/#formdataevent
    PrDomEventKind_Toggle,             // html/#toggleevent
    PrDomEventKind_Command,            // html/#commandevent
    PrDomEventKind_Progress,           // xhr/#progressevent
    PrDomEventKind_Count,
};
typedef uint8_t PrDomEventKind;

// Creates an event of `type`, with one reference that the caller holds. A null `init` means prDomEventInitDefault.
PrDomEvent* prDomEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init);

// Creates an uninitialized event of `kind`, with one reference that the caller holds (dom/#dom-document-createevent).
// Returns NotSupported for a kind that createEvent does not create.
PrStatus prDomEventCreateUninitialized(PrDomEventKind kind, PrDomEvent* PR_NULLABLE* PR_NONNULL out);

// Writes the kind that createEvent's `interface` names, matched ASCII case-insensitively, including legacy names.
// Returns false for the name of an interface that the engine lacks, and leaves `out` unchanged.
bool prDomEventKindFromInterface(PrStringView interface, PrDomEventKind* out);

// Adds or releases a reference. The last release frees the event.
void prDomEventRetain(PrDomEvent* event);
void prDomEventRelease(PrDomEvent* PR_NULLABLE event);

// Returns the event's type.
PrStringView prDomEventType(const PrDomEvent* event);

// Return the target that the event was last dispatched to, and the target whose listeners are being invoked. Each is
// null if there is none.
PrDomEventTarget* PR_NULLABLE prDomEventTarget(PrDomEvent* event);
PrDomEventTarget* PR_NULLABLE prDomEventCurrentTarget(PrDomEvent* event);

// Writes the event path, from the target outward (dom/#dom-event-composedpath). The path is empty unless the event is
// being dispatched. Writes up to `capacity` items to `items` and returns the total count.
uint32_t prDomEventComposedPath(PrDomEvent* event, PrDomEventTarget* PR_NONNULL* PR_NULLABLE items, uint32_t capacity);

// Returns the phase of the event's dispatch.
PrDomEventPhase prDomEventPhase(const PrDomEvent* event);

// Stops the event from propagating past the current target (dom/#dom-event-stoppropagation).
void prDomEventStopPropagation(PrDomEvent* event);

// Returns true if propagation is stopped (dom/#dom-event-cancelbubble).
bool prDomEventCancelBubble(const PrDomEvent* event);

// Stops propagation if `value` is true. False does nothing.
void prDomEventSetCancelBubble(PrDomEvent* event, bool value);

// Stops propagation, and stops the listeners of the current target that have not run yet
// (dom/#dom-event-stopimmediatepropagation).
void prDomEventStopImmediatePropagation(PrDomEvent* event);

// Return true if the event bubbles, and true if it can be canceled.
bool prDomEventBubbles(const PrDomEvent* event);
bool prDomEventCancelable(const PrDomEvent* event);

// Returns true if the event is not canceled (dom/#dom-event-returnvalue).
bool prDomEventReturnValue(const PrDomEvent* event);

// Cancels the event in the same way as prDomEventPreventDefault if `value` is false. True does nothing.
void prDomEventSetReturnValue(PrDomEvent* event, bool value);

// Cancels the event, unless it cannot be canceled or a passive listener is running (dom/#dom-event-preventdefault).
void prDomEventPreventDefault(PrDomEvent* event);

// Returns true if the event is canceled.
bool prDomEventDefaultPrevented(const PrDomEvent* event);

// Returns true if the event propagates from a shadow tree to the tree of its host (dom/#dom-event-composed).
bool prDomEventComposed(const PrDomEvent* event);

// Returns true if the engine fired the event, rather than a caller or a script (dom/#dom-event-istrusted).
bool prDomEventIsTrusted(const PrDomEvent* event);

// Returns the time at which the event was created, in milliseconds of the engine's monotonic clock.
double prDomEventTimeStamp(const PrDomEvent* event);

// Sets the event's type, bubbles and cancelable, and clears its target and the flags of a previous dispatch
// (dom/#dom-event-initevent). Does nothing while the event is being dispatched.
void prDomEventInitEvent(PrDomEvent* event, PrStringView type, bool bubbles, bool cancelable);

// Returns the most specific interface that the event implements.
PrDomEventKind prDomEventKind(const PrDomEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// CustomEvent : Event (dom)
//
// The caller holds the detail, which is a value of the caller's language.

// Converts a CustomEvent to the interface that it inherits from.
PR_HANDLE_UPCAST(DomCustomEvent, DomEvent, Event)

// Returns an Event as a CustomEvent, or null if it is not one.
PrDomCustomEvent* PR_NULLABLE prDomCustomEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a CustomEvent of `type`, in the same way as prDomEventCreate.
PrDomCustomEvent* prDomCustomEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init);

// Initializes the event in the same way as prDomEventInitEvent (dom/#dom-customevent-initcustomevent). If this returns
// true, the caller sets the detail. Returns false while the event is being dispatched.
bool prDomCustomEventInitCustomEvent(PrDomCustomEvent* event, PrStringView type, bool bubbles, bool cancelable);

// ---------------------------------------------------------------------------------------------------------------------
// UIEvent : Event (uievents)
//
// The view is a Window. The event holds no reference to it.

// Converts a UIEvent to the interface that it inherits from.
PR_HANDLE_UPCAST(DomUiEvent, DomEvent, Event)

// The members of UIEventInit (uievents/#dictdef-uieventinit), with the legacy `which`
// (uievents/#legacy-interface-UIEvent).
typedef struct PrDomUiEventInit {
    PrDomEventInit event;
    PrDomEventTarget* PR_NULLABLE view;
    int32_t detail;
    uint32_t which;
} PrDomUiEventInit;

// Returns an init with every member false, null or zero.
static inline PrDomUiEventInit prDomUiEventInitDefault(void) {
    PrDomUiEventInit value;
    value.event = prDomEventInitDefault();
    value.view = NULL;
    value.detail = 0;
    value.which = 0;
    return value;
}

// Returns an Event as a UIEvent, or null if it is not one.
PrDomUiEvent* PR_NULLABLE prDomUiEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a UIEvent of `type`, with one reference that the caller holds. A null `init` means prDomUiEventInitDefault.
PrDomUiEvent* prDomUiEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init);

// Return the event's view, or null if it has none, and its detail.
PrDomEventTarget* PR_NULLABLE prDomUiEventView(PrDomUiEvent* event);
int32_t prDomUiEventDetail(const PrDomUiEvent* event);

// Returns the legacy code of the event (uievents/#dom-uievent-which): one more than the button of a MouseEvent, the key
// code of a KeyboardEvent, or its character code if it has no key code, and the value that the init gave any other
// event.
uint32_t prDomUiEventWhich(const PrDomUiEvent* event);

// Initializes the event with the steps of the legacy initUIEvent (uievents/#dom-uievent-inituievent), from the type,
// bubbles, cancelable, view and detail of `init`. Does nothing while the event is being dispatched.
void prDomUiEventInitUiEvent(PrDomUiEvent* event, PrStringView type, const PrDomUiEventInit* init);

// ---------------------------------------------------------------------------------------------------------------------
// FocusEvent : UIEvent (uievents)
//
// The related target is retargeted as the event is dispatched (dom/#concept-event-dispatch).

// Converts a FocusEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomFocusEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomFocusEvent, DomEvent, Event)

// The members of FocusEventInit (uievents/#dictdef-focuseventinit).
typedef struct PrDomFocusEventInit {
    PrDomUiEventInit ui;
    PrDomEventTarget* PR_NULLABLE relatedTarget;
} PrDomFocusEventInit;

// Returns an init with every member false, null or zero.
static inline PrDomFocusEventInit prDomFocusEventInitDefault(void) {
    PrDomFocusEventInit value;
    value.ui = prDomUiEventInitDefault();
    value.relatedTarget = NULL;
    return value;
}

// Returns an Event as a FocusEvent, or null if it is not one.
PrDomFocusEvent* PR_NULLABLE prDomFocusEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a FocusEvent of `type`, with one reference that the caller holds. A null `init` means
// prDomFocusEventInitDefault.
PrDomFocusEvent* prDomFocusEventCreate(PrStringView type, const PrDomFocusEventInit* PR_NULLABLE init);

// Returns the related target, as the current dispatch retargets it, or null if there is none.
PrDomEventTarget* PR_NULLABLE prDomFocusEventRelatedTarget(PrDomFocusEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// MouseEvent : UIEvent (pointerevents4)
//
// Positions are in CSS pixels, relative to the screen and to the viewport
// (cssom-view-1/#extensions-to-the-mouseevent-interface). The related target is retargeted as the event is dispatched
// (dom/#concept-event-dispatch). A click that is a MouseEvent runs the activation behavior of its target or of the
// nearest inclusive ancestor that has one (dom/#eventtarget-activation-behavior).

// Converts a MouseEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomMouseEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomMouseEvent, DomEvent, Event)

// The modifier keys that an event reports (uievents/#dictdef-eventmodifierinit), each named by its key value
// (uievents-key/#keys-modifier).
enum {
    PrDomModifier_Control = 1 << 0,
    PrDomModifier_Shift = 1 << 1,
    PrDomModifier_Alt = 1 << 2,
    PrDomModifier_Meta = 1 << 3,
    PrDomModifier_AltGraph = 1 << 4,
    PrDomModifier_CapsLock = 1 << 5,
    PrDomModifier_Fn = 1 << 6,
    PrDomModifier_FnLock = 1 << 7,
    PrDomModifier_Hyper = 1 << 8,
    PrDomModifier_NumLock = 1 << 9,
    PrDomModifier_ScrollLock = 1 << 10,
    PrDomModifier_Super = 1 << 11,
    PrDomModifier_Symbol = 1 << 12,
    PrDomModifier_SymbolLock = 1 << 13,
};
typedef uint16_t PrDomModifiers;

// Returns the modifier that a key value names, in the form that getModifierState takes, or zero if it names none.
PrDomModifiers prDomModifiersFromKey(PrStringView key);

// The members of MouseEventInit (pointerevents4/#dom-mouseeventinit), and of the EventModifierInit that it extends
// (uievents/#dictdef-eventmodifierinit).
typedef struct PrDomMouseEventInit {
    PrDomUiEventInit ui;
    PrDomModifiers modifiers;
    double screenX;
    double screenY;
    double clientX;
    double clientY;
    int16_t button;                 // button that changed: 0 the main, 1 the auxiliary, 2 the secondary
    uint16_t buttons;               // buttons held, one bit each: 1 the main, 2 the secondary, 4 the auxiliary
    PrDomEventTarget* PR_NULLABLE relatedTarget;
} PrDomMouseEventInit;

// Returns an init with every member false, null or zero.
static inline PrDomMouseEventInit prDomMouseEventInitDefault(void) {
    PrDomMouseEventInit value;
    value.ui = prDomUiEventInitDefault();
    value.modifiers = 0;
    value.screenX = 0;
    value.screenY = 0;
    value.clientX = 0;
    value.clientY = 0;
    value.button = 0;
    value.buttons = 0;
    value.relatedTarget = NULL;
    return value;
}

// Returns an Event as a MouseEvent, or null if it is not one.
PrDomMouseEvent* PR_NULLABLE prDomMouseEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a MouseEvent of `type`, with one reference that the caller holds. A null `init` means
// prDomMouseEventInitDefault.
PrDomMouseEvent* prDomMouseEventCreate(PrStringView type, const PrDomMouseEventInit* PR_NULLABLE init);

// Returns the event's members, as its init gave them. The related target is retargeted by the current dispatch.
PrDomMouseEventInit prDomMouseEventMembers(PrDomMouseEvent* event);

// Initializes the event with the steps of the legacy initMouseEvent (pointerevents4/#dom-mouseevent-initmouseevent),
// from every member of `init` except composed and buttons, which the event keeps. Does nothing while the event is being
// dispatched.
void prDomMouseEventInitMouseEvent(PrDomMouseEvent* event, PrStringView type, const PrDomMouseEventInit* init);

// ---------------------------------------------------------------------------------------------------------------------
// WheelEvent : MouseEvent (pointerevents4)

// Converts a WheelEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomWheelEvent, DomMouseEvent, MouseEvent)
PR_HANDLE_UPCAST(DomWheelEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomWheelEvent, DomEvent, Event)

// The units of a WheelEvent's deltas (pointerevents4/#dom-wheelevent-deltamode). An init may give any other value.
enum {
    PrDomDeltaMode_Pixel = 0,
    PrDomDeltaMode_Line = 1,
    PrDomDeltaMode_Page = 2,
};
typedef uint32_t PrDomDeltaMode;

// The members of WheelEventInit (pointerevents4/#interface-wheelevent). A delta is positive right, down and away from
// the user.
typedef struct PrDomWheelEventInit {
    PrDomMouseEventInit mouse;
    double deltaX;
    double deltaY;
    double deltaZ;
    PrDomDeltaMode deltaMode;
} PrDomWheelEventInit;

// Returns an init with every member false, null or zero.
static inline PrDomWheelEventInit prDomWheelEventInitDefault(void) {
    PrDomWheelEventInit value;
    value.mouse = prDomMouseEventInitDefault();
    value.deltaX = 0;
    value.deltaY = 0;
    value.deltaZ = 0;
    value.deltaMode = PrDomDeltaMode_Pixel;
    return value;
}

// Returns an Event as a WheelEvent, or null if it is not one.
PrDomWheelEvent* PR_NULLABLE prDomWheelEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a WheelEvent of `type`, with one reference that the caller holds. A null `init` means
// prDomWheelEventInitDefault.
PrDomWheelEvent* prDomWheelEventCreate(PrStringView type, const PrDomWheelEventInit* PR_NULLABLE init);

// Returns the event's members, as its init gave them. The related target is retargeted by the current dispatch.
PrDomWheelEventInit prDomWheelEventMembers(PrDomWheelEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// KeyboardEvent : UIEvent (uievents)
//
// A key is identified by its meaning (uievents-key) and its position (uievents-code). The legacy character and key
// codes are those of uievents/#legacy-key-models.

// Converts a KeyboardEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomKeyboardEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomKeyboardEvent, DomEvent, Event)

// Where a key is among the keys that share its key value (uievents/#events-keyboard-key-location). An init may give any
// other value.
enum {
    PrDomKeyLocation_Standard = 0,
    PrDomKeyLocation_Left = 1,
    PrDomKeyLocation_Right = 2,
    PrDomKeyLocation_Numpad = 3,
};
typedef uint32_t PrDomKeyLocation;

// The members of KeyboardEventInit (uievents/#dictdef-keyboardeventinit), with those of the EventModifierInit that it
// extends and the legacy codes (uievents/#legacy-dictionary-KeyboardEventInit).
typedef struct PrDomKeyboardEventInit {
    PrDomUiEventInit ui;
    PrDomModifiers modifiers;
    PrStringView key;
    PrStringView code;
    PrDomKeyLocation location;
    bool repeat;
    bool isComposing;
    uint32_t charCode;
    uint32_t keyCode;
} PrDomKeyboardEventInit;

// Returns an init with every member false, empty or zero.
static inline PrDomKeyboardEventInit prDomKeyboardEventInitDefault(void) {
    PrDomKeyboardEventInit value;
    value.ui = prDomUiEventInitDefault();
    value.modifiers = 0;
    value.key.data = "";
    value.key.size = 0;
    value.code.data = "";
    value.code.size = 0;
    value.location = PrDomKeyLocation_Standard;
    value.repeat = false;
    value.isComposing = false;
    value.charCode = 0;
    value.keyCode = 0;
    return value;
}

// Returns an Event as a KeyboardEvent, or null if it is not one.
PrDomKeyboardEvent* PR_NULLABLE prDomKeyboardEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a KeyboardEvent of `type`, with one reference that the caller holds. A null `init` means
// prDomKeyboardEventInitDefault.
PrDomKeyboardEvent* prDomKeyboardEventCreate(PrStringView type, const PrDomKeyboardEventInit* PR_NULLABLE init);

// Returns the event's members, as its init gave them. The key and the code are valid until the event changes.
PrDomKeyboardEventInit prDomKeyboardEventMembers(PrDomKeyboardEvent* event);

// Initializes the event with the steps of the legacy initKeyboardEvent (uievents/#dom-keyboardevent-initkeyboardevent),
// from the type, bubbles, cancelable, view, key, location and modifiers of `init`. The event keeps its other members,
// and its detail becomes zero. Does nothing while the event is being dispatched.
void prDomKeyboardEventInitKeyboardEvent(PrDomKeyboardEvent* event, PrStringView type, const PrDomKeyboardEventInit* init);

// ---------------------------------------------------------------------------------------------------------------------
// ClipboardEvent : Event, and its DataTransfer (clipboard-apis)
//
// The transfer holds the event's text, as text/plain.

// Converts a ClipboardEvent to the interface that it inherits from.
PR_HANDLE_UPCAST(DomClipboardEvent, DomEvent, Event)

// Returns an Event as a ClipboardEvent, or null if it is not one.
PrDomClipboardEvent* PR_NULLABLE prDomClipboardEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a ClipboardEvent of `type` whose transfer holds `text`.
PrDomClipboardEvent* prDomClipboardEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init, PrStringView text);

// Returns the event's transfer, which the event holds a reference to.
PrDomDataTransfer* prDomClipboardEventClipboardData(const PrDomClipboardEvent* event);

// Adds or releases a reference to the transfer.
void prDomDataTransferRetain(PrDomDataTransfer* transfer);
void prDomDataTransferRelease(PrDomDataTransfer* PR_NULLABLE transfer);

// Returns or replaces the transfer's text. A transfer has text once it is set, even to an empty string.
PrStringView prDomDataTransferText(const PrDomDataTransfer* transfer);
bool prDomDataTransferHasText(const PrDomDataTransfer* transfer);
void prDomDataTransferSetText(PrDomDataTransfer* transfer, PrStringView text);
void prDomDataTransferClear(PrDomDataTransfer* transfer);

// InputEvent : UIEvent (uievents)

// Converts an InputEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomInputEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomInputEvent, DomEvent, Event)

// Returns an Event as an InputEvent, or null if it is not one.
PrDomInputEvent* PR_NULLABLE prDomInputEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates an InputEvent of `type` (uievents/#dictdef-inputeventinit). A null `data` is a null data attribute.
PrDomInputEvent* prDomInputEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init, PrStringView data,
    PrStringView inputType, bool isComposing);

// The attributes of the event. `data` is the null view if the event has no data.
PrStringView prDomInputEventData(const PrDomInputEvent* event);
PrStringView prDomInputEventInputType(const PrDomInputEvent* event);
bool prDomInputEventIsComposing(const PrDomInputEvent* event);

// CompositionEvent : UIEvent (uievents)

// Converts a CompositionEvent to each interface that it inherits from.
PR_HANDLE_UPCAST(DomCompositionEvent, DomUiEvent, UiEvent)
PR_HANDLE_UPCAST(DomCompositionEvent, DomEvent, Event)

// Returns an Event as a CompositionEvent, or null if it is not one.
PrDomCompositionEvent* PR_NULLABLE prDomCompositionEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a CompositionEvent of `type` whose data is `data` (uievents/#dictdef-compositioneventinit), with one
// reference that the caller holds. A null `init` means prDomUiEventInitDefault.
PrDomCompositionEvent* prDomCompositionEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init,
    PrStringView data);

// Returns the text that the input method generates, valid until the event changes.
PrStringView prDomCompositionEventData(const PrDomCompositionEvent* event);

// Initializes the event with the steps of the legacy initCompositionEvent
// (uievents/#dom-compositionevent-initcompositionevent), from the type, bubbles, cancelable and view of `init`, and
// `data`. The event's detail becomes zero. Does nothing while the event is being dispatched.
void prDomCompositionEventInitCompositionEvent(PrDomCompositionEvent* event, PrStringView type,
    const PrDomUiEventInit* init, PrStringView data);

// ---------------------------------------------------------------------------------------------------------------------
// EventListener (dom)

// Called with each event that the listener receives.
typedef void (*PrDomEventListenerFn)(void* PR_NULLABLE context, PrDomEvent* event);

// A listener's callback. Two registrations name the same listener if their invoke and identity are the same, whatever
// their context. The identity must therefore stay the same while the listener is registered. A registration keeps a
// copy of the listener, and calls retain and release on the copy's context if they are given.
struct PrDomEventListener {
    PrDomEventListenerFn invoke;
    void* PR_NULLABLE context;
    uint64_t identity;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
};

// ---------------------------------------------------------------------------------------------------------------------
// EventTarget (dom)

// The options of an added listener (dom/#dictdef-addeventlisteneroptions). A listener with a signal is not added if the
// signal is aborted, and is removed when it aborts (pr/dom_abort.h).
typedef struct PrDomAddEventListenerOptions {
    bool capture;
    bool passive;
    bool once;
    PrDomAbortSignal* PR_NULLABLE signal;
} PrDomAddEventListenerOptions;

// Returns the default options of a listener for `type` added to `target`. Its passive value is
// dom/#default-passive-value.
PrDomAddEventListenerOptions prDomAddEventListenerOptionsDefault(const PrDomEventTarget* target, PrStringView type);

// The options of a removed listener (dom/#dictdef-eventlisteneroptions).
typedef struct PrDomEventListenerOptions {
    bool capture;
} PrDomEventListenerOptions;

// Returns the default options, which are not capturing.
static inline PrDomEventListenerOptions prDomEventListenerOptionsDefault(void) {
    PrDomEventListenerOptions value;
    value.capture = false;
    return value;
}

// Creates a standalone EventTarget, with one reference that the caller holds.
PrDomEventTarget* prDomEventTargetCreate(void);

// Adds or releases a reference to any event target. A reference to a node keeps its tree alive, in the same way as
// prDomNodeRetain. The last release frees a standalone EventTarget. A reference to an object that a window presents as
// a member, such as its Performance, is a reference to the window, which lives while references to it remain.
void prDomEventTargetRetain(PrDomEventTarget* target);
void prDomEventTargetRelease(PrDomEventTarget* PR_NULLABLE target);

// Returns the slot in which the script binding of the target's agent stores the target's script object. The slot is
// null while the target has no object. The engine reads the slot only to decide whether to report a node's moves
// (PrDomTreeCallback).
void* PR_NULLABLE* PR_NONNULL prDomEventTargetBinding(PrDomEventTarget* target);

// Add `callback` as a listener for `type` (dom/#add-an-event-listener), or remove it (dom/#remove-an-event-listener).
// Adding does nothing if the same listener is registered for `type` with the same capture. A null callback does
// nothing. Null options mean the defaults.
void prDomEventTargetAddEventListener(PrDomEventTarget* target, PrStringView type,
    const PrDomEventListener* PR_NULLABLE callback, const PrDomAddEventListenerOptions* PR_NULLABLE options);
void prDomEventTargetRemoveEventListener(PrDomEventTarget* target, PrStringView type,
    const PrDomEventListener* PR_NULLABLE callback, const PrDomEventListenerOptions* PR_NULLABLE options);

// Dispatches `event` at the target as an untrusted event (dom/#dom-eventtarget-dispatchevent). If `out` is given,
// writes true to it if no listener canceled the event. Returns InvalidState for an event that is being dispatched or is
// not initialized.
PrStatus prDomEventTargetDispatchEvent(PrDomEventTarget* target, PrDomEvent* event, bool* PR_NULLABLE out);

// ---------------------------------------------------------------------------------------------------------------------
// EventHandler (html)
//
// An event handler is a member of an event target whose value is a callback or null (html/#event-handlers). Setting it
// from null to a callback adds a listener for its events to the end of the target's list. That listener runs whatever
// callback the handler holds at the time. Setting it to null removes that listener.

// The value that an event handler's callback returns, as the processing algorithm reads it
// (html/#the-event-handler-processing-algorithm).
enum {
    PrDomEventHandlerResult_Other,
    PrDomEventHandlerResult_True,
    PrDomEventHandlerResult_False,
};
typedef uint8_t PrDomEventHandlerResult;

// Called with each event that the handler receives. `error` is set when the handler belongs to a Window and the event
// is an ErrorEvent of type error. A script callback then receives the event's message, filename, lineno, colno and
// error instead of the event.
typedef PrDomEventHandlerResult (*PrDomEventHandlerFn)(void* PR_NULLABLE context, PrDomEvent* event, bool error);

// The callback of an event handler. The target keeps a copy, and calls retain and release on the copy's context if they
// are given.
typedef struct PrDomEventHandlerCallback {
    PrDomEventHandlerFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrDomEventHandlerCallback;

// Get and set the callback of the target's event handler `handler`. The getter returns a null invoke if the handler is
// null. A null `callback` sets the handler to null. The WindowEventHandlers of a body or frameset element, and the
// handlers that the Window reflects, belong to the element's Window
// (html/#window-reflecting-body-element-event-handler-set). The setter does not set them if the element has no Window.
PrDomEventHandlerCallback prDomEventTargetEventHandler(PrDomEventTarget* target, PrDomEventHandler handler);
void prDomEventTargetSetEventHandler(PrDomEventTarget* eventTarget, PrDomEventHandler handler,
    const PrDomEventHandlerCallback* PR_NULLABLE callback);

// ---------------------------------------------------------------------------------------------------------------------
// Node : EventTarget (dom)
//
// A call that changes the tree returns NotAllowed for a change that would break the tree's hierarchy
// (dom/#concept-node-ensure-pre-insertion-validity), and NotFound for a reference child that is not the parent's child.

// The type of a node (dom/#dom-node-nodetype).
enum {
    PrDomNodeType_Element               = 1,
    PrDomNodeType_Attribute             = 2,
    PrDomNodeType_Text                  = 3,
    PrDomNodeType_CdataSection          = 4,
    PrDomNodeType_EntityReference       = 5,
    PrDomNodeType_Entity                = 6,
    PrDomNodeType_ProcessingInstruction = 7,
    PrDomNodeType_Comment               = 8,
    PrDomNodeType_Document              = 9,
    PrDomNodeType_DocumentType          = 10,
    PrDomNodeType_DocumentFragment      = 11,
    PrDomNodeType_Notation              = 12,
};
typedef uint16_t PrDomNodeType;

// The bits of a document position (dom/#dom-node-document_position_disconnected).
enum {
    PrDomNodePosition_Disconnected           = 0x01,
    PrDomNodePosition_Preceding              = 0x02,
    PrDomNodePosition_Following              = 0x04,
    PrDomNodePosition_Contains               = 0x08,
    PrDomNodePosition_ContainedBy            = 0x10,
    PrDomNodePosition_ImplementationSpecific = 0x20,
};
typedef uint16_t PrDomNodePosition;

// Converts a Node to the interface that it inherits from.
PR_HANDLE_UPCAST(DomNode, DomEventTarget, EventTarget)

// Returns an EventTarget as a Node, or null if it is not one.
PrDomNode* PR_NULLABLE prDomNodeQuery(PrDomEventTarget* PR_NULLABLE object);

// Adds or releases a reference. A reference to a node keeps its tree alive. A tree other than the document's own tree
// keeps the document alive, whichever document that is after adoption. A tree is freed when its last reference is
// released. A node removed from its tree with no references is freed immediately.
void prDomNodeRetain(PrDomNode* node);
void prDomNodeRelease(PrDomNode* PR_NULLABLE node);

// Returns the root of the node's tree: its document, or the unowned node that contains it.
PrDomNode* prDomNodeTreeRoot(PrDomNode* node);

// Returns the node that holds the node in its tree: its parent, an attribute's element, or the host of template
// contents or a shadow root. Returns null for the root.
PrDomNode* PR_NULLABLE prDomNodeTreeParent(PrDomNode* node);

// Returns the node's type.
PrDomNodeType prDomNodeType(const PrDomNode* node);

// Returns the base URL of the node's document, serialized (dom/#dom-node-baseuri).
PrRawString prDomNodeBaseUri(const PrDomNode* node);

// Returns true if the node's shadow-including root is a document (dom/#connected).
bool prDomNodeIsConnected(const PrDomNode* node);

// Returns the node's document, or null for a document.
PrDomDocument* PR_NULLABLE prDomNodeOwnerDocument(PrDomNode* node);

// Calls `visit` with the node, its shadow-including descendants (dom/#concept-shadow-including-descendant), and the
// attributes of those that are elements. These are the nodes that move with it to another document.
void prDomNodeVisitSubtree(PrDomNode* root, void (*visit)(void* PR_NULLABLE context, PrDomNode* node), void* PR_NULLABLE context);

// Return the node's parent, and the parent if it is an element. Each is null if there is none. An attribute has no
// parent.
PrDomNode* PR_NULLABLE prDomNodeParentNode(PrDomNode* node);
PrDomElement* PR_NULLABLE prDomNodeParentElement(PrDomNode* node);

// Returns true if the node has children.
bool prDomNodeHasChildNodes(const PrDomNode* node);

// Return the node's first and last child, and its previous and next sibling. Each is null if there is none.
PrDomNode* PR_NULLABLE prDomNodeFirstChild(PrDomNode* node);
PrDomNode* PR_NULLABLE prDomNodeLastChild(PrDomNode* node);
PrDomNode* PR_NULLABLE prDomNodePreviousSibling(PrDomNode* node);
PrDomNode* PR_NULLABLE prDomNodeNextSibling(PrDomNode* node);

// Returns the node's text content (dom/#dom-node-textcontent). This is the text of its descendants for an element or a
// fragment, the value for an attribute or character data, and the null string for any other node.
PrRawString prDomNodeTextContent(const PrDomNode* node);

// Sets the node's text content. An element or a fragment gets a Text node of `value` as its only child, or no children
// for the empty string. An attribute or character data gets `value` as its value. Any other node is unchanged.
void prDomNodeSetTextContent(PrDomNode* node, PrStringView value);

// Returns the concatenation of the data of the node's Text children (dom/#concept-child-text-content).
PrRawString prDomNodeChildTextContent(const PrDomNode* node);

// Inserts `node` before `child`, or last for a null `child` (dom/#dom-node-insertbefore).
PrStatus prDomNodeInsertBefore(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child);

// Inserts `node` as the last child (dom/#dom-node-appendchild).
PrStatus prDomNodeAppendChild(PrDomNode* parent, PrDomNode* node);

// Replaces `child` with `node` (dom/#dom-node-replacechild).
PrStatus prDomNodeReplaceChild(PrDomNode* parent, PrDomNode* node, PrDomNode* child);

// Removes `child` (dom/#dom-node-removechild).
PrStatus prDomNodeRemoveChild(PrDomNode* node, PrDomNode* child);

// Returns the node's name (dom/#dom-node-nodename).
PrRawString prDomNodeName(const PrDomNode* node);

// Returns the name shared by every node of `type` (dom/#dom-node-nodename). The text lives as long as the process.
// Returns the null view for a type whose nodes each have their own name.
PrStringView prDomNodeTypeName(PrDomNodeType type);

// Returns the node's value: an attribute's value or character data's data, and the null view for any other node
// (dom/#dom-node-nodevalue).
PrStringView prDomNodeValue(const PrDomNode* node);

// Sets the node's value. Does nothing for a node that has no value. The null view sets the empty string.
void prDomNodeSetValue(PrDomNode* node, PrStringView value);

// Returns the root of the node or, if `composed` is set, its shadow-including root (dom/#dom-node-getrootnode).
PrDomNode* prDomNodeGetRootNode(PrDomNode* node, bool composed);

// Return true if `other` is the node or one of its descendants (dom/#dom-node-contains), and true if `other` is a node
// equal to it (dom/#concept-node-equals). Both return false for null.
bool prDomNodeContains(const PrDomNode* node, const PrDomNode* PR_NULLABLE other);
bool prDomNodeIsEqualNode(const PrDomNode* node, const PrDomNode* PR_NULLABLE other);

// Returns the PrDomNodePosition bits that describe the position of `other` relative to the node
// (dom/#dom-node-comparedocumentposition).
PrDomNodePosition prDomNodeCompareDocumentPosition(const PrDomNode* node, const PrDomNode* other);

// Removes the empty Text nodes among the node's descendants, and merges each run of adjacent Text nodes into the first
// node of the run (dom/#dom-node-normalize).
void prDomNodeNormalize(PrDomNode* node);

// Clones the node into its document, with its descendants if `deep` is set (dom/#dom-node-clonenode). The clone comes
// with a reference that the caller releases. Returns NotSupported for a document or an attribute.
PrStatus prDomNodeCloneNode(PrDomNode* node, bool deep, PrDomNode* PR_NULLABLE* PR_NONNULL out);

// An argument of the methods that take nodes or strings (dom/#convert-nodes-into-a-node): the node, or the string if
// the node is null. A string stands for a Text node with that data.
typedef struct PrDomNodeOrString {
    PrDomNode* PR_NULLABLE node;
    PrStringView string;
} PrDomNodeOrString;

// Returns the first element among the node's descendants whose ID is `id`, or null. The node is a document or a
// fragment (dom/#interface-nonelementparentnode).
PrDomElement* PR_NULLABLE prDomNodeGetElementById(PrDomNode* node, PrStringView id);

// Return the first and the last of the node's element children, and their number. The node is a document, fragment or
// element (dom/#interface-parentnode).
PrDomElement* PR_NULLABLE prDomNodeFirstElementChild(PrDomNode* node);
PrDomElement* PR_NULLABLE prDomNodeLastElementChild(PrDomNode* node);
uint32_t prDomNodeChildElementCount(const PrDomNode* node);

// Insert `nodes` before the first child (dom/#dom-parentnode-prepend), after the last child
// (dom/#dom-parentnode-append), or in place of all the children (dom/#dom-parentnode-replacechildren).
PrStatus prDomNodePrepend(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);
PrStatus prDomNodeAppend(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);
PrStatus prDomNodeReplaceChildren(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Return the nearest previous and next sibling that is an element, or null. The node is an element or character data
// (dom/#interface-nondocumenttypechildnode).
PrDomElement* PR_NULLABLE prDomNodePreviousElementSibling(PrDomNode* node);
PrDomElement* PR_NULLABLE prDomNodeNextElementSibling(PrDomNode* node);

// Insert `nodes` before the node (dom/#dom-childnode-before), after it (dom/#dom-childnode-after), or in its place
// (dom/#dom-childnode-replacewith). The node is an element or character data (dom/#interface-childnode).
PrStatus prDomNodeBefore(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);
PrStatus prDomNodeAfter(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);
PrStatus prDomNodeReplaceWith(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count);

// Removes the node from its parent, if it has one (dom/#dom-childnode-remove).
void prDomNodeRemove(PrDomNode* node);

// Return the prefix that is in scope at the node for `namespaceUri`, or the namespace that is in scope for `prefix`
// (dom/#locate-a-namespace-prefix, dom/#locate-a-namespace). These return the null view if there is none. An empty
// argument stands for null. The view is valid until the node's tree changes.
PrStringView prDomNodeLookupPrefix(PrDomNode* node, PrStringView namespaceUri);
PrStringView prDomNodeLookupNamespaceUri(PrDomNode* node, PrStringView prefix);

// Returns true if `namespaceUri` is the default namespace at the node (dom/#dom-node-isdefaultnamespace).
bool prDomNodeIsDefaultNamespace(PrDomNode* node, PrStringView namespaceUri);

// ---------------------------------------------------------------------------------------------------------------------
// Document : Node (dom)
//
// A node that a document creates comes with a reference that the caller releases.

// The options of createElement (dom/#dictdef-elementcreationoptions).
typedef struct PrDomElementCreationOptions {
    PrStringView is;                        // name of a customized built-in element, or the null view for none
} PrDomElementCreationOptions;

// Returns the default options, which name no customized built-in element.
static inline PrDomElementCreationOptions prDomElementCreationOptionsDefault(void) {
    PrDomElementCreationOptions value;
    value.is = PR_LITERAL(PrStringView){NULL, 0};
    return value;
}

// Converts a Document to each interface that it inherits from.
PR_HANDLE_UPCAST(DomDocument, DomNode, Node)
PR_HANDLE_UPCAST(DomDocument, DomEventTarget, EventTarget)

// Returns an EventTarget as a Document, or null if it is not one.
PrDomDocument* PR_NULLABLE prDomDocumentQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates an XML document (dom/#dom-document-document), with one reference that the caller holds.
PrDomDocument* prDomDocumentCreate(void);

// Creates an XML document for the implementation's document, with one reference that the caller holds
// (dom/#dom-domimplementation-createdocument). It contains `doctype` unless `doctype` is null, followed by the element
// that `qualifiedName` names in `namespaceUrl` unless the name is empty. A name that does not validate fails in the
// same way as for prDomElementCreateNs. The new document has what the implementation's document was given to run with,
// such as CSS (prCssEnable).
PrStatus prDomDocumentCreateXml(PrDomImplementation* implementation, PrStringView namespaceUrl,
    PrStringView qualifiedName, PrDomDocumentType* PR_NULLABLE doctype, PrDomDocument* PR_NULLABLE* PR_NONNULL out);

// Creates an HTML document for the implementation's document with a doctype, html, head and body elements, with one
// reference that the caller holds (dom/#dom-domimplementation-createhtmldocument). It has a title element containing
// `title` unless `title` is the null view. The new document has what the implementation's document was given to run
// with, such as CSS (prCssEnable).
PrDomDocument* prDomDocumentCreateHtml(PrDomImplementation* implementation, PrStringView title);

// Adds or releases a reference to the document as a node (prDomNodeRetain).
void prDomDocumentRetain(PrDomDocument* document);
void prDomDocumentRelease(PrDomDocument* PR_NULLABLE document);

// The states of an element that only the application knows. The host reports them for an element on request
// (PrHtmlHost::elementState). The engine decides which elements a state can apply to.
enum {
    PrDomHostState_Visited = 1 << 0,    // a link the user has visited (html/#selector-visited)
    PrDomHostState_Autofill = 1 << 1,   // an input the user agent has autofilled (html/#selector-autofill)
};
typedef uint8_t PrDomHostState;

// Notifications about the nodes of a document.
//
// `moved` is called once a node with a script object (prDomEventTargetBinding) has moved. The node has a new tree
// parent (prDomNodeTreeParent), or none, or, as a root, a new document. The call is for the node that moved and not for
// its descendants, whose tree parents are unchanged. The callback is the one that belongs to the document that owns the
// node after the move. A document that the node left is not destroyed before the call.
//
// `held` is called with true when the engine takes its first hold on a node, and with false when it releases its last
// hold. The engine holds a node while it has pending work that will give the node to script again.
typedef struct PrDomTreeCallback {
    void (*PR_NULLABLE moved)(void* PR_NULLABLE context, PrDomNode* node);
    void (*PR_NULLABLE held)(void* PR_NULLABLE context, PrDomNode* node, bool held);
    void* PR_NULLABLE context;
} PrDomTreeCallback;

// Sets the document's tree callback, or clears it if `callback` is null. `held` of the new callback is called at once
// for each node that the engine holds. The callback that it replaces is not called again.
void prDomDocumentSetTreeCallback(PrDomDocument* document, const PrDomTreeCallback* PR_NULLABLE callback);

// Returns the document's URL, serialized (dom/#dom-document-url). It is also dom/#dom-document-documenturi.
PrRawString prDomDocumentUrl(const PrDomDocument* document);

// Returns the document's content type (dom/#dom-document-contenttype).
PrStringView prDomDocumentContentType(const PrDomDocument* document);

// Returns the name of the document's encoding (dom/#dom-document-characterset), which is UTF-8 unless a load decoded
// the document from another charset. It is also dom/#dom-document-charset and dom/#dom-document-inputencoding.
PrStringView prDomDocumentCharacterSet(const PrDomDocument* document);

// Returns the document element, which is the first child that is an element, or null.
PrDomElement* PR_NULLABLE prDomDocumentElement(PrDomDocument* document);

// Clones `node` into the document, with its descendants if `deep` is set (dom/#dom-document-importnode). The clone is a
// detached root that the caller owns. Returns NotSupported for a document or a shadow root, and sets `*out` to null.
PrStatus prDomDocumentImportNode(PrDomDocument* document, PrDomNode* node, bool deep, PrDomNode* PR_NULLABLE* PR_NONNULL out);

// Moves `node` and its subtree into the document, and removes it from its parent or, for an attribute, from its element
// (dom/#dom-document-adoptnode). Returns NotSupported for a document, and NotAllowed for a shadow root.
PrStatus prDomDocumentAdoptNode(PrDomDocument* document, PrDomNode* node);

// Creates an element whose local name is `localName` (dom/#dom-document-createelement). An HTML document lowercases the
// name. An HTML or XHTML document creates an HTML element, and any other document an element in no namespace. Returns
// InvalidData if `localName` is not a valid element local name.
PrStatus prDomElementCreate(PrDomDocument* document, PrStringView localName,
    const PrDomElementCreationOptions* PR_NULLABLE options, PrDomElement* PR_NULLABLE* PR_NONNULL out);

// Creates an element in `namespaceUrl` named `qualifiedName` (dom/#dom-document-createelementns). The null view and the
// empty string both mean no namespace. Returns InvalidData if `qualifiedName` is not a qualified name with a valid
// element local name, and NotAllowed if `namespaceUrl` does not accept it (dom/#validate-and-extract).
PrStatus prDomElementCreateNs(PrDomDocument* document, PrStringView namespaceUrl, PrStringView qualifiedName,
    const PrDomElementCreationOptions* PR_NULLABLE options, PrDomElement* PR_NULLABLE* PR_NONNULL out);

// Creates an attribute in no namespace whose local name is `localName` (dom/#dom-document-createattribute). An HTML
// document lowercases the name. Returns InvalidData if `localName` is not a valid attribute local name.
PrStatus prDomAttrCreate(PrDomDocument* document, PrStringView localName, PrDomAttr* PR_NULLABLE* PR_NONNULL out);

// Creates an attribute in `namespaceUrl` named `qualifiedName` (dom/#dom-document-createattributens). Fails in the same
// cases as prDomElementCreateNs, applied to an attribute's name.
PrStatus prDomAttrCreateNs(PrDomDocument* document, PrStringView namespaceUrl, PrStringView qualifiedName,
    PrDomAttr* PR_NULLABLE* PR_NONNULL out);

// Create an empty fragment, and a Text node with `data`.
PrDomDocumentFragment* prDomDocumentFragmentCreate(PrDomDocument* document);
PrDomText* prDomTextCreate(PrDomDocument* document, PrStringView data);

// Creates a CDATA section with `data` (dom/#dom-document-createcdatasection). Returns NotSupported in an HTML document,
// and InvalidData if the data contains "]]>".
PrStatus prDomTextCreateCdataSection(PrDomDocument* document, PrStringView data, PrDomText* PR_NULLABLE* PR_NONNULL out);

// Creates a comment with `data`.
PrDomComment* prDomCommentCreate(PrDomDocument* document, PrStringView data);

// Creates a processing instruction (dom/#dom-document-createprocessinginstruction). Returns InvalidData if `target` is
// not an XML Name or `data` contains "?>".
PrStatus prDomProcessingInstructionCreate(PrDomDocument* document, PrStringView target, PrStringView data,
    PrDomProcessingInstruction* PR_NULLABLE* PR_NONNULL out);

// Returns the text of the document's title element, with ASCII whitespace stripped and collapsed, or the empty string
// if there is no title element (html/#document.title).
PrRawString prDomDocumentTitle(const PrDomDocument* document);

// Sets the text of the document's title element. Creates the element if the document element has a place for it, and
// otherwise does nothing (html/#document.title).
void prDomDocumentSetTitle(PrDomDocument* document, PrStringView value);

// Returns the body element, which is the first body or frameset child of an html document element, or null
// (html/#dom-document-body).
PrDomElement* PR_NULLABLE prDomDocumentBody(PrDomDocument* document);

// Replaces the body element with `value`, or appends `value` to the document element if there is no body
// (html/#dom-document-body). Returns NotAllowed if the value is not a body or frameset element, or if there is no
// document element to append it to.
PrStatus prDomDocumentSetBody(PrDomDocument* document, PrDomElement* PR_NULLABLE value);

// Returns the head element, which is the first head child of an html document element, or null
// (html/#dom-document-head).
PrDomElement* PR_NULLABLE prDomDocumentHead(PrDomDocument* document);

// Returns the document's doctype, which is its first child that is a doctype, or null (dom/#dom-document-doctype).
PrDomDocumentType* PR_NULLABLE prDomDocumentDoctype(PrDomDocument* document);

// Returns true if the document is in quirks mode (dom/#dom-document-compatmode). Script sees quirks mode as
// "BackCompat", and any other mode as "CSS1Compat".
bool prDomDocumentQuirks(const PrDomDocument* document);

// Returns the document's DOMImplementation, which is a view of the document.
PrDomImplementation* prDomDocumentImplementation(PrDomDocument* document);

// ---------------------------------------------------------------------------------------------------------------------
// DOMImplementation (dom)

// Creates a doctype in the document, which owns it (dom/#dom-domimplementation-createdocumenttype). Returns InvalidData
// if `name` is not a valid doctype name.
PrStatus prDomDocumentTypeCreate(PrDomImplementation* implementation, PrStringView name,
    PrStringView publicId, PrStringView systemId, PrDomDocumentType* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// DocumentType : Node (dom)

// Converts a DocumentType to each interface that it inherits from.
PR_HANDLE_UPCAST(DomDocumentType, DomNode, Node)
PR_HANDLE_UPCAST(DomDocumentType, DomEventTarget, EventTarget)

// Returns an EventTarget as a DocumentType, or null if it is not one.
PrDomDocumentType* PR_NULLABLE prDomDocumentTypeQuery(PrDomEventTarget* PR_NULLABLE object);

// Return the doctype's name, public ID and system ID. An ID that the doctype lacks is the empty string.
PrStringView prDomDocumentTypeName(const PrDomDocumentType* doctype);
PrStringView prDomDocumentTypePublicId(const PrDomDocumentType* doctype);
PrStringView prDomDocumentTypeSystemId(const PrDomDocumentType* doctype);

// ---------------------------------------------------------------------------------------------------------------------
// DocumentFragment : Node (dom)

// Converts a DocumentFragment to each interface that it inherits from.
PR_HANDLE_UPCAST(DomDocumentFragment, DomNode, Node)
PR_HANDLE_UPCAST(DomDocumentFragment, DomEventTarget, EventTarget)

// Returns an EventTarget as a DocumentFragment, or null if it is not one.
PrDomDocumentFragment* PR_NULLABLE prDomDocumentFragmentQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// ShadowRoot : DocumentFragment (dom)
//
// A shadow root is the root of a tree that its host element holds separately from its children. Its document owns it.

// The mode of a shadow root (dom/#shadowroot-mode). The host's shadowRoot returns an Open shadow root, and does not
// return a Closed one.
enum {
    PrDomShadowRootMode_Open,
    PrDomShadowRootMode_Closed,
};
typedef uint8_t PrDomShadowRootMode;

// How a shadow root assigns its host's children to its slots (dom/#shadowroot-slot-assignment): by the names of the
// slots, or as the assign() calls of the slots give them.
enum {
    PrDomSlotAssignmentMode_Named,
    PrDomSlotAssignmentMode_Manual,
};
typedef uint8_t PrDomSlotAssignmentMode;

// The members of ShadowRootInit (dom/#dictdef-shadowrootinit).
typedef struct PrDomShadowRootInit {
    PrDomShadowRootMode mode;
    bool delegatesFocus;
    PrDomSlotAssignmentMode slotAssignment;
    bool clonable;
    bool serializable;
} PrDomShadowRootInit;

// Returns the defaults of the members that a ShadowRootInit may omit, with the required `mode`.
static inline PrDomShadowRootInit prDomShadowRootInitDefault(PrDomShadowRootMode mode) {
    PrDomShadowRootInit value;
    value.mode = mode;
    value.delegatesFocus = false;
    value.slotAssignment = PrDomSlotAssignmentMode_Named;
    value.clonable = false;
    value.serializable = false;
    return value;
}

// Converts a ShadowRoot to each interface that it inherits from.
PR_HANDLE_UPCAST(DomShadowRoot, DomDocumentFragment, DocumentFragment)
PR_HANDLE_UPCAST(DomShadowRoot, DomNode, Node)
PR_HANDLE_UPCAST(DomShadowRoot, DomEventTarget, EventTarget)

// Returns an EventTarget as a ShadowRoot, or null if it is not one.
PrDomShadowRoot* PR_NULLABLE prDomShadowRootQuery(PrDomEventTarget* PR_NULLABLE object);

// Return the members of the ShadowRootInit that the shadow root was attached with.
PrDomShadowRootMode prDomShadowRootMode(const PrDomShadowRoot* shadow);
bool prDomShadowRootDelegatesFocus(const PrDomShadowRoot* shadow);
PrDomSlotAssignmentMode prDomShadowRootSlotAssignment(const PrDomShadowRoot* shadow);
bool prDomShadowRootClonable(const PrDomShadowRoot* shadow);
bool prDomShadowRootSerializable(const PrDomShadowRoot* shadow);

// Returns the element that hosts the shadow root.
PrDomElement* prDomShadowRootHost(PrDomShadowRoot* shadow);

// ---------------------------------------------------------------------------------------------------------------------
// Element : Node (dom)

// Converts an Element to each interface that it inherits from.
PR_HANDLE_UPCAST(DomElement, DomNode, Node)
PR_HANDLE_UPCAST(DomElement, DomEventTarget, EventTarget)

// The namespaces that the specs name (infra/#namespaces), and a value for any other namespace.
enum {
    PrDomNamespace_None,
    PrDomNamespace_Html,
    PrDomNamespace_MathMl,
    PrDomNamespace_Svg,
    PrDomNamespace_XLink,
    PrDomNamespace_Xml,
    PrDomNamespace_XmlNs,
    PrDomNamespace_Other,
};
typedef uint8_t PrDomNamespace;

// Returns the URL of a namespace that the specs name. The text lives as long as the process. Returns the null view for
// None and Other.
PrStringView prDomNamespaceUrl(PrDomNamespace space);

// Returns the name of `tag` as its defining spec writes it. The text lives as long as the process. Returns the null
// view for PrDomTag_Unknown.
PrStringView prDomTagName(PrDomTag tag);

// Returns the name of `tag` in ASCII uppercase, as tagName writes an HTML element's name in an HTML document
// (dom/#element-html-uppercased-qualified-name).
PrStringView prDomTagUpperName(PrDomTag tag);

// The state of an image request (html/#img-req-state).
enum {
    PrDomImageState_Unavailable,
    PrDomImageState_PartiallyAvailable,
    PrDomImageState_CompletelyAvailable,
    PrDomImageState_Broken,
};
typedef uint8_t PrDomImageState;

// Returns the keyword of an input element's type state (html/#attr-input-type). Returns the null view for a value past
// the last state.
PrStringView prDomInputTypeKeyword(PrDomInputType type);

// Returns an EventTarget as an Element, or null if it is not one.
PrDomElement* PR_NULLABLE prDomElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the element's local name.
PrStringView prDomElementLocalName(const PrDomElement* element);

// Returns the element's local name as a tag, or PrDomTag_Unknown for a name that PrDomTag does not list. Only
// prDomElementLocalName returns such a name.
PrDomTag prDomElementTag(const PrDomElement* element);

// Returns the element's namespace, or the null view for none.
PrStringView prDomElementNamespaceUri(const PrDomElement* element);

// Returns the element's namespace if the specs name it, or Other for a namespace that a script uses.
PrDomNamespace prDomElementNamespace(const PrDomElement* element);

// Returns the element's namespace prefix, or the null view for none.
PrStringView prDomElementPrefix(const PrDomElement* element);

// Returns the element's qualified name, in ASCII uppercase for an HTML element in an HTML document
// (dom/#dom-element-tagname).
PrRawString prDomElementTagName(const PrDomElement* element);

// Returns the tag that tagName names. Writes true to `uppercase` if tagName is in ASCII uppercase. Returns
// PrDomTag_Unknown if tagName names no tag, as for an element with a prefix. Only prDomElementTagName returns such a
// tagName.
PrDomTag prDomElementTagNameTag(const PrDomElement* element, bool* uppercase);

// Get and set the value of the element's id attribute. The getter returns the empty string if there is no id attribute.
// The setter sets the attribute.
PrStringView prDomElementId(const PrDomElement* element);
void prDomElementSetId(PrDomElement* element, PrStringView value);

// Get and set the value of the element's slot attribute, which is a slottable's name (dom/#slotable-name). The getter
// returns the empty string if there is no slot attribute. The setter sets the attribute (dom/#dom-element-slot).
PrStringView prDomElementSlot(const PrDomElement* element);
void prDomElementSetSlot(PrDomElement* element, PrStringView value);

// Get, set, remove and test for the first attribute whose qualified name is `qualifiedName`
// (dom/#dom-element-getattribute and the methods after it). A missing attribute's value is the null view. Setting
// returns InvalidData if the name is not a valid attribute local name, and adds an attribute in no namespace if the
// element has none of that name. An HTML element in an HTML document lowercases the name it sets.
PrStringView prDomElementGetAttribute(const PrDomElement* element, PrStringView qualifiedName);
PrStatus prDomElementSetAttribute(PrDomElement* element, PrStringView qualifiedName, PrStringView value);
void prDomElementRemoveAttribute(PrDomElement* element, PrStringView qualifiedName);
bool prDomElementHasAttribute(const PrDomElement* element, PrStringView qualifiedName);

// Returns true if the element has any attribute (dom/#dom-element-hasattributes).
bool prDomElementHasAttributes(const PrDomElement* element);

// Returns the value of the attribute in `namespaceUrl` whose local name is `localName`, or the null view if there is
// none. The empty string means no namespace.
PrStringView prDomElementGetAttributeNs(const PrDomElement* element, PrStringView namespaceUrl, PrStringView localName);

// Sets the attribute in `namespaceUrl` named `qualifiedName` to `value` (dom/#dom-element-setattributens). Fails in the
// same cases as prDomElementCreateNs, applied to an attribute's name.
PrStatus prDomElementSetAttributeNs(PrDomElement* element, PrStringView namespaceUrl,
    PrStringView qualifiedName, PrStringView value);

// Remove, and test for, the attribute in `namespaceUrl` whose local name is `localName`. The empty string means no
// namespace.
void prDomElementRemoveAttributeNs(PrDomElement* element, PrStringView namespaceUrl, PrStringView localName);
bool prDomElementHasAttributeNs(const PrDomElement* element, PrStringView namespaceUrl, PrStringView localName);

// Writes the qualified names of the element's attributes, in order (dom/#dom-element-getattributenames). Writes up to
// `capacity` names to `items`, which the caller releases, and returns the total count.
uint32_t prDomElementGetAttributeNames(const PrDomElement* element, PrRawString* PR_NULLABLE items, uint32_t capacity);

// Adds the attribute `qualifiedName` if the element lacks it, and removes it otherwise
// (dom/#dom-element-toggleattribute). A non-null `force` adds the attribute if true and removes it if false. Writes
// true if the element has the attribute afterward. Returns InvalidData if the name is not a valid attribute local name.
PrStatus prDomElementToggleAttribute(PrDomElement* element, PrStringView qualifiedName,
    const bool* PR_NULLABLE force, bool* PR_NULLABLE out);

// Return the element's attributes as its NamedNodeMap presents them (dom/#concept-element-attribute): the count, and
// the attribute at an index or null past the end.
uint32_t prDomElementAttributeCount(const PrDomElement* element);
PrDomAttr* PR_NULLABLE prDomElementAttributeAt(PrDomElement* element, uint32_t index);

// Writes the supported property names of the element's NamedNodeMap (dom/#interface-namednodemap), which are the
// qualified names of the element's attributes, each once. Names with an ASCII uppercase letter are excluded on an HTML
// element in an HTML document. Writes up to `capacity` names to `items`, which the caller releases, and returns the
// total count.
uint32_t prDomElementNamedAttributeNames(const PrDomElement* element, PrRawString* PR_NULLABLE items, uint32_t capacity);

// Returns the first attribute whose qualified name is `qualifiedName`, or null
// (dom/#concept-element-attributes-get-by-name).
PrDomAttr* PR_NULLABLE prDomElementGetAttributeNode(PrDomElement* element, PrStringView qualifiedName);

// Returns the attribute in `namespaceUrl` whose local name is `localName`, or null
// (dom/#concept-element-attributes-get-by-namespace). The empty string means no namespace.
PrDomAttr* PR_NULLABLE prDomElementGetAttributeNodeNs(PrDomElement* element, PrStringView namespaceUrl,
    PrStringView localName);

// Puts `attr` on the element, in place of the attribute with its namespace and local name
// (dom/#concept-element-attributes-set). Writes the replaced attribute, or null, with a reference that the caller
// releases. Returns InvalidState if `attr` belongs to another element.
PrStatus prDomElementSetAttributeNode(PrDomElement* element, PrDomAttr* attr, PrDomAttr* PR_NULLABLE* PR_NULLABLE out);

// Removes `attr` from the element (dom/#dom-element-removeattributenode). Returns NotFound if `attr` does not belong to
// the element.
PrStatus prDomElementRemoveAttributeNode(PrDomElement* element, PrDomAttr* attr);

// Removes the first attribute whose qualified name is `qualifiedName` (dom/#dom-namednodemap-removenameditem). Writes
// the removed attribute, with a reference that the caller releases. Returns NotFound if the element has no attribute of
// that name.
PrStatus prDomElementRemoveNamedAttribute(PrDomElement* element, PrStringView qualifiedName,
    PrDomAttr* PR_NULLABLE* PR_NONNULL out);

// Removes the attribute in `namespaceUrl` whose local name is `localName`, in the same way as
// prDomElementRemoveNamedAttribute (dom/#dom-namednodemap-removenameditemns).
PrStatus prDomElementRemoveNamedAttributeNs(PrDomElement* element, PrStringView namespaceUrl, PrStringView localName,
    PrDomAttr* PR_NULLABLE* PR_NONNULL out);

// The position of an inserted node relative to an element (dom/#insert-adjacent).
enum {
    PrDomInsertPosition_BeforeBegin,
    PrDomInsertPosition_AfterBegin,
    PrDomInsertPosition_BeforeEnd,
    PrDomInsertPosition_AfterEnd,
};
typedef uint8_t PrDomInsertPosition;

// Insert `element`, or a Text node with `data`, at `where` (dom/#dom-element-insertadjacentelement,
// dom/#dom-element-insertadjacenttext). Insert nothing if the position is outside an element with no parent.
// prDomElementInsertAdjacentElement writes the inserted element, or null if nothing was inserted. Both fail in the same
// cases as the other calls that change the tree.
PrStatus prDomElementInsertAdjacentElement(PrDomElement* target, PrDomInsertPosition where, PrDomElement* element,
    PrDomElement* PR_NULLABLE* PR_NULLABLE out);
PrStatus prDomElementInsertAdjacentText(PrDomElement* element, PrDomInsertPosition where, PrStringView data);

// Attaches a shadow root to the element (dom/#dom-element-attachshadow). Returns NotSupported if a shadow root cannot
// be attached to the element (dom/#valid-shadow-host-name), or if it already has one.
PrStatus prDomElementAttachShadow(PrDomElement* element, const PrDomShadowRootInit* init,
    PrDomShadowRoot* PR_NULLABLE* PR_NONNULL out);

// Returns the element's shadow root if it is open, and null otherwise (dom/#dom-element-shadowroot).
PrDomShadowRoot* PR_NULLABLE prDomElementShadowRoot(PrDomElement* element);

// Get and set the value of the element's class attribute. The getter returns the empty string if there is no class
// attribute. The setter sets the attribute.
PrStringView prDomElementClassName(const PrDomElement* element);
void prDomElementSetClassName(PrDomElement* element, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// Attr : Node (dom)
//
// An attribute belongs to its element while it is on one, and to its document once it is not.

// Converts an Attr to each interface that it inherits from.
PR_HANDLE_UPCAST(DomAttr, DomNode, Node)
PR_HANDLE_UPCAST(DomAttr, DomEventTarget, EventTarget)

// Returns an EventTarget as an Attr, or null if it is not one.
PrDomAttr* PR_NULLABLE prDomAttrQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the attribute's namespace, or the null view for none.
PrStringView prDomAttrNamespaceUri(const PrDomAttr* attr);

// Returns the attribute's namespace if the specs name it, or Other for a namespace that a script uses.
PrDomNamespace prDomAttrNamespace(const PrDomAttr* attr);

// Returns the attribute's namespace prefix, or the null view for none.
PrStringView prDomAttrPrefix(const PrDomAttr* attr);

// Returns the attribute's local name.
PrStringView prDomAttrLocalName(const PrDomAttr* attr);

// Returns the attribute's qualified name (dom/#concept-attribute-qualified-name).
PrRawString prDomAttrName(const PrDomAttr* attr);

// Returns the attribute's value.
PrStringView prDomAttrValue(const PrDomAttr* attr);

// Sets the attribute's value, as a change to its element's attribute if it is on one
// (dom/#set-an-existing-attribute-value).
void prDomAttrSetValue(PrDomAttr* attr, PrStringView value);

// Returns the element that the attribute is on, or null.
PrDomElement* PR_NULLABLE prDomAttrOwnerElement(PrDomAttr* attr);

// ---------------------------------------------------------------------------------------------------------------------
// Slottable (dom)
//
// The members that elements and text nodes have as slottables (dom/#concept-slotable).

// Returns the slot that the node is assigned to, or null unless that slot is in an open shadow root
// (dom/#dom-slotable-assignedslot).
PrDomElement* PR_NULLABLE prDomSlottableAssignedSlot(PrDomNode* slottable);

// ---------------------------------------------------------------------------------------------------------------------
// CharacterData : Node (dom)

// Converts a CharacterData to each interface that it inherits from.
PR_HANDLE_UPCAST(DomCharacterData, DomNode, Node)
PR_HANDLE_UPCAST(DomCharacterData, DomEventTarget, EventTarget)

// Returns an EventTarget as a CharacterData, or null if it is not one.
PrDomCharacterData* PR_NULLABLE prDomCharacterDataQuery(PrDomEventTarget* PR_NULLABLE object);

// Get and set the node's data, and return its length in UTF-16 code units.
PrStringView prDomCharacterDataData(const PrDomCharacterData* node);
void prDomCharacterDataSetData(PrDomCharacterData* node, PrStringView value);
uint32_t prDomCharacterDataLength(const PrDomCharacterData* node);

// Read or edit the data (dom/#dom-characterdata-substringdata and the methods after it). In order, the functions write
// the `count` code units at `offset` to `out`, append `data`, insert `data` at `offset`, delete the `count` code units
// at `offset`, or replace them with `data`. Offsets and counts are in UTF-16 code units. Each returns OutOfRange for an
// offset past the data.
PrStatus prDomCharacterDataSubstringData(const PrDomCharacterData* node, uint32_t offset, uint32_t count, PrRawString* out);
void prDomCharacterDataAppendData(PrDomCharacterData* node, PrStringView data);
PrStatus prDomCharacterDataInsertData(PrDomCharacterData* node, uint32_t offset, PrStringView data);
PrStatus prDomCharacterDataDeleteData(PrDomCharacterData* node, uint32_t offset, uint32_t count);
PrStatus prDomCharacterDataReplaceData(PrDomCharacterData* node, uint32_t offset, uint32_t count, PrStringView data);

// ---------------------------------------------------------------------------------------------------------------------
// Text : CharacterData (dom)

// Converts a Text to each interface that it inherits from.
PR_HANDLE_UPCAST(DomText, DomCharacterData, CharacterData)
PR_HANDLE_UPCAST(DomText, DomNode, Node)
PR_HANDLE_UPCAST(DomText, DomEventTarget, EventTarget)

// Returns an EventTarget as a Text, or null if it is not one.
PrDomText* PR_NULLABLE prDomTextQuery(PrDomEventTarget* PR_NULLABLE object);

// Splits the node at `offset` (dom/#concept-text-split). Writes the new node, with a reference that the caller
// releases. The new node follows the node if the node has a parent. Returns OutOfRange if `offset` is past the data's
// length.
PrStatus prDomTextSplitText(PrDomText* text, uint32_t offset, PrDomText* PR_NULLABLE* PR_NONNULL out);

// Returns the data of the node and of the contiguous Text siblings around it, in tree order (dom/#dom-text-wholetext).
PrRawString prDomTextWholeText(const PrDomText* text);

// ---------------------------------------------------------------------------------------------------------------------
// Comment : CharacterData (dom)

// Converts a Comment to each interface that it inherits from.
PR_HANDLE_UPCAST(DomComment, DomCharacterData, CharacterData)
PR_HANDLE_UPCAST(DomComment, DomNode, Node)
PR_HANDLE_UPCAST(DomComment, DomEventTarget, EventTarget)

// Returns an EventTarget as a Comment, or null if it is not one.
PrDomComment* PR_NULLABLE prDomCommentQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// ProcessingInstruction : CharacterData (dom)

// Converts a ProcessingInstruction to each interface that it inherits from.
PR_HANDLE_UPCAST(DomProcessingInstruction, DomCharacterData, CharacterData)
PR_HANDLE_UPCAST(DomProcessingInstruction, DomNode, Node)
PR_HANDLE_UPCAST(DomProcessingInstruction, DomEventTarget, EventTarget)

// Returns an EventTarget as a ProcessingInstruction, or null if it is not one.
PrDomProcessingInstruction* PR_NULLABLE prDomProcessingInstructionQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the instruction's target.
PrStringView prDomProcessingInstructionTarget(const PrDomProcessingInstruction* instruction);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLCollection (dom)
//
// A live collection of elements, in tree order. Each call reads it from the tree. A collection holds a reference to the
// node that it is made from.

// Create the collection of the node's element children (dom/#dom-parentnode-children); of the descendant elements of
// `root` that have every class in `classNames` (dom/#dom-document-getelementsbyclassname); and of those that have the
// qualified name `qualifiedName`, or of every descendant element for "*" (dom/#dom-document-getelementsbytagname).
// `root` is a document, fragment or element. The caller destroys the collection.
PrDomHtmlCollection* prDomHtmlCollectionCreateChildren(PrDomNode* parent);
PrDomHtmlCollection* prDomHtmlCollectionCreateByClassName(PrDomNode* root, PrStringView classNames);
PrDomHtmlCollection* prDomHtmlCollectionCreateByTagName(PrDomNode* root, PrStringView qualifiedName);

// Returns the node that the collection is rooted at (dom/#concept-collection).
PrDomNode* prDomHtmlCollectionRoot(PrDomHtmlCollection* collection);

// Return the number of elements, and the element at `index` or null past the end.
uint32_t prDomHtmlCollectionLength(const PrDomHtmlCollection* collection);
PrDomElement* PR_NULLABLE prDomHtmlCollectionElementAt(PrDomHtmlCollection* collection, uint32_t index);

// Returns the first element whose ID is `name` or, for an HTML element, whose name attribute is `name`. Returns null if
// there is none, and for the empty string (dom/#dom-htmlcollection-nameditem-key).
PrDomElement* PR_NULLABLE prDomHtmlCollectionNamedItem(PrDomHtmlCollection* collection, PrStringView name);

// Writes the collection's supported property names (dom/#ref-for-dfn-supported-property-names): for each element in
// order, its ID, then its name attribute if it is an HTML element, each name once and none empty. Writes up to
// `capacity` names to `items`, and returns the total count.
uint32_t prDomHtmlCollectionNames(const PrDomHtmlCollection* collection, PrStringView* PR_NULLABLE items, uint32_t capacity);

// Frees the collection.
void prDomHtmlCollectionDestroy(PrDomHtmlCollection* PR_NULLABLE collection);

// ---------------------------------------------------------------------------------------------------------------------
// NodeList (dom)
//
// A list of nodes, either the live children of a node or a snapshot. A live list holds a reference to its node, and a
// snapshot to each of its nodes. An operation that makes a list a snapshot, such as prCssNodeQuerySelectorAll, reuses
// the list's storage.

// Create an empty snapshot, and the live list of the node's children (dom/#dom-node-childnodes). The caller destroys
// the list.
PrDomNodeList* prDomNodeListCreate(void);
PrDomNodeList* prDomNodeListCreateChildNodes(PrDomNode* parent);

// Return the node at `index` or null past the end, and the number of nodes.
PrDomNode* PR_NULLABLE prDomNodeListNodeAt(PrDomNodeList* list, uint32_t index);
uint32_t prDomNodeListLength(const PrDomNodeList* list);

// Frees the list.
void prDomNodeListDestroy(PrDomNodeList* PR_NULLABLE list);

// ---------------------------------------------------------------------------------------------------------------------
// DOMTokenList (dom)
//
// The token set of one attribute of an element. Each call reads the set from the attribute, and each change writes it
// back. A call that changes the set returns Syntax for an empty token, and InvalidData for a token that contains ASCII
// whitespace. It then changes nothing. A token list holds a reference to its element.

// Creates the token list of the element's attribute in no namespace whose local name is `name`, as the element's
// classList (dom/#dom-element-classlist) is the list of its class attribute. The caller destroys the list.
PrDomTokenList* prDomTokenListCreate(PrDomElement* element, PrStringView name);

// Writes whether `token` is a supported token of the attribute, ASCII case-insensitively, to `out`
// (dom/#dom-domtokenlist-supports). Returns WrongType if the attribute defines no supported tokens.
PrStatus prDomTokenListSupports(const PrDomTokenList* list, PrStringView token, bool* out);

// Returns the number of tokens.
uint32_t prDomTokenListLength(const PrDomTokenList* list);

// Returns the token at `index`, or the null view past the last token.
PrStringView prDomTokenListItem(const PrDomTokenList* list, uint32_t index);

// Returns true if the set holds `token`.
bool prDomTokenListContains(const PrDomTokenList* list, PrStringView token);

// Add each of `tokens` that the set lacks to its end, or remove each of `tokens` from the set.
PrStatus prDomTokenListAdd(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count);
PrStatus prDomTokenListRemove(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count);

// Adds `token` if the set lacks it, and removes it otherwise. A non-null `force` adds the token if true and removes it
// if false. Writes true if the token is present afterward.
PrStatus prDomTokenListToggle(PrDomTokenList* list, PrStringView token, const bool* PR_NULLABLE force, bool* out);

// Replaces `token` with `newToken`. Writes true if `token` was present and replaced.
PrStatus prDomTokenListReplace(PrDomTokenList* list, PrStringView token, PrStringView newToken, bool* out);

// Get and set the attribute's value. The getter returns the empty string if there is no attribute. The setter sets the
// attribute.
PrStringView prDomTokenListValue(const PrDomTokenList* list);
void prDomTokenListSetValue(PrDomTokenList* list, PrStringView value);

// Frees the list.
void prDomTokenListDestroy(PrDomTokenList* PR_NULLABLE list);

// ---------------------------------------------------------------------------------------------------------------------
// MutationObserver (dom)
//
// An observer is reference counted. Its caller and each node that it observes hold a reference. The observer's records
// are delivered in a microtask, which the host of the changed node's document queues.

// The kind of change that a record describes (dom/#dom-mutationrecord-type).
enum {
    PrDomMutationType_Attributes,
    PrDomMutationType_CharacterData,
    PrDomMutationType_ChildList,
};
typedef uint8_t PrDomMutationType;

// The members of MutationObserverInit (dom/#dictdef-mutationobserverinit). The `has` flags say whether each member was
// given.
typedef struct PrDomMutationObserverInit {
    bool childList;
    bool attributes;
    bool hasAttributes;
    bool characterData;
    bool hasCharacterData;
    bool subtree;
    bool attributeOldValue;
    bool characterDataOldValue;
    bool hasAttributeFilter;
} PrDomMutationObserverInit;

// Returns an init with every member false and none given.
static inline PrDomMutationObserverInit prDomMutationObserverInitDefault(void) {
    PrDomMutationObserverInit value;
    value.childList = false;
    value.attributes = false;
    value.hasAttributes = false;
    value.characterData = false;
    value.hasCharacterData = false;
    value.subtree = false;
    value.attributeOldValue = false;
    value.characterDataOldValue = false;
    value.hasAttributeFilter = false;
    return value;
}

// Called when the observer has records to deliver, which the callback takes with prDomMutationObserverTakeRecord
// (dom/#notify-mutation-observers).
typedef void (*PrDomMutationCallbackFn)(void* PR_NULLABLE context, PrDomMutationObserver* observer);

// The callback of an observer. The observer keeps a copy, and calls retain and release on the copy's context if they
// are given. `pending`, if given, is called with true when a delivery is queued for the observer, and with false once
// the delivery has run. A queued delivery holds a reference to the observer. Any data that the caller keeps for the
// observer must last until the delivery has run.
typedef struct PrDomMutationCallback {
    PrDomMutationCallbackFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
    void (*PR_NULLABLE pending)(void* PR_NULLABLE context, PrDomMutationObserver* observer, bool pending);
} PrDomMutationCallback;

// Creates an observer that calls `callback`, with one reference that the caller holds.
PrDomMutationObserver* prDomMutationObserverCreate(const PrDomMutationCallback* callback);

// Adds or releases a reference. The last release frees the observer and its records.
void prDomMutationObserverRetain(PrDomMutationObserver* observer);
void prDomMutationObserverRelease(PrDomMutationObserver* PR_NULLABLE observer);

// Registers the observer on `target` with `options`, and with `filter` as the attribute filter if the options have one
// (dom/#dom-mutationobserver-observe). Returns InvalidData for options that observe nothing, or that request an old
// value or a filter for something they do not observe.
PrStatus prDomMutationObserverObserve(PrDomMutationObserver* observer, PrDomNode* target,
    const PrDomMutationObserverInit* options, const PrStringView* PR_NULLABLE filter, uint32_t filterCount);

// Removes the observer's registrations, and drops its records (dom/#dom-mutationobserver-disconnect).
void prDomMutationObserverDisconnect(PrDomMutationObserver* observer);

// Returns the number of records that the observer holds.
uint32_t prDomMutationObserverRecordCount(const PrDomMutationObserver* observer);

// Takes the oldest record into `record`, in place of what it held. Returns false, and leaves `record` unchanged, if the
// observer holds no records.
bool prDomMutationObserverTakeRecord(PrDomMutationObserver* observer, PrDomMutationRecord* record);

// ---------------------------------------------------------------------------------------------------------------------
// MutationRecord (dom)
//
// A record holds a reference to each node that it names.

// Creates a record to take an observer's records into (prDomMutationObserverTakeRecord). Until the first take fills it,
// the record is empty and is passed to no other function but prDomMutationRecordDestroy.
PrDomMutationRecord* prDomMutationRecordCreate(void);

// Frees the record.
void prDomMutationRecordDestroy(PrDomMutationRecord* PR_NULLABLE record);

// Return the kind of change, and the node that changed.
PrDomMutationType prDomMutationRecordType(const PrDomMutationRecord* record);
PrDomNode* prDomMutationRecordTarget(PrDomMutationRecord* record);

// Make `list` a snapshot of the nodes that the change added or removed, and return the siblings before and after those
// nodes. Each sibling is null if there is none.
void prDomMutationRecordAddedNodes(PrDomMutationRecord* record, PrDomNodeList* list);
void prDomMutationRecordRemovedNodes(PrDomMutationRecord* record, PrDomNodeList* list);
PrDomNode* PR_NULLABLE prDomMutationRecordPreviousSibling(PrDomMutationRecord* record);
PrDomNode* PR_NULLABLE prDomMutationRecordNextSibling(PrDomMutationRecord* record);

// Return the changed attribute's local name and namespace, and the value before the change. Each is the null view if
// the record has none.
PrStringView prDomMutationRecordAttributeName(const PrDomMutationRecord* record);
PrStringView prDomMutationRecordAttributeNamespace(const PrDomMutationRecord* record);
PrStringView prDomMutationRecordOldValue(const PrDomMutationRecord* record);

// ---------------------------------------------------------------------------------------------------------------------
// AbstractRange, StaticRange and Range (dom)
//
// A range is live or static (dom/#abstractrange). The mutation algorithms of its document keep a live range current. A
// range is reference counted, and a function that creates one gives the caller a reference. A live range holds a
// reference to its document, and a static range to its containers. The functions after prDomRangeCollapsed apply to a
// live range.

// Creates a live range collapsed at the start of the document (dom/#dom-range-range, dom/#dom-document-createrange).
PrDomRange* prDomRangeCreate(PrDomDocument* document);

// Creates a static range (dom/#dom-staticrange-staticrange). Returns WrongType if a container is a doctype or an
// attribute.
PrStatus prDomRangeCreateStatic(PrDomNode* startContainer, uint32_t startOffset, PrDomNode* endContainer,
    uint32_t endOffset, PrDomRange* PR_NULLABLE* PR_NONNULL out);

// Adds or releases a reference. The last release frees the range.
void prDomRangeRetain(PrDomRange* range);
void prDomRangeRelease(PrDomRange* PR_NULLABLE range);

// Returns true if the range is live rather than static.
bool prDomRangeIsLive(const PrDomRange* range);

// Return the node and offset of the range's start and of its end, and true if the two are the same point.
PrDomNode* prDomRangeStartContainer(PrDomRange* range);
uint32_t prDomRangeStartOffset(const PrDomRange* range);
PrDomNode* prDomRangeEndContainer(PrDomRange* range);
uint32_t prDomRangeEndOffset(const PrDomRange* range);
bool prDomRangeCollapsed(const PrDomRange* range);

// Returns the nearest node that contains both the start and the end (dom/#dom-range-commonancestorcontainer).
PrDomNode* prDomRangeCommonAncestorContainer(PrDomRange* range);

// Set the start or the end (dom/#dom-range-setstart, dom/#dom-range-setend). If the range would then end before it
// starts, or its points would be in different trees, the other point moves to the new one. Each returns WrongType for a
// doctype, and OutOfRange for an offset past the node's length.
PrStatus prDomRangeSetStart(PrDomRange* range, PrDomNode* node, uint32_t offset);
PrStatus prDomRangeSetEnd(PrDomRange* range, PrDomNode* node, uint32_t offset);

// The position of a boundary point relative to a node (dom/#dom-range-setstartbefore and the methods after it).
enum {
    PrDomRangeSide_StartBefore,
    PrDomRangeSide_StartAfter,
    PrDomRangeSide_EndBefore,
    PrDomRangeSide_EndAfter,
};
typedef uint8_t PrDomRangeSide;

// Sets the start or the end, as `side` says, to just before or after `node` in its parent. Returns WrongType for a node
// without a parent.
PrStatus prDomRangeSetBeside(PrDomRange* range, PrDomRangeSide side, PrDomNode* node);

// Collapses the range to its start if `toStart` is set, and to its end otherwise.
void prDomRangeCollapse(PrDomRange* range, bool toStart);

// Set the range to `node` itself (dom/#dom-range-selectnode), or to the contents of `node`
// (dom/#dom-range-selectnodecontents). Selecting a node returns WrongType for a node without a parent. Selecting the
// contents returns WrongType for a doctype.
PrStatus prDomRangeSelectNode(PrDomRange* range, PrDomNode* node);
PrStatus prDomRangeSelectNodeContents(PrDomRange* range, PrDomNode* node);

// The boundary points that compareBoundaryPoints compares (dom/#dom-range-start_to_start and the constants after it).
// The first word of a name is the source range's point, and the second is this range's.
enum {
    PrDomRangeHow_StartToStart = 0,
    PrDomRangeHow_StartToEnd   = 1,
    PrDomRangeHow_EndToEnd     = 2,
    PrDomRangeHow_EndToStart   = 3,
};
typedef uint8_t PrDomRangeHow;

// Writes -1, 0 or 1 if this range's point is before, equal to or after the source range's point
// (dom/#dom-range-compareboundarypoints). Returns Foreign for a source with another root.
PrStatus prDomRangeCompareBoundaryPoints(const PrDomRange* range, PrDomRangeHow how, const PrDomRange* source, int8_t* out);

// Removes the range's contents from the tree (dom/#dom-range-deletecontents).
void prDomRangeDeleteContents(PrDomRange* range);

// Create a fragment of the range's contents, and move the contents into it (dom/#dom-range-extractcontents) or copy
// them (dom/#dom-range-clonecontents). Write the fragment, with a reference that the caller releases. Each returns
// NotAllowed if a doctype is contained.
PrStatus prDomRangeExtractContents(PrDomRange* range, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomRangeCloneContents(PrDomRange* range, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out);

// Inserts `node` at the start of the range (dom/#dom-range-insertnode). Fails in the same cases as insertion, and also
// returns NotAllowed for a start in a comment, a processing instruction or a detached text node.
PrStatus prDomRangeInsertNode(PrDomRange* range, PrDomNode* node);

// Moves the range's contents into `node`, and inserts `node` in their place (dom/#dom-range-surroundcontents). Returns
// InvalidState if a non-text node is partially contained, and WrongType for a document, doctype or fragment.
PrStatus prDomRangeSurroundContents(PrDomRange* range, PrDomNode* node);

// Creates a live range with the same boundary points, with a reference that the caller holds.
PrDomRange* prDomRangeCloneRange(PrDomRange* range);

// prDomRangeIsPointInRange writes true if the point is in the range (dom/#dom-range-ispointinrange).
// prDomRangeComparePoint writes -1, 0 or 1 if the point is before, in or after the range (dom/#dom-range-comparepoint).
// Each returns WrongType for a doctype and OutOfRange for an offset past the node's length. prDomRangeComparePoint also
// returns Foreign for a node with another root.
PrStatus prDomRangeIsPointInRange(const PrDomRange* range, PrDomNode* node, uint32_t offset, bool* out);
PrStatus prDomRangeComparePoint(const PrDomRange* range, PrDomNode* node, uint32_t offset, int8_t* out);

// Returns true if the range intersects `node` (dom/#dom-range-intersectsnode).
bool prDomRangeIntersectsNode(const PrDomRange* range, PrDomNode* node);

// Returns the text within the range, from the Text nodes that it holds in whole or in part, in tree order
// (dom/#dom-range-stringifier).
PrRawString prDomRangeToString(const PrDomRange* range);

// ---------------------------------------------------------------------------------------------------------------------
// Selection (selection-api)
//
// A document has one selection (selection-api/#dfn-selection), which lives as long as the document. The selection has
// one range or none, and that range is a live range of the document. A change to the selection or to its range
// schedules a selectionchange event at the document.
//
// The selection holds a weak reference to its range, and the caller keeps the range alive while the selection has it.
// The range is one that the caller added, or the `range` argument of a call that gives the selection a new range. A
// call that gives it no new range leaves `range` unchanged.

// Returns the selection that selection-api/#dom-document-getselection returns, creating it on first request.
PrDomSelection* prDomDocumentSelection(PrDomDocument* document);

// Returns the document that the selection belongs to.
PrDomDocument* prDomSelectionDocument(PrDomSelection* selection);

// Returns the selection's range, if it has one, wherever its boundary points are.
PrDomRange* PR_NULLABLE prDomSelectionRange(PrDomSelection* selection);

// Return the node and offset of the selection's anchor and focus (selection-api/#dfn-anchor), and true if the two are
// the same point. The nodes are null and the offsets zero unless the range's boundary points are in the document tree.
// A selection without such a range is collapsed.
PrDomNode* PR_NULLABLE prDomSelectionAnchorNode(PrDomSelection* selection);
uint32_t prDomSelectionAnchorOffset(const PrDomSelection* selection);
PrDomNode* PR_NULLABLE prDomSelectionFocusNode(PrDomSelection* selection);
uint32_t prDomSelectionFocusOffset(const PrDomSelection* selection);
bool prDomSelectionIsCollapsed(const PrDomSelection* selection);

// Returns the number of ranges in the selection: one if its range is in the document tree, and zero otherwise.
uint32_t prDomSelectionRangeCount(const PrDomSelection* selection);

// The type of a selection (selection-api/#dom-selection-type).
enum {
    PrDomSelectionType_None,
    PrDomSelectionType_Caret,
    PrDomSelectionType_Range,
};
typedef uint8_t PrDomSelectionType;

// Returns the selection's type: None if its range count is zero, Caret if it is collapsed, and Range otherwise.
PrDomSelectionType prDomSelectionType(const PrDomSelection* selection);

// The direction of a selection (selection-api/#dom-selection-direction).
enum {
    PrDomSelectionDirection_None,
    PrDomSelectionDirection_Forwards,
    PrDomSelectionDirection_Backwards,
};
typedef uint8_t PrDomSelectionDirection;

// Returns the selection's direction, or None if it has no range.
PrDomSelectionDirection prDomSelectionDirection(const PrDomSelection* selection);

// Writes the selection's range at `index` (selection-api/#dom-selection-getrangeat). Returns OutOfRange for an index
// past the last.
PrStatus prDomSelectionGetRangeAt(PrDomSelection* selection, uint32_t index, PrDomRange* PR_NULLABLE* PR_NONNULL out);

// Makes `range` the selection's range if the selection has none and `range` is a live range whose root is the document
// (selection-api/#dom-selection-addrange). Does nothing otherwise.
void prDomSelectionAddRange(PrDomSelection* selection, PrDomRange* range);

// Empties the selection if `range` is its range (selection-api/#dom-selection-removerange). Returns NotFound for a
// range that is not the selection's.
PrStatus prDomSelectionRemoveRange(PrDomSelection* selection, const PrDomRange* range);

// Empties the selection (selection-api/#dom-selection-removeallranges).
void prDomSelectionRemoveAllRanges(PrDomSelection* selection);

// Collapses the selection to the point at `offset` in `node`, or empties it for a null node
// (selection-api/#dom-selection-collapse). Returns WrongType for a doctype, and OutOfRange for an offset past the
// node's length.
PrStatus prDomSelectionCollapse(PrDomSelection* selection, PrDomNode* PR_NULLABLE node, uint32_t offset, PrDomRange* range);

// Collapse the selection to the start or the end of its range (selection-api/#dom-selection-collapsetostart,
// selection-api/#dom-selection-collapsetoend). Each returns InvalidState for an empty selection.
PrStatus prDomSelectionCollapseToStart(PrDomSelection* selection, PrDomRange* range);
PrStatus prDomSelectionCollapseToEnd(PrDomSelection* selection, PrDomRange* range);

// Moves the focus to the point at `offset` in `node`, and keeps the anchor (selection-api/#dom-selection-extend).
// Returns InvalidState for an empty selection, and otherwise fails in the same cases as collapse.
PrStatus prDomSelectionExtend(PrDomSelection* selection, PrDomNode* node, uint32_t offset, PrDomRange* range);

// Sets the anchor and the focus (selection-api/#dom-selection-setbaseandextent). Fails in the same cases as collapse.
PrStatus prDomSelectionSetBaseAndExtent(PrDomSelection* selection, PrDomNode* anchorNode,
    uint32_t anchorOffset, PrDomNode* focusNode, uint32_t focusOffset, PrDomRange* range);

// Selects the children of `node` (selection-api/#dom-selection-selectallchildren). Returns WrongType for a doctype.
PrStatus prDomSelectionSelectAllChildren(PrDomSelection* selection, PrDomNode* node, PrDomRange* range);

// Removes the contents of the selection's range from the tree (selection-api/#dom-selection-deletefromdocument).
void prDomSelectionDeleteFromDocument(PrDomSelection* selection);

// Returns true if the selection's range contains `node` or, if `allowPartialContainment` is set, any part of it
// (selection-api/#dom-selection-containsnode).
bool prDomSelectionContainsNode(const PrDomSelection* selection, PrDomNode* node, bool allowPartialContainment);

// Returns the text within the selection's range, or the empty string if there is no range.
PrRawString prDomSelectionToString(const PrDomSelection* selection);

// Writes the range with each boundary point moved out of the shadow trees whose roots are not in `roots`, as a static
// range that the caller holds (selection-api/#dom-selection-getcomposedranges). Writes none for an empty selection.
// Returns the number written, which is zero or one.
uint32_t prDomSelectionComposedRanges(PrDomSelection* selection, PrDomShadowRoot* const PR_NONNULL* PR_NULLABLE roots,
    uint32_t count, PrDomRange* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Custom elements (html)
//
// A document's custom element definitions are in its registry (html/#custom-elements), which the scripting host of the
// document's window keeps. A definition belongs to the host, and the DOM does not look inside it. The DOM keeps each
// element's custom element state and its queue of reactions. It runs the reactions queued during a [CEReactions] member
// (html/#cereactions) when the member returns, and invokes each through the registry. A reaction queued outside any
// such member runs in a microtask.

// The custom element state of an element (dom/#concept-element-custom-element-state).
enum {
    PrDomCustomElementState_Uncustomized,
    PrDomCustomElementState_Undefined,
    PrDomCustomElementState_Failed,
    PrDomCustomElementState_Precustomized,
    PrDomCustomElementState_Custom,
};
typedef uint8_t PrDomCustomElementState;

// The lifecycle callbacks that a callback reaction invokes
// (html/#concept-custom-element-definition-lifecycle-callbacks).
enum {
    PrDomCustomCallback_Connected,
    PrDomCustomCallback_Disconnected,
    PrDomCustomCallback_Adopted,
    PrDomCustomCallback_AttributeChanged,
};
typedef uint8_t PrDomCustomCallback;

// The arguments of a callback reaction (html/#enqueue-a-custom-element-callback-reaction).
typedef struct PrDomCustomReaction {
    PrDomCustomCallback callback;

    // An attribute's local name, old value, value and namespace. Each is the null view for null.
    PrStringView name;
    PrStringView oldValue;
    PrStringView value;
    PrStringView namespaceUrl;

    // The documents between which an adopted element moved.
    PrDomDocument* PR_NULLABLE oldDocument;
    PrDomDocument* PR_NULLABLE newDocument;
} PrDomCustomReaction;

// The services that a custom element registry provides to the DOM. The definitions that it returns live as long as the
// registry.
typedef struct PrDomCustomElementRegistry {
    void* PR_NULLABLE context;

    // The agent of the registry's window. The reactions of the registry's elements are queued on its reactions stack.
    PrDomAgent* agent;

    // Looks up the definition for an HTML element's local name and is value. The null view means no is value
    // (html/#look-up-a-custom-element-definition). Returns the definition, or null if there is none.
    void* PR_NULLABLE (*PR_NONNULL lookup)(void* PR_NULLABLE context, PrStringView localName, PrStringView is);

    // Returns true if the definition has the callback. For attributeChanged, also requires that the definition observes
    // the attribute with local name `name` (html/#concept-custom-element-definition-observed-attributes).
    bool (*PR_NONNULL observes)(void* PR_NULLABLE context, void* definition, PrDomCustomCallback callback, PrStringView name);

    // Upgrades `element` by running the definition's constructor, which is the construction step of
    // html/#concept-upgrade-an-element. Returns false, with the exception reported, if the constructor throws or
    // returns an element other than `element`.
    bool (*PR_NONNULL upgrade)(void* PR_NULLABLE context, PrDomElement* element, void* definition);

    // Calls the definition's constructor synchronously, as a step of dom/#concept-create-element. Returns the element
    // that it produces, with a reference that the caller releases. Returns null, with the exception reported, if the
    // constructor throws or returns something that is not an element.
    PrDomElement* PR_NULLABLE (*PR_NONNULL construct)(void* PR_NULLABLE context, void* definition);

    // Reports an exception that the DOM throws at the registry's global (html/#report-the-exception): a DOMException
    // with the error name `name`, or a TypeError if `name` is "TypeError".
    void (*PR_NONNULL report)(void* PR_NULLABLE context, PrStringView name);

    // Invokes a callback reaction of the element, and reports any exception it throws.
    void (*PR_NONNULL invoke)(void* PR_NULLABLE context, PrDomElement* element, void* definition,
        const PrDomCustomReaction* reaction);

    // Runs `run` with `data` in a microtask (html/#queue-a-microtask). The DOM runs the reactions queued outside any
    // [CEReactions] member in such a microtask.
    void (*PR_NONNULL queueMicrotask)(void* PR_NULLABLE context, void (*PR_NONNULL run)(void* data), void* data);
} PrDomCustomElementRegistry;

// Returns true if `name` is a valid custom element name (html/#valid-custom-element-name).
bool prDomIsValidCustomElementName(PrStringView name);

// Sets the registry that defines the document's custom elements, or clears it if `registry` is null. The document keeps
// the pointer, not a copy.
void prDomDocumentSetCustomElementRegistry(PrDomDocument* document, const PrDomCustomElementRegistry* PR_NULLABLE registry);

// Returns the element's custom element state.
PrDomCustomElementState prDomElementCustomElementState(const PrDomElement* element);

// Returns the element's custom element definition, or null if it has none
// (dom/#concept-element-custom-element-definition).
void* PR_NULLABLE prDomElementCustomElementDefinition(const PrDomElement* element);

// Creates the element that an HTML element constructor creates when no element is being upgraded
// (html/#html-element-constructors). The element has the definition's local name, is custom, and is defined by the
// definition. The caller releases its reference.
PrDomElement* prDomElementCreateCustom(PrDomDocument* document, PrStringView localName, void* definition);

// Queues an upgrade reaction for each element of the document, in shadow-including tree order, that the definition of
// `localName` defines. These are the upgrades of html/#dom-customelementregistry-define.
void prDomDocumentUpgradeCandidates(PrDomDocument* document, PrStringView localName, void* definition);

// Tries to upgrade the node and its shadow-including descendants (html/#dom-customelementregistry-upgrade).
void prDomNodeUpgrade(PrDomNode* node);

// Returns the shadow root that the element hosts, open or closed, or null if there is none
// (html/#dom-elementinternals-shadowroot).
PrDomShadowRoot* PR_NULLABLE prDomElementHostedShadowRoot(PrDomElement* element);

// prDomElementSetCustomState adds `state` to the element's custom state set (html/#customstateset) if `present` is set,
// and removes it otherwise. prDomElementClearCustomStates clears the set. :state() matches the states in the set
// (selectors-5/#state-pseudo).
void prDomElementSetCustomState(PrDomElement* element, PrStringView state, bool present);
void prDomElementClearCustomStates(PrDomElement* element);

// Options for how an agent runs the rendering of its documents.
typedef struct PrDomAgentOptions {
    // The number of threads that a restyle divides its work among, including the agent's own thread. 0 means the width
    // of `restylePool`, up to a limit, and 1 means the agent's thread alone. Without a pool, a restyle runs on the
    // agent's thread alone.
    uint32_t restyleThreads;

    // The pool whose threads a restyle shares its work with, such as one over the host's job system, or null. The agent
    // does not own the pool, which must outlive it.
    PrTaskPool* PR_NULLABLE restylePool;
} PrDomAgentOptions;

// Returns the default options, which restyle on the agent's thread alone.
PrDomAgentOptions prDomAgentOptionsDefault(void);

// Creates an agent (html/#similar-origin-window-agent), which holds the state that the windows of one event loop share.
// That state is the custom element reactions stack, and the threads that its documents' presentations run on
// (PrCssPresentation). A null `options` means the defaults. The agent must outlive the documents that use it.
PrDomAgent* prDomAgentCreate(const PrDomAgentOptions* PR_NULLABLE options);

// Frees the agent, and drops the reactions queued for a microtask. No [CEReactions] member may be running.
void prDomAgentDestroy(PrDomAgent* PR_NULLABLE agent);

// Returns the number of milliseconds until a task of the agent's event loop (html/#event-loop) can run. Returns zero if
// a task is queued or a timer or an animation frame is due, and -1 if there is none. The loop runs the tasks of every
// window on the agent, including the windows of iframes. Deadlines use each window's clock. This call never waits or
// invokes callbacks.
double prDomAgentNextTaskDelay(PrDomAgent* agent);

// Returns true if nothing is pending on the agent's event loop except work that may never end: the rendering updates
// that animations want, the time updates of media that plays in a loop, and intervals, which run until a script clears
// them (html/#dom-setinterval). No task is posted (prDomAgentPost), and in any window, there is no task, other timer or
// animation frame request, no fetch that has not been answered, no scroll or animation event, no transition, no CSS
// animation that runs to an end, and no document to reveal. A host that runs pages until they settle stops at idle,
// because that work can run forever.
bool prDomAgentIsIdle(PrDomAgent* agent);

// Runs at most one task of the agent's event loop: a posted task, or else a task of a window whose task can run now.
// The windows take turns. In a window, the task queued first runs, or else the earliest due timer or animation frame.
// Returns true if a task ran, and does not wait. Returns false during a task, and for a window whose document is
// loading, except when called from its settle hook.
bool prDomAgentRunTask(PrDomAgent* agent);

// Runs at most one task as prDomAgentRunTask does, during a task too, which waits for it as a task does that spins the
// event loop (html/#spin-the-event-loop). Returns true if a task ran.
bool prDomAgentSpinTask(PrDomAgent* agent);

// Called on the agent's thread with the context that prDomAgentPost was given.
typedef void (*PrDomAgentTaskFn)(void* PR_NULLABLE context);

// Queues `fn`, called with `context`, as a task of the agent's event loop, after the tasks posted before it. Any thread
// may call it. prDomAgentNextTaskDelay is zero while a posted task waits, and prDomAgentRunTask runs the posted tasks
// before the windows' tasks. prDomAgentDestroy runs the posted tasks that have not run.
void prDomAgentPost(PrDomAgent* agent, PrDomAgentTaskFn fn, void* PR_NULLABLE context);

// Called on the thread that posts a task to the agent, once the task is queued. It must not call the agent. A host that
// sleeps until the agent's next task uses it to wake, so that the posted task runs.
typedef void (*PrDomAgentWakeFn)(void* PR_NULLABLE context);

// Sets the function that is called from now on as each task is posted. Null means no function, which is the initial
// state of an agent. Any thread may call it.
void prDomAgentSetWake(PrDomAgent* agent, PrDomAgentWakeFn PR_NULLABLE wake, void* PR_NULLABLE context);

// Called on the agent's layout thread as it finishes the jobs that the presentations of its documents started
// (prCssPresentationStart), at the point where waiting for them would no longer block. It must not call the agent or
// its presentations. A host that waits for its next frame uses it to wake and start the next jobs, so that the layout
// thread lays out what is ready without waiting for the frame.
typedef void (*PrDomAgentLayoutDoneFn)(void* PR_NULLABLE context);

// Sets the function that is called from now on as the agent's layout thread finishes its jobs. Null means no function,
// which is the initial state of an agent.
void prDomAgentSetLayoutDone(PrDomAgent* agent, PrDomAgentLayoutDoneFn PR_NULLABLE done, void* PR_NULLABLE context);

// Called on the agent's thread with the context that it was added with (prDomAgentAddCheckpoint, prDomAgentAddUpdate).
typedef void (*PrDomAgentHookFn)(void* PR_NULLABLE context);

// Adds or removes a microtask checkpoint (html/#perform-a-microtask-checkpoint), which prDomAgentRun performs after
// each task, in the order the checkpoints were added. A script engine adds one for each agent that its realms run on.
// Call on the agent's thread.
void prDomAgentAddCheckpoint(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context);
void prDomAgentRemoveCheckpoint(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context);

// Adds or removes an update, which prDomAgentRun calls once the tasks that it ran are done, in the order the updates
// were added. A view adds one to bring its document's rendering to its host's screen. Call on the agent's thread.
void prDomAgentAddUpdate(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context);
void prDomAgentRemoveUpdate(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context);

// Runs the tasks of the agent's event loop that can run, each followed by the checkpoints, until none can run or
// `milliseconds` have passed, then calls the updates. A long load runs in turns of a few milliseconds each, so that
// what it does reaches the screen as it goes, and a budget of zero runs one task. Returns true if a task ran. Call on
// the agent's thread, outside any task.
bool prDomAgentRun(PrDomAgent* agent, double milliseconds);

// Waits on the agent's thread until a task can run, a task is posted, the agent's layout thread finishes its jobs,
// prDomAgentWake is called, or `milliseconds` pass. A negative `milliseconds` waits without a limit. Returns false once
// prDomAgentStop has been called, at once.
bool prDomAgentWait(PrDomAgent* agent, double milliseconds);

// Ends the agent's wait, or the next one if none is in progress. Any thread may call it, such as a host's window when
// its frame starts.
void prDomAgentWake(PrDomAgent* agent);

// Ends the agent's waits from now on (prDomAgentWait). Any thread may call it.
void prDomAgentStop(PrDomAgent* agent);

// Push or pop the element queue of the host's own [CEReactions] member (html/#cereactions). The host pushes the queue
// when the member starts, and pops it, running its reactions, when the member returns.
void prDomAgentReactionsPush(PrDomAgent* agent);
void prDomAgentReactionsPop(PrDomAgent* agent);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
