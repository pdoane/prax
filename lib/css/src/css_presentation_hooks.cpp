#include "css/src/css_presentation_hooks.h"

#include "pr/css.h"

#include "css/src/css_presentation.h"
#include "css/src/css_root.h"
#include "dom/src/dom_hooks.h"

namespace pr_impl {
namespace {

PrCssPresentation* presentationOf(void* PR_NULLABLE context) {
    return static_cast<PrCssPresentation*>(context);
}

// The changes to a document that its presentation follows. The presentation follows the nodes in the document, and none
// other.
constexpr DomDocumentHooks kDomHooks = {
    .inserted = [](void* PR_NULLABLE context, PrDomNode* node) {
        if (domIsConnected(node))
            cssPresentationInserted(presentationOf(context), node);
    },
    .removing = [](void* PR_NULLABLE context, PrDomNode* node) {
        if (domIsConnected(node))
            cssPresentationRemoving(presentationOf(context), node);
    },
    .attributeChanged = [](void* PR_NULLABLE context, PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView oldValue, PrStringView value) {
            if (domIsConnected(element))
                cssPresentationAttributeChanged(presentationOf(context), element, space, name, oldValue, value);
        },
    .dataChanged = [](void* PR_NULLABLE context, PrDomNode* node) {
        if (domIsConnected(node))
            cssPresentationDataChanged(presentationOf(context), node);
    },
    .slotChanged = [](void* PR_NULLABLE context, PrDomElement* slot, PrDomNode* const PR_NONNULL* PR_NULLABLE before,
        uint32_t beforeCount) {
            if (domIsConnected(slot))
                cssPresentationSlotChanged(presentationOf(context), slot, before, beforeCount);
        },
    .shadowAttached = [](void* PR_NULLABLE context, PrDomElement* host) {
        if (domIsConnected(host))
            cssPresentationShadowAttached(presentationOf(context), host);
    },
    .wheelListenersChanged = [](void* PR_NULLABLE context, PrDomElement* element) {
        if (domIsConnected(element))
            cssPresentationWheelListenersChanged(presentationOf(context), element);
    },
    .statesChanged = [](void* PR_NULLABLE context, PrDomElement* element) {
        if (domIsConnected(element)) {
            cssPresentationStatesChanged(presentationOf(context), element);
            cssPresentationFieldChanged(presentationOf(context), element);
        }
    },
    .metaChanged = [](void* PR_NULLABLE context, PrDomDocument*) { cssPresentationMetaChanged(presentationOf(context)); },
    .resourceLoaded =
        [](void* PR_NULLABLE context, PrDomDocument*) { cssPresentationResourceLoaded(presentationOf(context)); },
};

// The functions through which CSS reaches the presentation.
constexpr CssPresentationHooks kCssHooks = {
    .styleChanged = [](void* context) { cssPresentationChanged(presentationOf(context)); },
    .ruleChanged = [](void* context, const CssRuleData* rule) { cssPresentationRuleChanged(presentationOf(context), rule); },
    .colorContext = [](void* context, const PrDomElement* element) {
        return cssPresentationColorContext(presentationOf(context), element);
    },
    .styleCurrent = [](void* context, const PrDomElement* element) {
        return cssPresentationStyleCurrent(presentationOf(context), element);
    },
    .awaitsFontRequests = [](const void* context) {
        return cssPresentationAwaitsFontRequests(static_cast<const PrCssPresentation*>(context));
    },
};

// Returns the presentation of the node's document, if the node is in the document.
PrCssPresentation* PR_NULLABLE connectedPresentation(const PrDomNode* node) {
    return domIsConnected(node) ? cssPresentationOf(node->document) : nullptr;
}

} // namespace

void cssPresentationAttach(PrCssPresentation* presentation, PrDomDocument* document) {
    prCssEnable(document);
    CssRoot* style = cssDocumentRoot(document);
    style->presentationHooks = &kCssHooks;
    style->presentation = presentation;
    domDocumentSubscribe(document, &kDomHooks, presentation);
}

void cssPresentationDetach(PrCssPresentation* presentation, PrDomDocument* document) {
    if (cssPresentationOf(document) != presentation)
        return;
    domDocumentUnsubscribe(document, &kDomHooks);
    CssRoot* style = document->style;
    style->presentationHooks = nullptr;
    style->presentation = nullptr;
}

PrCssPresentation* PR_NULLABLE cssPresentationOf(const PrDomDocument* document) {
    const DomDocumentSubscriber* subscriber = domDocumentSubscriber(document, &kDomHooks);
    return subscriber != nullptr ? presentationOf(subscriber->context) : nullptr;
}

void cssPresentationElementContentChanged(PrDomElement* element) {
    if (PrCssPresentation* presentation = connectedPresentation(element))
        cssPresentationContentChanged(presentation, element);
}

void cssPresentationStateMoved(PrDomElement* PR_NULLABLE from, PrDomElement* PR_NULLABLE to,
        const CssPseudoClass* states, uint32_t count) {
    PrDomElement* any = from != nullptr ? from : to;
    PrCssPresentation* presentation = any != nullptr ? connectedPresentation(any) : nullptr;
    if (presentation == nullptr)
        return;
    // A shadow host matches the states of an element in its shadow tree (css-shadow-1/#host-element-in-tree), so a line
    // continues past each shadow root to its host.
    auto onLine = [](const PrDomNode* element, const PrDomElement* PR_NULLABLE line) {
        for (const PrDomNode* at = line; at != nullptr; at = domShadowIncludingParent(at)) {
            if (at == element)
                return true;
        }
        return false;
    };
    for (PrDomNode* at = from; at != nullptr; at = domShadowIncludingParent(at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element != nullptr && domIsConnected(element) && !onLine(element, to))
            cssPresentationStateChanged(presentation, element, states, count);
    }
    for (PrDomNode* at = to; at != nullptr; at = domShadowIncludingParent(at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element != nullptr && !onLine(element, from))
            cssPresentationStateChanged(presentation, element, states, count);
    }
}

void cssPresentationDocumentMediaChanged(const PrDomDocument* PR_NULLABLE document) {
    if (PrCssPresentation* presentation = document != nullptr ? cssPresentationOf(document) : nullptr)
        cssPresentationMediaChanged(presentation);
}

} // namespace pr_impl
