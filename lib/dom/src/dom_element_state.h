// Element State
//
// html - https://html.spec.whatwg.org/
// selectors-4 - https://drafts.csswg.org/selectors-4/
//
// The states that the specs give an element, which selectors, focus, forms and events all read. A state follows from
// the element's attributes and tree, from the state of its document, or from what the document's host knows
// (DomDocumentHost::elementState).

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the element is an input in the Image Button state, whose type is image.
inline bool domElementIsImageButton(const PrDomElement* element) {
    return domIsHtml(element, PrDomTag_Input) &&
        domInputType(domAttributeValue(element, "type")) == PrDomInputType_Image;
}

// ---------------------------------------------------------------------------------------------------------------------
// Language and direction

// Returns the language tag that the element itself declares (html/#the-lang-and-xml:lang-attributes). This is its lang
// attribute in the XML namespace or, on an HTML or SVG element, its lang attribute in no namespace. Returns the null
// view if the element declares none.
PrStringView domElementOwnLanguage(const PrDomElement* element);

// Returns the language of the element (html/#language). This is the tag that the element or its nearest ancestor
// declares, or otherwise the default language of its document. The ancestors of a shadow root are those of its host.
// The empty string means unknown.
PrStringView domElementLanguage(const PrDomElement* element);

// Returns the document's default language (html/#pragma-set-default-language). This is the tag that the last
// content-language pragma set, or otherwise the first tag of the Content-Language header that the document was loaded
// with. Returns the empty string if there is neither.
PrStringView domDocumentLanguage(const PrDomDocument* document);

// Runs the insertion steps of a meta element (html/#attr-meta-http-equiv-content-language). A content-language pragma
// sets the default language of the document, and restyles the document if the language changes. Does nothing for any
// other element.
void domMetaConnected(PrDomElement* element);

// Finds the referrer policy that `token` names (referrer-policy/#referrer-policies), with the values of
// PrFetchReferrerPolicy. Returns false if it names none. The empty token names none.
bool domReferrerPolicyFromToken(PrStringView token, uint8_t* out);

// Returns true if the element's directionality is 'rtl' rather than 'ltr' (html/#the-directionality).
bool domElementRtl(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Form controls

// The enabled or disabled state of an element (html/#selector-enabled). An element to which neither applies is Unset.
enum {
    DomEnablement_Unset,
    DomEnablement_Enabled,
    DomEnablement_Disabled,
};
typedef uint8_t DomEnablement;

// Returns whether the element is enabled, disabled or neither.
DomEnablement domElementEnablement(const PrDomElement* element);

// Returns true if the element is disabled (html/#concept-element-disabled).
inline bool domElementDisabled(const PrDomElement* element) {
    return domElementEnablement(element) == DomEnablement_Disabled;
}

// The checked, unchecked and indeterminate states of an element (selectors-4/#checked, selectors-4/#indeterminate). A
// checkbox can be checked or unchecked and also indeterminate, and so can an unchecked radio whose group has no checked
// radio.
enum {
    DomCheckedFlag_Checked = 1 << 0,
    DomCheckedFlag_Unchecked = 1 << 1,
    DomCheckedFlag_Indeterminate = 1 << 2,
};
typedef uint8_t DomCheckedFlags;

// Returns the states of the element, or none for an element to which none applies.
DomCheckedFlags domElementCheckedness(const PrDomElement* element);

// Returns true if the element matches :default (html/#selector-default).
bool domElementDefault(const PrDomElement* element);

// The required or optional state of an element (html/#selector-required). An element to which neither applies is Unset.
enum {
    DomRequirement_Unset,
    DomRequirement_Required,
    DomRequirement_Optional,
};
typedef uint8_t DomRequirement;

// Returns whether the element is required, optional or neither.
DomRequirement domElementRequirement(const PrDomElement* element);

// Returns true if an HTML element matches :read-write (html/#selector-read-write).
bool domElementReadWrite(const PrDomElement* element);

// Returns true if the element matches :placeholder-shown (html/#selector-placeholder-shown).
bool domElementPlaceholderShown(const PrDomElement* element);

// Returns the labeled control of `label` (html/#labeled-control). It is the labelable element that the for attribute
// names or, without the attribute, the first labelable descendant. Returns null if there is none.
const PrDomElement* PR_NULLABLE domLabeledControl(const PrDomElement* label);

// The kinds of widget (html/#widgets), the elements that can have a native appearance (css-ui-4/#appearance-switching).
// A devolvable widget takes its primitive appearance once the author origin sets a property that disables native
// appearance (css-ui-4/#appearance-disabling-properties). A non-devolvable widget keeps its native appearance.
enum {
    DomWidget_None,
    DomWidget_Devolvable,
    DomWidget_NonDevolvable,
};
typedef uint8_t DomWidget;

// Returns the kind of widget that the element is. The widgets are HTML's button, input, meter, progress, select and
// textarea. An input in the Checkbox, Radio Button or Range state is non-devolvable, and every other widget is
// devolvable.
DomWidget domElementWidget(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Other elements

// Returns true if the element is defined, which means that its custom element state is uncustomized or custom
// (dom/#concept-element-defined).
bool domElementDefined(const PrDomElement* element);

// Returns true if the playback state of an HTML media element has `flag` (selectors-4/#video-state,
// selectors-4/#sound-state). Returns false for any other element.
bool domElementMediaFlag(const PrDomElement* element, DomMediaFlags flag);

// Returns the level of an h1 to h6 element, or zero for any other element (html/#heading-level).
uint32_t domElementHeadingLevel(const PrDomElement* element);

// Returns true if the element is the source of a hyperlink (html/#selector-link). Such an element is an HTML or MathML
// a element or an HTML area element with an href attribute, or an SVG a element with an href or an XLink href that is
// not inside another hyperlink. SVG makes an a element inside another hyperlink an inactive placeholder
// (SVG2/linking.html#AElement).
bool domElementLink(const PrDomElement* element);

// Returns the URL of a hyperlink (domElementLink) as written: its href, or for an SVG a element without one, its XLink
// href.
PrStringView domElementLinkHref(const PrDomElement* element);

// Returns true if the element is a link that the host reports the user has visited (html/#selector-visited).
bool domElementVisited(const PrDomElement* element);

// Returns true if the element is an input that the host has autofilled (html/#selector-autofill).
bool domElementAutofilled(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// User interaction

// Returns true if the element is the focused element, or a shadow host whose shadow tree contains the focused element
// (html/#selector-focus).
bool domElementFocused(const PrDomElement* element);

// Returns true if the element is the focused element or a shadow-including ancestor of the focused element
// (selectors-4/#focus-within-pseudo).
bool domElementFocusWithin(const PrDomElement* element);

// Returns true if the element matches :focus-visible (selectors-4/#the-focus-visible-pseudo).
bool domElementFocusVisible(const PrDomElement* element);

// Returns true if the element matches :hover (html/#selector-hover). It matches the element that the user designates
// (DomDocument::hovered), its flat-tree ancestors, and the labeled control of any label among them.
bool domElementHovered(const PrDomElement* element);

// Returns true if the element matches :active (html/#selector-active). It matches the element being activated
// (DomDocument::active), its flat-tree ancestors, and the labeled control of any label among them.
bool domElementActive(const PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
