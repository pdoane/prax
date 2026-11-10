#include "css/src/css_root.h"

#include "pr/memory.h"
#include "pr/string_ops.h"

#include "css/src/css_hints.h"
#include "css/src/css_inline_style.h"
#include "css/src/css_object.h"
#include "css/src/css_parser.h"
#include "css/src/css_selector_match.h"

namespace pr_impl {

CssElementStyle* cssElementStyle(PrDomElement* element) {
    if (element->style == nullptr)
        element->style = allocZeroed<CssElementStyle>();
    return element->style;
}

void cssElementStyleDestroy(CssElementStyle* PR_NULLABLE style) {
    if (style == nullptr)
        return;
    if (style->inlineStyle != nullptr) {
        cssDeclarationBlockReset(&style->inlineStyle->block);
        prMemoryFree(style->inlineStyle);
    }
    cssHintsDestroy(style);
    for (CssComputedDeclarations* computed : style->computed) {
        if (computed != nullptr) {
            cssDeclarationBlockReset(&computed->block);
            prMemoryFree(computed);
        }
    }
    cssStyleSheetFreeWithOwner(style->sheet);
    prMemoryFree(style);
}

CssRoot* cssDocumentRoot(PrDomDocument* document) {
    if (document->style == nullptr)
        document->style = allocZeroed<CssRoot>();
    return document->style;
}

void cssRootDestroy(CssRoot* PR_NULLABLE style) {
    if (style == nullptr)
        return;
    // The owner nodes own the sheets, and the document owns those of its processing instructions.
    style->sheets.sheets.reset();
    cssRootResetSheets(style);
    for (uint32_t i = 0; i < style->instructionSheets.count; ++i)
        cssStyleSheetLeaveOwner(style->instructionSheets.data[i]);
    style->instructionSheets.reset();
    for (uint32_t i = 0; i < style->cache.count; ++i) {
        prRawStringReset(&style->cache.data[i].text);
        cssSheetDataRelease(style->cache.data[i].data);
    }
    style->cache.reset();
    style->blocking.reset();
    for (uint32_t i = 0; i < style->registered.count; ++i) {
        CssRegisteredProperty* property = &style->registered.data[i];
        prRawStringReset(&property->name);
        prRawStringReset(&property->syntax);
        prRawStringReset(&property->initialValue);
        cssSyntaxDefinitionReset(&property->definition);
    }
    style->registered.reset();
    cssFontFaceSetDestroy(style->fonts);
    cssFontSourcesReset(&style->fontSources);
    cssHighlightRegistryDestroy(style->highlights);
    style->resizeObservers.reset();
    style->intersectionObservers.reset();
    prMemoryFree(style);
}

PrCssStyleSheetList* cssDocumentStyleSheets(PrDomDocument* document) {
    return &cssDocumentRoot(document)->sheets;
}

PrStatus cssRegisterProperty(PrDomDocument* document, PrStringView name, PrStringView syntax,
        bool inherits, PrStringView initialValue) {
    // css-variables-1/#custom-property: a custom property's name starts with two dashes.
    if (!prStringStartsWith(name, "--"))
        return PrStatus_Syntax;
    CssRoot* style = cssDocumentRoot(document);
    for (uint32_t i = 0; i < style->registered.count; ++i) {
        if (prRawStringView(&style->registered.data[i].name) == name)
            return PrStatus_NotAllowed;
    }
    CssParser parser;
    cssParserInit(&parser);
    CssRegisteredProperty property = {};
    bool valid = cssSyntaxDefinitionParse(&parser, syntax, &property.definition);
    // Only the universal syntax definition accepts no initial value.
    if (valid && initialValue.data == nullptr)
        valid = property.definition.universal;
    else if (valid)
        valid = cssSyntaxInitialValueValid(&parser, &property.definition, initialValue);
    cssParserReset(&parser);
    if (!valid) {
        cssSyntaxDefinitionReset(&property.definition);
        return PrStatus_Syntax;
    }
    property.name = prRawStringCopy(name);
    property.syntax = prRawStringCopy(syntax);
    property.initialValue = prRawStringCopy(initialValue);
    property.inherits = inherits;
    style->registered.push(property);
    return PrStatus_Ok;
}

void cssDocumentStyleChanged(const PrDomDocument* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    const CssRoot* style = document->style;
    if (style != nullptr && style->presentationHooks != nullptr)
        style->presentationHooks->styleChanged(style->presentation);
    cssFontsSourceChanged(const_cast<PrDomDocument*>(document));
}

void cssDocumentRuleChanged(const PrDomDocument* PR_NULLABLE document, const CssRuleData* rule) {
    if (document == nullptr)
        return;
    const CssRoot* style = document->style;
    if (style != nullptr && style->presentationHooks != nullptr)
        style->presentationHooks->ruleChanged(style->presentation, rule);
    cssFontsSourceChanged(const_cast<PrDomDocument*>(document));
}

namespace {

// The style attribute is the inline style's declarations. An element's hints are kept while it is out of the document
// too, and must not outlive the attribute that they were built from. An element without style data has neither to bring
// up to date, though the hints of others may read its attributes.
void attributeChanged(void* PR_NULLABLE, PrDomElement* element, DomNamespaceId space, PrStringView name, PrStringView,
        PrStringView value) {
    if (space != PrDomNamespace_None || (element->style == nullptr && !cssHintsHasReaders(element)))
        return;
    if (name == "style" && element->style != nullptr)
        cssElementStyleAttributeChanged(element, value);
    CssHintReach reach = cssHintsMapped(element, name);
    if (reach != CssHintReach_None)
        cssHintsChanged(element);
    // The hints of the elements that read the attribute, as a MathML table's attributes give its cells theirs.
    if (reach == CssHintReach_Readers)
        cssHintsEachReader(element, [](void*, PrDomElement* reader) { cssHintsChanged(reader); }, nullptr);
}

// The hints that read the place of a node in its tree, as a MathML cell's hints read its column, must not outlive it.
void structureChanged(void* PR_NULLABLE, PrDomNode* node) {
    cssHintsEachStructureReader(node, [](void*, PrDomElement* reader) { cssHintsChanged(reader); }, nullptr);
}

} // namespace

const DomDocumentHooks* cssDocumentHooks() {
    static constexpr DomDocumentHooks kHooks = {
        .inserted = structureChanged,
        .removing = structureChanged,
        .attributeChanged = attributeChanged,
        .dataChanged = structureChanged,
        // cssom-1/#dom-documentorshadowroot-adoptedstylesheets: a shadow root that moves to another document lets go of
        // its adopted sheets, which only their constructor document's trees may adopt.
        .shadowRootAdopted = [](void*, PrDomShadowRoot* shadow) {
            cssRootSetAdopted(shadow->style, shadow->document, nullptr, 0);
        },
        .languageChanged = [](void*, PrDomDocument* document) { cssDocumentStyleChanged(document); },
        .styleBlockUpdate = [](void*, PrDomElement* element, DomInsertedBy by) { cssStyleBlockUpdate(element, by); },
        .instructionInserted = [](void*, PrDomNode* node) { cssXmlStylesheetInserted(node); },
        .ownerAttributeChanged = [](void*, PrDomElement* element, PrStringView name, PrStringView value) {
            if (name == "media")
                cssOwnerMediaChanged(element, value);
        },
        .instructionRemoving = [](void*, PrDomNode* node) { cssInstructionRemoving(node); },
        .documentCreated = [](void*, PrDomDocument* created) { prCssEnable(created); },
        .elementDestroyed = [](void*, PrDomElement* element) { cssElementStyleDestroy(element->style); },
        .shadowRootDestroyed = [](void*, PrDomShadowRoot* shadow) { cssRootDestroy(shadow->style); },
        .documentDestroyed = [](void*, PrDomDocument* document) {
            cssRootDestroy(document->style);
            cssSelectorCacheDestroy(document->selectors);
        },
    };
    return &kHooks;
}

} // namespace pr_impl
