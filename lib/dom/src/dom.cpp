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
PrDomAttr* PR_NULLABLE attributeNs(const PrDomElement* element, PrStringView namespaceUrl, PrStringView localName) {
    for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<PrDomAttr*>(attribute->next)) {
        if (prRawStringView(&attribute->name) == localName &&
                domNamespaceUrl(element->document, attribute->space) == namespaceUrl)
            return attribute;
    }
    return nullptr;
}

// dom/#concept-attribute-qualified-name
PrRawString attributeQualifiedName(const PrDomAttr* attribute) {
    PrStringView prefix = domAttributePrefix(attribute);
    if (prefix.data == nullptr)
        return prRawStringClone(&attribute->name);
    PrRawString name = prRawStringCopy(prefix);
    prRawStringAppend(&name, ":");
    prRawStringAppend(&name, prRawStringView(&attribute->name));
    return name;
}

// Returns `node` as an attribute, if it is one.
PrDomAttr* PR_NULLABLE attrOf(PrDomNode* PR_NULLABLE node) {
    return node != nullptr && node->type == PrDomNodeType_Attribute ? static_cast<PrDomAttr*>(node) : nullptr;
}

// dom/#validate-and-extract, of an element's name or, if `element` is false, an attribute's name. Returns InvalidData
// if the local name is not valid, and NotAllowed if the namespace and the prefix are not compatible. Returns
// NotSupported if the document can hold no other namespace.
PrStatus validateAndExtract(PrDomDocument* document, PrStringView namespaceUrl,
        PrStringView qualifiedName, bool element, DomNamespaceId* space, PrStringView* prefix, PrStringView* local) {
    size_t colon = prStringFindByte(qualifiedName, ':');
    *prefix = colon < qualifiedName.size ? PrStringView{qualifiedName.data, colon} : PrStringView{};
    *local = qualifiedName;
    if (colon < qualifiedName.size)
        *local = prStringSlice(qualifiedName, colon + 1, qualifiedName.size - colon - 1);
    if (prefix->data != nullptr) {
        // dom/#valid-namespace-prefix
        bool valid = prefix->size != 0;
        for (size_t i = 0; valid && i < prefix->size; ++i)
            valid = !prAsciiIsSpace(prefix->data[i]) && prefix->data[i] != '\0' &&
                prefix->data[i] != '/' && prefix->data[i] != '>';
        if (!valid)
            return PrStatus_InvalidData;
    }
    if (element ? !domIsValidElementLocalName(*local) : !domIsValidAttributeLocalName(*local))
        return PrStatus_InvalidData;
    PrStringView xml = "http://www.w3.org/XML/1998/namespace";
    PrStringView xmlns = "http://www.w3.org/2000/xmlns/";
    bool none = namespaceUrl.size == 0;
    if ((prefix->data != nullptr && none) || (*prefix == "xml" && namespaceUrl != xml) ||
            ((qualifiedName == "xmlns" || *prefix == "xmlns") && namespaceUrl != xmlns) ||
            (namespaceUrl == xmlns && qualifiedName != "xmlns" && *prefix != "xmlns"))
        return PrStatus_NotAllowed;
    return domNamespaceFromUrl(document, namespaceUrl, space) ? PrStatus_Ok : PrStatus_NotSupported;
}

// Returns `name`, in ASCII lowercase if `lower` is set. The lowered name is written to `buffer` if lowering changes it.
PrStringView lowered(PrStringView name, bool lower, PrTextBuffer* buffer) {
    bool upper = false;
    for (size_t i = 0; lower && !upper && i < name.size; ++i)
        upper = prAsciiIsUpper(name.data[i]);
    if (!upper)
        return name;
    prTextBufferAppend(buffer, name);
    prTextBufferAsciiLower(buffer);
    return prTextBufferView(buffer);
}

// Returns the attribute that a Remove method took off the element, with a reference that the caller takes. Returns
// NotFound if there is none.
PrStatus removeNamed(PrDomElement* element, PrDomAttr* PR_NULLABLE attribute, PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
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
PrRawString stripAndCollapse(PrStringView text) {
    PrTextBuffer out = {};
    bool space = false;
    for (size_t i = 0; i < text.size; ++i) {
        if (prAsciiIsSpace(text.data[i])) {
            space = true;
            continue;
        }
        if (space && out.size != 0)
            prTextBufferAppendChar(&out, ' ');
        space = false;
        prTextBufferAppendChar(&out, text.data[i]);
    }
    return prTextBufferTake(&out);
}

// Returns the first element of `space` whose tag is `tag`, among the descendants of `root` or among its children alone.
const PrDomElement* PR_NULLABLE firstElement(const PrDomNode* root, DomNamespaceId space, PrDomTag tag, bool children) {
    for (const PrDomNode* node = root->first; node != nullptr; node = children ? node->next : domNodeNext(root, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && element->space == space && element->tag == tag)
            return element;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE firstElement(PrDomNode* root, DomNamespaceId space, PrDomTag tag, bool children) {
    return const_cast<PrDomElement*>(firstElement(static_cast<const PrDomNode*>(root), space, tag, children));
}

bool isSvgRoot(const PrDomElement* PR_NULLABLE element) {
    return element != nullptr && element->space == PrDomNamespace_Svg && element->tag == PrDomTag_Svg;
}

// dom/#insert-adjacent. A null `node` inserts nothing, and a position outside an element with no parent inserts
// nothing.
PrStatus insertAdjacent(PrDomElement* element, PrDomInsertPosition where, PrDomNode* node, bool* inserted) {
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
PrStatus rangeContents(PrDomRange* range, bool extract, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
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
PrStatus collapseTo(PrDomSelection* selection, PrDomRange* range, bool start) {
    const PrDomRange* current = domSelectionRange(selection);
    if (current == nullptr)
        return PrStatus_InvalidState;
    DomBoundary point = start ? current->start : current->end;
    selectRange(selection, range, point, point, PrDomSelectionDirection_None);
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
    *event->mouse = DomMouseEventData{
        init.modifiers, init.button,  init.buttons, init.screenX,
        init.screenY,   init.clientX, init.clientY,
    };
    domEventTargetAssign(&event->related, init.relatedTarget);
    domEventTargetAssign(&event->relatedTarget, event->related);
}

// Returns the data of a KeyboardEvent, which an event that createEvent made gains as it is first initialized.
DomKeyboardEventData* keyboardOf(PrDomEvent* event) {
    if (event->keyboard == nullptr) {
        event->keyboard = allocZeroed<DomKeyboardEventData>();
        event->keyboard->key = prRawStringEmpty();
        event->keyboard->code = prRawStringEmpty();
    }
    return event->keyboard;
}

// Returns the element whose scope holds the namespace declarations in effect at `node`, or null if a node of its type
// has none (dom/#locate-a-namespace).
PrDomElement* PR_NULLABLE scopeElement(PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_Element:
            return static_cast<PrDomElement*>(node);
        case PrDomNodeType_Document:
            return domDocumentElement(static_cast<PrDomDocument*>(node));
        case PrDomNodeType_DocumentType:
        case PrDomNodeType_DocumentFragment:
            return nullptr;
        case PrDomNodeType_Attribute:
            return static_cast<PrDomElement*>(node->parent);
        default:
            return prDomNodeParentElement(node);
    }
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
    if (kind != PrDomEventKind_Event && kind != PrDomEventKind_Custom && kind != PrDomEventKind_Ui &&
            kind != PrDomEventKind_Focus && kind != PrDomEventKind_Mouse && kind != PrDomEventKind_Keyboard &&
            kind != PrDomEventKind_Composition)
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
        if (prAsciiEqualsFold(interface, entry.name)) {
            *out = entry.kind;
            return true;
        }
    }
    return false;
}

void prDomEventRetain(PrDomEvent* event) {
    domEventRetain(event);
}

void prDomEventRelease(PrDomEvent* PR_NULLABLE event) {
    domEventRelease(event);
}

PrStringView prDomEventType(const PrDomEvent* event) {
    return prRawStringView(&event->type);
}

PrDomEventTarget* PR_NULLABLE prDomEventTarget(PrDomEvent* event) {
    return event->target;
}

PrDomEventTarget* PR_NULLABLE prDomEventCurrentTarget(PrDomEvent* event) {
    return event->currentTarget;
}

uint32_t prDomEventComposedPath(PrDomEvent* event, PrDomEventTarget* PR_NONNULL* PR_NULLABLE items, uint32_t capacity) {
    for (uint32_t i = 0; items != nullptr && i < event->path.count && i < capacity; ++i)
        items[i] = event->path.data[i];
    return event->path.count;
}

PrDomEventPhase prDomEventPhase(const PrDomEvent* event) {
    return event->phase;
}

void prDomEventStopPropagation(PrDomEvent* event) {
    event->stopPropagation = true;
}

bool prDomEventCancelBubble(const PrDomEvent* event) {
    return event->stopPropagation;
}

void prDomEventSetCancelBubble(PrDomEvent* event, bool value) {
    if (value)
        event->stopPropagation = true;
}

void prDomEventStopImmediatePropagation(PrDomEvent* event) {
    event->stopPropagation = true;
    event->stopImmediatePropagation = true;
}

bool prDomEventBubbles(const PrDomEvent* event) {
    return event->bubbles;
}

bool prDomEventCancelable(const PrDomEvent* event) {
    return event->cancelable;
}

bool prDomEventReturnValue(const PrDomEvent* event) {
    return !event->canceled;
}

void prDomEventSetReturnValue(PrDomEvent* event, bool value) {
    if (!value)
        domEventCancel(event);
}

void prDomEventPreventDefault(PrDomEvent* event) {
    domEventCancel(event);
}

bool prDomEventDefaultPrevented(const PrDomEvent* event) {
    return event->canceled;
}

bool prDomEventComposed(const PrDomEvent* event) {
    return event->composed;
}

bool prDomEventIsTrusted(const PrDomEvent* event) {
    return event->trusted;
}

double prDomEventTimeStamp(const PrDomEvent* event) {
    return event->timeStamp;
}

void prDomEventInitEvent(PrDomEvent* event, PrStringView type, bool bubbles, bool cancelable) {
    domEventInitialize(event, type, bubbles, cancelable);
}

PrDomEventKind prDomEventKind(const PrDomEvent* event) {
    return event->kind;
}

// ---------------------------------------------------------------------------------------------------------------------
// CustomEvent

PrDomCustomEvent* PR_NULLABLE prDomCustomEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Custom ? static_cast<PrDomCustomEvent*>(object) : nullptr;
}

PrDomCustomEvent* prDomCustomEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init) {
    PrDomEventInit values = init != nullptr ? *init : prDomEventInitDefault();
    return domEventCreate<PrDomCustomEvent>(type, values, PrDomEventKind_Custom);
}

bool prDomCustomEventInitCustomEvent(PrDomCustomEvent* event, PrStringView type, bool bubbles, bool cancelable) {
    if (event->dispatching)
        return false;
    domEventInitialize(event, type, bubbles, cancelable);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// UIEvent and FocusEvent

PrDomUiEvent* PR_NULLABLE prDomUiEventQuery(PrDomEvent* PR_NULLABLE object) {
    bool ui = object != nullptr && (object->kind == PrDomEventKind_Ui || object->kind == PrDomEventKind_Focus ||
        object->kind == PrDomEventKind_Mouse || object->kind == PrDomEventKind_Wheel ||
        object->kind == PrDomEventKind_Input ||
        object->kind == PrDomEventKind_Keyboard || object->kind == PrDomEventKind_Composition);
    return ui ? static_cast<PrDomUiEvent*>(object) : nullptr;
}

PrDomUiEvent* prDomUiEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init) {
    PrDomUiEventInit values = init != nullptr ? *init : prDomUiEventInitDefault();
    PrDomUiEvent* event = domEventCreate<PrDomUiEvent>(type, values.event, PrDomEventKind_Ui);
    setUiMembers(event, values);
    return event;
}

PrDomEventTarget* PR_NULLABLE prDomUiEventView(PrDomUiEvent* event) {
    return event->view;
}

int32_t prDomUiEventDetail(const PrDomUiEvent* event) {
    return event->detail;
}

uint32_t prDomUiEventWhich(const PrDomUiEvent* event) {
    switch (event->kind) {
        case PrDomEventKind_Mouse:
        case PrDomEventKind_Wheel:
            return event->mouse != nullptr ? uint32_t(event->mouse->button + 1) : 1;
        case PrDomEventKind_Keyboard:
            if (event->keyboard == nullptr)
                return 0;
            return event->keyboard->keyCode != 0 ? event->keyboard->keyCode : event->keyboard->charCode;
        default:
            return event->which;
    }
}

void prDomUiEventInitUiEvent(PrDomUiEvent* event, PrStringView type, const PrDomUiEventInit* init) {
    if (event->dispatching)
        return;
    domEventInitialize(event, type, init->event.bubbles, init->event.cancelable);
    event->view = init->view;
    event->detail = init->detail;
}

PrDomFocusEvent* PR_NULLABLE prDomFocusEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Focus ? static_cast<PrDomFocusEvent*>(object) : nullptr;
}

PrDomFocusEvent* prDomFocusEventCreate(PrStringView type, const PrDomFocusEventInit* PR_NULLABLE init) {
    PrDomFocusEventInit values = init != nullptr ? *init : prDomFocusEventInitDefault();
    PrDomFocusEvent* event = domEventCreate<PrDomFocusEvent>(type, values.ui.event, PrDomEventKind_Focus);
    setUiMembers(event, values.ui);
    domEventTargetAssign(&event->related, values.relatedTarget);
    domEventTargetAssign(&event->relatedTarget, event->related);
    return event;
}

PrDomEventTarget* PR_NULLABLE prDomFocusEventRelatedTarget(PrDomFocusEvent* event) {
    return event->relatedTarget;
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

PrDomMouseEventInit prDomMouseEventMembers(PrDomMouseEvent* event) {
    PrDomMouseEventInit members = prDomMouseEventInitDefault();
    members.ui.event.bubbles = event->bubbles;
    members.ui.event.cancelable = event->cancelable;
    members.ui.event.composed = event->composed;
    members.ui.view = event->view;
    members.ui.detail = event->detail;
    members.ui.which = event->which;
    members.relatedTarget = event->relatedTarget;
    if (const DomMouseEventData* mouse = event->mouse) {
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

void prDomMouseEventInitMouseEvent(PrDomMouseEvent* event, PrStringView type, const PrDomMouseEventInit* init) {
    if (event->dispatching)
        return;
    domEventInitialize(event, type, init->ui.event.bubbles, init->ui.event.cancelable);
    PrDomMouseEventInit members = *init;
    members.buttons = event->mouse != nullptr ? event->mouse->buttons : 0;
    setMouseMembers(event, members);
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

PrDomWheelEventInit prDomWheelEventMembers(PrDomWheelEvent* event) {
    PrDomWheelEventInit members = prDomWheelEventInitDefault();
    members.mouse = prDomMouseEventMembers(event);
    members.deltaX = event->wheel->deltaX;
    members.deltaY = event->wheel->deltaY;
    members.deltaZ = event->wheel->deltaZ;
    members.deltaMode = event->wheel->deltaMode;
    return members;
}

// ---------------------------------------------------------------------------------------------------------------------
// KeyboardEvent

PrDomKeyboardEvent* PR_NULLABLE prDomKeyboardEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Keyboard ? static_cast<PrDomKeyboardEvent*>(object) : nullptr;
}

PrDomKeyboardEvent* prDomKeyboardEventCreate(PrStringView type, const PrDomKeyboardEventInit* PR_NULLABLE init) {
    PrDomKeyboardEventInit values = init != nullptr ? *init : prDomKeyboardEventInitDefault();
    PrDomKeyboardEvent* event = domEventCreate<PrDomKeyboardEvent>(type, values.ui.event, PrDomEventKind_Keyboard);
    setUiMembers(event, values.ui);
    DomKeyboardEventData* keyboard = keyboardOf(event);
    prRawStringReplace(&keyboard->key, values.key);
    prRawStringReplace(&keyboard->code, values.code);
    keyboard->location = values.location;
    keyboard->charCode = values.charCode;
    keyboard->keyCode = values.keyCode;
    keyboard->modifiers = values.modifiers;
    keyboard->repeat = values.repeat;
    keyboard->isComposing = values.isComposing;
    return event;
}

PrDomKeyboardEventInit prDomKeyboardEventMembers(PrDomKeyboardEvent* event) {
    PrDomKeyboardEventInit members = prDomKeyboardEventInitDefault();
    members.ui.event.bubbles = event->bubbles;
    members.ui.event.cancelable = event->cancelable;
    members.ui.event.composed = event->composed;
    members.ui.view = event->view;
    members.ui.detail = event->detail;
    members.ui.which = event->which;
    if (const DomKeyboardEventData* keyboard = event->keyboard) {
        members.modifiers = keyboard->modifiers;
        members.key = prRawStringView(&keyboard->key);
        members.code = prRawStringView(&keyboard->code);
        members.location = keyboard->location;
        members.repeat = keyboard->repeat;
        members.isComposing = keyboard->isComposing;
        members.charCode = keyboard->charCode;
        members.keyCode = keyboard->keyCode;
    }
    return members;
}

void prDomKeyboardEventInitKeyboardEvent(PrDomKeyboardEvent* event, PrStringView type, const PrDomKeyboardEventInit* init) {
    if (event->dispatching)
        return;
    domEventInitialize(event, type, init->ui.event.bubbles, init->ui.event.cancelable);
    event->view = init->ui.view;
    event->detail = 0;
    DomKeyboardEventData* keyboard = keyboardOf(event);
    prRawStringReplace(&keyboard->key, init->key);
    keyboard->location = init->location;
    keyboard->modifiers = init->modifiers;
}

// ---------------------------------------------------------------------------------------------------------------------
// InputEvent

struct PrDomDataTransfer {
    uint32_t references;
    PrRawString text;
    bool hasText;
};

PrDomClipboardEvent* PR_NULLABLE prDomClipboardEventQuery(PrDomEvent* PR_NULLABLE object) {
    if (object == nullptr || object->kind != PrDomEventKind_Clipboard)
        return nullptr;
    return static_cast<PrDomClipboardEvent*>(object);
}

PrDomClipboardEvent* prDomClipboardEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init, PrStringView text) {
    PrDomEventInit values = init != nullptr ? *init : prDomEventInitDefault();
    PrDomClipboardEvent* event = domEventCreate<PrDomClipboardEvent>(type, values, PrDomEventKind_Clipboard);
    event->transfer = allocZeroed<PrDomDataTransfer>();
    event->transfer->references = 1;
    event->transfer->text = prRawStringCopy(text);
    event->transfer->hasText = text.size != 0;
    return event;
}

PrDomDataTransfer* prDomClipboardEventClipboardData(const PrDomClipboardEvent* event) {
    return event->transfer;
}

void prDomDataTransferRetain(PrDomDataTransfer* transfer) {
    ++transfer->references;
}

void prDomDataTransferRelease(PrDomDataTransfer* PR_NULLABLE transfer) {
    if (transfer == nullptr || --transfer->references != 0)
        return;
    prRawStringReset(&transfer->text);
    prMemoryFree(transfer);
}

PrStringView prDomDataTransferText(const PrDomDataTransfer* transfer) {
    return prRawStringView(&transfer->text);
}

bool prDomDataTransferHasText(const PrDomDataTransfer* transfer) {
    return transfer->hasText;
}

void prDomDataTransferSetText(PrDomDataTransfer* transfer, PrStringView text) {
    prRawStringReset(&transfer->text);
    transfer->text = prRawStringCopy(text);
    transfer->hasText = true;
}

void prDomDataTransferClear(PrDomDataTransfer* transfer) {
    prRawStringReset(&transfer->text);
    transfer->hasText = false;
}

PrDomInputEvent* PR_NULLABLE prDomInputEventQuery(PrDomEvent* PR_NULLABLE object) {
    if (object == nullptr || object->kind != PrDomEventKind_Input)
        return nullptr;
    return static_cast<PrDomInputEvent*>(object);
}

PrDomInputEvent* prDomInputEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init, PrStringView data,
        PrStringView inputType, bool isComposing) {
    PrDomUiEventInit values = init != nullptr ? *init : prDomUiEventInitDefault();
    PrDomInputEvent* event = domEventCreate<PrDomInputEvent>(type, values.event, PrDomEventKind_Input);
    setUiMembers(event, values);
    event->composition = prRawStringCopy(data);
    event->inputType = prRawStringCopy(inputType);
    event->isComposing = isComposing;
    return event;
}

PrStringView prDomInputEventData(const PrDomInputEvent* event) {
    return prRawStringView(&event->composition);
}

PrStringView prDomInputEventInputType(const PrDomInputEvent* event) {
    return prRawStringView(&event->inputType);
}

bool prDomInputEventIsComposing(const PrDomInputEvent* event) {
    return event->isComposing;
}

// ---------------------------------------------------------------------------------------------------------------------
// CompositionEvent

PrDomCompositionEvent* PR_NULLABLE prDomCompositionEventQuery(PrDomEvent* PR_NULLABLE object) {
    if (object == nullptr || object->kind != PrDomEventKind_Composition)
        return nullptr;
    return static_cast<PrDomCompositionEvent*>(object);
}

PrDomCompositionEvent* prDomCompositionEventCreate(PrStringView type, const PrDomUiEventInit* PR_NULLABLE init,
        PrStringView data) {
    PrDomUiEventInit values = init != nullptr ? *init : prDomUiEventInitDefault();
    PrDomCompositionEvent* event =
        domEventCreate<PrDomCompositionEvent>(type, values.event, PrDomEventKind_Composition);
    setUiMembers(event, values);
    event->composition = prRawStringCopy(data);
    return event;
}

PrStringView prDomCompositionEventData(const PrDomCompositionEvent* event) {
    return prRawStringView(&event->composition);
}

void prDomCompositionEventInitCompositionEvent(PrDomCompositionEvent* event, PrStringView type,
        const PrDomUiEventInit* init, PrStringView data) {
    if (event->dispatching)
        return;
    domEventInitialize(event, type, init->event.bubbles, init->event.cancelable);
    event->view = init->view;
    event->detail = 0;
    prRawStringReplace(&event->composition, data);
}

// ---------------------------------------------------------------------------------------------------------------------
// EventTarget

PrDomAddEventListenerOptions prDomAddEventListenerOptionsDefault(const PrDomEventTarget* target, PrStringView type) {
    return PrDomAddEventListenerOptions{.passive = domDefaultPassive(target, type)};
}

PrDomEventTarget* prDomEventTargetCreate(void) {
    DomStandaloneTarget* target = allocZeroed<DomStandaloneTarget>();
    target->references = 1;
    return target;
}

void prDomEventTargetRetain(PrDomEventTarget* target) {
    domEventTargetRetain(target);
}

void prDomEventTargetRelease(PrDomEventTarget* PR_NULLABLE target) {
    domEventTargetRelease(target);
}

void* PR_NULLABLE* PR_NONNULL prDomEventTargetBinding(PrDomEventTarget* target) {
    return &target->binding;
}

void prDomEventTargetAddEventListener(PrDomEventTarget* target, PrStringView type,
        const PrDomEventListener* PR_NULLABLE callback, const PrDomAddEventListenerOptions* PR_NULLABLE options) {
    if (callback != nullptr)
        domListenerAdd(target, type, callback, options);
}

void prDomEventTargetRemoveEventListener(PrDomEventTarget* target, PrStringView type,
        const PrDomEventListener* PR_NULLABLE callback, const PrDomEventListenerOptions* PR_NULLABLE options) {
    if (callback != nullptr)
        domListenerRemove(target, type, callback, options != nullptr && options->capture);
}

// dom/#dom-eventtarget-dispatchevent, which marks the event untrusted.
PrStatus prDomEventTargetDispatchEvent(PrDomEventTarget* target, PrDomEvent* event, bool* PR_NULLABLE out) {
    if (event->dispatching || !event->initialized)
        return PrStatus_InvalidState;
    event->trusted = false;
    bool proceed = domEventDispatch(target, event);
    if (out != nullptr)
        *out = proceed;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// EventHandler

PrDomEventHandlerCallback prDomEventTargetEventHandler(PrDomEventTarget* target, PrDomEventHandler handler) {
    PrDomEventTarget* handlerTarget = domEventHandlerTarget(target, handler);
    return handlerTarget != nullptr ? domEventHandler(handlerTarget, handler) : PrDomEventHandlerCallback{};
}

void prDomEventTargetSetEventHandler(PrDomEventTarget* eventTarget, PrDomEventHandler handler,
        const PrDomEventHandlerCallback* PR_NULLABLE callback) {
    if (PrDomEventTarget* target = domEventHandlerTarget(eventTarget, handler))
        domSetEventHandler(target, handler, callback);
}

// ---------------------------------------------------------------------------------------------------------------------
// Node

PrDomNode* PR_NULLABLE prDomNodeQuery(PrDomEventTarget* PR_NULLABLE object) {
    return domNodeFromTarget(object);
}

void prDomNodeRetain(PrDomNode* node) {
    domNodeRetain(node);
}

void prDomNodeRelease(PrDomNode* PR_NULLABLE node) {
    domNodeRelease(node);
}

PrDomNode* prDomNodeTreeRoot(PrDomNode* node) {
    return domTreeRoot(node);
}

PrDomNode* PR_NULLABLE prDomNodeTreeParent(PrDomNode* node) {
    return domNodeOwner(node);
}

PrDomNodeType prDomNodeType(const PrDomNode* node) {
    return node->type;
}

PrRawString prDomNodeBaseUri(const PrDomNode* node) {
    PrUrl* base = domDocumentBaseUrl(node->document);
    PrRawString result = prUrlSerialize(base, PrUrlSerialization_Whole);
    prUrlDestroy(base);
    return result;
}

bool prDomNodeIsConnected(const PrDomNode* node) {
    return domIsConnected(node);
}

PrDomDocument* PR_NULLABLE prDomNodeOwnerDocument(PrDomNode* node) {
    return node->type != PrDomNodeType_Document ? node->document : nullptr;
}

void prDomNodeVisitSubtree(PrDomNode* root, void (*visit)(void* PR_NULLABLE context, PrDomNode* node),
        void* PR_NULLABLE context) {
    struct Visit {
        void (*visit)(void* PR_NULLABLE context, PrDomNode* node);
        void* PR_NULLABLE context;
    } state = {visit, context};
    domVisitSubtree(root, [](void* data, PrDomNode* node) {
        Visit* given = static_cast<Visit*>(data);
        given->visit(given->context, node);
    }, &state);
}

PrDomNode* PR_NULLABLE prDomNodeParentNode(PrDomNode* node) {
    return domNodeParent(node);
}

PrDomElement* PR_NULLABLE prDomNodeParentElement(PrDomNode* node) {
    return domElementFromNode(domNodeParent(node));
}

bool prDomNodeHasChildNodes(const PrDomNode* node) {
    return node->first != nullptr;
}

PrDomNode* PR_NULLABLE prDomNodeFirstChild(PrDomNode* node) {
    return node->first;
}

PrDomNode* PR_NULLABLE prDomNodeLastChild(PrDomNode* node) {
    return node->last;
}

PrDomNode* PR_NULLABLE prDomNodePreviousSibling(PrDomNode* node) {
    return domNodeParent(node) != nullptr ? node->previous : nullptr;
}

PrDomNode* PR_NULLABLE prDomNodeNextSibling(PrDomNode* node) {
    return domNodeParent(node) != nullptr ? node->next : nullptr;
}

PrRawString prDomNodeTextContent(const PrDomNode* node) {
    return domNodeTextContent(node);
}

void prDomNodeSetTextContent(PrDomNode* node, PrStringView value) {
    domSetTextContent(node, value);
}

PrRawString prDomNodeChildTextContent(const PrDomNode* node) {
    return domChildTextContent(node);
}

PrStatus prDomNodeInsertBefore(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE child) {
    return domPreInsert(parent, node, child);
}

PrStatus prDomNodeAppendChild(PrDomNode* parent, PrDomNode* node) {
    return domPreInsert(parent, node, nullptr);
}

PrStatus prDomNodeReplaceChild(PrDomNode* parent, PrDomNode* node, PrDomNode* child) {
    return domReplace(parent, node, child);
}

PrStatus prDomNodeRemoveChild(PrDomNode* node, PrDomNode* child) {
    return domPreRemove(node, child);
}

PrRawString prDomNodeName(const PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_Element: return domElementTagName(static_cast<const PrDomElement*>(node));
        case PrDomNodeType_Attribute: return attributeQualifiedName(static_cast<const PrDomAttr*>(node));
        case PrDomNodeType_ProcessingInstruction:
            return prRawStringClone(&static_cast<const PrDomProcessingInstruction*>(node)->target);
        case PrDomNodeType_DocumentType:
            return prRawStringClone(&static_cast<const PrDomDocumentType*>(node)->name);
        default: {
            PrStringView name = prDomNodeTypeName(node->type);
            return name.data != nullptr ? prRawStringExternal(name) : prRawStringEmpty();
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

PrStringView prDomNodeValue(const PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_Attribute: return prRawStringView(&static_cast<const PrDomAttr*>(node)->value);
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection:
        case PrDomNodeType_ProcessingInstruction:
        case PrDomNodeType_Comment:
            return prRawStringView(&static_cast<const PrDomCharacterData*>(node)->data);
        default: return {};
    }
}

void prDomNodeSetValue(PrDomNode* node, PrStringView value) {
    // The node's value is its text content, if it has one.
    if (node->type != PrDomNodeType_Element && node->type != PrDomNodeType_Document &&
            node->type != PrDomNodeType_DocumentFragment && node->type != PrDomNodeType_DocumentType)
        domSetTextContent(node, value);
}

PrDomNode* prDomNodeGetRootNode(PrDomNode* node, bool composed) {
    return const_cast<PrDomNode*>(composed ? domShadowIncludingRoot(node) : domNodeRoot(node));
}

bool prDomNodeContains(const PrDomNode* node, const PrDomNode* PR_NULLABLE other) {
    for (const PrDomNode* ancestor = other; ancestor != nullptr; ancestor = domNodeParent(ancestor)) {
        if (ancestor == node)
            return true;
    }
    return false;
}

bool prDomNodeIsEqualNode(const PrDomNode* node, const PrDomNode* PR_NULLABLE other) {
    return other != nullptr && domNodeEquals(node, other);
}

PrDomNodePosition prDomNodeCompareDocumentPosition(const PrDomNode* node, const PrDomNode* other) {
    const PrDomNode* node1 = other;
    if (node1 == node)
        return 0;
    // dom/#dom-node-comparedocumentposition: an attribute stands for its element. Two attributes of one element are
    // ordered by the element's attribute list.
    const PrDomNode* node2 = node;
    const PrDomAttr* attr1 = node1->type == PrDomNodeType_Attribute ? static_cast<const PrDomAttr*>(node1) : nullptr;
    const PrDomAttr* attr2 = node2->type == PrDomNodeType_Attribute ? static_cast<const PrDomAttr*>(node2) : nullptr;
    if (attr1 != nullptr)
        node1 = attr1->parent;
    if (attr2 != nullptr) {
        node2 = attr2->parent;
        if (attr1 != nullptr && node1 != nullptr && node1 == node2) {
            for (const PrDomAttr* at = static_cast<const PrDomElement*>(node2)->firstAttribute;
                    at != nullptr; at = static_cast<const PrDomAttr*>(at->next)) {
                if (at == attr1)
                    return PrDomNodePosition_ImplementationSpecific | PrDomNodePosition_Preceding;
                if (at == attr2)
                    return PrDomNodePosition_ImplementationSpecific | PrDomNodePosition_Following;
            }
        }
    }
    if (node1 == nullptr || node2 == nullptr || domNodeRoot(node1) != domNodeRoot(node2)) {
        // An order that is consistent across calls: the order of the nodes' addresses.
        PrDomNodePosition position = PrDomNodePosition_Disconnected | PrDomNodePosition_ImplementationSpecific;
        position |= uintptr_t(node1) < uintptr_t(node2) ? PrDomNodePosition_Preceding : PrDomNodePosition_Following;
        return position;
    }
    if ((attr1 == nullptr && domIsInclusiveAncestor(node1, node2)) || (attr2 != nullptr && node1 == node2))
        return PrDomNodePosition_Contains | PrDomNodePosition_Preceding;
    if ((attr2 == nullptr && domIsInclusiveAncestor(node2, node1)) || (attr1 != nullptr && node1 == node2))
        return PrDomNodePosition_ContainedBy | PrDomNodePosition_Following;
    DomBoundary first = {const_cast<PrDomNode*>(node1), 0};
    DomBoundary second = {const_cast<PrDomNode*>(node2), 0};
    return domBoundaryCompare(first, second) < 0 ? PrDomNodePosition_Preceding : PrDomNodePosition_Following;
}

void prDomNodeNormalize(PrDomNode* node) {
    domNormalize(node);
}

PrStatus prDomNodeCloneNode(PrDomNode* node, bool deep, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* clone = made(domClone(node, node->document, deep));
    *out = clone;
    return clone != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
}

PrDomElement* PR_NULLABLE prDomNodeGetElementById(PrDomNode* node, PrStringView id) {
    return domElementById(node, id);
}

PrDomElement* PR_NULLABLE prDomNodeFirstElementChild(PrDomNode* node) {
    PrDomNode* child = node->first;
    while (child != nullptr && child->type != PrDomNodeType_Element)
        child = child->next;
    return domElementFromNode(child);
}

PrDomElement* PR_NULLABLE prDomNodeLastElementChild(PrDomNode* node) {
    PrDomNode* child = node->last;
    while (child != nullptr && child->type != PrDomNodeType_Element)
        child = child->previous;
    return domElementFromNode(child);
}

uint32_t prDomNodeChildElementCount(const PrDomNode* node) {
    uint32_t count = 0;
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        count += child->type == PrDomNodeType_Element;
    return count;
}

PrStatus prDomNodePrepend(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domPrepend(node, nodes, count);
}

PrStatus prDomNodeAppend(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domAppend(node, nodes, count);
}

PrStatus prDomNodeReplaceChildren(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domReplaceChildren(node, nodes, count);
}

PrDomElement* PR_NULLABLE prDomNodePreviousElementSibling(PrDomNode* node) {
    PrDomNode* sibling = node->parent != nullptr ? node->previous : nullptr;
    while (sibling != nullptr && sibling->type != PrDomNodeType_Element)
        sibling = sibling->previous;
    return domElementFromNode(sibling);
}

PrDomElement* PR_NULLABLE prDomNodeNextElementSibling(PrDomNode* node) {
    PrDomNode* sibling = node->parent != nullptr ? node->next : nullptr;
    while (sibling != nullptr && sibling->type != PrDomNodeType_Element)
        sibling = sibling->next;
    return domElementFromNode(sibling);
}

PrStatus prDomNodeBefore(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domBefore(node, nodes, count);
}

PrStatus prDomNodeAfter(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domAfter(node, nodes, count);
}

PrStatus prDomNodeReplaceWith(PrDomNode* node, const PrDomNodeOrString* PR_NULLABLE nodes, uint32_t count) {
    return domReplaceWith(node, nodes, count);
}

void prDomNodeRemove(PrDomNode* node) {
    domRemoveFromParent(node);
}

PrStringView prDomNodeLookupPrefix(PrDomNode* node, PrStringView namespaceUri) {
    if (namespaceUri.size == 0)
        return PrStringView{};
    for (PrDomElement* element = scopeElement(node); element != nullptr; element = prDomNodeParentElement(element)) {
        PrStringView prefix = prDomElementPrefix(element);
        if (prefix.size != 0 && prDomElementNamespaceUri(element) == namespaceUri)
            return prefix;
        uint32_t count = prDomElementAttributeCount(element);
        for (uint32_t i = 0; i < count; ++i) {
            const PrDomAttr* attribute = prDomElementAttributeAt(element, i);
            if (prDomAttrPrefix(attribute) == "xmlns" && prDomAttrValue(attribute) == namespaceUri)
                return prDomAttrLocalName(attribute);
        }
    }
    return PrStringView{};
}

PrStringView prDomNodeLookupNamespaceUri(PrDomNode* node, PrStringView prefix) {
    PrStringView xmlns = domNamedNamespaceUrl(PrDomNamespace_XmlNs);
    if (scopeElement(node) != nullptr) {
        if (prefix == "xml")
            return domNamedNamespaceUrl(PrDomNamespace_Xml);
        if (prefix == "xmlns")
            return xmlns;
    }
    for (PrDomElement* element = scopeElement(node); element != nullptr; element = prDomNodeParentElement(element)) {
        PrStringView space = prDomElementNamespaceUri(element);
        if (space.size != 0 && prDomElementPrefix(element) == prefix)
            return space;
        uint32_t count = prDomElementAttributeCount(element);
        for (uint32_t i = 0; i < count; ++i) {
            const PrDomAttr* attribute = prDomElementAttributeAt(element, i);
            if (prDomAttrNamespaceUri(attribute) != xmlns)
                continue;
            PrStringView declared = prDomAttrLocalName(attribute);
            bool named = prDomAttrPrefix(attribute) == "xmlns" && prefix.size != 0 && declared == prefix;
            bool fallback = prDomAttrPrefix(attribute).size == 0 && prefix.size == 0 && declared == "xmlns";
            if (named || fallback)
                return prDomAttrValue(attribute).size != 0 ? prDomAttrValue(attribute) : PrStringView{};
        }
    }
    return PrStringView{};
}

bool prDomNodeIsDefaultNamespace(PrDomNode* node, PrStringView namespaceUri) {
    PrStringView space = prDomNodeLookupNamespaceUri(node, PrStringView{});
    if (namespaceUri.size == 0)
        return space.size == 0;
    return space == namespaceUri;
}

PrDomDocument* PR_NULLABLE prDomDocumentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocument>(object, PrDomNodeType_Document);
}

PrDomDocument* prDomDocumentCreate(void) {
    return domDocumentCreate(DomDocumentKind_Xml);
}

PrStatus prDomDocumentCreateXml(PrDomImplementation* implementation, PrStringView namespaceUrl,
        PrStringView qualifiedName, PrDomDocumentType* PR_NULLABLE doctype, PrDomDocument* PR_NULLABLE* PR_NONNULL out) {
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
    PrStringView type = "application/xml";
    if (namespaceUrl == "http://www.w3.org/1999/xhtml")
        type = "application/xhtml+xml";
    else if (namespaceUrl == "http://www.w3.org/2000/svg")
        type = "image/svg+xml";
    document->contentType = prRawStringExternal(type);
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
    domInsert(html, domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, PrStringView{}), nullptr);
    return document;
}

void prDomDocumentRetain(PrDomDocument* document) {
    domNodeRetain(document);
}

void prDomDocumentRelease(PrDomDocument* PR_NULLABLE document) {
    domNodeRelease(document);
}

void prDomDocumentSetTreeCallback(PrDomDocument* document, const PrDomTreeCallback* PR_NULLABLE callback) {
    domDocumentSetTreeCallback(document, callback != nullptr ? *callback : PrDomTreeCallback{});
}

PrRawString prDomDocumentUrl(const PrDomDocument* document) {
    return prUrlSerialize(document->url, PrUrlSerialization_Whole);
}

PrStringView prDomDocumentContentType(const PrDomDocument* document) {
    return prRawStringView(&document->contentType);
}

PrStringView prDomDocumentCharacterSet(const PrDomDocument* document) {
    return prCharsetName(document->charset);
}

PrDomElement* PR_NULLABLE prDomDocumentElement(PrDomDocument* document) {
    return domDocumentElement(document);
}

PrStatus prDomDocumentImportNode(PrDomDocument* document, PrDomNode* node, bool deep, PrDomNode* PR_NULLABLE* PR_NONNULL out) {
    PrDomNode* clone = domNodeShadowRoot(node) == nullptr ? made(domClone(node, document, deep)) : nullptr;
    *out = clone;
    return clone != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
}

PrStatus prDomDocumentAdoptNode(PrDomDocument* document, PrDomNode* node) {
    if (node->type == PrDomNodeType_Document)
        return PrStatus_NotSupported;
    if (domNodeShadowRoot(node) != nullptr)
        return PrStatus_NotAllowed;
    // The contents of a template or the tree of a shadow host stay with their host.
    if (node->type == PrDomNodeType_DocumentFragment && static_cast<PrDomDocumentFragment*>(node)->host != nullptr)
        return PrStatus_Ok;
    if (node->type == PrDomNodeType_Attribute) {
        if (node->parent != nullptr)
            domRemoveAttributeNode(static_cast<PrDomElement*>(node->parent), static_cast<PrDomAttr*>(node));
    } else {
        domRemoveFromParent(node);
    }
    domAdopt(document, node);
    return PrStatus_Ok;
}

PrStatus prDomElementCreate(PrDomDocument* document, PrStringView localName,
        const PrDomElementCreationOptions* PR_NULLABLE options, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    if (!domIsValidElementLocalName(localName))
        return PrStatus_InvalidData;
    // An HTML document lowercases the name. An HTML or XHTML document creates an HTML element.
    PrTextBuffer buffer = {};
    PrStringView name = lowered(localName, document->kind == DomDocumentKind_Html, &buffer);
    bool html = document->kind == DomDocumentKind_Html ||
        prRawStringView(&document->contentType) == "application/xhtml+xml";
    PrStringView is = options != nullptr ? options->is : PrStringView{};
    bool held = false;
    PrDomElement* created;
    if (html)
        created = domElementCreateHtml(document, domTagFromText(name), name, is, true, &held);
    else
        created = domElementCreate(document, PrDomNamespace_None, PrDomTag_Unknown, name);
    PrDomElement* element = made(created);
    if (held)
        domNodeUnhold(element);
    prTextBufferReset(&buffer);
    if (out != nullptr)
        *out = element;
    else
        domNodeRelease(element);
    return PrStatus_Ok;
}

PrStatus prDomElementCreateNs(PrDomDocument* document, PrStringView namespaceUrl, PrStringView qualifiedName,
        const PrDomElementCreationOptions* PR_NULLABLE options, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(document, namespaceUrl, qualifiedName, true, &space, &prefix, &local);
    if (status != PrStatus_Ok)
        return status;
    PrDomElement* element = nullptr;
    if (space == PrDomNamespace_Html) {
        bool held = false;
        element = made(domElementCreateHtml(document, domTagFromText(local), local,
            options != nullptr ? options->is : PrStringView{}, true, &held));
        if (held)
            domNodeUnhold(element);
        if (prefix.data != nullptr)
            domElementState(element)->prefix = prRawStringCopy(prefix);
    } else {
        element = made(domElementCreateNs(document, space, prefix, local));
    }
    *out = element;
    return status;
}

PrStatus prDomAttrCreate(PrDomDocument* document, PrStringView localName, PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!domIsValidAttributeLocalName(localName))
        return PrStatus_InvalidData;
    PrTextBuffer buffer = {};
    PrStringView name = lowered(localName, document->kind == DomDocumentKind_Html, &buffer);
    *out = made(domAttrCreate(document, PrDomNamespace_None, PrStringView{}, name, ""));
    prTextBufferReset(&buffer);
    return PrStatus_Ok;
}

PrStatus prDomAttrCreateNs(PrDomDocument* document, PrStringView namespaceUrl, PrStringView qualifiedName,
        PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(document, namespaceUrl, qualifiedName, false, &space, &prefix, &local);
    if (status == PrStatus_Ok)
        *out = made(domAttrCreate(document, space, prefix, local, ""));
    return status;
}

PrDomDocumentFragment* prDomDocumentFragmentCreate(PrDomDocument* document) {
    return made(domDocumentFragmentCreate(document));
}

PrDomText* prDomTextCreate(PrDomDocument* document, PrStringView data) {
    return made(domTextCreate(document, data));
}

PrStatus prDomTextCreateCdataSection(PrDomDocument* document, PrStringView data, PrDomText* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (document->kind == DomDocumentKind_Html)
        return PrStatus_NotSupported;
    if (prStringContains(data, "]]>"))
        return PrStatus_InvalidData;
    *out = made(domTextCreateCdataSection(document, data));
    return PrStatus_Ok;
}

PrDomComment* prDomCommentCreate(PrDomDocument* document, PrStringView data) {
    return made(domCommentCreate(document, data));
}

PrStatus prDomProcessingInstructionCreate(PrDomDocument* document, PrStringView target, PrStringView data,
        PrDomProcessingInstruction* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!prXmlIsName(target) || prStringContains(data, "?>"))
        return PrStatus_InvalidData;
    *out = made(domProcessingInstructionCreate(document, target, data));
    return PrStatus_Ok;
}

// html/#document.title
PrRawString prDomDocumentTitle(const PrDomDocument* document) {
    const PrDomElement* root = domDocumentElement(document);
    const PrDomElement* title;
    if (isSvgRoot(root))
        title = firstElement(root, PrDomNamespace_Svg, PrDomTag_Title, true);
    else
        title = firstElement(document, PrDomNamespace_Html, PrDomTag_Title, false);
    if (title == nullptr)
        return prRawStringEmpty();
    PrRawString text = domChildTextContent(title);
    PrRawString result = stripAndCollapse(prRawStringView(&text));
    prRawStringReset(&text);
    return result;
}

void prDomDocumentSetTitle(PrDomDocument* document, PrStringView value) {
    PrDomElement* root = domDocumentElement(document);
    PrDomElement* title = nullptr;
    if (isSvgRoot(root)) {
        title = firstElement(root, PrDomNamespace_Svg, PrDomTag_Title, true);
        if (title == nullptr) {
            title = domElementCreate(document, PrDomNamespace_Svg, PrDomTag_Title, "title");
            domInsert(root, title, root->first);
        }
    } else if (root != nullptr && root->space == PrDomNamespace_Html) {
        title = firstElement(document, PrDomNamespace_Html, PrDomTag_Title, false);
        PrDomElement* head = domDocumentHead(document);
        if (title == nullptr && head == nullptr)
            return;
        if (title == nullptr) {
            title = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Title, "title");
            domInsert(head, title, nullptr);
        }
    } else {
        return;
    }
    domStringReplaceAll(title, value);
}

PrDomElement* PR_NULLABLE prDomDocumentBody(PrDomDocument* document) {
    return domDocumentBody(document);
}

PrStatus prDomDocumentSetBody(PrDomDocument* document, PrDomElement* PR_NULLABLE value) {
    if (!domIsHtml(value, PrDomTag_Body) && !domIsHtml(value, PrDomTag_Frameset))
        return PrStatus_NotAllowed;
    PrDomElement* body = domDocumentBody(document);
    if (value == body)
        return PrStatus_Ok;
    if (body != nullptr)
        return domReplace(body->parent, value, body);
    PrDomElement* root = domDocumentElement(document);
    return root != nullptr ? domPreInsert(root, value, nullptr) : PrStatus_NotAllowed;
}

PrDomElement* PR_NULLABLE prDomDocumentHead(PrDomDocument* document) {
    return domDocumentHead(document);
}

PrDomDocumentType* PR_NULLABLE prDomDocumentDoctype(PrDomDocument* document) {
    for (PrDomNode* child = document->first; child != nullptr; child = child->next) {
        if (child->type == PrDomNodeType_DocumentType)
            return static_cast<PrDomDocumentType*>(child);
    }
    return nullptr;
}

bool prDomDocumentQuirks(const PrDomDocument* document) {
    return document->quirks == DomQuirks_Yes;
}

PrDomImplementation* prDomDocumentImplementation(PrDomDocument* document) {
    return static_cast<PrDomImplementation*>(document);
}

PrStatus prDomDocumentTypeCreate(PrDomImplementation* implementation, PrStringView name,
        PrStringView publicId, PrStringView systemId, PrDomDocumentType* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // dom/#valid-doctype-name: no ASCII whitespace, U+0000 or '>'.
    for (size_t i = 0; i < name.size; ++i) {
        if (prAsciiIsSpace(name.data[i]) || name.data[i] == '\0' || name.data[i] == '>')
            return PrStatus_InvalidData;
    }
    *out = made(domDocumentTypeCreate(implementation, name, publicId, systemId));
    return PrStatus_Ok;
}

PrDomDocumentType* PR_NULLABLE prDomDocumentTypeQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocumentType>(object, PrDomNodeType_DocumentType);
}

PrStringView prDomDocumentTypeName(const PrDomDocumentType* doctype) {
    return prRawStringView(&doctype->name);
}

// A doctype that the parser created without an identifier holds the null string, which script sees as the empty string.
PrStringView prDomDocumentTypePublicId(const PrDomDocumentType* doctype) {
    const PrRawString* id = &doctype->publicId;
    return prRawStringIsNull(id) ? "" : prRawStringView(id);
}

PrStringView prDomDocumentTypeSystemId(const PrDomDocumentType* doctype) {
    const PrRawString* id = &doctype->systemId;
    return prRawStringIsNull(id) ? "" : prRawStringView(id);
}

PrDomDocumentFragment* PR_NULLABLE prDomDocumentFragmentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomDocumentFragment>(object, PrDomNodeType_DocumentFragment);
}

PrDomShadowRoot* PR_NULLABLE prDomShadowRootQuery(PrDomEventTarget* PR_NULLABLE object) {
    return domNodeShadowRoot(domNodeFromTarget(object));
}

PrDomShadowRootMode prDomShadowRootMode(const PrDomShadowRoot* shadow) {
    return shadow->mode;
}

bool prDomShadowRootDelegatesFocus(const PrDomShadowRoot* shadow) {
    return shadow->delegatesFocus;
}

PrDomSlotAssignmentMode prDomShadowRootSlotAssignment(const PrDomShadowRoot* shadow) {
    return shadow->slotAssignment;
}

bool prDomShadowRootClonable(const PrDomShadowRoot* shadow) {
    return shadow->clonable;
}

bool prDomShadowRootSerializable(const PrDomShadowRoot* shadow) {
    return shadow->serializable;
}

PrDomElement* prDomShadowRootHost(PrDomShadowRoot* shadow) {
    return shadow->host;
}

PrStringView prDomNamespaceUrl(PrDomNamespace space) {
    return space < PrDomNamespace_Other ? domNamedNamespaceUrl(DomNamespaceId(space)) : PrStringView{};
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

PrStringView prDomElementLocalName(const PrDomElement* element) {
    return domElementName(element);
}

PrDomTag prDomElementTag(const PrDomElement* element) {
    return domElementTag(element);
}

PrStringView prDomElementNamespaceUri(const PrDomElement* element) {
    return domNamespaceUrl(element->document, element->space);
}

PrDomNamespace prDomElementNamespace(const PrDomElement* element) {
    DomNamespaceId space = element->space;
    return space < PrDomNamespace_Other ? PrDomNamespace(space) : PrDomNamespace_Other;
}

PrStringView prDomElementPrefix(const PrDomElement* element) {
    return element->state != nullptr ? prRawStringView(&element->state->prefix) : PrStringView{};
}

PrRawString prDomElementTagName(const PrDomElement* element) {
    return domElementTagName(element);
}

PrDomTag prDomElementTagNameTag(const PrDomElement* element, bool* uppercase) {
    *uppercase = domIsHtmlInHtmlDocument(element);
    bool prefixed = element->state != nullptr && !prRawStringIsNull(&element->state->prefix);
    return prefixed ? PrDomTag_Unknown : domElementTag(element);
}

PrStringView prDomElementId(const PrDomElement* element) {
    PrStringView id = domElementId(element);
    return id.data != nullptr ? id : "";
}

void prDomElementSetId(PrDomElement* element, PrStringView value) {
    domSetAttribute(element, PrDomNamespace_None, "id", value);
}

PrStringView prDomElementSlot(const PrDomElement* element) {
    PrStringView slot = domAttributeValue(element, "slot");
    return slot.data != nullptr ? slot : "";
}

void prDomElementSetSlot(PrDomElement* element, PrStringView value) {
    domSetAttribute(element, PrDomNamespace_None, "slot", value);
}

PrStringView prDomElementGetAttribute(const PrDomElement* element, PrStringView qualifiedName) {
    const PrDomAttr* attribute = domAttributeByName(element, qualifiedName);
    return attribute != nullptr ? prRawStringView(&attribute->value) : PrStringView{};
}

PrStatus prDomElementSetAttribute(PrDomElement* element, PrStringView qualifiedName, PrStringView value) {
    if (!domIsValidAttributeLocalName(qualifiedName))
        return PrStatus_InvalidData;
    PrTextBuffer buffer = {};
    PrStringView name = lowered(qualifiedName, domIsHtmlInHtmlDocument(element), &buffer);
    // dom/#dom-element-setattribute: sets the attribute with that qualified name, whatever its namespace, or else adds
    // a new attribute in no namespace.
    if (PrDomAttr* attribute = domAttributeByName(element, name))
        domSetAttribute(element, attribute->space, prRawStringView(&attribute->name), value);
    else
        domSetAttribute(element, PrDomNamespace_None, name, value);
    prTextBufferReset(&buffer);
    return PrStatus_Ok;
}

void prDomElementRemoveAttribute(PrDomElement* element, PrStringView qualifiedName) {
    if (PrDomAttr* attribute = domAttributeByName(element, qualifiedName))
        domRemoveAttributeNode(element, attribute);
}

bool prDomElementHasAttribute(const PrDomElement* element, PrStringView qualifiedName) {
    return domAttributeByName(element, qualifiedName) != nullptr;
}

bool prDomElementHasAttributes(const PrDomElement* element) {
    return element->firstAttribute != nullptr;
}

PrStringView prDomElementGetAttributeNs(const PrDomElement* element, PrStringView namespaceUrl, PrStringView localName) {
    const PrDomAttr* attribute = attributeNs(element, namespaceUrl, localName);
    return attribute != nullptr ? prRawStringView(&attribute->value) : PrStringView{};
}

PrStatus prDomElementSetAttributeNs(PrDomElement* element, PrStringView namespaceUrl,
        PrStringView qualifiedName, PrStringView value) {
    DomNamespaceId space = PrDomNamespace_None;
    PrStringView prefix = {};
    PrStringView local = {};
    PrStatus status = validateAndExtract(element->document, namespaceUrl, qualifiedName, false, &space, &prefix, &local);
    if (status == PrStatus_Ok)
        domSetAttributeNs(element, space, prefix, local, value);
    return status;
}

void prDomElementRemoveAttributeNs(PrDomElement* element, PrStringView namespaceUrl, PrStringView localName) {
    if (PrDomAttr* attribute = attributeNs(element, namespaceUrl, localName))
        domRemoveAttributeNode(element, attribute);
}

bool prDomElementHasAttributeNs(const PrDomElement* element, PrStringView namespaceUrl, PrStringView localName) {
    return attributeNs(element, namespaceUrl, localName) != nullptr;
}

uint32_t prDomElementGetAttributeNames(const PrDomElement* element, PrRawString* PR_NULLABLE items, uint32_t capacity) {
    uint32_t count = 0;
    for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<const PrDomAttr*>(attribute->next), ++count) {
        if (count < capacity)
            items[count] = attributeQualifiedName(attribute);
    }
    return count;
}

PrStatus prDomElementToggleAttribute(PrDomElement* element, PrStringView qualifiedName,
        const bool* PR_NULLABLE force, bool* PR_NULLABLE out) {
    if (!domIsValidAttributeLocalName(qualifiedName))
        return PrStatus_InvalidData;
    PrTextBuffer buffer = {};
    PrStringView name = lowered(qualifiedName, domIsHtmlInHtmlDocument(element), &buffer);
    PrDomAttr* attribute = domAttributeByName(element, name);
    bool present = attribute != nullptr;
    if (attribute == nullptr && (force == nullptr || *force)) {
        domAppendAttribute(element, domAttrCreate(element->document, PrDomNamespace_None, PrStringView{}, name, ""));
        present = true;
    } else if (attribute != nullptr && (force == nullptr || !*force)) {
        domRemoveAttributeNode(element, attribute);
        present = false;
    }
    prTextBufferReset(&buffer);
    if (out != nullptr)
        *out = present;
    return PrStatus_Ok;
}

uint32_t prDomElementAttributeCount(const PrDomElement* element) {
    return domElementAttributeCount(element);
}

PrDomAttr* PR_NULLABLE prDomElementAttributeAt(PrDomElement* element, uint32_t index) {
    PrDomAttr* attribute = element->firstAttribute;
    for (; attribute != nullptr && index != 0; --index)
        attribute = static_cast<PrDomAttr*>(attribute->next);
    return attribute;
}

uint32_t prDomElementNamedAttributeNames(const PrDomElement* element, PrRawString* PR_NULLABLE items, uint32_t capacity) {
    bool html = domIsHtmlInHtmlDocument(element);
    uint32_t count = 0;
    for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<const PrDomAttr*>(attribute->next)) {
        PrRawString name = attributeQualifiedName(attribute);
        PrStringView view = prRawStringView(&name);
        bool keep = true;
        for (size_t i = 0; html && keep && i < view.size; ++i)
            keep = !prAsciiIsUpper(view.data[i]);
        // If an earlier attribute has the same name, the name belongs to that attribute.
        for (const PrDomAttr* before = element->firstAttribute; keep && before != attribute;
                before = static_cast<const PrDomAttr*>(before->next)) {
            PrRawString earlier = attributeQualifiedName(before);
            keep = prRawStringView(&earlier) != view;
            prRawStringReset(&earlier);
        }
        if (keep && count < capacity)
            items[count] = name;
        else
            prRawStringReset(&name);
        count += keep;
    }
    return count;
}

PrDomAttr* PR_NULLABLE prDomElementGetAttributeNode(PrDomElement* element, PrStringView qualifiedName) {
    return domAttributeByName(element, qualifiedName);
}

PrDomAttr* PR_NULLABLE prDomElementGetAttributeNodeNs(PrDomElement* element, PrStringView namespaceUrl,
        PrStringView localName) {
    return attributeNs(element, namespaceUrl, localName);
}

PrStatus prDomElementSetAttributeNode(PrDomElement* element, PrDomAttr* attr, PrDomAttr* PR_NULLABLE* PR_NULLABLE out) {
    PrDomAttr* attribute = attr;
    if (out != nullptr)
        *out = nullptr;
    if (attribute->parent != nullptr && attribute->parent != element)
        return PrStatus_InvalidState;
    PrDomAttr* old = attributeNs(element, domNamespaceUrl(attribute->document, attribute->space),
        prRawStringView(&attribute->name));
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
        domReplaceAttribute(element, old, attribute);
    else
        domAppendAttribute(element, attribute);
    if (out != nullptr)
        *out = old;
    return PrStatus_Ok;
}

PrStatus prDomElementRemoveAttributeNode(PrDomElement* element, PrDomAttr* attr) {
    if (attr->parent != element)
        return PrStatus_NotFound;
    domRemoveAttributeNode(element, attr);
    return PrStatus_Ok;
}

PrStatus prDomElementRemoveNamedAttribute(PrDomElement* element, PrStringView qualifiedName,
        PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    return removeNamed(element, domAttributeByName(element, qualifiedName), out);
}

PrStatus prDomElementRemoveNamedAttributeNs(PrDomElement* element, PrStringView namespaceUrl, PrStringView localName,
        PrDomAttr* PR_NULLABLE* PR_NONNULL out) {
    return removeNamed(element, attributeNs(element, namespaceUrl, localName), out);
}

PrStatus prDomElementInsertAdjacentElement(PrDomElement* target, PrDomInsertPosition where, PrDomElement* element,
        PrDomElement* PR_NULLABLE* PR_NULLABLE out) {
    bool inserted = false;
    PrStatus status = insertAdjacent(target, where, element, &inserted);
    if (out != nullptr)
        *out = status == PrStatus_Ok && inserted ? element : nullptr;
    return status;
}

PrStatus prDomElementInsertAdjacentText(PrDomElement* element, PrDomInsertPosition where, PrStringView data) {
    PrDomText* text = domTextCreate(element->document, data);
    bool inserted = false;
    PrStatus status = insertAdjacent(element, where, text, &inserted);
    if (!inserted || status != PrStatus_Ok)
        domDropTree(text);
    return status;
}

PrStatus prDomElementAttachShadow(PrDomElement* element, const PrDomShadowRootInit* init,
        PrDomShadowRoot* PR_NULLABLE* PR_NONNULL out) {
    PrDomShadowRoot* shadow = nullptr;
    PrStatus status = domAttachShadow(element, init, &shadow);
    *out = shadow;
    return status;
}

PrDomShadowRoot* PR_NULLABLE prDomElementShadowRoot(PrDomElement* element) {
    PrDomShadowRoot* shadow = domShadowRootOfHost(element);
    return shadow != nullptr && shadow->mode == PrDomShadowRootMode_Open ? shadow : nullptr;
}

PrStringView prDomElementClassName(const PrDomElement* element) {
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "class");
    return attribute != nullptr ? prRawStringView(&attribute->value) : "";
}

void prDomElementSetClassName(PrDomElement* element, PrStringView value) {
    domSetAttribute(element, PrDomNamespace_None, "class", value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attr

PrDomAttr* PR_NULLABLE prDomAttrQuery(PrDomEventTarget* PR_NULLABLE object) {
    return attrOf(domNodeFromTarget(object));
}

PrStringView prDomAttrNamespaceUri(const PrDomAttr* attr) {
    return domNamespaceUrl(attr->document, attr->space);
}

PrDomNamespace prDomAttrNamespace(const PrDomAttr* attr) {
    DomNamespaceId space = attr->space;
    return space < PrDomNamespace_Other ? PrDomNamespace(space) : PrDomNamespace_Other;
}

PrStringView prDomAttrPrefix(const PrDomAttr* attr) {
    return domAttributePrefix(attr);
}

PrStringView prDomAttrLocalName(const PrDomAttr* attr) {
    return prRawStringView(&attr->name);
}

PrRawString prDomAttrName(const PrDomAttr* attr) {
    return attributeQualifiedName(attr);
}

PrStringView prDomAttrValue(const PrDomAttr* attr) {
    return prRawStringView(&attr->value);
}

void prDomAttrSetValue(PrDomAttr* attr, PrStringView value) {
    domSetAttributeValue(attr, value);
}

PrDomElement* PR_NULLABLE prDomAttrOwnerElement(PrDomAttr* attr) {
    return domElementFromNode(attr->parent);
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

PrStringView prDomCharacterDataData(const PrDomCharacterData* node) {
    return prRawStringView(&node->data);
}

void prDomCharacterDataSetData(PrDomCharacterData* node, PrStringView value) {
    domSetData(node, value);
}

uint32_t prDomCharacterDataLength(const PrDomCharacterData* node) {
    return domCharacterDataLength(node);
}

PrStatus prDomCharacterDataSubstringData(const PrDomCharacterData* node, uint32_t offset, uint32_t count, PrRawString* out) {
    return domSubstringData(node, offset, count, out);
}

void prDomCharacterDataAppendData(PrDomCharacterData* node, PrStringView data) {
    domReplaceData(node, domCharacterDataLength(node), 0, data);
}

PrStatus prDomCharacterDataInsertData(PrDomCharacterData* node, uint32_t offset, PrStringView data) {
    return domReplaceData(node, offset, 0, data);
}

PrStatus prDomCharacterDataDeleteData(PrDomCharacterData* node, uint32_t offset, uint32_t count) {
    return domReplaceData(node, offset, count, "");
}

PrStatus prDomCharacterDataReplaceData(PrDomCharacterData* node, uint32_t offset, uint32_t count, PrStringView data) {
    return domReplaceData(node, offset, count, data);
}

PrDomText* PR_NULLABLE prDomTextQuery(PrDomEventTarget* PR_NULLABLE object) {
    // A CDATASection is a Text (dom/#interface-cdatasection).
    PrDomText* text = nodeOfType<PrDomText>(object, PrDomNodeType_Text);
    return text != nullptr ? text : nodeOfType<PrDomText>(object, PrDomNodeType_CdataSection);
}

// dom/#concept-text-split
PrStatus prDomTextSplitText(PrDomText* text, uint32_t offset, PrDomText* PR_NULLABLE* PR_NONNULL out) {
    PrDomText* created = domSplitText(text, offset);
    if (out != nullptr)
        *out = made(created);
    return created != nullptr ? PrStatus_Ok : PrStatus_OutOfRange;
}

// dom/#dom-text-wholetext: the data of the contiguous Text nodes, in tree order.
PrRawString prDomTextWholeText(const PrDomText* text) {
    const PrDomNode* first = text;
    while (first->previous != nullptr && first->parent != nullptr && domIsText(first->previous))
        first = first->previous;
    PrTextBuffer out = {};
    for (const PrDomNode* node = first; node != nullptr && domIsText(node); node = node->parent != nullptr ? node->next : nullptr)
        prTextBufferAppend(&out, prRawStringView(&static_cast<const PrDomText*>(node)->data));
    return prTextBufferTake(&out);
}

PrDomComment* PR_NULLABLE prDomCommentQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomComment>(object, PrDomNodeType_Comment);
}

PrDomProcessingInstruction* PR_NULLABLE prDomProcessingInstructionQuery(PrDomEventTarget* PR_NULLABLE object) {
    return nodeOfType<PrDomProcessingInstruction>(object, PrDomNodeType_ProcessingInstruction);
}

PrStringView prDomProcessingInstructionTarget(const PrDomProcessingInstruction* instruction) {
    return prRawStringView(&instruction->target);
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

PrDomNode* prDomHtmlCollectionRoot(PrDomHtmlCollection* collection) {
    return collection->root;
}

uint32_t prDomHtmlCollectionLength(const PrDomHtmlCollection* collection) {
    return domHtmlCollectionLength(collection);
}

PrDomElement* PR_NULLABLE prDomHtmlCollectionElementAt(PrDomHtmlCollection* collection, uint32_t index) {
    return domHtmlCollectionItem(collection, index);
}

PrDomElement* PR_NULLABLE prDomHtmlCollectionNamedItem(PrDomHtmlCollection* collection, PrStringView name) {
    return domHtmlCollectionNamedItem(collection, name);
}

uint32_t prDomHtmlCollectionNames(const PrDomHtmlCollection* collection, PrStringView* PR_NULLABLE items,
        uint32_t capacity) {
    RawArray<PrStringView> names;
    domHtmlCollectionNames(collection, &names);
    for (uint32_t i = 0; i < names.count && i < capacity; ++i)
        items[i] = names.data[i];
    uint32_t count = names.count;
    names.reset();
    return count;
}

void prDomHtmlCollectionDestroy(PrDomHtmlCollection* PR_NULLABLE collection) {
    domHtmlCollectionDestroy(collection);
}

PrDomNodeList* prDomNodeListCreate(void) {
    return domNodeListCreate(nullptr);
}

PrDomNodeList* prDomNodeListCreateChildNodes(PrDomNode* parent) {
    return domNodeListCreate(parent);
}

PrDomNode* PR_NULLABLE prDomNodeListNodeAt(PrDomNodeList* list, uint32_t index) {
    return domNodeListItem(list, index);
}

uint32_t prDomNodeListLength(const PrDomNodeList* list) {
    return domNodeListLength(list);
}

void prDomNodeListDestroy(PrDomNodeList* PR_NULLABLE list) {
    domNodeListDestroy(list);
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMTokenList

PrDomTokenList* prDomTokenListCreate(PrDomElement* element, PrStringView name) {
    return domTokenListCreate(element, name);
}

PrStatus prDomTokenListSupports(const PrDomTokenList* list, PrStringView token, bool* out) {
    *out = false;
    if (list->supported == nullptr)
        return PrStatus_WrongType;
    for (uint32_t i = 0; i < list->supportedCount; ++i)
        *out = *out || prAsciiEqualsFold(token, list->supported[i]);
    return PrStatus_Ok;
}

uint32_t prDomTokenListLength(const PrDomTokenList* list) {
    return domTokenListLength(list);
}

PrStringView prDomTokenListItem(const PrDomTokenList* list, uint32_t index) {
    return domTokenListItem(list, index);
}

bool prDomTokenListContains(const PrDomTokenList* list, PrStringView token) {
    return domTokenListContains(list, token);
}

PrStatus prDomTokenListAdd(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count) {
    return domTokenListAdd(list, tokens, count);
}

PrStatus prDomTokenListRemove(PrDomTokenList* list, const PrStringView* PR_NULLABLE tokens, uint32_t count) {
    return domTokenListRemove(list, tokens, count);
}

PrStatus prDomTokenListToggle(PrDomTokenList* list, PrStringView token, const bool* PR_NULLABLE force, bool* out) {
    return domTokenListToggle(list, token, force, out);
}

PrStatus prDomTokenListReplace(PrDomTokenList* list, PrStringView token, PrStringView newToken, bool* out) {
    return domTokenListReplace(list, token, newToken, out);
}

PrStringView prDomTokenListValue(const PrDomTokenList* list) {
    return domTokenListValue(list);
}

void prDomTokenListSetValue(PrDomTokenList* list, PrStringView value) {
    domTokenListSetValue(list, value);
}

void prDomTokenListDestroy(PrDomTokenList* PR_NULLABLE list) {
    domTokenListDestroy(list);
}

// ---------------------------------------------------------------------------------------------------------------------
// MutationObserver

PrDomMutationObserver* prDomMutationObserverCreate(const PrDomMutationCallback* callback) {
    return domMutationObserverCreate(callback);
}

void prDomMutationObserverRetain(PrDomMutationObserver* observer) {
    domMutationObserverRetain(observer);
}

void prDomMutationObserverRelease(PrDomMutationObserver* PR_NULLABLE observer) {
    domMutationObserverRelease(observer);
}

PrStatus prDomMutationObserverObserve(PrDomMutationObserver* observer, PrDomNode* target,
        const PrDomMutationObserverInit* options, const PrStringView* PR_NULLABLE filter, uint32_t filterCount) {
    return domMutationObserverObserve(observer, target, options, filter, filterCount);
}

void prDomMutationObserverDisconnect(PrDomMutationObserver* observer) {
    domMutationObserverDisconnect(observer);
}

uint32_t prDomMutationObserverRecordCount(const PrDomMutationObserver* observer) {
    return observer->records.count;
}

bool prDomMutationObserverTakeRecord(PrDomMutationObserver* observer, PrDomMutationRecord* record) {
    return domMutationObserverTake(observer, record);
}

// ---------------------------------------------------------------------------------------------------------------------
// MutationRecord

PrDomMutationRecord* prDomMutationRecordCreate(void) {
    return allocZeroed<PrDomMutationRecord>();
}

void prDomMutationRecordDestroy(PrDomMutationRecord* PR_NULLABLE record) {
    domMutationRecordDestroy(record);
}

PrDomMutationType prDomMutationRecordType(const PrDomMutationRecord* record) {
    return record->type;
}

PrDomNode* prDomMutationRecordTarget(PrDomMutationRecord* record) {
    return record->target;
}

void prDomMutationRecordAddedNodes(PrDomMutationRecord* record, PrDomNodeList* list) {
    domNodeListAssign(list, record->added.data, record->added.count);
}

void prDomMutationRecordRemovedNodes(PrDomMutationRecord* record, PrDomNodeList* list) {
    domNodeListAssign(list, record->removed.data, record->removed.count);
}

PrDomNode* PR_NULLABLE prDomMutationRecordPreviousSibling(PrDomMutationRecord* record) {
    return record->previous;
}

PrDomNode* PR_NULLABLE prDomMutationRecordNextSibling(PrDomMutationRecord* record) {
    return record->next;
}

PrStringView prDomMutationRecordAttributeName(const PrDomMutationRecord* record) {
    return prRawStringView(&record->attributeName);
}

PrStringView prDomMutationRecordAttributeNamespace(const PrDomMutationRecord* record) {
    return prRawStringView(&record->attributeNamespace);
}

PrStringView prDomMutationRecordOldValue(const PrDomMutationRecord* record) {
    return prRawStringView(&record->oldValue);
}

PrDomRange* prDomRangeCreate(PrDomDocument* document) {
    return domRangeCreate(document);
}

PrStatus prDomRangeCreateStatic(PrDomNode* startContainer, uint32_t startOffset,
        PrDomNode* endContainer, uint32_t endOffset, PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    auto invalid = [](const PrDomNode* node) {
        return node->type == PrDomNodeType_DocumentType || node->type == PrDomNodeType_Attribute;
    };
    if (invalid(startContainer) || invalid(endContainer))
        return PrStatus_WrongType;
    *out = domStaticRangeCreate(DomBoundary{startContainer, startOffset}, DomBoundary{endContainer, endOffset});
    return PrStatus_Ok;
}

void prDomRangeRetain(PrDomRange* range) {
    domRangeRetain(range);
}

void prDomRangeRelease(PrDomRange* PR_NULLABLE range) {
    domRangeRelease(range);
}

bool prDomRangeIsLive(const PrDomRange* range) {
    return range->document != nullptr;
}

PrDomNode* prDomRangeStartContainer(PrDomRange* range) {
    return range->start.node;
}

uint32_t prDomRangeStartOffset(const PrDomRange* range) {
    return range->start.offset;
}

PrDomNode* prDomRangeEndContainer(PrDomRange* range) {
    return range->end.node;
}

uint32_t prDomRangeEndOffset(const PrDomRange* range) {
    return range->end.offset;
}

bool prDomRangeCollapsed(const PrDomRange* range) {
    return domRangeCollapsed(range);
}

PrDomNode* prDomRangeCommonAncestorContainer(PrDomRange* range) {
    PrDomNode* container = range->start.node;
    while (!domIsInclusiveAncestor(container, range->end.node))
        container = domNodeParent(container);
    return container;
}

PrStatus prDomRangeSetStart(PrDomRange* range, PrDomNode* node, uint32_t offset) {
    return changed(range, domRangeSetStart(range, node, offset));
}

PrStatus prDomRangeSetEnd(PrDomRange* range, PrDomNode* node, uint32_t offset) {
    return changed(range, domRangeSetEnd(range, node, offset));
}

PrStatus prDomRangeSetBeside(PrDomRange* range, PrDomRangeSide side, PrDomNode* node) {
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return PrStatus_WrongType;
    bool after = side == PrDomRangeSide_StartAfter || side == PrDomRangeSide_EndAfter;
    uint32_t offset = domNodeIndex(node) + (after ? 1 : 0);
    bool start = side == PrDomRangeSide_StartBefore || side == PrDomRangeSide_StartAfter;
    return changed(range, start ? domRangeSetStart(range, parent, offset) : domRangeSetEnd(range, parent, offset));
}

void prDomRangeCollapse(PrDomRange* range, bool toStart) {
    domRangeCollapse(range, toStart);
    changed(range, PrStatus_Ok);
}

PrStatus prDomRangeSelectNode(PrDomRange* range, PrDomNode* node) {
    return changed(range, domRangeSelect(range, node));
}

PrStatus prDomRangeSelectNodeContents(PrDomRange* range, PrDomNode* node) {
    return changed(range, domRangeSelectContents(range, node));
}

PrStatus prDomRangeCompareBoundaryPoints(const PrDomRange* range, PrDomRangeHow how, const PrDomRange* source, int8_t* out) {
    if (domRangeRoot(range) != domRangeRoot(source))
        return PrStatus_Foreign;
    bool start = how == PrDomRangeHow_StartToStart || how == PrDomRangeHow_EndToStart;
    bool otherStart = how == PrDomRangeHow_StartToStart || how == PrDomRangeHow_StartToEnd;
    DomBoundary point = start ? range->start : range->end;
    DomBoundary otherPoint = otherStart ? source->start : source->end;
    *out = int8_t(domBoundaryCompare(point, otherPoint));
    return PrStatus_Ok;
}

void prDomRangeDeleteContents(PrDomRange* range) {
    domRangeDelete(range);
    changed(range, PrStatus_Ok);
}

PrStatus prDomRangeExtractContents(PrDomRange* range, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    return rangeContents(range, true, out);
}

PrStatus prDomRangeCloneContents(PrDomRange* range, PrDomDocumentFragment* PR_NULLABLE* PR_NONNULL out) {
    return rangeContents(range, false, out);
}

PrStatus prDomRangeInsertNode(PrDomRange* range, PrDomNode* node) {
    return changed(range, domRangeInsert(range, node));
}

PrStatus prDomRangeSurroundContents(PrDomRange* range, PrDomNode* node) {
    return changed(range, domRangeSurround(range, node));
}

PrDomRange* prDomRangeCloneRange(PrDomRange* range) {
    PrDomRange* copy = domRangeCreate(range->start.node->document);
    domRangeSetStart(copy, range->start.node, range->start.offset);
    domRangeSetEnd(copy, range->end.node, range->end.offset);
    return copy;
}

PrStatus prDomRangeIsPointInRange(const PrDomRange* range, PrDomNode* node, uint32_t offset, bool* out) {
    *out = false;
    if (domNodeRoot(node) != domRangeRoot(range))
        return PrStatus_Ok;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary boundary = {node, offset};
    *out = domBoundaryCompare(boundary, range->start) >= 0 && domBoundaryCompare(boundary, range->end) <= 0;
    return PrStatus_Ok;
}

PrStatus prDomRangeComparePoint(const PrDomRange* range, PrDomNode* node, uint32_t offset, int8_t* out) {
    if (domNodeRoot(node) != domRangeRoot(range))
        return PrStatus_Foreign;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary boundary = {node, offset};
    *out = domBoundaryCompare(boundary, range->start) < 0 ? -1 : domBoundaryCompare(boundary, range->end) > 0 ? 1 : 0;
    return PrStatus_Ok;
}

bool prDomRangeIntersectsNode(const PrDomRange* range, PrDomNode* node) {
    if (domNodeRoot(node) != domRangeRoot(range))
        return false;
    PrDomNode* parent = domNodeParent(node);
    if (parent == nullptr)
        return true;
    uint32_t offset = domNodeIndex(node);
    return domBoundaryCompare(DomBoundary{parent, offset}, range->end) < 0 &&
        domBoundaryCompare(DomBoundary{parent, offset + 1}, range->start) > 0;
}

PrRawString prDomRangeToString(const PrDomRange* range) {
    return domRangeString(range);
}

PrDomSelection* prDomDocumentSelection(PrDomDocument* document) {
    return domDocumentSelection(document);
}

PrDomDocument* prDomSelectionDocument(PrDomSelection* selection) {
    return selection->document;
}

PrDomRange* PR_NULLABLE prDomSelectionRange(PrDomSelection* selection) {
    return domSelectionRange(selection);
}

PrDomNode* PR_NULLABLE prDomSelectionAnchorNode(PrDomSelection* selection) {
    DomBoundary point;
    return domSelectionAnchor(selection, &point) ? point.node : nullptr;
}

uint32_t prDomSelectionAnchorOffset(const PrDomSelection* selection) {
    DomBoundary point;
    return domSelectionAnchor(selection, &point) ? point.offset : 0;
}

PrDomNode* PR_NULLABLE prDomSelectionFocusNode(PrDomSelection* selection) {
    DomBoundary point;
    return domSelectionFocus(selection, &point) ? point.node : nullptr;
}

uint32_t prDomSelectionFocusOffset(const PrDomSelection* selection) {
    DomBoundary point;
    return domSelectionFocus(selection, &point) ? point.offset : 0;
}

bool prDomSelectionIsCollapsed(const PrDomSelection* selection) {
    DomBoundary anchor;
    DomBoundary focus;
    if (!domSelectionAnchor(selection, &anchor))
        return true;
    domSelectionFocus(selection, &focus);
    return anchor.node == focus.node && anchor.offset == focus.offset;
}

uint32_t prDomSelectionRangeCount(const PrDomSelection* selection) {
    DomBoundary anchor;
    return domSelectionAnchor(selection, &anchor) ? 1 : 0;
}

PrDomSelectionType prDomSelectionType(const PrDomSelection* selection) {
    if (prDomSelectionRangeCount(selection) == 0)
        return PrDomSelectionType_None;
    return prDomSelectionIsCollapsed(selection) ? PrDomSelectionType_Caret : PrDomSelectionType_Range;
}

PrDomSelectionDirection prDomSelectionDirection(const PrDomSelection* selection) {
    return domSelectionRange(selection) != nullptr ? selection->direction : PrDomSelectionDirection_None;
}

PrStatus prDomSelectionGetRangeAt(PrDomSelection* selection, uint32_t index, PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (index != 0 || prDomSelectionRangeCount(selection) == 0)
        return PrStatus_OutOfRange;
    *out = domSelectionRange(selection);
    return PrStatus_Ok;
}

void prDomSelectionAddRange(PrDomSelection* selection, PrDomRange* range) {
    if (range->document == nullptr || domRangeRoot(range) != selection->document || domSelectionRange(selection) != nullptr)
        return;
    domSelectionSetRange(selection, range, PrDomSelectionDirection_None);
}

PrStatus prDomSelectionRemoveRange(PrDomSelection* selection, const PrDomRange* range) {
    if (domSelectionRange(selection) == nullptr || domSelectionRange(selection) != range)
        return PrStatus_NotFound;
    domSelectionSetRange(selection, nullptr, PrDomSelectionDirection_None);
    return PrStatus_Ok;
}

void prDomSelectionRemoveAllRanges(PrDomSelection* selection) {
    domSelectionSetRange(selection, nullptr, PrDomSelectionDirection_None);
}

PrStatus prDomSelectionCollapse(PrDomSelection* selection, PrDomNode* PR_NULLABLE node, uint32_t offset, PrDomRange* range) {
    if (node == nullptr) {
        prDomSelectionRemoveAllRanges(selection);
        return PrStatus_Ok;
    }
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok || !inSelectionDocument(selection, node))
        return status;
    selectRange(selection, range, DomBoundary{node, offset}, DomBoundary{node, offset}, PrDomSelectionDirection_None);
    return PrStatus_Ok;
}

PrStatus prDomSelectionCollapseToStart(PrDomSelection* selection, PrDomRange* range) {
    return collapseTo(selection, range, true);
}

PrStatus prDomSelectionCollapseToEnd(PrDomSelection* selection, PrDomRange* range) {
    return collapseTo(selection, range, false);
}

PrStatus prDomSelectionExtend(PrDomSelection* selection, PrDomNode* node, uint32_t offset, PrDomRange* range) {
    if (!inSelectionDocument(selection, node))
        return PrStatus_Ok;
    if (domSelectionRange(selection) == nullptr)
        return PrStatus_InvalidState;
    PrStatus status = domCheckBoundaryPoint(node, offset);
    if (status != PrStatus_Ok)
        return status;
    DomBoundary anchor;
    if (selection->direction == PrDomSelectionDirection_Backwards)
        anchor = domSelectionRange(selection)->end;
    else
        anchor = domSelectionRange(selection)->start;
    DomBoundary focus = {node, offset};
    if (domNodeRoot(node) != domRangeRoot(domSelectionRange(selection))) {
        selectRange(selection, range, focus, focus, PrDomSelectionDirection_Forwards);
        return PrStatus_Ok;
    }
    bool backwards = domBoundaryCompare(focus, anchor) < 0;
    selectRange(selection, range, backwards ? focus : anchor, backwards ? anchor : focus,
        backwards ? PrDomSelectionDirection_Backwards : PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

PrStatus prDomSelectionSetBaseAndExtent(PrDomSelection* selection, PrDomNode* anchorNode,
        uint32_t anchorOffset, PrDomNode* focusNode, uint32_t focusOffset, PrDomRange* range) {
    if (anchorOffset > domNodeLength(anchorNode) || focusOffset > domNodeLength(focusNode))
        return PrStatus_OutOfRange;
    if (!inSelectionDocument(selection, anchorNode) || !inSelectionDocument(selection, focusNode))
        return PrStatus_Ok;
    DomBoundary anchor = {anchorNode, anchorOffset};
    DomBoundary focus = {focusNode, focusOffset};
    bool backwards = domNodeRoot(anchorNode) == domNodeRoot(focusNode) && domBoundaryCompare(focus, anchor) < 0;
    selectRange(selection, range, backwards ? focus : anchor, backwards ? anchor : focus,
        backwards ? PrDomSelectionDirection_Backwards : PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

PrStatus prDomSelectionSelectAllChildren(PrDomSelection* selection, PrDomNode* node, PrDomRange* range) {
    if (node->type == PrDomNodeType_DocumentType)
        return PrStatus_WrongType;
    if (domNodeRoot(node) != selection->document)
        return PrStatus_Ok;
    uint32_t count = 0;
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        ++count;
    selectRange(selection, range, DomBoundary{node, 0}, DomBoundary{node, count}, PrDomSelectionDirection_Forwards);
    return PrStatus_Ok;
}

void prDomSelectionDeleteFromDocument(PrDomSelection* selection) {
    if (domSelectionRange(selection) != nullptr)
        prDomRangeDeleteContents(domSelectionRange(selection));
}

bool prDomSelectionContainsNode(const PrDomSelection* selection, PrDomNode* node, bool allowPartialContainment) {
    const PrDomRange* range = domSelectionRange(selection);
    if (range == nullptr || domNodeRoot(node) != selection->document || domRangeRoot(range) != selection->document)
        return false;
    DomBoundary first = {node, 0};
    DomBoundary last = {node, domNodeLength(node)};
    if (allowPartialContainment)
        return domBoundaryCompare(range->start, last) <= 0 && domBoundaryCompare(range->end, first) >= 0;
    return domBoundaryCompare(range->start, first) <= 0 && domBoundaryCompare(range->end, last) >= 0;
}

PrRawString prDomSelectionToString(const PrDomSelection* selection) {
    return domSelectionRange(selection) != nullptr ? domRangeString(domSelectionRange(selection)) : prRawStringEmpty();
}

uint32_t prDomSelectionComposedRanges(PrDomSelection* selection, PrDomShadowRoot* const PR_NONNULL* PR_NULLABLE roots,
        uint32_t count, PrDomRange* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (domSelectionRange(selection) == nullptr)
        return 0;
    // A boundary point moves out of each shadow tree that is neither one of the roots nor an ancestor of one.
    auto hostOf = [](const PrDomNode* node) -> const PrDomNode* {
        const PrDomShadowRoot* shadow = domNodeShadowRoot(domNodeRoot(node));
        return shadow != nullptr ? shadow->host : nullptr;
    };
    auto listed = [&](const PrDomNode* root) {
        for (uint32_t i = 0; i < count; ++i) {
            const PrDomNode* shadow = roots[i];
            for (const PrDomNode* at = shadow; at != nullptr; at = hostOf(at)) {
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
    DomBoundary start = rescope(domSelectionRange(selection)->start, 0);
    DomBoundary end = rescope(domSelectionRange(selection)->end, 1);
    if (start.node == nullptr || end.node == nullptr)
        return 0;
    *out = domStaticRangeCreate(start, end);
    return 1;
}

bool prDomIsValidCustomElementName(PrStringView name) {
    return domIsValidCustomElementName(name);
}

void prDomDocumentSetCustomElementRegistry(PrDomDocument* document,
        const PrDomCustomElementRegistry* PR_NULLABLE registry) {
    document->customElements = registry;
}

PrDomCustomElementState prDomElementCustomElementState(const PrDomElement* element) {
    return element->customState;
}

void* PR_NULLABLE prDomElementCustomElementDefinition(const PrDomElement* element) {
    return element->state != nullptr && element->state->custom != nullptr ? element->state->custom->definition : nullptr;
}

PrDomElement* prDomElementCreateCustom(PrDomDocument* document, PrStringView localName, void* definition) {
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, domTagFromText(localName), localName);
    element->customState = PrDomCustomElementState_Custom;
    DomElementState* state = domElementState(element);
    state->custom = allocZeroed<DomCustomElement>();
    state->custom->is = prRawStringNull();
    state->custom->definition = definition;
    state->custom->registry = document->customElements;
    if (document->customElements != nullptr)
        domCustomConstructed(document->customElements->agent, element);
    return made(element);
}

void prDomDocumentUpgradeCandidates(PrDomDocument* document, PrStringView localName, void* definition) {
    domCustomUpgradeCandidates(document, localName, definition);
}

void prDomNodeUpgrade(PrDomNode* node) {
    domCustomUpgradeTree(node);
}

PrDomShadowRoot* PR_NULLABLE prDomElementHostedShadowRoot(PrDomElement* element) {
    return domShadowRootOfHost(element);
}

void prDomElementSetCustomState(PrDomElement* element, PrStringView state, bool present) {
    if (domCustomHasState(element, state) == present)
        return;
    RawArray<PrRawString>& states = domCustomElementFromElement(element)->states;
    if (present) {
        states.push(prRawStringCopy(state));
    } else {
        for (uint32_t i = 0; i < states.count; ++i) {
            if (prRawStringView(&states.data[i]) == state) {
                prRawStringReset(&states.data[i]);
                states.erase(i);
                break;
            }
        }
    }
    domNotifyStatesChanged(element);
}

void prDomElementClearCustomStates(PrDomElement* element) {
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

void prDomAgentDestroy(PrDomAgent* PR_NULLABLE agent) {
    domAgentDestroy(agent);
}

double prDomAgentNextTaskDelay(PrDomAgent* agent) {
    return domAgentNextTaskDelay(agent);
}

bool prDomAgentIsIdle(PrDomAgent* agent) {
    return domAgentIsIdle(agent);
}

bool prDomAgentRunTask(PrDomAgent* agent) {
    return domAgentRunTask(agent);
}

bool prDomAgentSpinTask(PrDomAgent* agent) {
    return domAgentSpinTask(agent);
}

void prDomAgentPost(PrDomAgent* agent, PrDomAgentTaskFn fn, void* PR_NULLABLE context) {
    domAgentPost(agent, fn, context);
}

void prDomAgentSetWake(PrDomAgent* agent, PrDomAgentWakeFn PR_NULLABLE wake, void* PR_NULLABLE context) {
    domAgentSetWake(agent, wake, context);
}

void prDomAgentSetLayoutDone(PrDomAgent* agent, PrDomAgentLayoutDoneFn PR_NULLABLE done, void* PR_NULLABLE context) {
    agent->layoutDone = done;
    agent->layoutDoneContext = context;
}

void prDomAgentAddCheckpoint(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    domAgentAddHook(&agent->checkpoints, fn, context);
}

void prDomAgentRemoveCheckpoint(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    domAgentRemoveHook(&agent->checkpoints, fn, context);
}

void prDomAgentAddUpdate(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    domAgentAddHook(&agent->updates, fn, context);
}

void prDomAgentRemoveUpdate(PrDomAgent* agent, PrDomAgentHookFn fn, void* PR_NULLABLE context) {
    domAgentRemoveHook(&agent->updates, fn, context);
}

bool prDomAgentRun(PrDomAgent* agent, double milliseconds) {
    return domAgentRun(agent, milliseconds);
}

bool prDomAgentWait(PrDomAgent* agent, double milliseconds) {
    return domAgentWait(agent, milliseconds);
}

void prDomAgentWake(PrDomAgent* agent) {
    domAgentWake(agent);
}

void prDomAgentStop(PrDomAgent* agent) {
    domAgentStop(agent);
}

void prDomAgentReactionsPush(PrDomAgent* agent) {
    domCustomReactionsPush(agent);
}

void prDomAgentReactionsPop(PrDomAgent* agent) {
    domCustomReactionsPop(agent);
}

