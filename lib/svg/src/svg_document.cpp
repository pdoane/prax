// What SVG keeps for each document that it is enabled for.

#include "svg/src/svg_document.h"

#include "pr/alloc.h"
#include "pr/memory.h"
#include "pr/svg.h"

#include "dom/src/dom_event.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_animate.h"
#include "svg/src/svg_font.h"

namespace pr_impl {

namespace {

// What SVG keeps for a document.
struct SvgDocument {
    SvgDocumentFonts* fonts;
    SvgAnimations* animations;
};

SvgDocument* documentOf(void* PR_NULLABLE context) {
    return static_cast<SvgDocument*>(context);
}

void inserted(void* PR_NULLABLE context, PrDomNode* node) {
    if (!domIsConnected(node))
        return;
    svgDocumentFontsInserted(documentOf(context)->fonts, node);
    svgAnimationsTreeChanged(documentOf(context)->animations, node);
}

void removing(void* PR_NULLABLE context, PrDomNode* node) {
    if (!domIsConnected(node))
        return;
    svgDocumentFontsRemoving(documentOf(context)->fonts, node);
    svgAnimationsRemoving(documentOf(context)->animations, node);
}

void attributeChanged(void* PR_NULLABLE context, PrDomElement* element, DomNamespaceId, PrStringView name, PrStringView,
        PrStringView) {
    svgDocumentFontsAttributeChanged(documentOf(context)->fonts, element, name);
    if (domIsConnected(element))
        svgAnimationsAttributeChanged(documentOf(context)->animations, element, name);
}

void resourceLoaded(void* PR_NULLABLE context, PrDomDocument*) {
    svgDocumentFontsResourceLoaded(documentOf(context)->fonts);
}

void documentCreated(void* PR_NULLABLE, PrDomDocument* created) {
    prSvgEnable(created);
}

void elementDestroyed(void* PR_NULLABLE context, PrDomElement* element) {
    svgAnimationsElementDestroyed(documentOf(context)->animations, element);
}

void documentDestroyed(void* PR_NULLABLE context, PrDomDocument* document);

void readinessChanged(void* PR_NULLABLE context, PrDomDocument*) {
    svgAnimationsReadinessChanged(documentOf(context)->animations);
}

// Fires load at an svg element (SVG2/interact.html#LoadEvent), which neither bubbles nor can be canceled.
void fireLoad(void* data) {
    PrDomElement* svg = static_cast<PrDomElement*>(data);
    domEventFire(svg, "load");
    domNodeRelease(svg);
}

// An svg element that a parser finished fires load, in a task of its own, whether the parser was HTML's or XML's.
void elementParsed(void* PR_NULLABLE, PrDomElement* element) {
    const DomDocumentHost* host = element->document->host;
    if (element->space != PrDomNamespace_Svg || element->tag != PrDomTag_Svg || host == nullptr)
        return;
    domNodeRetain(element);
    host->queueTask(host->context, fireLoad, element, [](void* data) { domNodeRelease(static_cast<PrDomElement*>(data)); });
}

// The changes to a document that SVG follows (dom_hooks.h), whose context is the document's SvgDocument.
constexpr DomDocumentHooks kDocumentHooks = {
    .inserted = inserted,
    .removing = removing,
    .attributeChanged = attributeChanged,
    .resourceLoaded = resourceLoaded,
    .readinessChanged = readinessChanged,
    .elementParsed = elementParsed,
    .documentCreated = documentCreated,
    .elementDestroyed = elementDestroyed,
    .documentDestroyed = documentDestroyed,
};

// The document frees its elements after it reports its own free, so SVG stops following it before it frees what it
// keeps for the document, which the reports of the elements' frees would read.
void documentDestroyed(void* PR_NULLABLE context, PrDomDocument* document) {
    domDocumentUnsubscribe(document, &kDocumentHooks);
    SvgDocument* state = documentOf(context);
    svgAnimationsDestroy(state->animations);
    svgDocumentFontsDestroy(state->fonts);
    prMemoryFree(state);
}

} // namespace

bool svgDocumentFollow(PrDomDocument* document) {
    if (domDocumentSubscriber(document, &kDocumentHooks) != nullptr)
        return false;
    SvgDocument* state = allocZeroed<SvgDocument>();
    state->fonts = svgDocumentFontsCreate(document);
    state->animations = svgAnimationsCreate(document);
    return domDocumentSubscribe(document, &kDocumentHooks, state);
}

SvgDocumentFonts* svgDocumentFonts(const PrDomDocument* document) {
    prSvgEnable(const_cast<PrDomDocument*>(document));
    return documentOf(domDocumentSubscriber(document, &kDocumentHooks)->context)->fonts;
}

SvgAnimations* PR_NULLABLE svgDocumentAnimations(const PrDomDocument* document) {
    const DomDocumentSubscriber* subscriber = domDocumentSubscriber(document, &kDocumentHooks);
    return subscriber != nullptr ? documentOf(subscriber->context)->animations : nullptr;
}

} // namespace pr_impl
