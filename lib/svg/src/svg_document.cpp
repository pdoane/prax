// What SVG keeps for each document that it is enabled for.

#include "svg/src/svg_document.h"

#include "pr/svg.h"

#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_font.h"

namespace pr_impl {

namespace {

void inserted(void* PR_NULLABLE fonts, PrDomNode* node) {
    if (domIsConnected(node))
        svgDocumentFontsInserted(static_cast<SvgDocumentFonts*>(fonts), node);
}

void removing(void* PR_NULLABLE fonts, PrDomNode* node) {
    if (domIsConnected(node))
        svgDocumentFontsRemoving(static_cast<SvgDocumentFonts*>(fonts), node);
}

void attributeChanged(void* PR_NULLABLE fonts, PrDomElement* element, DomNamespaceId, PrStringView name, PrStringView,
        PrStringView) {
    svgDocumentFontsAttributeChanged(static_cast<SvgDocumentFonts*>(fonts), element, name);
}

void resourceLoaded(void* PR_NULLABLE fonts, PrDomDocument*) {
    svgDocumentFontsResourceLoaded(static_cast<SvgDocumentFonts*>(fonts));
}

void documentCreated(void* PR_NULLABLE, PrDomDocument* created) {
    prSvgEnable(created);
}

void documentDestroyed(void* PR_NULLABLE fonts, PrDomDocument*) {
    svgDocumentFontsDestroy(static_cast<SvgDocumentFonts*>(fonts));
}

// The changes to a document that SVG follows (dom_hooks.h), whose context is the document's SVG fonts.
constexpr DomDocumentHooks kDocumentHooks = {
    .inserted = inserted,
    .removing = removing,
    .attributeChanged = attributeChanged,
    .resourceLoaded = resourceLoaded,
    .documentCreated = documentCreated,
    .documentDestroyed = documentDestroyed,
};

} // namespace

bool svgDocumentFollow(PrDomDocument* document) {
    if (domDocumentSubscriber(document, &kDocumentHooks) != nullptr)
        return false;
    return domDocumentSubscribe(document, &kDocumentHooks, svgDocumentFontsCreate(document));
}

SvgDocumentFonts* svgDocumentFonts(const PrDomDocument* document) {
    prSvgEnable(const_cast<PrDomDocument*>(document));
    return static_cast<SvgDocumentFonts*>(domDocumentSubscriber(document, &kDocumentHooks)->context);
}

} // namespace pr_impl
