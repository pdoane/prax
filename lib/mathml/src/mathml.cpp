// MathML API entry points.

#include "pr/mathml.h"

#include "dom/src/dom_node.h"

using namespace pr_impl;

// C opaque types. An element in the MathML namespace.
struct PrMathMlElement : PrDomElement {};

PrMathMlElement* PR_NULLABLE prMathMlElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return element != nullptr && element->space == PrDomNamespace_MathMl ? static_cast<PrMathMlElement*>(element) : nullptr;
}
