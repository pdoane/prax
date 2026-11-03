// HTML API entry points.

#include "pr/html.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/float.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/text_buffer.h"
#include "pr/url.h"

#include "css/src/css_hints.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_view.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_slot.h"
#include "html/src/html_canvas.h"
#include "html/src/html_document.h"
#include "html/src/html_focus.h"
#include "html/src/html_interface.h"
#include "html/src/html_media.h"
#include "html/src/html_media_source.h"
#include "html/src/html_message.h"
#include "html/src/html_page.h"
#include "html/src/html_parser.h"
#include "html/src/html_serialize.h"
#include "html/src/html_style.h"
#include "html/src/html_window.h"
#include "html/src/html_xml.h"
#include "mathml/src/mathml_style.h"
#include "svg/src/svg_style.h"

namespace pr_impl {

namespace {

// A document timeline (web-animations-1/#document-timelines).
struct HtmlDocumentTimeline {
    PrDomDocument* document;
    double originTime;   // web-animations-1/#origin-time, on the same clock as htmlWindowNow
};

// A message that a window posted, and the origin that its target's document must have, or the null string for any
// origin.
struct PostedMessage {
    PrHtmlWindow* target;
    PrHtmlWindow* source;
    HtmlMessage message;
    PrRawString sourceOrigin;
    PrRawString targetOrigin;
};

void dropMessage(void* data) {
    PostedMessage* posted = static_cast<PostedMessage*>(data);
    htmlMessageReset(&posted->message);
    prRawStringReset(&posted->sourceOrigin);
    prRawStringReset(&posted->targetOrigin);
    prMemoryFree(posted);
}

// html/#window-post-message-steps, in the task that those steps queue. Fires a message event at the target if the
// target's document has the origin that the message was posted to.
void deliverMessage(void* data) {
    PostedMessage* posted = static_cast<PostedMessage*>(data);
    PrRawString origin = htmlWindowOrigin(posted->target);
    bool matches = prRawStringIsNull(&posted->targetOrigin) ||
        prRawStringView(&origin) == prRawStringView(&posted->targetOrigin);
    prRawStringReset(&origin);
    if (matches) {
        htmlFireMessage(prHtmlWindowAsEventTarget(posted->target), &posted->message, posted->sourceOrigin,
            posted->source);
        posted->sourceOrigin = prRawStringNull();
    }
    dropMessage(posted);
}

// html/#merge-with-the-next-text-node: if `node` and its next sibling are Text nodes, moves the sibling's data to the
// end of `node` and removes the sibling.
void htmlMergeWithNextText(PrDomNode* node) {
    PrDomNode* next = node->next;
    if (node->type != PrDomNodeType_Text || next == nullptr || next->type != PrDomNodeType_Text)
        return;
    domAppendData(static_cast<PrDomCharacterData*>(node),
        prRawStringView(&static_cast<const PrDomCharacterData*>(next)->data));
    domRemoveFromParent(next);
}

// html/#rendered-text-fragment: creates runs of text between line breaks, and a br element for each break. CR LF counts
// as one break.
PrDomNode* renderedTextFragment(PrDomDocument* document, PrStringView input) {
    PrDomNode* fragment = domDocumentFragmentCreate(document);
    size_t at = 0;
    while (at < input.size) {
        size_t start = at;
        while (at < input.size && input.data[at] != '\n' && input.data[at] != '\r')
            ++at;
        if (at != start)
            domAttach(fragment, domTextCreate(document, PrStringView{input.data + start, at - start}), nullptr);
        while (at < input.size && (input.data[at] == '\n' || input.data[at] == '\r')) {
            at += input.data[at] == '\r' && at + 1 < input.size && input.data[at + 1] == '\n' ? 2 : 1;
            domAttach(fragment, domElementCreate(document, PrDomNamespace_Html, PrDomTag_Br, PrStringView{}), nullptr);
        }
    }
    return fragment;
}

// Parses `markup` in the context of `context` (html/#html-fragment-parsing-algorithm) and returns its children in a
// fragment of the context's document. The caller releases the fragment's one reference.
PrDomNode* parseFragment(PrDomElement* context, PrStringView markup) {
    HtmlParser parser = {};
    htmlParserInit(&parser, context->document, 0);
    PrDomNode* fragment = htmlParseFragment(&parser, context, markup);
    htmlParserReset(&parser);
    return fragment;
}

// Parses a fragment as parseFragment does in `context`, or in a body element of the document if `context` is null.
PrDomNode* parseFragmentOrBody(PrDomDocument* document, PrDomElement* PR_NULLABLE context, PrStringView markup) {
    if (context != nullptr)
        return parseFragment(context, markup);
    PrDomElement* body = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, PrStringView{});
    domNodeRetain(body);
    PrDomNode* fragment = parseFragment(body, markup);
    domNodeRelease(body);
    return fragment;
}

// The media feature that each preference overrides, and the keywords that an override may take
// (mediaqueries-5/#auto-pref).
struct PreferenceFeature {
    PrCssDescriptor feature;
    PrHtmlPreferenceValue values[3];   // the values that an override may take
    uint32_t count;
};

constexpr PreferenceFeature kPreferences[PrHtmlPreference_Count] = {
    {PrCssDescriptor_MediaPrefersColorScheme, {PrHtmlPreferenceValue_Light, PrHtmlPreferenceValue_Dark}, 2},
    {
        PrCssDescriptor_MediaPrefersContrast,
        {PrHtmlPreferenceValue_More, PrHtmlPreferenceValue_Less, PrHtmlPreferenceValue_NoPreference},
        3,
    },
    {PrCssDescriptor_MediaPrefersReducedMotion, {PrHtmlPreferenceValue_Reduce, PrHtmlPreferenceValue_NoPreference}, 2},
    {
        PrCssDescriptor_MediaPrefersReducedTransparency,
        {PrHtmlPreferenceValue_Reduce, PrHtmlPreferenceValue_NoPreference},
        2,
    },
    {PrCssDescriptor_MediaPrefersReducedData, {PrHtmlPreferenceValue_Reduce, PrHtmlPreferenceValue_NoPreference}, 2},
};

// The keyword of each value.
constexpr PrCssKeyword kPreferenceKeywords[PrHtmlPreferenceValue_Count] = {
    PrCssKeyword_Unknown, PrCssKeyword_Light, PrCssKeyword_Dark,   PrCssKeyword_NoPreference,
    PrCssKeyword_More,    PrCssKeyword_Less,  PrCssKeyword_Custom, PrCssKeyword_Reduce,
};

PrHtmlPreferenceValue preferenceValueOf(PrCssKeyword keyword) {
    for (PrHtmlPreferenceValue value = 0; value < PrHtmlPreferenceValue_Count; ++value) {
        if (kPreferenceKeywords[value] == keyword)
            return value;
    }
    return PrHtmlPreferenceValue_None;
}

const CssMediaEnvironment* environmentOf(const PrHtmlPreferenceObject* preference) {
    return &preference->window->media;
}

// Returns the state of a media element, which the window whose document is the element's document runs if there is one.
HtmlMedia* mediaOf(const PrHtmlMediaElement* mediaElement) {
    PrDomElement* element = const_cast<PrHtmlMediaElement*>(mediaElement);
    HtmlMedia* running = htmlMediaRunning(element);
    return running != nullptr ? running : htmlMediaFromElement(element);
}

constexpr PrStringView kDataPrefix = "data-";

// Writes the name of the element's custom data attribute into `out`, as its dataset exposes it. Returns false if the
// attribute is not a custom data attribute (html/#concept-domstringmap-pairs).
bool datasetName(const PrDomAttr* attribute, PrTextBuffer* out) {
    PrStringView name = prRawStringView(&attribute->name);
    if (attribute->space != PrDomNamespace_None || !prStringStartsWith(name, kDataPrefix))
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        if (prAsciiIsUpper(name.data[i]))
            return false;
    }
    prTextBufferClear(out);
    for (size_t i = kDataPrefix.size; i < name.size; ++i) {
        char c = name.data[i];
        if (c == '-' && i + 1 < name.size && prAsciiIsLower(name.data[i + 1]))
            c = prAsciiUpper(name.data[++i]);
        prTextBufferAppendChar(out, c);
    }
    return true;
}

// Writes the attribute name for a dataset name: data- is prepended, and each ASCII uppercase letter is replaced by a
// hyphen and the lowercased letter.
void attributeName(PrStringView name, PrTextBuffer* out) {
    prTextBufferAppend(out, kDataPrefix);
    for (size_t i = 0; i < name.size; ++i) {
        if (prAsciiIsUpper(name.data[i])) {
            prTextBufferAppendChar(out, '-');
            prTextBufferAppendChar(out, prAsciiLower(name.data[i]));
        } else {
            prTextBufferAppendChar(out, name.data[i]);
        }
    }
}

const PrDomAttr* PR_NULLABLE firstAttribute(const PrDomElement* element) {
    return element->firstAttribute;
}

const PrDomAttr* PR_NULLABLE nextAttribute(const PrDomAttr* attribute) {
    return static_cast<const PrDomAttr*>(attribute->next);
}

// Returns the URL of a hyperlink, its href attribute parsed against the document base URL, or null if the element has
// no href attribute or it does not parse (html/#reinitialise-url). The caller destroys the URL.
PrUrl* PR_NULLABLE hyperlinkUrl(const PrHtmlElement* element) {
    PrStringView href = domAttributeValue(element, "href");
    if (href.data == nullptr)
        return nullptr;
    PrUrl* url = nullptr;
    if (domDocumentParseUrl(element->document, href, &url) != PrStatus_Ok) {
        prUrlDestroy(url);
        url = nullptr;
    }
    return url;
}

// Sets the element that a document's pointer designates or activates, or null for none. The element must be connected
// in the document. `state` is the pseudo-class that the designated element matches.
void setPointed(PrDomElement* PR_NULLABLE* PR_NONNULL pointed, PrDomDocument* document,
        PrDomElement* PR_NULLABLE element, CssPseudoClass state) {
    PR_ASSERT(element == nullptr || (element->document == document && domIsConnected(element)));
    PrDomElement* from = *pointed;
    *pointed = element;
    if (from != element)
        cssPresentationStateMoved(from, element, &state, 1);
}

// Dispatches, as trusted, a report that the window makes of an error or a rejection. Returns true if the report was not
// canceled. A report made while it is already being dispatched is not dispatched again.
bool dispatchReport(PrHtmlWindow* window, PrDomEvent* event) {
    if (event->dispatching)
        return true;
    event->trusted = true;
    return domEventDispatch(window, event);
}

// The user agent's style sheets (html_style.h).
constexpr PrCssUserAgentSheet kUserAgentSheets[] = {
    {.source = kHtmlStyle, .conditions = 0},
    {.source = kHtmlStyleBase, .conditions = 0, .appearance = PrCssSheetAppearance_Base},
    {.source = kHtmlStyleQuirks, .conditions = PrCssSheetCondition_Quirks},
    {.source = kHtmlStyleHtmlDocument, .conditions = PrCssSheetCondition_HtmlDocument},
    {.source = kHtmlStyleMarkers, .conditions = 0},
    {.source = kHtmlStyleMarkdown, .conditions = PrCssSheetCondition_Markdown},
    {.source = kSvgStyle, .conditions = 0},
    {.source = kMathMlStyle, .conditions = 0},
};

// Returns the value of the media feature `feature` of the window, rounded to an integer, or 0 if it has none.
int32_t mediaInteger(const PrHtmlWindow* window, PrCssDescriptor feature) {
    double number = window->media.features[feature - kCssDescriptorMediaFirst].number;
    return prF64IsFinite(number) ? int32_t(prF64Floor(number + 0.5)) : 0;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrHtmlDocumentTimeline : HtmlDocumentTimeline {};

PrDomDocument* prHtmlDomParserParseFromString(PrStringView source, PrHtmlDomParserSupportedType type) {
    if (type == PrHtmlDomParserSupportedType_TextHtml) {
        PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
        prCssEnable(document);
        prSvgEnable(document);
        htmlEnable(document);
        HtmlParser parser = {};
        htmlParserInit(&parser, document, 0);
        htmlParse(&parser, source);
        htmlParserReset(&parser);
        return document;
    }
    // An XML type is parsed with scripting disabled, into a document of that content type.
    constexpr PrStringView kXmlTypes[] = {"text/xml", "application/xml", "application/xhtml+xml", "image/svg+xml"};
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Xml);
    prCssEnable(document);
    prSvgEnable(document);
    htmlEnable(document);
    document->contentType = prRawStringExternal(kXmlTypes[type - PrHtmlDomParserSupportedType_TextXml]);
    PrXmlError error = {};
    if (domXmlParse(document, source, {}, htmlXhtmlEntities(), &error) == PrStatus_Syntax)
        htmlShowXmlError(document, error);
    return document;
}

PrRawString prHtmlInnerHtml(const PrDomNode* node) {
    PrTextBuffer text = {};
    htmlSerializeChildren(&text, node);
    return prTextBufferTake(&text);
}

void prHtmlSetInnerHtml(PrDomNode* node, PrStringView markup) {
    PrDomShadowRoot* shadow = domNodeShadowRoot(node);
    PrDomElement* context = shadow != nullptr ? shadow->host : domElementFromNode(node);
    PrDomNode* fragment = parseFragment(context, markup);
    PrDomNode* target = node;
    if (domIsHtml(context, PrDomTag_Template) && shadow == nullptr)
        target = domElementContents(context);
    domReplaceAll(target, fragment);
    domNodeRelease(fragment);
}

PrRawString prHtmlOuterHtml(const PrDomElement* element) {
    PrTextBuffer text = {};
    htmlSerializeNode(&text, element);
    return prTextBufferTake(&text);
}

PrStatus prHtmlSetOuterHtml(PrDomElement* element, PrStringView markup) {
    PrDomNode* parent = domNodeParent(element);
    if (parent == nullptr)
        return PrStatus_Ok;
    if (parent->type == PrDomNodeType_Document)
        return PrStatus_NotAllowed;
    // If the parent is a document fragment, the markup is parsed as the contents of a body element.
    PrDomNode* fragment = parseFragmentOrBody(element->document, domElementFromNode(parent), markup);
    PrStatus status = domReplace(parent, fragment, element);
    domNodeRelease(fragment);
    return status;
}

PrStatus prHtmlInsertAdjacentHtml(PrDomElement* element, PrDomInsertPosition where, PrStringView markup) {
    bool outside = where == PrDomInsertPosition_BeforeBegin || where == PrDomInsertPosition_AfterEnd;
    PrDomNode* context = outside ? domNodeParent(element) : element;
    if (context == nullptr || context->type == PrDomNodeType_Document)
        return PrStatus_NotAllowed;
    // If the context is not an element, or is an html element, the markup is parsed as the contents of a body element.
    PrDomElement* contextElement = domElementFromNode(context);
    if (domIsHtml(contextElement, PrDomTag_Html))
        contextElement = nullptr;
    PrDomNode* fragment = parseFragmentOrBody(element->document, contextElement, markup);
    PrStatus status = PrStatus_Ok;
    switch (where) {
        case PrDomInsertPosition_BeforeBegin: status = domPreInsert(context, fragment, element); break;
        case PrDomInsertPosition_AfterBegin: status = domPreInsert(element, fragment, element->first); break;
        case PrDomInsertPosition_BeforeEnd: status = domPreInsert(element, fragment, nullptr); break;
        default: status = domPreInsert(context, fragment, element->next); break;
    }
    domNodeRelease(fragment);
    return status;
}

void prHtmlFocus(PrDomElement* element, const PrHtmlFocusOptions* PR_NULLABLE options) {
    PrHtmlFocusOptions values = options != nullptr ? *options : prHtmlFocusOptionsDefault();
    HtmlFocusIndication indication = HtmlFocusIndication_Hidden;
    if (!values.hasFocusVisible)
        indication = HtmlFocusIndication_Script;
    else if (values.focusVisible)
        indication = HtmlFocusIndication_Shown;
    htmlFocus(element, indication);
}

void prHtmlBlur(PrDomElement* element) {
    htmlBlur(element);
}

int32_t prHtmlTabIndex(const PrDomElement* element) {
    return htmlTabIndex(element);
}

void prHtmlSetTabIndex(PrDomElement* element, int32_t value) {
    PrTextBuffer text = {};
    if (value < 0)
        prTextBufferAppendChar(&text, '-');
    prTextBufferAppendU64(&text, value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value), 10);
    domSetAttribute(element, PrDomNamespace_None, "tabindex", prTextBufferView(&text));
    prTextBufferReset(&text);
}

PrHtmlWindow* PR_NULLABLE prHtmlDocumentDefaultView(PrDomDocument* document) {
    PrDomEventTarget* global = document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return nullptr;
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(global);
    return window->document == document ? window : nullptr;
}

PrStringView prHtmlDocumentReferrer(PrDomDocument* document) {
    PrHtmlWindow* window = prHtmlDocumentDefaultView(document);
    if (window == nullptr || prRawStringIsNull(&window->referrer))
        return "";
    return prRawStringView(&window->referrer);
}

PrRawString prHtmlWindowCreateObjectUrl(PrHtmlWindow* window, const PrFetchBlob* blob) {
    PrHtmlPage* page = htmlDocumentPage(window->document);
    PrFetchNetwork* fetcher = page != nullptr ? page->host.fetcher : nullptr;
    PrRawString origin = htmlWindowOrigin(window);
    PrRawString url = prRawStringEmpty();
    if (fetcher != nullptr)
        url = prFetchBlobUrlCreate(fetcher, blob, prRawStringView(&origin));
    prRawStringReset(&origin);
    if (fetcher != nullptr)
        window->objectUrls.push(prRawStringClone(&url));
    return url;
}

void prHtmlWindowRevokeObjectUrl(PrHtmlWindow* window, PrStringView url) {
    htmlMediaSourceRevokeUrl(window, url);
    PrHtmlPage* page = htmlDocumentPage(window->document);
    if (page == nullptr || page->host.fetcher == nullptr)
        return;
    PrRawString origin = htmlWindowOrigin(window);
    prFetchBlobUrlRevoke(page->host.fetcher, url, prRawStringView(&origin));
    prRawStringReset(&origin);
}

PrStatus prHtmlWindowSendBeacon(PrHtmlWindow* window, PrStringView url, PrByteView body, PrStringView contentType, bool* queued) {
    // beacon/#sec-processing-model: the keepalive limit of fetch/#http-network-or-cache-fetch.
    constexpr size_t kBeaconMaxBytes = 64 * 1024;
    *queued = false;
    PrUrl* base = domDocumentBaseUrl(window->document);
    PrUrl* parsed = nullptr;
    PrStatus status = prUrlCreate(url, base, PrCharset_Utf8, &parsed);
    prUrlDestroy(base);
    if (status != PrStatus_Ok)
        return PrStatus_InvalidData;
    bool http = prUrlScheme(parsed) == "http" || prUrlScheme(parsed) == "https";
    prUrlDestroy(parsed);
    if (!http)
        return PrStatus_InvalidData;
    PrHtmlPage* page = htmlDocumentPage(window->document);
    if (page == nullptr || body.size > kBeaconMaxBytes)
        return PrStatus_Ok;
    PrFetchHeader header{"Content-Type", contentType};
    bool safelisted = contentType.data == nullptr || prFetchIsCorsSafelistedRequestHeader("Content-Type", contentType);
    PrFetchClientRequest request{
        .url = url,
        .method = "POST",
        .headers = contentType.data != nullptr ? &header : nullptr,
        .headerCount = contentType.data != nullptr ? 1u : 0u,
        .body = body,
        .mode = PrFetchMode(safelisted ? PrFetchMode_NoCors : PrFetchMode_Cors),
        .credentials = PrFetchCredentials_Include,
        .redirect = PrFetchRedirect_Follow,
        .initiator = PrFetchInitiator_Beacon,
    };
    PrFetchResponseFn ignore = [](void* PR_NULLABLE, const PrFetchResponse*) {};
    status = prHtmlPageFetch(page, &request, ignore, nullptr);
    *queued = status == PrStatus_Ok;
    return status;
}

PrStringView prHtmlDocumentBodyAttribute(PrDomDocument* document, PrStringView name) {
    PrDomElement* body = domDocumentBody(document);
    PrStringView value = domIsHtml(body, PrDomTag_Body) ? domAttributeValue(body, name) : PrStringView{};
    return value.data != nullptr ? value : "";
}

void prHtmlDocumentSetBodyAttribute(PrDomDocument* document, PrStringView name, PrStringView value) {
    PrDomElement* body = domDocumentBody(document);
    if (domIsHtml(body, PrDomTag_Body))
        domSetAttribute(body, PrDomNamespace_None, name, value);
}

PrDomElement* PR_NULLABLE prHtmlDocumentCurrentScript(PrDomDocument* document) {
    PrHtmlWindow* window = prHtmlDocumentDefaultView(document);
    return window != nullptr ? window->currentScript : nullptr;
}

PrStatus prHtmlDocumentOpen(PrDomDocument* document) {
    return htmlDocumentOpen(document);
}

PrStatus prHtmlDocumentWrite(PrDomDocument* document, PrStringView text) {
    return htmlDocumentWrite(document, text);
}

PrStatus prHtmlDocumentClose(PrDomDocument* document) {
    return htmlDocumentClose(document);
}

PrHtmlDocumentReadyState prHtmlDocumentReadyState(const PrDomDocument* document) {
    switch (document->readiness) {
        case DomReadiness_Loading:     return PrHtmlDocumentReadyState_Loading;
        case DomReadiness_Interactive: return PrHtmlDocumentReadyState_Interactive;
        default:                       return PrHtmlDocumentReadyState_Complete;
    }
}

PrDomHtmlCollection* prHtmlDocumentCollection(PrDomDocument* document, PrHtmlDocumentCollection kind) {
    DomCollectionFilter filter = DomCollectionFilter_Images;
    switch (kind) {
        case PrHtmlDocumentCollection_Images: filter = DomCollectionFilter_Images; break;
        case PrHtmlDocumentCollection_Embeds: filter = DomCollectionFilter_Embeds; break;
        case PrHtmlDocumentCollection_Links: filter = DomCollectionFilter_Links; break;
        case PrHtmlDocumentCollection_Forms: filter = DomCollectionFilter_Forms; break;
        case PrHtmlDocumentCollection_Scripts: filter = DomCollectionFilter_Scripts; break;
        case PrHtmlDocumentCollection_Anchors: filter = DomCollectionFilter_Anchors; break;
        default: PR_ASSERT(false); break;
    }
    return domHtmlCollectionCreate(document, filter, {});
}

bool prHtmlDocumentHasFocus(PrDomDocument* document) {
    return htmlHasFocus(document);
}

PrHtmlVisibility prHtmlDocumentVisibilityState(const PrDomDocument* document) {
    const PrHtmlPage* page = htmlDocumentPage(document);
    return page != nullptr ? page->visibility : PrHtmlVisibility_Hidden;
}

PrDomElement* PR_NULLABLE prHtmlActiveElement(PrDomNode* root) {
    return htmlActiveElement(root);
}

void prHtmlDocumentSetHover(PrDomDocument* document, PrDomElement* PR_NULLABLE element) {
    PrDomDocument* data = document;
    setPointed(&data->hovered, data, element, CssPseudoClass_Hover);
}

void prHtmlDocumentSetActive(PrDomDocument* document, PrDomElement* PR_NULLABLE element) {
    PrDomDocument* data = document;
    setPointed(&data->active, data, element, CssPseudoClass_Active);
}

PrHtmlElement* PR_NULLABLE prHtmlElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return element != nullptr && element->space == PrDomNamespace_Html ? static_cast<PrHtmlElement*>(element) : nullptr;
}

PrHtmlDir prHtmlElementDir(const PrHtmlElement* element) {
    // html/#dom-dir reflects the attribute, limited to its known values.
    switch (domDir(domAttributeValue(element, "dir"))) {
        case DomDir_Ltr: return PrHtmlDir_Ltr;
        case DomDir_Rtl: return PrHtmlDir_Rtl;
        case DomDir_Auto: return PrHtmlDir_Auto;
        default: return PrHtmlDir_None;
    }
}

void prHtmlElementSetDir(PrHtmlElement* element, PrStringView value) {
    domSetAttribute(element, PrDomNamespace_None, "dir", value);
}

PrHtmlHidden prHtmlElementHidden(const PrHtmlElement* element) {
    const PrDomAttr* hidden = domAttribute(element, PrDomNamespace_None, "hidden");
    if (hidden == nullptr)
        return PrHtmlHidden_NotHidden;
    if (prAsciiEqualsFold(prRawStringView(&hidden->value), "until-found"))
        return PrHtmlHidden_UntilFound;
    return PrHtmlHidden_Hidden;
}

void prHtmlElementSetHidden(PrHtmlElement* element, PrHtmlHidden value) {
    if (value == PrHtmlHidden_NotHidden)
        domRemoveAttribute(element, PrDomNamespace_None, "hidden");
    else
        domSetAttribute(element, PrDomNamespace_None, "hidden", value == PrHtmlHidden_UntilFound ? "until-found" : "");
}

PrRawString prHtmlElementInnerText(const PrHtmlElement* element) {
    PrHtmlElement* mutableElement = const_cast<PrHtmlElement*>(element);
    // html/#get-the-text-steps: the descendant text content of an element that is not being rendered.
    return cssViewIsRendered(mutableElement) ? cssViewRenderedText(mutableElement) : domNodeTextContent(element);
}

void prHtmlElementSetInnerText(PrHtmlElement* element, PrStringView value) {
    domReplaceAll(element, renderedTextFragment(element->document, value));
}

PrStatus prHtmlElementSetOuterText(PrHtmlElement* element, PrStringView value) {
    PrDomNode* parent = element->parent;
    if (parent == nullptr)
        return PrStatus_NotAllowed;
    PrDomNode* next = element->next;
    PrDomNode* previous = element->previous;
    PrDomNode* fragment = renderedTextFragment(element->document, value);
    // html/#dom-outertext: a value with no text leaves an empty text node in the element's place.
    if (fragment->first == nullptr)
        domAttach(fragment, domTextCreate(element->document, ""), nullptr);
    PrStatus status = domReplace(parent, fragment, element);
    if (status != PrStatus_Ok)
        return status;
    // Text nodes next to the new content merge with it (html/#merge-with-the-next-text-node).
    if (next != nullptr && next->previous != nullptr)
        htmlMergeWithNextText(next->previous);
    if (previous != nullptr)
        htmlMergeWithNextText(previous);
    return PrStatus_Ok;
}

void prHtmlElementClick(PrHtmlElement* element) {
    DomElementState* state = domElementState(element);
    if (state->clicking || domElementDisabled(element))
        return;
    PrDomMouseEventInit init = prDomMouseEventInitDefault();
    init.ui.event.bubbles = true;
    init.ui.event.cancelable = true;
    init.ui.event.composed = true;
    PrDomMouseEvent* event = prDomMouseEventCreate("click", &init);
    domNodeRetain(element);
    state->clicking = true;
    prDomEventTargetDispatchEvent(prDomElementAsEventTarget(element), prDomMouseEventAsEvent(event), nullptr);
    state->clicking = false;
    domNodeRelease(element);
    prDomEventRelease(prDomMouseEventAsEvent(event));
}

PrStringView prHtmlContentEditableKeyword(PrHtmlContentEditable state) {
    constexpr PrStringView kKeywords[] = {"inherit", "true", "false", "plaintext-only"};
    return kKeywords[state];
}

bool prHtmlContentEditableFromKeyword(PrStringView keyword, PrHtmlContentEditable* out) {
    for (PrHtmlContentEditable state = PrHtmlContentEditable_Inherit; state <= PrHtmlContentEditable_PlaintextOnly; ++state) {
        if (prAsciiEqualsFold(keyword, prHtmlContentEditableKeyword(state))) {
            *out = state;
            return true;
        }
    }
    return false;
}

PrHtmlContentEditable prHtmlElementContentEditable(const PrHtmlElement* element) {
    PrStringView value = domAttributeValue(element, "contenteditable");
    if (value.data == nullptr)
        return PrHtmlContentEditable_Inherit;
    if (value.size == 0)
        return PrHtmlContentEditable_True;
    PrHtmlContentEditable state = PrHtmlContentEditable_Inherit;
    prHtmlContentEditableFromKeyword(value, &state);
    return state;
}

void prHtmlElementSetContentEditable(PrHtmlElement* element, PrHtmlContentEditable value) {
    if (value == PrHtmlContentEditable_Inherit)
        domRemoveAttribute(element, PrDomNamespace_None, "contenteditable");
    else
        domSetAttribute(element, PrDomNamespace_None, "contenteditable", prHtmlContentEditableKeyword(value));
}

bool prHtmlElementIsContentEditable(const PrHtmlElement* element) {
    for (const PrDomElement* at = element; at != nullptr; at = domNodeParentElement(at)) {
        if (at->space != PrDomNamespace_Html)
            continue;
        switch (prHtmlElementContentEditable(static_cast<const PrHtmlElement*>(at))) {
            case PrHtmlContentEditable_True:
            case PrHtmlContentEditable_PlaintextOnly:
                return true;
            case PrHtmlContentEditable_False:
                return false;
            default:
                break;
        }
    }
    return false;
}

bool prHtmlElementDraggable(const PrHtmlElement* element) {
    PrStringView value = domAttributeValue(element, "draggable");
    if (prAsciiEqualsFold(value, "true"))
        return true;
    if (prAsciiEqualsFold(value, "false"))
        return false;
    return domIsHtml(element, PrDomTag_Img) || (domIsHtml(element, PrDomTag_A) && domHasAttribute(element, "href"));
}

void prHtmlElementSetDraggable(PrHtmlElement* element, bool value) {
    domSetAttribute(element, PrDomNamespace_None, "draggable", value ? "true" : "false");
}

PrDomTokenList* prHtmlElementRelList(PrHtmlElement* element) {
    // The keywords that the engine acts on, which is what feature detection asks about (html/#linkTypes).
    static constexpr PrStringView kLinkTokens[] = {"alternate", "icon", "preload", "stylesheet"};
    static constexpr PrStringView kHyperlinkTokens[] = {"noopener", "noreferrer", "opener"};
    PrDomTokenList* list = domTokenListCreate(element, "rel");
    bool link = domIsHtml(element, PrDomTag_Link);
    list->supported = link ? kLinkTokens : kHyperlinkTokens;
    list->supportedCount = uint32_t(link ? countOf(kLinkTokens) : countOf(kHyperlinkTokens));
    return list;
}

PrDomElement* PR_NULLABLE prHtmlElementOffsetParent(PrHtmlElement* element) {
    return cssViewOffsetParent(element);
}

int32_t prHtmlElementOffsetTop(PrHtmlElement* element) {
    return int32_t(prF64Floor(cssViewOffsetRect(element).y + 0.5));
}

int32_t prHtmlElementOffsetLeft(PrHtmlElement* element) {
    return int32_t(prF64Floor(cssViewOffsetRect(element).x + 0.5));
}

int32_t prHtmlElementOffsetWidth(PrHtmlElement* element) {
    return int32_t(prF64Floor(cssViewOffsetRect(element).width + 0.5));
}

int32_t prHtmlElementOffsetHeight(PrHtmlElement* element) {
    return int32_t(prF64Floor(cssViewOffsetRect(element).height + 0.5));
}

PrRawString prHtmlElementReflectedUrl(const PrHtmlElement* element, PrStringView name) {
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, name);
    if (attribute == nullptr)
        return prRawStringEmpty();
    PrUrl* url = nullptr;
    PrRawString out;
    if (domDocumentParseUrl(element->document, prRawStringView(&attribute->value), &url) == PrStatus_Ok)
        out = prUrlSerialize(url, PrUrlSerialization_Whole);
    else
        out = prRawStringClone(&attribute->value);
    prUrlDestroy(url);
    return out;
}

int64_t prHtmlElementReflectedInteger(const PrHtmlElement* element, PrStringView name, PrHtmlReflectedInteger kind,
        int64_t fallback) {
    int64_t value = 0;
    int64_t least = kind == PrHtmlReflectedInteger_Long ? -2147483648ll : 0;
    if (!domParseInteger(domAttributeValue(element, name), &value) || value < least || value > 2147483647 ||
            (kind == PrHtmlReflectedInteger_Positive && value == 0))
        return fallback;
    return value;
}

PrStatus prHtmlElementSetReflectedInteger(PrHtmlElement* element, PrStringView name, PrHtmlReflectedInteger kind,
        int64_t value, int64_t fallback) {
    if ((kind == PrHtmlReflectedInteger_NonNegative && value < 0) || (kind == PrHtmlReflectedInteger_Positive && value == 0))
        return PrStatus_OutOfRange;
    PrTextBuffer text = {};
    int64_t least = kind == PrHtmlReflectedInteger_Long ? -2147483648ll : 0;
    int64_t set = value >= least && value <= 2147483647 ? value : fallback;
    if (set < 0)
        prTextBufferAppendChar(&text, '-');
    prTextBufferAppendU64(&text, uint64_t(set < 0 ? -set : set), 10);
    domSetAttribute(element, PrDomNamespace_None, name, prTextBufferView(&text));
    prTextBufferReset(&text);
    return PrStatus_Ok;
}

uint32_t prHtmlDatasetNames(const PrDomElement* element, PrRawString* PR_NULLABLE items, uint32_t capacity) {
    uint32_t count = 0;
    PrTextBuffer name = {};
    for (const PrDomAttr* attribute = firstAttribute(element);
            attribute != nullptr; attribute = nextAttribute(attribute)) {
        if (!datasetName(attribute, &name))
            continue;
        if (count < capacity)
            items[count] = prRawStringCopy(prTextBufferView(&name));
        ++count;
    }
    prTextBufferReset(&name);
    return count;
}

PrStringView prHtmlDatasetGet(const PrDomElement* element, PrStringView name) {
    PrStringView value = {};
    PrTextBuffer candidate = {};
    for (const PrDomAttr* attribute = firstAttribute(element);
            attribute != nullptr; attribute = nextAttribute(attribute)) {
        if (datasetName(attribute, &candidate) && prTextBufferView(&candidate) == name) {
            value = prRawStringView(&attribute->value);
            break;
        }
    }
    prTextBufferReset(&candidate);
    return value;
}

PrStatus prHtmlDatasetSet(PrDomElement* element, PrStringView name, PrStringView value) {
    for (size_t i = 0; i + 1 < name.size; ++i) {
        if (name.data[i] == '-' && prAsciiIsLower(name.data[i + 1]))
            return PrStatus_Syntax;
    }
    PrTextBuffer attribute = {};
    attributeName(name, &attribute);
    PrStatus status = PrStatus_InvalidData;
    if (domIsValidAttributeLocalName(prTextBufferView(&attribute))) {
        domSetAttribute(element, PrDomNamespace_None, prTextBufferView(&attribute), value);
        status = PrStatus_Ok;
    }
    prTextBufferReset(&attribute);
    return status;
}

void prHtmlDatasetDelete(PrDomElement* element, PrStringView name) {
    PrTextBuffer attribute = {};
    attributeName(name, &attribute);
    if (PrDomAttr* found = domAttributeByName(element, prTextBufferView(&attribute)))
        domRemoveAttributeNode(element, found);
    prTextBufferReset(&attribute);
}

PrHtmlDialogElement* PR_NULLABLE prHtmlDialogElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlDialogElement>(object, PrDomTag_Dialog);
}

bool prHtmlDialogElementOpen(const PrHtmlDialogElement* element) {
    return domAttribute(element, PrDomNamespace_None, "open") != nullptr;
}

void prHtmlDialogElementSetOpen(PrHtmlDialogElement* element, bool value) {
    if (value)
        domSetAttribute(element, PrDomNamespace_None, "open", "");
    else
        domRemoveAttribute(element, PrDomNamespace_None, "open");
}

PrStringView prHtmlDialogElementReturnValue(const PrHtmlDialogElement* element) {
    return element->state != nullptr ? prRawStringView(&element->state->returnValue) : "";
}

void prHtmlDialogElementSetReturnValue(PrHtmlDialogElement* element, PrStringView value) {
    prRawStringReplace(&domElementState(element)->returnValue, value);
}

PrStatus prHtmlDialogElementShow(PrHtmlDialogElement* element) {
    bool modal = element->state != nullptr && element->state->modal;
    if (prHtmlDialogElementOpen(element))
        return modal ? PrStatus_InvalidState : PrStatus_Ok;
    prHtmlDialogElementSetOpen(element, true);
    htmlFocusEndAutofocus(element->document);
    return PrStatus_Ok;
}

PrStatus prHtmlDialogElementShowModal(PrHtmlDialogElement* element) {
    bool modal = element->state != nullptr && element->state->modal;
    if (prHtmlDialogElementOpen(element))
        return modal ? PrStatus_Ok : PrStatus_InvalidState;
    if (!domIsConnected(element))
        return PrStatus_InvalidState;
    prHtmlDialogElementSetOpen(element, true);
    domElementState(element)->modal = true;
    htmlFocusEndAutofocus(element->document);
    return PrStatus_Ok;
}

void prHtmlDialogElementClose(PrHtmlDialogElement* element, PrStringView result) {
    if (!prHtmlDialogElementOpen(element))
        return;
    prHtmlDialogElementSetOpen(element, false);
    DomElementState* state = domElementState(element);
    state->modal = false;
    if (result.data != nullptr)
        prRawStringReplace(&state->returnValue, result);
    const DomDocumentHost* host = element->document->host;
    if (host == nullptr)
        return;
    // The task holds the dialog.
    domNodeHold(element);
    host->queueTask(host->context, [](void* data) {
        PrDomElement* dialog = static_cast<PrDomElement*>(data);
        domEventFire(dialog, "close");
        domNodeUnhold(dialog);
    }, element, [](void* data) { domNodeUnhold(static_cast<PrDomElement*>(data)); });
}

void prHtmlDialogElementRequestClose(PrHtmlDialogElement* element, PrStringView result) {
    if (!prHtmlDialogElementOpen(element))
        return;
    PrDomEventInit init = prDomEventInitDefault();
    init.cancelable = true;
    if (domEventFire(element, "cancel", init))
        prHtmlDialogElementClose(element, result);
}

PrHtmlIFrameElement* PR_NULLABLE prHtmlIFrameElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlIFrameElement>(object, PrDomTag_Iframe);
}

PrHtmlWindow* PR_NULLABLE prHtmlIFrameElementContentWindow(PrHtmlIFrameElement* element) {
    return htmlFrameWindow(element);
}

PrDomDocument* PR_NULLABLE prHtmlIFrameElementContentDocument(PrHtmlIFrameElement* element) {
    PrHtmlWindow* window = htmlFrameWindow(element);
    return window != nullptr ? window->document : nullptr;
}

PrHtmlImageElement* PR_NULLABLE prHtmlImageElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlImageElement>(object, PrDomTag_Img);
}

PrDomImageState prHtmlImageElementState(const PrHtmlImageElement* element) {
    const DomElementState* state = element->state;
    return state != nullptr ? state->image : PrDomImageState_Unavailable;
}

bool prHtmlImageElementComplete(const PrHtmlImageElement* element) {
    const PrDomAttr* src = domAttribute(element, PrDomNamespace_None, "src");
    if (src == nullptr || prRawStringView(&src->value).size == 0)
        return true;
    PrDomImageState state = prHtmlImageElementState(element);
    return state == PrDomImageState_CompletelyAvailable || state == PrDomImageState_Broken;
}

PrStringView prHtmlImageElementCurrentSrc(const PrHtmlImageElement* element) {
    const DomElementState* state = element->state;
    return state != nullptr && !prRawStringIsNull(&state->imageUrl) ? prRawStringView(&state->imageUrl) : "";
}

uint32_t prHtmlImageElementNaturalWidth(const PrHtmlImageElement* element) {
    return htmlImageDimension(element, true);
}

uint32_t prHtmlImageElementNaturalHeight(const PrHtmlImageElement* element) {
    return htmlImageDimension(element, false);
}

uint32_t prHtmlImageElementWidth(const PrHtmlImageElement* element) {
    return htmlImageDimension(element, true);
}

uint32_t prHtmlImageElementHeight(const PrHtmlImageElement* element) {
    return htmlImageDimension(element, false);
}

void prHtmlImageElementSetWidth(PrHtmlImageElement* element, uint32_t value) {
    htmlSetReflectedUnsigned(element, "width", value);
}

void prHtmlImageElementSetHeight(PrHtmlImageElement* element, uint32_t value) {
    htmlSetReflectedUnsigned(element, "height", value);
}

bool prHtmlImageElementImageSource(PrHtmlImageElement* element, PrCanvasImageSource* out) {
    *out = PrCanvasImageSource{};
    const DomElementState* state = element->state;
    if (state == nullptr || state->image != PrDomImageState_CompletelyAvailable || state->picture == nullptr)
        return false;
    prDrawImageRetain(state->picture);
    *out = PrCanvasImageSource{
        .image = state->picture,
        .extent = PrVec2{float(state->naturalWidth), float(state->naturalHeight)},
        .originClean = true,
    };
    return true;
}

PrHtmlCanvasElement* PR_NULLABLE prHtmlCanvasElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlCanvasElement>(object, PrDomTag_Canvas);
}

uint32_t prHtmlCanvasElementWidth(const PrHtmlCanvasElement* element) {
    return domReflectedUnsigned(element, "width", 300);
}

uint32_t prHtmlCanvasElementHeight(const PrHtmlCanvasElement* element) {
    return domReflectedUnsigned(element, "height", 150);
}

PrStatus prHtmlCanvasElementSetWidth(PrHtmlCanvasElement* element, uint32_t value) {
    if (prHtmlCanvasElementContextMode(element) == PrHtmlCanvasContextMode_Placeholder)
        return PrStatus_InvalidState;
    htmlSetReflectedUnsigned(element, "width", value, 300);
    return PrStatus_Ok;
}

PrStatus prHtmlCanvasElementSetHeight(PrHtmlCanvasElement* element, uint32_t value) {
    if (prHtmlCanvasElementContextMode(element) == PrHtmlCanvasContextMode_Placeholder)
        return PrStatus_InvalidState;
    htmlSetReflectedUnsigned(element, "height", value, 150);
    return PrStatus_Ok;
}

PrHtmlCanvasContextMode prHtmlCanvasElementContextMode(const PrHtmlCanvasElement* element) {
    const HtmlCanvas* canvas = htmlCanvasFind(element);
    return canvas != nullptr ? canvas->mode : PrHtmlCanvasContextMode_None;
}

PrStatus prHtmlCanvasElementGetContext2D(PrHtmlCanvasElement* element, const PrCanvasSettings* PR_NULLABLE settings,
        PrCanvas* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    HtmlCanvas* canvas = htmlCanvasFromElement(element);
    switch (canvas->mode) {
        case PrHtmlCanvasContextMode_Placeholder:
            return PrStatus_InvalidState;
        case PrHtmlCanvasContextMode_2d:
            *out = canvas->context;
            return PrStatus_Ok;
        default:
            *out = htmlCanvasCreateContext2D(canvas, settings != nullptr ? *settings : prCanvasSettingsDefault());
            return PrStatus_Ok;
    }
}

PrStatus prHtmlCanvasElementToDataUrl(PrHtmlCanvasElement* element, PrStringView type, PrRawString* out) {
    return htmlCanvasToDataUrl(element, type, out);
}

bool prHtmlCanvasElementImageSource(PrHtmlCanvasElement* element, PrCanvasImageSource* out) {
    return htmlCanvasImageSource(element, out);
}

PrHtmlAnchorElement* PR_NULLABLE prHtmlAnchorElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlAnchorElement>(object, PrDomTag_A);
}

PrHtmlAreaElement* PR_NULLABLE prHtmlAreaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlAreaElement>(object, PrDomTag_Area);
}

PrRawString prHtmlHyperlinkComponent(const PrHtmlElement* element, PrUrlComponent component) {
    PR_ASSERT(domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area));
    PrUrl* url = hyperlinkUrl(element);
    if (url == nullptr) {
        PrStringView href = domAttributeValue(element, "href");
        if (component == PrUrlComponent_Href && href.data != nullptr)
            return prRawStringCopy(href);
        return component == PrUrlComponent_Protocol ? prRawStringExternal(":") : prRawStringEmpty();
    }
    PrRawString value = prUrlComponent(url, component);
    prUrlDestroy(url);
    return value;
}

PrStatus prHtmlHyperlinkSetComponent(PrHtmlElement* element, PrUrlComponent component, PrStringView value) {
    PR_ASSERT(domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area));
    if (component == PrUrlComponent_Origin)
        return PrStatus_ReadOnly;
    if (component == PrUrlComponent_Href)
        return prDomElementSetAttribute(element, "href", value);
    PrUrl* url = hyperlinkUrl(element);
    if (url == nullptr)
        return PrStatus_Ok;
    // A value that does not parse leaves the URL as it was, and the attribute is set to it all the same
    // (html/#update-href).
    prUrlSetComponent(url, component, value);
    PrRawString href = prUrlSerialize(url, PrUrlSerialization_Whole);
    prUrlDestroy(url);
    PrStatus status = prDomElementSetAttribute(element, "href", prRawStringView(&href));
    prRawStringReset(&href);
    return status;
}

PrHtmlBaseElement* PR_NULLABLE prHtmlBaseElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlBaseElement>(object, PrDomTag_Base);
}

PrRawString prHtmlBaseElementHref(const PrHtmlBaseElement* element) {
    PrStringView href = domAttributeValue(element, "href");
    if (href.data == nullptr)
        href = "";
    PrUrl* url = nullptr;
    if (prUrlCreate(href, element->document->url, element->document->charset, &url) != PrStatus_Ok)
        return prRawStringCopy(href);
    PrRawString value = prUrlSerialize(url, PrUrlSerialization_Whole);
    prUrlDestroy(url);
    return value;
}

PrHtmlLinkElement* PR_NULLABLE prHtmlLinkElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlLinkElement>(object, PrDomTag_Link);
}

PrCssStyleSheet* PR_NULLABLE prHtmlLinkElementSheet(PrHtmlLinkElement* element) {
    return cssElementSheet(element);
}

PrHtmlMetaElement* PR_NULLABLE prHtmlMetaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlMetaElement>(object, PrDomTag_Meta);
}

PrHtmlPictureElement* PR_NULLABLE prHtmlPictureElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlPictureElement>(object, PrDomTag_Picture);
}

PrHtmlSourceElement* PR_NULLABLE prHtmlSourceElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlSourceElement>(object, PrDomTag_Source);
}

PrHtmlMediaElement* PR_NULLABLE prHtmlMediaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    if (PrHtmlMediaElement* video = htmlElementQuery<PrHtmlMediaElement>(object, PrDomTag_Video))
        return video;
    return htmlElementQuery<PrHtmlMediaElement>(object, PrDomTag_Audio);
}

bool prHtmlMediaElementIsVideo(const PrHtmlMediaElement* element) {
    return element->tag == PrDomTag_Video;
}

PrHtmlMediaElement* prHtmlMediaElementCreateAudio(PrDomDocument* document, PrStringView src) {
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Audio, "audio");
    domNodeRetain(element);
    domSetAttribute(element, PrDomNamespace_None, "preload", "auto");
    if (src.data != nullptr)
        domSetAttribute(element, PrDomNamespace_None, "src", src);
    return static_cast<PrHtmlMediaElement*>(element);
}

PrHtmlMediaError prHtmlMediaElementError(const PrHtmlMediaElement* element) {
    return mediaOf(element)->error;
}

PrStringView prHtmlMediaElementErrorMessage(const PrHtmlMediaElement* element) {
    return prRawStringView(&mediaOf(element)->errorMessage);
}

PrStringView prHtmlMediaElementCurrentSrc(const PrHtmlMediaElement* element) {
    return prRawStringView(&mediaOf(element)->currentSrc);
}

PrHtmlMediaNetworkState prHtmlMediaElementNetworkState(const PrHtmlMediaElement* element) {
    return mediaOf(element)->networkState;
}

PrHtmlMediaReadyState prHtmlMediaElementReadyState(const PrHtmlMediaElement* element) {
    return mediaOf(element)->readyState;
}

void prHtmlMediaElementLoad(PrHtmlMediaElement* element) {
    htmlMediaLoad(mediaOf(element));
}

PrHtmlCanPlay prHtmlMediaElementCanPlayType(const PrHtmlMediaElement* element, PrStringView type) {
    return htmlCanPlayType(mediaOf(element)->window, type);
}

bool prHtmlMediaElementSeeking(const PrHtmlMediaElement* element) {
    return mediaOf(element)->seeking;
}

double prHtmlMediaElementCurrentTime(PrHtmlMediaElement* element) {
    return htmlMediaCurrentTime(mediaOf(element));
}

void prHtmlMediaElementSetCurrentTime(PrHtmlMediaElement* element, double seconds) {
    htmlMediaSeek(mediaOf(element), seconds);
}

void prHtmlMediaElementFastSeek(PrHtmlMediaElement* element, double seconds) {
    prHtmlMediaElementSetCurrentTime(element, seconds);
}

double prHtmlMediaElementDuration(const PrHtmlMediaElement* element) {
    return mediaOf(element)->duration;
}

bool prHtmlMediaElementPaused(const PrHtmlMediaElement* element) {
    return mediaOf(element)->paused;
}

bool prHtmlMediaElementEnded(PrHtmlMediaElement* element) {
    return htmlMediaEnded(mediaOf(element));
}

double prHtmlMediaElementDefaultPlaybackRate(const PrHtmlMediaElement* element) {
    return mediaOf(element)->defaultRate;
}

void prHtmlMediaElementSetDefaultPlaybackRate(PrHtmlMediaElement* element, double rate) {
    htmlMediaSetRate(mediaOf(element), rate, true);
}

double prHtmlMediaElementPlaybackRate(const PrHtmlMediaElement* element) {
    return mediaOf(element)->rate;
}

PrStatus prHtmlMediaElementSetPlaybackRate(PrHtmlMediaElement* element, double rate) {
    // html/#dom-media-playbackrate: the engine can play its timeline at any rate, forwards and backwards.
    htmlMediaSetRate(mediaOf(element), rate, false);
    return PrStatus_Ok;
}

bool prHtmlMediaElementPreservesPitch(const PrHtmlMediaElement* element) {
    return mediaOf(element)->preservesPitch;
}

void prHtmlMediaElementSetPreservesPitch(PrHtmlMediaElement* element, bool preserves) {
    htmlMediaSetPreservesPitch(mediaOf(element), preserves);
}

uint32_t prHtmlMediaElementRanges(PrHtmlMediaElement* element, PrHtmlMediaRanges which,
        double* PR_NULLABLE starts, double* PR_NULLABLE ends, uint32_t capacity) {
    HtmlMedia* media = mediaOf(element);
    if (which == PrHtmlMediaRanges_Played) {
        uint32_t count = media->played.count / 2;
        for (uint32_t i = 0; i < count && i < capacity; ++i) {
            starts[i] = media->played.data[i * 2];
            ends[i] = media->played.data[i * 2 + 1];
        }
        return count;
    }
    // A media source answers from what it has buffered and what it can seek to.
    if (media->mediaSource != nullptr) {
        RawArray<PrMediaTimeRange> ranges{};
        if (which == PrHtmlMediaRanges_Buffered)
            htmlMediaSourceBuffered(media->mediaSource, &ranges);
        else
            htmlMediaSourceSeekable(media->mediaSource, &ranges);
        uint32_t count = ranges.count;
        for (uint32_t i = 0; i < count && i < capacity; ++i) {
            starts[i] = ranges.data[i].start;
            ends[i] = ranges.data[i].end;
        }
        ranges.reset();
        return count;
    }
    // The whole resource is fetched, so all of its data is available to seek to.
    if (media->readyState < PrHtmlMediaReadyState_HaveMetadata || prF64IsNan(media->duration))
        return 0;
    if (capacity != 0) {
        starts[0] = 0;
        ends[0] = media->duration;
    }
    return 1;
}

void prHtmlMediaElementPlay(PrHtmlMediaElement* element, const PrHtmlPlayPromise* promise) {
    HtmlMedia* media = mediaOf(element);
    if (media->window == nullptr) {
        // An element that no window runs has no resource to play.
        promise->settle(promise->context, "NotSupportedError");
        promise->release(promise->context);
        return;
    }
    htmlMediaPlay(media, *promise);
}

void prHtmlMediaElementPause(PrHtmlMediaElement* element) {
    htmlMediaPause(mediaOf(element));
}

double prHtmlMediaElementVolume(const PrHtmlMediaElement* element) {
    return mediaOf(element)->volume;
}

PrStatus prHtmlMediaElementSetVolume(PrHtmlMediaElement* element, double volume) {
    if (volume < 0 || volume > 1)
        return PrStatus_OutOfRange;
    htmlMediaSetVolume(mediaOf(element), volume, prHtmlMediaElementMuted(element));
    return PrStatus_Ok;
}

bool prHtmlMediaElementMuted(const PrHtmlMediaElement* element) {
    const HtmlMedia* media = mediaOf(element);
    if (media->mutedSet)
        return media->muted;
    // html/#dom-media-muted: an element follows its muted content attribute until a script sets muted.
    return domAttribute(element, PrDomNamespace_None, "muted") != nullptr;
}

void prHtmlMediaElementSetMuted(PrHtmlMediaElement* element, bool muted) {
    HtmlMedia* media = mediaOf(element);
    htmlMediaSetVolume(media, media->volume, muted);
}

uint32_t prHtmlMediaElementVideoWidth(const PrHtmlMediaElement* element) {
    return mediaOf(element)->videoWidth;
}

uint32_t prHtmlMediaElementVideoHeight(const PrHtmlMediaElement* element) {
    return mediaOf(element)->videoHeight;
}

PrHtmlTrackList* prHtmlMediaElementTracks(PrHtmlMediaElement* element, PrHtmlTrackKind kind) {
    HtmlMedia* media = mediaOf(element);
    switch (kind) {
        case PrHtmlTrackKind_Audio: return static_cast<PrHtmlTrackList*>(&media->audioTracks);
        case PrHtmlTrackKind_Video: return static_cast<PrHtmlTrackList*>(&media->videoTracks);
        default: return static_cast<PrHtmlTrackList*>(&media->textTracks);
    }
}

PrHtmlTrackList* PR_NULLABLE prHtmlTrackListQuery(PrDomEventTarget* PR_NULLABLE object) {
    bool list = object != nullptr && (object->type == DomTargetType_TextTrackList ||
        object->type == DomTargetType_AudioTrackList || object->type == DomTargetType_VideoTrackList);
    return list ? static_cast<PrHtmlTrackList*>(object) : nullptr;
}

PrHtmlTrackKind prHtmlTrackListKind(const PrHtmlTrackList* list) {
    switch (list->type) {
        case DomTargetType_AudioTrackList: return PrHtmlTrackKind_Audio;
        case DomTargetType_VideoTrackList: return PrHtmlTrackKind_Video;
        default: return PrHtmlTrackKind_Text;
    }
}

uint32_t prHtmlTrackListLength(const PrHtmlTrackList*) {
    return 0;
}

int32_t prHtmlTrackListSelectedIndex(const PrHtmlTrackList*) {
    return -1;
}

PrHtmlSlotElement* PR_NULLABLE prHtmlSlotElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomNode* node = domNodeFromTarget(object);
    return node != nullptr && domIsSlot(node) ? static_cast<PrHtmlSlotElement*>(domElementFromNode(node)) : nullptr;
}

uint32_t prHtmlSlotElementAssignedNodes(PrHtmlSlotElement* element, bool flatten, PrDomNode* PR_NONNULL* PR_NULLABLE items,
        uint32_t capacity) {
    RawArray<PrDomNode* PR_NONNULL> nodes;
    domSlotAssignedNodes(element, flatten, &nodes);
    for (uint32_t i = 0; items != nullptr && i < nodes.count && i < capacity; ++i)
        items[i] = nodes.data[i];
    uint32_t count = nodes.count;
    nodes.reset();
    return count;
}

void prHtmlSlotElementAssign(PrHtmlSlotElement* element, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    domSlotAssign(element, reinterpret_cast<PrDomNode* const*>(nodes), count);
}

PrHtmlScriptElement* PR_NULLABLE prHtmlScriptElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlScriptElement>(object, PrDomTag_Script);
}

bool prHtmlScriptElementAsync(const PrHtmlScriptElement* element) {
    bool forceAsync = (domScriptFlags(element) & DomScriptFlag_NotForceAsync) == 0;
    return forceAsync || domAttributeValue(element, "async").data != nullptr;
}

void prHtmlScriptElementSetAsync(PrHtmlScriptElement* element, bool value) {
    domScriptSetFlags(element, domScriptFlags(element) | DomScriptFlag_NotForceAsync);
    if (value)
        domSetAttribute(element, PrDomNamespace_None, "async", "");
    else
        domRemoveAttribute(element, PrDomNamespace_None, "async");
}

PrRawString prHtmlScriptElementText(const PrHtmlScriptElement* element) {
    return domChildTextContent(element);
}

void prHtmlScriptElementSetText(PrHtmlScriptElement* element, PrStringView value) {
    domStringReplaceAll(element, value);
}

bool prHtmlScriptElementSupports(PrStringView type) {
    return type == "classic" || type == "module";
}

PrHtmlStyleElement* PR_NULLABLE prHtmlStyleElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlStyleElement>(object, PrDomTag_Style);
}

PrCssStyleSheet* PR_NULLABLE prHtmlStyleElementSheet(PrHtmlStyleElement* element) {
    return cssElementSheet(element);
}

PrHtmlTemplateElement* PR_NULLABLE prHtmlTemplateElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTemplateElement>(object, PrDomTag_Template);
}

PrDomDocumentFragment* prHtmlTemplateElementContent(PrHtmlTemplateElement* element) {
    return static_cast<PrDomDocumentFragment*>(domElementContents(element));
}

bool prHtmlTemplateElementShadowRootMode(const PrHtmlTemplateElement* element, PrDomShadowRootMode* out) {
    PrStringView mode = domAttributeValue(element, "shadowrootmode");
    if (prAsciiEqualsFold(mode, "open"))
        *out = PrDomShadowRootMode_Open;
    else if (prAsciiEqualsFold(mode, "closed"))
        *out = PrDomShadowRootMode_Closed;
    else
        return false;
    return true;
}

PrDomSlotAssignmentMode prHtmlTemplateElementShadowRootSlotAssignment(const PrHtmlTemplateElement* element) {
    PrStringView value = domAttributeValue(element, "shadowrootslotassignment");
    return prAsciiEqualsFold(value, "manual") ? PrDomSlotAssignmentMode_Manual : PrDomSlotAssignmentMode_Named;
}

PrHtmlErrorEvent* PR_NULLABLE prHtmlErrorEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->error != nullptr ? static_cast<PrHtmlErrorEvent*>(object) : nullptr;
}

PrHtmlErrorEvent* prHtmlErrorEventCreate(PrStringView type, const PrHtmlErrorEventInit* PR_NULLABLE init) {
    PrHtmlErrorEventInit values = init != nullptr ? *init : prHtmlErrorEventInitDefault();
    PrHtmlErrorEvent* event = domEventCreate<PrHtmlErrorEvent>(type, values.event, PrDomEventKind_Error);
    domEventSetError(event, values.message, values.filename, values.lineno, values.colno);
    return event;
}

PrStringView prHtmlErrorEventMessage(const PrHtmlErrorEvent* event) {
    return prRawStringView(&event->error->message);
}

PrStringView prHtmlErrorEventFilename(const PrHtmlErrorEvent* event) {
    return prRawStringView(&event->error->filename);
}

uint32_t prHtmlErrorEventLineno(const PrHtmlErrorEvent* event) {
    return event->error->lineno;
}

uint32_t prHtmlErrorEventColno(const PrHtmlErrorEvent* event) {
    return event->error->colno;
}

PrHtmlMessageEvent* PR_NULLABLE prHtmlMessageEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Message ? static_cast<PrHtmlMessageEvent*>(object) : nullptr;
}

PrHtmlMessageEvent* prHtmlMessageEventCreate(PrStringView type, const PrHtmlMessageEventInit* PR_NULLABLE init) {
    PrHtmlMessageEventInit values = init != nullptr ? *init : prHtmlMessageEventInitDefault();
    PrHtmlMessageEvent* event = domEventCreate<PrHtmlMessageEvent>(type, values.event, PrDomEventKind_Message);
    event->message = allocZeroed<DomMessageEventData>();
    *event->message = DomMessageEventData{
        .data = prRawStringNull(),
        .origin = prRawStringCopy(values.origin),
        .lastEventId = prRawStringCopy(values.lastEventId),
        .source = values.source,
        .ports = {},
    };
    return event;
}

PrStringView prHtmlMessageEventSerializedData(const PrHtmlMessageEvent* event) {
    const PrRawString* data = &event->message->data;
    return prRawStringIsNull(data) ? PrStringView{} : prRawStringView(data);
}

PrStringView prHtmlMessageEventOrigin(const PrHtmlMessageEvent* event) {
    return prRawStringView(&event->message->origin);
}

PrStringView prHtmlMessageEventLastEventId(const PrHtmlMessageEvent* event) {
    return prRawStringView(&event->message->lastEventId);
}

PrDomEventTarget* PR_NULLABLE prHtmlMessageEventSource(PrHtmlMessageEvent* event) {
    return event->message->source;
}

PrHtmlPageRevealEvent* PR_NULLABLE prHtmlPageRevealEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_PageReveal ? static_cast<PrHtmlPageRevealEvent*>(object) : nullptr;
}

PrHtmlPageRevealEvent* prHtmlPageRevealEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init) {
    return domEventCreate<PrHtmlPageRevealEvent>(type, init != nullptr ? *init : prDomEventInitDefault(),
        PrDomEventKind_PageReveal);
}

PrHtmlPromiseRejectionEvent* PR_NULLABLE prHtmlPromiseRejectionEventQuery(PrDomEvent* PR_NULLABLE object) {
    if (object != nullptr && object->kind == PrDomEventKind_PromiseRejection)
        return static_cast<PrHtmlPromiseRejectionEvent*>(object);
    return nullptr;
}

PrHtmlPromiseRejectionEvent* prHtmlPromiseRejectionEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init) {
    PrDomEventInit values = init != nullptr ? *init : prDomEventInitDefault();
    return domEventCreate<PrHtmlPromiseRejectionEvent>(type, values, PrDomEventKind_PromiseRejection);
}

PrStringView prHtmlPreferenceValueName(PrHtmlPreferenceValue value) {
    return value != PrHtmlPreferenceValue_None ? cssKeywordName(kPreferenceKeywords[value]) : "";
}

PrHtmlPreferenceValue prHtmlPreferenceValueFromName(PrStringView text) {
    // A name matches a keyword of the preferences' features only if it is spelled exactly, in the same way as the valid
    // values.
    PrCssKeyword keyword = cssKeywordFromText(text);
    PrHtmlPreferenceValue value = preferenceValueOf(keyword);
    return value != PrHtmlPreferenceValue_None && text == cssKeywordName(keyword) ? value : PrHtmlPreferenceValue_None;
}

PrHtmlPreferenceObject* PR_NULLABLE prHtmlPreferenceObjectQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_Preference ? static_cast<PrHtmlPreferenceObject*>(object) : nullptr;
}

PrHtmlPreferenceValue prHtmlPreferenceObjectOverride(const PrHtmlPreferenceObject* preference) {
    PrCssDescriptor feature = kPreferences[preference->preference].feature;
    return preferenceValueOf(environmentOf(preference)->overrides[feature - kCssDescriptorMediaFirst].keyword);
}

PrHtmlPreferenceValue prHtmlPreferenceObjectValue(const PrHtmlPreferenceObject* preference) {
    return preferenceValueOf(cssMediaEnvironmentKeyword(environmentOf(preference),
        kPreferences[preference->preference].feature));
}

uint32_t prHtmlPreferenceObjectValidValues(const PrHtmlPreferenceObject* preference,
        PrHtmlPreferenceValue* PR_NULLABLE items, uint32_t capacity) {
    const PreferenceFeature& feature = kPreferences[preference->preference];
    for (uint32_t i = 0; items != nullptr && i < feature.count && i < capacity; ++i)
        items[i] = feature.values[i];
    return feature.count;
}

void prHtmlPreferenceObjectClearOverride(PrHtmlPreferenceObject* preference) {
    PrCssDescriptor feature = kPreferences[preference->preference].feature;
    CssMediaEnvironment* environment = &preference->window->media;
    if (environment->overrides[feature - kCssDescriptorMediaFirst].keyword == PrCssKeyword_Unknown)
        return;
    cssMediaEnvironmentOverride(environment, feature, PrCssKeyword_Unknown);
    cssPresentationDocumentMediaChanged(preference->window->document);
    domEventFire(preference, "change");
}

PrStatus prHtmlPreferenceObjectRequestOverride(PrHtmlPreferenceObject* preference, PrHtmlPreferenceValue value) {
    if (value == PrHtmlPreferenceValue_None) {
        prHtmlPreferenceObjectClearOverride(preference);
        return PrStatus_Ok;
    }
    const PreferenceFeature& feature = kPreferences[preference->preference];
    for (uint32_t i = 0; i < feature.count; ++i) {
        if (value == feature.values[i]) {
            cssMediaEnvironmentOverride(&preference->window->media, feature.feature, kPreferenceKeywords[value]);
            cssPresentationDocumentMediaChanged(preference->window->document);
            domEventFire(preference, "change");
            return PrStatus_Ok;
        }
    }
    return PrStatus_InvalidData;
}

PrHtmlPerformance* PR_NULLABLE prHtmlPerformanceQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_Performance ? static_cast<PrHtmlPerformance*>(object) : nullptr;
}

double prHtmlPerformanceNow(const PrHtmlPerformance* performance) {
    return htmlWindowNow(performance->window);
}

double prHtmlPerformanceTimeOrigin(const PrHtmlPerformance* performance) {
    return performance->window->timeOriginWall;
}

PrStringView prHtmlOrientationTypeName(PrHtmlOrientationType type) {
    switch (type) {
        case PrHtmlOrientationType_PortraitPrimary: return "portrait-primary";
        case PrHtmlOrientationType_PortraitSecondary: return "portrait-secondary";
        case PrHtmlOrientationType_LandscapePrimary: return "landscape-primary";
        case PrHtmlOrientationType_LandscapeSecondary: return "landscape-secondary";
    }
    return "";
}

PrHtmlScreenOrientation* PR_NULLABLE prHtmlScreenOrientationQuery(PrDomEventTarget* PR_NULLABLE object) {
    if (object != nullptr && object->type == DomTargetType_ScreenOrientation)
        return static_cast<PrHtmlScreenOrientation*>(object);
    return nullptr;
}

PrHtmlOrientationType prHtmlScreenOrientationType(const PrHtmlScreenOrientation* orientation) {
    if (mediaInteger(orientation->window, PrCssDescriptor_MediaDeviceHeight) >
            mediaInteger(orientation->window, PrCssDescriptor_MediaDeviceWidth))
        return PrHtmlOrientationType_PortraitPrimary;
    return PrHtmlOrientationType_LandscapePrimary;
}

uint16_t prHtmlScreenOrientationAngle(const PrHtmlScreenOrientation*) {
    return 0;
}

PrHtmlDocumentTimeline* prHtmlDocumentTimelineCreate(PrDomDocument* document, double originTime) {
    PrHtmlDocumentTimeline* timeline = allocZeroed<PrHtmlDocumentTimeline>();
    *timeline = PrHtmlDocumentTimeline{{document, originTime}};
    return timeline;
}

void prHtmlDocumentTimelineDestroy(PrHtmlDocumentTimeline* PR_NULLABLE timeline) {
    prMemoryFree(timeline);
}

bool prHtmlDocumentTimelineCurrentTime(const PrHtmlDocumentTimeline* timeline, double* out) {
    PrHtmlWindow* window = prHtmlDocumentDefaultView(timeline->document);
    if (window == nullptr)
        return false;
    *out = htmlWindowFrameTime(window) - timeline->originTime;
    return true;
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_Window ? static_cast<PrHtmlWindow*>(object) : nullptr;
}

PrDomDocument* prHtmlWindowDocument(PrHtmlWindow* window) {
    return window->document;
}

PrDomAgent* prHtmlWindowAgent(PrHtmlWindow* window) {
    return window->agent;
}

void prHtmlWindowSetCustomElementRegistry(PrHtmlWindow* window, const PrDomCustomElementRegistry* PR_NULLABLE registry) {
    window->customElements = registry;
    window->document->customElements = registry;
}

PrHtmlPreferenceObject* prHtmlWindowPreference(PrHtmlWindow* window, PrHtmlPreference preference) {
    return static_cast<PrHtmlPreferenceObject*>(&window->preferences[preference]);
}

PrHtmlPerformance* prHtmlWindowPerformance(PrHtmlWindow* window) {
    return static_cast<PrHtmlPerformance*>(&window->performance);
}

double prHtmlWindowCurrentTime(const PrHtmlWindow* window) {
    if (window->wallClock != nullptr)
        return window->wallClock(window->clockContext);
    return window->timeOriginWall + htmlWindowNow(window);
}

PrHtmlScreenOrientation* prHtmlWindowScreenOrientation(PrHtmlWindow* window) {
    return static_cast<PrHtmlScreenOrientation*>(&window->orientation);
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowParent(PrHtmlWindow* window) {
    return window->parent;
}

PrStatus prHtmlWindowPostMessage(PrHtmlWindow* window, PrHtmlWindow* source, PrStringView serialized,
        PrDomEventTarget* PR_NONNULL const* PR_NULLABLE ports, uint32_t portCount, PrStringView targetOrigin) {
    PrRawString origin = prRawStringNull();
    if (targetOrigin == "/") {
        origin = htmlWindowOrigin(source);
    } else if (targetOrigin != "*") {
        PrUrl* url = nullptr;
        if (prUrlCreate(targetOrigin, nullptr, PrCharset_Utf8, &url) != PrStatus_Ok)
            return PrStatus_Syntax;
        origin = prUrlOrigin(url);
        prUrlDestroy(url);
    }
    PostedMessage* posted = allocZeroed<PostedMessage>();
    *posted = PostedMessage{window, source, HtmlMessage{.data = prRawStringCopy(serialized)}, htmlWindowOrigin(source), origin};
    for (uint32_t i = 0; i < portCount; ++i)
        posted->message.ports.push(ports[i]);
    htmlWindowQueueTask(window, deliverMessage, posted, dropMessage);
    return PrStatus_Ok;
}

PrRawString prHtmlWindowLocation(const PrHtmlWindow* window, PrUrlComponent component) {
    return prUrlComponent(window->document->url, component);
}

PrStatus prHtmlWindowSetLocation(PrHtmlWindow* window, PrHtmlWindow* source, PrUrlComponent component, PrStringView value,
        PrHtmlHistoryHandling handling) {
    // A document that is not fully active navigates nowhere (html/#navigate).
    if (!htmlWindowIsFullyActive(window))
        return PrStatus_Ok;
    PrDomDocument* document = window->document;
    PrUrl* copy = nullptr;
    if (component == PrUrlComponent_Href) {
        PrStatus status = prUrlCreate(value, source->document->url, PrCharset_Utf8, &copy);
        if (status != PrStatus_Ok)
            return PrStatus_Syntax;
    } else {
        copy = prUrlClone(document->url);
        prUrlSetComponent(copy, component, value);
    }
    PrStringView was = prUrlFragment(document->url);
    PrStringView now = prUrlFragment(copy);
    bool sameFragment = was == now && (was.data == nullptr) == (now.data == nullptr);
    // html/#dom-location-hash: a fragment that the URL has already navigates nowhere.
    if (component == PrUrlComponent_Hash && sameFragment) {
        prUrlDestroy(copy);
        return PrStatus_Ok;
    }
    PrRawString before = prUrlSerialize(document->url, PrUrlSerialization_ExcludeFragment);
    PrRawString after = prUrlSerialize(copy, PrUrlSerialization_ExcludeFragment);
    bool same = prRawStringView(&before) == prRawStringView(&after);
    bool fragment = same && (component == PrUrlComponent_Hash || now.data != nullptr);
    prRawStringReset(&before);
    prRawStringReset(&after);
    if (!fragment) {
        PrRawString href = prUrlSerialize(copy, PrUrlSerialization_Whole);
        prUrlDestroy(copy);
        HtmlHistoryCommit commit =
            handling == PrHtmlHistoryHandling_Replace ? HtmlHistoryCommit_Replace : HtmlHistoryCommit_Push;
        bool navigated = htmlPageNavigate(window, source, prRawStringView(&href), commit, window->history->index);
        prRawStringReset(&href);
        return navigated ? PrStatus_Ok : PrStatus_NotSupported;
    }
    // html/#navigate-fragid: the URL that the document has already replaces its entry (html/#navigate), popstate fires,
    // hashchange follows if the fragment changed, and the document scrolls to the fragment.
    PrRawString old = prUrlSerialize(document->url, PrUrlSerialization_Whole);
    PrRawString next = prUrlSerialize(copy, PrUrlSerialization_Whole);
    if (prRawStringView(&old) == prRawStringView(&next))
        handling = PrHtmlHistoryHandling_Replace;
    prRawStringReset(&next);
    prUrlDestroy(document->url);
    document->url = copy;
    htmlHistoryUpdate(window, prRawStringNull(), handling);
    htmlHistoryNotify(window, prRawStringView(&old));
    prRawStringReset(&old);
    htmlWindowScrollToFragment(window);
    return PrStatus_Ok;
}

PrRawString prHtmlWindowName(const PrHtmlWindow* window) {
    const PrHtmlPage* page = htmlDocumentPage(window->document);
    bool named = page != nullptr && page->window == window && prRawStringView(&page->targetName).data != nullptr;
    return named ? prRawStringClone(&page->targetName) : prRawStringCopy("");
}

void prHtmlWindowSetName(PrHtmlWindow* window, PrStringView name) {
    PrHtmlPage* page = htmlDocumentPage(window->document);
    if (page != nullptr && page->window == window)
        prRawStringReplace(&page->targetName, name);
}

void prHtmlWindowNotifyUserActivation(PrHtmlWindow* window) {
    double now = htmlWindowClock(window);
    for (PrHtmlWindow* at = window; at != nullptr; at = at->parent)
        at->lastActivation = now;
    htmlWindowNotifyDescendants(window, now);
}

bool prHtmlWindowHasStickyActivation(const PrHtmlWindow* window) {
    return htmlWindowClock(window) >= window->lastActivation;
}

bool prHtmlWindowHasTransientActivation(const PrHtmlWindow* window) {
    double now = htmlWindowClock(window);
    return now >= window->lastActivation && now < window->lastActivation + kHtmlTransientActivationDuration;
}

uint32_t prHtmlWindowLength(const PrHtmlWindow* window) {
    return window->frames.count;
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowFrame(PrHtmlWindow* window, uint32_t index) {
    return htmlWindowFrameInTreeOrder(window, index);
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowFrameFromName(PrHtmlWindow* window, PrStringView name) {
    if (name.size == 0)
        return nullptr;
    // A global's every unresolved name comes here, so the names are compared before the frames are ordered.
    const HtmlFrame* first = nullptr;
    for (uint32_t i = 0; i < window->frames.count; ++i) {
        const HtmlFrame* frame = &window->frames.data[i];
        const PrHtmlPage* page = htmlDocumentPage(frame->window->document);
        if (page == nullptr || prRawStringView(&page->targetName) != name)
            continue;
        if (first == nullptr) {
            first = frame;
            continue;
        }
        PrDomNode* current = prDomElementAsNode(first->container);
        if ((prDomNodeCompareDocumentPosition(current, prDomElementAsNode(frame->container)) &
                PrDomNodePosition_Preceding) != 0)
            first = frame;
    }
    return first != nullptr && htmlWindowIsSameOrigin(first->window, window) ? first->window : nullptr;
}

PrDomElement* PR_NULLABLE prHtmlWindowNamedItem(PrHtmlWindow* window, PrStringView name) {
    return htmlWindowNamedObject(window, name);
}

PrCssMediaQueryList* prHtmlWindowMatchMedia(PrHtmlWindow* window, PrStringView query) {
    return cssMediaQueryListCreate(&window->media, query);
}

int32_t prHtmlWindowInnerWidth(PrHtmlWindow* window) {
    PrLayoutVec extent = cssViewViewportExtent(window->document);
    double width;
    if (extent.x.raw != 0 || extent.y.raw != 0)
        width = prLayoutUnitToPxDouble(extent.x);
    else
        width = window->media.features[PrCssDescriptor_MediaWidth - kCssDescriptorMediaFirst].number;
    return prF64IsFinite(width) ? int32_t(prF64Floor(width + 0.5)) : 0;
}

int32_t prHtmlWindowInnerHeight(PrHtmlWindow* window) {
    PrLayoutVec extent = cssViewViewportExtent(window->document);
    double height;
    if (extent.x.raw != 0 || extent.y.raw != 0)
        height = prLayoutUnitToPxDouble(extent.y);
    else
        height = window->media.features[PrCssDescriptor_MediaHeight - kCssDescriptorMediaFirst].number;
    return prF64IsFinite(height) ? int32_t(prF64Floor(height + 0.5)) : 0;
}

double prHtmlWindowDevicePixelRatio(PrHtmlWindow* window) {
    return double(cssMediaEnvironmentPixelRatio(&window->media));
}

int32_t prHtmlWindowScreenWidth(PrHtmlWindow* window) {
    return mediaInteger(window, PrCssDescriptor_MediaDeviceWidth);
}

int32_t prHtmlWindowScreenHeight(PrHtmlWindow* window) {
    return mediaInteger(window, PrCssDescriptor_MediaDeviceHeight);
}

int32_t prHtmlWindowScreenColorDepth(PrHtmlWindow* window) {
    return mediaInteger(window, PrCssDescriptor_MediaColor) * 3;
}

double prHtmlWindowScrollX(PrHtmlWindow* window) {
    return prLayoutUnitToPxDouble(cssViewViewportScroll(window->document).x);
}

double prHtmlWindowScrollY(PrHtmlWindow* window) {
    return prLayoutUnitToPxDouble(cssViewViewportScroll(window->document).y);
}

void prHtmlWindowScroll(PrHtmlWindow* window, const PrCssScrollToOptions* options) {
    PrDomDocument* document = window->document;
    PrLayoutVec at = cssViewViewportScroll(document);
    auto units = [](double px) { return prLayoutUnitFromPxDouble(prF64IsFinite(px) ? px : 0); };
    cssViewScrollViewport(document, PrLayoutVec{
        options->hasLeft ? units(options->left) : at.x,
        options->hasTop ? units(options->top) : at.y,
    });
}

void prHtmlWindowScrollBy(PrHtmlWindow* window, const PrCssScrollToOptions* options) {
    auto units = [](double px) { return prLayoutUnitFromPxDouble(prF64IsFinite(px) ? px : 0); };
    PrLayoutVec delta{
        options->hasLeft ? units(options->left) : PrLayoutUnit{0},
        options->hasTop ? units(options->top) : PrLayoutUnit{0},
    };
    cssViewScrollViewportBy(window->document, delta);
}

int32_t prHtmlWindowSetTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, int32_t timeout, bool repeats) {
    return htmlWindowSetTimeout(window, handler, timeout, repeats, false);
}

void prHtmlWindowClearTimeout(PrHtmlWindow* window, int32_t id) {
    htmlWindowClearTimeout(window, id);
}

void prHtmlWindowRunAfterTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, double milliseconds) {
    htmlWindowRunAfterTimeout(window, handler, milliseconds);
}

uint32_t prHtmlWindowRequestAnimationFrame(PrHtmlWindow* window, const PrHtmlFrameRequestCallback* callback) {
    return htmlWindowRequestAnimationFrame(window, callback);
}

void prHtmlWindowCancelAnimationFrame(PrHtmlWindow* window, uint32_t handle) {
    htmlWindowCancelAnimationFrame(window, handle);
}

uint32_t prHtmlWindowRequestIdleCallback(PrHtmlWindow* window, const PrHtmlIdleRequestCallback* callback, uint32_t timeout) {
    return htmlWindowRequestIdleCallback(window, callback, timeout);
}

void prHtmlWindowCancelIdleCallback(PrHtmlWindow* window, uint32_t handle) {
    htmlWindowCancelIdleCallback(window, handle);
}

double prHtmlWindowIdleTimeRemaining(const PrHtmlWindow* window, double deadline) {
    return htmlWindowIdleTimeRemaining(window, deadline);
}

bool prHtmlWindowReportError(PrHtmlWindow* window, PrHtmlErrorEvent* event) {
    // An exception that an error listener throws is not dispatched again, which would never end if the listener always
    // throws.
    if (window->inErrorReportingMode)
        return true;
    window->inErrorReportingMode = true;
    bool notHandled = dispatchReport(window, event);
    window->inErrorReportingMode = false;
    return notHandled;
}

bool prHtmlWindowNotifyRejection(PrHtmlWindow* window, PrHtmlPromiseRejectionEvent* event) {
    return dispatchReport(window, event);
}

PrHtmlNavigatorCompatibilityMode prHtmlNavigatorCompatibilityMode(const PrHtmlWindow* window) {
    return window->navigatorCompatibilityMode;
}

PrStringView prHtmlNavigatorUserAgent(const PrHtmlWindow* window) {
    return window->userAgent.data != nullptr ? window->userAgent : PrStringView{""};
}

PrRawString prHtmlNavigatorAppVersion(const PrHtmlWindow* window) {
    PrStringView userAgent = prHtmlNavigatorUserAgent(window);
    if (!prStringStartsWith(userAgent, "Mozilla/5.0 ("))
        return prRawStringCopy("");
    PrStringView trail = prStringSlice(userAgent, 8, userAgent.size - 8);
    if (window->navigatorCompatibilityMode != PrHtmlNavigatorCompatibilityMode_Gecko)
        return prRawStringCopy(trail);
    if (prStringStartsWith(trail, "5.0 (Windows"))
        return prRawStringCopy("5.0 (Windows)");
    // The trail up to its first semicolon, closed with a parenthesis.
    size_t end = 0;
    while (end < trail.size && trail.data[end] != ';')
        ++end;
    PrTextBuffer text = {};
    prTextBufferAppend(&text, prStringSlice(trail, 0, end));
    prTextBufferAppendChar(&text, ')');
    return prTextBufferTake(&text);
}

PrStringView prHtmlNavigatorPlatform(const PrHtmlWindow*) {
#if defined(__APPLE__)
    return "MacIntel";
#elif defined(_WIN32)
    return "Win32";
#else
    return "Linux x86_64";
#endif
}

PrStringView prHtmlNavigatorProductSub(const PrHtmlWindow* window) {
    return window->navigatorCompatibilityMode == PrHtmlNavigatorCompatibilityMode_Gecko ? "20100101" : "20030107";
}

PrStringView prHtmlNavigatorVendor(const PrHtmlWindow* window) {
    switch (window->navigatorCompatibilityMode) {
        case PrHtmlNavigatorCompatibilityMode_Chrome: return "Google Inc.";
        case PrHtmlNavigatorCompatibilityMode_WebKit: return "Apple Computer, Inc.";
        default: return "";
    }
}

const PrCssUserAgentSheet* prHtmlUserAgentSheets(uint32_t* count) {
    *count = countOf(kUserAgentSheets);
    return kUserAgentSheets;
}

PrHtmlMapElement* PR_NULLABLE prHtmlMapElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlMapElement>(object, PrDomTag_Map);
}

PrDomHtmlCollection* prHtmlMapElementAreas(PrHtmlMapElement* map) {
    return domHtmlCollectionCreate(map, DomCollectionFilter_MapAreas, PrStringView{});
}
