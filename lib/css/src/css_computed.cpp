#include "css/src/css_computed.h"

#include "pr/memory.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_selector.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

CssComputedDeclarations* computedOf(const PrCssStyleDeclaration* declaration) {
    return reinterpret_cast<CssComputedDeclarations*>(reinterpret_cast<uintptr_t>(declaration) -
        offsetof(CssComputedDeclarations, declaration));
}

// Fills a computed style's block from the presentation of its element's document. Empties the block if the document has
// no presentation, the element is not in the document, or the style is of nothing.
void refreshComputed(const PrCssStyleDeclaration* declaration) {
    PrDomElement* element = domElementFromNode(declaration->ownerNode);
    PrCssPresentation* presentation = domIsConnected(element) ? cssPresentationOf(element->document) : nullptr;
    CssComputedDeclarations* computed = computedOf(declaration);
    if (presentation == nullptr || computed->pseudo == CssPseudo_Count) {
        cssDeclarationBlockParse(declaration->block, "", nullptr);
        computed->filler = nullptr;
        return;
    }
    cssPresentationComputedBlock(presentation, element, computed->pseudo, declaration->block, &computed->filler,
        &computed->version);
}

// Serializes one longhand or custom property of a computed style from the presentation of its element's document.
bool computedValue(const PrCssStyleDeclaration* declaration, const CssDeclaredName& name, TextBuffer* out) {
    PrDomElement* element = domElementFromNode(declaration->ownerNode);
    PrCssPresentation* presentation = domIsConnected(element) ? cssPresentationOf(element->document) : nullptr;
    CssPseudo pseudo = computedOf(declaration)->pseudo;
    if (presentation == nullptr || pseudo == CssPseudo_Count)
        return true;
    if (name.property == PrCssProperty_Count)
        return cssPresentationComputedCustomValue(presentation, element, pseudo, name.text, out);
    return cssPresentationComputedValue(presentation, element, pseudo, name.property, out);
}

// Returns what getComputedStyle's pseudo-element argument names (cssom-1/#dom-window-getcomputedstyle): the element if
// the argument does not start with a colon, CssPseudo_Count if it is not a pseudo-element selector or is ::slotted() or
// ::part(), and otherwise the pseudo-element, or the element for a pseudo-element that the cascade does not style.
CssPseudo computedTarget(PrStringView pseudoElement) {
    if (pseudoElement.size == 0 || pseudoElement.data[0] != ':')
        return CssPseudo_None;
    PrCssSelectors* selectors = cssParseDomSelectors(pseudoElement);
    if (selectors == nullptr)
        return CssPseudo_Count;
    CssPseudo pseudo = CssPseudo_Count;
    const CssComplexSelector* complex = &selectors->complex[selectors->list.first];
    if (selectors->list.count == 1 && (complex->flags & CssComplexFlag_Invalid) == 0 && complex->compoundCount == 1) {
        const CssCompoundSelector* compound = &selectors->compounds[complex->compound];
        bool only = true;
        for (uint32_t k = 0; k < compound->simpleCount && only; ++k) {
            const CssSimpleSelector* simple = &selectors->simple[compound->simple + k];
            only = simple->kind == CssSimpleKind_PseudoElement && simple->pseudoElement != CssPseudoElement_SlottedFn &&
                simple->pseudoElement != CssPseudoElement_PartFn;
        }
        if (only) {
            pseudo = cssSelectorPseudo(selectors, selectors->list.first);
            if (pseudo == CssPseudo_Count || pseudo == CssPseudo_Placeholder || pseudo == CssPseudo_Selection)
                pseudo = CssPseudo_None;
        }
    }
    cssSelectorsDestroy(selectors);
    return pseudo;
}

} // namespace

PrCssStyleDeclaration* cssElementComputedStyle(PrDomElement* element, PrStringView pseudoElement) {
    CssElementStyle* style = cssElementStyle(element);
    CssPseudo pseudo = computedTarget(pseudoElement);
    CssComputedDeclarations*& computed = style->computed[pseudo];
    if (computed == nullptr) {
        computed = allocZeroed<CssComputedDeclarations>();
        computed->declaration = PrCssStyleDeclaration{
            {nullptr, element, &computed->block, nullptr, refreshComputed, computedValue, true},
        };
        computed->pseudo = pseudo;
    }
    return &computed->declaration;
}

} // namespace pr_impl
