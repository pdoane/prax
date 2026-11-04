#include "css/src/css_inline_style.h"

#include "pr/memory.h"

#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// cssom-1/#update-style-attribute-for, for an inline style.
void updateStyleAttribute(PrCssStyleDeclaration* declaration) {
    PrDomElement* element = domElementFromNode(declaration->ownerNode);
    PrTextBuffer text = {};
    cssSerializeDeclarationBlock(&text, declaration->block);
    element->style->updatingAttribute = true;
    domSetAttribute(element, PrDomNamespace_None, "style", prTextBufferView(&text));
    element->style->updatingAttribute = false;
    prTextBufferReset(&text);
}

} // namespace

PrCssStyleDeclaration* cssElementInlineStyle(PrDomElement* element) {
    CssElementStyle* style = cssElementStyle(element);
    if (style->inlineStyle != nullptr)
        return &style->inlineStyle->declaration;

    CssInlineStyle* inlineStyle = allocZeroed<CssInlineStyle>();
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "style");
    if (attribute != nullptr)
        cssDeclarationBlockParse(&inlineStyle->block, prRawStringView(&attribute->value), nullptr, CssOrigin_Author);
    inlineStyle->declaration =
        PrCssStyleDeclaration{{nullptr, element, &inlineStyle->block, updateStyleAttribute, nullptr, nullptr, false}};
    style->inlineStyle = inlineStyle;
    return &inlineStyle->declaration;
}

void cssElementStyleAttributeChanged(PrDomElement* element, PrStringView value) {
    CssElementStyle* style = element->style;
    if (style == nullptr || style->inlineStyle == nullptr || style->updatingAttribute)
        return;
    cssDeclarationBlockParse(&style->inlineStyle->block, value, nullptr, CssOrigin_Author);
}

} // namespace pr_impl
