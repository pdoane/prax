// Custom Elements
//
// html - https://html.spec.whatwg.org/multipage/custom-elements.html
//
// The DOM's side of custom elements: the state of each element, its reaction queue, and the element queues that run the
// reactions. The definitions belong to the document's registry (PrDomCustomElementRegistry).

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_agent.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// One reaction of an element's custom element reaction queue (html/#custom-element-reaction-queue). It is an upgrade
// reaction with its definition, or a callback reaction with its arguments.
struct DomCustomReactionEntry {
    void* PR_NULLABLE upgrade;   // definition for an upgrade reaction, or null for a callback reaction
    PrDomCustomCallback callback;

    // An attribute's local name, old value, value and namespace. Each is the null string for null.
    PrString name;
    PrString oldValue;
    PrString value;
    PrString space;

    // The documents between which an adopted element moved. The reaction holds a reference to each.
    PrDomDocument* PR_NULLABLE oldDocument;
    PrDomDocument* PR_NULLABLE newDocument;
};

// The state of an element that is, or may become, a custom element.
struct DomCustomElement {
    void* PR_NULLABLE definition;                           // dom/#concept-element-custom-element-definition
    const PrDomCustomElementRegistry* PR_NULLABLE registry; // registry of the definition
    PrString is;                                            // dom/#concept-element-is-value, or the null string
    Array<DomCustomReactionEntry> reactions;                // html/#custom-element-reaction-queue
    Array<PrString> states;                                 // html/#customstateset, in the order added
};

// Creates an HTML element of `tag` and `name` whose is value is `is` (dom/#concept-create-element). If the document's
// registry defines the element and `synchronous` is set (the synchronous custom elements flag), the definition's
// constructor creates it. Otherwise a reaction upgrades it later. An element that a constructor created comes with a
// hold (domNodeHold) that keeps the script's object for it, and the caller releases the hold. Writes true to `held` if
// the element comes with a hold. Any other element belongs to its creator.
PrDomElement* domElementCreateHtml(PrDomDocument* document, PrDomTag tag, PrStringView name,
                                   PrStringView is, bool synchronous, bool* held);

// Returns true if `name` is a valid custom element name (html/#valid-custom-element-name).
bool domIsValidCustomElementName(PrStringView name);

// Runs the custom element steps when the element is connected or disconnected (dom/#concept-node-insert,
// dom/#concept-node-remove), adopted from `from` (dom/#concept-node-adopt), or has an attribute change
// (dom/#handle-attribute-changes). Null attribute values are the null view.
void domCustomConnectionChanged(PrDomElement* element);
void domCustomAdopted(PrDomElement* element, PrDomDocument* from);
void domCustomAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name,
                               PrStringView oldValue, PrStringView value);

// Tries to upgrade the element (html/#concept-try-upgrade).
void domCustomTryUpgrade(PrDomElement* element);

// Enqueues an upgrade reaction for each element of the document, in shadow-including tree order, that the definition of
// the local name `name` defines (html/#dom-customelementregistry-define).
void domCustomUpgradeCandidates(PrDomDocument* document, PrStringView name, void* definition);

// Tries to upgrade the root and its shadow-including descendants (html/#dom-customelementregistry-upgrade).
void domCustomUpgradeTree(PrDomNode* root);

// Returns the custom element state of an element that may become a custom element, creating it on first access.
DomCustomElement* domCustomElementFromElement(PrDomElement* element);

// Returns true if the element's custom state set holds `state`.
bool domCustomHasState(const PrDomElement* element, PrStringView state);

// Frees the custom element state, and drops its pending reactions.
void domCustomDestroy(DomCustomElement* PR_NULLABLE custom);

// Records an element that an HTML element constructor created (html/#html-element-constructors). The agent holds it
// until the synchronous construction that runs the constructor is done, if one is running.
void domCustomConstructed(PrDomAgent* agent, PrDomElement* element);

// Pushes or pops an element queue of the agent's reactions stack. Popping invokes the queue's reactions
// (html/#invoke-custom-element-reactions).
void domCustomReactionsPush(PrDomAgent* agent);
void domCustomReactionsPop(PrDomAgent* agent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
