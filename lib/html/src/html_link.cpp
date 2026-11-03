#include "html/src/html_link.h"

#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/memory.h"
#include "pr/mime.h"
#include "pr/url.h"

#include "css/src/css_owner.h"
#include "css/src/css_root.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// The event that the task queued by a link's fetch fires when it runs (DomElementState::linkEvent). Fetching the sheet
// again, or disconnecting the link, replaces it.
enum {
    HtmlLinkEvent_None,
    HtmlLinkEvent_Load,
    HtmlLinkEvent_Error,
};
typedef uint8_t HtmlLinkEvent;

// The longest type of an icon link that the codec is asked about.
constexpr size_t kIconTypeMaxSize = 128;

// Returns true if the element's `rel` contains the keyword `keyword`, ASCII case-insensitively (html/#attr-link-rel).
bool relHolds(const PrDomElement* element, PrStringView keyword) {
    const PrDomAttr* rel = domAttribute(element, PrDomNamespace_None, "rel");
    if (rel == nullptr)
        return false;
    PrStringView text = prRawStringView(&rel->value);
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        size_t start = at;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        if (at != start && prAsciiEqualsFold(PrStringView{text.data + start, at - start}, keyword))
            return true;
    }
    return false;
}

// Fires the event that the latest sheet fetch of a link queued, if the link still has one. Releases the link that the
// task held.
void fireLinkEvent(void* data) {
    PrDomElement* element = static_cast<PrDomElement*>(data);
    HtmlLinkEvent pending = element->state != nullptr ? element->state->linkEvent : HtmlLinkEvent_None;
    if (pending != HtmlLinkEvent_None) {
        element->state->linkEvent = HtmlLinkEvent_None;
        domEventFire(element, pending == HtmlLinkEvent_Load ? "load" : "error");
    }
    domNodeUnhold(element);
}

// Queues the link's event, replacing any event that is queued and not yet fired. The task holds the link.
void queueLinkEvent(const DomDocumentHost* host, PrDomElement* element, HtmlLinkEvent event) {
    DomElementState* state = domElementState(element);
    if (state->linkEvent == HtmlLinkEvent_None) {
        domNodeHold(element);
        host->queueDelayingTask(host->context, fireLinkEvent, element,
            [](void* data) { domNodeUnhold(static_cast<PrDomElement*>(data)); });
    }
    state->linkEvent = event;
}

// The steps of a stylesheet link whose sheets have settled: it fires load or error (html/#link-type-stylesheet).
void linkSettled(PrDomElement* element, bool loaded) {
    queueLinkEvent(element->document->host, element, loaded ? HtmlLinkEvent_Load : HtmlLinkEvent_Error);
}

// A fetch for a link that preloads its resource (html/#link-type-preload). It holds the link (domNodeHold).
struct PreloadFetch {
    PrDomElement* element;
    uint32_t generation;            // the link's CssElementStyle::generation when the fetch started
};

void dropPreloadFetch(void* PR_NULLABLE data) {
    PreloadFetch* fetch = static_cast<PreloadFetch*>(data);
    domNodeUnhold(fetch->element);
    prMemoryFree(fetch);
}

// Queues load or error at the link once its resource is fetched, unless the link has been updated since.
void preloadFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    PreloadFetch* fetch = static_cast<PreloadFetch*>(data);
    PrDomElement* element = fetch->element;
    const DomDocumentHost* host = element->document->host;
    if (host != nullptr && element->style != nullptr && element->style->generation == fetch->generation)
        queueLinkEvent(host, element, domFetchIsOk(result) ? HtmlLinkEvent_Load : HtmlLinkEvent_Error);
    dropPreloadFetch(fetch);
}

// Writes the destination that a preload link's as attribute names (html/#attr-link-as), and returns false if it names
// none, in which case the link fetches nothing.
bool preloadDestination(const PrDomElement* element, DomFetchDestination* out) {
    PrStringView as = domAttributeValue(element, "as");
    if (prAsciiEqualsFold(as, "style"))
        *out = DomFetchDestination_Style;
    else if (prAsciiEqualsFold(as, "script"))
        *out = DomFetchDestination_Script;
    else if (prAsciiEqualsFold(as, "image"))
        *out = DomFetchDestination_Image;
    else if (prAsciiEqualsFold(as, "font"))
        *out = DomFetchDestination_Font;
    else if (prAsciiEqualsFold(as, "audio") || prAsciiEqualsFold(as, "video") || prAsciiEqualsFold(as, "track"))
        *out = DomFetchDestination_Media;
    else if (prAsciiEqualsFold(as, "fetch") || prAsciiEqualsFold(as, "document"))
        *out = DomFetchDestination_Document;
    else
        return false;
    return true;
}

// Fetches the resource of a link that preloads it (html/#link-type-preload), which fires load or error at the link once
// the fetch answers. The resource itself is not kept.
void preload(PrDomElement* element, const DomDocumentHost* host) {
    DomFetchDestination destination = DomFetchDestination_Document;
    PrStringView href = domAttributeValue(element, "href");
    if (href.size == 0 || !preloadDestination(element, &destination))
        return;
    PrUrl* url = nullptr;
    PrStatus parsed = domDocumentParseUrl(element->document, href, &url);
    if (parsed != PrStatus_Ok) {
        prUrlDestroy(url);
        queueLinkEvent(host, element, HtmlLinkEvent_Error);
        return;
    }
    PreloadFetch* fetch = allocZeroed<PreloadFetch>();
    domNodeHold(element);
    fetch->element = element;
    fetch->generation = cssElementStyle(element)->generation;
    PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(url);
    DomFetchRequest asked{
        prRawStringView(&request), destination,
        domCorsSettings(domAttributeValue(element, "crossorigin")),
        DomFetchInitiator_Link,
        domAttributeValue(element, "referrerpolicy"),
    };
    host->fetch(host->context, &asked, preloadFetched, fetch, dropPreloadFetch);
    prRawStringReset(&request);
}

} // namespace

void htmlLinkUpdate(PrDomElement* element, DomInsertedBy by) {
    if (!domIsHtml(element, PrDomTag_Link))
        return;
    PrDomDocument* document = element->document;
    // Fetching the sheet again, or not fetching it at all, drops the event that the last fetch queued, and the results
    // that it has yet to deliver.
    cssOwnerReset(element);
    if (element->state != nullptr)
        element->state->linkEvent = HtmlLinkEvent_None;
    // html/#link-type-stylesheet: the link must be a stylesheet link of a document in a browsing context, and its type,
    // if it has one, must be CSS.
    const DomDocumentHost* host = document->host;
    const PrDomAttr* href = domAttribute(element, PrDomNamespace_None, "href");
    if (host != nullptr && domIsConnected(element) && !relHolds(element, "stylesheet") && relHolds(element, "preload")) {
        preload(element, host);
        return;
    }
    // A disabled link obtains no sheet (html/#attr-link-disabled).
    if (host == nullptr || !domIsConnected(element) || !relHolds(element, "stylesheet") ||
            href == nullptr || domAttribute(element, PrDomNamespace_None, "disabled") != nullptr ||
            prRawStringIsEmpty(&href->value) || !cssOwnerTypeIsCss(element))
        return;
    PrUrl* url = nullptr;
    PrStatus parsed = domDocumentParseUrl(document, prRawStringView(&href->value), &url);
    if (parsed != PrStatus_Ok) {
        prUrlDestroy(url);
        queueLinkEvent(host, element, HtmlLinkEvent_Error);
        return;
    }
    // A link that the parser inserted blocks scripts and rendering until its sheets arrive.
    if (by == DomInsertedBy_Parser)
        cssOwnerSetBlocking(element, true);
    // The sheet decodes in the charset that the link's charset attribute names, or else its document's
    // (html/#link-type-stylesheet).
    PrCharset environment = prCharsetFromLabel(domAttributeValue(element, "charset"));
    if (environment == PrCharset_None)
        environment = document->charset;
    cssOwnerFetch(element, url, domCorsSettings(domAttributeValue(element, "crossorigin")), environment,
        relHolds(element, "alternate"), linkSettled);
    prUrlDestroy(url);
}

void htmlLinkAttributeChanged(PrDomElement* element, PrStringView name) {
    if (domIsHtml(element, PrDomTag_Link) &&
            (name == "href" || name == "rel" || name == "type" || name == "disabled" || name == "as"))
        htmlLinkUpdate(element);
}

PrRawString htmlLinkIconUrl(const PrDomDocument* document, PrImageCodec* PR_NULLABLE codec) {
    PrRawString found = prRawStringNull();
    for (const PrDomNode* node = document->first; node != nullptr; node = domNodeNext(document, node)) {
        const PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || !domIsHtml(element, PrDomTag_Link) || !relHolds(element, "icon"))
            continue;
        PrStringView href = domAttributeValue(element, "href");
        if (href.size == 0)
            continue;
        // The codec takes a type's essence in lowercase. A type longer than any image type's is not one.
        PrStringView essence = prMimeTypeEssence(domAttributeValue(element, "type"));
        if (codec != nullptr && essence.size != 0) {
            char lowered[kIconTypeMaxSize];
            if (essence.size > sizeof lowered)
                continue;
            for (size_t i = 0; i < essence.size; ++i)
                lowered[i] = prAsciiLower(essence.data[i]);
            if (!prImageCodecSupportsType(codec, PrStringView{lowered, essence.size}))
                continue;
        }
        PrUrl* url = nullptr;
        if (domDocumentParseUrl(document, href, &url) == PrStatus_Ok) {
            prRawStringReset(&found);
            found = prUrlSerialize(url, PrUrlSerialization_Whole);
        }
        prUrlDestroy(url);
    }
    return found;
}

} // namespace pr_impl
