// DOM API entry points.
//
// dom - https://dom.spec.whatwg.org/

#include "pr/dom.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/charset.h"
#include "pr/string_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url.h"
#include "pr/xml.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_custom.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_mutation.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_range.h"
#include "dom/src/dom_selection.h"
#include "dom/src/dom_slot.h"

namespace pr_impl {

namespace {

// Returns a node that a call created, with a reference that the caller takes.
template <typename T>
T* PR_NULLABLE made(T* PR_NULLABLE node) {
    if (node != nullptr)
        domNodeRetain(node);
    return node;
}

// dom/#concept-element-attributes-get-by-namespace, with the empty string meaning no namespace.
PrDomAttr* PR_NULLABLE attributeNs(const PrDomElement* element, PrStringView namespaceUrl,
                                   PrStringView localName) {
    for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
         attribute = static_cast<PrDomAttr*>(attribute->next)) {
        if (prStringView(&attribute->name) == localName
            && domNamespaceUrl(element->document, attribute->space) == namespaceUrl)
            return attribute;
    }
    return nullptr;
}

// dom/#concept-attribute-qualified-name
PrString attributeQualifiedName(const PrDomAttr* attribute) {
    PrStringView prefix = domAttributePrefix(attribute);
    if (prefix.data == nullptr)
        return prStringClone(&attribute->name);
    PrString name = prStringCopy(prefix);
    prStringAppend(&name, ":");
    prStringAppend(&name, prStringView(&attribute->name));
    return name;
}

// Returns `node` as an attribute, if it is one.
PrDomAttr* PR_NULLABLE attrOf(PrDomNode* PR_NULLABLE node) {
    return node != nullptr && node->type == PrDomNodeType_Attribute ? static_cast<PrDomAttr*>(node)
                                                                    : nullptr;
}

// dom/#validate-and-extract, of an element's name or, if `element` is false, an attribute's name. Returns InvalidData
// if the local name is not valid, and NotAllowed if the namespace and the prefix are not compatible. Returns
// NotSupported if the document can hold no other namespace.
PrStatus validateAndExtract(PrDomDocument* document, PrStringView namespaceUrl,
                            PrStringView qualifiedName, bool element, DomNamespaceId* space,
                            PrStringView* prefix, PrStringView* local) {
    size_t colon = stringFind(qualifiedName, ':');
    *prefix = colon < qualifiedName.size ? PrStringView{qualifiedName.data, colon} : PrStringView{};
    *local = colon < qualifiedName.size
                 ? stringSlice(qualifiedName, colon + 1, qualifiedName.size - colon - 1)
                 : qualifiedName;
    if (prefix->data != nullptr) {
        // dom/#valid-namespace-prefix
        bool valid = prefix->size != 0;
        for (size_t i = 0; valid && i < prefix->size; ++i)
            valid = !asciiIsSpace(prefix->data[i]) && prefix->data[i] != '\0'
                    && prefix->data[i] != '/' && prefix->data[i] != '>';
        if (!valid)
            return PrStatus_InvalidData;
    }
    if (element ? !domIsValidElementLocalName(*local) : !domIsValidAttributeLocalName(*local))
        return PrStatus_InvalidData;
    PrStringView xml = "http://www.w3.org/XML/1998/namespace";
    PrStringView xmlns = "http://www.w3.org/2000/xmlns/";
    bool none = namespaceUrl.size == 0;
    if ((prefix->data != nullptr && none)
        || (*prefix == "xml" && namespaceUrl != xml)
        || ((qualifiedName == "xmlns" || *prefix == "xmlns")
            && namespaceUrl != xmlns)
        || (namespaceUrl == xmlns && qualifiedName != "xmlns"
            && *prefix != "xmlns"))
        return PrStatus_NotAllowed;
    return domNamespaceFromUrl(document, namespaceUrl, space) ? PrStatus_Ok : PrStatus_NotSupported;
}

// Returns `name`, in ASCII lowercase if `lower` is set. The lowered name is written to `buffer` if lowering changes it.
PrStringView lowered(PrStringView name, bool lower, TextBuffer* buffer) {
    bool upper = false;
    for (size_t i = 0; lower && !upper && i < name.size; ++i)
        upper = asciiIsUpper(name.data[i]);
    if (!upper)
        return name;
    textAppend(buffer, name);
    textAsciiLower(buffer);
    return textView(buffer);
}

// Returns the attribute that a Remove method took off the element, with a reference that the caller takes. Returns
// NotFound if there is none.
PrStatus removeNamed(PrDomElement* element, PrDomAttr* PR_NULLABLE attribute,
                     PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    *out = attribute;
    if (attribute == nullptr)
        return PrStatus_NotFound;
    domNodeRetain(attribute);
    domRemoveAttributeNode(element, attribute);
    return PrStatus_Ok;
}

// Returns the node behind `target` as a `Handle`, if it is of `type`.
template <typename Handle>
Handle* PR_NULLABLE nodeOfType(PrDomEventTarget* PR_NULLABLE target, PrDomNodeType type) {
    PrDomNode* node = domNodeFromTarget(target);
    return node != nullptr && node->type == type ? static_cast<Handle*>(node) : nullptr;
}

// infra/#strip-and-collapse-ascii-whitespace
PrString stripAndCollapse(PrStringView text) {
    TextBuffer out;
    bool space = false;
    for (size_t i = 0; i < text.size; ++i) {
        if (asciiIsSpace(text.data[i])) {
            space = true;
            continue;
        }
        if (space && out.text.count != 0)
            textAppendChar(&out, ' ');
        space = false;
        textAppendChar(&out, text.data[i]);
    }
    return textTake(&out);
}

// Returns the first element of `space` whose tag is `tag`, among the descendants of `root` or among its children alone.
const PrDomElement* PR_NULLABLE firstElement(const PrDomNode* root, DomNamespaceId space, PrDomTag tag,
                                             bool children) {
    for (const PrDomNode* node = root->first; node != nullptr;
         node = children ? node->next : domNodeNext(root, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && element->space == space && element->tag == tag)
            return element;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE firstElement(PrDomNode* root, DomNamespaceId space, PrDomTag tag,
                                       bool children) {
    return const_cast<PrDomElement*>(
        firstElement(static_cast<const PrDomNode*>(root), space, tag, children));
}

bool isSvgRoot(const PrDomElement* PR_NULLABLE element) {
    return element != nullptr && element->space == PrDomNamespace_Svg && element->tag == PrDomTag_Svg;
}

// dom/#insert-adjacent. A null `node` inserts nothing, and a position outside an element with no parent inserts
// nothing.
PrStatus insertAdjacent(PrDomElement* element, PrDomInsertPosition where, PrDomNode* node,
                        bool* inserted) {
    *inserted = false;
    PrDomNode* parent = domNodeParent(element);
    switch (where) {
        case PrDomInsertPosition_BeforeBegin:
            if (parent == nullptr)
                return PrStatus_Ok;
            *inserted = true;
            return domPreInsert(parent, node, element);
        case PrDomInsertPosition_AfterBegin:
            *inserted = true;
            return domPreInsert(element, node, element->first);
        case PrDomInsertPosition_BeforeEnd:
            *inserted = true;
            return domPreInsert(element, node, nullptr);
        default:
            if (parent == nullptr)
                return PrStatus_Ok;
            *inserted = true;
            return domPreInsert(parent, node, element->next);
    }
}

// Returns the status of a change that a Range member made. On success, notifies the selection whose range it is.
PrStatus changed(PrDomRange* range, PrStatus status) {
    if (status == PrStatus_Ok)
        domSelectionRangeChanged(range);
    return status;
}

// Returns true if the selection's document is a shadow-including inclusive ancestor of the node, for methods that
// require it.
bool inSelectionDocument(const PrDomSelection* selection, const PrDomNode* node) {
    return domShadowIncludingRoot(node) == selection->document;
}

// Extracts the range's contents, or clones them if `extract` is false, into a fragment that the caller takes on
// success.
PrStatus rangeContents(PrDomRange* range, bool extract,
                       PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    PrDomDocumentFragment* fragment = nullptr;
    PrStatus status = domRangeExtract(range, extract, &fragment);
    if (extract)
        changed(range, status);
    if (status != PrStatus_Ok) {
        domNodeRelease(fragment);
        fragment = nullptr;
    }
    *out = fragment;
    return status;
}

// Makes `range`, set from `start` to `end`, the selection's range.
void selectRange(PrDomSelection* selection, PrDomRange* given, DomBoundary start, DomBoundary end,
                 PrDomSelectionDirection direction) {
    domRangeSetStart(given, start.node, start.offset);
    domRangeSetEnd(given, end.node, end.offset);
    domSelectionSetRange(selection, given, direction);
}

// Collapses the selection's range to its start or its end. `range` is set to that point and becomes the selection's
// range.
PrStatus collapseTo(PrDomSelection* self, PrDomRange* range, bool start) {
    const PrDomRange* current = domSelectionRange(self);
    if (current == nullptr)
        return PrStatus_InvalidState;
    DomBoundary point = start ? current->start : current->end;
    selectRange(self, range, point, point, PrDomSelectionDirection_None);
    return PrStatus_Ok;
}

// Gives a UIEvent the members of `init` beyond those of Event.
void setUiMembers(PrDomEvent* event, const PrDomUiEventInit& init) {
    event->view = init.view;
    event->detail = init.detail;
    event->which = init.which;
}

// Gives a MouseEvent the members of `init` beyond those of Event.
void setMouseMembers(PrDomEvent* event, const PrDomMouseEventInit& init) {
    setUiMembers(event, init.ui);
    if (event->mouse == nullptr)
        event->mouse = allocZeroed<DomMouseEventData>();
    *event->mouse = DomMouseEventData{init.modifiers, init.button,  init.buttons, init.screenX,
                                      init.screenY,   init.clientX, init.clientY};
    domEventTargetAssign(&event->related, init.relatedTarget);
    domEventTargetAssign(&event->relatedTarget, event->related);
}

// Returns the data of a KeyboardEvent, which an event that createEvent made gains as it is first initialized.
DomKeyboardEventData* keyboardOf(PrDomEvent* event) {
    if (event->keyboard == nullptr) {
        event->keyboard = allocZeroed<DomKeyboardEventData>();
        event->keyboard->key = prStringEmpty();
        event->keyboard->code = prStringEmpty();
    }
    return event->keyboard;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Event

PrDomEvent* prDomEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init) {
    return domEventCreate(type, init != nullptr ? *init : prDomEventInitDefault());
}

PrStatus prDomEventCreateUninitialized(PrDomEventKind kind, PrDomEvent* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // An event of any other kind holds data that only its constructor provides.
    if (kind != PrDomEventKind_Event && kind != PrDomEventKind_Custom && kind != PrDomEventKind_Ui
        && kind != PrDomEventKind_Focus && kind != PrDomEventKind_Mouse && kind != PrDomEventKind_Keyboard
        && kind != PrDomEventKind_Composition)
        return PrStatus_NotSupported;
    switch (kind) {
        case PrDomEventKind_Custom: *out = domEventCreateUninitialized<PrDomCustomEvent>(kind); break;
        case PrDomEventKind_Ui: *out = domEventCreateUninitialized<PrDomUiEvent>(kind); break;
        case PrDomEventKind_Focus: *out = domEventCreateUninitialized<PrDomFocusEvent>(kind); break;
        case PrDomEventKind_Mouse: *out = domEventCreateUninitialized<PrDomMouseEvent>(kind); break;
        case PrDomEventKind_Keyboard: *out = domEventCreateUninitialized<PrDomKeyboardEvent>(kind); break;
        case PrDomEventKind_Composition: *out = domEventCreateUninitialized<PrDomCompositionEvent>(kind); break;
        default: *out = domEventCreateUninitialized(kind); break;
    }
    return PrStatus_Ok;
}

bool prDomEventKindFromInterface(PrStringView interface, PrDomEventKind* out) {
    // The names of the interfaces that the engine has, among those listed in the method's table.
    struct EventInterface {
        PrStringView name;
        PrDomEventKind kind;
    };
    constexpr EventInterface kInterfaces[] = {
        {"compositionevent", PrDomEventKind_Composition},
        {"customevent", PrDomEventKind_Custom},
        {"event", PrDomEventKind_Event},
        {"events", PrDomEventKind_Event},
        {"focusevent", PrDomEventKind_Focus},
        {"htmlevents", PrDomEventKind_Event},
        {"keyboardevent", PrDomEventKind_Keyboard},
        {"mouseevent", PrDomEventKind_Mouse},
        {"mouseevents", PrDomEventKind_Mouse},
        {"svgevents", PrDomEventKind_Event},
        {"uievent", PrDomEventKind_Ui},
        {"uievents", PrDomEventKind_Ui},
    };
    for (const EventInterface& entry : kInterfaces) {
        if (asciiEqualsFold(interface, entry.name)) {
            *out = entry.kind;
            return true;
        }
    }
    return false;
}

void prDomEventRetain(PrDomEvent* self) {
    domEventRetain(self);
}

void prDomEventRelease(PrDomEvent* PR_NULLABLE self) {
    domEventRelease(self);
}

PrStringView prDomEventType(const PrDomEvent* self) {
    return prStringView(&self->type);
}

PrDomEventTarget* PR_NULLABLE prDomEventTarget(PrDomEvent* self) {
    return self->target;
}

PrDomEventTarget* PR_NULLABLE prDomEventCurrentTarget(PrDomEvent* self) {
    return self->currentTarget;
}

uint32_t prDomEventComposedPath(PrDomEvent* self, PrDomEventTarget* PR_NONNULL* PR_NULLABLE items,
                                uint32_t capacity) {
    for (uint32_t i = 0; items != nullptr && i < self->path.count && i < capacity; ++i)
        items[i] = self->path.data[i];
    return self->path.count;
}

PrDomEventPhase prDomEventPhase(const PrDomEvent* self) {
    return self->phase;
}

void prDomEventStopPropagation(PrDomEvent* self) {
    self->stopPropagation = true;
}

bool prDomEventCancelBubble(const PrDomEvent* self) {
    return self->stopPropagation;
}

void prDomEventSetCancelBubble(PrDomEvent* self, bool value) {
    if (value)
        self->stopPropagation = true;
}

void prDomEventStopImmediatePropagation(PrDomEvent* self) {
    self->stopPropagation = true;
    self->stopImmediatePropagation = true;
}

bool prDomEventBubbles(const PrDomEvent* self) {
    return self->bubbles;
}

bool prDomEventCancelable(const PrDomEvent* self) {
    return self->cancelable;
}

bool prDomEventReturnValue(const PrDomEvent* self) {
    return !self->canceled;
}

void prDomEventSetReturnValue(PrDomEvent* self, bool value) {
    if (!value)
        domEventCancel(self);
}

void prDomEventPreventDefault(PrDomEvent* self) {
    domEventCancel(self);
}

bool prDomEventDefaultPrevented(const PrDomEvent* self) {
    return self->canceled;
}

bool prDomEventComposed(const PrDomEvent* self) {
    return self->composed;
}

bool prDomEventIsTrusted(const PrDomEvent* self) {
    return self->trusted;
}

double prDomEventTimeStamp(const PrDomEvent* self) {
    return self->timeStamp;
}

void prDomEventInitEvent(PrDomEvent* self, PrStringView type, bool bubbles, bool cancelable) {
    domEventInitialize(self, type, bubbles, cancelable);
}

PrDomEventKind prDomEventKind(const PrDomEvent* self) {
    return self->kind;
}

// ---------------------------------------------------------------------------------------------------------------------
// CustomEvent

PrDomCustomEvent* PR_NULLABLE prDomCustomEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Custom
               ? static_cast<PrDomCustomEvent*>(object)
               : nullptr;
}

PrDomCustomEvent* prDomCustomEventCreate(PrStringView type,
                                         const PrDomEventInit* PR_NULLABLE init) {
    PrDomEventInit values = init != nullptr ? *init : prDomEventInitDefault();
    return domEventCreate<PrDomCustomEvent>(type, values, PrDomEventKind_Custom);
}

bool prDomCustomEventInitCustomEvent(PrDomCustomEvent* self, PrStringView type, bool bubbles,
                                     bool cancelable) {
    if (self->dispatching)
        return false;
    domEventInitialize(self, type, bubbles, cancelable);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// UIEvent and FocusEvent

PrDomUiEvent* PR_NULLABLE prDomUiEventQuery(PrDomEvent* PR_NULLABLE object) {
    bool ui = object != nullptr
              && (object->kind == PrDomEventKind_Ui || object->kind == PrDomEventKind_Focus
                  || object->kind == PrDomEventKind_Mouse || object->kind == PrDomEventKind_Wheel
                  || object->kind == PrDomEventKind_Keyboard || object->kind == PrDomEventKind_Composition);
    return ui ? static_cast<PrDomUiEvent*>(object) : nullptr;
}

PrDomUiEvent* prDomUiEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init) {
    PrDomUiEventInit values = init != nullptr ? *init : prDomUiEventInitDefault();
    PrDomUiEvent* event = domEventCreate<PrDomUiEvent>(type, values.event, PrDomEventKind_Ui);
    setUiMembers(event, values);
    return event;
}

PrDomEventTarget* PR_NULLABLE prDomUiEventView(PrDomUiEvent* self) {
    return self->view;
}

int32_t prDomUiEventDetail(const PrDomUiEvent* self) {
    return self->detail;
}

uint32_t prDomUiEventWhich(const PrDomUiEvent* self) {
    switch (self->kind) {
        case PrDomEventKind_Mouse:
        case PrDomEventKind_Wheel:
            return self->mouse != nullptr ? uint32_t(self->mouse->button + 1) : 1;
        case PrDomEventKind_Keyboard:
            if (self->keyboard == nullptr)
                return 0;
            return self->keyboard->keyCode != 0 ? self->keyboard->keyCode : self->keyboard->charCode;
        default:
            return self->which;
    }
}

void prDomUiEventInitUiEvent(PrDomUiEvent* self, PrStringView type, const PrDomUiEventInit* init) {
    if (self->dispatching)
        return;
    domEventInitialize(self, type, init->event.bubbles, init->event.cancelable);
    self->view = init->view;
    self->detail = init->detail;
}

PrDomFocusEvent* PR_NULLABLE prDomFocusEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Focus ? static_cast<PrDomFocusEvent*>(object) : nullptr;
}

PrDomFocusEvent* prDomFocusEventCreate(PrStringView type,
                                       const PrDomFocusEventInit* PR_NULLABLE init) {
    PrDomFocusEventInit values = init != nullptr ? *init : prDomFocusEventInitDefault();
    PrDomFocusEvent* event = domEventCreate<PrDomFocusEvent>(type, values.ui.event, PrDomEventKind_Focus);
    setUiMembers(event, values.ui);
    domEventTargetAssign(&event->related, values.relatedTarget);
    domEventTargetAssign(&event->relatedTarget, event->related);
    return event;
}

PrDomEventTarget* PR_NULLABLE prDomFocusEventRelatedTarget(PrDomFocusEvent* self) {
    return self->relatedTarget;
}

// ---------------------------------------------------------------------------------------------------------------------
// MouseEvent

PrDomModifiers prDomModifiersFromKey(PrStringView key) {
    struct ModifierName {
        PrStringView name;
        PrDomModifiers modifier;
    };
    constexpr ModifierName kModifiers[] = {
        {"Control", PrDomModifier_Control},   {"Shift", PrDomModifier_Shift},
        {"Alt", PrDomModifier_Alt},           {"Meta", PrDomModifier_Meta},
        {"AltGraph", PrDomModifier_AltGraph}, {"CapsLock", PrDomModifier_CapsLock},
        {"Fn", PrDomModifier_Fn},             {"FnLock", PrDomModifier_FnLock},
        {"Hyper", PrDomModifier_Hyper},       {"NumLock", PrDomModifier_NumLock},
        {"ScrollLock", PrDomModifier_ScrollLock}, {"Super", PrDomModifier_Super},
        {"Symbol", PrDomModifier_Symbol},     {"SymbolLock", PrDomModifier_SymbolLock},
    };
    for (const ModifierName& entry : kModifiers) {
        if (key == entry.name)
            return entry.modifier;
    }
    return 0;
}

PrDomMouseEvent* PR_NULLABLE prDomMouseEventQuery(PrDomEvent* PR_NULLABLE object) {
    bool mouse = object != nullptr && (object->kind == PrDomEventKind_Mouse || object->kind == PrDomEventKind_Wheel);
    return mouse ? static_cast<PrDomMouseEvent*>(object) : nullptr;
}

PrDomMouseEvent* prDomMouseEventCreate(PrStringView type, const PrDomMouseEventInit* PR_NULLABLE init) {
    PrDomMouseEventInit values = init != nullptr ? *init : prDomMouseEventInitDefault();
    PrDomMouseEvent* event = domEventCreate<PrDomMouseEvent>(type, values.ui.event, PrDomEventKind_Mouse);
    setMouseMembers(event, values);
    return event;
}

PrDomMouseEventInit prDomMouseEventMembers(PrDomMouseEvent* self) {
    PrDomMouseEventInit members = prDomMouseEventInitDefault();
    members.ui.event.bubbles = self->bubbles;
    members.ui.event.cancelable = self->cancelable;
    members.ui.event.composed = self->composed;
    members.ui.view = self->view;
    members.ui.detail = self->detail;
    members.ui.which = self->which;
    members.relatedTarget = self->relatedTarget;
    if (const DomMouseEventData* mouse = self->mouse) {
        members.modifiers = mouse->modifiers;
        members.screenX = mouse->screenX;
        members.screenY = mouse->screenY;
        members.clientX = mouse->clientX;
        members.clientY = mouse->clientY;
        members.button = mouse->button;
        members.buttons = mouse->buttons;
    }
    return members;
}

void prDomMouseEventInitMouseEvent(PrDomMouseEvent* self, PrStringView type, const PrDomMouseEventInit* init) {
    if (self->dispatching)
        return;
    domEventInitialize(self, type, init->ui.event.bubbles, init->ui.event.cancelable);
    PrDomMouseEventInit members = *init;
    members.buttons = self->mouse != nullptr ? self->mouse->buttons : 0;
    setMouseMembers(self, members);
}

// ---------------------------------------------------------------------------------------------------------------------
// WheelEvent

PrDomWheelEvent* PR_NULLABLE prDomWheelEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Wheel ? static_cast<PrDomWheelEvent*>(object) : nullptr;
}

PrDomWheelEvent* prDomWheelEventCreate(PrStringView type, const PrDomWheelEventInit* PR_NULLABLE init) {
    PrDomWheelEventInit values = init != nullptr ? *init : prDomWheelEventInitDefault();
    PrDomWheelEvent* event = domEventCreate<PrDomWheelEvent>(type, values.mouse.ui.event, PrDomEventKind_Wheel);
    setMouseMembers(event, values.mouse);
    event->wheel = alloc<DomWheelEventData>();
    *event->wheel = DomWheelEventData{values.deltaX, values.deltaY, values.deltaZ, values.deltaMode};
    return event;
}

PrDomWheelEventInit prDomWheelEventMembers(PrDomWheelEvent* self) {
    PrDomWheelEventInit members = prDomWheelEventInitDefault();
    members.mouse = prDomMouseEventMembers(self);
    members.deltaX = self->wheel->deltaX;
    members.deltaY = self->wheel->deltaY;
    members.deltaZ = self->wheel->deltaZ;
    members.deltaMode = self->wheel->deltaMode;
    return members;
}

// ---------------------------------------------------------------------------------------------------------------------
// KeyboardEvent

PrDomKeyboardEvent* PR_NULLABLE prDomKeyboardEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Keyboard ? static_cast<PrDomKeyboardEvent*>(object)
                                                                        : nullptr;
}

PrDomKeyboardEvent* prDomKeyboardEventCreate(PrStringView type, const PrDomKeyboardEventInit* PR_NULLABLE init) {
    PrDomKeyboardEventInit values = init != nullptr ? *init : prDomKeyboardEventInitDefault();
    PrDomKeyboardEvent* event = domEventCreate<PrDomKeyboardEvent>(type, values.ui.event, PrDomEventKind_Keyboard);
    setUiMembers(event, values.ui);
    DomKeyboardEventData* keyboard = keyboardOf(event);
    prStringReplace(&keyboard->key, values.key);
    prStringReplace(&keyboard->code, values.code);
    keyboard->location = values.location;
    keyboard->charCode = values.charCode;
    keyboard->keyCode = values.keyCode;
    keyboard->modifiers = values.modifiers;
    keyboard->repeat = values.repeat;
    keyboard->isComposing = values.isComposing;
    return event;
}

PrDomKeyboardEventInit prDomKeyboardEventMembers(PrDomKeyboardEvent* self) {
    PrDomKeyboardEventInit members = prDomKeyboardEventInitDefault();
    members.ui.event.bubbles = self->bubbles;
    members.ui.event.cancelable = self->cancelable;
    members.ui.event.composed = self->composed;
    members.ui.view = self->view;
    members.ui.detail = self->detail;
    members.ui.which = self->which;
    if (const DomKeyboardEventData* keyboard = self->keyboard) {
        members.modifiers = keyboard->modifiers;
        members.key = prStringView(&keyboard->key);
        members.code = prStringView(&keyboard->code);
        members.location = keyboard->location;
        members.repeat = keyboard->repeat;
        members.isComposing = keyboard->isComposing;
        members.charCode = keyboard->charCode;
        members.keyCode = keyboard->keyCode;
    }
    return members;
}

void prDomKeyboardEventInitKeyboardEvent(PrDomKeyboardEvent* self, PrStringView type,
                                         const PrDomKeyboardEventInit* init) {
    if (self->dispatching)
        return;
    domEventInitialize(self, type, init->ui.event.bubbles, init->ui.event.cancelable);
    self->view = init->ui.view;
    self->detail = 0;
    DomKeyboardEventData* keyboard = keyboardOf(self);
    prStringReplace(&keyboard->key, init->key);
    keyboard->location = init->location;
    keyboard->modifiers = init->modifiers;
}

// ---------------------------------------------------------------------------------------------------------------------
// CompositionEvent

PrDomCompositionEvent* PR_NULLABLE prDomCompositionEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Composition
               ? static_cast<PrDomCompositionEvent*>(object)
               : nullptr;
}

PrDomCompositionEvent* prDomCompositionEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init,
                                                   PrStringView data) {
    PrDomUiEventInit values = init != nullptr ? *init : prDomUiEventInitDefault();
    PrDomCompositionEvent* event =
        domEventCreate<PrDomCompositionEvent>(type, values.event, PrDomEventKind_Composition);
    setUiMembers(event, values);
    event->composition = prStringCopy(data);
    return event;
}

PrStringView prDomCompositionEventData(const PrDomCompositionEvent* self) {
    return prStringView(&self->composition);
}

void prDomCompositionEventInitCompositionEvent(PrDomCompositionEvent* self, PrStringView type,
                                               const PrDomUiEventInit* init, PrStringView data) {
    if (self->dispatching)
        return;
    domEventInitialize(self, type, init->event.bubbles, init->event.cancelable);
    self->view = init->view;
    self->detail = 0;
    prStringReplace(&self->composition, data);
}

// ---------------------------------------------------------------------------------------------------------------------
// EventTarget

PrDomAddEventListenerOptions prDomAddEventListenerOptionsDefault(const PrDomEventTarget* target,
                                                                 PrStringView type) {
    return PrDomAddEventListenerOptions{.passive = domDefaultPassive(target, type)};
}

PrDomEventTarget* prDomEventTargetCreate(void) {
    DomStandaloneTarget* target = allocZeroed<DomStandaloneTarget>();
    target->references = 1;
    return target;
}

void prDomEventTargetRetain(PrDomEventTarget* self) {
    domEventTargetRetain(self);
}

void prDomEventTargetRelease(PrDomEventTarget* PR_NULLABLE self) {
    domEventTargetRelease(self);
}

void* PR_NULLABLE* PR_NONNULL prDomEventTargetBinding(PrDomEventTarget* self) {
    return &self->binding;
}

void prDomEventTargetAddEventListener(PrDomEventTarget* self, PrStringView type,
                                      const PrDomEventListener* PR_NULLABLE callback,
                                      const PrDomAddEventListenerOptions* PR_NULLABLE options) {
    if (callback != nullptr)
        domListenerAdd(self, type, callback, options);
}

void prDomEventTargetRemoveEventListener(PrDomEventTarget* self, PrStringView type,
                                         const PrDomEventListener* PR_NULLABLE callback,
                                         const PrDomEventListenerOptions* PR_NULLABLE options) {
    if (callback != nullptr)
        domListenerRemove(self, type, callback,
                          options != nullptr && options->capture);
}

// dom/#dom-eventtarget-dispatchevent, which marks the event untrusted.
PrStatus prDomEventTargetDispatchEvent(PrDomEventTarget* self, PrDomEvent* event,
                                       bool* PR_NULLABLE out) {
    if (event->dispatching || !event->initialized)
        return PrStatus_InvalidState;
    event->trusted = false;
    bool proceed = domEventDispatch(self, event);
    if (out != nullptr)
        *out = proceed;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// EventHandler

PrDomEventHandlerCallback prDomEventTargetEventHandler(PrDomEventTarget* self,
                                                       PrDomEventHandler handler) {
    PrDomEventTarget* target = domEventHandlerTarget(self, handler);
    return target != nullptr ? domEventHandler(target, handler) : PrDomEventHandlerCallback{};
}

void prDomEventTargetSetEventHandler(PrDomEventTarget* self, PrDomEventHandler handler,
                                     const PrDomEventHandlerCallback* PR_NULLABLE callback) {
    if (PrDomEventTarget* target = domEventHandlerTarget(self, handler))
        domSetEventHandler(target, handler, callback);
}

// ---------------------------------------------------------------------------------------------------------------------
// Node

PrDomNode* PR_NULLABLE prDomNodeQuery(PrDomEventTarget* PR_NULLABLE object) {
    return domNodeFromTarget(object);
}

void prDomNodeRetain(PrDomNode* self) {
    domNodeRetain(self);
}

void prDomNodeRelease(PrDomNode* PR_NULLABLE self) {
    domNodeRelease(self);
}

PrDomNode* prDomNodeTreeRoot(PrDomNode* self) {
    return domTreeRoot(self);
}

PrDomNode* PR_NULLABLE prDomNodeTreeParent(PrDomNode* self) {
    return domNodeOwner(self);
}

PrDomNodeType prDomNodeType(const PrDomNode* self) {
    return self->type;
}

PrString prDomNodeBaseUri(const PrDomNode* self) {
    PrUrl* base = domDocumentBaseUrl(self->document);
    PrString result = prUrlSerialize(base, PrUrlSerialization_Whole);
    prUrlDestroy(base);
    return result;
}

bool prDomNodeIsConnected(const PrDomNode* self) {
    return domIsConnected(self);
}

PrDomDocument* PR_NULLABLE prDomNodeOwnerDocument(PrDomNode* self) {
    return self->type != PrDomNodeType_Document ? self->document : nullptr;
}

void prDomNodeVisitSubtree(PrDomNode* self,
                           void (*visit)(void* PR_NULLABLE context, PrDomNode* node),
                           void* PR_NULLABLE context) {
    struct Visit {
        void (*visit)(void* PR_NULLABLE context, PrDomNode* node);
        void* PR_NULLABLE context;
    } state = {visit, context};
    domVisitSubtree(self, [](void* data, PrDomNode* node) {
        Visit* given = static_cast<Visit*>(data);
        given->visit(given->context, node);
    }, &state);
}

PrDomNode* PR_NULLABLE prDomNodeParentNode(PrDomNode* self) {
    return domNodeParent(self);
}

PrDomElement* PR_NULLABLE prDomNodeParentElement(PrDomNode* self) {
    return domElementFromNode(domNodeParent(self));
}

bool prDomNodeHasChildNodes(const PrDomNode* self) {
    return self->first != nullptr;
}

PrDomNode* PR_NULLABLE prDomNodeFirstChild(PrDomNode* self) {
    return self->first;
}

PrDomNode* PR_NULLABLE prDomNodeLastChild(PrDomNode* self) {
    return self->last;
}

PrDomNode* PR_NULLABLE prDomNodePreviousSibling(PrDomNode* self) {
    return domNodeParent(self) != nullptr ? self->previous : nullptr;
}

PrDomNode* PR_NULLABLE prDomNodeNextSibling(PrDomNode* self) {
    return domNodeParent(self) != nullptr ? self->next : nullptr;
}

PrString prDomNodeTextContent(const PrDomNode* self) {
    return domNodeTextContent(self);
}

void prDomNodeSetTextContent(PrDomNode* self, PrStringView value) {
    domSetTextContent(self, value);
}

PrStatus prDomNodeInsertBefore(PrDomNode* self, PrDomNode* node, PrDomNode* PR_NULLABLE child) {
    return domPreInsert(self, node, child);
}

PrStatus prDomNodeAppendChild(PrDomNode* self, PrDomNode* node) {
    return domPreInsert(self, node, nullptr);
}

PrStatus prDomNodeReplaceChild(PrDomNode* self, PrDomNode* node, PrDomNode* child) {
    return domReplace(self, node, child);
}

PrStatus prDomNodeRemoveChild(PrDomNode* self, PrDomNode* child) {
    return domPreRemove(self, child);
}

PrString prDomNodeName(const PrDomNode* self) {
    switch (self->type) {
        case PrDomNodeType_Element: return domElementTagName(static_cast<const PrDomElement*>(self));
        case PrDomNodeType_Attribute: return attributeQualifiedName(static_cast<const PrDomAttr*>(self));
        case PrDomNodeType_ProcessingInstruction:
            return prStringClone(&static_cast<const PrDomProcessingInstruction*>(self)->target);
        case PrDomNodeType_DocumentType:
            return prStringClone(&static_cast<const PrDomDocumentType*>(self)->name);
        default: {
            PrStringView name = prDomNodeTypeName(self->type);
            return name.data != nullptr ? prStringExternal(name) : prStringEmpty();
        }
    }
}

PrStringView prDomNodeTypeName(PrDomNodeType type) {
    switch (type) {
        case PrDomNodeType_Text: return "#text";
        case PrDomNodeType_CdataSection: return "#cdata-section";
        case PrDomNodeType_Comment: return "#comment";
        case PrDomNodeType_Document: return "#document";
        case PrDomNodeType_DocumentFragment: return "#document-fragment";
        default: return PrStringView{};
    }
}

PrStringView prDomNodeValue(const PrDomNode* self) {
    switch (self->type) {
        case PrDomNodeType_Attribute: return prStringView(&static_cast<const PrDomAttr*>(self)->value);
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_ProcessingInstruction:
        case PrDomNodeType_Comment:
            return prStringView(&static_cast<const PrDomCharacterData*>(self)->data);
        default: return {};
    }
}

void prDomNodeSetValue(PrDomNode* self, PrStringView value) {
    // The node's value is its text content, if it has one.
    if (self->type != PrDomNodeType_Element && self->type != PrDomNodeType_Document
        && self->type != PrDomNodeType_DocumentFragment && self->type != PrDomNodeType_DocumentType)
        domSetTextContent(self, value);
}

PrDomNode* prDomNodeGetRootNode(PrDomNode* self, bool composed) {
    return const_cast<PrDomNode*>(composed ? domShadowIncludingRoot(self) : domNodeRoot(self));
}

bool prDomNodeContains(const PrDomNode* self, const PrDomNode* PR_NULLABLE other) {
    for (const PrDomNode* ancestor = other; ancestor != nullptr;
         ancestor = domNodeParent(ancestor)) {
        if (ancestor == self)
            return true;
    }
    return false;
}

bool prDomNodeIsEqualNode(const PrDomNode* self, const PrDomNode* PR_NULLABLE other) {
    return other != nullptr && domNodeEquals(self, other);
}

PrDomNodePosition prDomNodeCompareDocumentPosition(const PrDomNode* self, const PrDomNode* other) {
    const PrDomNode* node1 = other;
    if (node1 == self)
        return 0;
    // dom/#dom-node-comparedocumentposition: an attribute stands for its element. Two attributes of one element are
    // ordered by the element's attribute list.
    const PrDomNode* node2 = self;
    const PrDomAttr* attr1 =
        node1->type == PrDomNodeType_Attribute ? static_cast<const PrDomAttr*>(node1) : nullptr;
    const PrDomAttr* attr2 =
        node2->type == PrDomNodeType_Attribute ? static_cast<const PrDomAttr*>(node2) : nullptr;
    if (attr1 != nullptr)
        node1 = attr1->parent;
    if (attr2 != nullptr) {
        node2 = attr2->parent;
        if (attr1 != nullptr && node1 != nullptr && node1 == node2) {
            for (const PrDomAttr* at = static_cast<const PrDomElement*>(node2)->firstAttribute;
                 at != nullptr; at = static_cast<const PrDomAttr*>(at->next)) {
                if (at == attr1)
                    return PrDomNodePosition_ImplementationSpecific
                           | PrDomNodePosition_Preceding;
                if (at == attr2)
                    return PrDomNodePosition_ImplementationSpecific
                           | PrDomNodePosition_Following;
            }
        }
    }
    if (node1 == nullptr || node2 == nullptr || domNodeRoot(node1) != domNodeRoot(node2)) {
        // An order that is consistent across calls: the order of the nodes' addresses.
        PrDomNodePosition position =
            PrDomNodePosition_Disconnected | PrDomNodePosition_ImplementationSpecific;
        position |= uintptr_t(node1) < uintptr_t(node2) ? PrDomNodePosition_Preceding
                                                        : PrDomNodePosition_Following;
        return position;
    }
    if ((attr1 == nullptr && domIsInclusiveAncestor(node1, node2))
        || (attr2 != nullptr && node1 == node2))
        return PrDomNodePosition_Contains | PrDomNodePosition_Preceding;
    if ((attr2 == nullptr && domIsInclusiveAncestor(node2, node1))
        || (attr1 != nullptr && node1 == node2))
        return PrDomNodePosition_ContainedBy | PrDomNodePosition_Following;
    DomBoundary first = {const_cast<PrDomNode*>(node1), 0};
    DomBoundary second = {const_cast<PrDomNode*>(node2), 0};
    return domBoundaryCompare(first, second) < 0 ? PrDomNodePosition_Preceding
                                                 : PrDomNodePosition_Following;
}

void prDomNodeNormalize(PrDomNode* self) {
    domNormalize(self);
}

PrStatus prDomNodeCloneNode(PrDomNode* self, bool deep, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* clone = made(domClone(self, self->document, deep));
    *out = clone;
    return clone != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
}

PrDomElement* PR_NULLABLE prDomNodeGetElementById(PrDomNode* self, PrStringView id) {
    return domElementById(self, id);
}

PrDomElement* PR_NULLABLE prDomNodeFirstElementChild(PrDomNode* self) {
    PrDomNode* child = self->first;
    while (child != nullptr && child->type != PrDomNodeType_Element)
        child = child->next;
    return domElementFromNode(child);
}

PrDomElement* PR_NULLABLE prDomNodeLastElementChild(PrDomNode* self) {
    PrDomNode* child = self->last;
    while (child != nullptr && child->type != PrDomNodeType_Element)
        child = child->previous;
    return domElementFromNode(child);
}

uint32_t prDomNodeChildElementCount(const PrDomNode* self) {
    uint32_t count = 0;
    for (const PrDomNode* child = self->first; child != nullptr;
         child = child->next)
        count += child->type == PrDomNodeType_Element;
    return count;
}

PrStatus prDomNodePrepend(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                          uint32_t count) {
    return domPrepend(self, nodes, count);
}

PrStatus prDomNodeAppend(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                         uint32_t count) {
    return domAppend(self, nodes, count);
}

PrStatus prDomNodeReplaceChildren(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                                  uint32_t count) {
    return domReplaceChildren(self, nodes, count);
}

PrDomElement* PR_NULLABLE prDomNodePreviousElementSibling(PrDomNode* self) {
    PrDomNode* sibling = self->parent != nullptr ? self->previous : nullptr;
    while (sibling != nullptr && sibling->type != PrDomNodeType_Element)
        sibling = sibling->previous;
    return domElementFromNode(sibling);
}

PrDomElement* PR_NULLABLE prDomNodeNextElementSibling(PrDomNode* self) {
    PrDomNode* sibling = self->parent != nullptr ? self->next : nullptr;
    while (sibling != nullptr && sibling->type != PrDomNodeType_Element)
        sibling = sibling->next;
    return domElementFromNode(sibling);
}

PrStatus prDomNodeBefore(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                         uint32_t count) {
    return domBefore(self, nodes, count);
}

PrStatus prDomNodeAfter(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                        uint32_t count) {
    return domAfter(self, nodes, count);
}

PrStatus prDomNodeReplaceWith(PrDomNode* self, const PrDomNodeOrString* PR_NULLABLE nodes,
                              uint32_t count) {
    return domReplaceWith(self, nodes, count);
}

void prDomNodeRemove(PrDomNode* self) {
    domRemoveFromParent(self);
}

PrDomDocument* PR_NULLABLE prDomDocumentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocument>(object, PrDomNodeType_Document);
}

PrDomDocument* prDomDocumentCreate(void) {
    return domDocumentCreate(DomDocumentKind_Xml);
}

PrStatus prDomDocumentCreateXml(PrDomImplementation* implementation, PrStringView namespaceUrl,
                                PrStringView qualifiedName, PrDomDocumentType* PR_NULLABLE doctype,
                                PrDomDocument* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDomDocument* handle = prDomDocumentCreate();
    domNotifyDocumentCreated(implementation, handle);
    PrDomElement* element = nullptr;
    if (qualifiedName.size != 0) {
        PrStatus status = prDomElementCreateNs(handle, namespaceUrl, qualifiedName, nullptr, &element);
        if (status != PrStatus_Ok) {
            prDomDocumentRelease(handle);
            return status;
        }
    }
    PrDomDocument* document = handle;
    if (doctype != nullptr)
        domInsert(document, doctype, nullptr);
    if (element != nullptr) {
        domInsert(document, element, nullptr);
        prDomNodeRelease(prDomElementAsNode(element));
    }
    // The content type follows the namespace: XHTML, SVG, or XML.
    PrStringView type = namespaceUrl == "http://www.w3.org/1999/xhtml"
                            ? "application/xhtml+xml"
                        : namespaceUrl == "http://www.w3.org/2000/svg"
                            ? "image/svg+xml"
                            : "application/xml";
    document->contentType = prStringExternal(type);
    *out = handle;
    return PrStatus_Ok;
}

PrDomDocument* prDomDocumentCreateHtml(PrDomImplementation* implementation, PrStringView title) {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    domNotifyDocumentCreated(implementation, document);
    domInsert(document, domDocumentTypeCreate(document, "html", "", ""), nullptr);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, PrStringView{});
    domInsert(document, html, nullptr);
    PrDomElement* head = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Head, PrStringView{});
    domInsert(html, head, nullptr);
    if (title.data != nullptr) {
        PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Title, PrStringView{});
        domInsert(head, element, nullptr);
        domInsert(element, domTextCreate(document, title), nullptr);
    }
    domInsert(html, domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, PrStringView{}),
              nullptr);
    return document;
}

void prDomDocumentRetain(PrDomDocument* self) {
    domNodeRetain(self);
}

void prDomDocumentRelease(PrDomDocument* PR_NULLABLE self) {
    domNodeRelease(self);
}

void prDomDocumentSetTreeCallback(PrDomDocument* self,
                                  const PrDomTreeCallback* PR_NULLABLE callback) {
    domDocumentSetTreeCallback(self,
                               callback != nullptr ? *callback : PrDomTreeCallback{});
}

PrString prDomDocumentUrl(const PrDomDocument* self) {
    return prUrlSerialize(self->url, PrUrlSerialization_Whole);
}

PrStringView prDomDocumentContentType(const PrDomDocument* self) {
    return prStringView(&self->contentType);
}

PrStringView prDomDocumentCharacterSet(const PrDomDocument* self) {
    return prCharsetName(self->charset);
}

PrDomElement* PR_NULLABLE prDomDocumentElement(PrDomDocument* self) {
    return domDocumentElement(self);
}

PrStatus prDomDocumentImportNode(PrDomDocument* self, PrDomNode* node, bool deep,
                                 PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* clone = domNodeShadowRoot(node) == nullptr ? made(domClone(node, self, deep)) : nullptr;
    *out = clone;
    return clone != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
}

PrStatus prDomElementCreate(PrDomDocument* self, PrStringView localName,
                            const PrDomElementCreationOptions* PR_NULLABLE options,
                            PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    if (!domIsValidElementLocalName(localName))
        return PrStatus_InvalidData;
    // An HTML document lowercases the name. An HTML or XHTML document creates an HTML element.
    PrDomDocument* document = self;
    TextBuffer buffer;
    PrStringView name = lowered(localName, document->kind == DomDocumentKind_Html, &buffer);
    bool html = document->kind == DomDocumentKind_Html
        || prStringView(&document->contentType) == "application/xhtml+xml";
    PrStringView is = options != nullptr ? options->is : PrStringView{};
    bool held = false;
    PrDomElement* element =
        made(html ? domElementCreateHtml(document, domTagFromText(name), name, is, true, &held)
                  : domElementCreate(document, PrDomNamespace_None, PrDomTag_Unknown, name));
    if (held)
        domNodeUnhold(element);
    textReset(&buffer);
    if (out != nullptr)
        *out = element;
    else
        domNodeRelease(element);
    return PrStatus_Ok;
}

PrStatus prDomElementCreateNs(PrDomDocument* self, PrStringView namespaceUrl,
                              PrStringView qualifiedName,
                              const PrDomElementCreationOptions* PR_NULLABLE options,
                              PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(self, namespaceUrl, qualifiedName, true, &space, &prefix, &local);
    if (status != PrStatus_Ok)
        return status;
    PrDomElement* element = nullptr;
    if (space == PrDomNamespace_Html) {
        bool held = false;
        element = made(domElementCreateHtml(self, domTagFromText(local), local,
                                            options != nullptr ? options->is : PrStringView{}, true,
                                            &held));
        if (held)
            domNodeUnhold(element);
        if (prefix.data != nullptr)
            domElementState(element)->prefix = prStringCopy(prefix);
    } else {
        element = made(domElementCreateNs(self, space, prefix, local));
    }
    *out = element;
    return status;
}

PrStatus prDomAttrCreate(PrDomDocument* self, PrStringView localName,
                         PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!domIsValidAttributeLocalName(localName))
        return PrStatus_InvalidData;
    TextBuffer buffer;
    PrStringView name = lowered(localName, self->kind == DomDocumentKind_Html, &buffer);
    *out = made(domAttrCreate(self, PrDomNamespace_None, PrStringView{}, name, ""));
    textReset(&buffer);
    return PrStatus_Ok;
}

PrStatus prDomAttrCreateNs(PrDomDocument* self, PrStringView namespaceUrl,
                           PrStringView qualifiedName,
                           PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(self, namespaceUrl, qualifiedName, false, &space, &prefix, &local);
    if (status == PrStatus_Ok)
        *out = made(domAttrCreate(self, space, prefix, local, ""));
    return status;
}

PrDomDocumentFragment* prDomDocumentFragmentCreate(PrDomDocument* self) {
    return made(domDocumentFragmentCreate(self));
}

PrDomText* prDomTextCreate(PrDomDocument* self, PrStringView data) {
    return made(domTextCreate(self, data));
}

PrStatus prDomTextCreateCdataSection(PrDomDocument* self, PrStringView data,
                                     PrDomText* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (self->kind == DomDocumentKind_Html)
        return PrStatus_NotSupported;
    if (stringContains(data, "]]>"))
        return PrStatus_InvalidData;
    *out = made(domTextCreateCdataSection(self, data));
    return PrStatus_Ok;
}

PrDomComment* prDomCommentCreate(PrDomDocument* self, PrStringView data) {
    return made(domCommentCreate(self, data));
}

PrStatus
prDomProcessingInstructionCreate(PrDomDocument* self, PrStringView target,
                                 PrStringView data,
                                 PrDomProcessingInstruction* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!prXmlIsName(target) || stringContains(data, "?>"))
        return PrStatus_InvalidData;
    *out = made(domProcessingInstructionCreate(self, target, data));
    return PrStatus_Ok;
}

// html/#document.title
PrString prDomDocumentTitle(const PrDomDocument* self) {
    const PrDomElement* root = domDocumentElement(self);
    const PrDomElement* title =
        isSvgRoot(root) ? firstElement(root, PrDomNamespace_Svg, PrDomTag_Title, true)
                        : firstElement(self, PrDomNamespace_Html, PrDomTag_Title, false);
    if (title == nullptr)
        return prStringEmpty();
    PrString text = domChildTextContent(title);
    PrString result = stripAndCollapse(prStringView(&text));
    prStringReset(&text);
    return result;
}

void prDomDocumentSetTitle(PrDomDocument* self, PrStringView value) {
    PrDomElement* root = domDocumentElement(self);
    PrDomElement* title = nullptr;
    if (isSvgRoot(root)) {
        title = firstElement(root, PrDomNamespace_Svg, PrDomTag_Title, true);
        if (title == nullptr) {
            title = domElementCreate(self, PrDomNamespace_Svg, PrDomTag_Title, "title");
            domInsert(root, title, root->first);
        }
    } else if (root != nullptr && root->space == PrDomNamespace_Html) {
        title = firstElement(self, PrDomNamespace_Html, PrDomTag_Title, false);
        PrDomElement* head = domDocumentHead(self);
        if (title == nullptr && head == nullptr)
            return;
        if (title == nullptr) {
            title = domElementCreate(self, PrDomNamespace_Html, PrDomTag_Title, "title");
            domInsert(head, title, nullptr);
        }
    } else {
        return;
    }
    domStringReplaceAll(title, value);
}

PrDomElement* PR_NULLABLE prDomDocumentBody(PrDomDocument* self) {
    return domDocumentBody(self);
}

PrStatus prDomDocumentSetBody(PrDomDocument* self, PrDomElement* PR_NULLABLE value) {
    if (!domIsHtml(value, PrDomTag_Body) && !domIsHtml(value, PrDomTag_Frameset))
        return PrStatus_NotAllowed;
    PrDomElement* body = domDocumentBody(self);
    if (value == body)
        return PrStatus_Ok;
    if (body != nullptr)
        return domReplace(body->parent, value, body);
    PrDomElement* root = domDocumentElement(self);
    return root != nullptr ? domPreInsert(root, value, nullptr) : PrStatus_NotAllowed;
}

PrDomElement* PR_NULLABLE prDomDocumentHead(PrDomDocument* self) {
    return domDocumentHead(self);
}

PrDomDocumentType* PR_NULLABLE prDomDocumentDoctype(PrDomDocument* self) {
    for (PrDomNode* child = self->first; child != nullptr;
         child = child->next) {
        if (child->type == PrDomNodeType_DocumentType)
            return static_cast<PrDomDocumentType*>(child);
    }
    return nullptr;
}

bool prDomDocumentQuirks(const PrDomDocument* self) {
    return self->quirks == DomQuirks_Yes;
}

PrDomImplementation* prDomDocumentImplementation(PrDomDocument* self) {
    return static_cast<PrDomImplementation*>(self);
}

PrStatus prDomDocumentTypeCreate(PrDomImplementation* self, PrStringView name,
                                 PrStringView publicId, PrStringView systemId,
                                 PrDomDocumentType* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // dom/#valid-doctype-name: no ASCII whitespace, U+0000 or '>'.
    for (size_t i = 0; i < name.size; ++i) {
        if (asciiIsSpace(name.data[i]) || name.data[i] == '\0' || name.data[i] == '>')
            return PrStatus_InvalidData;
    }
    *out = made(domDocumentTypeCreate(self, name, publicId, systemId));
    return PrStatus_Ok;
}

PrDomDocumentType* PR_NULLABLE prDomDocumentTypeQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocumentType>(object, PrDomNodeType_DocumentType);
}

PrStringView prDomDocumentTypeName(const PrDomDocumentType* self) {
    return prStringView(&self->name);
}

// A doctype that the parser created without an identifier holds the null string, which script sees as the empty string.
PrStringView prDomDocumentTypePublicId(const PrDomDocumentType* self) {
    const PrString* id = &self->publicId;
    return prStringIsNull(id) ? "" : prStringView(id);
}

PrStringView prDomDocumentTypeSystemId(const PrDomDocumentType* self) {
    const PrString* id = &self->systemId;
    return prStringIsNull(id) ? "" : prStringView(id);
}

PrDomDocumentFragment* PR_NULLABLE
prDomDocumentFragmentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocumentFragment>(object, PrDomNodeType_DocumentFragment);
}

PrDomShadowRoot* PR_NULLABLE prDomShadowRootQuery(PrDomEventTarget* PR_NULLABLE object) {
    return domNodeShadowRoot(domNodeFromTarget(object));
}

PrDomShadowRootMode prDomShadowRootMode(const PrDomShadowRoot* self) {
    return self->mode;
}

bool prDomShadowRootDelegatesFocus(const PrDomShadowRoot* self) {
    return self->delegatesFocus;
}

PrDomSlotAssignmentMode prDomShadowRootSlotAssignment(const PrDomShadowRoot* self) {
    return self->slotAssignment;
}

bool prDomShadowRootClonable(const PrDomShadowRoot* self) {
    return self->clonable;
}

bool prDomShadowRootSerializable(const PrDomShadowRoot* self) {
    return self->serializable;
}

PrDomElement* prDomShadowRootHost(PrDomShadowRoot* self) {
    return self->host;
}

PrStringView prDomNamespaceUrl(PrDomNamespace space) {
    return space < PrDomNamespace_Other ? domNamedNamespaceUrl(DomNamespaceId(space))
                                        : PrStringView{};
}

PrStringView prDomTagName(PrDomTag tag) {
    return tag != PrDomTag_Unknown ? domTagName(tag) : PrStringView{};
}

PrStringView prDomTagUpperName(PrDomTag tag) {
    return tag != PrDomTag_Unknown ? domTagUpperName(tag) : PrStringView{};
}

PrStringView prDomInputTypeKeyword(PrDomInputType type) {
    return type < PrDomInputType_Count ? domInputTypeKeyword(type) : PrStringView{};
}

PrDomElement* PR_NULLABLE prDomElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomElement>(object, PrDomNodeType_Element);
}

PrStringView prDomElementLocalName(const PrDomElement* self) {
    return domElementName(self);
}

PrDomTag prDomElementTag(const PrDomElement* self) {
    return domElementTag(self);
}

PrStringView prDomElementNamespaceUri(const PrDomElement* self) {
    return domNamespaceUrl(self->document, self->space);
}

PrDomNamespace prDomElementNamespace(const PrDomElement* self) {
    DomNamespaceId space = self->space;
    return space < PrDomNamespace_Other ? PrDomNamespace(space) : PrDomNamespace_Other;
}

PrStringView prDomElementPrefix(const PrDomElement* self) {
    const PrDomElement* element = self;
    return element->state != nullptr ? prStringView(&element->state->prefix) : PrStringView{};
}

PrString prDomElementTagName(const PrDomElement* self) {
    return domElementTagName(self);
}

PrDomTag prDomElementTagNameTag(const PrDomElement* self, bool* uppercase) {
    const PrDomElement* element = self;
    *uppercase = domIsHtmlInHtmlDocument(element);
    bool prefixed = element->state != nullptr && !prStringIsNull(&element->state->prefix);
    return prefixed ? PrDomTag_Unknown : domElementTag(element);
}

PrStringView prDomElementId(const PrDomElement* self) {
    PrStringView id = domElementId(self);
    return id.data != nullptr ? id : "";
}

void prDomElementSetId(PrDomElement* self, PrStringView value) {
    domSetAttribute(self, PrDomNamespace_None, "id", value);
}

PrStringView prDomElementSlot(const PrDomElement* self) {
    PrStringView slot = domAttributeValue(self, "slot");
    return slot.data != nullptr ? slot : "";
}

void prDomElementSetSlot(PrDomElement* self, PrStringView value) {
    domSetAttribute(self, PrDomNamespace_None, "slot", value);
}

PrStringView prDomElementGetAttribute(const PrDomElement* self, PrStringView qualifiedName) {
    const PrDomAttr* attribute = domAttributeByName(self, qualifiedName);
    return attribute != nullptr ? prStringView(&attribute->value) : PrStringView{};
}

PrStatus prDomElementSetAttribute(PrDomElement* self, PrStringView qualifiedName,
                                  PrStringView value) {
    if (!domIsValidAttributeLocalName(qualifiedName))
        return PrStatus_InvalidData;
    TextBuffer buffer;
    PrStringView name = lowered(qualifiedName, domIsHtmlInHtmlDocument(self), &buffer);
    // dom/#dom-element-setattribute: sets the attribute with that qualified name, whatever its namespace, or else adds
    // a new attribute in no namespace.
    if (PrDomAttr* attribute = domAttributeByName(self, name))
        domSetAttribute(self, attribute->space, prStringView(&attribute->name), value);
    else
        domSetAttribute(self, PrDomNamespace_None, name, value);
    textReset(&buffer);
    return PrStatus_Ok;
}

void prDomElementRemoveAttribute(PrDomElement* self, PrStringView qualifiedName) {
    if (PrDomAttr* attribute = domAttributeByName(self, qualifiedName))
        domRemoveAttributeNode(self, attribute);
}

bool prDomElementHasAttribute(const PrDomElement* self, PrStringView qualifiedName) {
    return domAttributeByName(self, qualifiedName) != nullptr;
}

bool prDomElementHasAttributes(const PrDomElement* self) {
    return self->firstAttribute != nullptr;
}

PrStringView prDomElementGetAttributeNs(const PrDomElement* self, PrStringView namespaceUrl,
                                        PrStringView localName) {
    const PrDomAttr* attribute = attributeNs(self, namespaceUrl, localName);
    return attribute != nullptr ? prStringView(&attribute->value) : PrStringView{};
}

PrStatus prDomElementSetAttributeNs(PrDomElement* self, PrStringView namespaceUrl,
                                    PrStringView qualifiedName, PrStringView value) {
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(self->document, namespaceUrl, qualifiedName, false,
                                         &space, &prefix, &local);
    if (status == PrStatus_Ok)
        domSetAttributeNs(self, space, prefix, local, value);
    return status;
}

void prDomElementRemoveAttributeNs(PrDomElement* self, PrStringView namespaceUrl,
                                   PrStringView localName) {
    if (PrDomAttr* attribute = attributeNs(self, namespaceUrl, localName))
        domRemoveAttributeNode(self, attribute);
}

bool prDomElementHasAttributeNs(const PrDomElement* self, PrStringView namespaceUrl,
                                PrStringView localName) {
    return attributeNs(self, namespaceUrl, localName) != nullptr;
}

uint32_t prDomElementGetAttributeNames(const PrDomElement* self, PrString* PR_NULLABLE items,
                                       uint32_t capacity) {
    uint32_t count = 0;
    for (const PrDomAttr* attribute = self->firstAttribute; attribute != nullptr;
         attribute = static_cast<const PrDomAttr*>(attribute->next), ++count) {
        if (count < capacity)
            items[count] = attributeQualifiedName(attribute);
    }
    return count;
}

PrStatus prDomElementToggleAttribute(PrDomElement* self, PrStringView qualifiedName,
                                     const bool* PR_NULLABLE force, bool* PR_NULLABLE out) {
    if (!domIsValidAttributeLocalName(qualifiedName))
        return PrStatus_InvalidData;
    TextBuffer buffer;
    PrStringView name = lowered(qualifiedName, domIsHtmlInHtmlDocument(self), &buffer);
    PrDomAttr* attribute = domAttributeByName(self, name);
    bool present = attribute != nullptr;
    if (attribute == nullptr && (force == nullptr || *force)) {
        domAppendAttribute(self, domAttrCreate(self->document, PrDomNamespace_None, PrStringView{}, name, ""));
        present = true;
    } else if (attribute != nullptr && (force == nullptr || !*force)) {
        domRemoveAttributeNode(self, attribute);
        present = false;
    }
    textReset(&buffer);
    if (out != nullptr)
        *out = present;
    return PrStatus_Ok;
}

uint32_t prDomElementAttributeCount(const PrDomElement* self) {
    return domElementAttributeCount(self);
}

PrDomAttr* PR_NULLABLE prDomElementAttributeAt(PrDomElement* self, uint32_t index) {
    PrDomAttr* attribute = self->firstAttribute;
    for (; attribute != nullptr && index != 0; --index)
        attribute = static_cast<PrDomAttr*>(attribute->next);
    return attribute;
}

uint32_t prDomElementNamedAttributeNames(const PrDomElement* self, PrString* PR_NULLABLE items,
                                         uint32_t capacity) {
    bool html = domIsHtmlInHtmlDocument(self);
    uint32_t count = 0;
    for (const PrDomAttr* attribute = self->firstAttribute; attribute != nullptr;
         attribute = static_cast<const PrDomAttr*>(attribute->next)) {
        PrString name = attributeQualifiedName(attribute);
        PrStringView view = prStringView(&name);
        bool keep = true;
        for (size_t i = 0; html && keep && i < view.size; ++i)
            keep = !asciiIsUpper(view.data[i]);
        // If an earlier attribute has the same name, the name belongs to that attribute.
        for (const PrDomAttr* before = self->firstAttribute; keep && before != attribute;
             before = static_cast<const PrDomAttr*>(before->next)) {
            PrString earlier = attributeQualifiedName(before);
            keep = prStringView(&earlier) != view;
            prStringReset(&earlier);
        }
        if (keep && count < capacity)
            items[count] = name;
        else
            prStringReset(&name);
        count += keep;
    }
    return count;
}

PrDomAttr* PR_NULLABLE prDomElementGetAttributeNode(PrDomElement* self,
                                                    PrStringView qualifiedName) {
    return domAttributeByName(self, qualifiedName);
}

PrDomAttr* PR_NULLABLE prDomElementGetAttributeNodeNs(PrDomElement* self, PrStringView namespaceUrl,
                                                      PrStringView localName) {
    return attributeNs(self, namespaceUrl, localName);
}

PrStatus prDomElementSetAttributeNode(PrDomElement* self, PrDomAttr* attr,
                                      PrDomAttr* PR_NULLABLE* PR_NULLABLE out) {
    PrDomAttr* attribute = attr;
    if (out != nullptr)
        *out = nullptr;
    if (attribute->parent != nullptr && attribute->parent != self)
        return PrStatus_InvalidState;
    PrDomAttr* old = attributeNs(self, domNamespaceUrl(attribute->document, attribute->space),
                                 prStringView(&attribute->name));
    if (old == attribute) {
        if (out != nullptr) {
            domNodeRetain(attribute);
            *out = attr;
        }
        return PrStatus_Ok;
    }
    if (old != nullptr && out != nullptr)
        domNodeRetain(old);
    if (old != nullptr)
        domReplaceAttribute(self, old, attribute);
    else
        domAppendAttribute(self, attribute);
    if (out != nullptr)
        *out = old;
    return PrStatus_Ok;
}

PrStatus prDomElementRemoveAttributeNode(PrDomElement* self, PrDomAttr* attr) {
    if (attr->parent != self)
        return PrStatus_NotFound;
    domRemoveAttributeNode(self, attr);
    return PrStatus_Ok;
}

PrStatus prDomElementRemoveNamedAttribute(PrDomElement* self, PrStringView qualifiedName,
                                          PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    return removeNamed(self, domAttributeByName(self, qualifiedName), out);
}

PrStatus prDomElementRemoveNamedAttributeNs(PrDomElement* self, PrStringView namespaceUrl,
                                            PrStringView localName,
                                            PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    return removeNamed(self, attributeNs(self, namespaceUrl, localName), out);
}

PrStatus prDomElementInsertAdjacentElement(PrDomElement* self, PrDomInsertPosition where,
                                           PrDomElement* element,
                                           PrDomElement* PR_NULLABLE* PR_NULLABLE out) {
    bool inserted = false;
    PrStatus status = insertAdjacent(self, where, element, &inserted);
    if (out != nullptr)
        *out = status == PrStatus_Ok && inserted ? element : nullptr;
    return status;
}

PrStatus prDomElementInsertAdjacentText(PrDomElement* self, PrDomInsertPosition where,
                                        PrStringView data) {
    PrDomText* text = domTextCreate(self->document, data);
    bool inserted = false;
    PrStatus status = insertAdjacent(self, where, text, &inserted);
    if (!inserted || status != PrStatus_Ok)
        domDropTree(text);
    return status;
}

PrStatus prDomElementAttachShadow(PrDomElement* self, const PrDomShadowRootInit* init,
                                  PrDomShadowRoot* PR_NULLABLE* PR_NONNULL out) {
    PrDomShadowRoot* shadow = nullptr;
    PrStatus status = domAttachShadow(self, init, &shadow);
    *out = shadow;
    return status;
}

PrDomShadowRoot* PR_NULLABLE prDomElementShadowRoot(PrDomElement* self) {
    PrDomShadowRoot* shadow = domShadowRootOfHost(self);
    return shadow != nullptr && shadow->mode == PrDomShadowRootMode_Open
               ? shadow
               : nullptr;
}

PrStringView prDomElementClassName(const PrDomElement* self) {
    const PrDomAttr* attribute = domAttribute(self, PrDomNamespace_None, "class");
    return attribute != nullptr ? prStringView(&attribute->value) : "";
}

void prDomElementSetClassName(PrDomElement* self, PrStringView value) {
    domSetAttribute(self, PrDomNamespace_None, "class", value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attr

PrDomAttr* PR_NULLABLE prDomAttrQuery(PrDomEventTarget* PR_NULLABLE object) {
    return attrOf(domNodeFromTarget(object));
}

PrStringView prDomAttrNamespaceUri(const PrDomAttr* self) {
    return domNamespaceUrl(self->document, self->space);
}

PrDomNamespace prDomAttrNamespace(const PrDomAttr* self) {
    DomNamespaceId space = self->space;
    return space < PrDomNamespace_Other ? PrDomNamespace(space) : PrDomNamespace_Other;
}

PrStringView prDomAttrPrefix(const PrDomAttr* self) {
    return domAttributePrefix(self);
}

PrStringView prDomAttrLocalName(const PrDomAttr* self) {
    return prStringView(&self->name);
}

PrString prDomAttrName(const PrDomAttr* self) {
    return attributeQualifiedName(self);
}

PrStringView prDomAttrValue(const PrDomAttr* self) {
    return prStringView(&self->value);
}

void prDomAttrSetValue(PrDomAttr* self, PrStringView value) {
    domSetAttributeValue(self, value);
}

PrDomElement* PR_NULLABLE prDomAttrOwnerElement(PrDomAttr* self) {
    return domElementFromNode(self->parent);
}

// ---------------------------------------------------------------------------------------------------------------------
// Slottable

PrDomElement* PR_NULLABLE prDomSlottableAssignedSlot(PrDomNode* slottable) {
    if (slottable->type != PrDomNodeType_Element && slottable->type != PrDomNodeType_Text)
        return nullptr;
    return domFindSlot(slottable, true);
}

PrDomCharacterData* PR_NULLABLE prDomCharacterDataQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomNode* node = domNodeFromTarget(object);
    if (node == nullptr)
        return nullptr;
    switch (node->type) {
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_Comment:
        case PrDomNodeType_ProcessingInstruction:
            return static_cast<PrDomCharacterData*>(node);
        default:
            return nullptr;
    }
}

PrStringView prDomCharacterDataData(const PrDomCharacterData* self) {
    return prStringView(&self->data);
}

void prDomCharacterDataSetData(PrDomCharacterData* self, PrStringView value) {
    domSetData(self, value);
}

uint32_t prDomCharacterDataLength(const PrDomCharacterData* self) {
    return domCharacterDataLength(self);
}

PrStatus prDomCharacterDataSubstringData(const PrDomCharacterData* self, uint32_t offset,
                                         uint32_t count, PrString* out) {
    return domSubstringData(self, offset, count, out);
}

void prDomCharacterDataAppendData(PrDomCharacterData* self, PrStringView data) {
    domReplaceData(self, domCharacterDataLength(self), 0, data);
}

PrStatus prDomCharacterDataInsertData(PrDomCharacterData* self, uint32_t offset,
                                      PrStringView data) {
    return domReplaceData(self, offset, 0, data);
}

PrStatus prDomCharacterDataDeleteData(PrDomCharacterData* self, uint32_t offset, uint32_t count) {
    return domReplaceData(self, offset, count, "");
}

PrStatus prDomCharacterDataReplaceData(PrDomCharacterData* self, uint32_t offset, uint32_t count,
                                       PrStringView data) {
    return domReplaceData(self, offset, count, data);
}

PrDomText* PR_NULLABLE prDomTextQuery(PrDomEventTarget* PR_NULLABLE object) {
    // A CDATASection is a Text (dom/#interface-cdatasection).
    PrDomText* text = nodeOfType<PrDomText>(object, PrDomNodeType_Text);
    return text != nullptr ? text : nodeOfType<PrDomText>(object, PrDomNodeType_CdataSection);
}

// dom/#concept-text-split
PrStatus prDomTextSplitText(PrDomText* self, uint32_t offset,
                            PrDomText* PR_NULLABLE* PR_NONNULL out) {
    PrDomText* created = domSplitText(self, offset);
    if (out != nullptr)
        *out = made(created);
    return created != nullptr ? PrStatus_Ok : PrStatus_OutOfRange;
}

// dom/#dom-text-wholetext: the data of the contiguous Text nodes, in tree order.
PrString prDomTextWholeText(const PrDomText* self) {
    const PrDomNode* first = self;
    while (first->previous != nullptr && first->parent != nullptr && domIsText(first->previous))
        first = first->previous;
    TextBuffer out;
    for (const PrDomNode* node = first; node != nullptr && domIsText(node);
         node = node->parent != nullptr ? node->next : nullptr)
        textAppend(&out, prStringView(&static_cast<const PrDomText*>(node)->data));
    return textTake(&out);
}

PrDomComment* PR_NULLABLE prDomCommentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomComment>(object, PrDomNodeType_Comment);
}

PrDomProcessingInstruction* PR_NULLABLE
prDomProcessingInstructionQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomProcessingInstruction>(object, PrDomNodeType_ProcessingInstruction);
}

PrStringView prDomProcessingInstructionTarget(const PrDomProcessingInstruction* self) {
    return prStringView(&self->target);
}

PrDomHtmlCollection* prDomHtmlCollectionCreateChildren(PrDomNode* parent) {
    return domHtmlCollectionCreate(parent, DomCollectionFilter_Children, PrStringView{});
}

PrDomHtmlCollection* prDomHtmlCollectionCreateByClassName(PrDomNode* root, PrStringView classNames) {
    return domHtmlCollectionCreate(root, DomCollectionFilter_ClassNames, classNames);
}

PrDomHtmlCollection* prDomHtmlCollectionCreateByTagName(PrDomNode* root, PrStringView qualifiedName) {
    return domHtmlCollectionCreate(root, DomCollectionFilter_QualifiedName, qualifiedName);
}

uint32_t prDomHtmlCollectionLength(const PrDomHtmlCollection* self) {
    return domHtmlCollectionLength(self);
}

PrDomElement* PR_NULLABLE prDomHtmlCollectionElementAt(PrDomHtmlCollection* self, uint32_t index) {
    return domHtmlCollectionItem(self, index);
}

PrDomElement* PR_NULLABLE prDomHtmlCollectionNamedItem(PrDomHtmlCollection* self,
                                                       PrStringView name) {
    return domHtmlCollectionNamedItem(self, name);
}

uint32_t prDomHtmlCollectionNames(const PrDomHtmlCollection* self, PrStringView* PR_NULLABLE items,
                                  uint32_t capacity) {
    Array<PrStringView> names;
    domHtmlCollectionNames(self, &names);
    for (uint32_t i = 0; i < names.count && i < capacity; ++i)
        items[i] = names.data[i];
    uint32_t count = names.count;
    names.reset();
    return count;
}

void prDomHtmlCollectionDestroy(PrDomHtmlCollection* PR_NULLABLE self) {
    domHtmlCollectionDestroy(self);
}

PrDomNodeList* prDomNodeListCreate(void) {
    return domNodeListCreate(nullptr);
}

PrDomNodeList* prDomNodeListCreateChildNodes(PrDomNode* parent) {
    return domNodeListCreate(parent);
}

PrDomNode* PR_NULLABLE prDomNodeListNodeAt(PrDomNodeList* self, uint32_t index) {
    return domNodeListItem(self, index);
}

uint32_t prDomNodeListLength(const PrDomNodeList* self) {
    return domNodeListLength(self);
}

void prDomNodeListDestroy(PrDomNodeList* PR_NULLABLE self) {
    domNodeListDestroy(self);
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMTokenList

PrDomTokenList* prDomTokenListCreate(PrDomElement* element, PrStringView name) {
    return domTokenListCreate(element, name);
}

uint32_t prDomTokenListLength(const PrDomTokenList* self) {
    return domTokenListLength(self);
}

PrStringView prDomTokenListItem(const PrDomTokenList* self, uint32_t index) {
    return domTokenListItem(self, index);
}

bool prDomTokenListContains(const PrDomTokenList* self, PrStringView token) {
    return domTokenListContains(self, token);
}

PrStatus prDomTokenListAdd(PrDomTokenList* self, const PrStringView* PR_NULLABLE tokens,
                           uint32_t count) {
    return domTokenListAdd(self, tokens, count);
}

PrStatus prDomTokenListRemove(PrDomTokenList* self, const PrStringView* PR_NULLABLE tokens,
                              uint32_t count) {
    return domTokenListRemove(self, tokens, count);
}

PrStatus prDomTokenListToggle(PrDomTokenList* self, PrStringView token,
                              const bool* PR_NULLABLE force, bool* out) {
    return domTokenListToggle(self, token, force, out);
}

PrStatus prDomTokenListReplace(PrDomTokenList* self, PrStringView token, PrStringView newToken,
                               bool* out) {
    return domTokenListReplace(self, token, newToken, out);
}

PrStringView prDomTokenListValue(const PrDomTokenList* self) {
    return domTokenListValue(self);
}

void prDomTokenListSetValue(PrDomTokenList* self, PrStringView value) {
    domTokenListSetValue(self, value);
}

void prDomTokenListDestroy(PrDomTokenList* PR_NULLABLE self) {
    domTokenListDestroy(self);
}

// ---------------------------------------------------------------------------------------------------------------------
// MutationObserver

PrDomMutationObserver* prDomMutationObserverCreate(const PrDomMutationCallback* callback) {
    return domMutationObserverCreate(callback);
}

void prDomMutationObserverRetain(PrDomMutationObserver* self) {
    domMutationObserverRetain(self);
}

void prDomMutationObserverRelease(PrDomMutationObserver* PR_NULLABLE self) {
    domMutationObserverRelease(self);
}

PrStatus prDomMutationObserverObserve(PrDomMutationObserver* self, PrDomNode* target,
                                      const PrDomMutationObserverInit* options,
                                      const PrStringView* PR_NULLABLE filter,
                                      uint32_t filterCount) {
    return domMutationObserverObserve(self,
                                      target, options, filter, filterCount);
}

void prDomMutationObserverDisconnect(PrDomMutationObserver* self) {
    domMutationObserverDisconnect(self);
}

uint32_t prDomMutationObserverRecordCount(const PrDomMutationObserver* self) {
    return self->records.count;
}

bool prDomMutationObserverTakeRecord(PrDomMutationObserver* self, PrDomMutationRecord* record) {
    return domMutationObserverTake(self, record);
}

// ---------------------------------------------------------------------------------------------------------------------
// MutationRecord

PrDomMutationRecord* prDomMutationRecordCreate(void) {
    return allocZeroed<PrDomMutationRecord>();
}

void prDomMutationRecordDestroy(PrDomMutationRecord* PR_NULLABLE self) {
    domMutationRecordDestroy(self);
}

PrDomMutationType prDomMutationRecordType(const PrDomMutationRecord* self) {
    return self->type;
}

PrDomNode* prDomMutationRecordTarget(PrDomMutationRecord* self) {
    return self->target;
}

void prDomMutationRecordAddedNodes(PrDomMutationRecord* self, PrDomNodeList* list) {
    domNodeListAssign(list, self->added.data, self->added.count);
}

void prDomMutationRecordRemovedNodes(PrDomMutationRecord* self, PrDomNodeList* list) {
    domNodeListAssign(list, self->removed.data, self->removed.count);
}

PrDomNode* PR_NULLABLE prDomMutationRecordPreviousSibling(PrDomMutationRecord* self) {
    return self->previous;
}

PrDomNode* PR_NULLABLE prDomMutationRecordNextSibling(PrDomMutationRecord* self) {
    return self->next;
}

PrStringView prDomMutationRecordAttributeName(const PrDomMutationRecord* self) {
    return prStringView(&self->attributeName);
}

PrStringView prDomMutationRecordAttributeNamespace(const PrDomMutationRecord* self) {
    return prStringView(&self->attributeNamespace);
}

PrStringView prDomMutationRecordOldValue(const PrDomMutationRecord* self) {
    return prStringView(&self->oldValue);
}

PrDomRange* prDomRangeCreate(PrDomDocument* document) {
    return domRangeCreate(document);
}

PrStatus prDomRangeCreateStatic(PrDomNode* startContainer, uint32_t startOffset,
                                PrDomNode* endContainer, uint32_t endOffset,
                                PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    auto invalid = [](const PrDomNode* node) {
        return node->type == PrDomNodeType_DocumentType || node->type == PrDomNodeType_Attribute;
    };
    if (invalid(startContainer) || invalid(endContainer))
        return PrStatus_WrongType;
    *out = domStaticRangeCreate(DomBoundary{startContainer, startOffset}, DomBoundary{endContainer, endOffset});
    return PrStatus_Ok;
}

void prDomRangeRetain(PrDomRange* self) {
    domRangeRetain(self);
}

void prDomRangeRelease(PrDomRange* PR_NULLABLE self) {
    domRangeRelease(self);
}

bool prDomRangeIsLive(const PrDomRange* self) {
    return self->document != nullptr;
}

PrDomNode* prDomRangeStartContainer(PrDomRange* self) {
    return self->start.node;
}

uint32_t prDomRangeStartOffset(const PrDomRange* self) {
    return self->start.offset;
}

PrDomNode* prDomRangeEndContainer(PrDomRange* self) {
    return self->end.node;
}

uint32_t prDomRangeEndOffset(const PrDomRange* self) {
    return self->end.offset;
}

bool prDomRangeCollapsed(const PrDomRange* self) {
    return domRangeCollapsed(self);
}

PrDomNode* prDomRangeCommonAncestorContainer(PrDomRange* self) {
    PrDomNode* container = self->start.node;
    while (!domIsInclusiveAncestor(container, self->end.node))
        container = domNodeParent(container);
    return container;
}

PrStatus prDomRangeSetStart(PrDomRange* self, PrDomNode* node, uint32_t offset) {
    return changed(self, domRangeSetStart(self, node, offset));
}

PrStatus prDomRangeSetEnd(PrDomRange* self, PrDomNode* node, uint32_t offset) {
    return changed(self, domRangeSetEnd(self, node, offset));
}

PrStatus prDomRangeSetBeside(PrDomRange* self, PrDomRangeSide side, PrDomNode* node) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_WrongType;
    bool after = side == PrDomRangeSide_StartAfter || side == PrDomRangeSide_EndAfter;
    uint32_t offset = domNodeIndex(node) + (after ? 1 : 0);
    bool start = side == PrDomRangeSide_StartBefore || side == PrDomRangeSide_StartAfter;
    return changed(self, start ? domRangeSetStart(self, parent, offset)
                                : domRangeSetEnd(self, parent, offset));
}

void prDomRangeCollapse(PrDomRange* self, bool toStart) {
    domRangeCollapse(self, toStart);
    changed(self, PrStatus_Ok);
}

PrStatus prDomRangeSelectNode(PrDomRange* self, PrDomNode* node) {
    return changed(self, domRangeSelect(self, node));
}

PrStatus prDomRangeSelectNodeContents(PrDomRange* self, PrDomNode* node) {
    return changed(self, domRangeSelectContents(self, node));
}

PrStatus prDomRangeCompareBoundaryPoints(const PrDomRange* self, PrDomRangeHow how,
                                         const PrDomRange* source, int8_t* out) {
    if (domRangeRoot(self) != domRangeRoot(source))
        return PrStatus_Foreign;
    bool start = how == PrDomRangeHow_StartToStart || how == PrDomRangeHow_EndToStart;
    bool otherStart = how == PrDomRangeHow_StartToStart || how == PrDomRangeHow_StartToEnd;
    DomBoundary point = start ? self->start : self->end;
    DomBoundary otherPoint = otherStart ? source->start : source->end;
    *out = int8_t(domBoundaryCompare(point, otherPoint));
    return PrStatus_Ok;
}

void prDomRangeDeleteContents(PrDomRange* self) {
    domRangeDelete(self);
    changed(self, PrStatus_Ok);
}

PrStatus prDomRangeExtractContents(PrDomRange* self,
                                   PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    return rangeContents(self, true, out);
}

PrStatus prDomRangeCloneContents(PrDomRange* self,
                                 PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    return rangeContents(self, false, out);
}

PrStatus prDomRangeInsertNode(PrDomRange* self, PrDomNode* node) {
    return changed(self, domRangeInsert(self, node));
}

PrStatus prDomRangeSurroundContents(PrDomRange* self, PrDomNode* node) {
    return changed(self, domRangeSurround(self, node));
}

PrDomRange* prDomRangeCloneRange(PrDomRange* self) {
    PrDomRange* copy = domRangeCreate(self->start.node->document);
    domRangeSetStart(copy, self->start.node, self->start.offset);
    domRangeSetEnd(copy, self->end.node, self->end.offset);
    return copy;
}

PrStatus prDomRangeIsPointInRange(const PrDomRange* self, PrDomNode* node, uint32_t offset,
                                  bool* out) {
    *out = false;
    if (domNodeRoot(node) != domRangeRoot(self))
        return PrStatus_Ok;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary boundary = {node, offset};
    *out = domBoundaryCompare(boundary, self->start) >= 0
           && domBoundaryCompare(boundary, self->end) <= 0;
    return PrStatus_Ok;
}

PrStatus prDomRangeComparePoint(const PrDomRange* self, PrDomNode* node, uint32_t offset,
                                int8_t* out) {
    if (domNodeRoot(node) != domRangeRoot(self))
        return PrStatus_Foreign;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary boundary = {node, offset};
    *out = domBoundaryCompare(boundary, self->start) < 0 ? -1
           : domBoundaryCompare(boundary, self->end) > 0 ? 1
                                                          : 0;
    return PrStatus_Ok;
}

bool prDomRangeIntersectsNode(const PrDomRange* self, PrDomNode* node) {
    if (domNodeRoot(node) != domRangeRoot(self))
        return false;
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return true;
    uint32_t offset = domNodeIndex(node);
    return domBoundaryCompare(DomBoundary{parent, offset}, self->end) < 0
        && domBoundaryCompare(DomBoundary{parent, offset + 1}, self->start) > 0;
}

PrString prDomRangeToString(const PrDomRange* self) {
    return domRangeString(self);
}

PrDomSelection* prDomDocumentSelection(PrDomDocument* self) {
    return domDocumentSelection(self);
}

PrDomDocument* prDomSelectionDocument(PrDomSelection* self) {
    return self->document;
}

PrDomRange* PR_NULLABLE prDomSelectionRange(PrDomSelection* self) {
    return domSelectionRange(self);
}

PrDomNode* PR_NULLABLE prDomSelectionAnchorNode(PrDomSelection* self) {
    DomBoundary point;
    return domSelectionAnchor(self, &point)
               ? point.node
               : nullptr;
}

uint32_t prDomSelectionAnchorOffset(const PrDomSelection* self) {
    DomBoundary point;
    return domSelectionAnchor(self, &point) ? point.offset : 0;
}

PrDomNode* PR_NULLABLE prDomSelectionFocusNode(PrDomSelection* self) {
    DomBoundary point;
    return domSelectionFocus(self, &point) ? point.node
                                                                   : nullptr;
}

uint32_t prDomSelectionFocusOffset(const PrDomSelection* self) {
    DomBoundary point;
    return domSelectionFocus(self, &point) ? point.offset : 0;
}

bool prDomSelectionIsCollapsed(const PrDomSelection* self) {
    DomBoundary anchor;
    DomBoundary focus;
    if (!domSelectionAnchor(self, &anchor))
        return true;
    domSelectionFocus(self, &focus);
    return anchor.node == focus.node && anchor.offset == focus.offset;
}

uint32_t prDomSelectionRangeCount(const PrDomSelection* self) {
    DomBoundary anchor;
    return domSelectionAnchor(self, &anchor) ? 1 : 0;
}

PrDomSelectionType prDomSelectionType(const PrDomSelection* self) {
    if (prDomSelectionRangeCount(self) == 0)
        return PrDomSelectionType_None;
    return prDomSelectionIsCollapsed(self) ? PrDomSelectionType_Caret : PrDomSelectionType_Range;
}

PrDomSelectionDirection prDomSelectionDirection(const PrDomSelection* self) {
    return domSelectionRange(self) != nullptr ? self->direction
                                                   : PrDomSelectionDirection_None;
}

PrStatus prDomSelectionGetRangeAt(PrDomSelection* self, uint32_t index,
                                  PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (index != 0 || prDomSelectionRangeCount(self) == 0)
        return PrStatus_OutOfRange;
    *out = domSelectionRange(self);
    return PrStatus_Ok;
}

void prDomSelectionAddRange(PrDomSelection* self, PrDomRange* range) {
    if (range->document == nullptr || domRangeRoot(range) != self->document
        || domSelectionRange(self) != nullptr)
        return;
    domSelectionSetRange(self, range, PrDomSelectionDirection_None);
}

PrStatus prDomSelectionRemoveRange(PrDomSelection* self, const PrDomRange* range) {
    if (domSelectionRange(self) == nullptr
        || domSelectionRange(self) != range)
        return PrStatus_NotFound;
    domSelectionSetRange(self, nullptr, PrDomSelectionDirection_None);
    return PrStatus_Ok;
}

void prDomSelectionRemoveAllRanges(PrDomSelection* self) {
    domSelectionSetRange(self, nullptr, PrDomSelectionDirection_None);
}

PrStatus prDomSelectionCollapse(PrDomSelection* self, PrDomNode* PR_NULLABLE node, uint32_t offset,
                                PrDomRange* range) {
    if (node == nullptr) {
        prDomSelectionRemoveAllRanges(self);
        return PrStatus_Ok;
    }
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok || !inSelectionDocument(self, node))
        return status;
    selectRange(self, range, DomBoundary{node, offset}, DomBoundary{node, offset},
                PrDomSelectionDirection_None);
    return PrStatus_Ok;
}

PrStatus prDomSelectionCollapseToStart(PrDomSelection* self, PrDomRange* range) {
    return collapseTo(self, range, true);
}

PrStatus prDomSelectionCollapseToEnd(PrDomSelection* self, PrDomRange* range) {
    return collapseTo(self, range, false);
}

PrStatus prDomSelectionExtend(PrDomSelection* self, PrDomNode* node, uint32_t offset,
                              PrDomRange* range) {
    if (!inSelectionDocument(self, node))
        return PrStatus_Ok;
    if (domSelectionRange(self) == nullptr)
        return PrStatus_InvalidState;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary anchor = self->direction == PrDomSelectionDirection_Backwards
                             ? domSelectionRange(self)->end
                             : domSelectionRange(self)->start;
    DomBoundary focus = {node, offset};
    if (domNodeRoot(node) != domRangeRoot(domSelectionRange(self))) {
        selectRange(self, range, focus, focus, PrDomSelectionDirection_Forwards);
        return PrStatus_Ok;
    }
    bool backwards = domBoundaryCompare(focus, anchor) < 0;
    selectRange(self, range, backwards ? focus : anchor, backwards ? anchor : focus,
                backwards ? PrDomSelectionDirection_Backwards : PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

PrStatus prDomSelectionSetBaseAndExtent(PrDomSelection* self, PrDomNode* anchorNode,
                                        uint32_t anchorOffset, PrDomNode* focusNode,
                                        uint32_t focusOffset, PrDomRange* range) {
    if (anchorOffset > domNodeLength(anchorNode) || focusOffset > domNodeLength(focusNode))
        return PrStatus_OutOfRange;
    if (!inSelectionDocument(self, anchorNode) || !inSelectionDocument(self, focusNode))
        return PrStatus_Ok;
    DomBoundary anchor = {anchorNode, anchorOffset};
    DomBoundary focus = {focusNode, focusOffset};
    bool backwards = domNodeRoot(anchorNode) == domNodeRoot(focusNode)
                     && domBoundaryCompare(focus, anchor) < 0;
    selectRange(self, range, backwards ? focus : anchor, backwards ? anchor : focus,
                backwards ? PrDomSelectionDirection_Backwards : PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

PrStatus prDomSelectionSelectAllChildren(PrDomSelection* self, PrDomNode* node, PrDomRange* range) {
    if (node->type == PrDomNodeType_DocumentType)
        return PrStatus_WrongType;
    if (domNodeRoot(node) != self->document)
        return PrStatus_Ok;
    uint32_t count = 0;
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        ++count;
    selectRange(self, range, DomBoundary{node, 0}, DomBoundary{node, count},
                PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

void prDomSelectionDeleteFromDocument(PrDomSelection* self) {
    if (domSelectionRange(self) != nullptr)
        prDomRangeDeleteContents(domSelectionRange(self));
}

bool prDomSelectionContainsNode(const PrDomSelection* self, PrDomNode* node,
                                bool allowPartialContainment) {
    const PrDomRange* range = domSelectionRange(self);
    if (range == nullptr || domNodeRoot(node) != self->document
        || domRangeRoot(range) != self->document)
        return false;
    DomBoundary first = {node, 0};
    DomBoundary last = {node, domNodeLength(node)};
    if (allowPartialContainment)
        return domBoundaryCompare(range->start, last) <= 0
               && domBoundaryCompare(range->end, first) >= 0;
    return domBoundaryCompare(range->start, first) <= 0
           && domBoundaryCompare(range->end, last) >= 0;
}

PrString prDomSelectionToString(const PrDomSelection* self) {
    return domSelectionRange(self) != nullptr ? domRangeString(domSelectionRange(self))
                                                   : prStringEmpty();
}

uint32_t prDomSelectionComposedRanges(PrDomSelection* self,
                                      PrDomShadowRoot* const PR_NONNULL* PR_NULLABLE roots,
                                      uint32_t count, PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (domSelectionRange(self) == nullptr)
        return 0;
    // A boundary point moves out of each shadow tree that is neither one of the roots nor an ancestor of one.
    auto listed = [&](const PrDomNode* root) {
        for (uint32_t i = 0; i < count; ++i) {
            const PrDomNode* shadow = roots[i];
            for (const PrDomNode* at = shadow; at != nullptr;
                 at = domNodeShadowRoot(domNodeRoot(at)) != nullptr
                          ? domNodeShadowRoot(domNodeRoot(at))->host
                          : nullptr) {
                if (domNodeRoot(at) == root || at == root)
                    return true;
            }
        }
        return false;
    };
    auto rescope = [&](DomBoundary point, uint32_t after) {
        while (point.node != nullptr) {
            PrDomShadowRoot* shadow = domNodeShadowRoot(domNodeRoot(point.node));
            if (shadow == nullptr || listed(shadow))
                break;
            point = DomBoundary{domNodeParent(shadow->host), domNodeIndex(shadow->host) + after};
        }
        return point;
    };
    DomBoundary start = rescope(domSelectionRange(self)->start, 0);
    DomBoundary end = rescope(domSelectionRange(self)->end, 1);
    if (start.node == nullptr || end.node == nullptr)
        return 0;
    *out = domStaticRangeCreate(start, end);
    return 1;
}

bool prDomIsValidCustomElementName(PrStringView name) {
    return domIsValidCustomElementName(name);
}

void prDomDocumentSetCustomElementRegistry(PrDomDocument* self,
                                           const PrDomCustomElementRegistry* PR_NULLABLE registry) {
    self->customElements = registry;
}

PrDomCustomElementState prDomElementCustomElementState(const PrDomElement* self) {
    return self->customState;
}

void* PR_NULLABLE prDomElementCustomElementDefinition(const PrDomElement* self) {
    return self->state != nullptr && self->state->custom != nullptr
               ? self->state->custom->definition
               : nullptr;
}

PrDomElement* prDomElementCreateCustom(PrDomDocument* self, PrStringView localName,
                                       void* definition) {
    PrDomElement* element =
        domElementCreate(self, PrDomNamespace_Html, domTagFromText(localName), localName);
    element->customState = PrDomCustomElementState_Custom;
    DomElementState* state = domElementState(element);
    state->custom = allocZeroed<DomCustomElement>();
    state->custom->is = prStringNull();
    state->custom->definition = definition;
    state->custom->registry = self->customElements;
    if (self->customElements != nullptr)
        domCustomConstructed(self->customElements->agent, element);
    return made(element);
}

void prDomDocumentUpgradeCandidates(PrDomDocument* self, PrStringView localName, void* definition) {
    domCustomUpgradeCandidates(self, localName, definition);
}

void prDomNodeUpgrade(PrDomNode* self) {
    domCustomUpgradeTree(self);
}

PrDomShadowRoot* PR_NULLABLE prDomElementHostedShadowRoot(PrDomElement* self) {
    return domShadowRootOfHost(self);
}

void prDomElementSetCustomState(PrDomElement* self, PrStringView state, bool present) {
    if (domCustomHasState(self, state) == present)
        return;
    Array<PrString>& states = domCustomElementFromElement(self)->states;
    if (present) {
        states.push(prStringCopy(state));
    } else {
        for (uint32_t i = 0; i < states.count; ++i) {
            if (prStringView(&states.data[i]) == state) {
                prStringReset(&states.data[i]);
                states.erase(i);
                break;
            }
        }
    }
    domNotifyStatesChanged(self);
}

void prDomElementClearCustomStates(PrDomElement* self) {
    PrDomElement* element = self;
    if (element->state == nullptr || element->state->custom == nullptr || element->state->custom->states.count == 0)
        return;
    stringsClear(&element->state->custom->states);
    domNotifyStatesChanged(element);
}

PrDomAgentOptions prDomAgentOptionsDefault(void) {
    PrDomAgentOptions options{};
    options.restyleThreads = 0;
    options.restylePool = nullptr;
    return options;
}

PrDomAgent* prDomAgentCreate(const PrDomAgentOptions* PR_NULLABLE options) {
    PrDomAgent* agent = allocZeroed<PrDomAgent>();
    domAgentInit(agent, options != nullptr ? *options : prDomAgentOptionsDefault());
    return agent;
}

void prDomAgentDestroy(PrDomAgent* PR_NULLABLE self) {
    domAgentDestroy(self);
}

double prDomAgentNextTaskDelay(PrDomAgent* self) {
    return domAgentNextTaskDelay(self);
}

bool prDomAgentIsIdle(PrDomAgent* self) {
    return domAgentIsIdle(self);
}

bool prDomAgentRunTask(PrDomAgent* self) {
    return domAgentRunTask(self);
}

bool prDomAgentSpinTask(PrDomAgent* self) {
    return domAgentSpinTask(self);
}

void prDomAgentPost(PrDomAgent* self, PrDomAgentTaskFn fn, void* PR_NULLABLE context) {
    domAgentPost(self, fn, context);
}

void prDomAgentSetWake(PrDomAgent* self, PrDomAgentWakeFn PR_NULLABLE wake, void* PR_NULLABLE context) {
    domAgentSetWake(self, wake, context);
}

void prDomAgentSetLayoutDone(PrDomAgent* self, PrDomAgentLayoutDoneFn PR_NULLABLE done, void* PR_NULLABLE context) {
    self->layoutDone = done;
    self->layoutDoneContext = context;
    if (self->worker != nullptr)
        prLayoutWorkerSetDone(self->worker, done, context);
}

void prDomAgentReactionsPush(PrDomAgent* self) {
    domCustomReactionsPush(self);
}

void prDomAgentReactionsPop(PrDomAgent* self) {
    domCustomReactionsPop(self);
}

