// Element State
//
// html - https://html.spec.whatwg.org/
// selectors-4 - https://drafts.csswg.org/selectors-4/

#include "dom/src/dom_element_state.h"

#include "pr/count_of.h"
#include "pr/string_ops.h"
#include "pr/unicode_bidi.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_select.h"
#include "dom/src/dom_slot.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The tree

// Returns the nearest ancestor that is an HTML element of `tag`.
const PrDomElement* PR_NULLABLE htmlAncestor(const PrDomElement* element, PrDomTag tag) {
    for (const PrDomElement* at = domNodeParentElement(element); at != nullptr; at = domNodeParentElement(at)) {
        if (domIsHtml(at, tag))
            return at;
    }
    return nullptr;
}

PrDomInputType inputType(const PrDomElement* input) {
    return domInputType(domAttributeValue(input, "type"));
}

// Returns the parent of `node` in the flat tree (css-shadow-1/#flat-tree). This is the slot that the node is assigned
// to, or otherwise its shadow-including parent.
const PrDomNode* PR_NULLABLE flatTreeParent(const PrDomNode* node) {
    if (const PrDomElement* slot = domAssignedSlot(node))
        return slot;
    return domShadowIncludingParent(node);
}

// ---------------------------------------------------------------------------------------------------------------------
// Direction (html/#the-directionality)

// Returns true if html/#contained-text-auto-directionality skips `element` and its descendants.
bool skipsDirection(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Bdi:
        case PrDomTag_Script:
        case PrDomTag_Style:
        case PrDomTag_Textarea:
            return true;
        default:
            return domDir(domAttributeValue(element, "dir")) != DomDir_Undefined;
    }
}

// Returns the first strong direction of the value of a control.
PrUnicodeBidiStrong valueStrong(const PrDomElement* element) {
    PrTextBuffer value = {};
    domControlValue(element, &value);
    PrUnicodeBidiStrong strong = prUnicodeBidiFirstStrong(prTextBufferView(&value));
    prTextBufferReset(&value);
    return strong;
}

// html/#auto-directionality: returns true if the auto directionality is 'rtl'. If the auto directionality is null, the
// directionality is 'ltr'.
bool autoRtl(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Textarea))
        return valueStrong(element) == PrUnicodeBidiStrong_Right;
    if (domIsHtml(element, PrDomTag_Input)) {
        switch (inputType(element)) {
            case PrDomInputType_Hidden:
            case PrDomInputType_Text:
            case PrDomInputType_Search:
            case PrDomInputType_Tel:
            case PrDomInputType_Url:
            case PrDomInputType_Email:
            case PrDomInputType_Password:
            case PrDomInputType_Submit:
            case PrDomInputType_Reset:
            case PrDomInputType_Button:
                return valueStrong(element) == PrUnicodeBidiStrong_Right;
            default:
                break;
        }
    }

    for (const PrDomNode* node = domNodeNext(element, element); node != nullptr;) {
        if (domIsText(node)) {
            PrUnicodeBidiStrong strong =
                prUnicodeBidiFirstStrong(prRawStringView(&static_cast<const PrDomText*>(node)->data));
            if (strong != PrUnicodeBidiStrong_None)
                return strong == PrUnicodeBidiStrong_Right;
        }
        const PrDomElement* child = domElementFromNode(node);
        node = child != nullptr && skipsDirection(child) ? domNodeNextSkipping(element, node) : domNodeNext(element, node);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Form controls

// html/#attr-fieldset-disabled: returns true if a fieldset with a disabled attribute contains `element` outside the
// fieldset's first legend child.
bool inDisabledFieldset(const PrDomElement* element) {
    const PrDomElement* child = element;
    for (const PrDomElement* at = domNodeParentElement(element); at != nullptr;
            child = at, at = domNodeParentElement(at)) {
        if (!domIsHtml(at, PrDomTag_Fieldset) || !domHasAttribute(at, "disabled"))
            continue;
        const PrDomElement* legend = nullptr;
        for (const PrDomNode* node = at->first; node != nullptr && legend == nullptr; node = node->next) {
            const PrDomElement* candidate = domElementFromNode(node);
            legend = domIsHtml(candidate, PrDomTag_Legend) ? candidate : nullptr;
        }
        if (legend != child)
            return true;
    }
    return false;
}

// html/#concept-fe-disabled
bool formControlDisabled(const PrDomElement* element) {
    return domHasAttribute(element, "disabled") || inDisabledFieldset(element);
}

bool selectDisabled(const PrDomElement* element) {
    const PrDomElement* select = htmlAncestor(element, PrDomTag_Select);
    return select != nullptr && formControlDisabled(select);
}

bool isCheckable(const PrDomElement* element) {
    if (!domIsHtml(element, PrDomTag_Input))
        return false;
    PrDomInputType type = inputType(element);
    return type == PrDomInputType_Checkbox || type == PrDomInputType_Radio;
}

// html/#default-button: returns true if the button is the first submit button in tree order with the same form owner.
bool isDefaultButton(const PrDomElement* button) {
    const PrDomElement* owner = domFormOwner(button);
    if (owner == nullptr)
        return false;
    const PrDomNode* root = domNodeRoot(button);
    for (const PrDomNode* node = domNodeNext(root, root); node != button; node = domNodeNext(root, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domIsSubmitButton(element) && domFormOwner(element) == owner)
            return false;
    }
    return true;
}

// Returns true if the readonly attribute applies to an input of `type` (html/#attr-input-readonly).
bool takesReadOnly(PrDomInputType type) {
    switch (type) {
        case PrDomInputType_Text:
        case PrDomInputType_Search:
        case PrDomInputType_Url:
        case PrDomInputType_Tel:
        case PrDomInputType_Email:
        case PrDomInputType_Password:
        case PrDomInputType_Date:
        case PrDomInputType_Month:
        case PrDomInputType_Week:
        case PrDomInputType_Time:
        case PrDomInputType_DatetimeLocal:
        case PrDomInputType_Number:
            return true;
        default:
            return false;
    }
}

// html/#editing-host: returns true if the element is editable. The element itself, or its nearest ancestor with a
// contenteditable state other than Inherit, decides.
bool isEditable(const PrDomElement* element) {
    for (const PrDomElement* at = element; at != nullptr; at = domNodeParentElement(at)) {
        if (at->space != PrDomNamespace_Html)
            continue;
        switch (domContentEditable(domAttributeValue(at, "contenteditable"))) {
            case DomContentEditable_Inherit:
                break;
            case DomContentEditable_False:
                return false;
            default:
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// User interaction

// Returns true if `element` matches `pointed`, the document's element that the user designates or activates. It matches
// if `pointed` is `element` or one of its descendants in the flat tree. It also matches if `pointed` is a label, or is
// in a label, whose labeled control is `element`.
bool pointedAt(const PrDomElement* PR_NULLABLE pointed, const PrDomElement* element) {
    for (const PrDomNode* node = pointed; node != nullptr; node = flatTreeParent(node)) {
        if (node == element)
            return true;
        const PrDomElement* at = domElementFromNode(node);
        if (domIsHtml(at, PrDomTag_Label) && domLabeledControl(at) == element)
            return true;
    }
    return false;
}

// Returns true if the document's host reports `state` for the element.
bool hostState(const PrDomElement* element, PrDomHostState state) {
    const DomDocumentHost* host = element->document->host;
    return host != nullptr && host->elementState != nullptr &&
        (host->elementState(host->context, element, state) & state) != 0;
}

// Returns true if the element is an a or area element, of HTML, of SVG or of MathML (mathml-core/#the-a-element), that
// names a URL, whether or not it is a hyperlink.
bool namesLink(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area))
        return domHasAttribute(element, "href");
    if (element->space == PrDomNamespace_MathMl && element->tag == PrDomTag_A)
        return domHasAttribute(element, "href");
    return element->space == PrDomNamespace_Svg && element->tag == PrDomTag_A &&
        (domHasAttribute(element, "href") || domAttribute(element, PrDomNamespace_XLink, "href") != nullptr);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Language and direction

PrStringView domElementOwnLanguage(const PrDomElement* element) {
    // Every style reads the language, so both attributes are looked for in one pass.
    constexpr uint32_t kHash = domAttributeHash("lang");
    bool plain = element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg;
    bool lowered = domIsHtmlInHtmlDocument(element);
    const PrDomAttr* lang = nullptr;
    for (const PrDomAttr* a = element->firstAttribute; a != nullptr; a = static_cast<const PrDomAttr*>(a->next)) {
        if (a->hash != kHash)
            continue;
        PrStringView name = prRawStringView(&a->name);
        if (a->space == PrDomNamespace_Xml && name == "lang")
            return prRawStringView(&a->value);
        if (plain && lang == nullptr && a->space == PrDomNamespace_None &&
                (lowered ? prAsciiEqualsLowered(name, "lang") : name == "lang"))
            lang = a;
    }
    return lang != nullptr ? prRawStringView(&lang->value) : PrStringView{nullptr, 0};
}

PrStringView domElementLanguage(const PrDomElement* element) {
    const PrDomElement* at = element;
    while (at != nullptr) {
        PrStringView own = domElementOwnLanguage(at);
        if (own.data != nullptr)
            return own;
        const PrDomShadowRoot* shadow = domNodeShadowRoot(at->parent);
        at = shadow != nullptr ? shadow->host : domNodeParentElement(at);
    }
    return domDocumentLanguage(element->document);
}

PrStringView domDocumentLanguage(const PrDomDocument* document) {
    if (!prRawStringIsNull(&document->languagePragma))
        return prRawStringView(&document->languagePragma);
    if (!prRawStringIsNull(&document->languageHeader))
        return prRawStringView(&document->languageHeader);
    return "";
}

void domMetaConnected(PrDomElement* element) {
    // html/#meta-referrer, with the legacy keywords that it maps.
    if (domIsHtml(element, PrDomTag_Meta) && domIsConnected(element) &&
            prAsciiEqualsFold(domAttributeValue(element, "name"), "referrer")) {
        PrStringView content = prStringTrim(domAttributeValue(element, "content"));
        if (prAsciiEqualsFold(content, "never"))
            content = "no-referrer";
        else if (prAsciiEqualsFold(content, "default"))
            content = "strict-origin-when-cross-origin";
        else if (prAsciiEqualsFold(content, "always"))
            content = "unsafe-url";
        else if (prAsciiEqualsFold(content, "origin-when-crossorigin"))
            content = "origin-when-cross-origin";
        domReferrerPolicyFromToken(content, &element->document->referrerPolicy);
    }
    if (!domIsHtml(element, PrDomTag_Meta) || !domIsConnected(element) ||
            !prAsciiEqualsFold(domAttributeValue(element, "http-equiv"), "content-language"))
        return;
    // A content attribute that lists several languages sets none. One with several tokens sets the first.
    PrStringView content = domAttributeValue(element, "content");
    if (content.data == nullptr || prStringFindByte(content, ',') != content.size)
        return;
    size_t start = 0;
    while (start < content.size && prAsciiIsSpace(content.data[start]))
        ++start;
    size_t end = start;
    while (end < content.size && !prAsciiIsSpace(content.data[end]))
        ++end;
    PrStringView candidate{content.data + start, end - start};
    PrDomDocument* document = element->document;
    if (candidate.size == 0 || prRawStringView(&document->languagePragma) == candidate)
        return;
    prRawStringReplace(&document->languagePragma, candidate);
    domNotifyLanguageChanged(document);
}

bool domReferrerPolicyFromToken(PrStringView token, uint8_t* out) {
    constexpr PrStringView kTokens[] = {
        "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
        "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url",
    };
    for (uint32_t i = 0; i < countOf(kTokens); ++i) {
        if (prAsciiEqualsFold(token, kTokens[i])) {
            *out = uint8_t(i + 1);
            return true;
        }
    }
    return false;
}

// The dir attribute belongs to HTML elements, so every other element takes its parent's directionality.
bool domElementRtl(const PrDomElement* element) {
    for (const PrDomElement* at = element; at != nullptr; at = domNodeParentElement(at)) {
        if (at->space != PrDomNamespace_Html)
            continue;
        switch (domDir(domAttributeValue(at, "dir"))) {
            case DomDir_Ltr:
                return false;
            case DomDir_Rtl:
                return true;
            case DomDir_Auto:
                return autoRtl(at);
            default:
                break;
        }
        if (at->tag == PrDomTag_Bdi)
            return autoRtl(at);
        if (at->tag == PrDomTag_Input && inputType(at) == PrDomInputType_Tel)
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Form controls

DomEnablement domElementEnablement(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return DomEnablement_Unset;
    bool disabled;
    switch (element->tag) {
        case PrDomTag_Button:
        case PrDomTag_Input:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            disabled = formControlDisabled(element);
            break;
        case PrDomTag_Optgroup:
            disabled = domHasAttribute(element, "disabled") || selectDisabled(element);
            break;
        case PrDomTag_Option:
            disabled = domOptionDisabled(element) || selectDisabled(element);
            break;
        case PrDomTag_Fieldset:
            disabled = domHasAttribute(element, "disabled") || inDisabledFieldset(element);
            break;
        default:
            return DomEnablement_Unset;
    }
    return disabled ? DomEnablement_Disabled : DomEnablement_Enabled;
}

DomCheckedFlags domElementCheckedness(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Option))
        return domOptionSelected(element) ? DomCheckedFlag_Checked : DomCheckedFlag_Unchecked;
    if (domIsHtml(element, PrDomTag_Progress))
        return domHasAttribute(element, "value") ? 0 : DomCheckedFlag_Indeterminate;
    if (!domIsHtml(element, PrDomTag_Input))
        return 0;
    switch (inputType(element)) {
        case PrDomInputType_Checkbox: {
            DomCheckedFlags flags = domInputChecked(element) ? DomCheckedFlag_Checked : DomCheckedFlag_Unchecked;
            return flags | (domInputIndeterminate(element) ? DomCheckedFlag_Indeterminate : 0);
        }
        case PrDomInputType_Radio:
            if (domInputChecked(element))
                return DomCheckedFlag_Checked;
            return DomCheckedFlag_Unchecked | (domRadioGroupChecked(element) ? 0 : DomCheckedFlag_Indeterminate);
        default:
            return 0;
    }
}

bool domElementDefault(const PrDomElement* element) {
    if (isCheckable(element))
        return domHasAttribute(element, "checked");
    if (domIsHtml(element, PrDomTag_Option))
        return domHasAttribute(element, "selected");
    return domIsSubmitButton(element) && isDefaultButton(element);
}

DomRequirement domElementRequirement(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return DomRequirement_Unset;
    switch (element->tag) {
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            break;
        case PrDomTag_Input:
            switch (inputType(element)) {
                case PrDomInputType_Hidden:
                case PrDomInputType_Range:
                case PrDomInputType_Color:
                case PrDomInputType_Submit:
                case PrDomInputType_Image:
                case PrDomInputType_Reset:
                case PrDomInputType_Button:
                    return DomRequirement_Unset;
                default:
                    break;
            }
            break;
        default:
            return DomRequirement_Unset;
    }
    return domHasAttribute(element, "required") ? DomRequirement_Required : DomRequirement_Optional;
}

bool domElementReadWrite(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Input)) {
        return takesReadOnly(inputType(element)) && !domHasAttribute(element,
            "readonly") && !formControlDisabled(element);
    }
    if (domIsHtml(element, PrDomTag_Textarea))
        return !domHasAttribute(element, "readonly") && !formControlDisabled(element);
    return isEditable(element);
}

// Returns true if a placeholder attribute applies and the value is empty.
bool domElementPlaceholderShown(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html || !domHasAttribute(element, "placeholder"))
        return false;
    if (element->tag == PrDomTag_Textarea)
        return domControlValueEmpty(element);
    if (element->tag != PrDomTag_Input)
        return false;
    switch (inputType(element)) {
        case PrDomInputType_Text:
        case PrDomInputType_Search:
        case PrDomInputType_Url:
        case PrDomInputType_Tel:
        case PrDomInputType_Email:
        case PrDomInputType_Password:
        case PrDomInputType_Number:
            return domControlValueEmpty(element);
        default:
            return false;
    }
}

const PrDomElement* PR_NULLABLE domLabeledControl(const PrDomElement* label) {
    if (const PrDomAttr* named = domAttribute(label, PrDomNamespace_None, "for")) {
        const PrDomElement* control =
            domElementById(const_cast<PrDomNode*>(domNodeRoot(label)), prRawStringView(&named->value));
        return control != nullptr && domIsLabelable(control) ? control : nullptr;
    }
    for (const PrDomNode* node = domNodeNext(label, label); node != nullptr; node = domNodeNext(label, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domIsLabelable(element))
            return element;
    }
    return nullptr;
}

DomWidget domElementWidget(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return DomWidget_None;
    switch (element->tag) {
        case PrDomTag_Input:
            switch (inputType(element)) {
                case PrDomInputType_Checkbox:
                case PrDomInputType_Radio:
                case PrDomInputType_Range: return DomWidget_NonDevolvable;
                default: return DomWidget_Devolvable;
            }
        case PrDomTag_Button:
        case PrDomTag_Meter:
        case PrDomTag_Progress:
        case PrDomTag_Select:
        case PrDomTag_Textarea: return DomWidget_Devolvable;
        default: return DomWidget_None;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Other elements

bool domElementDefined(const PrDomElement* element) {
    return element->customState == PrDomCustomElementState_Uncustomized ||
        element->customState == PrDomCustomElementState_Custom;
}

bool domElementMediaFlag(const PrDomElement* element, DomMediaFlags flag) {
    if (!domIsHtml(element, PrDomTag_Audio) && !domIsHtml(element, PrDomTag_Video))
        return false;
    DomMediaFlags flags = element->state != nullptr ? element->state->media : 0;
    if ((flags & DomMediaFlag_Tracked) == 0) {
        flags = DomMediaFlag_Paused;
        if (domHasAttribute(element, "muted"))
            flags |= DomMediaFlag_Muted;
    }
    return (flags & flag) != 0;
}

uint32_t domElementHeadingLevel(const PrDomElement* element) {
    static_assert(PrDomTag_H6 - PrDomTag_H1 == 5);
    if (element->space != PrDomNamespace_Html || element->tag < PrDomTag_H1 || element->tag > PrDomTag_H6)
        return 0;
    return uint32_t(element->tag - PrDomTag_H1) + 1;
}

bool domElementLink(const PrDomElement* element) {
    if (!namesLink(element))
        return false;
    if (element->space != PrDomNamespace_Svg)
        return true;
    for (const PrDomNode* node = domShadowIncludingParent(element); node != nullptr;
            node = domShadowIncludingParent(node)) {
        const PrDomElement* ancestor = domElementFromNode(node);
        if (ancestor != nullptr && namesLink(ancestor))
            return false;
    }
    return true;
}

PrStringView domElementLinkHref(const PrDomElement* element) {
    PrStringView href = domAttributeValue(element, "href");
    if (href.data != nullptr || element->space != PrDomNamespace_Svg)
        return href;
    const PrDomAttr* xlink = domAttribute(element, PrDomNamespace_XLink, "href");
    return xlink != nullptr ? prRawStringView(&xlink->value) : PrStringView{};
}

bool domElementVisited(const PrDomElement* element) {
    return domElementLink(element) && hostState(element, PrDomHostState_Visited);
}

bool domElementAutofilled(const PrDomElement* element) {
    return domIsHtml(element, PrDomTag_Input) && hostState(element, PrDomHostState_Autofill);
}

// ---------------------------------------------------------------------------------------------------------------------
// User interaction

bool domElementFocused(const PrDomElement* element) {
    const PrDomElement* focused = element->document->focused;
    if (focused == element)
        return true;
    const PrDomShadowRoot* shadow = domShadowRootOfHost(element);
    for (const PrDomNode* node = focused; shadow != nullptr && node != nullptr; node = domShadowIncludingParent(node)) {
        if (node == shadow)
            return true;
    }
    return false;
}

bool domElementFocusWithin(const PrDomElement* element) {
    for (const PrDomNode* node = element->document->focused; node != nullptr; node = domShadowIncludingParent(node)) {
        if (node == element)
            return true;
    }
    return false;
}

bool domElementFocusVisible(const PrDomElement* element) {
    return element->document->focusVisible && element->document->focused == element;
}

bool domElementHovered(const PrDomElement* element) {
    return pointedAt(element->document->hovered, element);
}

bool domElementActive(const PrDomElement* element) {
    return pointedAt(element->document->active, element);
}

} // namespace pr_impl
