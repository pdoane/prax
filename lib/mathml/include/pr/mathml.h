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

// A MathML a element.
typedef struct PrMathMlAnchorElement PrMathMlAnchorElement;

// ---------------------------------------------------------------------------------------------------------------------
// MathMLElement : Element (mathml-core)
//
// An element in the MathML namespace (mathml-core/#dom-and-javascript).

// A MathMLElement as each interface it inherits from.
PR_HANDLE_UPCAST(MathMlElement, DomElement, Element)
PR_HANDLE_UPCAST(MathMlElement, DomNode, Node)
PR_HANDLE_UPCAST(MathMlElement, DomEventTarget, EventTarget)

// Returns `object` as a MathMLElement, or null if it is not one.
PrMathMlElement* PR_NULLABLE prMathMlElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// MathMLAnchorElement : MathMLElement, with HyperlinkElementUtils (mathml-core)
//
// MathML's a element, which is a hyperlink if it has an href attribute (mathml-core/#the-a-element). Its members are
// those of HTML's a element that MathML Core names, and HTML gives it the members of HyperlinkElementUtils.

// A MathMLAnchorElement as each interface it inherits from.
PR_HANDLE_UPCAST(MathMlAnchorElement, MathMlElement, MathMlElement)
PR_HANDLE_UPCAST(MathMlAnchorElement, DomElement, Element)
PR_HANDLE_UPCAST(MathMlAnchorElement, DomNode, Node)
PR_HANDLE_UPCAST(MathMlAnchorElement, DomEventTarget, EventTarget)

// Returns `object` as a MathMLAnchorElement, or null if it is not one.
PrMathMlAnchorElement* PR_NULLABLE prMathMlAnchorElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// Enables MathML in the document, after enabling CSS (prCssEnable). The attributes of MathML elements then style them
// (mathml-core/#global-attributes), and their markup gives their boxes math layout (mathml-core/#layout-algorithms). A
// document that the DOM creates from this one, such as by DOMImplementation, has MathML too. Does nothing for a
// document that already has MathML.
void prMathMlEnable(PrDomDocument* document);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
