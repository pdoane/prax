// The interfaces that HTML defines over DOM objects.
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/dom.h"
#include "pr/html.h"
#include "pr/string_view.h"

#include "dom/src/dom_control.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

// C opaque types. An element in the HTML namespace, with the interface that its local name gives it.
struct PrHtmlElement : PrDomElement {};
struct PrHtmlAnchorElement : PrHtmlElement {};
struct PrHtmlAreaElement : PrHtmlElement {};
struct PrHtmlBaseElement : PrHtmlElement {};
struct PrHtmlCanvasElement : PrHtmlElement {};
struct PrHtmlDialogElement : PrHtmlElement {};
struct PrHtmlIFrameElement : PrHtmlElement {};
struct PrHtmlImageElement : PrHtmlElement {};
struct PrHtmlInputElement : PrHtmlElement {};
struct PrHtmlFormElement : PrHtmlElement {};
struct PrHtmlButtonElement : PrHtmlElement {};
struct PrHtmlTextAreaElement : PrHtmlElement {};
struct PrHtmlFieldSetElement : PrHtmlElement {};
struct PrHtmlLegendElement : PrHtmlElement {};
struct PrHtmlLabelElement : PrHtmlElement {};
struct PrHtmlOutputElement : PrHtmlElement {};
struct PrHtmlProgressElement : PrHtmlElement {};
struct PrHtmlMeterElement : PrHtmlElement {};
struct PrHtmlDetailsElement : PrHtmlElement {};
struct PrHtmlLinkElement : PrHtmlElement {};
struct PrHtmlMapElement : PrHtmlElement {};
struct PrHtmlSelectElement : PrHtmlElement {};
struct PrHtmlOptionElement : PrHtmlElement {};
struct PrHtmlDataListElement : PrHtmlElement {};
struct PrHtmlTableElement : PrHtmlElement {};
struct PrHtmlTableSectionElement : PrHtmlElement {};
struct PrHtmlTableRowElement : PrHtmlElement {};
struct PrHtmlTableCellElement : PrHtmlElement {};
struct PrHtmlMediaElement : PrHtmlElement {};
struct PrHtmlMetaElement : PrHtmlElement {};
struct PrHtmlPictureElement : PrHtmlElement {};
struct PrHtmlSourceElement : PrHtmlElement {};
struct PrHtmlSlotElement : PrHtmlElement {};
struct PrHtmlScriptElement : PrHtmlElement {};
struct PrHtmlStyleElement : PrHtmlElement {};
struct PrHtmlTemplateElement : PrHtmlElement {};

// C opaque types. The events whose kinds HTML defines.
struct PrHtmlErrorEvent : PrDomEvent {};
struct PrHtmlMessageEvent : PrDomEvent {};
struct PrHtmlPopStateEvent : PrDomEvent {};
struct PrHtmlHashChangeEvent : PrDomEvent {};
struct PrHtmlPageRevealEvent : PrDomEvent {};
struct PrHtmlPromiseRejectionEvent : PrDomEvent {};
struct PrHtmlSubmitEvent : PrDomEvent {};
struct PrHtmlFormDataEvent : PrDomEvent {};
struct PrHtmlToggleEvent : PrDomEvent {};
struct PrHtmlCommandEvent : PrDomEvent {};

// C opaque type. A FormData is its entry list.
struct PrHtmlFormData : pr_impl::DomEntryList {};

namespace pr_impl {

// Returns the object as the HTML element `tag`, or null for any other object.
template <typename Handle>
Handle* PR_NULLABLE htmlElementQuery(PrDomEventTarget* PR_NULLABLE object, PrDomTag tag) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return domIsHtml(element, tag) ? static_cast<Handle*>(element) : nullptr;
}

// Returns the natural width or height of the element's current request, or zero if it is not available.
inline uint32_t htmlImageDimension(const PrDomElement* element, bool width) {
    const DomElementState* state = element->state;
    if (state == nullptr || state->image != PrDomImageState_CompletelyAvailable)
        return 0;
    return width ? state->naturalWidth : state->naturalHeight;
}

// Sets a content attribute that an unsigned long with the default `fallback` reflects.
inline void htmlSetReflectedUnsigned(PrDomElement* element, PrStringView name, uint32_t value, uint32_t fallback = 0) {
    prHtmlElementSetReflectedInteger(static_cast<PrHtmlElement*>(element), name, PrHtmlReflectedInteger_Unsigned, value,
        fallback);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
