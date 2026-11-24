#include "html/src/html_document.h"

#include "pr/alloc.h"
#include "pr/mathml.h"
#include "pr/memory.h"

#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "html/src/html_boxes.h"
#include "html/src/html_forms.h"
#include "html/src/html_hints.h"
#include "html/src/html_link.h"
#include "html/src/html_parser.h"
#include "html/src/html_render_blocking.h"

namespace pr_impl {
namespace {

// What HTML keeps for a document that it follows: the script-created parser of a document that no browsing context
// shows, while one is active (htmlDocumentParser).
struct HtmlDocumentState {
    HtmlParser* PR_NULLABLE parser;
    PrDomEventTarget* PR_NULLABLE formerWindow;     // not held (htmlDocumentFormerWindow)
    bool replaced;                                  // htmlDocumentReplaced

    // The elements in the document's render-blocking element set (htmlDocumentCountRenderBlocking). The mechanism's
    // hooks (kBlockingHooks) follow the document from when the set first has one, so that they cost nothing in a
    // document that never blocks rendering, as almost none does.
    uint32_t renderBlocking;
};

bool blocksRendering(void* context) {
    return static_cast<const HtmlDocumentState*>(context)->renderBlocking != 0;
}

void freeParser(HtmlDocumentState* state) {
    if (state->parser == nullptr)
        return;
    htmlParserReset(state->parser);
    prMemoryFree(state->parser);
    state->parser = nullptr;
}

// The hooks of a document's render-blocking element set (html/#render-blocking-element-set), which follow a document
// from when the set first holds an element (htmlDocumentCountRenderBlocking): its elements stop blocking as they leave
// the document or lose what made them block, and the internal resource links are processed again as the document
// changes, while the set holds any.
constexpr DomDocumentHooks kBlockingHooks = {
    .inserted = [](void* context, PrDomNode* node) {
        if (blocksRendering(context))
            htmlRenderBlockingInserted(node);
    },
    .removing = [](void* context, PrDomNode* node) {
        if (blocksRendering(context))
            htmlRenderBlockingRemoving(node);
    },
    .attributeChanged = [](void* context, PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView, PrStringView) {
            if (blocksRendering(context))
                htmlRenderBlockingAttributeChanged(element, space, name);
        },
    .readinessChanged = [](void* context, PrDomDocument* document) {
        if (blocksRendering(context))
            htmlProcessInternalResourceLinks(document);
    },
    .elementParsed = [](void* context, PrDomElement* element) {
        if (blocksRendering(context))
            htmlProcessInternalResourceLinks(element->document);
    },
};

constexpr DomDocumentHooks kHooks = {
    .inserted = [](void*, PrDomNode* node) { htmlDetailsInserted(node); },
    .attributeChanged = [](void* context, PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView oldValue, PrStringView value) {
            if (space == PrDomNamespace_None && domIsHtml(element, PrDomTag_Details))
                htmlDetailsAttributeChanged(element, name, oldValue.data != nullptr, value.data != nullptr);
            // A link's own attributes can make it block rendering. While the set holds any element, kBlockingHooks
            // follows every change.
            if (domIsHtml(element, PrDomTag_Link) && !blocksRendering(context))
                htmlRenderBlockingAttributeChanged(element, space, name);
        },
    .colorValue =
        [](void*, const PrDomElement* input, PrStringView text, PrTextBuffer* out) { htmlColorWellValue(input, text, out); },
    .linkUpdate = [](void*, PrDomElement* element, DomInsertedBy by) { htmlLinkUpdate(element, by); },
    .ownerAttributeChanged =
        [](void*, PrDomElement* element, PrStringView name, PrStringView) { htmlLinkAttributeChanged(element, name); },
    .documentCreated = [](void*, PrDomDocument* created) { htmlEnable(created); },
    .documentDestroyed = [](void* context, PrDomDocument*) {
        HtmlDocumentState* state = static_cast<HtmlDocumentState*>(context);
        freeParser(state);
        prMemoryFree(state);
    },
};

HtmlDocumentState* PR_NULLABLE stateOf(const PrDomDocument* document) {
    const DomDocumentSubscriber* subscriber = domDocumentSubscriber(document, &kHooks);
    return subscriber != nullptr ? static_cast<HtmlDocumentState*>(subscriber->context) : nullptr;
}

} // namespace

void htmlEnable(PrDomDocument* document) {
    HtmlDocumentState* state = allocZeroed<HtmlDocumentState>();
    if (!domDocumentSubscribe(document, &kHooks, state)) {
        prMemoryFree(state);
        return;
    }
    cssHintsProvide(document, PrDomNamespace_Html, htmlHints());
    cssBoxesProvide(document, PrDomNamespace_Html, htmlBoxes());
    // HTML holds MathML content (html/#mathml).
    prMathMlEnable(document);
    // The links that the document already holds take their sheets, as if the parser had just inserted them.
    for (PrDomNode* node = document->first; node != nullptr; node = domNodeNext(document, node)) {
        if (PrDomElement* element = domElementFromNode(node))
            htmlLinkUpdate(element, DomInsertedBy_Parser);
    }
}

HtmlParser* PR_NULLABLE htmlDocumentParser(PrDomDocument* document, bool start) {
    HtmlDocumentState* state = stateOf(document);
    if (state == nullptr || !start)
        return state != nullptr ? state->parser : nullptr;
    freeParser(state);
    state->parser = allocZeroed<HtmlParser>();
    htmlParserInit(state->parser, document, HtmlParseFlag_DeclarativeShadowRoots);
    htmlParserOpen(state->parser);
    return state->parser;
}

void htmlDocumentEndParser(PrDomDocument* document) {
    if (HtmlDocumentState* state = stateOf(document))
        freeParser(state);
}

PrDomEventTarget* PR_NULLABLE htmlDocumentFormerWindow(const PrDomDocument* document) {
    const HtmlDocumentState* state = stateOf(document);
    return state != nullptr ? state->formerWindow : nullptr;
}

void htmlDocumentSetFormerWindow(PrDomDocument* document, PrDomEventTarget* PR_NULLABLE window) {
    if (HtmlDocumentState* state = stateOf(document))
        state->formerWindow = window;
}

void htmlDocumentCountRenderBlocking(PrDomDocument* document, int32_t change) {
    HtmlDocumentState* state = stateOf(document);
    if (state == nullptr)
        return;
    state->renderBlocking = uint32_t(int32_t(state->renderBlocking) + change);
    // The set's hooks follow the document once it first blocks rendering, and stay, since a subscriber that leaves
    // during a change would leave the hooks that the change is calling.
    if (state->renderBlocking != 0 && domDocumentSubscriber(document, &kBlockingHooks) == nullptr)
        domDocumentSubscribe(document, &kBlockingHooks, state);
}

bool htmlDocumentHasRenderBlocking(const PrDomDocument* document) {
    const HtmlDocumentState* state = stateOf(document);
    return state != nullptr && state->renderBlocking != 0;
}

bool htmlDocumentReplaced(const PrDomDocument* document) {
    const HtmlDocumentState* state = stateOf(document);
    return state != nullptr && state->replaced;
}

void htmlDocumentSetReplaced(PrDomDocument* document) {
    if (HtmlDocumentState* state = stateOf(document))
        state->replaced = true;
}

} // namespace pr_impl
