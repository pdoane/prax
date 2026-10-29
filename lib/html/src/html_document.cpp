#include "html/src/html_document.h"

#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "html/src/html_boxes.h"
#include "html/src/html_forms.h"
#include "html/src/html_hints.h"
#include "html/src/html_link.h"

namespace pr_impl {
namespace {

constexpr DomDocumentHooks kHooks = {
    .colorValue = [](void*, const PrDomElement* input, PrStringView text,
                     TextBuffer* out) { htmlColorWellValue(input, text, out); },
    .linkUpdate = [](void*, PrDomElement* element, DomInsertedBy by) { htmlLinkUpdate(element, by); },
    .ownerAttributeChanged =
        [](void*, PrDomElement* element, PrStringView name, PrStringView) { htmlLinkAttributeChanged(element, name); },
    .documentCreated = [](void*, PrDomDocument* created) { htmlEnable(created); },
};

} // namespace

void htmlEnable(PrDomDocument* document) {
    if (!domDocumentSubscribe(document, &kHooks, nullptr))
        return;
    cssHintsProvide(document, PrDomNamespace_Html, htmlHints());
    cssBoxesProvide(document, PrDomNamespace_Html, htmlBoxes());
    // The links that the document already holds take their sheets, as if the parser had just inserted them.
    for (PrDomNode* node = document->first; node != nullptr; node = domNodeNext(document, node)) {
        if (PrDomElement* element = domElementFromNode(node))
            htmlLinkUpdate(element, DomInsertedBy_Parser);
    }
}

} // namespace pr_impl
