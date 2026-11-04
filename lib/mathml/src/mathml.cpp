// MathML API entry points.

#include "pr/mathml.h"

#include "pr/css.h"

#include "css/src/css_boxes.h"
#include "css/src/css_hints.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "mathml/src/mathml_boxes.h"
#include "mathml/src/mathml_hints.h"

namespace pr_impl {
namespace {

// The document hooks that MathML subscribes to (dom_hooks.h). MathML follows only the documents created from the
// document.
constexpr DomDocumentHooks kDocumentHooks = {
    .documentCreated = [](void*, PrDomDocument* created) { prMathMlEnable(created); },
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// C opaque types. An element in the MathML namespace, and its a element.
struct PrMathMlElement : PrDomElement {};
struct PrMathMlAnchorElement : PrMathMlElement {};

PrMathMlElement* PR_NULLABLE prMathMlElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    if (element == nullptr || element->space != PrDomNamespace_MathMl)
        return nullptr;
    return static_cast<PrMathMlElement*>(element);
}

PrMathMlAnchorElement* PR_NULLABLE prMathMlAnchorElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrMathMlElement* element = prMathMlElementQuery(object);
    return element != nullptr && element->tag == PrDomTag_A ? static_cast<PrMathMlAnchorElement*>(element) : nullptr;
}

void prMathMlEnable(PrDomDocument* document) {
    prCssEnable(document);
    if (!domDocumentSubscribe(document, &kDocumentHooks, nullptr))
        return;
    cssHintsProvide(document, PrDomNamespace_MathMl, mathMlHints());
    cssBoxesProvide(document, PrDomNamespace_MathMl, mathMlBoxes());
}
