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
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_slot.h"
#include "html/src/html_canvas.h"
#include "html/src/html_document.h"
#include "html/src/html_focus.h"
#include "html/src/html_interface.h"
#include "html/src/html_media.h"
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
    PrString data;
    PrString sourceOrigin;
    PrString targetOrigin;
};

void dropMessage(void* data) {
    PostedMessage* posted = static_cast<PostedMessage*>(data);
    prStringReset(&posted->data);
    prStringReset(&posted->sourceOrigin);
    prStringReset(&posted->targetOrigin);
    prMemoryFree(posted);
}

// html/#window-post-message-steps, in the task that those steps queue. Fires a message event at the target if the
// target's document has the origin that the message was posted to.
void deliverMessage(void* data) {
    PostedMessage* posted = static_cast<PostedMessage*>(data);
    PrString origin = htmlWindowOrigin(posted->target);
    bool matches = prStringIsNull(&posted->targetOrigin) ||
        prStringView(&origin) == prStringView(&posted->targetOrigin);
    prStringReset(&origin);
    if (matches) {
        PrHtmlMessageEvent* event =
            domEventCreate<PrHtmlMessageEvent>("message", prDomEventInitDefault(), PrDomEventKind_Message);
        event->trusted = true;
        event->message = allocZeroed<DomMessageEventData>();
        *event->message = DomMessageEventData{posted->data, posted->sourceOrigin, prStringEmpty(), posted->source};
        posted->data = prStringNull();
        posted->sourceOrigin = prStringNull();
        domEventDispatch(posted->target, event);
        domEventRelease(event);
    }
    dropMessage(posted);
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

// Returns the state of a media element that its window runs, or null for an element outside its window's document.
HtmlMedia* PR_NULLABLE mediaOf(const PrHtmlMediaElement* self) {
    return htmlMediaFromElement(const_cast<PrHtmlMediaElement*>(self));
}

constexpr PrStringView kDataPrefix = "data-";

// Writes the name of the element's custom data attribute into `out`, as its dataset exposes it. Returns false if the
// attribute is not a custom data attribute (html/#concept-domstringmap-pairs).
bool datasetName(const PrDomAttr* attribute, TextBuffer* out) {
    PrStringView name = prStringView(&attribute->name);
    if (attribute->space != PrDomNamespace_None || !stringStartsWith(name, kDataPrefix))
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        if (asciiIsUpper(name.data[i]))
            return false;
    }
    textClear(out);
    for (size_t i = kDataPrefix.size; i < name.size; ++i) {
        char c = name.data[i];
        if (c == '-' && i + 1 < name.size && asciiIsLower(name.data[i + 1]))
            c = asciiUpper(name.data[++i]);
        textAppendChar(out, c);
    }
    return true;
}

// Writes the attribute name for a dataset name: data- is prepended, and each ASCII uppercase letter is replaced by a
// hyphen and the lowercased letter.
void attributeName(PrStringView name, TextBuffer* out) {
    textAppend(out, kDataPrefix);
    for (size_t i = 0; i < name.size; ++i) {
        if (asciiIsUpper(name.data[i])) {
            textAppendChar(out, '-');
            textAppendChar(out, asciiLower(name.data[i]));
        } else {
            textAppendChar(out, name.data[i]);
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
    return isFiniteF64(number) ? int32_t(floorF64(number + 0.5)) : 0;
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
    document->contentType = prStringExternal(kXmlTypes[type - PrHtmlDomParserSupportedType_TextXml]);
    PrXmlError error = {};
    if (domXmlParse(document, source, {}, htmlXhtmlEntities(), &error) == PrStatus_Syntax)
        htmlShowXmlError(document, error);
    return document;
}

PrString prHtmlInnerHtml(const PrDomNode* node) {
    TextBuffer text;
    htmlSerializeChildren(&text, node);
    return textTake(&text);
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

PrString prHtmlOuterHtml(const PrDomElement* element) {
    TextBuffer text;
    htmlSerializeNode(&text, element);
    return textTake(&text);
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
    TextBuffer text;
    if (value < 0)
        textAppendChar(&text, '-');
    textAppendU64(&text, value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value));
    domSetAttribute(element, PrDomNamespace_None, "tabindex", textView(&text));
    textReset(&text);
}

PrHtmlWindow* PR_NULLABLE prHtmlDocumentDefaultView(PrDomDocument* document) {
    PrDomEventTarget* global = document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return nullptr;
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(global);
    return window->document == document ? window : nullptr;
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

PrHtmlDir prHtmlElementDir(const PrHtmlElement* self) {
    // html/#dom-dir reflects the attribute, limited to its known values.
    switch (domDir(domAttributeValue(self, "dir"))) {
        case DomDir_Ltr: return PrHtmlDir_Ltr;
        case DomDir_Rtl: return PrHtmlDir_Rtl;
        case DomDir_Auto: return PrHtmlDir_Auto;
        default: return PrHtmlDir_None;
    }
}

void prHtmlElementSetDir(PrHtmlElement* self, PrStringView value) {
    domSetAttribute(self, PrDomNamespace_None, "dir", value);
}

PrHtmlHidden prHtmlElementHidden(const PrHtmlElement* self) {
    const PrDomAttr* hidden = domAttribute(self, PrDomNamespace_None, "hidden");
    if (hidden == nullptr)
        return PrHtmlHidden_NotHidden;
    return asciiEqualsFold(prStringView(&hidden->value), "until-found") ? PrHtmlHidden_UntilFound : PrHtmlHidden_Hidden;
}

void prHtmlElementSetHidden(PrHtmlElement* self, PrHtmlHidden value) {
    if (value == PrHtmlHidden_NotHidden)
        domRemoveAttribute(self, PrDomNamespace_None, "hidden");
    else
        domSetAttribute(self, PrDomNamespace_None, "hidden", value == PrHtmlHidden_UntilFound ? "until-found" : "");
}

PrString prHtmlElementInnerText(const PrHtmlElement* self) {
    // html/#get-the-text-steps: the descendant text content of an element that is not being rendered.
    return domNodeTextContent(self);
}

void prHtmlElementSetInnerText(PrHtmlElement* self, PrStringView value) {
    domReplaceAll(self, renderedTextFragment(self->document, value));
}

PrDomElement* PR_NULLABLE prHtmlElementOffsetParent(PrHtmlElement* self) {
    return cssViewOffsetParent(self);
}

int32_t prHtmlElementOffsetTop(PrHtmlElement* self) {
    return int32_t(floorF64(cssViewOffsetRect(self).y + 0.5));
}

int32_t prHtmlElementOffsetLeft(PrHtmlElement* self) {
    return int32_t(floorF64(cssViewOffsetRect(self).x + 0.5));
}

int32_t prHtmlElementOffsetWidth(PrHtmlElement* self) {
    return int32_t(floorF64(cssViewOffsetRect(self).width + 0.5));
}

int32_t prHtmlElementOffsetHeight(PrHtmlElement* self) {
    return int32_t(floorF64(cssViewOffsetRect(self).height + 0.5));
}

PrString prHtmlElementReflectedUrl(const PrHtmlElement* self, PrStringView name) {
    const PrDomAttr* attribute = domAttribute(self, PrDomNamespace_None, name);
    if (attribute == nullptr)
        return prStringEmpty();
    PrUrl* url = nullptr;
    PrString out;
    if (domDocumentParseUrl(self->document, prStringView(&attribute->value), &url) == PrStatus_Ok)
        out = prUrlSerialize(url, PrUrlSerialization_Whole);
    else
        out = prStringClone(&attribute->value);
    prUrlDestroy(url);
    return out;
}

int64_t prHtmlElementReflectedInteger(const PrHtmlElement* self, PrStringView name, PrHtmlReflectedInteger kind,
        int64_t fallback) {
    int64_t value = 0;
    if (!domParseInteger(domAttributeValue(self, name), &value) || value < 0 || value > 2147483647 ||
            (kind == PrHtmlReflectedInteger_Positive && value == 0))
        return fallback;
    return value;
}

PrStatus prHtmlElementSetReflectedInteger(PrHtmlElement* self, PrStringView name, PrHtmlReflectedInteger kind,
        int64_t value, int64_t fallback) {
    if ((kind == PrHtmlReflectedInteger_NonNegative && value < 0) || (kind == PrHtmlReflectedInteger_Positive && value == 0))
        return PrStatus_OutOfRange;
    TextBuffer text;
    int64_t set = value >= 0 && value <= 2147483647 ? value : fallback;
    if (set < 0)
        textAppendChar(&text, '-');
    textAppendU64(&text, uint64_t(set < 0 ? -set : set));
    domSetAttribute(self, PrDomNamespace_None, name, textView(&text));
    textReset(&text);
    return PrStatus_Ok;
}

uint32_t prHtmlDatasetNames(const PrDomElement* element, PrString* PR_NULLABLE items, uint32_t capacity) {
    uint32_t count = 0;
    TextBuffer name;
    for (const PrDomAttr* attribute = firstAttribute(element);
            attribute != nullptr; attribute = nextAttribute(attribute)) {
        if (!datasetName(attribute, &name))
            continue;
        if (count < capacity)
            items[count] = prStringCopy(textView(&name));
        ++count;
    }
    textReset(&name);
    return count;
}

PrStringView prHtmlDatasetGet(const PrDomElement* element, PrStringView name) {
    PrStringView value = {};
    TextBuffer candidate;
    for (const PrDomAttr* attribute = firstAttribute(element);
            attribute != nullptr; attribute = nextAttribute(attribute)) {
        if (datasetName(attribute, &candidate) && textView(&candidate) == name) {
            value = prStringView(&attribute->value);
            break;
        }
    }
    textReset(&candidate);
    return value;
}

PrStatus prHtmlDatasetSet(PrDomElement* element, PrStringView name, PrStringView value) {
    for (size_t i = 0; i + 1 < name.size; ++i) {
        if (name.data[i] == '-' && asciiIsLower(name.data[i + 1]))
            return PrStatus_Syntax;
    }
    TextBuffer attribute;
    attributeName(name, &attribute);
    PrStatus status = PrStatus_InvalidData;
    if (domIsValidAttributeLocalName(textView(&attribute))) {
        domSetAttribute(element, PrDomNamespace_None, textView(&attribute), value);
        status = PrStatus_Ok;
    }
    textReset(&attribute);
    return status;
}

void prHtmlDatasetDelete(PrDomElement* element, PrStringView name) {
    TextBuffer attribute;
    attributeName(name, &attribute);
    if (PrDomAttr* found = domAttributeByName(element, textView(&attribute)))
        domRemoveAttributeNode(element, found);
    textReset(&attribute);
}

PrHtmlDialogElement* PR_NULLABLE prHtmlDialogElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlDialogElement>(object, PrDomTag_Dialog);
}

bool prHtmlDialogElementOpen(const PrHtmlDialogElement* self) {
    return domAttribute(self, PrDomNamespace_None, "open") != nullptr;
}

void prHtmlDialogElementSetOpen(PrHtmlDialogElement* self, bool value) {
    if (value)
        domSetAttribute(self, PrDomNamespace_None, "open", "");
    else
        domRemoveAttribute(self, PrDomNamespace_None, "open");
}

PrStringView prHtmlDialogElementReturnValue(const PrHtmlDialogElement* self) {
    const PrDomElement* element = self;
    return element->state != nullptr ? prStringView(&element->state->returnValue) : "";
}

void prHtmlDialogElementSetReturnValue(PrHtmlDialogElement* self, PrStringView value) {
    prStringReplace(&domElementState(self)->returnValue, value);
}

PrStatus prHtmlDialogElementShow(PrHtmlDialogElement* self) {
    bool modal = self->state != nullptr && self->state->modal;
    if (prHtmlDialogElementOpen(self))
        return modal ? PrStatus_InvalidState : PrStatus_Ok;
    prHtmlDialogElementSetOpen(self, true);
    htmlFocusEndAutofocus(self->document);
    return PrStatus_Ok;
}

PrStatus prHtmlDialogElementShowModal(PrHtmlDialogElement* self) {
    bool modal = self->state != nullptr && self->state->modal;
    if (prHtmlDialogElementOpen(self))
        return modal ? PrStatus_Ok : PrStatus_InvalidState;
    if (!domIsConnected(self))
        return PrStatus_InvalidState;
    prHtmlDialogElementSetOpen(self, true);
    domElementState(self)->modal = true;
    htmlFocusEndAutofocus(self->document);
    return PrStatus_Ok;
}

void prHtmlDialogElementClose(PrHtmlDialogElement* self, PrStringView result) {
    if (!prHtmlDialogElementOpen(self))
        return;
    prHtmlDialogElementSetOpen(self, false);
    DomElementState* state = domElementState(self);
    state->modal = false;
    if (result.data != nullptr)
        prStringReplace(&state->returnValue, result);
    const DomDocumentHost* host = self->document->host;
    if (host == nullptr)
        return;
    // The task holds the dialog.
    domNodeHold(self);
    host->queueTask(host->context, [](void* data) {
        PrDomElement* dialog = static_cast<PrDomElement*>(data);
        domEventFire(dialog, "close");
        domNodeUnhold(dialog);
    }, self, [](void* data) { domNodeUnhold(static_cast<PrDomElement*>(data)); });
}

void prHtmlDialogElementRequestClose(PrHtmlDialogElement* self, PrStringView result) {
    if (!prHtmlDialogElementOpen(self))
        return;
    PrDomEventInit init = prDomEventInitDefault();
    init.cancelable = true;
    if (domEventFire(self, "cancel", init))
        prHtmlDialogElementClose(self, result);
}

PrHtmlIFrameElement* PR_NULLABLE prHtmlIFrameElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlIFrameElement>(object, PrDomTag_Iframe);
}

PrHtmlWindow* PR_NULLABLE prHtmlIFrameElementContentWindow(PrHtmlIFrameElement* self) {
    return htmlFrameWindow(self);
}

PrDomDocument* PR_NULLABLE prHtmlIFrameElementContentDocument(PrHtmlIFrameElement* self) {
    PrHtmlWindow* window = htmlFrameWindow(self);
    return window != nullptr ? window->document : nullptr;
}

PrHtmlImageElement* PR_NULLABLE prHtmlImageElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlImageElement>(object, PrDomTag_Img);
}

PrDomImageState prHtmlImageElementState(const PrHtmlImageElement* self) {
    const DomElementState* state = self->state;
    return state != nullptr ? state->image : PrDomImageState_Unavailable;
}

bool prHtmlImageElementComplete(const PrHtmlImageElement* self) {
    const PrDomAttr* src = domAttribute(self, PrDomNamespace_None, "src");
    if (src == nullptr || prStringView(&src->value).size == 0)
        return true;
    PrDomImageState state = prHtmlImageElementState(self);
    return state == PrDomImageState_CompletelyAvailable || state == PrDomImageState_Broken;
}

PrStringView prHtmlImageElementCurrentSrc(const PrHtmlImageElement* self) {
    const DomElementState* state = self->state;
    return state != nullptr && !prStringIsNull(&state->imageUrl) ? prStringView(&state->imageUrl) : "";
}

uint32_t prHtmlImageElementNaturalWidth(const PrHtmlImageElement* self) {
    return htmlImageDimension(self, true);
}

uint32_t prHtmlImageElementNaturalHeight(const PrHtmlImageElement* self) {
    return htmlImageDimension(self, false);
}

uint32_t prHtmlImageElementWidth(const PrHtmlImageElement* self) {
    return htmlImageDimension(self, true);
}

uint32_t prHtmlImageElementHeight(const PrHtmlImageElement* self) {
    return htmlImageDimension(self, false);
}

void prHtmlImageElementSetWidth(PrHtmlImageElement* self, uint32_t value) {
    htmlSetReflectedUnsigned(self, "width", value);
}

void prHtmlImageElementSetHeight(PrHtmlImageElement* self, uint32_t value) {
    htmlSetReflectedUnsigned(self, "height", value);
}

bool prHtmlImageElementImageSource(PrHtmlImageElement* self, PrCanvasImageSource* out) {
    *out = PrCanvasImageSource{};
    const DomElementState* state = self->state;
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

uint32_t prHtmlCanvasElementWidth(const PrHtmlCanvasElement* self) {
    return domReflectedUnsigned(self, "width", 300);
}

uint32_t prHtmlCanvasElementHeight(const PrHtmlCanvasElement* self) {
    return domReflectedUnsigned(self, "height", 150);
}

PrStatus prHtmlCanvasElementSetWidth(PrHtmlCanvasElement* self, uint32_t value) {
    if (prHtmlCanvasElementContextMode(self) == PrHtmlCanvasContextMode_Placeholder)
        return PrStatus_InvalidState;
    htmlSetReflectedUnsigned(self, "width", value, 300);
    return PrStatus_Ok;
}

PrStatus prHtmlCanvasElementSetHeight(PrHtmlCanvasElement* self, uint32_t value) {
    if (prHtmlCanvasElementContextMode(self) == PrHtmlCanvasContextMode_Placeholder)
        return PrStatus_InvalidState;
    htmlSetReflectedUnsigned(self, "height", value, 150);
    return PrStatus_Ok;
}

PrHtmlCanvasContextMode prHtmlCanvasElementContextMode(const PrHtmlCanvasElement* self) {
    const HtmlCanvas* canvas = htmlCanvasFind(self);
    return canvas != nullptr ? canvas->mode : PrHtmlCanvasContextMode_None;
}

PrStatus prHtmlCanvasElementGetContext2D(PrHtmlCanvasElement* self, const PrCanvasSettings* PR_NULLABLE settings,
        PrCanvas* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    HtmlCanvas* canvas = htmlCanvasFromElement(self);
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

PrStatus prHtmlCanvasElementToDataUrl(PrHtmlCanvasElement* self, PrStringView type, PrString* out) {
    return htmlCanvasToDataUrl(self, type, out);
}

bool prHtmlCanvasElementImageSource(PrHtmlCanvasElement* self, PrCanvasImageSource* out) {
    return htmlCanvasImageSource(self, out);
}

PrHtmlAnchorElement* PR_NULLABLE prHtmlAnchorElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlAnchorElement>(object, PrDomTag_A);
}

PrHtmlAreaElement* PR_NULLABLE prHtmlAreaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlAreaElement>(object, PrDomTag_Area);
}

PrString prHtmlHyperlinkComponent(const PrHtmlElement* self, PrUrlComponent component) {
    PR_ASSERT(domIsHtml(self, PrDomTag_A) || domIsHtml(self, PrDomTag_Area));
    PrUrl* url = hyperlinkUrl(self);
    if (url == nullptr) {
        PrStringView href = domAttributeValue(self, "href");
        if (component == PrUrlComponent_Href && href.data != nullptr)
            return prStringCopy(href);
        return component == PrUrlComponent_Protocol ? prStringExternal(":") : prStringEmpty();
    }
    PrString value = prUrlComponent(url, component);
    prUrlDestroy(url);
    return value;
}

PrStatus prHtmlHyperlinkSetComponent(PrHtmlElement* self, PrUrlComponent component, PrStringView value) {
    PR_ASSERT(domIsHtml(self, PrDomTag_A) || domIsHtml(self, PrDomTag_Area));
    if (component == PrUrlComponent_Origin)
        return PrStatus_ReadOnly;
    if (component == PrUrlComponent_Href)
        return prDomElementSetAttribute(self, "href", value);
    PrUrl* url = hyperlinkUrl(self);
    if (url == nullptr)
        return PrStatus_Ok;
    // A value that does not parse leaves the URL as it was, and the attribute is set to it all the same
    // (html/#update-href).
    prUrlSetComponent(url, component, value);
    PrString href = prUrlSerialize(url, PrUrlSerialization_Whole);
    prUrlDestroy(url);
    PrStatus status = prDomElementSetAttribute(self, "href", prStringView(&href));
    prStringReset(&href);
    return status;
}

PrHtmlBaseElement* PR_NULLABLE prHtmlBaseElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlBaseElement>(object, PrDomTag_Base);
}

PrString prHtmlBaseElementHref(const PrHtmlBaseElement* self) {
    PrStringView href = domAttributeValue(self, "href");
    if (href.data == nullptr)
        href = "";
    PrUrl* url = nullptr;
    if (prUrlCreate(href, self->document->url, self->document->charset, &url) != PrStatus_Ok)
        return prStringCopy(href);
    PrString value = prUrlSerialize(url, PrUrlSerialization_Whole);
    prUrlDestroy(url);
    return value;
}

PrHtmlLinkElement* PR_NULLABLE prHtmlLinkElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlLinkElement>(object, PrDomTag_Link);
}

PrCssStyleSheet* PR_NULLABLE prHtmlLinkElementSheet(PrHtmlLinkElement* self) {
    return cssElementSheet(self);
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

bool prHtmlMediaElementIsVideo(const PrHtmlMediaElement* self) {
    return self->tag == PrDomTag_Video;
}

PrHtmlMediaError prHtmlMediaElementError(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->error : PrHtmlMediaError_None;
}

PrStringView prHtmlMediaElementErrorMessage(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? prStringView(&media->errorMessage) : "";
}

PrStringView prHtmlMediaElementCurrentSrc(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? prStringView(&media->currentSrc) : "";
}

PrHtmlMediaNetworkState prHtmlMediaElementNetworkState(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->networkState : PrHtmlMediaNetworkState_Empty;
}

PrHtmlMediaReadyState prHtmlMediaElementReadyState(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->readyState : PrHtmlMediaReadyState_HaveNothing;
}

void prHtmlMediaElementLoad(PrHtmlMediaElement* self) {
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaLoad(media);
}

PrHtmlCanPlay prHtmlMediaElementCanPlayType(const PrHtmlMediaElement*, PrStringView type) {
    return htmlCanPlayType(type);
}

bool prHtmlMediaElementSeeking(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr && media->seeking;
}

double prHtmlMediaElementCurrentTime(PrHtmlMediaElement* self) {
    HtmlMedia* media = mediaOf(self);
    return media != nullptr ? htmlMediaCurrentTime(media) : 0;
}

void prHtmlMediaElementSetCurrentTime(PrHtmlMediaElement* self, double seconds) {
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaSeek(media, seconds);
}

void prHtmlMediaElementFastSeek(PrHtmlMediaElement* self, double seconds) {
    prHtmlMediaElementSetCurrentTime(self, seconds);
}

double prHtmlMediaElementDuration(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->duration : kNanF64;
}

bool prHtmlMediaElementPaused(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media == nullptr || media->paused;
}

bool prHtmlMediaElementEnded(PrHtmlMediaElement* self) {
    HtmlMedia* media = mediaOf(self);
    return media != nullptr && htmlMediaEnded(media);
}

double prHtmlMediaElementDefaultPlaybackRate(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->defaultRate : 1;
}

void prHtmlMediaElementSetDefaultPlaybackRate(PrHtmlMediaElement* self, double rate) {
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaSetRate(media, rate, true);
}

double prHtmlMediaElementPlaybackRate(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->rate : 1;
}

PrStatus prHtmlMediaElementSetPlaybackRate(PrHtmlMediaElement* self, double rate) {
    // html/#dom-media-playbackrate: the engine can play its timeline at any rate, forwards and backwards.
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaSetRate(media, rate, false);
    return PrStatus_Ok;
}

bool prHtmlMediaElementPreservesPitch(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media == nullptr || media->preservesPitch;
}

void prHtmlMediaElementSetPreservesPitch(PrHtmlMediaElement* self, bool preserves) {
    if (HtmlMedia* media = mediaOf(self))
        media->preservesPitch = preserves;
}

uint32_t prHtmlMediaElementRanges(PrHtmlMediaElement* self, PrHtmlMediaRanges which,
        double* PR_NULLABLE starts, double* PR_NULLABLE ends, uint32_t capacity) {
    HtmlMedia* media = mediaOf(self);
    if (media == nullptr)
        return 0;
    if (which == PrHtmlMediaRanges_Played) {
        uint32_t count = media->played.count / 2;
        for (uint32_t i = 0; i < count && i < capacity; ++i) {
            starts[i] = media->played.data[i * 2];
            ends[i] = media->played.data[i * 2 + 1];
        }
        return count;
    }
    // The whole resource is fetched, so all of its data is available to seek to.
    if (media->readyState < PrHtmlMediaReadyState_HaveMetadata || isNanF64(media->duration))
        return 0;
    if (capacity != 0) {
        starts[0] = 0;
        ends[0] = media->duration;
    }
    return 1;
}

void prHtmlMediaElementPlay(PrHtmlMediaElement* self, const PrHtmlPlayPromise* promise) {
    HtmlMedia* media = mediaOf(self);
    if (media == nullptr) {
        // An element outside its window's document has no resource to play.
        promise->settle(promise->context, "NotSupportedError");
        promise->release(promise->context);
        return;
    }
    htmlMediaPlay(media, *promise);
}

void prHtmlMediaElementPause(PrHtmlMediaElement* self) {
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaPause(media);
}

double prHtmlMediaElementVolume(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->volume : 1;
}

PrStatus prHtmlMediaElementSetVolume(PrHtmlMediaElement* self, double volume) {
    if (volume < 0 || volume > 1)
        return PrStatus_OutOfRange;
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaSetVolume(media, volume, prHtmlMediaElementMuted(self));
    return PrStatus_Ok;
}

bool prHtmlMediaElementMuted(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    if (media != nullptr && media->mutedSet)
        return media->muted;
    // html/#dom-media-muted: an element follows its muted content attribute until a script sets muted.
    return domAttribute(self, PrDomNamespace_None, "muted") != nullptr;
}

void prHtmlMediaElementSetMuted(PrHtmlMediaElement* self, bool muted) {
    if (HtmlMedia* media = mediaOf(self))
        htmlMediaSetVolume(media, media->volume, muted);
}

uint32_t prHtmlMediaElementVideoWidth(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->videoWidth : 0;
}

uint32_t prHtmlMediaElementVideoHeight(const PrHtmlMediaElement* self) {
    const HtmlMedia* media = mediaOf(self);
    return media != nullptr ? media->videoHeight : 0;
}

PrHtmlSlotElement* PR_NULLABLE prHtmlSlotElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomNode* node = domNodeFromTarget(object);
    return node != nullptr && domIsSlot(node) ? static_cast<PrHtmlSlotElement*>(domElementFromNode(node)) : nullptr;
}

uint32_t prHtmlSlotElementAssignedNodes(PrHtmlSlotElement* self, bool flatten, PrDomNode* PR_NONNULL* PR_NULLABLE items,
        uint32_t capacity) {
    Array<PrDomNode* PR_NONNULL> nodes;
    domSlotAssignedNodes(self, flatten, &nodes);
    for (uint32_t i = 0; items != nullptr && i < nodes.count && i < capacity; ++i)
        items[i] = nodes.data[i];
    uint32_t count = nodes.count;
    nodes.reset();
    return count;
}

void prHtmlSlotElementAssign(PrHtmlSlotElement* self, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count) {
    domSlotAssign(self, reinterpret_cast<PrDomNode* const*>(nodes), count);
}

PrHtmlScriptElement* PR_NULLABLE prHtmlScriptElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlScriptElement>(object, PrDomTag_Script);
}

bool prHtmlScriptElementAsync(const PrHtmlScriptElement* self) {
    bool forceAsync = (domScriptFlags(self) & DomScriptFlag_NotForceAsync) == 0;
    return forceAsync || domAttributeValue(self, "async").data != nullptr;
}

void prHtmlScriptElementSetAsync(PrHtmlScriptElement* self, bool value) {
    domScriptSetFlags(self, domScriptFlags(self) | DomScriptFlag_NotForceAsync);
    if (value)
        domSetAttribute(self, PrDomNamespace_None, "async", "");
    else
        domRemoveAttribute(self, PrDomNamespace_None, "async");
}

PrString prHtmlScriptElementText(const PrHtmlScriptElement* self) {
    return domChildTextContent(self);
}

void prHtmlScriptElementSetText(PrHtmlScriptElement* self, PrStringView value) {
    domStringReplaceAll(self, value);
}

bool prHtmlScriptElementSupports(PrStringView type) {
    return type == "classic" || type == "module";
}

PrHtmlStyleElement* PR_NULLABLE prHtmlStyleElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlStyleElement>(object, PrDomTag_Style);
}

PrCssStyleSheet* PR_NULLABLE prHtmlStyleElementSheet(PrHtmlStyleElement* self) {
    return cssElementSheet(self);
}

PrHtmlTemplateElement* PR_NULLABLE prHtmlTemplateElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTemplateElement>(object, PrDomTag_Template);
}

PrDomDocumentFragment* prHtmlTemplateElementContent(PrHtmlTemplateElement* self) {
    return static_cast<PrDomDocumentFragment*>(domElementContents(self));
}

bool prHtmlTemplateElementShadowRootMode(const PrHtmlTemplateElement* self, PrDomShadowRootMode* out) {
    PrStringView mode = domAttributeValue(self, "shadowrootmode");
    if (asciiEqualsFold(mode, "open"))
        *out = PrDomShadowRootMode_Open;
    else if (asciiEqualsFold(mode, "closed"))
        *out = PrDomShadowRootMode_Closed;
    else
        return false;
    return true;
}

PrDomSlotAssignmentMode prHtmlTemplateElementShadowRootSlotAssignment(const PrHtmlTemplateElement* self) {
    PrStringView value = domAttributeValue(self, "shadowrootslotassignment");
    return asciiEqualsFold(value, "manual") ? PrDomSlotAssignmentMode_Manual : PrDomSlotAssignmentMode_Named;
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

PrStringView prHtmlErrorEventMessage(const PrHtmlErrorEvent* self) {
    return prStringView(&self->error->message);
}

PrStringView prHtmlErrorEventFilename(const PrHtmlErrorEvent* self) {
    return prStringView(&self->error->filename);
}

uint32_t prHtmlErrorEventLineno(const PrHtmlErrorEvent* self) {
    return self->error->lineno;
}

uint32_t prHtmlErrorEventColno(const PrHtmlErrorEvent* self) {
    return self->error->colno;
}

PrHtmlMessageEvent* PR_NULLABLE prHtmlMessageEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Message ? static_cast<PrHtmlMessageEvent*>(object) : nullptr;
}

PrHtmlMessageEvent* prHtmlMessageEventCreate(PrStringView type, const PrHtmlMessageEventInit* PR_NULLABLE init) {
    PrHtmlMessageEventInit values = init != nullptr ? *init : prHtmlMessageEventInitDefault();
    PrHtmlMessageEvent* event = domEventCreate<PrHtmlMessageEvent>(type, values.event, PrDomEventKind_Message);
    event->message = allocZeroed<DomMessageEventData>();
    *event->message = DomMessageEventData{
        prStringNull(), prStringCopy(values.origin),
        prStringCopy(values.lastEventId),
        values.source,
    };
    return event;
}

PrStringView prHtmlMessageEventSerializedData(const PrHtmlMessageEvent* self) {
    const PrString* data = &self->message->data;
    return prStringIsNull(data) ? PrStringView{} : prStringView(data);
}

PrStringView prHtmlMessageEventOrigin(const PrHtmlMessageEvent* self) {
    return prStringView(&self->message->origin);
}

PrStringView prHtmlMessageEventLastEventId(const PrHtmlMessageEvent* self) {
    return prStringView(&self->message->lastEventId);
}

PrDomEventTarget* PR_NULLABLE prHtmlMessageEventSource(PrHtmlMessageEvent* self) {
    return self->message->source;
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

PrHtmlPreferenceValue prHtmlPreferenceObjectOverride(const PrHtmlPreferenceObject* self) {
    PrCssDescriptor feature = kPreferences[self->preference].feature;
    return preferenceValueOf(environmentOf(self)->overrides[feature - kCssDescriptorMediaFirst].keyword);
}

PrHtmlPreferenceValue prHtmlPreferenceObjectValue(const PrHtmlPreferenceObject* self) {
    return preferenceValueOf(cssMediaEnvironmentKeyword(environmentOf(self), kPreferences[self->preference].feature));
}

uint32_t prHtmlPreferenceObjectValidValues(const PrHtmlPreferenceObject* self, PrHtmlPreferenceValue* PR_NULLABLE items,
        uint32_t capacity) {
    const PreferenceFeature& feature = kPreferences[self->preference];
    for (uint32_t i = 0; items != nullptr && i < feature.count && i < capacity; ++i)
        items[i] = feature.values[i];
    return feature.count;
}

void prHtmlPreferenceObjectClearOverride(PrHtmlPreferenceObject* self) {
    PrHtmlPreferenceObject* preference = self;
    PrCssDescriptor feature = kPreferences[preference->preference].feature;
    CssMediaEnvironment* environment = &preference->window->media;
    if (environment->overrides[feature - kCssDescriptorMediaFirst].keyword == PrCssKeyword_Unknown)
        return;
    cssMediaEnvironmentOverride(environment, feature, PrCssKeyword_Unknown);
    cssPresentationDocumentMediaChanged(preference->window->document);
    domEventFire(preference, "change");
}

PrStatus prHtmlPreferenceObjectRequestOverride(PrHtmlPreferenceObject* self, PrHtmlPreferenceValue value) {
    if (value == PrHtmlPreferenceValue_None) {
        prHtmlPreferenceObjectClearOverride(self);
        return PrStatus_Ok;
    }
    PrHtmlPreferenceObject* preference = self;
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

double prHtmlPerformanceNow(const PrHtmlPerformance* self) {
    return htmlWindowNow(self->window);
}

double prHtmlPerformanceTimeOrigin(const PrHtmlPerformance* self) {
    return self->window->timeOriginWall;
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

PrHtmlOrientationType prHtmlScreenOrientationType(const PrHtmlScreenOrientation* self) {
    if (mediaInteger(self->window, PrCssDescriptor_MediaDeviceHeight) >
            mediaInteger(self->window, PrCssDescriptor_MediaDeviceWidth))
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

void prHtmlDocumentTimelineDestroy(PrHtmlDocumentTimeline* PR_NULLABLE self) {
    prMemoryFree(self);
}

bool prHtmlDocumentTimelineCurrentTime(const PrHtmlDocumentTimeline* self, double* out) {
    PrHtmlWindow* window = prHtmlDocumentDefaultView(self->document);
    if (window == nullptr)
        return false;
    *out = htmlWindowFrameTime(window) - self->originTime;
    return true;
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_Window ? static_cast<PrHtmlWindow*>(object) : nullptr;
}

PrDomDocument* prHtmlWindowDocument(PrHtmlWindow* self) {
    return self->document;
}

PrDomAgent* prHtmlWindowAgent(PrHtmlWindow* self) {
    return self->agent;
}

void prHtmlWindowSetCustomElementRegistry(PrHtmlWindow* self, const PrDomCustomElementRegistry* PR_NULLABLE registry) {
    self->customElements = registry;
    self->document->customElements = registry;
}

PrHtmlPreferenceObject* prHtmlWindowPreference(PrHtmlWindow* self, PrHtmlPreference preference) {
    return static_cast<PrHtmlPreferenceObject*>(&self->preferences[preference]);
}

PrHtmlPerformance* prHtmlWindowPerformance(PrHtmlWindow* self) {
    return static_cast<PrHtmlPerformance*>(&self->performance);
}

PrHtmlScreenOrientation* prHtmlWindowScreenOrientation(PrHtmlWindow* self) {
    return static_cast<PrHtmlScreenOrientation*>(&self->orientation);
}

PrHtmlWindow* PR_NULLABLE prHtmlWindowParent(PrHtmlWindow* self) {
    return self->parent;
}

PrStatus prHtmlWindowPostMessage(PrHtmlWindow* self, PrHtmlWindow* source, PrStringView serialized,
        PrStringView targetOrigin) {
    PrString origin = prStringNull();
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
    *posted = PostedMessage{self, source, prStringCopy(serialized), htmlWindowOrigin(source), origin};
    htmlWindowQueueTask(self, deliverMessage, posted, dropMessage);
    return PrStatus_Ok;
}

PrString prHtmlWindowLocation(const PrHtmlWindow* self, PrUrlComponent component) {
    return prUrlComponent(self->document->url, component);
}

PrStatus prHtmlWindowSetLocation(PrHtmlWindow* self, PrUrlComponent component, PrStringView value,
        PrHtmlHistoryHandling handling) {
    PrDomDocument* document = self->document;
    PrUrl* copy = nullptr;
    if (component == PrUrlComponent_Href) {
        PrStatus status = prUrlCreate(value, document->url, PrCharset_Utf8, &copy);
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
    PrString before = prUrlSerialize(document->url, PrUrlSerialization_ExcludeFragment);
    PrString after = prUrlSerialize(copy, PrUrlSerialization_ExcludeFragment);
    bool fragment = prStringView(&before) == prStringView(&after) && (component == PrUrlComponent_Hash || now.data != nullptr);
    prStringReset(&before);
    prStringReset(&after);
    if (!fragment) {
        PrString href = prUrlSerialize(copy, PrUrlSerialization_Whole);
        prUrlDestroy(copy);
        HtmlHistoryCommit commit =
            handling == PrHtmlHistoryHandling_Replace ? HtmlHistoryCommit_Replace : HtmlHistoryCommit_Push;
        bool navigated = htmlPageNavigate(self, prStringView(&href), commit, self->history->index);
        prStringReset(&href);
        return navigated ? PrStatus_Ok : PrStatus_NotSupported;
    }
    // html/#navigate-fragid: the URL that the document has already replaces its entry (html/#navigate), popstate fires,
    // hashchange follows if the fragment changed, and the document scrolls to the fragment.
    PrString old = prUrlSerialize(document->url, PrUrlSerialization_Whole);
    PrString next = prUrlSerialize(copy, PrUrlSerialization_Whole);
    if (prStringView(&old) == prStringView(&next))
        handling = PrHtmlHistoryHandling_Replace;
    prStringReset(&next);
    prUrlDestroy(document->url);
    document->url = copy;
    htmlHistoryUpdate(self, prStringNull(), handling);
    htmlHistoryNotify(self, prStringView(&old));
    prStringReset(&old);
    htmlWindowScrollToFragment(self);
    return PrStatus_Ok;
}

PrDomElement* PR_NULLABLE prHtmlWindowNamedItem(PrHtmlWindow* self, PrStringView name) {
    return htmlWindowNamedObject(self, name);
}

PrCssMediaQueryList* prHtmlWindowMatchMedia(PrHtmlWindow* self, PrStringView query) {
    return cssMediaQueryListCreate(&self->media, query);
}

int32_t prHtmlWindowInnerWidth(PrHtmlWindow* self) {
    PrLayoutVec extent = cssViewViewportExtent(self->document);
    double width;
    if (extent.x.raw != 0 || extent.y.raw != 0)
        width = prLayoutUnitToPxDouble(extent.x);
    else
        width = self->media.features[PrCssDescriptor_MediaWidth - kCssDescriptorMediaFirst].number;
    return isFiniteF64(width) ? int32_t(floorF64(width + 0.5)) : 0;
}

int32_t prHtmlWindowInnerHeight(PrHtmlWindow* self) {
    PrLayoutVec extent = cssViewViewportExtent(self->document);
    double height;
    if (extent.x.raw != 0 || extent.y.raw != 0)
        height = prLayoutUnitToPxDouble(extent.y);
    else
        height = self->media.features[PrCssDescriptor_MediaHeight - kCssDescriptorMediaFirst].number;
    return isFiniteF64(height) ? int32_t(floorF64(height + 0.5)) : 0;
}

double prHtmlWindowDevicePixelRatio(PrHtmlWindow* self) {
    return double(cssMediaEnvironmentPixelRatio(&self->media));
}

int32_t prHtmlWindowScreenWidth(PrHtmlWindow* self) {
    return mediaInteger(self, PrCssDescriptor_MediaDeviceWidth);
}

int32_t prHtmlWindowScreenHeight(PrHtmlWindow* self) {
    return mediaInteger(self, PrCssDescriptor_MediaDeviceHeight);
}

int32_t prHtmlWindowScreenColorDepth(PrHtmlWindow* self) {
    return mediaInteger(self, PrCssDescriptor_MediaColor) * 3;
}

double prHtmlWindowScrollX(PrHtmlWindow* self) {
    return prLayoutUnitToPxDouble(cssViewViewportScroll(self->document).x);
}

double prHtmlWindowScrollY(PrHtmlWindow* self) {
    return prLayoutUnitToPxDouble(cssViewViewportScroll(self->document).y);
}

void prHtmlWindowScroll(PrHtmlWindow* self, const PrCssScrollToOptions* options) {
    PrDomDocument* document = self->document;
    PrLayoutVec at = cssViewViewportScroll(document);
    auto units = [](double px) { return prLayoutUnitFromPxDouble(isFiniteF64(px) ? px : 0); };
    cssViewScrollViewport(document, PrLayoutVec{
        options->hasLeft ? units(options->left) : at.x,
        options->hasTop ? units(options->top) : at.y,
    });
}

void prHtmlWindowScrollBy(PrHtmlWindow* self, const PrCssScrollToOptions* options) {
    auto units = [](double px) { return prLayoutUnitFromPxDouble(isFiniteF64(px) ? px : 0); };
    PrLayoutVec delta{
        options->hasLeft ? units(options->left) : PrLayoutUnit{0},
        options->hasTop ? units(options->top) : PrLayoutUnit{0},
    };
    cssViewScrollViewportBy(self->document, delta);
}

int32_t prHtmlWindowSetTimeout(PrHtmlWindow* self, const PrHtmlTimerHandler* handler, int32_t timeout, bool repeats) {
    return htmlWindowSetTimeout(self, handler, timeout, repeats, false);
}

void prHtmlWindowClearTimeout(PrHtmlWindow* self, int32_t id) {
    htmlWindowClearTimeout(self, id);
}

void prHtmlWindowRunAfterTimeout(PrHtmlWindow* self, const PrHtmlTimerHandler* handler, double milliseconds) {
    htmlWindowRunAfterTimeout(self, handler, milliseconds);
}

uint32_t prHtmlWindowRequestAnimationFrame(PrHtmlWindow* self, const PrHtmlFrameRequestCallback* callback) {
    return htmlWindowRequestAnimationFrame(self, callback);
}

void prHtmlWindowCancelAnimationFrame(PrHtmlWindow* self, uint32_t handle) {
    htmlWindowCancelAnimationFrame(self, handle);
}

bool prHtmlWindowReportError(PrHtmlWindow* self, PrHtmlErrorEvent* event) {
    return dispatchReport(self, event);
}

bool prHtmlWindowNotifyRejection(PrHtmlWindow* self, PrHtmlPromiseRejectionEvent* event) {
    return dispatchReport(self, event);
}

PrHtmlNavigatorCompatibilityMode prHtmlNavigatorCompatibilityMode(const PrHtmlWindow* window) {
    return window->navigatorCompatibilityMode;
}

PrStringView prHtmlNavigatorUserAgent(const PrHtmlWindow* window) {
    return window->userAgent.data != nullptr ? window->userAgent : PrStringView{""};
}

PrString prHtmlNavigatorAppVersion(const PrHtmlWindow* window) {
    PrStringView userAgent = prHtmlNavigatorUserAgent(window);
    if (!stringStartsWith(userAgent, "Mozilla/5.0 ("))
        return prStringCopy("");
    PrStringView trail = stringSlice(userAgent, 8, userAgent.size - 8);
    if (window->navigatorCompatibilityMode != PrHtmlNavigatorCompatibilityMode_Gecko)
        return prStringCopy(trail);
    if (stringStartsWith(trail, "5.0 (Windows"))
        return prStringCopy("5.0 (Windows)");
    // The trail up to its first semicolon, closed with a parenthesis.
    size_t end = 0;
    while (end < trail.size && trail.data[end] != ';')
        ++end;
    TextBuffer text;
    textAppend(&text, stringSlice(trail, 0, end));
    textAppendChar(&text, ')');
    return textTake(&text);
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
