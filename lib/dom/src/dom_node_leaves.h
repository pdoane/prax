// The node tree: the interfaces that derive CharacterData and DocumentFragment.
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

#include "dom/src/dom_node_interfaces.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A shadow root (dom/#concept-shadow-root): a document fragment that its host holds separately from its children.
struct DomShadowRoot : PrDomDocumentFragment {
    CssRoot* PR_NULLABLE style;   // sheets of the style and link elements in the shadow tree
    DomIds* PR_NULLABLE ids;      // created by the first connected element with an ID
    PrDomShadowRootMode mode;
    PrDomSlotAssignmentMode slotAssignment;
    bool delegatesFocus;
    bool clonable;
    bool serializable;
    bool declarative;                      // dom/#shadowroot-declarative
};

// A Text node, which may be a CDATA section (dom/#interface-text).
struct DomText : PrDomCharacterData {};

// A comment (dom/#interface-comment).
struct DomComment : PrDomCharacterData {};

// A processing instruction (dom/#interface-processinginstruction).
struct DomProcessingInstruction : PrDomCharacterData {
    PrString target;
};

} // namespace pr_impl

// C opaque types
struct PrDomShadowRoot : pr_impl::DomShadowRoot {};
struct PrDomText : pr_impl::DomText {};
struct PrDomComment : pr_impl::DomComment {};
struct PrDomProcessingInstruction : pr_impl::DomProcessingInstruction {};

PR_ASSUME_NONNULL_END
