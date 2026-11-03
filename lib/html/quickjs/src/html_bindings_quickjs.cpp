// JavaScript binding of HTML.
//
// html - https://html.spec.whatwg.org/

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "pr/base64.h"
#include "pr/canvas_quickjs.h"
#include "pr/count_of.h"
#include "pr/css_quickjs.h"
#include "pr/css_typed_om_quickjs.h"
#include "pr/dom_abort.h"
#include "pr/dom_quickjs.h"
#include "pr/fetch_blob_quickjs.h"
#include "pr/fetch_quickjs.h"
#include "pr/float.h"
#include "pr/html.h"
#include "pr/html_console_quickjs.h"
#include "pr/html_history.h"
#include "pr/html_storage.h"
#include "pr/integer.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/random.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/custom_elements_quickjs.h"
#include "html/quickjs/src/html_clone_quickjs.h"
#include "html/quickjs/src/html_elements_quickjs.h"
#include "html/quickjs/src/html_forms_quickjs.h"
#include "html/quickjs/src/html_media_source_quickjs.h"
#include "html/quickjs/src/html_message_quickjs.h"
#include "html/quickjs/src/html_performance_quickjs.h"
#include "html/quickjs/src/html_reflect_quickjs.h"

namespace pr_impl {

JSClassID htmlElementClass;
JSClassID htmlPerformanceClass;

namespace {

JSClassID htmlDomParserClass;
JSClassID htmlAnchorElementClass;
JSClassID htmlAreaElementClass;
JSClassID htmlBaseElementClass;
JSClassID htmlCanvasElementClass;
JSClassID htmlDialogElementClass;
JSClassID htmlDomStringMapClass;
JSClassID htmlIFrameElementClass;
JSClassID htmlImageElementClass;
JSClassID htmlLinkElementClass;
JSClassID htmlMediaElementClass;
JSClassID htmlMetaElementClass;
JSClassID htmlPictureElementClass;
JSClassID htmlSourceElementClass;
JSClassID htmlMediaErrorClass;
JSClassID htmlTimeRangesClass;
JSClassID htmlVideoElementClass;
JSClassID htmlAudioElementClass;
JSClassID htmlSlotElementClass;
JSClassID htmlScriptElementClass;
JSClassID htmlStyleElementClass;
JSClassID htmlTemplateElementClass;
JSClassID htmlErrorEventClass;
JSClassID htmlPromiseRejectionEventClass;
JSClassID htmlMessageEventClass;
JSClassID htmlPageRevealEventClass;
JSClassID htmlWindowClass;
JSClassID htmlNavigatorClass;
JSClassID htmlUserActivationClass;
JSClassID htmlIdleDeadlineClass;
JSClassID htmlScreenClass;
JSClassID htmlScreenOrientationClass;
JSClassID htmlTextTrackListClass;
JSClassID htmlAudioTrackListClass;
JSClassID htmlVideoTrackListClass;
JSClassID htmlCryptoClass;
JSClassID htmlLocationClass;
JSClassID htmlHistoryClass;
JSClassID htmlPopStateEventClass;
JSClassID htmlHashChangeEventClass;
JSClassID htmlPreferenceManagerClass;
JSClassID htmlPreferenceObjectClass;
JSClassID htmlStorageClass;
JSClassID htmlAnimationTimelineClass;
JSClassID htmlDocumentTimelineClass;
JSClassID htmlWindowPropertiesClass;

// ---------------------------------------------------------------------------------------------------------------------
// Classifiers, which map the DOM's handles to HTML's classes

JSClassID classifyElement(void* handle) {
    PrDomEventTarget* target = prDomElementAsEventTarget(static_cast<PrDomElement*>(handle));
    if (prHtmlAnchorElementQuery(target) != nullptr)
        return htmlAnchorElementClass;
    if (prHtmlAreaElementQuery(target) != nullptr)
        return htmlAreaElementClass;
    if (prHtmlBaseElementQuery(target) != nullptr)
        return htmlBaseElementClass;
    if (prHtmlLinkElementQuery(target) != nullptr)
        return htmlLinkElementClass;
    if (prHtmlMetaElementQuery(target) != nullptr)
        return htmlMetaElementClass;
    if (prHtmlPictureElementQuery(target) != nullptr)
        return htmlPictureElementClass;
    if (prHtmlSourceElementQuery(target) != nullptr)
        return htmlSourceElementClass;
    if (prHtmlIFrameElementQuery(target) != nullptr)
        return htmlIFrameElementClass;
    if (prHtmlImageElementQuery(target) != nullptr)
        return htmlImageElementClass;
    if (prHtmlCanvasElementQuery(target) != nullptr)
        return htmlCanvasElementClass;
    if (prHtmlDialogElementQuery(target) != nullptr)
        return htmlDialogElementClass;
    if (PrHtmlMediaElement* media = prHtmlMediaElementQuery(target))
        return prHtmlMediaElementIsVideo(media) ? htmlVideoElementClass : htmlAudioElementClass;
    if (prHtmlScriptElementQuery(target) != nullptr)
        return htmlScriptElementClass;
    if (prHtmlStyleElementQuery(target) != nullptr)
        return htmlStyleElementClass;
    if (prHtmlSlotElementQuery(target) != nullptr)
        return htmlSlotElementClass;
    if (prHtmlTemplateElementQuery(target) != nullptr)
        return htmlTemplateElementClass;
    if (JSClassID forms = jsHtmlFormsElementClass(static_cast<PrDomElement*>(handle)))
        return forms;
    if (prHtmlElementQuery(target) != nullptr) {
        JSClassID specific = jsHtmlElementsClass(static_cast<PrDomElement*>(handle));
        return specific != 0 ? specific : htmlElementClass;
    }
    return prJsDomElementClass;
}

JSClassID classifyEvent(void* handle) {
    switch (prDomEventKind(static_cast<PrDomEvent*>(handle))) {
        case PrDomEventKind_Error: return htmlErrorEventClass;
        case PrDomEventKind_PromiseRejection: return htmlPromiseRejectionEventClass;
        case PrDomEventKind_Message: return htmlMessageEventClass;
        case PrDomEventKind_PageReveal: return htmlPageRevealEventClass;
        case PrDomEventKind_PopState: return htmlPopStateEventClass;
        case PrDomEventKind_HashChange: return htmlHashChangeEventClass;
        default: {
            JSClassID forms = jsHtmlFormsEventClass(static_cast<PrDomEvent*>(handle));
            return forms != 0 ? forms : prJsDomEventClass;
        }
    }
}

// Returns the class of a list of tracks of `kind`.
JSClassID trackListClass(PrHtmlTrackKind kind) {
    switch (kind) {
        case PrHtmlTrackKind_Audio: return htmlAudioTrackListClass;
        case PrHtmlTrackKind_Video: return htmlVideoTrackListClass;
        default: return htmlTextTrackListClass;
    }
}

JSClassID classifyEventTarget(void* handle) {
    PrDomEventTarget* target = static_cast<PrDomEventTarget*>(handle);
    if (prHtmlWindowQuery(target) != nullptr)
        return htmlWindowClass;
    if (prHtmlPreferenceObjectQuery(target) != nullptr)
        return htmlPreferenceObjectClass;
    if (prHtmlPerformanceQuery(target) != nullptr)
        return htmlPerformanceClass;
    if (prHtmlScreenOrientationQuery(target) != nullptr)
        return htmlScreenOrientationClass;
    if (PrHtmlTrackList* list = prHtmlTrackListQuery(target))
        return trackListClass(prHtmlTrackListKind(list));
    if (prHtmlMessagePortQuery(target) != nullptr)
        return htmlMessagePortClass;
    if (prHtmlMediaSourceQuery(target) != nullptr)
        return htmlMediaSourceClass;
    if (prHtmlSourceBufferQuery(target) != nullptr)
        return htmlSourceBufferClass;
    if (prHtmlSourceBufferListQuery(target) != nullptr)
        return htmlSourceBufferListClass;
    return prJsDomEventTargetClass;
}

// Returns the global's window, or null if it has none.
PrHtmlWindow* PR_NULLABLE globalWindow(JSContext* ctx) {
    return static_cast<PrHtmlWindow*>(jsGlobalHandleIn(ctx, htmlWindowClass));
}

PrDomDocument* PR_NULLABLE globalDocument(JSContext* ctx) {
    PrHtmlWindow* window = globalWindow(ctx);
    return window != nullptr ? prHtmlWindowDocument(window) : nullptr;
}

constexpr PrJsDomGlobalDocument kGlobalDocument = {globalDocument};

// The slots of a Window's object:
// - the about-to-be-notified rejected promises (html/#about-to-be-notified-rejected-promises-list), as an array of
// [promise, reason] arrays;
// - the global object whose Window this is, if there is a script global;
// - its document's object, which holds the objects of the document's nodes, so that they live as long as the window has
// the document;
// - the objects of the event targets that the window owns, which own their listeners.
enum {
    WindowSlot_Rejected,
    WindowSlot_Global,
    WindowSlot_Document,
    WindowSlot_Performance,
    WindowSlot_ScreenOrientation,
};
typedef uint8_t WindowSlot;

// Returns the global object whose Window is `window`: its context's global, or else the window's own object, for a
// window that runs no scripts.
JSValue globalOf(JSContext* ctx, PrHtmlWindow* PR_NULLABLE window) {
    if (window == nullptr)
        return JS_NULL;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue global = jsReference(ctx, object, WindowSlot_Global);
    if (JS_IsUndefined(global))
        return object;
    JS_FreeValue(ctx, object);
    return global;
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMParser (html/#dom-parsing-and-serialization)

// A DOMParser holds nothing. parseFromString depends only on its arguments.
JSValue parserConstruct(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, htmlDomParserClass, uint8_t(0)));
}

// The DOMParserSupportedType enumeration (webidl/#js-enumeration).
bool readSupportedType(JSContext* ctx, JSValueConst value, PrHtmlDomParserSupportedType* out) {
    struct SupportedType {
        PrStringView name;
        PrHtmlDomParserSupportedType type;
    };
    constexpr SupportedType kTypes[] = {
        {"text/html", PrHtmlDomParserSupportedType_TextHtml},
        {"text/xml", PrHtmlDomParserSupportedType_TextXml},
        {"application/xml", PrHtmlDomParserSupportedType_ApplicationXml},
        {"application/xhtml+xml", PrHtmlDomParserSupportedType_ApplicationXhtmlXml},
        {"image/svg+xml", PrHtmlDomParserSupportedType_ImageSvgXml},
    };
    StringArgument name = {ctx};
    if (!name.read(value))
        return false;
    for (const SupportedType& type : kTypes) {
        if (name.value() == type.name) {
            *out = type.type;
            return true;
        }
    }
    JS_ThrowTypeError(ctx, "Invalid DOMParserSupportedType");
    return false;
}

JSValue parseFromString(JSContext* ctx, JSValueConst jsObject, int argc, JSValueConst* argv) {
    StringArgument source = {ctx};
    PrHtmlDomParserSupportedType type = {};
    if (jsThis<uint8_t>(ctx, jsObject, htmlDomParserClass) == nullptr || !jsArgCount(ctx, argc, 2) ||
            !source.read(argv[0]) || !readSupportedType(ctx, argv[1], &type))
        return JS_EXCEPTION;
    PrDomDocument* document = prHtmlDomParserParseFromString(source.value(), type);
    JSValue object = jsWrapAs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, true);
    if (JS_IsException(object))
        prDomDocumentRelease(document);
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// Element (html/#dom-parsing-and-serialization)

// innerHTML and outerHTML, selected by the magic.
JSValue markup(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, magic == 0 ? prHtmlInnerHtml(prDomElementAsNode(element)) : prHtmlOuterHtml(element));
}

// The setters, which treat null as the empty string ([LegacyNullToEmptyString]).
JSValue setMarkup(JSContext* ctx, JSValueConst elementObject, JSValueConst input, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    if (magic == 0) {
        prHtmlSetInnerHtml(prDomElementAsNode(element), value.value());
        return JS_UNDEFINED;
    }
    PrStatus status = prHtmlSetOuterHtml(element, value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "NoModificationAllowedError");
}

JSValue insertAdjacentHtml(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    PrDomInsertPosition where = PrDomInsertPosition_BeforeBegin;
    StringArgument markup = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 2) ||
            !prJsDomInsertPositionRead(ctx, argv[0], &where) || !markup.read(argv[1]))
        return JS_EXCEPTION;
    PrStatus status = prHtmlInsertAdjacentHtml(element, where, markup.value());
    if (status == PrStatus_NotAllowed)
        return jsDomExceptionThrow(ctx, "NoModificationAllowedError");
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// ShadowRoot's innerHTML (html/#dom-shadowroot-innerhtml). Its setter treats null as the empty string.
JSValue shadowMarkup(JSContext* ctx, JSValueConst shadowObject) {
    PrDomShadowRoot* shadow = jsThis<PrDomShadowRoot>(ctx, shadowObject, prJsDomShadowRootClass);
    if (shadow == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlInnerHtml(prDomShadowRootAsNode(shadow)));
}

JSValue setShadowMarkup(JSContext* ctx, JSValueConst shadowObject, JSValueConst input) {
    PrDomShadowRoot* shadow = jsThis<PrDomShadowRoot>(ctx, shadowObject, prJsDomShadowRootClass);
    StringArgument value = {ctx};
    if (shadow == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    prHtmlSetInnerHtml(prDomShadowRootAsNode(shadow), value.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLElement's dir and hidden (html/#dom-dir, html/#dom-hidden)

JSValue dir(JSContext* ctx, JSValueConst elementObject) {
    constexpr PrStringView kDirs[] = {"", "ltr", "rtl", "auto"};
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    return element != nullptr ? jsFixedString(ctx, kDirs[prHtmlElementDir(element)]) : JS_EXCEPTION;
}

JSValue setDir(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    prHtmlElementSetDir(element, value.value());
    return JS_UNDEFINED;
}

// offsetParent (cssom-view-1/#dom-htmlelement-offsetparent).
JSValue offsetParent(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element != nullptr)
        return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlElementOffsetParent(element), false);
    return JS_EXCEPTION;
}

// offsetTop, offsetLeft, offsetWidth and offsetHeight, selected by the magic.
JSValue offsetGet(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    int32_t (*const offsets[])(PrHtmlElement*) = {
        prHtmlElementOffsetTop, prHtmlElementOffsetLeft,
        prHtmlElementOffsetWidth, prHtmlElementOffsetHeight,
    };
    return JS_NewInt32(ctx, offsets[magic](element));
}

// hidden, a (boolean or unrestricted double or DOMString), which is true, false or "until-found".
JSValue hidden(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrHtmlHidden state = prHtmlElementHidden(element);
    if (state == PrHtmlHidden_UntilFound)
        return jsFixedString(ctx, "until-found");
    return JS_NewBool(ctx, state == PrHtmlHidden_Hidden);
}

JSValue setHidden(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrHtmlHidden state = JS_ToBool(ctx, input) ? PrHtmlHidden_Hidden : PrHtmlHidden_NotHidden;
    if (JS_IsString(input)) {
        StringArgument value = {ctx};
        if (!value.read(input))
            return JS_EXCEPTION;
        if (prAsciiEqualsFold(value.value(), "until-found"))
            state = PrHtmlHidden_UntilFound;
    }
    prHtmlElementSetHidden(element, state);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// Focus (html/#focus)

// focus, blur and tabIndex, of an HTML, SVG or MathML element (html/#htmlorsvgelement).
JSValue elementFocus(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    bool present = false;
    PrHtmlFocusOptions options = prHtmlFocusOptionsDefault();
    JSValueConst dictionary = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (element == nullptr || !jsDictionary(ctx, dictionary, &present) || (present &&
            (!jsDictBool(ctx, dictionary, "focusVisible", &options.focusVisible, &options.hasFocusVisible) ||
                !jsDictBool(ctx, dictionary, "preventScroll", &options.preventScroll))))
        return JS_EXCEPTION;
    prHtmlFocus(element, &options);
    return JS_UNDEFINED;
}

JSValue elementBlur(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    prHtmlBlur(element);
    return JS_UNDEFINED;
}

JSValue setTabIndex(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    int32_t value = 0;
    if (element == nullptr || JS_ToInt32(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    prHtmlSetTabIndex(element, value);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMStringMap (html/#domstringmap), whose handle is its element's handle

bool datasetNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    return jsNameAtoms(ctx, static_cast<const PrDomElement*>(handle), prHtmlDatasetNames, out);
}

JSValue datasetItem(JSContext* ctx, JSValueConst, void* handle, PrStringView name) {
    PrStringView value = prHtmlDatasetGet(static_cast<const PrDomElement*>(handle), name);
    return value.data != nullptr ? jsNewStringView(ctx, value) : JS_UNDEFINED;
}

bool datasetSetItem(JSContext* ctx, JSValueConst, void* handle, PrStringView name, JSValueConst input) {
    StringArgument value = {ctx};
    if (!value.read(input))
        return false;
    switch (prHtmlDatasetSet(static_cast<PrDomElement*>(handle), name, value.value())) {
        case PrStatus_Ok: return true;
        case PrStatus_Syntax: jsDomExceptionThrow(ctx, "SyntaxError"); return false;
        default: jsDomExceptionThrow(ctx, "InvalidCharacterError"); return false;
    }
}

void datasetDeleteItem(void* handle, PrStringView name) {
    prHtmlDatasetDelete(static_cast<PrDomElement*>(handle), name);
}

// [LegacyOverrideBuiltIns], with a named setter and deleter.
constexpr JsNamed kDataset = {
    datasetNames, datasetItem, datasetSetItem, datasetDeleteItem,
    JsNamedFlag_OverrideBuiltIns,
};

// html/#dom-dataset, of an HTML, SVG or MathML element.
JSValue dataset(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, "dataset",
        [&] { return jsWrap(ctx, htmlDomStringMapClass, elementObject, element, false); });
}

// activeElement, of a Document or a ShadowRoot.
JSValue activeElement(JSContext* ctx, JSValueConst rootObject) {
    PrDomNode* root = jsThis<PrDomNode>(ctx, rootObject, prJsDomNodeClass);
    if (root == nullptr)
        return JS_EXCEPTION;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlActiveElement(root), false);
}

// The getter of a document's collection of the PrHtmlDocumentCollection `magic`, which returns the same object at each
// call. plugins returns the object that embeds does (html/#dom-document-plugins).
JSValue documentCollection(JSContext* ctx, JSValueConst documentObject, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    constexpr const char* kNames[] = {"images", "embeds", "links", "forms", "scripts", "anchors"};
    PrHtmlDocumentCollection kind = PrHtmlDocumentCollection(magic);
    return jsSameObject(ctx, documentObject, kNames[kind], [&] {
        PrDomHtmlCollection* collection = prHtmlDocumentCollection(document, kind);
        JSValue object = jsWrapAs(ctx, prJsDomHtmlCollectionClass, documentObject, collection, true);
        if (JS_IsException(object))
            prDomHtmlCollectionDestroy(collection);
        return object;
    });
}

JSValue hasFocus(JSContext* ctx, JSValueConst documentObject, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    return document != nullptr ? JS_NewBool(ctx, prHtmlDocumentHasFocus(document)) : JS_EXCEPTION;
}

// visibilityState (html/#dom-document-visibilitystate) and hidden (html/#dom-document-hidden), selected by the magic.
JSValue documentVisibility(JSContext* ctx, JSValueConst documentObject, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    bool visible = prHtmlDocumentVisibilityState(document) == PrHtmlVisibility_Visible;
    if (magic == 1)
        return JS_NewBool(ctx, !visible);
    return jsFixedString(ctx, visible ? "visible" : "hidden");
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLDialogElement (html/#htmldialogelement)

JSValue dialogOpen(JSContext* ctx, JSValueConst dialogObject) {
    PrHtmlDialogElement* dialog = jsThis<PrHtmlDialogElement>(ctx, dialogObject, htmlDialogElementClass);
    return dialog != nullptr ? JS_NewBool(ctx, prHtmlDialogElementOpen(dialog)) : JS_EXCEPTION;
}

JSValue setDialogOpen(JSContext* ctx, JSValueConst dialogObject, JSValueConst input) {
    PrHtmlDialogElement* dialog = jsThis<PrHtmlDialogElement>(ctx, dialogObject, htmlDialogElementClass);
    if (dialog == nullptr)
        return JS_EXCEPTION;
    prHtmlDialogElementSetOpen(dialog, JS_ToBool(ctx, input));
    return JS_UNDEFINED;
}

// show and showModal, selected by the magic.
JSValue dialogShow(JSContext* ctx, JSValueConst dialogObject, int, JSValueConst*, int magic) {
    PrHtmlDialogElement* dialog = jsThis<PrHtmlDialogElement>(ctx, dialogObject, htmlDialogElementClass);
    if (dialog == nullptr)
        return JS_EXCEPTION;
    PrStatus status = magic == 0 ? prHtmlDialogElementShow(dialog) : prHtmlDialogElementShowModal(dialog);
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "InvalidStateError");
}

// close and requestClose, selected by the magic. If the optional result is omitted, the return value is unchanged.
JSValue dialogClose(JSContext* ctx, JSValueConst dialogObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlDialogElement* dialog = jsThis<PrHtmlDialogElement>(ctx, dialogObject, htmlDialogElementClass);
    StringArgument result = {ctx, PrStringView{}};
    if (dialog == nullptr || (jsArgGiven(argc, argv, 0) && !result.read(argv[0])))
        return JS_EXCEPTION;
    if (magic == 0)
        prHtmlDialogElementClose(dialog, result.value());
    else
        prHtmlDialogElementRequestClose(dialog, result.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLIFrameElement (html/#htmliframeelement)

JSValue contentWindow(JSContext* ctx, JSValueConst frameObject) {
    PrHtmlIFrameElement* frame = jsThis<PrHtmlIFrameElement>(ctx, frameObject, htmlIFrameElementClass);
    return frame != nullptr ? globalOf(ctx, prHtmlIFrameElementContentWindow(frame)) : JS_EXCEPTION;
}

JSValue contentDocument(JSContext* ctx, JSValueConst frameObject) {
    PrHtmlIFrameElement* frame = jsThis<PrHtmlIFrameElement>(ctx, frameObject, htmlIFrameElementClass);
    if (frame == nullptr)
        return JS_EXCEPTION;
    PrDomDocument* document = prHtmlIFrameElementContentDocument(frame);
    return jsWrapAs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLImageElement (html/#htmlimageelement)

// html/#dom-img-decode: the image is fetched whenever its source changes, so the promise settles immediately.
JSValue imageDecode(JSContext* ctx, JSValueConst imageObject, int, JSValueConst*) {
    PrHtmlImageElement* image = jsThis<PrHtmlImageElement>(ctx, imageObject, htmlImageElementClass);
    if (image == nullptr)
        return JS_EXCEPTION;
    bool available = prHtmlImageElementState(image) == PrDomImageState_CompletelyAvailable;
    JSValue value = available ? JS_UNDEFINED : jsDomExceptionNew(ctx, "EncodingError");
    JSValue promise = jsSettledPromise(ctx, value, !available);
    JS_FreeValue(ctx, value);
    return promise;
}

// html/#dom-image, the legacy factory function Image(width, height): creates an img in the global's document, with the
// given dimensions.
JSValue imageFactory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    PrDomElement* element = nullptr;
    if (document == nullptr || prDomElementCreate(document, "img", nullptr, &element) != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    // The object replaces the reference that the element was created with.
    JSValue object = jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, element, false);
    prDomNodeRelease(prDomElementAsNode(element));
    PrHtmlImageElement* image = prHtmlImageElementQuery(prDomElementAsEventTarget(element));
    void (*const kSetters[])(PrHtmlImageElement*, uint32_t) = {prHtmlImageElementSetWidth, prHtmlImageElementSetHeight};
    for (int i = 0; i < 2 && i < argc; ++i) {
        uint32_t value = 0;
        if (JS_IsUndefined(argv[i]))
            continue;
        if (JS_ToUint32(ctx, &value, argv[i]) < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        kSetters[i](image, value);
    }
    return object;
}

// html/#dom-audio, the legacy factory function Audio(src).
JSValue audioFactory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    StringArgument source = {ctx};
    bool given = jsArgGiven(argc, argv, 0);
    if (given && !source.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlMediaElement* audio = prHtmlMediaElementCreateAudio(document, given ? source.value() : PrStringView{});
    // The object replaces the reference that the element was created with.
    PrDomElement* element = prHtmlMediaElementAsElement(audio);
    JSValue object = jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, element, false);
    prDomNodeRelease(prDomElementAsNode(element));
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLCanvasElement (html/#htmlcanvaselement)

// Implements the setters of width and height, selected by the magic, which throw for a placeholder canvas.
JSValue setCanvasDimension(JSContext* ctx, JSValueConst canvasObject, JSValueConst input, int magic) {
    PrHtmlCanvasElement* canvas = jsThis<PrHtmlCanvasElement>(ctx, canvasObject, htmlCanvasElementClass);
    uint32_t value = 0;
    if (canvas == nullptr || JS_ToUint32(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    PrStatus status = magic == 0 ? prHtmlCanvasElementSetWidth(canvas, value) : prHtmlCanvasElementSetHeight(canvas, value);
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "InvalidStateError");
}

JSValue canvasDimension(JSContext* ctx, JSValueConst canvasObject, int magic) {
    PrHtmlCanvasElement* canvas = jsThis<PrHtmlCanvasElement>(ctx, canvasObject, htmlCanvasElementClass);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, magic == 0 ? prHtmlCanvasElementWidth(canvas) : prHtmlCanvasElementHeight(canvas));
}

// html/#dom-canvas-getcontext. The engine makes 2D contexts, and every other context ID gives null. The context's
// object is the same each time, and the canvas's object holds it.
JSValue canvasGetContext(JSContext* ctx, JSValueConst canvasObject, int argc, JSValueConst* argv) {
    PrHtmlCanvasElement* canvas = jsThis<PrHtmlCanvasElement>(ctx, canvasObject, htmlCanvasElementClass);
    StringArgument id = {ctx};
    if (canvas == nullptr || !jsArgCount(ctx, argc, 1) || !id.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlCanvasContextMode mode = prHtmlCanvasElementContextMode(canvas);
    if (mode == PrHtmlCanvasContextMode_Placeholder)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    if (id.value() != "2d")
        return JS_NULL;
    PrCanvasSettings settings = prCanvasSettingsDefault();
    if (mode == PrHtmlCanvasContextMode_None &&
            !prJsCanvasSettingsRead(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &settings))
        return JS_EXCEPTION;
    PrCanvas* context = nullptr;
    if (prHtmlCanvasElementGetContext2D(canvas, &settings, &context) != PrStatus_Ok || context == nullptr)
        return JS_NULL;
    return jsSameObject(ctx, canvasObject, "context2d",
        [&] { return prJsCanvasRenderingContext2DNew(ctx, context, canvasObject); });
}

// html/#dom-canvas-todataurl. The quality is not read: the engine encodes no type that has one.
JSValue canvasToDataUrl(JSContext* ctx, JSValueConst canvasObject, int argc, JSValueConst* argv) {
    PrHtmlCanvasElement* canvas = jsThis<PrHtmlCanvasElement>(ctx, canvasObject, htmlCanvasElementClass);
    StringArgument type = {.ctx = ctx, .fallback = "image/png"};
    if (canvas == nullptr || (jsArgGiven(argc, argv, 0) && !type.read(argv[0])))
        return JS_EXCEPTION;
    PrString url = prStringNull();
    if (prHtmlCanvasElementToDataUrl(canvas, type.value(), &url) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "SecurityError");
    return jsTakeString(ctx, url);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLMediaElement and HTMLVideoElement (html/#htmlmediaelement, html/#htmlvideoelement), with MediaError
// (html/#mediaerror) and TimeRanges (html/#timeranges)

// A MediaError's code and message.
struct MediaErrorValue {
    uint16_t code;
    PrString message;
};

void mediaErrorDestroy(MediaErrorValue* PR_NULLABLE error) {
    if (error == nullptr)
        return;
    prStringReset(&error->message);
    prMemoryFree(error);
}

// The ranges of a TimeRanges: the start and end of each, in order.
struct TimeRangesValue {
    Array<double> bounds;
};

void timeRangesDestroy(TimeRangesValue* PR_NULLABLE ranges) {
    if (ranges == nullptr)
        return;
    ranges->bounds.reset();
    prMemoryFree(ranges);
}

PrHtmlMediaElement* PR_NULLABLE mediaThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlMediaElement>(ctx, object, htmlMediaElementClass);
}

// The element's error, as the same object while the error is unchanged.
JSValue mediaError(JSContext* ctx, JSValueConst mediaObject) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    PrHtmlMediaError code = prHtmlMediaElementError(media);
    if (code == PrHtmlMediaError_None) {
        jsSetSameObject(ctx, mediaObject, "[[MediaError]]", JS_UNDEFINED);
        return JS_NULL;
    }
    JSValue held = jsSameObject(ctx, mediaObject, "[[MediaError]]");
    MediaErrorValue* value = static_cast<MediaErrorValue*>(jsHandleIn(ctx, held, htmlMediaErrorClass));
    if (value != nullptr && value->code == code)
        return held;
    JS_FreeValue(ctx, held);
    MediaErrorValue* error = alloc<MediaErrorValue>();
    *error = MediaErrorValue{code, prStringCopy(prHtmlMediaElementErrorMessage(media))};
    JSValue object = jsWrap(ctx, htmlMediaErrorClass, JS_NULL, error, true);
    if (JS_IsException(object)) {
        mediaErrorDestroy(error);
        return object;
    }
    jsSetSameObject(ctx, mediaObject, "[[MediaError]]", object);
    return object;
}

JSValue mediaErrorCode(JSContext* ctx, JSValueConst errorObject) {
    MediaErrorValue* error = jsThis<MediaErrorValue>(ctx, errorObject, htmlMediaErrorClass);
    return error != nullptr ? JS_NewUint32(ctx, error->code) : JS_EXCEPTION;
}

JSValue mediaErrorMessage(JSContext* ctx, JSValueConst errorObject) {
    MediaErrorValue* error = jsThis<MediaErrorValue>(ctx, errorObject, htmlMediaErrorClass);
    return error != nullptr ? jsNewString(ctx, &error->message) : JS_EXCEPTION;
}

// buffered, played and seekable, selected by the magic, which is the PrHtmlMediaRanges. Each read returns a new
// TimeRanges (html/#dom-media-buffered).
JSValue mediaRanges(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    uint32_t count = prHtmlMediaElementRanges(media, PrHtmlMediaRanges(magic), nullptr, nullptr, 0);
    TimeRangesValue* ranges = allocZeroed<TimeRangesValue>();
    Array<double> starts;
    Array<double> ends;
    starts.reserve(count);
    ends.reserve(count);
    prHtmlMediaElementRanges(media, PrHtmlMediaRanges(magic), starts.data, ends.data, count);
    for (uint32_t i = 0; i < count; ++i) {
        ranges->bounds.push(starts.data[i]);
        ranges->bounds.push(ends.data[i]);
    }
    starts.reset();
    ends.reset();
    JSValue object = jsWrap(ctx, htmlTimeRangesClass, JS_NULL, ranges, true);
    if (JS_IsException(object))
        timeRangesDestroy(ranges);
    return object;
}

JSValue timeRangesLength(JSContext* ctx, JSValueConst rangesObject) {
    TimeRangesValue* ranges = jsThis<TimeRangesValue>(ctx, rangesObject, htmlTimeRangesClass);
    return ranges != nullptr ? JS_NewUint32(ctx, ranges->bounds.count / 2) : JS_EXCEPTION;
}

// start and end, selected by the magic. Throws IndexSizeError for an index past the last range.
JSValue timeRangesBound(JSContext* ctx, JSValueConst rangesObject, int argc, JSValueConst* argv, int magic) {
    TimeRangesValue* ranges = jsThis<TimeRangesValue>(ctx, rangesObject, htmlTimeRangesClass);
    uint32_t index = 0;
    if (ranges == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    if (index >= ranges->bounds.count / 2)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return JS_NewFloat64(ctx, ranges->bounds.data[index * 2 + uint32_t(magic)]);
}

// The unsigned short attributes of a media element, selected by the magic: networkState and readyState.
JSValue mediaState(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, magic == 0 ? prHtmlMediaElementNetworkState(media) : prHtmlMediaElementReadyState(media));
}

// The boolean attributes, selected by the magic: seeking, paused, ended, muted and preservesPitch.
JSValue mediaFlag(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return JS_NewBool(ctx, prHtmlMediaElementSeeking(media));
        case 1: return JS_NewBool(ctx, prHtmlMediaElementPaused(media));
        case 2: return JS_NewBool(ctx, prHtmlMediaElementEnded(media));
        case 3: return JS_NewBool(ctx, prHtmlMediaElementMuted(media));
        default: return JS_NewBool(ctx, prHtmlMediaElementPreservesPitch(media));
    }
}

JSValue setMediaFlag(JSContext* ctx, JSValueConst mediaObject, JSValueConst input, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    bool value = JS_ToBool(ctx, input) != 0;
    if (magic == 3)
        prHtmlMediaElementSetMuted(media, value);
    else
        prHtmlMediaElementSetPreservesPitch(media, value);
    return JS_UNDEFINED;
}

// The double attributes, selected by the magic: currentTime, duration, defaultPlaybackRate, playbackRate and volume.
JSValue mediaNumber(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 0: return JS_NewFloat64(ctx, prHtmlMediaElementCurrentTime(media));
        case 1: return JS_NewFloat64(ctx, prHtmlMediaElementDuration(media));
        case 2: return JS_NewFloat64(ctx, prHtmlMediaElementDefaultPlaybackRate(media));
        case 3: return JS_NewFloat64(ctx, prHtmlMediaElementPlaybackRate(media));
        default: return JS_NewFloat64(ctx, prHtmlMediaElementVolume(media));
    }
}

JSValue setMediaNumber(JSContext* ctx, JSValueConst mediaObject, JSValueConst input, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    double value = 0;
    if (media == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(value))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    switch (magic) {
        case 0: prHtmlMediaElementSetCurrentTime(media, value); break;
        case 2: prHtmlMediaElementSetDefaultPlaybackRate(media, value); break;
        case 3:
            if (prHtmlMediaElementSetPlaybackRate(media, value) != PrStatus_Ok)
                return jsDomExceptionThrow(ctx, "NotSupportedError");
            break;
        default:
            if (prHtmlMediaElementSetVolume(media, value) != PrStatus_Ok)
                return jsDomExceptionThrow(ctx, "IndexSizeError");
            break;
    }
    return JS_UNDEFINED;
}

JSValue mediaCurrentSrc(JSContext* ctx, JSValueConst mediaObject) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    return jsNewStringView(ctx, prHtmlMediaElementCurrentSrc(media));
}

JSValue mediaLoad(JSContext* ctx, JSValueConst mediaObject, int, JSValueConst*) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    prHtmlMediaElementLoad(media);
    return JS_UNDEFINED;
}

JSValue mediaCanPlayType(JSContext* ctx, JSValueConst mediaObject, int argc, JSValueConst* argv) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    StringArgument type = {ctx};
    if (media == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    constexpr PrStringView kResults[] = {"", "maybe", "probably"};
    return jsFixedString(ctx, kResults[prHtmlMediaElementCanPlayType(media, type.value())]);
}

JSValue mediaFastSeek(JSContext* ctx, JSValueConst mediaObject, int argc, JSValueConst* argv) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    double time = 0;
    if (media == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToFloat64(ctx, &time, argv[0]) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(time))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    prHtmlMediaElementFastSeek(media, time);
    return JS_UNDEFINED;
}

// html/#dom-media-getstartdate: the timeline has no offset from a date, so this returns NaN.
JSValue mediaStartDate(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (mediaThis(ctx, object) == nullptr)
        return JS_EXCEPTION;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue date = JS_GetPropertyStr(ctx, global, "Date");
    JSValue nan = JS_NewFloat64(ctx, kPrF64Nan);
    JSValue result = JS_CallConstructor(ctx, date, 1, &nan);
    JS_FreeValue(ctx, date);
    JS_FreeValue(ctx, global);
    return result;
}

// The functions of a pending play promise, which the element settles.
struct PlayPromise {
    JSContext* ctx;
    JsCallback* resolve;
    JsCallback* reject;
};

void settlePlay(void* PR_NULLABLE context, PrStringView rejection) {
    PlayPromise* promise = static_cast<PlayPromise*>(context);
    JSContext* ctx = promise->ctx;
    JSValue argument = rejection.data != nullptr ? jsDomExceptionNew(ctx, rejection) : JS_UNDEFINED;
    JsCallback* callback = rejection.data != nullptr ? promise->reject : promise->resolve;
    JSValue result = JS_Call(ctx, callback->value, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, argument);
}

void releasePlay(void* PR_NULLABLE context) {
    PlayPromise* promise = static_cast<PlayPromise*>(context);
    jsCallbackRelease(promise->resolve);
    jsCallbackRelease(promise->reject);
    prMemoryFree(promise);
}

// html/#dom-media-play
JSValue mediaPlay(JSContext* ctx, JSValueConst mediaObject, int, JSValueConst*) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise))
        return promise;
    PlayPromise* play = alloc<PlayPromise>();
    *play = PlayPromise{
        ctx, jsCallbackCreate(ctx, functions[0], JS_UNDEFINED),
        jsCallbackCreate(ctx, functions[1], JS_UNDEFINED),
    };
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    PrHtmlPlayPromise callback = {play, settlePlay, releasePlay};
    prHtmlMediaElementPlay(media, &callback);
    return promise;
}

JSValue mediaPause(JSContext* ctx, JSValueConst mediaObject, int, JSValueConst*) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    prHtmlMediaElementPause(media);
    return JS_UNDEFINED;
}

// The content attributes of the enumerated attributes of a media element, indexed by the magic. crossOrigin also serves
// the image, link and script elements.
constexpr PrStringView kMediaEnumerated[] = {"crossorigin", "preload", "loading"};

// An enumerated attribute of a media element (html/#enumerated-attribute), selected by the magic: crossOrigin, whose
// missing value is null (html/#cors-settings-attribute), preload, and loading (html/#lazy-loading-attributes).
JSValue mediaEnumerated(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrStringView text = prDomElementGetAttribute(element, kMediaEnumerated[magic]);
    const char* result = nullptr;
    if (magic == 0) {
        result = text.data == nullptr ? nullptr : prAsciiEqualsFold(text, "use-credentials") ? "use-credentials" : "anonymous";
    } else if (magic == 1) {
        result = prAsciiEqualsFold(text, "none") ? "none" : prAsciiEqualsFold(text, "metadata") ? "metadata" : "auto";
    } else {
        result = prAsciiEqualsFold(text, "lazy") ? "lazy" : "eager";
    }
    return result != nullptr ? JS_NewString(ctx, result) : JS_NULL;
}

JSValue setMediaEnumerated(JSContext* ctx, JSValueConst elementObject, JSValueConst input, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    if (magic == 0 && JS_IsNull(input)) {
        prDomElementRemoveAttribute(element, kMediaEnumerated[magic]);
        return JS_UNDEFINED;
    }
    StringArgument value = {ctx};
    if (!value.read(input))
        return JS_EXCEPTION;
    PrStatus status = prDomElementSetAttribute(element, kMediaEnumerated[magic], value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// videoWidth and videoHeight, selected by the magic.
JSValue videoSize(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, magic == 0 ? prHtmlMediaElementVideoWidth(media) : prHtmlMediaElementVideoHeight(media));
}

// [SameObject] textTracks, audioTracks and videoTracks, selected by the magic, a PrHtmlTrackKind. The element's object
// holds the list's object, which keeps the element's object alive.
JSValue mediaTracks(JSContext* ctx, JSValueConst mediaObject, int magic) {
    PrHtmlMediaElement* media = mediaThis(ctx, mediaObject);
    if (media == nullptr)
        return JS_EXCEPTION;
    constexpr const char* kNames[] = {"textTracks", "audioTracks", "videoTracks"};
    PrHtmlTrackKind kind = PrHtmlTrackKind(magic);
    return jsSameObject(ctx, mediaObject, kNames[kind], [&] {
        PrHtmlTrackList* list = prHtmlMediaElementTracks(media, kind);
        return jsWrap(ctx, trackListClass(kind), mediaObject, prHtmlTrackListAsEventTarget(list), false);
    });
}

// Returns `targetObject` as a track list of any kind, or null with an exception pending.
PrHtmlTrackList* PR_NULLABLE trackListThis(JSContext* ctx, JSValueConst targetObject) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, targetObject, prJsDomEventTargetClass);
    PrHtmlTrackList* list = prHtmlTrackListQuery(target);
    if (target != nullptr && list == nullptr)
        JS_ThrowTypeError(ctx, "Illegal invocation");
    return list;
}

JSValue trackListLength(JSContext* ctx, JSValueConst listObject) {
    PrHtmlTrackList* list = trackListThis(ctx, listObject);
    return list != nullptr ? JS_NewUint32(ctx, prHtmlTrackListLength(list)) : JS_EXCEPTION;
}

// getTrackById (html/#dom-texttracklist-gettrackbyid and the others): the list's track whose id is the argument, or
// null.
JSValue trackListGetTrackById(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    StringArgument id = {ctx};
    if (trackListThis(ctx, object) == nullptr || !jsArgCount(ctx, argc, 1) || !id.read(argv[0]))
        return JS_EXCEPTION;
    return JS_NULL;
}

JSValue trackListSelectedIndex(JSContext* ctx, JSValueConst listObject) {
    PrHtmlTrackList* list = trackListThis(ctx, listObject);
    return list != nullptr ? JS_NewInt32(ctx, prHtmlTrackListSelectedIndex(list)) : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLSlotElement (html/#htmlslotelement)

// assignedNodes(options) and assignedElements(options). The magic says whether only elements are returned.
JSValue slotAssigned(JSContext* ctx, JSValueConst slotObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlSlotElement* slot = jsThis<PrHtmlSlotElement>(ctx, slotObject, htmlSlotElementClass);
    bool present = false;
    bool flatten = false;
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (slot == nullptr || !jsDictionary(ctx, options, &present) ||
            (present && !jsDictBool(ctx, options, "flatten", &flatten)))
        return JS_EXCEPTION;
    Array<PrDomNode* PR_NONNULL> nodes;
    nodes.reserve(prHtmlSlotElementAssignedNodes(slot, flatten, nullptr, 0));
    nodes.count = prHtmlSlotElementAssignedNodes(slot, flatten, nodes.data, nodes.capacity);
    JSValue array = JS_NewArray(ctx);
    uint32_t length = 0;
    for (uint32_t i = 0; i < nodes.count; ++i) {
        if (magic != 0 && prDomNodeType(nodes.data[i]) != PrDomNodeType_Element)
            continue;
        JS_SetPropertyUint32(ctx, array, length++, jsWrapAs(ctx, prJsDomNodeClass, JS_UNDEFINED, nodes.data[i], false));
    }
    nodes.reset();
    return array;
}

// assign(...nodes), of elements and text nodes.
JSValue slotAssign(JSContext* ctx, JSValueConst slotObject, int argc, JSValueConst* argv) {
    PrHtmlSlotElement* slot = jsThis<PrHtmlSlotElement>(ctx, slotObject, htmlSlotElementClass);
    if (slot == nullptr)
        return JS_EXCEPTION;
    Array<PrDomNode* PR_NONNULL> nodes;
    for (int i = 0; i < argc; ++i) {
        PrDomNode* node = jsHandleAs<PrDomNode>(ctx, argv[i], prJsDomNodeClass);
        PrDomNodeType type = node != nullptr ? prDomNodeType(node) : PrDomNodeType_Element;
        if (node == nullptr || (type != PrDomNodeType_Element && type != PrDomNodeType_Text)) {
            nodes.reset();
            return JS_ThrowTypeError(ctx, "assign takes elements and text nodes");
        }
        nodes.push(node);
    }
    prHtmlSlotElementAssign(slot, nodes.data, nodes.count);
    nodes.reset();
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTemplateElement's shadow root attributes (html/#dom-template-shadowrootmode)

JSValue shadowRootMode(JSContext* ctx, JSValueConst elementObject, int) {
    PrHtmlTemplateElement* element = jsThis<PrHtmlTemplateElement>(ctx, elementObject, htmlTemplateElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomShadowRootMode mode = PrDomShadowRootMode_Open;
    if (!prHtmlTemplateElementShadowRootMode(element, &mode))
        return jsFixedString(ctx, "");
    return jsFixedString(ctx, mode == PrDomShadowRootMode_Open ? "open" : "closed");
}

JSValue shadowRootSlotAssignment(JSContext* ctx, JSValueConst elementObject, int) {
    PrHtmlTemplateElement* element = jsThis<PrHtmlTemplateElement>(ctx, elementObject, htmlTemplateElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    bool manual = prHtmlTemplateElementShadowRootSlotAssignment(element) == PrDomSlotAssignmentMode_Manual;
    return jsFixedString(ctx, manual ? "manual" : "named");
}

// The setters, which set the attribute to the given string.
JSValue setAttributeString(JSContext* ctx, JSValueConst elementObject, JSValueConst input, int magic) {
    constexpr PrStringView kNames[] = {"shadowrootmode", "shadowrootslotassignment"};
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    PrStatus status = prDomElementSetAttribute(element, kNames[magic], value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLHyperlinkElementUtils (html/#api-for-a-and-area-elements), each attribute named by its PrUrlComponent

JSValue hyperlinkComponent(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlHyperlinkComponent(element, PrUrlComponent(magic)));
}

JSValue setHyperlinkComponent(JSContext* ctx, JSValueConst elementObject, JSValueConst input, int magic) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    PrStatus status = prHtmlHyperlinkSetComponent(element, PrUrlComponent(magic), value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// The stringifier, which is the href attribute.
JSValue hyperlinkToString(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return hyperlinkComponent(ctx, object, PrUrlComponent_Href);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLBaseElement (html/#htmlbaseelement)

// The href attribute's getter. Its setter is the same as a DOMString attribute's setter.
JSValue baseHref(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlBaseElement* element = jsThis<PrHtmlBaseElement>(ctx, elementObject, htmlBaseElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlBaseElementHref(element));
}

// html/#dom-outertext, whose setter treats null as the empty string.
JSValue setOuterText(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    StringArgument value = {ctx, ""};
    if (element == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    PrStatus status = prHtmlElementSetOuterText(element, value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "NoModificationAllowedError");
}

// ---------------------------------------------------------------------------------------------------------------------
// Document's fgColor, bgColor, linkColor, vlinkColor and alinkColor

// The body attribute that each reflects, as the magic of its accessors.
constexpr PrStringView kBodyColorAttributes[] = {"text", "bgcolor", "link", "vlink", "alink"};

JSValue bodyColor(JSContext* ctx, JSValueConst documentObject, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return jsNewStringView(ctx, prHtmlDocumentBodyAttribute(document, kBodyColorAttributes[magic]));
}

// [LegacyNullToEmptyString]
JSValue setBodyColor(JSContext* ctx, JSValueConst documentObject, JSValueConst input, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    StringArgument value = {ctx, ""};
    if (document == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    prHtmlDocumentSetBodyAttribute(document, kBodyColorAttributes[magic], value.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLElement's click, contenteditable and draggable, and relList

JSValue elementClick(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    prHtmlElementClick(element);
    return JS_UNDEFINED;
}

JSValue contentEditable(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prHtmlContentEditableKeyword(prHtmlElementContentEditable(element)));
}

JSValue setContentEditable(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    PrHtmlContentEditable state = PrHtmlContentEditable_Inherit;
    if (!prHtmlContentEditableFromKeyword(value.value(), &state))
        return jsDomExceptionThrow(ctx, "SyntaxError");
    prHtmlElementSetContentEditable(element, state);
    return JS_UNDEFINED;
}

JSValue isContentEditable(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    return element != nullptr ? JS_NewBool(ctx, prHtmlElementIsContentEditable(element)) : JS_EXCEPTION;
}

JSValue draggable(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    return element != nullptr ? JS_NewBool(ctx, prHtmlElementDraggable(element)) : JS_EXCEPTION;
}

JSValue setDraggable(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    prHtmlElementSetDraggable(element, JS_ToBool(ctx, input) > 0);
    return JS_UNDEFINED;
}

// relList, the same DOMTokenList of the rel attribute at each read.
JSValue relList(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlElement* element = jsThis<PrHtmlElement>(ctx, elementObject, htmlElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, "relList", [&] {
        PrDomTokenList* list = prHtmlElementRelList(element);
        JSValue object = jsWrapAs(ctx, prJsDomTokenListClass, elementObject, list, true);
        if (JS_IsException(object))
            prDomTokenListDestroy(list);
        return object;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLLinkElement (html/#htmllinkelement)

JSValue linkSheet(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlLinkElement* element = jsThis<PrHtmlLinkElement>(ctx, elementObject, htmlLinkElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsWrap(ctx, prJsCssStyleSheetClass, elementObject, prHtmlLinkElementSheet(element), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLStyleElement (html/#htmlstyleelement)

JSValue sheet(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlStyleElement* element = jsThis<PrHtmlStyleElement>(ctx, elementObject, htmlStyleElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsWrap(ctx, prJsCssStyleSheetClass, elementObject, prHtmlStyleElementSheet(element), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTemplateElement (html/#htmltemplateelement)

JSValue content(JSContext* ctx, JSValueConst elementObject) {
    PrHtmlTemplateElement* element = jsThis<PrHtmlTemplateElement>(ctx, elementObject, htmlTemplateElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomDocumentFragment* fragment = prHtmlTemplateElementContent(element);
    return jsWrapAs(ctx, prJsDomNodeClass, elementObject, prDomDocumentFragmentAsNode(fragment), false);
}

// ---------------------------------------------------------------------------------------------------------------------
// ErrorEvent (html/#errorevent)

// The reference slot in which an ErrorEvent's object holds the error value.
constexpr uint32_t kErrorSlot = 0;

JSValue errorEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument filename = {ctx};
    StringArgument message = {ctx};
    PrHtmlErrorEventInit init = prHtmlErrorEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue error = JS_UNDEFINED;
    bool present = false;
    // html/#erroreventinit, after EventInit. The object holds its `error`.
    bool read = jsArgCount(ctx, argc, 1) && type.read(argv[0]) && jsDictionary(ctx, dictionary, &present);
    if (read && present) {
        read =
            prJsDomEventInitRead(ctx, dictionary, &init.event) && jsDictUint32(ctx, dictionary, "colno", &init.colno);
        if (read) {
            error = JS_GetPropertyStr(ctx, dictionary, "error");
            read = !JS_IsException(error);
        }
        read = read && jsDictString(ctx, dictionary, "filename", &filename) &&
            jsDictUint32(ctx, dictionary, "lineno", &init.lineno) && jsDictString(ctx, dictionary, "message", &message);
    }
    if (!read) {
        JS_FreeValue(ctx, error);
        return JS_EXCEPTION;
    }
    init.filename = filename.value();
    init.message = message.value();
    PrHtmlErrorEvent* event = prHtmlErrorEventCreate(type.value(), &init);
    JSValue object = jsWrap(ctx, htmlErrorEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object))
        prDomEventRelease(prHtmlErrorEventAsEvent(event));
    else
        jsSetReference(ctx, object, kErrorSlot, error);
    JS_FreeValue(ctx, error);
    return jsConstructed(ctx, newTarget, object);
}

JSValue errorValue(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrHtmlErrorEvent>(ctx, object, htmlErrorEventClass) == nullptr)
        return JS_EXCEPTION;
    return jsReference(ctx, object, kErrorSlot);
}

// html/#report-an-exception at the global's window, as an ErrorEvent that holds the exception. If no listener cancels
// the event, the exception is written to stderr.
void reportException(JSContext* ctx, JSValueConst error) {
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr) {
        jsPrintException(ctx, error);
        return;
    }
    StringArgument message = {ctx};
    PrHtmlErrorEventInit init = prHtmlErrorEventInitDefault();
    init.event.cancelable = true;
    // QuickJS records an error's location only in its stack. The stack's first frame ends with the script's URL, line
    // and column, as "at f (url:line:column)" or "at url:line:column".
    JSValue stack = JS_IsObject(error) ? JS_GetPropertyStr(ctx, error, "stack") : JS_UNDEFINED;
    StringArgument frames = {ctx};
    if (message.read(error) && JS_IsString(stack) && frames.read(stack)) {
        PrStringView text = frames.value();
        size_t end = prStringFindByte(text, '\n');
        PrStringView frame = prStringTrim(PrStringView{text.data, end < text.size ? end : text.size});
        if (frame.size != 0 && frame.data[frame.size - 1] == ')') {
            size_t open = prStringFindLastByte(frame, '(');
            frame = open < frame.size ? PrStringView{frame.data + open + 1, frame.size - open - 2} : PrStringView{};
        } else if (prStringStartsWith(frame, "at ")) {
            frame = PrStringView{frame.data + 3, frame.size - 3};
        }
        size_t columnColon = prStringFindLastByte(frame, ':');
        size_t lineColon =
            columnColon < frame.size ? prStringFindLastByte(PrStringView{frame.data, columnColon}, ':') : frame.size;
        if (lineColon < frame.size) {
            init.filename = PrStringView{frame.data, lineColon};
            uint64_t line = 0;
            uint64_t column = 0;
            prU64Parse(frame.data + lineColon + 1, columnColon - lineColon - 1, &line, 10);
            prU64Parse(frame.data + columnColon + 1, frame.size - columnColon - 1, &column, 10);
            init.lineno = uint32_t(line < UINT32_MAX ? line : UINT32_MAX);
            init.colno = uint32_t(column < UINT32_MAX ? column : UINT32_MAX);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, JS_GetException(ctx));
    init.message = message.value();
    PrHtmlErrorEvent* event = prHtmlErrorEventCreate("error", &init);
    JSValue object = jsWrap(ctx, htmlErrorEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object)) {
        prDomEventRelease(prHtmlErrorEventAsEvent(event));
        jsPrintException(ctx, error);
        return;
    }
    jsSetReference(ctx, object, kErrorSlot, error);
    if (prHtmlWindowReportError(window, event))
        jsPrintException(ctx, error);
    JS_FreeValue(ctx, object);
}

constexpr JsExceptionReporter kReporter = {reportException};

// ---------------------------------------------------------------------------------------------------------------------
// MessageEvent (html/#messageevent) and postMessage (html/#dom-window-postmessage)
//
// A message is serialized in the engine's own format, which preserves an object graph with its references
// (html/#structuredserialize). It is deserialized in the realm that reads it.

// The slots of a MessageEvent's object.
enum {
    MessageSlot_Data,
    MessageSlot_Ports,
};
typedef uint8_t MessageSlot;

// MessageEventInit (html/#messageeventinit), after EventInit: data, lastEventId, origin, ports and source. The source
// is a window or null.
JSValue messageConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument lastEventId = {ctx, ""};
    StringArgument origin = {ctx, ""};
    PrHtmlMessageEventInit init = prHtmlMessageEventInitDefault();
    bool present = false;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    JSValue data = JS_NULL;
    JSValue ports = JS_UNDEFINED;
    if (present) {
        if (!prJsDomEventInitRead(ctx, dictionary, &init.event))
            return JS_EXCEPTION;
        data = JS_GetPropertyStr(ctx, dictionary, "data");
        if (JS_IsException(data))
            return data;
        if (JS_IsUndefined(data))
            data = JS_NULL;
        JSValue source = JS_UNDEFINED;
        bool ok = jsDictString(ctx, dictionary, "lastEventId", &lastEventId) && jsDictString(ctx, dictionary, "origin", &origin);
        if (ok) {
            source = JS_GetPropertyStr(ctx, dictionary, "source");
            ok = !JS_IsException(source);
        }
        if (ok && !JS_IsUndefined(source) && !JS_IsNull(source)) {
            PrHtmlWindow* window = jsHandleAs<PrHtmlWindow>(ctx, source, htmlWindowClass);
            if (window == nullptr && jsIsGlobal(ctx, source))
                window = globalWindow(ctx);
            PrHtmlMessagePort* port = jsHtmlMessagePortOf(ctx, source);
            if (window != nullptr) {
                init.source = prHtmlWindowAsEventTarget(window);
            } else if (port != nullptr) {
                init.source = prHtmlMessagePortAsEventTarget(port);
            } else {
                JS_ThrowTypeError(ctx, "source is not a window or a port");
                ok = false;
            }
        }
        JS_FreeValue(ctx, source);
        if (ok) {
            JSValue given = JS_GetPropertyStr(ctx, dictionary, "ports");
            ok = !JS_IsException(given);
            if (ok && !JS_IsUndefined(given)) {
                ports = JS_NewArray(ctx);
                uint32_t count = 0;
                ok = jsSequence(ctx, given, [&](JSValue item) {
                    bool isPort = jsHtmlMessagePortOf(ctx, item) != nullptr;
                    if (isPort)
                        JS_SetPropertyUint32(ctx, ports, count++, item);
                    else
                        JS_FreeValue(ctx, item);
                    if (!isPort)
                        JS_ThrowTypeError(ctx, "A message event's ports are not all MessagePorts");
                    return isPort;
                });
                JS_FreezeObject(ctx, ports);
            }
            JS_FreeValue(ctx, given);
        }
        if (!ok) {
            JS_FreeValue(ctx, ports);
            JS_FreeValue(ctx, data);
            return JS_EXCEPTION;
        }
        init.lastEventId = lastEventId.value();
        init.origin = origin.value();
    }
    PrHtmlMessageEvent* event = prHtmlMessageEventCreate(type.value(), &init);
    JSValue object = jsWrap(ctx, htmlMessageEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object)) {
        prDomEventRelease(prHtmlMessageEventAsEvent(event));
    } else {
        jsSetReference(ctx, object, MessageSlot_Data, data);
        if (!JS_IsUndefined(ports))
            jsSetReference(ctx, object, MessageSlot_Ports, ports);
    }
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, ports);
    return jsConstructed(ctx, newTarget, object);
}

// Returns the frozen array of the event's ports, whose transfer finishes in this realm the first time it is read.
JSValue messagePortsOf(JSContext* ctx, JSValueConst portsObject, PrHtmlMessageEvent* event) {
    JSValue ports = jsReference(ctx, portsObject, MessageSlot_Ports);
    if (!JS_IsUndefined(ports))
        return ports;
    Array<PrHtmlMessagePort*> list;
    for (uint32_t i = 0; i < prHtmlMessageEventPortCount(event); ++i)
        list.push(prHtmlMessageEventPort(event, i));
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr && list.count != 0) {
        list.reset();
        return JS_ThrowTypeError(ctx, "A message's ports need a window");
    }
    ports = jsHtmlMessagePortsAdopt(ctx, list.data, list.count, window);
    list.reset();
    jsSetReference(ctx, portsObject, MessageSlot_Ports, ports);
    return ports;
}

// data: the value that the event was created with, or the message that it carries, deserialized once in this realm.
JSValue messageData(JSContext* ctx, JSValueConst eventObject) {
    PrHtmlMessageEvent* event = jsThis<PrHtmlMessageEvent>(ctx, eventObject, htmlMessageEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    JSValue data = jsReference(ctx, eventObject, MessageSlot_Data);
    if (!JS_IsUndefined(data))
        return data;
    PrStringView bytes = prHtmlMessageEventSerializedData(event);
    if (bytes.data == nullptr)
        return JS_NULL;
    JSValue ports = messagePortsOf(ctx, eventObject, event);
    if (JS_IsException(ports))
        return ports;
    data = jsHtmlDeserialize(ctx, bytes, ports);
    JS_FreeValue(ctx, ports);
    if (JS_IsException(data))
        return data;
    jsSetReference(ctx, eventObject, MessageSlot_Data, data);
    return data;
}

JSValue messageSource(JSContext* ctx, JSValueConst eventObject) {
    PrHtmlMessageEvent* event = jsThis<PrHtmlMessageEvent>(ctx, eventObject, htmlMessageEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    PrDomEventTarget* source = prHtmlMessageEventSource(event);
    if (source == nullptr)
        return JS_NULL;
    PrHtmlWindow* window = prHtmlWindowQuery(source);
    return window != nullptr ? globalOf(ctx, window) : jsWrap(ctx, htmlMessagePortClass, JS_UNDEFINED, source, false);
}

// ports, a FrozenArray of the ports that a message transferred, or that the event was created with.
JSValue messagePorts(JSContext* ctx, JSValueConst eventObject) {
    PrHtmlMessageEvent* event = jsThis<PrHtmlMessageEvent>(ctx, eventObject, htmlMessageEventClass);
    return event != nullptr ? messagePortsOf(ctx, eventObject, event) : JS_EXCEPTION;
}

// postMessage(message, targetOrigin, transfer) and postMessage(message, options). The source is the window of the
// incumbent realm. A message that the engine cannot serialize throws a DataCloneError, and a target origin that is not
// a URL throws a SyntaxError.
JSValue postMessage(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    PrHtmlWindow* source = globalWindow(jsIncumbentRealm(ctx));
    StringArgument targetOrigin = {ctx, "/"};
    if (window == nullptr || source == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    Array<JSValue> transfer;
    bool ok = true;
    if (argc > 1 && JS_IsObject(argv[1])) {
        ok = jsDictString(ctx, argv[1], "targetOrigin", &targetOrigin) && jsHtmlTransferOption(ctx, argv[1], &transfer);
    } else {
        ok = !(argc > 1 && !JS_IsUndefined(argv[1]) && !targetOrigin.read(argv[1])) &&
            !(argc > 2 && !JS_IsUndefined(argv[2]) && !jsHtmlTransferRead(ctx, argv[2], &transfer));
    }
    JsHtmlSerialized message{};
    ok = ok && jsHtmlSerialize(ctx, argv[0], &transfer, nullptr, &message);
    jsHtmlTransferReset(ctx, &transfer);
    if (!ok)
        return JS_EXCEPTION;
    Array<PrDomEventTarget*> ports;
    for (uint32_t i = 0; i < message.ports.count; ++i)
        ports.push(prHtmlMessagePortAsEventTarget(message.ports.data[i]));
    PrStatus status = prHtmlWindowPostMessage(window, source, prStringView(&message.bytes), ports.data, ports.count,
        targetOrigin.value());
    if (status != PrStatus_Ok) {
        for (uint32_t i = 0; i < ports.count; ++i)
            prDomEventTargetRelease(ports.data[i]);
    }
    ports.reset();
    message.ports.reset();
    prStringReset(&message.bytes);
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "SyntaxError");
}

// ---------------------------------------------------------------------------------------------------------------------
// PageRevealEvent (html/#pagerevealevent)

// PageRevealEventInit, after EventInit: a view transition. The engine has none to provide.
JSValue pageRevealConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    if (present) {
        if (!prJsDomEventInitRead(ctx, dictionary, &init))
            return JS_EXCEPTION;
        JSValue transition = JS_GetPropertyStr(ctx, dictionary, "viewTransition");
        if (JS_IsException(transition))
            return transition;
        bool none = JS_IsUndefined(transition) || JS_IsNull(transition);
        JS_FreeValue(ctx, transition);
        if (!none)
            return JS_ThrowTypeError(ctx, "viewTransition is not a ViewTransition");
    }
    PrHtmlPageRevealEvent* event = prHtmlPageRevealEventCreate(type.value(), &init);
    JSValue object = jsWrap(ctx, htmlPageRevealEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object))
        prDomEventRelease(prHtmlPageRevealEventAsEvent(event));
    return jsConstructed(ctx, newTarget, object);
}

JSValue pageRevealViewTransition(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlPageRevealEvent>(ctx, object, htmlPageRevealEventClass) != nullptr ? JS_NULL : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// PromiseRejectionEvent (html/#promiserejectionevent)

// The slots of a PromiseRejectionEvent's object.
enum {
    RejectionSlot_Promise,
    RejectionSlot_Reason,
};
typedef uint8_t RejectionSlot;

// Creates a PromiseRejectionEvent object that holds `promise` and `reason`.
JSValue rejectionEvent(JSContext* ctx, PrStringView type, const PrDomEventInit* init, JSValueConst promise, JSValueConst reason) {
    PrHtmlPromiseRejectionEvent* event = prHtmlPromiseRejectionEventCreate(type, init);
    JSValue object = jsWrap(ctx, htmlPromiseRejectionEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object)) {
        prDomEventRelease(prHtmlPromiseRejectionEventAsEvent(event));
        return object;
    }
    jsSetReference(ctx, object, RejectionSlot_Promise, promise);
    jsSetReference(ctx, object, RejectionSlot_Reason, reason);
    return object;
}

JSValue rejectionConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    // PromiseRejectionEventInit (html/#promiserejectioneventinit), after EventInit: promise, which is required, then
    // reason.
    if (!jsArgCount(ctx, argc, 2) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present) ||
            !prJsDomEventInitRead(ctx, dictionary, &init))
        return JS_EXCEPTION;
    JSValue promise = JS_GetPropertyStr(ctx, dictionary, "promise");
    if (JS_IsException(promise))
        return promise;
    if (!JS_IsObject(promise)) {
        JS_FreeValue(ctx, promise);
        return JS_ThrowTypeError(ctx, "PromiseRejectionEventInit needs a promise");
    }
    JSValue reason = JS_GetPropertyStr(ctx, dictionary, "reason");
    if (JS_IsException(reason)) {
        JS_FreeValue(ctx, promise);
        return reason;
    }
    JSValue object = rejectionEvent(ctx, type.value(), &init, promise, reason);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, reason);
    return jsConstructed(ctx, newTarget, object);
}

// promise and reason, selected by the magic, which is the slot.
JSValue rejectionMember(JSContext* ctx, JSValueConst object, int magic) {
    if (jsThis<PrHtmlPromiseRejectionEvent>(ctx, object, htmlPromiseRejectionEventClass) == nullptr)
        return JS_EXCEPTION;
    return jsReference(ctx, object, uint32_t(magic));
}

// html/#the-hostpromiserejectiontracker-implementation: a promise rejected with no handler is added to the window's
// list. A promise that gets a handler before the notification is removed from the list.
void trackRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void*) {
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr)
        return;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue list = jsReference(ctx, object, WindowSlot_Rejected);
    if (JS_IsUndefined(list)) {
        list = JS_NewArray(ctx);
        jsSetReference(ctx, object, WindowSlot_Rejected, list);
    }
    int64_t length = 0;
    JS_GetLength(ctx, list, &length);
    if (!handled) {
        JS_SetPropertyUint32(ctx, list, uint32_t(length), jsEntry(ctx, JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)));
    } else {
        // The list is rebuilt without the promise, which appears in it at most once.
        JSValue kept = JS_NewArray(ctx);
        uint32_t count = 0;
        for (int64_t i = 0; i < length; ++i) {
            JSValue entry = JS_GetPropertyUint32(ctx, list, uint32_t(i));
            JSValue held = JS_GetPropertyUint32(ctx, entry, 0);
            bool same = JS_IsStrictEqual(ctx, held, promise);
            JS_FreeValue(ctx, held);
            if (same)
                JS_FreeValue(ctx, entry);
            else
                JS_SetPropertyUint32(ctx, kept, count++, entry);
        }
        jsSetReference(ctx, object, WindowSlot_Rejected, kept);
        JS_FreeValue(ctx, kept);
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, object);
}

// ---------------------------------------------------------------------------------------------------------------------
// Window (html/#window)

// Returns the object of the window's document, which the window's object `holder` holds (WindowSlot_Document).
JSValue heldDocument(JSContext* ctx, JSValueConst holder, PrHtmlWindow* window) {
    JSValue object = jsWrapAs(ctx, prJsDomDocumentClass, holder, prHtmlWindowDocument(window), false);
    if (JS_IsException(object))
        return object;
    JSValue held = jsReference(ctx, holder, WindowSlot_Document);
    bool current = JS_VALUE_GET_PTR(held) == JS_VALUE_GET_PTR(object);
    JS_FreeValue(ctx, held);
    if (!current)
        jsSetReference(ctx, holder, WindowSlot_Document, object);
    return object;
}

JSValue document(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    // The receiver may be the global, whose prototype is the window's object.
    JSValue holder = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    if (JS_IsException(holder))
        return holder;
    JSValue object = heldDocument(ctx, holder, window);
    JS_FreeValue(ctx, holder);
    return object;
}

// cssom-1/#dom-window-getcomputedstyle: returns the element's computed style, which keeps the element alive. A
// pseudo-element has no style of its own here.
JSValue getComputedStyle(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomElement* element = jsValueOf<PrDomElement>(ctx, argv[0], prJsDomElementClass, "an Element");
    if (element == nullptr)
        return JS_EXCEPTION;
    // An optional, nullable pseudo-element (cssom-1/#dom-window-getcomputedstyle).
    StringArgument pseudoElement = {ctx};
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !pseudoElement.readNullable(argv[1]))
        return JS_EXCEPTION;
    return jsWrap(ctx, prJsCssStylePropertiesClass, argv[0], prCssElementComputedStyle(element, pseudoElement.value()), false);
}

JSValue matchMedia(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    StringArgument query = {ctx};
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || !query.read(argv[0]))
        return JS_EXCEPTION;
    PrCssMediaQueryList* list = prHtmlWindowMatchMedia(window, query.value());
    JSValue object = jsWrap(ctx, prJsCssMediaQueryListClass, windowObject, list, true);
    if (JS_IsException(object))
        prCssMediaQueryListRelease(list);
    return object;
}

// html/#dom-btoa: the base64 of the bytes that the string's code units are. A code unit above U+00FF is not a byte.
JSValue btoa(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    StringArgument data = {ctx};
    if (jsThis<PrHtmlWindow>(ctx, object, htmlWindowClass) == nullptr || !jsArgCount(ctx, argc, 1) ||
            !data.read(argv[0]))
        return JS_EXCEPTION;
    PrStringView text = data.value();
    PrTextBuffer bytes = {};
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        if (codePoint > 0xFF) {
            prTextBufferReset(&bytes);
            return jsDomExceptionThrow(ctx, "InvalidCharacterError");
        }
        prTextBufferAppendChar(&bytes, char(codePoint));
    }
    PrStringView input = prTextBufferView(&bytes);
    size_t size = prBase64EncodedSize(input.size);
    char* encoded = static_cast<char*>(prMemoryAlloc(size));
    prBase64Encode(input, encoded, size);
    JSValue result = JS_NewStringLen(ctx, encoded, size);
    prMemoryFree(encoded);
    prTextBufferReset(&bytes);
    return result;
}

// html/#dom-atob: the bytes that the forgiving-base64 decode of the string gives, each as a code unit.
JSValue atob(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    StringArgument data = {ctx};
    if (jsThis<PrHtmlWindow>(ctx, object, htmlWindowClass) == nullptr || !jsArgCount(ctx, argc, 1) ||
            !data.read(argv[0]))
        return JS_EXCEPTION;
    PrStringView text = data.value();
    size_t capacity = prBase64DecodedCapacity(text.size);
    char* decoded = static_cast<char*>(prMemoryAlloc(capacity != 0 ? capacity : 1));
    size_t size = 0;
    if (!prBase64Decode(text, decoded, capacity, &size)) {
        prMemoryFree(decoded);
        return jsDomExceptionThrow(ctx, "InvalidCharacterError");
    }
    PrTextBuffer out = {};
    for (size_t i = 0; i < size; ++i)
        prTextBufferAppendCodePoint(&out, uint8_t(decoded[i]));
    prMemoryFree(decoded);
    PrStringView view = prTextBufferView(&out);
    JSValue result = JS_NewStringLen(ctx, view.data, view.size);
    prTextBufferReset(&out);
    return result;
}

// Calls a timer's or an animation frame's callback with the global as the receiver, and reports any exception that it
// throws.
void callWithGlobal(JsCallback* callback, int argc, JSValueConst* PR_NULLABLE argv) {
    JSContext* ctx = callback->ctx;
    JSValue receiver = JS_GetGlobalObject(ctx);
    JSValue result = jsCallbackCall(callback, nullptr, receiver, argc, argv);
    JS_FreeValue(ctx, receiver);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
}

void invokeTimer(void* PR_NULLABLE context) {
    callWithGlobal(static_cast<JsCallback*>(context), 0, nullptr);
}

// Calls the function that `data` holds first with the global as the receiver and the rest of `data` as the arguments.
JSValue callTimerFunction(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
    JSValue global = JS_GetGlobalObject(ctx);
    int count = 0;
    JS_ToInt32(ctx, &count, data[1]);
    JSValue result = JS_Call(ctx, data[0], global, count, data + 2);
    JS_FreeValue(ctx, global);
    return result;
}

// Returns the function that a timer calls (html/#timer-initialisation-steps): `argv[0]` with the arguments after the
// timeout. A handler that is not a function is converted to a string, which the global's eval runs as a classic script
// is run.
JSValue timerFunction(JSContext* ctx, int argc, JSValueConst* argv) {
    Array<JSValue> data;
    if (JS_IsFunction(ctx, argv[0])) {
        data.push(JS_DupValue(ctx, argv[0]));
        data.push(JS_NewInt32(ctx, argc > 2 ? argc - 2 : 0));
        for (int i = 2; i < argc; ++i)
            data.push(JS_DupValue(ctx, argv[i]));
    } else {
        JSValue code = JS_ToString(ctx, argv[0]);
        if (JS_IsException(code))
            return code;
        JSValue global = JS_GetGlobalObject(ctx);
        data.push(JS_GetPropertyStr(ctx, global, "eval"));
        JS_FreeValue(ctx, global);
        data.push(JS_NewInt32(ctx, 1));
        data.push(code);
    }
    JSValue function = JS_NewCFunctionData(ctx, callTimerFunction, 0, 0, int(data.count), data.data);
    for (uint32_t i = 0; i < data.count; ++i)
        JS_FreeValue(ctx, data.data[i]);
    data.reset();
    return function;
}

// html/#dom-settimeout: setTimeout and setInterval, whose magic is 1 for an interval.
JSValue setTimeout(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    int32_t timeout = 0;
    if (jsArgGiven(argc, argv, 1) && JS_ToInt32(ctx, &timeout, argv[1]) < 0)
        return JS_EXCEPTION;
    // A function handler with no arguments is called as it is.
    bool bare = JS_IsFunction(ctx, argv[0]) && argc <= 2;
    JSValue function = bare ? JS_DupValue(ctx, argv[0]) : timerFunction(ctx, argc, argv);
    if (JS_IsException(function))
        return function;
    CallbackArgument<PrHtmlTimerHandler> handler = {ctx, JS_UNDEFINED};
    bool ok = handler.readFunction(function, invokeTimer);
    JS_FreeValue(ctx, function);
    if (!ok)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, prHtmlWindowSetTimeout(window, handler.get(), timeout, magic != 0));
}

JSValue clearTimeout(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    int32_t id = 0;
    if (window == nullptr || (jsArgGiven(argc, argv, 0) && JS_ToInt32(ctx, &id, argv[0]) < 0))
        return JS_EXCEPTION;
    prHtmlWindowClearTimeout(window, id);
    return JS_UNDEFINED;
}

// Aborts the signal of AbortSignal.timeout with a TimeoutError, from the timer's task. The timer's callback holds the
// signal's object, which keeps the signal's listeners until the timer fires or the window goes.
void abortOnTimeout(void* PR_NULLABLE context) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    if (callback->runtime == nullptr)
        return;
    JSContext* ctx = callback->ctx;
    PrDomAbortSignal* signal = jsHandleAs<PrDomAbortSignal>(ctx, callback->value, prJsDomAbortSignalClass);
    JSValue error = jsDomExceptionNew(ctx, "TimeoutError");
    if (JS_IsException(error)) {
        jsReportException(ctx);
        return;
    }
    prDomAbortSignalSignalAbort(signal, &error);
    JS_FreeValue(ctx, error);
}

// dom/#dom-abortsignal-timeout, on the timers of the realm's window.
JSValue abortSignalTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    double milliseconds = 0;
    if (!jsArgCount(ctx, argc, 1) || !jsEnforceRange(ctx, argv[0], 0, kJsMaxSafeInteger, &milliseconds))
        return JS_EXCEPTION;
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(jsGlobalHandleIn(ctx, htmlWindowClass));
    JSValue object = jsWrap(ctx, prJsDomAbortSignalClass, JS_UNDEFINED, prDomAbortSignalCreate(), true);
    if (JS_IsException(object) || window == nullptr)
        return object;
    JsCallback* callback = jsCallbackCreate(ctx, object, JS_UNDEFINED);
    PrHtmlTimerHandler handler = {abortOnTimeout, callback, jsCallbackRetain, jsCallbackRelease};
    prHtmlWindowRunAfterTimeout(window, &handler, milliseconds);
    jsCallbackRelease(callback);
    return object;
}

constexpr JSCFunctionListEntry kAbortSignalStatic[] = {PR_JS_METHOD("timeout", 1, abortSignalTimeout)};

// The frame's time is on the same clock as performance.now().
void invokeFrame(void* PR_NULLABLE context, double now) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSValue time = JS_NewFloat64(callback->ctx, now);
    callWithGlobal(callback, 1, &time);
}

JSValue requestAnimationFrame(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    CallbackArgument<PrHtmlFrameRequestCallback> callback = {ctx, JS_UNDEFINED};
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeFrame))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, prHtmlWindowRequestAnimationFrame(window, callback.get()));
}

// structuredClone(value, options) (html/#dom-structuredclone).
JSValue structuredClone(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    Array<JSValue> transfer;
    bool present = false;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool ok = jsDictionary(ctx, options, &present) && (!present || jsHtmlTransferOption(ctx, options, &transfer));
    JsHtmlSerialized message{};
    ok = ok && jsHtmlSerialize(ctx, argv[0], &transfer, nullptr, &message);
    jsHtmlTransferReset(ctx, &transfer);
    if (!ok)
        return JS_EXCEPTION;
    JSValue ports = jsHtmlMessagePortsAdopt(ctx, message.ports.data, message.ports.count, window);
    JSValue clone = jsHtmlDeserialize(ctx, prStringView(&message.bytes), ports);
    JS_FreeValue(ctx, ports);
    jsHtmlSerializedReset(&message);
    return clone;
}

// IdleDeadline (requestidlecallback/#the-idledeadline-interface). It holds its window, whose clock it reads.
struct HtmlIdleDeadline {
    PrHtmlWindow* window;
    double deadline;
    bool didTimeout;
};

void freeIdleDeadline(HtmlIdleDeadline* PR_NULLABLE deadline) {
    if (deadline == nullptr)
        return;
    prDomEventTargetRelease(prHtmlWindowAsEventTarget(deadline->window));
    prMemoryFree(deadline);
}

JSValue idleDeadlineTimeRemaining(JSContext* ctx, JSValueConst deadlineObject, int, JSValueConst*) {
    HtmlIdleDeadline* deadline = jsThis<HtmlIdleDeadline>(ctx, deadlineObject, htmlIdleDeadlineClass);
    if (deadline == nullptr)
        return JS_EXCEPTION;
    if (deadline->didTimeout)
        return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, prHtmlWindowIdleTimeRemaining(deadline->window, deadline->deadline));
}

JSValue idleDeadlineDidTimeout(JSContext* ctx, JSValueConst deadlineObject) {
    HtmlIdleDeadline* deadline = jsThis<HtmlIdleDeadline>(ctx, deadlineObject, htmlIdleDeadlineClass);
    return deadline != nullptr ? JS_NewBool(ctx, deadline->didTimeout) : JS_EXCEPTION;
}

constexpr JSCFunctionListEntry kIdleDeadlineProto[] = {
    PR_JS_TAG("IdleDeadline"),
    PR_JS_METHOD("timeRemaining", 0, idleDeadlineTimeRemaining),
    PR_JS_GET("didTimeout", idleDeadlineDidTimeout),
};

void invokeIdle(void* PR_NULLABLE context, double end, bool didTimeout) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr)
        return;
    HtmlIdleDeadline* deadline = allocZeroed<HtmlIdleDeadline>();
    *deadline = HtmlIdleDeadline{window, end, didTimeout};
    prDomEventTargetRetain(prHtmlWindowAsEventTarget(window));
    JSValue object = jsWrap(ctx, htmlIdleDeadlineClass, JS_UNDEFINED, deadline, true);
    callWithGlobal(callback, 1, &object);
    JS_FreeValue(ctx, object);
}

// requestIdleCallback(callback, options) (requestidlecallback/#the-requestidlecallback-method).
JSValue requestIdleCallback(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    CallbackArgument<PrHtmlIdleRequestCallback> callback = {ctx, JS_UNDEFINED};
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeIdle))
        return JS_EXCEPTION;
    uint32_t timeout = 0;
    bool present = false;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsDictionary(ctx, options, &present))
        return JS_EXCEPTION;
    if (present && !jsDictUint32(ctx, options, "timeout", &timeout))
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, prHtmlWindowRequestIdleCallback(window, callback.get(), timeout));
}

JSValue cancelIdleCallback(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    uint32_t handle = 0;
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &handle, argv[0]) < 0)
        return JS_EXCEPTION;
    prHtmlWindowCancelIdleCallback(window, handle);
    return JS_UNDEFINED;
}

JSValue cancelAnimationFrame(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    uint32_t handle = 0;
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &handle, argv[0]) < 0)
        return JS_EXCEPTION;
    prHtmlWindowCancelAnimationFrame(window, handle);
    return JS_UNDEFINED;
}

// The magic of the [Replaceable] parent attribute, by which setReplaceable names it.
constexpr int kParentMagic = 7;

// html/#dom-parent and html/#dom-top, selected by the magic. Returns the global of the window's parent for
// kParentMagic, and otherwise of its outermost ancestor. Returns the window's own global if it has no such ancestor.
JSValue ancestor(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    for (PrHtmlWindow* parent = prHtmlWindowParent(window); parent != nullptr; parent = prHtmlWindowParent(window)) {
        window = parent;
        if (magic == kParentMagic)
            break;
    }
    return globalOf(ctx, window);
}

// [Replaceable] performance (hr-time-3/#the-performance-attribute). The window owns it, and the window's object holds
// its object.
JSValue performance(JSContext* ctx, JSValueConst windowObject, int) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue holder = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    if (JS_IsException(holder))
        return holder;
    JSValue object = jsReference(ctx, holder, WindowSlot_Performance);
    if (JS_IsUndefined(object)) {
        object = jsWrapAs(ctx, htmlPerformanceClass, JS_UNDEFINED,
            prHtmlPerformanceAsEventTarget(prHtmlWindowPerformance(window)), false);
        if (!JS_IsException(object))
            jsSetReference(ctx, holder, WindowSlot_Performance, object);
    }
    JS_FreeValue(ctx, holder);
    return object;
}

// A [Replaceable] attribute's setter (webidl/#Replaceable): the value becomes an own property of the receiver, named by
// the attribute that the magic selects.
JSValue setReplaceable(JSContext* ctx, JSValueConst object, JSValueConst input, int magic) {
    constexpr const char* kNames[] = {
        "performance", "innerWidth",  "innerHeight", "scrollX",
        "scrollY",     "pageXOffset", "pageYOffset", "parent",
        "devicePixelRatio", "length",
    };
    if (JS_DefinePropertyValueStr(ctx, object, kNames[magic], JS_DupValue(ctx, input), JS_PROP_C_W_E) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

// The [Replaceable] measures of the viewport (cssom-view-1/#extensions-to-the-window-interface), selected by the magic
// by which setReplaceable names them.
JSValue viewportGet(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case 1: return JS_NewInt32(ctx, prHtmlWindowInnerWidth(window));
        case 2: return JS_NewInt32(ctx, prHtmlWindowInnerHeight(window));
        case 3:
        case 5: return JS_NewFloat64(ctx, prHtmlWindowScrollX(window));
        case 8: return JS_NewFloat64(ctx, prHtmlWindowDevicePixelRatio(window));
        default: return JS_NewFloat64(ctx, prHtmlWindowScrollY(window));
    }
}

// scroll and scrollTo for magic 0, and scrollBy for 1 (cssom-view-1/#dom-window-scroll). Each scroll completes as soon
// as it is made.
JSValue windowScroll(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    PrCssScrollToOptions options{};
    if (window == nullptr || !prJsCssScrollToOptionsRead(ctx, argc, argv, &options))
        return prJsCssScrollPromise(ctx, false);
    if (magic == 0)
        prHtmlWindowScroll(window, &options);
    else
        prHtmlWindowScrollBy(window, &options);
    return prJsCssScrollPromise(ctx, true);
}

JSValue performanceNow(JSContext* ctx, JSValueConst performanceObject, int, JSValueConst*) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    return performance != nullptr ? JS_NewFloat64(ctx, prHtmlPerformanceNow(performance)) : JS_EXCEPTION;
}

// hr-time-3/#dom-performance-tojson: an object of its attributes.
JSValue performanceToJson(JSContext* ctx, JSValueConst performanceObject, int, JSValueConst*) {
    PrHtmlPerformance* performance = jsThis<PrHtmlPerformance>(ctx, performanceObject, htmlPerformanceClass);
    if (performance == nullptr)
        return JS_EXCEPTION;
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "timeOrigin", JS_NewFloat64(ctx, prHtmlPerformanceTimeOrigin(performance)));
    constexpr const char* kMembers[] = {"timing", "navigation"};
    for (const char* name : kMembers) {
        JSValue member = JS_GetPropertyStr(ctx, performanceObject, name);
        JSValue method = JS_GetPropertyStr(ctx, member, "toJSON");
        JS_SetPropertyStr(ctx, object, name, JS_Call(ctx, method, member, 0, nullptr));
        JS_FreeValue(ctx, method);
        JS_FreeValue(ctx, member);
    }
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// AnimationTimeline and DocumentTimeline (web-animations-1/#the-animationtimeline-interface)

JSValue timelineCurrentTime(JSContext* ctx, JSValueConst timelineObject) {
    PrHtmlDocumentTimeline* timeline = jsThis<PrHtmlDocumentTimeline>(ctx, timelineObject, htmlAnimationTimelineClass);
    if (timeline == nullptr)
        return JS_EXCEPTION;
    double time = 0;
    return prHtmlDocumentTimelineCurrentTime(timeline, &time) ? JS_NewFloat64(ctx, time) : JS_NULL;
}

// duration (web-animations-2/#dom-animationtimeline-duration). A document timeline has none.
JSValue timelineDuration(JSContext* ctx, JSValueConst timelineObject) {
    PrHtmlDocumentTimeline* timeline = jsThis<PrHtmlDocumentTimeline>(ctx, timelineObject, htmlAnimationTimelineClass);
    return timeline != nullptr ? JS_NULL : JS_EXCEPTION;
}

// Creates a timeline of `document`, whose object keeps the document's object alive.
JSValue timelineOf(JSContext* ctx, JSValueConst documentObject, PrDomDocument* document, double originTime) {
    PrHtmlDocumentTimeline* timeline = prHtmlDocumentTimelineCreate(document, originTime);
    JSValue object = jsWrap(ctx, htmlDocumentTimelineClass, documentObject, timeline, true);
    if (JS_IsException(object))
        prHtmlDocumentTimelineDestroy(timeline);
    return object;
}

// DocumentTimeline(options): a timeline of the document of the current global.
JSValue timelineConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    bool present = false;
    double originTime = 0;
    JSValueConst dictionary = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!jsDictionary(ctx, dictionary, &present) ||
            (present && !jsDictNumber(ctx, dictionary, "originTime", &originTime)))
        return JS_EXCEPTION;
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    JSValue documentObject = jsWrapAs(ctx, prJsDomDocumentClass, JS_UNDEFINED, document, false);
    JSValue object = timelineOf(ctx, documentObject, document, originTime);
    JS_FreeValue(ctx, documentObject);
    return jsConstructed(ctx, newTarget, object);
}

// [SameObject] timeline (web-animations-1/#dom-document-timeline): the document's default timeline, whose origin time
// is zero.
JSValue documentTimeline(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, documentObject, "timeline", [&] { return timelineOf(ctx, documentObject, document, 0); });
}

JSValue navigator(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, "navigator", [&] {
        return jsWrap(ctx, htmlNavigatorClass, object, window, false);
    });
    JS_FreeValue(ctx, object);
    return result;
}

// [SameObject] screen (cssom-view-1/#dom-window-screen): a view of the window.
JSValue screen(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, "screen", [&] {
        return jsWrap(ctx, htmlScreenClass, object, window, false);
    });
    JS_FreeValue(ctx, object);
    return result;
}

// The attributes of Screen (cssom-view-1/#the-screen-interface), selected by the magic.
enum {
    ScreenMeasure_Width,
    ScreenMeasure_Height,
    ScreenMeasure_ColorDepth,   // which pixelDepth also is
};
typedef int ScreenMeasure;

JSValue screenMeasure(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlScreenClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    switch (ScreenMeasure(magic)) {
        case ScreenMeasure_Width: return JS_NewInt32(ctx, prHtmlWindowScreenWidth(window));
        case ScreenMeasure_Height: return JS_NewInt32(ctx, prHtmlWindowScreenHeight(window));
        default: return JS_NewInt32(ctx, prHtmlWindowScreenColorDepth(window));
    }
}

// [SameObject] orientation (screen-orientation/#dom-screen-orientation). The window owns it, and the window's object
// holds its object.
JSValue screenOrientation(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlScreenClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue holder = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    if (JS_IsException(holder))
        return holder;
    JSValue object = jsReference(ctx, holder, WindowSlot_ScreenOrientation);
    if (JS_IsUndefined(object)) {
        PrHtmlScreenOrientation* orientation = prHtmlWindowScreenOrientation(window);
        object = jsWrapAs(ctx, htmlScreenOrientationClass, JS_UNDEFINED,
            prHtmlScreenOrientationAsEventTarget(orientation), false);
        if (!JS_IsException(object))
            jsSetReference(ctx, holder, WindowSlot_ScreenOrientation, object);
    }
    JS_FreeValue(ctx, holder);
    return object;
}

JSValue orientationType(JSContext* ctx, JSValueConst orientationObject) {
    PrHtmlScreenOrientation* orientation =
        jsThis<PrHtmlScreenOrientation>(ctx, orientationObject, htmlScreenOrientationClass);
    if (orientation != nullptr)
        return jsFixedString(ctx, prHtmlOrientationTypeName(prHtmlScreenOrientationType(orientation)));
    return JS_EXCEPTION;
}

JSValue orientationAngle(JSContext* ctx, JSValueConst orientationObject) {
    PrHtmlScreenOrientation* orientation =
        jsThis<PrHtmlScreenOrientation>(ctx, orientationObject, htmlScreenOrientationClass);
    return orientation != nullptr ? JS_NewUint32(ctx, prHtmlScreenOrientationAngle(orientation)) : JS_EXCEPTION;
}

// lock (screen-orientation/#dom-screenorientation-lock): rejects with NotSupportedError, since a document cannot lock
// the orientation.
JSValue orientationLock(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (jsThis<PrHtmlScreenOrientation>(ctx, object, htmlScreenOrientationClass) == nullptr)
        return JS_EXCEPTION;
    JSValue error = jsDomExceptionNew(ctx, "NotSupportedError");
    JSValue promise = jsSettledPromise(ctx, error, true);
    JS_FreeValue(ctx, error);
    return promise;
}

// unlock (screen-orientation/#dom-screenorientation-unlock): does nothing, since the orientation is never locked.
JSValue orientationUnlock(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (jsThis<PrHtmlScreenOrientation>(ctx, object, htmlScreenOrientationClass) == nullptr)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

// selection-api/#dom-document-getselection: returns the document's selection, as the same object each time, if the
// document has a browsing context. Returns null for a document without one. The window's object holds the selection
// object of its current document, which lives as long as the document. The selection's range object lives as long as
// the selection object.
JSValue selectionOf(JSContext* ctx, PrDomDocument* document) {
    PrHtmlWindow* window = prHtmlDocumentDefaultView(document);
    if (window == nullptr)
        return JS_NULL;
    PrDomSelection* selection = prDomDocumentSelection(document);
    JSValue owner = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue held = jsSameObject(ctx, owner, "[[Selection]]");
    if (jsHandleAs<PrDomSelection>(ctx, held, prJsDomSelectionClass) != selection) {
        JS_FreeValue(ctx, held);
        held = jsWrap(ctx, prJsDomSelectionClass, owner, selection, false);
        jsSetSameObject(ctx, owner, "[[Selection]]", held);
    }
    JS_FreeValue(ctx, owner);
    return held;
}

JSValue documentGetSelection(JSContext* ctx, JSValueConst documentObject, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    return document != nullptr ? selectionOf(ctx, document) : JS_EXCEPTION;
}

JSValue windowGetSelection(JSContext* ctx, JSValueConst windowObject, int, JSValueConst*) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    return window != nullptr ? selectionOf(ctx, prHtmlWindowDocument(window)) : JS_EXCEPTION;
}

// html/#dom-window-customelements, held by the window's own object.
JSValue windowCustomElements(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsCustomElements(ctx, object);
    JS_FreeValue(ctx, object);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Location (html/#the-location-interface)

// Returns the window's Location: a view of the window, held as the [SameObject] location of the window's own object.
JSValue locationOf(JSContext* ctx, PrHtmlWindow* window) {
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, "location", [&] {
        return jsWrap(ctx, htmlLocationClass, object, window, false);
    });
    JS_FreeValue(ctx, object);
    return result;
}

JSValue windowLocation(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    return window != nullptr ? locationOf(ctx, window) : JS_EXCEPTION;
}

// Throws the error that setting the location returned, if it failed.
JSValue locationStatus(JSContext* ctx, PrStatus status) {
    if (status == PrStatus_Ok)
        return JS_UNDEFINED;
    return jsDomExceptionThrow(ctx, status == PrStatus_Syntax ? "SyntaxError" : "NotSupportedError");
}

// [PutForwards=href] [Replaceable] length (html/#dom-length): the number of the window's document-tree child
// navigables.
JSValue windowLength(JSContext* ctx, JSValueConst windowObject, int) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    return window != nullptr ? JS_NewUint32(ctx, prHtmlWindowLength(window)) : JS_EXCEPTION;
}

JSValue setWindowLocation(JSContext* ctx, JSValueConst windowObject, JSValueConst input) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    PrHtmlWindow* source = globalWindow(jsIncumbentRealm(ctx));
    StringArgument value = {ctx};
    if (window == nullptr || source == nullptr || !value.read(input))
        return JS_EXCEPTION;
    return locationStatus(
        ctx, prHtmlWindowSetLocation(window, source, PrUrlComponent_Href, value.value(), PrHtmlHistoryHandling_Push));
}

JSValue windowName(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    return window != nullptr ? jsTakeString(ctx, prHtmlWindowName(window)) : JS_EXCEPTION;
}

JSValue setWindowName(JSContext* ctx, JSValueConst windowObject, JSValueConst input) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    StringArgument value = {ctx};
    if (window == nullptr || !value.read(input))
        return JS_EXCEPTION;
    prHtmlWindowSetName(window, value.value());
    return JS_UNDEFINED;
}

// html/#dom-document-location: the Location of the window whose document this is, or null for a document with no
// window.
JSValue documentLocation(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrHtmlWindow* window = prHtmlDocumentDefaultView(document);
    return window != nullptr ? locationOf(ctx, window) : JS_NULL;
}

JSValue defaultView(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    return document != nullptr ? globalOf(ctx, prHtmlDocumentDefaultView(document)) : JS_EXCEPTION;
}

// open(), which returns the document, and open(url, name, features), which is window.open(). The engine opens no
// windows.
JSValue documentOpen(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    if (argc > 2)
        return jsDomExceptionThrow(ctx, "NotSupportedError");
    PrStatus status = prHtmlDocumentOpen(document);
    return status == PrStatus_Ok ? JS_DupValue(ctx, documentObject) : jsDomExceptionThrow(ctx, "InvalidStateError");
}

// write(...text) and writeln(...text). The magic says whether a line feed follows.
JSValue documentWrite(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv, int magic) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrTextBuffer text = {};
    for (int i = 0; i < argc; ++i) {
        StringArgument part = {ctx};
        if (!part.read(argv[i])) {
            prTextBufferReset(&text);
            return JS_EXCEPTION;
        }
        prTextBufferAppend(&text, part.value());
    }
    if (magic != 0)
        prTextBufferAppendChar(&text, '\n');
    PrStatus status = prHtmlDocumentWrite(document, prTextBufferView(&text));
    prTextBufferReset(&text);
    if (status == PrStatus_InvalidState)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return status == PrStatus_Ok ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue documentClose(JSContext* ctx, JSValueConst documentObject, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrStatus status = prHtmlDocumentClose(document);
    if (status == PrStatus_InvalidState)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return status == PrStatus_Ok ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue readyState(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    constexpr PrStringView kStates[] = {"loading", "interactive", "complete"};
    return jsFixedString(ctx, kStates[prHtmlDocumentReadyState(document)]);
}

// html/#dom-document-cookie, as for a user agent that blocks every cookie: a document reads no cookies and its writes
// are ignored. The value is still converted to a string, which may run script and throw.
JSValue documentCookie(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrDomDocument>(ctx, object, prJsDomDocumentClass) == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, "");
}

JSValue setDocumentCookie(JSContext* ctx, JSValueConst object, JSValueConst input) {
    StringArgument value = {ctx};
    if (jsThis<PrDomDocument>(ctx, object, prJsDomDocumentClass) == nullptr || !value.read(input))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

JSValue documentReferrer(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    return document != nullptr ? jsNewStringView(ctx, prHtmlDocumentReferrer(document)) : JS_EXCEPTION;
}

// sendBeacon(url, data) (beacon/#dom-navigator-sendbeacon).
JSValue navigatorSendBeacon(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    StringArgument url = {ctx};
    if (window == nullptr || !jsArgCount(ctx, argc, 1) || !url.read(argv[0]))
        return JS_EXCEPTION;
    PrByteView body{nullptr, 0};
    PrStringView type{nullptr, 0};
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !prJsFetchBodyExtract(ctx, argv[1], &body, &type))
        return JS_EXCEPTION;
    bool queued = false;
    if (prHtmlWindowSendBeacon(window, url.value(), body, type, &queued) == PrStatus_InvalidData)
        return JS_ThrowTypeError(ctx, "A beacon's URL must be HTTP or HTTPS");
    return JS_NewBool(ctx, queued);
}

// The API base URL of the realm (html/#api-base-url).
PrString realmBaseUrl(JSContext* ctx) {
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    if (window == nullptr)
        return prStringNull();
    return prDomNodeBaseUri(prDomDocumentAsNode(prHtmlWindowDocument(window)));
}

// URL.createObjectURL (FileAPI/#dfn-createObjectURL).
JSValue createObjectUrl(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrHtmlWindow* window = jsHtmlGlobalWindow(ctx);
    // A MediaSource's URL names it for a media element to attach (FileAPI/#dfn-createObjectURL).
    if (PrHtmlMediaSource* source = jsHtmlMediaSourceOf(ctx, argv[0])) {
        if (window == nullptr)
            return JS_ThrowTypeError(ctx, "createObjectURL needs a window");
        PrString url = prHtmlMediaSourceUrlCreate(window, source);
        JSValue result = jsNewStringView(ctx, prStringView(&url));
        prStringReset(&url);
        return result;
    }
    const PrFetchBlob* blob = prJsFetchBlobFromValue(ctx, argv[0]);
    if (blob == nullptr)
        return JS_ThrowTypeError(ctx, "The argument is not a Blob or a MediaSource");
    if (window == nullptr)
        return JS_ThrowTypeError(ctx, "createObjectURL needs a window");
    PrString url = prHtmlWindowCreateObjectUrl(window, blob);
    JSValue result = jsNewStringView(ctx, prStringView(&url));
    prStringReset(&url);
    return result;
}

// URL.revokeObjectURL (FileAPI/#dfn-revokeObjectURL).
JSValue revokeObjectUrl(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrStringView url;
    if (!jsArgCount(ctx, argc, 1) || !jsReadString(ctx, argv[0], &url))
        return JS_EXCEPTION;
    if (PrHtmlWindow* window = jsHtmlGlobalWindow(ctx))
        prHtmlWindowRevokeObjectUrl(window, url);
    return JS_UNDEFINED;
}

constexpr JSCFunctionListEntry kUrlStatics[] = {
    PR_JS_METHOD("createObjectURL", 1, createObjectUrl),
    PR_JS_METHOD("revokeObjectURL", 1, revokeObjectUrl),
};

void bindObjectUrls(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue url = JS_GetPropertyStr(ctx, global, "URL");
    if (JS_IsObject(url))
        JS_SetPropertyFunctionList(ctx, url, kUrlStatics, int(countOf(kUrlStatics)));
    JS_FreeValue(ctx, url);
    JS_FreeValue(ctx, global);
}

JSValue currentScript(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlDocumentCurrentScript(document), false);
}

// The attributes, selected by the magic, which is the PrUrlComponent.
JSValue locationGet(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlLocationClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prHtmlWindowLocation(window, PrUrlComponent(magic)));
}

JSValue locationSet(JSContext* ctx, JSValueConst windowObject, JSValueConst input, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlLocationClass);
    PrHtmlWindow* source = globalWindow(jsIncumbentRealm(ctx));
    StringArgument value = {ctx};
    if (window == nullptr || source == nullptr || !value.read(input))
        return JS_EXCEPTION;
    return locationStatus(ctx, prHtmlWindowSetLocation(window, source, PrUrlComponent(magic), value.value(),
        PrHtmlHistoryHandling_Push));
}

// assign and replace, which navigate in the same way as setting href, and reload, which loads the document again.
// Selected by the magic.
JSValue locationNavigate(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlLocationClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    // html/#dom-location-reload reloads the document, as a traversal by 0 does.
    if (magic == 2) {
        prHtmlHistoryGo(window, 0);
        return JS_UNDEFINED;
    }
    PrHtmlWindow* source = globalWindow(jsIncumbentRealm(ctx));
    StringArgument value = {ctx};
    if (source == nullptr || !jsArgCount(ctx, argc, 1) || !value.read(argv[0]))
        return JS_EXCEPTION;
    PrHtmlHistoryHandling handling = magic == 1 ? PrHtmlHistoryHandling_Replace : PrHtmlHistoryHandling_Push;
    return locationStatus(ctx, prHtmlWindowSetLocation(window, source, PrUrlComponent_Href, value.value(), handling));
}

JSValue locationToString(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return locationGet(ctx, object, PrUrlComponent_Href);
}

// ---------------------------------------------------------------------------------------------------------------------
// History (html/#the-history-interface), whose handle is its window

// The slots of a History object: its window's history state, deserialized in this realm, and the generation of the
// state that it holds (prHtmlHistoryState).
enum {
    HistorySlot_State,
    HistorySlot_Generation,
};
typedef uint8_t HistorySlot;

// Throws the SecurityError of a History member whose window's document is not fully active, or of a URL that the
// document cannot have.
JSValue historyStatus(JSContext* ctx, PrStatus status) {
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "SecurityError");
}

JSValue historyOf(JSContext* ctx, PrHtmlWindow* window) {
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, "history", [&] {
        return jsWrap(ctx, htmlHistoryClass, object, window, false);
    });
    JS_FreeValue(ctx, object);
    return result;
}

JSValue windowHistory(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    return window != nullptr ? historyOf(ctx, window) : JS_EXCEPTION;
}

// html/#dom-history-state: the window's history state, which the History object deserializes once for each time it is
// set, so that each read returns the same value until then.
JSValue historyStateOf(JSContext* ctx, JSValueConst history, PrHtmlWindow* window) {
    PrStringView bytes{};
    uint32_t generation = 0;
    if (prHtmlHistoryState(window, &bytes, &generation) != PrStatus_Ok)
        return historyStatus(ctx, PrStatus_NotAllowed);
    JSValue held = jsReference(ctx, history, HistorySlot_Generation);
    uint32_t heldGeneration = 0;
    bool current = JS_IsNumber(held) && JS_ToUint32(ctx, &heldGeneration, held) == 0 && heldGeneration == generation;
    JS_FreeValue(ctx, held);
    if (current)
        return jsReference(ctx, history, HistorySlot_State);
    JSValue state;
    if (bytes.data == nullptr)
        state = JS_NULL;
    else
        state = jsHtmlDeserialize(ctx, bytes, JS_UNDEFINED);
    if (JS_IsException(state))
        return state;
    jsSetReference(ctx, history, HistorySlot_State, state);
    jsSetReference(ctx, history, HistorySlot_Generation, JS_NewUint32(ctx, generation));
    return state;
}

JSValue historyLength(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    uint32_t length = 0;
    if (window == nullptr)
        return JS_EXCEPTION;
    PrStatus status = prHtmlHistoryLength(window, &length);
    return status == PrStatus_Ok ? JS_NewUint32(ctx, length) : historyStatus(ctx, status);
}

JSValue historyState(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    return window != nullptr ? historyStateOf(ctx, windowObject, window) : JS_EXCEPTION;
}

JSValue historyScrollRestoration(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    PrHtmlScrollRestoration mode = PrHtmlScrollRestoration_Auto;
    if (window == nullptr)
        return JS_EXCEPTION;
    PrStatus status = prHtmlHistoryScrollRestoration(window, &mode);
    if (status != PrStatus_Ok)
        return historyStatus(ctx, status);
    return jsFixedString(ctx, mode == PrHtmlScrollRestoration_Manual ? "manual" : "auto");
}

// A value that is not one of ScrollRestoration's is ignored (webidl/#es-attributes).
JSValue setHistoryScrollRestoration(JSContext* ctx, JSValueConst windowObject, JSValueConst input) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    StringArgument value = {ctx};
    if (window == nullptr || !value.read(input))
        return JS_EXCEPTION;
    if (value.value() != "auto" && value.value() != "manual")
        return JS_UNDEFINED;
    PrHtmlScrollRestoration mode =
        value.value() == "manual" ? PrHtmlScrollRestoration_Manual : PrHtmlScrollRestoration_Auto;
    return historyStatus(ctx, prHtmlHistorySetScrollRestoration(window, mode));
}

// go(delta), back() and forward(), whose magic is 0, -1 and 1. go's delta is a long.
JSValue historyGo(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    int32_t delta = magic;
    if (window == nullptr || (magic == 0 && jsArgGiven(argc, argv, 0) && JS_ToInt32(ctx, &delta, argv[0]) < 0))
        return JS_EXCEPTION;
    return historyStatus(ctx, prHtmlHistoryGo(window, delta));
}

// pushState(data, unused, url) and replaceState(data, unused, url), whose magic is the PrHtmlHistoryHandling.
JSValue historyPushState(JSContext* ctx, JSValueConst windowObject, int argc, JSValueConst* argv, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlHistoryClass);
    StringArgument unused = {ctx};
    StringArgument url = {ctx, PrStringView{}};
    if (window == nullptr || !jsArgCount(ctx, argc, 2) || !unused.read(argv[1]) ||
            (jsArgGiven(argc, argv, 2) && !url.readNullable(argv[2])))
        return JS_EXCEPTION;
    JsHtmlSerialized state{};
    if (!JS_IsNull(argv[0]) && !jsHtmlSerialize(ctx, argv[0], nullptr, nullptr, &state))
        return JS_EXCEPTION;
    PrStatus status = prHtmlHistoryPushState(window, JS_IsNull(argv[0]) ? PrStringView{} : prStringView(&state.bytes),
        url.value(), PrHtmlHistoryHandling(magic));
    jsHtmlSerializedReset(&state);
    return historyStatus(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// PopStateEvent (html/#popstateevent) and HashChangeEvent (html/#hashchangeevent)

enum {
    PopStateSlot_State,
};
typedef uint8_t PopStateSlot;

// PopStateEventInit, after EventInit: hasUAVisualTransition and state.
JSValue popStateConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    bool transition = false;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    JSValue state = JS_NULL;
    if (present) {
        if (!prJsDomEventInitRead(ctx, dictionary, &init) ||
                !jsDictBool(ctx, dictionary, "hasUAVisualTransition", &transition))
            return JS_EXCEPTION;
        state = JS_GetPropertyStr(ctx, dictionary, "state");
        if (JS_IsException(state))
            return state;
        if (JS_IsUndefined(state))
            state = JS_NULL;
    }
    PrHtmlPopStateEvent* event = prHtmlPopStateEventCreate(type.value(), &init, transition);
    JSValue object = jsWrap(ctx, htmlPopStateEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object))
        prDomEventRelease(prHtmlPopStateEventAsEvent(event));
    else
        jsSetReference(ctx, object, PopStateSlot_State, state);
    JS_FreeValue(ctx, state);
    return jsConstructed(ctx, newTarget, object);
}

// state: the value that the event was created with, or, for a popstate that the engine fired, the history state of the
// realm's window when it is first read.
JSValue popStateState(JSContext* ctx, JSValueConst eventObject) {
    PrHtmlPopStateEvent* event = jsThis<PrHtmlPopStateEvent>(ctx, eventObject, htmlPopStateEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    JSValue state = jsReference(ctx, eventObject, PopStateSlot_State);
    if (!JS_IsUndefined(state) || !prHtmlPopStateEventCarriesHistoryState(event))
        return JS_IsUndefined(state) ? JS_NULL : state;
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr)
        return JS_NULL;
    JSValue history = historyOf(ctx, window);
    state = historyStateOf(ctx, history, window);
    JS_FreeValue(ctx, history);
    if (JS_IsException(state)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        state = JS_NULL;
    }
    jsSetReference(ctx, eventObject, PopStateSlot_State, state);
    return state;
}

// HashChangeEventInit, after EventInit: newURL and oldURL.
JSValue hashChangeConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument newUrl = {ctx, ""};
    StringArgument oldUrl = {ctx, ""};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]) || !jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    if (present && (!prJsDomEventInitRead(ctx, dictionary, &init) || !jsDictString(ctx, dictionary, "newURL", &newUrl) ||
            !jsDictString(ctx, dictionary, "oldURL", &oldUrl)))
        return JS_EXCEPTION;
    PrHtmlHashChangeEvent* event = prHtmlHashChangeEventCreate(type.value(), &init, oldUrl.value(), newUrl.value());
    JSValue object = jsWrap(ctx, htmlHashChangeEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object))
        prDomEventRelease(prHtmlHashChangeEventAsEvent(event));
    return jsConstructed(ctx, newTarget, object);
}

// ---------------------------------------------------------------------------------------------------------------------
// Crypto (webcrypto-2/#crypto-interface)

// The most bytes that one call to getRandomValues fills (webcrypto-2/#Crypto-method-getRandomValues).
constexpr size_t kCryptoMaxRandomBytes = 65536;

// [SameObject] crypto (webcrypto-2/#dom-windoworworkerglobalscope-crypto): a view of the window.
JSValue crypto(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, "crypto", [&] {
        return jsWrap(ctx, htmlCryptoClass, object, window, false);
    });
    JS_FreeValue(ctx, object);
    return result;
}

// getRandomValues(array) fills an integer typed array with random bytes and returns it. Any other array throws a
// TypeMismatchError, and an array of more than 65536 bytes throws a QuotaExceededError.
JSValue cryptoGetRandomValues(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    if (jsThis<PrHtmlWindow>(ctx, object, htmlCryptoClass) == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    int type = JS_GetTypedArrayType(argv[0]);
    if (type < 0)
        return JS_ThrowTypeError(ctx, "getRandomValues takes an integer typed array");
    if (type == JS_TYPED_ARRAY_FLOAT16 || type == JS_TYPED_ARRAY_FLOAT32 || type == JS_TYPED_ARRAY_FLOAT64)
        return jsDomExceptionThrow(ctx, "TypeMismatchError");
    size_t offset = 0;
    size_t length = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &length, nullptr);
    if (JS_IsException(buffer))
        return buffer;
    size_t size = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &size, buffer);
    JS_FreeValue(ctx, buffer);
    if (length > kCryptoMaxRandomBytes)
        return jsDomExceptionThrow(ctx, "QuotaExceededError");
    if (bytes != nullptr)
        prRandomBytes(bytes + offset, length);
    return JS_DupValue(ctx, argv[0]);
}

// randomUUID() returns a version 4 UUID (rfc 9562, section 5.4) in lowercase hexadecimal, as
// webcrypto-2/#Crypto-method-randomUUID serializes it.
JSValue cryptoRandomUuid(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (jsThis<PrHtmlWindow>(ctx, object, htmlCryptoClass) == nullptr)
        return JS_EXCEPTION;
    uint8_t bytes[16];
    prRandomBytes(bytes, sizeof(bytes));
    bytes[6] = uint8_t((bytes[6] & 0x0F) | 0x40);
    bytes[8] = uint8_t((bytes[8] & 0x3F) | 0x80);
    constexpr char kHex[] = "0123456789abcdef";
    char text[36];
    size_t at = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text[at++] = '-';
        text[at++] = kHex[bytes[i] >> 4];
        text[at++] = kHex[bytes[i] & 0x0F];
    }
    return JS_NewStringLen(ctx, text, sizeof(text));
}

// ---------------------------------------------------------------------------------------------------------------------
// Navigator (html/#the-navigator-object), PreferenceManager and PreferenceObject (mediaqueries-5/#auto-pref)

// The language of the user (html/#dom-navigator-language), which is also the whole of navigator.languages.
constexpr const char* kNavigatorLanguage = "en-US";

// The string attributes of Navigator, selected by the magic. HTML fixes appCodeName, appName, product and vendorSub for
// every user agent, and the window gives the others.
enum {
    NavigatorString_AppCodeName,    // html/#dom-navigator-appcodename
    NavigatorString_AppName,        // html/#dom-navigator-appname
    NavigatorString_AppVersion,
    NavigatorString_Platform,
    NavigatorString_Product,        // html/#dom-navigator-product
    NavigatorString_ProductSub,
    NavigatorString_UserAgent,
    NavigatorString_Vendor,
    NavigatorString_VendorSub,      // html/#dom-navigator-vendorsub
    NavigatorString_Language,
};
typedef int NavigatorString;

JSValue navigatorString(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlNavigatorClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    switch (NavigatorString(magic)) {
        case NavigatorString_AppCodeName: return jsFixedString(ctx, "Mozilla");
        case NavigatorString_AppName: return jsFixedString(ctx, "Netscape");
        case NavigatorString_AppVersion: return jsTakeString(ctx, prHtmlNavigatorAppVersion(window));
        case NavigatorString_Platform: return jsFixedString(ctx, prHtmlNavigatorPlatform(window));
        case NavigatorString_Product: return jsFixedString(ctx, "Gecko");
        case NavigatorString_ProductSub: return jsFixedString(ctx, prHtmlNavigatorProductSub(window));
        case NavigatorString_UserAgent: return jsNewStringView(ctx, prHtmlNavigatorUserAgent(window));
        case NavigatorString_Vendor: return jsFixedString(ctx, prHtmlNavigatorVendor(window));
        case NavigatorString_Language: return jsFixedString(ctx, kNavigatorLanguage);
        default: return jsFixedString(ctx, "");
    }
}

// languages (html/#dom-navigator-languages): the same frozen array each time, until the languages change.
JSValue navigatorLanguages(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrHtmlWindow>(ctx, object, htmlNavigatorClass) == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, object, "languages", [&] {
        JSValue array = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, array, 0, jsFixedString(ctx, kNavigatorLanguage));
        JS_FreezeObject(ctx, array);
        return array;
    });
}

// The boolean attributes, selected by the magic: onLine (html/#dom-navigator-online), cookieEnabled
// (html/#dom-navigator-cookieenabled), pdfViewerEnabled (html/#dom-navigator-pdfviewerenabled) and webdriver
// (webdriver2/#dom-navigatorautomationinformation-webdriver). The window is online and keeps cookies, and has neither a
// PDF viewer nor automation.
enum {
    NavigatorFlag_OnLine,
    NavigatorFlag_CookieEnabled,
    NavigatorFlag_PdfViewerEnabled,
    NavigatorFlag_Webdriver,
};
typedef int NavigatorFlag;

JSValue navigatorFlag(JSContext* ctx, JSValueConst object, int magic) {
    if (jsThis<PrHtmlWindow>(ctx, object, htmlNavigatorClass) == nullptr)
        return JS_EXCEPTION;
    NavigatorFlag flag = NavigatorFlag(magic);
    return JS_NewBool(ctx, flag == NavigatorFlag_OnLine || flag == NavigatorFlag_CookieEnabled);
}

// userActivation (html/#dom-navigator-useractivation): a view of the window, held as the [SameObject] member of its
// Navigator.
JSValue navigatorUserActivation(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlNavigatorClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, windowObject, "userActivation", [&] {
        return jsWrap(ctx, htmlUserActivationClass, windowObject, window, false);
    });
}

JSValue userActivationHasBeenActive(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlUserActivationClass);
    return window != nullptr ? JS_NewBool(ctx, prHtmlWindowHasStickyActivation(window)) : JS_EXCEPTION;
}

JSValue userActivationIsActive(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlUserActivationClass);
    return window != nullptr ? JS_NewBool(ctx, prHtmlWindowHasTransientActivation(window)) : JS_EXCEPTION;
}

// javaEnabled (html/#dom-navigator-javaenabled): false, as HTML requires.
JSValue navigatorJavaEnabled(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return jsThis<PrHtmlWindow>(ctx, object, htmlNavigatorClass) != nullptr ? JS_FALSE : JS_EXCEPTION;
}

// hardwareConcurrency (html/#dom-navigator-hardwareconcurrency): the logical processors that the system has.
JSValue navigatorHardwareConcurrency(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrHtmlWindow>(ctx, object, htmlNavigatorClass) == nullptr)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, prThreadHardwareCount());
}

// maxTouchPoints (pointerevents4/#dom-navigator-maxtouchpoints): zero, since the window takes no touches.
JSValue navigatorMaxTouchPoints(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlWindow>(ctx, object, htmlNavigatorClass) != nullptr ? JS_NewInt32(ctx, 0) : JS_EXCEPTION;
}

// [SameObject] preferences: a view of the navigator's window.
JSValue preferences(JSContext* ctx, JSValueConst windowObject) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlNavigatorClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, windowObject, "preferences", [&] {
        return jsWrap(ctx, htmlPreferenceManagerClass, windowObject, window, false);
    });
}

// colorScheme and the manager's other preferences, selected by the magic, which is the PrHtmlPreference. The manager's
// object holds each preference's object, in the slot that the magic selects, because the window fires change at a
// preference and the preference's object owns its listeners.
JSValue preference(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlPreferenceManagerClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    JSValue held = jsReference(ctx, windowObject, uint32_t(magic));
    if (!JS_IsUndefined(held))
        return held;
    PrHtmlPreferenceObject* object = prHtmlWindowPreference(window, PrHtmlPreference(magic));
    JSValue made = jsWrap(ctx, htmlPreferenceObjectClass, windowObject, object, false);
    if (!JS_IsException(made))
        jsSetReference(ctx, windowObject, uint32_t(magic), made);
    return made;
}

JSValue preferenceOverride(JSContext* ctx, JSValueConst jsObject) {
    PrHtmlPreferenceObject* object = jsThis<PrHtmlPreferenceObject>(ctx, jsObject, htmlPreferenceObjectClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    PrHtmlPreferenceValue value = prHtmlPreferenceObjectOverride(object);
    return value != PrHtmlPreferenceValue_None ? jsFixedString(ctx, prHtmlPreferenceValueName(value)) : JS_NULL;
}

JSValue preferenceValue(JSContext* ctx, JSValueConst jsObject) {
    PrHtmlPreferenceObject* object = jsThis<PrHtmlPreferenceObject>(ctx, jsObject, htmlPreferenceObjectClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prHtmlPreferenceValueName(prHtmlPreferenceObjectValue(object)));
}

JSValue validValues(JSContext* ctx, JSValueConst jsObject) {
    PrHtmlPreferenceObject* object = jsThis<PrHtmlPreferenceObject>(ctx, jsObject, htmlPreferenceObjectClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    PrHtmlPreferenceValue items[PrHtmlPreferenceValue_Count];
    uint32_t count = prHtmlPreferenceObjectValidValues(object, items, uint32_t(countOf(items)));
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count && i < countOf(items); ++i)
        JS_SetPropertyUint32(ctx, array, i, jsFixedString(ctx, prHtmlPreferenceValueName(items[i])));
    // A FrozenArray (webidl/#js-frozen-array).
    JS_FreezeObject(ctx, array);
    return array;
}

JSValue clearOverride(JSContext* ctx, JSValueConst jsObject, int, JSValueConst*) {
    PrHtmlPreferenceObject* object = jsThis<PrHtmlPreferenceObject>(ctx, jsObject, htmlPreferenceObjectClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    prHtmlPreferenceObjectClearOverride(object);
    return JS_UNDEFINED;
}

// requestOverride. Its promise rejects with a NotSupportedError for a value that the preference does not accept.
JSValue requestOverride(JSContext* ctx, JSValueConst jsObject, int argc, JSValueConst* argv) {
    PrHtmlPreferenceObject* object = jsThis<PrHtmlPreferenceObject>(ctx, jsObject, htmlPreferenceObjectClass);
    StringArgument value = {ctx, PrStringView{}};
    if (object == nullptr || !jsArgCount(ctx, argc, 1) || !value.readNullable(argv[0]))
        return JS_EXCEPTION;
    // A value that is not a preference keyword is not valid, and the empty string clears the override.
    PrHtmlPreferenceValue named = prHtmlPreferenceValueFromName(value.value());
    PrStatus status;
    if (named == PrHtmlPreferenceValue_None && value.value().size != 0)
        status = PrStatus_InvalidData;
    else
        status = prHtmlPreferenceObjectRequestOverride(object, named);
    JSValue settled = status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionNew(ctx, "NotSupportedError");
    JSValue promise = jsSettledPromise(ctx, settled, status != PrStatus_Ok);
    JS_FreeValue(ctx, settled);
    return promise;
}

// Returns true if `text` is an array index in canonical form, and writes it to `out`.
bool arrayIndexOf(PrStringView text, uint32_t* out) {
    uint64_t value = 0;
    if ((text.size > 1 && text.data[0] == '0') || !prU64Parse(text.data, text.size, &value, 10) || value >= 0xffffffffu)
        return false;
    *out = uint32_t(value);
    return true;
}

// The Window's named properties object (html/#named-access-on-the-window-object). Its own properties are the elements
// that the global's window names.
int namedProperty(JSContext* ctx, JSPropertyDescriptor* PR_NULLABLE desc, JSValueConst, JSAtom atom) {
    PrHtmlWindow* window = globalWindow(ctx);
    JSValue key = JS_AtomToValue(ctx, atom);
    if (window == nullptr || JS_IsSymbol(key)) {
        JS_FreeValue(ctx, key);
        return 0;
    }
    StringArgument name = {ctx};
    bool read = name.read(key);
    JS_FreeValue(ctx, key);
    if (!read)
        return -1;
    // html/#accessing-other-browsing-contexts: an array index names a frame, and nothing else.
    uint32_t index = 0;
    PrHtmlWindow* frame = nullptr;
    if (arrayIndexOf(name.value(), &index)) {
        frame = prHtmlWindowFrame(window, index);
        if (frame == nullptr)
            return 0;
    } else {
        frame = prHtmlWindowFrameFromName(window, name.value());
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue value;
    if (frame != nullptr)
        value = globalOf(ctx, frame);
    else
        value = jsWrapAs(ctx, prJsDomElementClass, global, prHtmlWindowNamedItem(window, name.value()), false);
    JS_FreeValue(ctx, global);
    if (JS_IsException(value))
        return -1;
    if (JS_IsNull(value))
        return 0;

    // webidl/#named-properties-object-getownproperty: writable and configurable, and not enumerable under
    // [LegacyUnenumerableNamedProperties].
    if (desc != nullptr) {
        desc->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        desc->value = value;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    } else {
        JS_FreeValue(ctx, value);
    }
    return 1;
}

JSClassExoticMethods kNamedPropertiesExotic = {.get_own_property = JsCallScoped<namedProperty>::call};

// Puts the named properties object between Window.prototype and EventTarget.prototype.
void defineNamedProperties(JSContext* ctx) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    JSClassDef definition = {"WindowProperties",      nullptr, nullptr, nullptr, &kNamedPropertiesExotic, nullptr};
    jsRegisterClass(runtime, &htmlWindowPropertiesClass, definition);
    JSValue proto = JS_GetClassProto(ctx, htmlWindowClass);
    JSValue properties = JS_NewObjectClass(ctx, int(htmlWindowPropertiesClass));
    JSValue above = JS_GetPrototype(ctx, proto);
    JS_SetPrototype(ctx, properties, above);
    JS_SetPrototype(ctx, proto, properties);
    JS_FreeValue(ctx, above);
    JS_FreeValue(ctx, properties);
    JS_FreeValue(ctx, proto);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLScriptElement (html/#htmlscriptelement)

JSValue setScriptAsync(JSContext* ctx, JSValueConst scriptObject, JSValueConst input) {
    PrHtmlScriptElement* script = jsThis<PrHtmlScriptElement>(ctx, scriptObject, htmlScriptElementClass);
    if (script == nullptr)
        return JS_EXCEPTION;
    prHtmlScriptElementSetAsync(script, JS_ToBool(ctx, input) != 0);
    return JS_UNDEFINED;
}

JSValue scriptSupports(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prHtmlScriptElementSupports(type.value()));
}

// ---------------------------------------------------------------------------------------------------------------------
// Storage (html/#storage-2), whose handle is the window's PrHtmlStorage

PrHtmlStorage* PR_NULLABLE storageOf(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlStorage>(ctx, object, htmlStorageClass);
}

// html/#dom-localstorage and html/#dom-sessionstorage, selected by the magic, which is the PrHtmlStorageKind.
JSValue windowStorage(JSContext* ctx, JSValueConst windowObject, int magic) {
    PrHtmlWindow* window = jsThis<PrHtmlWindow>(ctx, windowObject, htmlWindowClass);
    if (window == nullptr)
        return JS_EXCEPTION;
    PrHtmlStorage* storage = prHtmlWindowStorage(window, PrHtmlStorageKind(magic));
    if (storage == nullptr)
        return jsDomExceptionThrow(ctx, "SecurityError");
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue result = jsSameObject(ctx, object, magic == PrHtmlStorageKind_Local ? "localStorage" : "sessionStorage",
        [&] { return jsWrap(ctx, htmlStorageClass, object, storage, false); });
    JS_FreeValue(ctx, object);
    return result;
}

JSValue storageLength(JSContext* ctx, JSValueConst storageObject) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    return storage != nullptr ? JS_NewUint32(ctx, prHtmlStorageLength(storage)) : JS_EXCEPTION;
}

// Returns the string, or null for the null string.
JSValue nullableString(JSContext* ctx, PrString value) {
    return prStringIsNull(&value) ? JS_NULL : jsTakeString(ctx, value);
}

JSValue storageKey(JSContext* ctx, JSValueConst storageObject, int argc, JSValueConst* argv) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    uint32_t index = 0;
    if (storage == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return nullableString(ctx, prHtmlStorageKey(storage, index));
}

JSValue storageGetItem(JSContext* ctx, JSValueConst storageObject, int argc, JSValueConst* argv) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    StringArgument key = {ctx};
    if (storage == nullptr || !jsArgCount(ctx, argc, 1) || !key.read(argv[0]))
        return JS_EXCEPTION;
    return nullableString(ctx, prHtmlStorageGetItem(storage, key.value()));
}

// Sets the item, or throws a QuotaExceededError whose quota and requested amount are null.
bool setStorageItem(JSContext* ctx, PrHtmlStorage* storage, PrStringView key, PrStringView value) {
    if (prHtmlStorageSetItem(storage, key, value) == PrStatus_Ok)
        return true;
    jsQuotaExceededErrorThrow(ctx, kPrF64Nan, kPrF64Nan);
    return false;
}

JSValue storageSetItem(JSContext* ctx, JSValueConst storageObject, int argc, JSValueConst* argv) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    StringArgument key = {ctx};
    StringArgument value = {ctx};
    if (storage == nullptr || !jsArgCount(ctx, argc, 2) || !key.read(argv[0]) || !value.read(argv[1]))
        return JS_EXCEPTION;
    return setStorageItem(ctx, storage, key.value(), value.value()) ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue storageRemoveItem(JSContext* ctx, JSValueConst storageObject, int argc, JSValueConst* argv) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    StringArgument key = {ctx};
    if (storage == nullptr || !jsArgCount(ctx, argc, 1) || !key.read(argv[0]))
        return JS_EXCEPTION;
    prHtmlStorageRemoveItem(storage, key.value());
    return JS_UNDEFINED;
}

JSValue storageClear(JSContext* ctx, JSValueConst storageObject, int, JSValueConst*) {
    PrHtmlStorage* storage = storageOf(ctx, storageObject);
    if (storage == nullptr)
        return JS_EXCEPTION;
    prHtmlStorageClear(storage);
    return JS_UNDEFINED;
}

// The named properties: the keys, whose values are the items (getItem, setItem and removeItem).
bool storageNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    const PrHtmlStorage* storage = static_cast<const PrHtmlStorage*>(handle);
    uint32_t length = prHtmlStorageLength(storage);
    for (uint32_t i = 0; i < length; ++i) {
        PrString key = prHtmlStorageKey(storage, i);
        if (prStringIsNull(&key))
            break;
        PrStringView view = prStringView(&key);
        JSAtom atom = JS_NewAtomLen(ctx, view.data, view.size);
        prStringReset(&key);
        if (atom == JS_ATOM_NULL)
            return false;
        out->push(atom);
    }
    return true;
}

JSValue storageNamedItem(JSContext* ctx, JSValueConst, void* handle, PrStringView name) {
    PrString value = prHtmlStorageGetItem(static_cast<const PrHtmlStorage*>(handle), name);
    return prStringIsNull(&value) ? JS_UNDEFINED : jsTakeString(ctx, value);
}

bool storageNamedSetItem(JSContext* ctx, JSValueConst, void* handle, PrStringView name, JSValueConst input) {
    StringArgument value = {ctx};
    return value.read(input) && setStorageItem(ctx, static_cast<PrHtmlStorage*>(handle), name, value.value());
}

void storageNamedDeleteItem(void* handle, PrStringView name) {
    prHtmlStorageRemoveItem(static_cast<PrHtmlStorage*>(handle), name);
}

constexpr JsNamed kStorageNamed = {storageNames, storageNamedItem, storageNamedSetItem, storageNamedDeleteItem, 0};

// ---------------------------------------------------------------------------------------------------------------------
// Classes

constexpr JSCFunctionListEntry kDomParserProto[] = {
    PR_JS_TAG("DOMParser"),
    PR_JS_METHOD("parseFromString", 2, parseFromString),
};

// The members that HTML adds to Element, in the DOM's class.
constexpr JSCFunctionListEntry kDomElementProto[] = {
    PR_JS_MAGIC_GET_SET("innerHTML", markup, setMarkup, 0),
    PR_JS_MAGIC_GET_SET("outerHTML", markup, setMarkup, 1),
    PR_JS_METHOD("insertAdjacentHTML", 2, insertAdjacentHtml),
};

// The members that HTML adds to Document, in the DOM's class.
constexpr JSCFunctionListEntry kDocumentProto[] = {
    PR_JS_MAGIC_GET_SET("fgColor", bodyColor, setBodyColor, 0),
    PR_JS_MAGIC_GET_SET("bgColor", bodyColor, setBodyColor, 1),
    PR_JS_MAGIC_GET_SET("linkColor", bodyColor, setBodyColor, 2),
    PR_JS_MAGIC_GET_SET("vlinkColor", bodyColor, setBodyColor, 3),
    PR_JS_MAGIC_GET_SET("alinkColor", bodyColor, setBodyColor, 4),
    PR_JS_GET("location", documentLocation),
    PR_JS_GET("defaultView", defaultView),
    PR_JS_GET("readyState", readyState),
    PR_JS_GET_SET("cookie", documentCookie, setDocumentCookie),
    PR_JS_GET("referrer", documentReferrer),
    PR_JS_METHOD("getSelection", 0, documentGetSelection),
    PR_JS_GET("currentScript", currentScript),
    PR_JS_GET("activeElement", activeElement),
    PR_JS_METHOD("hasFocus", 0, hasFocus),
    PR_JS_MAGIC_GET("visibilityState", documentVisibility, 0),
    PR_JS_MAGIC_GET("hidden", documentVisibility, 1),
    PR_JS_DOM_EVENT_HANDLER(onreadystatechange),
    PR_JS_DOM_EVENT_HANDLER(onvisibilitychange),
    PR_JS_MAGIC_GET("images", documentCollection, PrHtmlDocumentCollection_Images),
    PR_JS_MAGIC_GET("embeds", documentCollection, PrHtmlDocumentCollection_Embeds),
    PR_JS_MAGIC_GET("plugins", documentCollection, PrHtmlDocumentCollection_Embeds),
    PR_JS_MAGIC_GET("links", documentCollection, PrHtmlDocumentCollection_Links),
    PR_JS_MAGIC_GET("forms", documentCollection, PrHtmlDocumentCollection_Forms),
    PR_JS_MAGIC_GET("scripts", documentCollection, PrHtmlDocumentCollection_Scripts),
    PR_JS_MAGIC_GET("anchors", documentCollection, PrHtmlDocumentCollection_Anchors),
    PR_JS_GET("timeline", documentTimeline),
    PR_JS_METHOD("open", 0, documentOpen),
    PR_JS_MAGIC("write", 1, documentWrite, 0),
    PR_JS_MAGIC("writeln", 1, documentWrite, 1),
    PR_JS_METHOD("close", 0, documentClose),
};

constexpr JSCFunctionListEntry kDomStringMapProto[] = {PR_JS_TAG("DOMStringMap")};

constexpr JSCFunctionListEntry kStorageProto[] = {
    PR_JS_TAG("Storage"),
    PR_JS_GET("length", storageLength),
    PR_JS_METHOD("key", 1, storageKey),
    PR_JS_METHOD("getItem", 1, storageGetItem),
    PR_JS_METHOD("setItem", 2, storageSetItem),
    PR_JS_METHOD("removeItem", 1, storageRemoveItem),
    PR_JS_METHOD("clear", 0, storageClear),
};

// The members that HTML adds to ShadowRoot, in the DOM's class.
constexpr JSCFunctionListEntry kShadowRootProto[] = {
    PR_JS_GET_SET("innerHTML", shadowMarkup, setShadowMarkup),
    PR_JS_GET("activeElement", activeElement),
};

// The members that HTML gives HTML, SVG and MathML elements alike: HTMLOrSVGElement's members and GlobalEventHandlers.
#define PR_HTML_OR_SVG_ELEMENT                                                                     \
    PR_JS_GET("dataset", dataset),                                                                 \
        PR_JS_GET_SET("tabIndex", (jsNumberGetter<&prJsDomElementClass, prHtmlTabIndex>),              \
                      setTabIndex),                                                                \
        PR_REFLECT_BOOL("autofocus", "autofocus"),                                         \
        PR_JS_METHOD("focus", 0, elementFocus), PR_JS_METHOD("blur", 0, elementBlur),              \
        PR_JS_DOM_GLOBAL_EVENT_HANDLER_MEMBERS

constexpr JSCFunctionListEntry kElementProto[] = {
    PR_JS_TAG("HTMLElement"),
    PR_JS_METHOD("attachInternals", 0, jsCustomElementsAttachInternals),
    PR_JS_METHOD("click", 0, elementClick),
    PR_JS_GET_SET("contentEditable", contentEditable, setContentEditable),
    PR_JS_GET("isContentEditable", isContentEditable),
    PR_JS_GET_SET("draggable", draggable, setDraggable),
    PR_REFLECT_BOOL("inert", "inert"),
    PR_REFLECT_STRING("accessKey", "accesskey"),
    PR_REFLECT_STRING("enterKeyHint", "enterkeyhint"),
    PR_REFLECT_STRING("inputMode", "inputmode"),
    // html/#dom-innertext, whose setter treats null as the empty string.
    PR_JS_GET_SET("innerText", (jsStringGetter<&htmlElementClass, prHtmlElementInnerText>),
        (jsStringSetter<&htmlElementClass, prHtmlElementSetInnerText, true>)),
    PR_JS_GET_SET("outerText", (jsStringGetter<&htmlElementClass, prHtmlElementInnerText>), setOuterText),
    PR_REFLECT_STRING("title", "title"),
    PR_REFLECT_STRING("lang", "lang"),
    PR_JS_GET_SET("dir", dir, setDir),
    PR_JS_GET_SET("hidden", hidden, setHidden),
    PR_JS_GET("offsetParent", offsetParent),
    PR_JS_MAGIC_GET("offsetTop", offsetGet, 0),
    PR_JS_MAGIC_GET("offsetLeft", offsetGet, 1),
    PR_JS_MAGIC_GET("offsetWidth", offsetGet, 2),
    PR_JS_MAGIC_GET("offsetHeight", offsetGet, 3),
    PR_JS_CSS_INLINE_STYLE,
    PR_HTML_OR_SVG_ELEMENT
};

// The members HTML adds to the classes of SVG and MathML elements.
constexpr JSCFunctionListEntry kForeignElementProto[] = {PR_HTML_OR_SVG_ELEMENT};

constexpr JSCFunctionListEntry kDialogElementProto[] = {
    PR_JS_TAG("HTMLDialogElement"),
    PR_JS_GET_SET("open", dialogOpen, setDialogOpen),
    PR_JS_GET_SET("returnValue", (jsStringGetter<&htmlDialogElementClass, prHtmlDialogElementReturnValue>),
        (jsStringSetter<&htmlDialogElementClass, prHtmlDialogElementSetReturnValue>)),
    PR_JS_MAGIC("show", 0, dialogShow, 0),
    PR_JS_MAGIC("showModal", 0, dialogShow, 1),
    PR_JS_MAGIC("close", 0, dialogClose, 0),
    PR_JS_MAGIC("requestClose", 0, dialogClose, 1),
};

constexpr JSCFunctionListEntry kIFrameElementProto[] = {
    PR_JS_TAG("HTMLIFrameElement"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("srcdoc", "srcdoc"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("width", "width"),
    PR_REFLECT_STRING("height", "height"),
    PR_JS_GET("contentWindow", contentWindow),
    PR_JS_GET("contentDocument", contentDocument),
};

constexpr JSCFunctionListEntry kImageElementProto[] = {
    PR_JS_TAG("HTMLImageElement"),
    PR_JS_MAGIC_GET_SET("crossOrigin", mediaEnumerated, setMediaEnumerated, 0),
    PR_REFLECT_STRING("alt", "alt"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("srcset", "srcset"),
    PR_REFLECT_STRING("sizes", "sizes"),
    PR_JS_GET_SET("width", (jsNumberGetter<&htmlImageElementClass, prHtmlImageElementWidth>),
        (jsUint32Setter<&htmlImageElementClass, prHtmlImageElementSetWidth>)),
    PR_JS_GET_SET("height", (jsNumberGetter<&htmlImageElementClass, prHtmlImageElementHeight>),
        (jsUint32Setter<&htmlImageElementClass, prHtmlImageElementSetHeight>)),
    PR_JS_GET("naturalWidth", (jsNumberGetter<&htmlImageElementClass, prHtmlImageElementNaturalWidth>)),
    PR_JS_GET("naturalHeight", (jsNumberGetter<&htmlImageElementClass, prHtmlImageElementNaturalHeight>)),
    PR_JS_GET("complete", (jsBoolGetter<&htmlImageElementClass, prHtmlImageElementComplete>)),
    PR_JS_GET("currentSrc", (jsStringGetter<&htmlImageElementClass, prHtmlImageElementCurrentSrc>)),
    PR_JS_METHOD("decode", 0, imageDecode),
};

constexpr JSCFunctionListEntry kCanvasElementProto[] = {
    PR_JS_TAG("HTMLCanvasElement"),
    PR_JS_MAGIC_GET_SET("width", canvasDimension, setCanvasDimension, 0),
    PR_JS_MAGIC_GET_SET("height", canvasDimension, setCanvasDimension, 1),
    PR_JS_METHOD("getContext", 1, canvasGetContext),
    PR_JS_METHOD("toDataURL", 0, canvasToDataUrl),
};

#define PR_HYPERLINK(name, component)                                                               \
    PR_JS_MAGIC_GET_SET(name, hyperlinkComponent, setHyperlinkComponent, component)

// The attributes of HTMLHyperlinkElementUtils, which an a and an area element share.
#define PR_HYPERLINK_UTILS                                                                          \
    PR_HYPERLINK("href", PrUrlComponent_Href), PR_JS_MAGIC_GET("origin", hyperlinkComponent, PrUrlComponent_Origin), \
        PR_HYPERLINK("protocol", PrUrlComponent_Protocol), PR_HYPERLINK("username", PrUrlComponent_Username),       \
        PR_HYPERLINK("password", PrUrlComponent_Password), PR_HYPERLINK("host", PrUrlComponent_Host),               \
        PR_HYPERLINK("hostname", PrUrlComponent_Hostname), PR_HYPERLINK("port", PrUrlComponent_Port),               \
        PR_HYPERLINK("pathname", PrUrlComponent_Pathname), PR_HYPERLINK("search", PrUrlComponent_Search),           \
        PR_HYPERLINK("hash", PrUrlComponent_Hash), PR_JS_METHOD("toString", 0, hyperlinkToString)

constexpr JSCFunctionListEntry kAnchorElementProto[] = {
    PR_JS_TAG("HTMLAnchorElement"),
    PR_JS_GET_SET("relList", relList, (jsPutForwards<"relList", "value">)),
    PR_REFLECT_STRING("charset", "charset"),
    PR_REFLECT_STRING("coords", "coords"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("rev", "rev"),
    PR_REFLECT_STRING("shape", "shape"),
    PR_REFLECT_STRING("target", "target"),
    PR_REFLECT_STRING("download", "download"),
    PR_REFLECT_STRING("ping", "ping"),
    PR_REFLECT_STRING("rel", "rel"),
    PR_REFLECT_STRING("hreflang", "hreflang"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("referrerPolicy", "referrerpolicy"),
    PR_JS_GET_SET("text", (jsStringGetter<&prJsDomNodeClass, prDomNodeTextContent>),
        (jsStringSetter<&prJsDomNodeClass, prDomNodeSetTextContent, true>)),
    PR_HYPERLINK_UTILS,
};

constexpr JSCFunctionListEntry kAreaElementProto[] = {
    PR_JS_TAG("HTMLAreaElement"),
    PR_JS_GET_SET("relList", relList, (jsPutForwards<"relList", "value">)),
    PR_REFLECT_BOOL("noHref", "nohref"),
    PR_REFLECT_STRING("alt", "alt"),
    PR_REFLECT_STRING("coords", "coords"),
    PR_REFLECT_STRING("shape", "shape"),
    PR_REFLECT_STRING("target", "target"),
    PR_REFLECT_STRING("download", "download"),
    PR_REFLECT_STRING("ping", "ping"),
    PR_REFLECT_STRING("rel", "rel"),
    PR_REFLECT_STRING("referrerPolicy", "referrerpolicy"),
    PR_HYPERLINK_UTILS,
};

constexpr JSCFunctionListEntry kBaseElementProto[] = {
    PR_JS_TAG("HTMLBaseElement"),
    PR_JS_GET_SET("href", baseHref, jsSetReflectString<"href">),
    PR_REFLECT_STRING("target", "target"),
};

constexpr JSCFunctionListEntry kLinkElementProto[] = {
    PR_JS_TAG("HTMLLinkElement"),
    PR_JS_MAGIC_GET_SET("crossOrigin", mediaEnumerated, setMediaEnumerated, 0),
    PR_JS_GET_SET("relList", relList, (jsPutForwards<"relList", "value">)),
    PR_REFLECT_STRING("charset", "charset"),
    PR_REFLECT_STRING("rev", "rev"),
    PR_REFLECT_STRING("target", "target"),
    PR_REFLECT_URL("href", "href"),
    PR_REFLECT_STRING("rel", "rel"),
    PR_REFLECT_STRING("media", "media"),
    PR_REFLECT_STRING("hreflang", "hreflang"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("integrity", "integrity"),
    PR_REFLECT_STRING("imageSrcset", "imagesrcset"),
    PR_REFLECT_STRING("imageSizes", "imagesizes"),
    PR_REFLECT_STRING("referrerPolicy", "referrerpolicy"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_JS_GET("sheet", linkSheet),
};

constexpr JSCFunctionListEntry kPictureElementProto[] = {PR_JS_TAG("HTMLPictureElement")};

// html/#htmlsourceelement
constexpr JSCFunctionListEntry kSourceElementProto[] = {
    PR_JS_TAG("HTMLSourceElement"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("srcset", "srcset"),
    PR_REFLECT_STRING("sizes", "sizes"),
    PR_REFLECT_STRING("media", "media"),
};

constexpr JSCFunctionListEntry kMetaElementProto[] = {
    PR_JS_TAG("HTMLMetaElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("httpEquiv", "http-equiv"),
    PR_REFLECT_STRING("content", "content"),
    PR_REFLECT_STRING("media", "media"),
    PR_REFLECT_STRING("scheme", "scheme"),
};

constexpr JSCFunctionListEntry kMediaElementProto[] = {
    PR_JS_TAG("HTMLMediaElement"),
    PR_JS_GET("error", mediaError),
    PR_REFLECT_URL("src", "src"),
    PR_JS_GET("currentSrc", mediaCurrentSrc),
    PR_JS_MAGIC_GET_SET("crossOrigin", mediaEnumerated, setMediaEnumerated, 0),
    PR_JS_MAGIC_GET("networkState", mediaState, 0),
    PR_JS_MAGIC_GET_SET("preload", mediaEnumerated, setMediaEnumerated, 1),
    PR_JS_MAGIC_GET("buffered", mediaRanges, PrHtmlMediaRanges_Buffered),
    PR_JS_METHOD("load", 0, mediaLoad),
    PR_JS_METHOD("canPlayType", 1, mediaCanPlayType),
    PR_JS_MAGIC_GET("readyState", mediaState, 1),
    PR_JS_MAGIC_GET("seeking", mediaFlag, 0),
    PR_JS_MAGIC_GET_SET("currentTime", mediaNumber, setMediaNumber, 0),
    PR_JS_METHOD("fastSeek", 1, mediaFastSeek),
    PR_JS_MAGIC_GET("duration", mediaNumber, 1),
    PR_JS_METHOD("getStartDate", 0, mediaStartDate),
    PR_JS_MAGIC_GET("paused", mediaFlag, 1),
    PR_JS_MAGIC_GET_SET("defaultPlaybackRate", mediaNumber, setMediaNumber, 2),
    PR_JS_MAGIC_GET_SET("playbackRate", mediaNumber, setMediaNumber, 3),
    PR_JS_MAGIC_GET_SET("preservesPitch", mediaFlag, setMediaFlag, 4),
    PR_JS_MAGIC_GET("played", mediaRanges, PrHtmlMediaRanges_Played),
    PR_JS_MAGIC_GET("seekable", mediaRanges, PrHtmlMediaRanges_Seekable),
    PR_JS_MAGIC_GET("ended", mediaFlag, 2),
    PR_REFLECT_BOOL("autoplay", "autoplay"),
    PR_REFLECT_BOOL("loop", "loop"),
    PR_JS_METHOD("play", 0, mediaPlay),
    PR_JS_METHOD("pause", 0, mediaPause),
    PR_REFLECT_BOOL("controls", "controls"),
    PR_JS_MAGIC_GET_SET("volume", mediaNumber, setMediaNumber, 4),
    PR_JS_MAGIC_GET_SET("muted", mediaFlag, setMediaFlag, 3),
    PR_REFLECT_BOOL("defaultMuted", "muted"),
    PR_JS_MAGIC_GET_SET("loading", mediaEnumerated, setMediaEnumerated, 2),
    PR_JS_MAGIC_GET("textTracks", mediaTracks, PrHtmlTrackKind_Text),
    PR_JS_MAGIC_GET("audioTracks", mediaTracks, PrHtmlTrackKind_Audio),
    PR_JS_MAGIC_GET("videoTracks", mediaTracks, PrHtmlTrackKind_Video),
};

// The object of a window, or of a part of another target (DomPartTarget) such as a track list or a window's
// Performance, holds a reference to the target, which for a part is a reference to its owner. So the target outlives
// the object however the collector orders their finalizers, and whatever replaces the target.
constexpr JsReferences kTargetReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    false,
};

constexpr JSCFunctionListEntry kTextTrackListProto[] = {
    PR_JS_TAG("TextTrackList"),
    PR_JS_GET("length", trackListLength),
    PR_JS_METHOD("getTrackById", 1, trackListGetTrackById),
    PR_JS_DOM_EVENT_HANDLER(onchange),
    PR_JS_DOM_EVENT_HANDLER(onaddtrack),
    PR_JS_DOM_EVENT_HANDLER(onremovetrack),
};

constexpr JSCFunctionListEntry kAudioTrackListProto[] = {
    PR_JS_TAG("AudioTrackList"),
    PR_JS_GET("length", trackListLength),
    PR_JS_METHOD("getTrackById", 1, trackListGetTrackById),
    PR_JS_DOM_EVENT_HANDLER(onchange),
    PR_JS_DOM_EVENT_HANDLER(onaddtrack),
    PR_JS_DOM_EVENT_HANDLER(onremovetrack),
};

constexpr JSCFunctionListEntry kVideoTrackListProto[] = {
    PR_JS_TAG("VideoTrackList"),
    PR_JS_GET("length", trackListLength),
    PR_JS_METHOD("getTrackById", 1, trackListGetTrackById),
    PR_JS_GET("selectedIndex", trackListSelectedIndex),
    PR_JS_DOM_EVENT_HANDLER(onchange),
    PR_JS_DOM_EVENT_HANDLER(onaddtrack),
    PR_JS_DOM_EVENT_HANDLER(onremovetrack),
};

constexpr JsConstant kMediaElementConstants[] = {
    {"NETWORK_EMPTY", PrHtmlMediaNetworkState_Empty},
    {"NETWORK_IDLE", PrHtmlMediaNetworkState_Idle},
    {"NETWORK_LOADING", PrHtmlMediaNetworkState_Loading},
    {"NETWORK_NO_SOURCE", PrHtmlMediaNetworkState_NoSource},
    {"HAVE_NOTHING", PrHtmlMediaReadyState_HaveNothing},
    {"HAVE_METADATA", PrHtmlMediaReadyState_HaveMetadata},
    {"HAVE_CURRENT_DATA", PrHtmlMediaReadyState_HaveCurrentData},
    {"HAVE_FUTURE_DATA", PrHtmlMediaReadyState_HaveFutureData},
    {"HAVE_ENOUGH_DATA", PrHtmlMediaReadyState_HaveEnoughData},
};

constexpr JSCFunctionListEntry kVideoElementProto[] = {
    PR_JS_TAG("HTMLVideoElement"),
    PR_REFLECT_INTEGER("width", "width", PrHtmlReflectedInteger_Unsigned, 0),
    PR_REFLECT_INTEGER("height", "height", PrHtmlReflectedInteger_Unsigned, 0),
    PR_JS_MAGIC_GET("videoWidth", videoSize, 0),
    PR_JS_MAGIC_GET("videoHeight", videoSize, 1),
    PR_REFLECT_URL("poster", "poster"),
    PR_REFLECT_BOOL("playsInline", "playsinline"),
};

constexpr JSCFunctionListEntry kMediaErrorProto[] = {
    PR_JS_TAG("MediaError"),
    PR_JS_GET("code", mediaErrorCode),
    PR_JS_GET("message", mediaErrorMessage),
};

constexpr JsConstant kMediaErrorConstants[] = {
    {"MEDIA_ERR_ABORTED", PrHtmlMediaError_Aborted},
    {"MEDIA_ERR_NETWORK", PrHtmlMediaError_Network},
    {"MEDIA_ERR_DECODE", PrHtmlMediaError_Decode},
    {"MEDIA_ERR_SRC_NOT_SUPPORTED", PrHtmlMediaError_SrcNotSupported},
};

constexpr JSCFunctionListEntry kTimeRangesProto[] = {
    PR_JS_TAG("TimeRanges"),
    PR_JS_GET("length", timeRangesLength),
    PR_JS_MAGIC("start", 1, timeRangesBound, 0),
    PR_JS_MAGIC("end", 1, timeRangesBound, 1),
};

constexpr JSCFunctionListEntry kAudioElementProto[] = {PR_JS_TAG("HTMLAudioElement")};

constexpr JSCFunctionListEntry kSlotElementProto[] = {
    PR_JS_TAG("HTMLSlotElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_JS_MAGIC("assignedNodes", 0, slotAssigned, 0),
    PR_JS_MAGIC("assignedElements", 0, slotAssigned, 1),
    PR_JS_METHOD("assign", 0, slotAssign),
};

constexpr JSCFunctionListEntry kScriptElementProto[] = {
    PR_JS_TAG("HTMLScriptElement"),
    PR_JS_MAGIC_GET_SET("crossOrigin", mediaEnumerated, setMediaEnumerated, 0),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_BOOL("noModule", "nomodule"),
    PR_JS_GET_SET("async", (jsBoolGetter<&htmlScriptElementClass, prHtmlScriptElementAsync>), setScriptAsync),
    PR_REFLECT_BOOL("defer", "defer"),
    PR_REFLECT_STRING("charset", "charset"),
    PR_REFLECT_STRING("integrity", "integrity"),
    PR_REFLECT_STRING("referrerPolicy", "referrerpolicy"),
    PR_REFLECT_STRING("event", "event"),
    PR_REFLECT_STRING("htmlFor", "for"),
    PR_JS_GET_SET("text", (jsStringGetter<&htmlScriptElementClass, prHtmlScriptElementText>),
        (jsStringSetter<&htmlScriptElementClass, prHtmlScriptElementSetText>)),
};

constexpr JSCFunctionListEntry kScriptElementStatics[] = {PR_JS_METHOD("supports", 1, scriptSupports)};

constexpr JSCFunctionListEntry kStyleElementProto[] = {PR_JS_TAG("HTMLStyleElement"), PR_JS_GET("sheet", sheet)};

constexpr JSCFunctionListEntry kTemplateElementProto[] = {
    PR_JS_TAG("HTMLTemplateElement"),
    PR_JS_GET("content", content),
    PR_JS_MAGIC_GET_SET("shadowRootMode", shadowRootMode, setAttributeString, 0),
    PR_REFLECT_BOOL("shadowRootDelegatesFocus", "shadowrootdelegatesfocus"),
    PR_REFLECT_BOOL("shadowRootClonable", "shadowrootclonable"),
    PR_REFLECT_BOOL("shadowRootSerializable", "shadowrootserializable"),
    PR_JS_MAGIC_GET_SET("shadowRootSlotAssignment", shadowRootSlotAssignment, setAttributeString, 1),
};

#define PR_LOCATION(name, component) PR_JS_MAGIC_GET_SET(name, locationGet, locationSet, component)

constexpr JSCFunctionListEntry kLocationProto[] = {
    PR_JS_TAG("Location"),
    PR_LOCATION("href", PrUrlComponent_Href),
    PR_JS_MAGIC_GET("origin", locationGet, PrUrlComponent_Origin),
    PR_LOCATION("protocol", PrUrlComponent_Protocol),
    PR_LOCATION("host", PrUrlComponent_Host),
    PR_LOCATION("hostname", PrUrlComponent_Hostname),
    PR_LOCATION("port", PrUrlComponent_Port),
    PR_LOCATION("pathname", PrUrlComponent_Pathname),
    PR_LOCATION("search", PrUrlComponent_Search),
    PR_LOCATION("hash", PrUrlComponent_Hash),
    PR_JS_MAGIC("assign", 1, locationNavigate, 0),
    PR_JS_MAGIC("replace", 1, locationNavigate, 1),
    PR_JS_MAGIC("reload", 0, locationNavigate, 2),
    PR_JS_METHOD("toString", 0, locationToString),
};

#undef PR_LOCATION

constexpr JSCFunctionListEntry kHistoryProto[] = {
    PR_JS_TAG("History"),
    PR_JS_GET("length", historyLength),
    PR_JS_GET_SET("scrollRestoration", historyScrollRestoration, setHistoryScrollRestoration),
    PR_JS_GET("state", historyState),
    PR_JS_MAGIC("go", 0, historyGo, 0),
    PR_JS_MAGIC("back", 0, historyGo, -1),
    PR_JS_MAGIC("forward", 0, historyGo, 1),
    PR_JS_MAGIC("pushState", 2, historyPushState, PrHtmlHistoryHandling_Push),
    PR_JS_MAGIC("replaceState", 2, historyPushState, PrHtmlHistoryHandling_Replace),
};

constexpr JSCFunctionListEntry kPopStateEventProto[] = {
    PR_JS_TAG("PopStateEvent"),
    PR_JS_GET("state", popStateState),
    PR_JS_GET("hasUAVisualTransition",
        (jsBoolGetter<&htmlPopStateEventClass, prHtmlPopStateEventHasUAVisualTransition>)),
};

constexpr JSCFunctionListEntry kHashChangeEventProto[] = {
    PR_JS_TAG("HashChangeEvent"),
    PR_JS_GET("oldURL", (jsStringGetter<&htmlHashChangeEventClass, prHtmlHashChangeEventOldUrl>)),
    PR_JS_GET("newURL", (jsStringGetter<&htmlHashChangeEventClass, prHtmlHashChangeEventNewUrl>)),
};

constexpr JSCFunctionListEntry kUserActivationProto[] = {
    PR_JS_TAG("UserActivation"),
    PR_JS_GET("hasBeenActive", userActivationHasBeenActive),
    PR_JS_GET("isActive", userActivationIsActive),
};

constexpr JSCFunctionListEntry kNavigatorProto[] = {
    PR_JS_TAG("Navigator"),
    PR_JS_MAGIC_GET("appCodeName", navigatorString, NavigatorString_AppCodeName),
    PR_JS_MAGIC_GET("appName", navigatorString, NavigatorString_AppName),
    PR_JS_MAGIC_GET("appVersion", navigatorString, NavigatorString_AppVersion),
    PR_JS_MAGIC_GET("platform", navigatorString, NavigatorString_Platform),
    PR_JS_MAGIC_GET("product", navigatorString, NavigatorString_Product),
    PR_JS_MAGIC_GET("productSub", navigatorString, NavigatorString_ProductSub),
    PR_JS_MAGIC_GET("userAgent", navigatorString, NavigatorString_UserAgent),
    PR_JS_MAGIC_GET("vendor", navigatorString, NavigatorString_Vendor),
    PR_JS_MAGIC_GET("vendorSub", navigatorString, NavigatorString_VendorSub),
    PR_JS_MAGIC_GET("language", navigatorString, NavigatorString_Language),
    PR_JS_GET("languages", navigatorLanguages),
    PR_JS_MAGIC_GET("onLine", navigatorFlag, NavigatorFlag_OnLine),
    PR_JS_GET("userActivation", navigatorUserActivation),
    PR_JS_MAGIC_GET("cookieEnabled", navigatorFlag, NavigatorFlag_CookieEnabled),
    PR_JS_MAGIC_GET("pdfViewerEnabled", navigatorFlag, NavigatorFlag_PdfViewerEnabled),
    PR_JS_MAGIC_GET("webdriver", navigatorFlag, NavigatorFlag_Webdriver),
    PR_JS_METHOD("javaEnabled", 0, navigatorJavaEnabled),
    PR_JS_METHOD("sendBeacon", 1, navigatorSendBeacon),
    PR_JS_GET("hardwareConcurrency", navigatorHardwareConcurrency),
    PR_JS_GET("maxTouchPoints", navigatorMaxTouchPoints),
    PR_JS_GET("preferences", preferences),
};

constexpr JSCFunctionListEntry kScreenProto[] = {
    PR_JS_TAG("Screen"),
    PR_JS_MAGIC_GET("availWidth", screenMeasure, ScreenMeasure_Width),
    PR_JS_MAGIC_GET("availHeight", screenMeasure, ScreenMeasure_Height),
    PR_JS_MAGIC_GET("width", screenMeasure, ScreenMeasure_Width),
    PR_JS_MAGIC_GET("height", screenMeasure, ScreenMeasure_Height),
    PR_JS_MAGIC_GET("colorDepth", screenMeasure, ScreenMeasure_ColorDepth),
    PR_JS_MAGIC_GET("pixelDepth", screenMeasure, ScreenMeasure_ColorDepth),
    PR_JS_GET("orientation", screenOrientation),
};

constexpr JSCFunctionListEntry kScreenOrientationProto[] = {
    PR_JS_TAG("ScreenOrientation"),
    PR_JS_METHOD("lock", 1, orientationLock),
    PR_JS_METHOD("unlock", 0, orientationUnlock),
    PR_JS_GET("type", orientationType),
    PR_JS_GET("angle", orientationAngle),
    PR_JS_DOM_EVENT_HANDLER(onchange),
};

constexpr JSCFunctionListEntry kCryptoProto[] = {
    PR_JS_TAG("Crypto"),
    PR_JS_METHOD("getRandomValues", 1, cryptoGetRandomValues),
    PR_JS_METHOD("randomUUID", 0, cryptoRandomUuid),
};

constexpr JSCFunctionListEntry kPreferenceManagerProto[] = {
    PR_JS_TAG("PreferenceManager"),
    PR_JS_MAGIC_GET("colorScheme", preference, PrHtmlPreference_ColorScheme),
    PR_JS_MAGIC_GET("contrast", preference, PrHtmlPreference_Contrast),
    PR_JS_MAGIC_GET("reducedMotion", preference, PrHtmlPreference_ReducedMotion),
    PR_JS_MAGIC_GET("reducedTransparency", preference, PrHtmlPreference_ReducedTransparency),
    PR_JS_MAGIC_GET("reducedData", preference, PrHtmlPreference_ReducedData),
};

constexpr JSCFunctionListEntry kPreferenceObjectProto[] = {
    PR_JS_TAG("PreferenceObject"),
    PR_JS_GET("override", preferenceOverride),
    PR_JS_GET("value", preferenceValue),
    PR_JS_GET("validValues", validValues),
    PR_JS_METHOD("clearOverride", 0, clearOverride),
    PR_JS_METHOD("requestOverride", 1, requestOverride),
    PR_JS_DOM_EVENT_HANDLER(onchange),
};

constexpr JSCFunctionListEntry kPromiseRejectionEventProto[] = {
    PR_JS_TAG("PromiseRejectionEvent"),
    PR_JS_MAGIC_GET("promise", rejectionMember, RejectionSlot_Promise),
    PR_JS_MAGIC_GET("reason", rejectionMember, RejectionSlot_Reason),
};

constexpr JSCFunctionListEntry kErrorEventProto[] = {
    PR_JS_TAG("ErrorEvent"),
    PR_JS_GET("message", (jsStringGetter<&htmlErrorEventClass, prHtmlErrorEventMessage>)),
    PR_JS_GET("filename", (jsStringGetter<&htmlErrorEventClass, prHtmlErrorEventFilename>)),
    PR_JS_GET("lineno", (jsNumberGetter<&htmlErrorEventClass, prHtmlErrorEventLineno>)),
    PR_JS_GET("colno", (jsNumberGetter<&htmlErrorEventClass, prHtmlErrorEventColno>)),
    PR_JS_GET("error", errorValue),
};

constexpr JSCFunctionListEntry kPageRevealEventProto[] = {
    PR_JS_TAG("PageRevealEvent"),
    PR_JS_GET("viewTransition", pageRevealViewTransition),
};

constexpr JSCFunctionListEntry kMessageEventProto[] = {
    PR_JS_TAG("MessageEvent"),
    PR_JS_GET("data", messageData),
    PR_JS_GET("origin", (jsStringGetter<&htmlMessageEventClass, prHtmlMessageEventOrigin>)),
    PR_JS_GET("lastEventId", (jsStringGetter<&htmlMessageEventClass, prHtmlMessageEventLastEventId>)),
    PR_JS_GET("source", messageSource),
    PR_JS_GET("ports", messagePorts),
};

constexpr JSCFunctionListEntry kPerformanceProto[] = {
    PR_JS_TAG("Performance"),
    PR_JS_METHOD("now", 0, performanceNow),
    PR_JS_GET("timeOrigin", (jsNumberGetter<&htmlPerformanceClass, prHtmlPerformanceTimeOrigin>)),
    PR_JS_METHOD("toJSON", 0, performanceToJson),
};

constexpr JSCFunctionListEntry kAnimationTimelineProto[] = {
    PR_JS_TAG("AnimationTimeline"),
    PR_JS_GET("currentTime", timelineCurrentTime),
    PR_JS_GET("duration", timelineDuration),
};

constexpr JSCFunctionListEntry kDocumentTimelineProto[] = {PR_JS_TAG("DocumentTimeline")};

constexpr JSCFunctionListEntry kWindowProto[] = {
    PR_JS_TAG("Window"),
    PR_JS_METHOD("postMessage", 1, postMessage),
    PR_JS_GET("document", document),
    PR_JS_GET("navigator", navigator),
    PR_JS_GET("screen", screen),
    PR_JS_GET("crypto", crypto),
    PR_JS_GET("customElements", windowCustomElements),
    PR_JS_METHOD("getSelection", 0, windowGetSelection),
    PR_JS_MAGIC_GET_SET("performance", performance, setReplaceable, 0),
    PR_JS_MAGIC_GET_SET("innerWidth", viewportGet, setReplaceable, 1),
    PR_JS_MAGIC_GET_SET("innerHeight", viewportGet, setReplaceable, 2),
    PR_JS_MAGIC_GET_SET("devicePixelRatio", viewportGet, setReplaceable, 8),
    PR_JS_MAGIC_GET_SET("length", windowLength, setReplaceable, 9),
    PR_JS_MAGIC_GET_SET("scrollX", viewportGet, setReplaceable, 3),
    PR_JS_MAGIC_GET_SET("scrollY", viewportGet, setReplaceable, 4),
    PR_JS_MAGIC_GET_SET("pageXOffset", viewportGet, setReplaceable, 5),
    PR_JS_MAGIC_GET_SET("pageYOffset", viewportGet, setReplaceable, 6),
    PR_JS_MAGIC("scroll", 0, windowScroll, 0),
    PR_JS_MAGIC("scrollTo", 0, windowScroll, 0),
    PR_JS_MAGIC("scrollBy", 0, windowScroll, 1),
    PR_JS_MAGIC_GET_SET("parent", ancestor, setReplaceable, kParentMagic),
    PR_JS_MAGIC_GET("top", ancestor, 0),
    PR_JS_GET_SET("location", windowLocation, setWindowLocation),
    PR_JS_GET_SET("name", windowName, setWindowName),
    PR_JS_GET("history", windowHistory),
    PR_JS_MAGIC_GET("localStorage", windowStorage, PrHtmlStorageKind_Local),
    PR_JS_MAGIC_GET("sessionStorage", windowStorage, PrHtmlStorageKind_Session),
    PR_JS_METHOD("getComputedStyle", 1, getComputedStyle),
    PR_JS_METHOD("matchMedia", 1, matchMedia),
    PR_JS_METHOD("btoa", 1, btoa),
    PR_JS_METHOD("atob", 1, atob),
    PR_JS_MAGIC("setTimeout", 1, setTimeout, 0),
    PR_JS_METHOD("clearTimeout", 0, clearTimeout),
    PR_JS_MAGIC("setInterval", 1, setTimeout, 1),
    PR_JS_METHOD("clearInterval", 0, clearTimeout),
    PR_JS_METHOD("requestAnimationFrame", 1, requestAnimationFrame),
    PR_JS_METHOD("cancelAnimationFrame", 1, cancelAnimationFrame),
    PR_JS_METHOD("requestIdleCallback", 1, requestIdleCallback),
    PR_JS_METHOD("cancelIdleCallback", 1, cancelIdleCallback),
    PR_JS_METHOD("structuredClone", 1, structuredClone),
    PR_JS_DOM_GLOBAL_EVENT_HANDLER_MEMBERS PR_JS_DOM_WINDOW_EVENT_HANDLER_MEMBERS
};

constexpr JsClassDefinition kClasses[] = {
    {
        .name = "DOMParser",
        .id = &htmlDomParserClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kDomParserProto,
        .protoCount = int(countOf(kDomParserProto)),
        .constructor = parserConstruct,
    },
    {
        PR_JS_CLASS("HTMLElement", htmlElementClass, jsViewFinalizer<&htmlElementClass>, kElementProto),
        .constructor = jsCustomElementsConstruct,
        .base = &prJsDomElementClass,
    },
    {
        PR_JS_CLASS("HTMLDialogElement", htmlDialogElementClass,
            jsViewFinalizer<&htmlDialogElementClass>, kDialogElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLIFrameElement", htmlIFrameElementClass,
            jsViewFinalizer<&htmlIFrameElementClass>, kIFrameElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLImageElement", htmlImageElementClass, jsViewFinalizer<&htmlImageElementClass>,
            kImageElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLCanvasElement", htmlCanvasElementClass, jsViewFinalizer<&htmlCanvasElementClass>,
            kCanvasElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLAnchorElement", htmlAnchorElementClass, jsViewFinalizer<&htmlAnchorElementClass>,
            kAnchorElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLAreaElement", htmlAreaElementClass, jsViewFinalizer<&htmlAreaElementClass>, kAreaElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLBaseElement", htmlBaseElementClass, jsViewFinalizer<&htmlBaseElementClass>, kBaseElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLLinkElement", htmlLinkElementClass, jsViewFinalizer<&htmlLinkElementClass>, kLinkElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLMetaElement", htmlMetaElementClass, jsViewFinalizer<&htmlMetaElementClass>, kMetaElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLPictureElement", htmlPictureElementClass, jsViewFinalizer<&htmlPictureElementClass>,
            kPictureElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLSourceElement", htmlSourceElementClass, jsViewFinalizer<&htmlSourceElementClass>,
            kSourceElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLMediaElement", htmlMediaElementClass, jsViewFinalizer<&htmlMediaElementClass>,
            kMediaElementProto),
        PR_JS_CONSTANTS(kMediaElementConstants),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLVideoElement", htmlVideoElementClass, jsViewFinalizer<&htmlVideoElementClass>,
            kVideoElementProto),
        .base = &htmlMediaElementClass,
    },
    {
        PR_JS_CLASS("HTMLAudioElement", htmlAudioElementClass, jsViewFinalizer<&htmlAudioElementClass>,
            kAudioElementProto),
        .base = &htmlMediaElementClass,
    },
    {
        PR_JS_CLASS("MediaError", htmlMediaErrorClass,
            (jsFinalizer<&htmlMediaErrorClass, mediaErrorDestroy>), kMediaErrorProto),
        PR_JS_CONSTANTS(kMediaErrorConstants)
    },
    {
        PR_JS_CLASS("TimeRanges", htmlTimeRangesClass,
            (jsFinalizer<&htmlTimeRangesClass, timeRangesDestroy>), kTimeRangesProto)
    },
    {
        PR_JS_CLASS("HTMLSlotElement", htmlSlotElementClass, jsViewFinalizer<&htmlSlotElementClass>, kSlotElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLScriptElement", htmlScriptElementClass, jsViewFinalizer<&htmlScriptElementClass>,
            kScriptElementProto),
        .statics = kScriptElementStatics,
        .staticCount = int(countOf(kScriptElementStatics)),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLStyleElement", htmlStyleElementClass, jsViewFinalizer<&htmlStyleElementClass>,
            kStyleElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("HTMLTemplateElement", htmlTemplateElementClass,
            jsViewFinalizer<&htmlTemplateElementClass>, kTemplateElementProto),
        .base = &htmlElementClass,
    },
    {
        PR_JS_CLASS("ErrorEvent", htmlErrorEventClass, jsViewFinalizer<&htmlErrorEventClass>, kErrorEventProto),
        .constructor = errorEventConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("MessageEvent", htmlMessageEventClass, jsViewFinalizer<&htmlMessageEventClass>, kMessageEventProto),
        .constructor = messageConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("PageRevealEvent", htmlPageRevealEventClass,
            jsViewFinalizer<&htmlPageRevealEventClass>, kPageRevealEventProto),
        .constructor = pageRevealConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("PromiseRejectionEvent", htmlPromiseRejectionEventClass,
            jsViewFinalizer<&htmlPromiseRejectionEventClass>, kPromiseRejectionEventProto),
        .constructor = rejectionConstruct,
        .length = 2,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("DOMStringMap", htmlDomStringMapClass, jsViewFinalizer<&htmlDomStringMapClass>, kDomStringMapProto),
        .named = &kDataset,
    },
    {
        PR_JS_CLASS("Storage", htmlStorageClass, jsViewFinalizer<&htmlStorageClass>, kStorageProto),
        .named = &kStorageNamed,
    },
    {
        PR_JS_CLASS("IdleDeadline", htmlIdleDeadlineClass, (jsFinalizer<&htmlIdleDeadlineClass, freeIdleDeadline>),
            kIdleDeadlineProto)
    },
    {PR_JS_CLASS("Location", htmlLocationClass, jsViewFinalizer<&htmlLocationClass>, kLocationProto)},
    {
        PR_JS_CLASS("UserActivation", htmlUserActivationClass, jsViewFinalizer<&htmlUserActivationClass>,
            kUserActivationProto)
    },
    {PR_JS_CLASS("History", htmlHistoryClass, jsViewFinalizer<&htmlHistoryClass>, kHistoryProto)},
    {
        PR_JS_CLASS("PopStateEvent", htmlPopStateEventClass, jsViewFinalizer<&htmlPopStateEventClass>,
            kPopStateEventProto),
        .constructor = popStateConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("HashChangeEvent", htmlHashChangeEventClass, jsViewFinalizer<&htmlHashChangeEventClass>,
            kHashChangeEventProto),
        .constructor = hashChangeConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {PR_JS_CLASS("Navigator", htmlNavigatorClass, jsViewFinalizer<&htmlNavigatorClass>, kNavigatorProto)},
    {PR_JS_CLASS("Screen", htmlScreenClass, jsViewFinalizer<&htmlScreenClass>, kScreenProto)},
    {
        PR_JS_CLASS("ScreenOrientation", htmlScreenOrientationClass,
            jsViewFinalizer<&htmlScreenOrientationClass>, kScreenOrientationProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("TextTrackList", htmlTextTrackListClass, jsViewFinalizer<&htmlTextTrackListClass>,
            kTextTrackListProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("AudioTrackList", htmlAudioTrackListClass, jsViewFinalizer<&htmlAudioTrackListClass>,
            kAudioTrackListProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("VideoTrackList", htmlVideoTrackListClass, jsViewFinalizer<&htmlVideoTrackListClass>,
            kVideoTrackListProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {PR_JS_CLASS("Crypto", htmlCryptoClass, jsViewFinalizer<&htmlCryptoClass>, kCryptoProto)},
    {
        PR_JS_CLASS("PreferenceManager", htmlPreferenceManagerClass,
            jsViewFinalizer<&htmlPreferenceManagerClass>, kPreferenceManagerProto)
    },
    {
        PR_JS_CLASS("PreferenceObject", htmlPreferenceObjectClass,
            jsViewFinalizer<&htmlPreferenceObjectClass>, kPreferenceObjectProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("Performance", htmlPerformanceClass, jsViewFinalizer<&htmlPerformanceClass>, kPerformanceProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
    {
        PR_JS_CLASS("AnimationTimeline", htmlAnimationTimelineClass,
            (jsFinalizer<&htmlAnimationTimelineClass, prHtmlDocumentTimelineDestroy>), kAnimationTimelineProto),
    },
    {
        PR_JS_CLASS("DocumentTimeline", htmlDocumentTimelineClass,
            (jsFinalizer<&htmlDocumentTimelineClass, prHtmlDocumentTimelineDestroy>), kDocumentTimelineProto),
        .constructor = timelineConstruct,
        .base = &htmlAnimationTimelineClass,
    },
    {
        PR_JS_CLASS("Window", htmlWindowClass, jsViewFinalizer<&htmlWindowClass>, kWindowProto),
        .base = &prJsDomEventTargetClass,
        .references = &kTargetReferences,
    },
};

} // namespace

void jsHtmlBind(JSContext* ctx) {
    prJsCanvasBind(ctx);
    jsCustomElementsBind(ctx);
    prJsHtmlConsoleBind(ctx);
    for (const JsClassDefinition& definition : kClasses) {
        JSValue constructor = jsDefineClass(ctx, definition);
        if (definition.id == &htmlImageElementClass)
            jsHtmlDefineFactory(ctx, "Image", JsCallScoped<imageFactory>::call, 0, constructor);
        if (definition.id == &htmlAudioElementClass)
            jsHtmlDefineFactory(ctx, "Audio", JsCallScoped<audioFactory>::call, 0, constructor);
        JS_FreeValue(ctx, constructor);
    }
    jsHtmlFormsBind(ctx);
    jsHtmlElementsBind(ctx);
    prJsFetchSetBaseUrl(realmBaseUrl);
    bindObjectUrls(ctx);
    jsHtmlMessageBind(ctx);
    jsHtmlMediaSourceBind(ctx);
    jsHtmlPerformanceBind(ctx);
    // The members that HTML adds to the classes of other modules.
    struct Extension {
        JSClassID id;
        const JSCFunctionListEntry* members;
        int count;
    };
    const Extension extensions[] = {
        {prJsDomElementClass, kDomElementProto, int(countOf(kDomElementProto))},
        {prJsDomDocumentClass, kDocumentProto, int(countOf(kDocumentProto))},
        {prJsDomShadowRootClass, kShadowRootProto, int(countOf(kShadowRootProto))},
    };
    for (const Extension& extended : extensions) {
        JSValue proto = JS_GetClassProto(ctx, extended.id);
        JS_SetPropertyFunctionList(ctx, proto, extended.members, extended.count);
        JS_FreeValue(ctx, proto);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    // The interface object of Document also goes by the name that browsers give HTML documents.
    JS_DefinePropertyValueStr(ctx, global, "HTMLDocument", JS_GetPropertyStr(ctx, global, "Document"),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    // HTMLOrSVGElement's members, on the elements of the foreign modules that the realm binds.
    constexpr const char* kForeign[] = {"SVGElement", "MathMLElement"};
    for (const char* foreign : kForeign) {
        JSValue constructor = JS_GetPropertyStr(ctx, global, foreign);
        if (JS_IsObject(constructor)) {
            JSValue proto = JS_GetPropertyStr(ctx, constructor, "prototype");
            JS_SetPropertyFunctionList(ctx, proto, kForeignElementProto, int(countOf(kForeignElementProto)));
            JS_FreeValue(ctx, proto);
        }
        JS_FreeValue(ctx, constructor);
    }
    // AbortSignal.timeout, which runs on a window's timers.
    JSValue abortSignal = JS_GetPropertyStr(ctx, global, "AbortSignal");
    if (JS_IsObject(abortSignal))
        JS_SetPropertyFunctionList(ctx, abortSignal, kAbortSignalStatic, int(countOf(kAbortSignalStatic)));
    JS_FreeValue(ctx, abortSignal);
    JS_FreeValue(ctx, global);
    defineNamedProperties(ctx);

    JSRuntime* runtime = JS_GetRuntime(ctx);
    jsAddClassifier(runtime, prJsDomElementClass, classifyElement);
    jsAddClassifier(runtime, prJsDomEventClass, classifyEvent);
    jsAddClassifier(runtime, prJsDomEventTargetClass, classifyEventTarget);
    prJsDomSetGlobalDocument(runtime, &kGlobalDocument);
    jsSetExceptionReporter(runtime, &kReporter);
    JS_SetHostPromiseRejectionTracker(runtime, trackRejection, nullptr);
}

PrHtmlWindow* PR_NULLABLE jsHtmlGlobalWindow(JSContext* ctx) {
    return globalWindow(ctx);
}

JSValue jsHtmlTimeRangesNew(JSContext* ctx, const PrMediaTimeRange* PR_NULLABLE ranges, uint32_t count) {
    TimeRangesValue* value = allocZeroed<TimeRangesValue>();
    for (uint32_t i = 0; i < count; ++i) {
        value->bounds.push(ranges[i].start);
        value->bounds.push(ranges[i].end);
    }
    JSValue object = jsWrap(ctx, htmlTimeRangesClass, JS_NULL, value, true);
    if (JS_IsException(object))
        timeRangesDestroy(value);
    return object;
}

bool jsHtmlSetGlobal(JSContext* ctx, PrHtmlWindow* handle) {
    // The runtime runs the agent of the first window it is given.
    JSRuntime* runtime = JS_GetRuntime(ctx);
    if (prJsDomAgent(runtime) == nullptr)
        prJsDomSetAgent(runtime, prHtmlWindowAgent(handle));
    if (prJsDomAgent(runtime) != prHtmlWindowAgent(handle)) {
        JS_ThrowInternalError(ctx, "A runtime runs the windows of one agent");
        return false;
    }
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, handle, false);
    if (JS_IsException(object))
        return false;
    JSValue global = JS_GetGlobalObject(ctx);
    bool ok = JS_SetPrototype(ctx, global, object) >= 0;
    jsSetReference(ctx, object, WindowSlot_Global, global);
    // The document's object is held before script reaches any of its nodes, which script can also reach without it, as
    // through the window's named properties. A window keeps its document, since a new document gets a new window.
    JSValue document = heldDocument(ctx, object, handle);
    ok &= !JS_IsException(document);
    JS_FreeValue(ctx, document);
    ok &= jsCustomElementsSetGlobal(ctx, object, handle);
    // html/#dom-window, html/#dom-self and html/#dom-frames, for a browsing context that no script opened.
    constexpr const char* kSelves[] = {"window", "self", "frames"};
    for (const char* name : kSelves)
        ok &= JS_DefinePropertyValueStr(ctx, global, name, JS_DupValue(ctx, global), JS_PROP_CONFIGURABLE) >= 0;
    ok &= JS_DefinePropertyValueStr(ctx, global, "opener", JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0;
    // The engine's own performance object would hide the window's.
    JSAtom performance = JS_NewAtom(ctx, "performance");
    ok &= JS_DeleteProperty(ctx, global, performance, 0) >= 0;
    JS_FreeAtom(ctx, performance);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, object);
    return ok;
}

bool jsHtmlNotifyRejectedPromises(JSContext* ctx) {
    PrHtmlWindow* window = globalWindow(ctx);
    if (window == nullptr)
        return false;
    JSValue object = jsWrap(ctx, htmlWindowClass, JS_NULL, window, false);
    JSValue list = jsReference(ctx, object, WindowSlot_Rejected);
    jsSetReference(ctx, object, WindowSlot_Rejected, JS_UNDEFINED);
    JS_FreeValue(ctx, object);
    if (JS_IsUndefined(list))
        return false;
    int64_t length = 0;
    JS_GetLength(ctx, list, &length);
    PrDomEventInit init = prDomEventInitDefault();
    init.cancelable = true;
    for (int64_t i = 0; i < length; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, list, uint32_t(i));
        JSValue promise = JS_GetPropertyUint32(ctx, entry, 0);
        JSValue reason = JS_GetPropertyUint32(ctx, entry, 1);
        JSValue event = rejectionEvent(ctx, "unhandledrejection", &init, promise, reason);
        PrHtmlPromiseRejectionEvent* handle =
            jsHandleAs<PrHtmlPromiseRejectionEvent>(ctx, event, htmlPromiseRejectionEventClass);
        if (handle != nullptr && prHtmlWindowNotifyRejection(window, handle))
            jsPrintException(ctx, reason);
        JS_FreeValue(ctx, event);
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, list);
    return length != 0;
}

bool jsHtmlCompileEventHandler(JSContext* ctx, PrDomEventTarget* target, PrDomElement* element,
        PrDomEventHandler handler, PrStringView body, PrDomEventHandlerCallback* out) {
    // If the handler belongs to an element, the scope is the element and its document. If it belongs to the Window, the
    // scope is the global alone, and the Window's onerror receives an ErrorEvent's members.
    bool scoped = prDomElementAsEventTarget(element) == target;
    bool error = !scoped && handler == PrDomEventHandler_onerror;
    PrTextBuffer source = {};
    const char* opening = "(function() { return function(event) {\n";
    if (scoped)
        opening = "(function(document__, element__) { with (document__) with (element__) return function(event) {\n";
    else if (error)
        opening = "(function() { return function(event, source, lineno, colno, error) {\n";
    prTextBufferAppend(&source, opening);
    // An SVG element's handler also names its event evt (SVG2/interact.html#EventAttributes).
    if (scoped && prDomElementNamespace(element) == PrDomNamespace_Svg)
        prTextBufferAppend(&source, "var evt = event;\n");
    prTextBufferAppend(&source, body);
    prTextBufferAppend(&source, "\n}; })");
    // JS_Eval reads a terminator after the source.
    prTextBufferAppendChar(&source, '\0');
    PrStringView text = prTextBufferView(&source);
    JSValue outer = JS_Eval(ctx, text.data, text.size - 1, "event handler", JS_EVAL_TYPE_GLOBAL);
    prTextBufferReset(&source);
    JSValue function = JS_EXCEPTION;
    if (!JS_IsException(outer)) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue args[2] = {JS_UNDEFINED, JS_UNDEFINED};
        if (scoped) {
            PrDomNode* node = prDomElementAsNode(element);
            args[0] = jsWrapAs(ctx, prJsDomDocumentClass, global, prDomNodeOwnerDocument(node), false);
            args[1] = jsWrapAs(ctx, prJsDomElementClass, global, element, false);
        }
        if (JS_IsException(args[0]) || JS_IsException(args[1]))
            function = JS_EXCEPTION;
        else
            function = JS_Call(ctx, outer, global, scoped ? 2 : 0, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, global);
    }
    JS_FreeValue(ctx, outer);
    JSValue owner = JS_IsException(function) ? JS_UNDEFINED : prJsDomWrapEventTarget(ctx, JS_UNDEFINED, target);
    bool ok = !JS_IsException(function) && !JS_IsException(owner) && prJsDomEventHandlerCallback(ctx, function, owner, out);
    JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, function);
    if (!ok)
        jsReportException(ctx);
    return ok;
}

} // namespace pr_impl
