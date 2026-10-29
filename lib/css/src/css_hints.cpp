// Presentational hints: the cascade's origin of the hints that elements' attributes map to (css-cascade-5/#preshint).

#include "css/src/css_hints.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/text_buffer.h"

#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

// The hints that an element gives other elements, one block for each kind of element that takes them. A block is null
// if it holds no declarations.
struct CssGivenHints {
    CssDeclarationBlock* PR_NULLABLE blocks[kCssHintTakers];
    uint64_t ids[kCssHintTakers];   // the numbers that identify the blocks (cssSharedHints)
    bool built;                     // true if the blocks reflect the attributes that they read
};

namespace {

// The number of blocks of shared hints built so far. Each block's number is the count once it is built, so no two
// blocks get the same number and none gets 0.
Atomic<uint64_t> gSharedBuilt;

// Returns the provider of the hints of the element's namespace, or null if its document has none.
const CssHintProvider* PR_NULLABLE providerOf(const PrDomElement* element) {
    const CssRoot* style = element->document->style;
    return style != nullptr && element->space < kCssProviderNamespaces ? style->hintProviders[element->space] : nullptr;
}

// Returns the element that gives hints to `element`, or null if none does, and writes the kind of element that it is to
// `taker`.
const PrDomElement* PR_NULLABLE giver(const PrDomElement* element, uint32_t* taker) {
    const CssHintProvider* provider = providerOf(element);
    return provider != nullptr && provider->giver != nullptr ? provider->giver(element, taker) : nullptr;
}

// Replaces `*block` with the declarations of `text`, or with null if there are none.
void setBlock(CssDeclarationBlock* PR_NULLABLE* PR_NONNULL block, const TextBuffer& text) {
    if (text.text.count == 0) {
        if (*block != nullptr) {
            cssDeclarationBlockReset(*block);
            prMemoryFree(*block);
            *block = nullptr;
        }
        return;
    }
    if (*block == nullptr)
        *block = allocZeroed<CssDeclarationBlock>();
    cssDeclarationBlockParse(*block, textView(&text), nullptr);
}

// Builds the hints that an element gives other elements.
void buildGiven(PrDomElement* element) {
    CssElementStyle* style = cssElementStyle(element);
    if (style->given == nullptr)
        style->given = allocZeroed<CssGivenHints>();
    CssGivenHints* given = style->given;
    if (given->built)
        return;
    const CssHintProvider* provider = providerOf(element);
    for (uint32_t taker = 0; taker < kCssHintTakers; ++taker) {
        TextBuffer text{};
        if (provider != nullptr && provider->given != nullptr)
            provider->given(&text, element, taker);
        setBlock(&given->blocks[taker], text);
        given->ids[taker] = gSharedBuilt.fetchAddRelaxed(1) + 1;
        textReset(&text);
    }
    given->built = true;
}

} // namespace

void cssHintsProvide(PrDomDocument* document, DomNamespaceId space, const CssHintProvider* provider) {
    PR_ASSERT(space < kCssProviderNamespaces);
    cssDocumentRoot(document)->hintProviders[space] = provider;
}

void cssHintAdd(TextBuffer* out, PrStringView property, PrStringView value) {
    textAppend(out, property);
    textAppendChar(out, ':');
    textAppend(out, value);
    textAppendChar(out, ';');
}

void cssHintAddPixels(TextBuffer* out, PrStringView property, double pixels) {
    textAppend(out, property);
    textAppendChar(out, ':');
    cssSerializeNumber(out, pixels);
    textAppend(out, "px;");
}

CssHintReach cssHintsMapped(const PrDomElement* element, PrStringView name) {
    const CssHintProvider* provider = providerOf(element);
    return provider != nullptr ? provider->mapped(element, name) : CssHintReach_None;
}

const CssDeclarationBlock* PR_NULLABLE cssHints(PrDomElement* element) {
    const CssHintProvider* provider = providerOf(element);
    if (provider == nullptr)
        return nullptr;
    CssElementStyle* style = element->style;
    if (style != nullptr && style->hintsBuilt)
        return style->hints;
    // Most elements have no hints, which the provider tells without building them.
    if (!provider->hasOwn(element) && (style == nullptr || style->hints == nullptr))
        return nullptr;
    TextBuffer text{};
    provider->own(&text, element);
    if (text.text.count == 0 && (style == nullptr || style->hints == nullptr)) {
        textReset(&text);
        if (style != nullptr)
            style->hintsBuilt = true;
        return nullptr;
    }
    style = cssElementStyle(element);
    setBlock(&style->hints, text);
    textReset(&text);
    style->hintsBuilt = true;
    return style->hints;
}

void cssSharedHintsPrepare(PrDomElement* element) {
    uint32_t taker = 0;
    const PrDomElement* from = giver(element, &taker);
    if (from != nullptr)
        buildGiven(const_cast<PrDomElement*>(from));
}

const CssDeclarationBlock* PR_NULLABLE cssSharedHints(const PrDomElement* element, uint64_t* id) {
    uint32_t taker = 0;
    const PrDomElement* from = giver(element, &taker);
    const CssGivenHints* given = from != nullptr && from->style != nullptr ? from->style->given : nullptr;
    if (given == nullptr || !given->built || given->blocks[taker] == nullptr)
        return nullptr;
    *id = given->ids[taker];
    return given->blocks[taker];
}

void cssHintsChanged(PrDomElement* element) {
    if (element->style == nullptr)
        return;
    element->style->hintsBuilt = false;
    if (element->style->given != nullptr)
        element->style->given->built = false;
}

void cssHintsDestroy(CssElementStyle* style) {
    CssDeclarationBlock* PR_NULLABLE* blocks[kCssHintTakers + 1] = {&style->hints};
    for (uint32_t i = 0; i < kCssHintTakers && style->given != nullptr; ++i)
        blocks[i + 1] = &style->given->blocks[i];
    for (CssDeclarationBlock* PR_NULLABLE* block : blocks) {
        if (block == nullptr || *block == nullptr)
            continue;
        cssDeclarationBlockReset(*block);
        prMemoryFree(*block);
        *block = nullptr;
    }
    prMemoryFree(style->given);
    style->given = nullptr;
}

} // namespace pr_impl
