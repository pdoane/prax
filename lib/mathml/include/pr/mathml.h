// MathML elements
//
// mathml-core - https://w3c.github.io/mathml-core/

#pragma once

#include "pr/base.h"
#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A MathML element.
typedef struct PrMathMlElement PrMathMlElement;

// ---------------------------------------------------------------------------------------------------------------------
// MathMLElement : Element (mathml-core)
//
// An element in the MathML namespace (mathml-core/#dom-mathmlelement).

// A MathMLElement as each interface it inherits from.
PR_HANDLE_UPCAST(MathMlElement, DomElement, Element)
PR_HANDLE_UPCAST(MathMlElement, DomNode, Node)
PR_HANDLE_UPCAST(MathMlElement, DomEventTarget, EventTarget)

// Returns `object` as a MathMLElement, or null if it is not one.
PrMathMlElement* PR_NULLABLE prMathMlElementQuery(PrDomEventTarget* PR_NULLABLE object);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
