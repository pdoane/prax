#include "html/src/html_render_blocking.h"

#include "pr/url.h"

#include "css/src/css_media.h"
#include "dom/src/dom_node.h"
#include "html/src/html_document.h"
#include "html/src/html_link.h"
#include "html/src/html_page.h"
#include "html/src/html_window.h"

namespace pr_impl {
namespace {

// Returns the window whose document is `document`, which keeps its render-blocking element set, or null if no window
// shows the document.
PrHtmlWindow* PR_NULLABLE windowOf(const PrDomDocument* document) {
    PrHtmlPage* page = htmlDocumentPage(document);
    return page != nullptr ? page->window : nullptr;
}

// Returns true if the document allows adding render-blocking elements (html/#allows-adding-render-blocking-elements):
// it is an HTML document whose body element is null.
bool allowsAdding(PrDomDocument* document) {
    return prRawStringView(&document->contentType) == "text/html" && domDocumentBody(document) == nullptr;
}

// Returns true if the element is on the stack of open elements of the parser of its document
// (html/#stack-of-open-elements).
bool onOpenStack(const PrDomElement* element) {
    PrHtmlPage* page = htmlDocumentPage(element->document);
    if (page == nullptr || page->parser.document != element->document)
        return false;
    for (uint32_t i = 0; i < page->parser.open.count; ++i) {
        if (page->parser.open.data[i] == element)
            return true;
    }
    return false;
}

// Returns true if the link is an expect link: an HTML link element whose rel holds expect, which creates an internal
// resource link (html/#link-type-expect).
bool isExpect(const PrDomElement* element) {
    return domIsHtml(element, PrDomTag_Link) && htmlRelHolds(element, "expect");
}

// Returns true if the link's media attribute matches its document's environment; a link without one matches.
bool mediaMatches(const PrDomElement* link) {
    PrStringView media = domAttributeValue(link, "media");
    const CssMediaEnvironment* environment = link->document->media;
    return media.data == nullptr || environment == nullptr || cssMediaQueryMatches(media, environment);
}

// Returns true if `url` is the document's URL apart from their fragments.
bool sameDocumentUrl(const PrDomDocument* document, const PrUrl* url) {
    PrRawString a = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    PrRawString b = prUrlSerialize(document->url, PrUrlSerialization_ExcludeFragment);
    bool same = prRawStringView(&a) == prRawStringView(&b);
    prRawStringReset(&a);
    prRawStringReset(&b);
    return same;
}

// Returns true if the expect link blocks rendering, as html/#process-internal-resource-link decides.
bool expectBlocks(PrDomElement* link) {
    PrDomDocument* document = link->document;
    // The checks that read no URL come first, since most links are not expect links.
    if (document->readiness != DomReadiness_Loading || !isExpect(link) || !domIsConnected(link))
        return false;
    PrUrl* url = nullptr;
    bool parsed = domDocumentParseUrl(document, domAttributeValue(link, "href"), &url) == PrStatus_Ok;
    if (!parsed || !sameDocumentUrl(document, url)) {
        prUrlDestroy(url);
        return false;
    }
    // A URL without a fragment indicates no part of the document, so it waits for nothing, as an empty href does.
    PrStringView fragment = prUrlFragment(url);
    if (fragment.data == nullptr) {
        prUrlDestroy(url);
        return false;
    }
    bool top = false;
    PrDomElement* indicated = htmlIndicatedElement(document, fragment, &top);
    prUrlDestroy(url);
    return document->readiness == DomReadiness_Loading && isExpect(link) && domIsConnected(link) &&
        windowOf(document) != nullptr && htmlPotentiallyRenderBlocking(link) && mediaMatches(link) &&
        (indicated == nullptr || onOpenStack(indicated));
}

} // namespace

bool htmlPotentiallyRenderBlocking(const PrDomElement* element, bool implicitly) {
    return implicitly || htmlTokensHold(element, "blocking", "render");
}

void htmlBlockRendering(PrDomElement* element) {
    PrHtmlWindow* window = windowOf(element->document);
    if (window == nullptr || !allowsAdding(element->document) || htmlBlocksRendering(element))
        return;
    domNodeHold(element);
    window->renderBlocking.push(element);
    htmlDocumentCountRenderBlocking(element->document, 1);
}

void htmlUnblockRendering(PrDomElement* element) {
    if (!htmlDocumentHasRenderBlocking(element->document))
        return;
    PrHtmlWindow* window = windowOf(element->document);
    if (window == nullptr)
        return;
    for (uint32_t i = 0; i < window->renderBlocking.count; ++i) {
        if (window->renderBlocking.data[i] != element)
            continue;
        window->renderBlocking.erase(i);
        htmlDocumentCountRenderBlocking(element->document, -1);
        domNodeRelease(element);
        return;
    }
}

bool htmlBlocksRendering(const PrDomElement* element) {
    const PrHtmlWindow* window = windowOf(element->document);
    if (window == nullptr)
        return false;
    for (uint32_t i = 0; i < window->renderBlocking.count; ++i) {
        if (window->renderBlocking.data[i] == element)
            return true;
    }
    return false;
}

void htmlProcessInternalResourceLink(PrDomElement* link) {
    if (!domIsHtml(link, PrDomTag_Link))
        return;
    if (expectBlocks(link))
        htmlBlockRendering(link);
    else
        htmlUnblockRendering(link);
}

void htmlProcessInternalResourceLinks(PrDomDocument* document) {
    PrHtmlWindow* window = windowOf(document);
    if (window == nullptr)
        return;
    // Processing a link may remove it from the set, so each pass reads the set as it stands.
    for (uint32_t i = window->renderBlocking.count; i-- > 0;) {
        if (i >= window->renderBlocking.count)
            continue;
        PrDomElement* element = window->renderBlocking.data[i];
        if (isExpect(element))
            htmlProcessInternalResourceLink(element);
    }
}

void htmlRenderBlockingInserted(PrDomNode* node) {
    PrHtmlWindow* window = windowOf(node->document);
    if (window == nullptr || window->renderBlocking.count == 0 || domElementFromNode(node) == nullptr)
        return;
    // The parser's own insertions wait for the element's parse to end, when the element leaves the stack.
    PrHtmlPage* page = htmlDocumentPage(node->document);
    if (page->parser.document == node->document && page->parser.inserting)
        return;
    htmlProcessInternalResourceLinks(node->document);
}

void htmlRenderBlockingRemoving(PrDomNode* node) {
    PrHtmlWindow* window = windowOf(node->document);
    if (window == nullptr)
        return;
    for (uint32_t i = window->renderBlocking.count; i-- > 0;) {
        PrDomElement* element = window->renderBlocking.data[i];
        if (domIsHostIncludingInclusiveAncestor(node, element)) {
            window->renderBlocking.erase(i);
            htmlDocumentCountRenderBlocking(element->document, -1);
            domNodeRelease(element);
        }
    }
}

void htmlRenderBlockingAttributeChanged(PrDomElement* element, DomNamespaceId space, PrStringView name) {
    if (space != PrDomNamespace_None)
        return;
    // An expect link follows its own attributes (html/#link-type-expect), and a link that is no longer one stops
    // blocking. Any other element whose blocking no longer holds render stops blocking
    // (html/#render-blocking-mechanism).
    if (domIsHtml(element, PrDomTag_Link) && (name == "href" || name == "media" || name == "rel" || name == "blocking"))
        htmlProcessInternalResourceLink(element);
    else if (name == "blocking" && !htmlPotentiallyRenderBlocking(element))
        htmlUnblockRendering(element);
    // A changed ID, or name of an a element, may make or unmake the element that an expect link indicates, unless the
    // parser is still building the element.
    if ((name == "id" || (name == "name" && domIsHtml(element, PrDomTag_A))) && !onOpenStack(element))
        htmlProcessInternalResourceLinks(element->document);
}

void htmlRenderBlockingReset(PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->renderBlocking.count; ++i) {
        htmlDocumentCountRenderBlocking(window->renderBlocking.data[i]->document, -1);
        domNodeRelease(window->renderBlocking.data[i]);
    }
    window->renderBlocking.reset();
}

} // namespace pr_impl
