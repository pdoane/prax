// What HTML's elements give their boxes beyond their computed style

#include "html/src/html_boxes.h"

#include "pr/ascii.h"
#include "pr/layout_style.h"

#include "css/src/css_media.h"
#include "css/src/css_rule.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_tag.h"

namespace pr_impl {
namespace {

// Returns the value of the attribute `name` of `element` as a non-negative integer clamped to `max`, or `otherwise` if
// it does not parse or is below `least` (html/#rules-for-parsing-non-negative-integers).
uint16_t clampedAttribute(const PrDomElement* element, PrStringView name, int64_t least, int64_t max,
        uint16_t otherwise) {
    int64_t value = 0;
    if (!domParseInteger(domAttributeValue(element, name), &value) || value < least)
        return otherwise;
    return uint16_t(value < max ? value : max);
}

// Returns true if `element` is an input that renders as a one-line text control
// (html/#the-input-element-as-a-text-entry-widget): one whose type is text, search, tel, url, email or password.
bool isTextField(const PrDomElement* element) {
    PrDomInputType type = domInputType(domAttributeValue(element, "type"));
    return type == PrDomInputType_Text || type == PrDomInputType_Search || type == PrDomInputType_Tel ||
        type == PrDomInputType_Url || type == PrDomInputType_Email || type == PrDomInputType_Password;
}

// Returns true if a canvas element is replaced by its bitmap, which it is where scripting is enabled. It otherwise
// represents its fallback content (html/#the-canvas-element).
bool canvasReplaced(const PrDomElement* element) {
    const CssMediaEnvironment* media = element->document->media;
    return media != nullptr &&
        cssMediaEnvironmentKeyword(media, PrCssDescriptor_MediaScripting) == PrCssKeyword_Enabled;
}

// Returns true if display: contents makes the element generate no box at all, the same as display: none. This applies
// to an element whose box is unusual, such as a br, a replaced element or a form control (css-display-3/#unbox-html).
bool unboxes(const PrDomElement* element) {
    switch (element->tag) {
        case PrDomTag_Br: case PrDomTag_Wbr: case PrDomTag_Meter: case PrDomTag_Progress: case PrDomTag_Canvas:
        case PrDomTag_Embed: case PrDomTag_Object: case PrDomTag_Audio: case PrDomTag_Iframe: case PrDomTag_Img:
        case PrDomTag_Video: case PrDomTag_Frame: case PrDomTag_Frameset: case PrDomTag_Input: case PrDomTag_Textarea:
        case PrDomTag_Select:
            return true;
        default:
            return false;
    }
}

// The replaced elements are img, iframe, video, an object that represents an image or its content navigable
// (html/#the-object-element), and a canvas where scripting is enabled (html/#replaced-elements). A video's children are
// fallback content. An iframe has no natural dimensions, and takes the default object size. A canvas's are its
// bitmap's, which its width and height attributes give (html/#attr-canvas-width), and its children are fallback
// content. A text field's inline size is its size attribute in characters, or 20
// (html/#the-input-element-as-a-text-entry-widget). A td or th spans 1 to 1000 columns, and 0 to 65534 rows, where 0 is
// the rest of its row group (html/#attr-tdth-colspan, html/#attr-tdth-rowspan). A column or column group spans 1 to
// 1000 columns (html/#attr-col-span).
void traits(const PrDomElement* element, CssBoxTraits* out) {
    out->flags = unboxes(element) ? CssBoxTraitFlag_Unboxes : 0;
    switch (element->tag) {
        case PrDomTag_Img:
            out->kind = CssBoxKind_Replaced;
            break;
        case PrDomTag_Iframe:
            out->kind = CssBoxKind_Replaced;
            out->flags |= CssBoxTraitFlag_Sized;
            out->natural = prLayoutNaturalSizeNone();
            break;
        case PrDomTag_Object: {
            const DomElementState* state = element->state;
            if (state != nullptr && (state->image == PrDomImageState_CompletelyAvailable || state->embed != 0))
                out->kind = CssBoxKind_Replaced;
            break;
        }
        case PrDomTag_Video: {
            // The video's natural size is its resource's or its poster frame's, which its state records, and it has
            // none without either (html/#the-video-element).
            out->kind = CssBoxKind_Replaced;
            out->flags |= CssBoxTraitFlag_HoldsFallback | CssBoxTraitFlag_Sized;
            out->natural = prLayoutNaturalSizeNone();
            const DomElementState* state = element->state;
            if (state != nullptr && state->image == PrDomImageState_CompletelyAvailable) {
                out->natural.width = prLayoutUnitFromPxDouble(state->naturalWidth);
                out->natural.height = prLayoutUnitFromPxDouble(state->naturalHeight);
            }
            break;
        }
        case PrDomTag_Audio:
            // The natural size of the controls that a shown audio element has (html/#the-audio-element).
            out->kind = CssBoxKind_Replaced;
            out->flags |= CssBoxTraitFlag_HoldsFallback | CssBoxTraitFlag_Sized;
            out->natural = prLayoutNaturalSizeNone();
            out->natural.width = prLayoutUnitFromPxDouble(300);
            out->natural.height = prLayoutUnitFromPxDouble(54);
            break;
        case PrDomTag_Textarea:
            out->flags |= CssBoxTraitFlag_ChildrenAreValue;
            break;
        case PrDomTag_Details:
            // A closed details element shows only its first summary child (html/#the-details-and-summary-elements).
            if (!domHasAttribute(element, "open")) {
                out->flags |= CssBoxTraitFlag_ShowsOneChild;
                for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
                    const PrDomElement* summary = domElementFromNode(child);
                    if (summary != nullptr && domIsHtml(summary, PrDomTag_Summary)) {
                        out->shownChild = child;
                        break;
                    }
                }
            }
            break;
        case PrDomTag_Canvas:
            out->kind = canvasReplaced(element) ? CssBoxKind_Replaced : CssBoxKind_Normal;
            out->flags |= CssBoxTraitFlag_HoldsFallback | CssBoxTraitFlag_Sized;
            out->natural = prLayoutNaturalSizeNone();
            out->natural.width = prLayoutUnitFromPxDouble(domReflectedUnsigned(element, "width", 300));
            out->natural.height = prLayoutUnitFromPxDouble(domReflectedUnsigned(element, "height", 150));
            break;
        case PrDomTag_Br:
            out->kind = CssBoxKind_LineBreak;
            break;
        case PrDomTag_Button:
            out->kind = CssBoxKind_Button;
            break;
        case PrDomTag_Input:
            if (isTextField(element)) {
                out->kind = CssBoxKind_TextField;
                out->characters = clampedAttribute(element, "size", 1, 0xffff, 20);
            }
            break;
        case PrDomTag_Td:
        case PrDomTag_Th:
            out->flags |= CssBoxTraitFlag_Spans;
            out->columnSpan = clampedAttribute(element, "colspan", 1, kPrLayoutTableMaxColumnSpan, 1);
            out->rowSpan = clampedAttribute(element, "rowspan", 0, kPrLayoutTableMaxRowSpan, 1);
            break;
        case PrDomTag_Col:
        case PrDomTag_Colgroup:
            out->flags |= CssBoxTraitFlag_Spans;
            out->columnSpan = clampedAttribute(element, "span", 1, kPrLayoutTableMaxColumnSpan, 1);
            out->rowSpan = 1;
            break;
        case PrDomTag_Bdi:
            out->flags |= CssBoxTraitFlag_AutoDirection;
            break;
        default:
            break;
    }
}

bool reads(const PrDomElement* element, PrStringView name) {
    switch (element->tag) {
        case PrDomTag_Td: case PrDomTag_Th: return name == "colspan" || name == "rowspan";
        case PrDomTag_Col: case PrDomTag_Colgroup: return name == "span";
        case PrDomTag_Input: return name == "size";
        case PrDomTag_Details: return name == "open";
        default: return false;
    }
}

CssColorSchemeFlags pageSchemesOf(PrDomDocument* document) {
    return htmlPageSchemes(document);
}

constexpr CssBoxProvider kProvider = {
    .tags = kDomTagsHtmlBoxes,
    .traits = traits,
    .reads = reads,
    .paragraphs = kDomTagsHtmlParagraphs,
    .pageSchemes = pageSchemesOf,
};

} // namespace

const CssBoxProvider* htmlBoxes() {
    return &kProvider;
}

CssColorSchemeFlags htmlPageSchemes(PrDomDocument* document) {
    for (const PrDomNode* node = domNodeNext(document, document); node != nullptr; node = domNodeNext(document, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || !domIsHtml(element, PrDomTag_Meta) ||
                !asciiEqualsFold(domAttributeValue(element, "name"), "color-scheme"))
            continue;
        const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "content");
        if (attribute == nullptr)
            continue;
        PrStringView content = prStringView(&attribute->value);
        if (!cssDeclarationSupported(cssPropertyDeclaredName(PrCssProperty_ColorScheme), content))
            continue;
        CssColorSchemeFlags schemes = 0;
        for (size_t at = 0; at < content.size;) {
            while (at < content.size && asciiIsSpace(content.data[at]))
                ++at;
            size_t start = at;
            while (at < content.size && !asciiIsSpace(content.data[at]))
                ++at;
            PrStringView word{content.data + start, at - start};
            if (asciiEqualsFold(word, "light"))
                schemes |= CssColorSchemeFlag_Light;
            else if (asciiEqualsFold(word, "dark"))
                schemes |= CssColorSchemeFlag_Dark;
        }
        return schemes;
    }
    return 0;
}

} // namespace pr_impl
