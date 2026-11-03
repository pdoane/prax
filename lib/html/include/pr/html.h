// HTML elements, parsing and the Window
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/canvas.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/url.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The interfaces that this header declares, each named for its web interface.
typedef struct PrHtmlElement PrHtmlElement;
typedef struct PrHtmlAnchorElement PrHtmlAnchorElement;
typedef struct PrHtmlAreaElement PrHtmlAreaElement;
typedef struct PrHtmlBaseElement PrHtmlBaseElement;
typedef struct PrHtmlCanvasElement PrHtmlCanvasElement;
typedef struct PrHtmlDialogElement PrHtmlDialogElement;
typedef struct PrHtmlIFrameElement PrHtmlIFrameElement;
typedef struct PrHtmlImageElement PrHtmlImageElement;
typedef struct PrHtmlLinkElement PrHtmlLinkElement;
typedef struct PrHtmlMediaElement PrHtmlMediaElement;
typedef struct PrHtmlTrackList PrHtmlTrackList;
typedef struct PrHtmlMetaElement PrHtmlMetaElement;
typedef struct PrHtmlPictureElement PrHtmlPictureElement;
typedef struct PrHtmlScriptElement PrHtmlScriptElement;
typedef struct PrHtmlSourceElement PrHtmlSourceElement;
typedef struct PrHtmlStyleElement PrHtmlStyleElement;
typedef struct PrHtmlSlotElement PrHtmlSlotElement;
typedef struct PrHtmlTemplateElement PrHtmlTemplateElement;
typedef struct PrHtmlErrorEvent PrHtmlErrorEvent;
typedef struct PrHtmlPromiseRejectionEvent PrHtmlPromiseRejectionEvent;
typedef struct PrHtmlMessageEvent PrHtmlMessageEvent;
typedef struct PrHtmlPageRevealEvent PrHtmlPageRevealEvent;
typedef struct PrHtmlTimerHandler PrHtmlTimerHandler;
typedef struct PrHtmlFrameRequestCallback PrHtmlFrameRequestCallback;
typedef struct PrHtmlWindow PrHtmlWindow;
typedef struct PrHtmlPreferenceObject PrHtmlPreferenceObject;
typedef struct PrHtmlPerformance PrHtmlPerformance;
typedef struct PrHtmlScreenOrientation PrHtmlScreenOrientation;
typedef struct PrHtmlDocumentTimeline PrHtmlDocumentTimeline;

// ---------------------------------------------------------------------------------------------------------------------
// DOMParser (html)

// The types that a DOMParser parses, each named for its MIME type (html/#domparsersupportedtype). TextHtml parses HTML,
// and every other type parses XML.
enum {
    PrHtmlDomParserSupportedType_TextHtml,
    PrHtmlDomParserSupportedType_TextXml,
    PrHtmlDomParserSupportedType_ApplicationXml,
    PrHtmlDomParserSupportedType_ApplicationXhtmlXml,
    PrHtmlDomParserSupportedType_ImageSvgXml,
};
typedef uint8_t PrHtmlDomParserSupportedType;

// Creates a document from `source` parsed as `type`, without running its scripts (html/#dom-domparser-parsefromstring).
// The caller holds the one reference. If an XML source is not well-formed, the document holds a parsererror element
// that describes the error.
PrDomDocument* prHtmlDomParserParseFromString(PrStringView source, PrHtmlDomParserSupportedType type);

// ---------------------------------------------------------------------------------------------------------------------
// DOM parsing and serialization (html)
//
// The members that HTML adds to every Element (html/#dom-parsing-and-serialization).

// Returns the HTML serialization of the children of `node`, which is an element or a shadow root
// (html/#dom-element-innerhtml and html/#dom-shadowroot-innerhtml).
PrString prHtmlInnerHtml(const PrDomNode* node);

// Replaces the children of `node` with `markup` parsed as a fragment. `node` is an element, a shadow root or a
// template's contents. The context of the parse is the element, or the shadow root's host.
void prHtmlSetInnerHtml(PrDomNode* node, PrStringView markup);

// Returns the HTML serialization of the element itself (html/#dom-element-outerhtml).
PrString prHtmlOuterHtml(const PrDomElement* element);

// Replaces the element with `markup` parsed as a fragment in the context of its parent. Returns NotAllowed if the
// parent is a document, and does nothing if the element has no parent.
PrStatus prHtmlSetOuterHtml(PrDomElement* element, PrStringView markup);

// Parses `markup` as a fragment and inserts it at `where` (html/#dom-element-insertadjacenthtml). Returns NotAllowed if
// `where` is outside the element and the element's parent is null or a document. Otherwise returns any failure of the
// mutation algorithms.
PrStatus prHtmlInsertAdjacentHtml(PrDomElement* element, PrDomInsertPosition where, PrStringView markup);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOrSVGElement (html)
//
// The members that HTML adds to HTML, SVG and MathML elements (html/#htmlorsvgelement).

// The members of FocusOptions (html/#focusoptions). `focusVisible` applies only if `hasFocusVisible` is set.
typedef struct PrHtmlFocusOptions {
    bool preventScroll;
    bool focusVisible;
    bool hasFocusVisible;
} PrHtmlFocusOptions;

// Returns the options that a focus without options uses, with every member false.
static inline PrHtmlFocusOptions prHtmlFocusOptionsDefault(void) {
    PrHtmlFocusOptions value;
    value.preventScroll = false;
    value.focusVisible = false;
    value.hasFocusVisible = false;
    return value;
}

// Moves the focus to the element if it is a focusable area, and fires the focus events (html/#dom-focus). Focus that a
// script moves is shown unless `options` says otherwise.
void prHtmlFocus(PrDomElement* element, const PrHtmlFocusOptions* PR_NULLABLE options);

// Removes the focus from the element if it has it, and fires the blur events (html/#dom-blur).
void prHtmlBlur(PrDomElement* element);

// Returns the tabindex attribute as an integer (html/#dom-tabindex). If the attribute is absent or does not parse,
// returns 0 for an element that is focusable by its kind, such as a link or a form control, and -1 for any other
// element. prHtmlSetTabIndex sets the attribute.
int32_t prHtmlTabIndex(const PrDomElement* element);
void prHtmlSetTabIndex(PrDomElement* element, int32_t value);

// ---------------------------------------------------------------------------------------------------------------------
// Document (html)
//
// The members that HTML adds to every Document (html/#the-document-object).

// Returns the window whose document this is, or null if there is none (html/#dom-document-defaultview).
PrHtmlWindow* PR_NULLABLE prHtmlDocumentDefaultView(PrDomDocument* document);

// Returns the script element whose classic script is running (html/#dom-document-currentscript). Returns null if no
// script is running, or if a module script is running.
PrDomElement* PR_NULLABLE prHtmlDocumentCurrentScript(PrDomDocument* document);

// Removes the document's children and listeners (html/#dom-document-open). A script-created parser then parses the
// writes that follow until prHtmlDocumentClose. Returns InvalidState for an XML document. Also returns InvalidState
// while the document's parser constructs a custom element or runs the reactions that follow
// (html/#throw-on-dynamic-markup-insertion-counter). prHtmlDocumentWrite and prHtmlDocumentClose return InvalidState in
// the same cases. Does nothing for a document that is not a browsing context's current document, or while its parser
// runs a script or parses a load.
PrStatus prHtmlDocumentOpen(PrDomDocument* document);

// Parses `text` (html/#dom-document-write). For a script that the document's parser runs, `text` is inserted at the
// parser's read position. Otherwise it is appended to the input that prHtmlDocumentOpen opened. If no input is open and
// no load is under way, the write opens one. Returns InvalidState for an XML document, and does nothing for a document
// that is not a browsing context's current document.
PrStatus prHtmlDocumentWrite(PrDomDocument* document, PrStringView text);

// Ends the input that prHtmlDocumentOpen opened, and the parse of that input (html/#dom-document-close).
PrStatus prHtmlDocumentClose(PrDomDocument* document);

// The readiness of a document (html/#current-document-readiness). A document that no parser loads is Complete.
enum {
    PrHtmlDocumentReadyState_Loading,
    PrHtmlDocumentReadyState_Interactive,
    PrHtmlDocumentReadyState_Complete,
};
typedef uint8_t PrHtmlDocumentReadyState;

// Returns the document's readiness (html/#dom-document-readystate).
PrHtmlDocumentReadyState prHtmlDocumentReadyState(const PrDomDocument* document);

// The collections of a document's elements that HTML names (html/#dom-document-images and the members after it).
enum {
    PrHtmlDocumentCollection_Images,    // img elements
    PrHtmlDocumentCollection_Embeds,    // embed elements, which plugins also returns
    PrHtmlDocumentCollection_Links,     // a and area elements with an href attribute
    PrHtmlDocumentCollection_Forms,     // form elements
    PrHtmlDocumentCollection_Scripts,   // script elements
    PrHtmlDocumentCollection_Anchors,   // a elements with a name attribute
};
typedef uint8_t PrHtmlDocumentCollection;

// Creates a live collection of the document's elements of `kind`, in tree order. The caller destroys it with
// prDomHtmlCollectionDestroy.
PrDomHtmlCollection* prHtmlDocumentCollection(PrDomDocument* document, PrHtmlDocumentCollection kind);

// Returns true if the document is its window's document and has the system focus (html/#dom-document-hasfocus). The
// top-level browsing context is taken to have the system focus. A nested browsing context has it if its parent has it
// and the iframe that contains it is focused in the parent's document.
bool prHtmlDocumentHasFocus(PrDomDocument* document);

// The visibility states of a document (html/#visibility-state).
enum {
    PrHtmlVisibility_Hidden,
    PrHtmlVisibility_Visible,
};
typedef uint8_t PrHtmlVisibility;

// Returns the document's visibility state (html/#dom-document-visibilitystate): its page's, as the host sets it
// (prHtmlPageSetVisibility), or hidden for a document that is not its window's document.
PrHtmlVisibility prHtmlDocumentVisibilityState(const PrDomDocument* document);

// Returns the focused element as `root` sees it (html/#dom-documentorshadowroot-activeelement). `root` is a document or
// a shadow root.
PrDomElement* PR_NULLABLE prHtmlActiveElement(PrDomNode* root);

// Sets the element that the user designates with a pointing device, or null for none. The element, its flat-tree
// ancestors, and the labeled control of any label among them match :hover (html/#selector-hover). The element must be
// connected in `document`. An element removed from the document is no longer designated.
void prHtmlDocumentSetHover(PrDomDocument* document, PrDomElement* PR_NULLABLE element);

// Sets the element being activated, or null for none. The element, its flat-tree ancestors, and the labeled control of
// any label among them match :active (html/#selector-active). The same rules as for prHtmlDocumentSetHover apply to
// `element`.
void prHtmlDocumentSetActive(PrDomDocument* document, PrDomElement* PR_NULLABLE element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLElement : Element (html)

// An HTMLElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlElement, DomEventTarget, EventTarget)

// The states of the dir attribute that a script sees (html/#dom-dir). None is the empty string.
enum {
    PrHtmlDir_None,
    PrHtmlDir_Ltr,
    PrHtmlDir_Rtl,
    PrHtmlDir_Auto,
};
typedef uint8_t PrHtmlDir;

// The states of the hidden attribute (html/#attr-hidden).
enum {
    PrHtmlHidden_NotHidden,
    PrHtmlHidden_Hidden,
    PrHtmlHidden_UntilFound,
};
typedef uint8_t PrHtmlHidden;

// Returns `object` as an HTMLElement, or null if it is not one.
PrHtmlElement* PR_NULLABLE prHtmlElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the state of the dir attribute, or None if the attribute names no state (html/#dom-dir). prHtmlElementSetDir
// sets the attribute to `value`.
PrHtmlDir prHtmlElementDir(const PrHtmlElement* element);
void prHtmlElementSetDir(PrHtmlElement* element, PrStringView value);

// Returns the state of the hidden attribute (html/#dom-hidden).
PrHtmlHidden prHtmlElementHidden(const PrHtmlElement* element);

// Sets the hidden attribute to `value`, or removes it for NotHidden (html/#dom-hidden).
void prHtmlElementSetHidden(PrHtmlElement* element, PrHtmlHidden value);

// Returns the text of the element's descendants, computed as for an element that is not being rendered
// (html/#dom-innertext).
PrString prHtmlElementInnerText(const PrHtmlElement* element);

// Replaces the element's children with `value`: a text node for each run of text, and a br element for each line break
// between them.
void prHtmlElementSetInnerText(PrHtmlElement* element, PrStringView value);

// Returns the element that the element's offsets are measured from (cssom-view-1/#dom-htmlelement-offsetparent). This
// is its nearest positioned ancestor, or the body. Returns null if the element generates no box, is the root or the
// body, or is fixed.
PrDomElement* PR_NULLABLE prHtmlElementOffsetParent(PrHtmlElement* element);

// Returns the element's border box as its document's render laid it out, before any transform or scroll. The top and
// left are measured from the offset parent's padding box (cssom-view-1/#dom-htmlelement-offsettop). The width and
// height are the extent of the bounds of the element's boxes. Each returns zero if the element generates no box.
int32_t prHtmlElementOffsetTop(PrHtmlElement* element);
int32_t prHtmlElementOffsetLeft(PrHtmlElement* element);
int32_t prHtmlElementOffsetWidth(PrHtmlElement* element);
int32_t prHtmlElementOffsetHeight(PrHtmlElement* element);

// Returns the value of a URL-reflecting attribute (html/#reflecting-content-attributes-in-idl-attributes), which is the
// content attribute `name` parsed against the document base URL. If the attribute does not parse, returns the
// attribute's value. If there is no attribute, returns the empty string.
PrString prHtmlElementReflectedUrl(const PrHtmlElement* element, PrStringView name);

// The kinds of integer IDL attribute that reflect a content attribute
// (html/#reflecting-content-attributes-in-idl-attributes).
enum {
    PrHtmlReflectedInteger_Unsigned,        // unsigned long
    PrHtmlReflectedInteger_Positive,        // unsigned long limited to only positive numbers
    PrHtmlReflectedInteger_NonNegative,     // long limited to only non-negative numbers
};
typedef uint8_t PrHtmlReflectedInteger;

// Returns the value of an integer IDL attribute of `kind` that reflects the content attribute `name`: the attribute
// parsed as an integer, or `fallback` if there is no attribute, it does not parse, or its integer is outside the kind's
// range. Every kind's range ends at 2147483647.
int64_t prHtmlElementReflectedInteger(const PrHtmlElement* element, PrStringView name, PrHtmlReflectedInteger kind,
    int64_t fallback);

// Sets the content attribute that prHtmlElementReflectedInteger reads to `value`, or to `fallback` for a value past
// 2147483647. Returns OutOfRange, and sets nothing, for a negative value of a NonNegative attribute or zero for a
// Positive one.
PrStatus prHtmlElementSetReflectedInteger(PrHtmlElement* element, PrStringView name, PrHtmlReflectedInteger kind,
    int64_t value, int64_t fallback);

// ---------------------------------------------------------------------------------------------------------------------
// DOMStringMap (html)
//
// An element's custom data attributes (html/#custom-data-attribute), by the names that script uses for them
// (html/#domstringmap). Such a name is the attribute's name without its data- prefix, with each hyphen before an ASCII
// lowercase letter removed and the letter uppercased.

// Writes up to `capacity` names of the element's custom data attributes to `items`, in the order of its attributes
// (html/#concept-domstringmap-pairs). Returns the total number of names.
uint32_t prHtmlDatasetNames(const PrDomElement* element, PrString* PR_NULLABLE items, uint32_t capacity);

// Returns the value of the attribute that `name` refers to, or the null view if the element has no such attribute.
PrStringView prHtmlDatasetGet(const PrDomElement* element, PrStringView name);

// Sets the attribute that `name` refers to. Returns Syntax if a hyphen comes before an ASCII lowercase letter in
// `name`, and InvalidData if the attribute's name is not a valid attribute local name.
PrStatus prHtmlDatasetSet(PrDomElement* element, PrStringView name, PrStringView value);

// Removes the attribute that `name` refers to, if the element has it.
void prHtmlDatasetDelete(PrDomElement* element, PrStringView name);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLDialogElement : HTMLElement (html)

// An HTMLDialogElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlDialogElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlDialogElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlDialogElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlDialogElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLDialogElement, or null if it is not one.
PrHtmlDialogElement* PR_NULLABLE prHtmlDialogElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns true if the open attribute is present (html/#dom-dialog-open). prHtmlDialogElementSetOpen adds or removes it.
bool prHtmlDialogElementOpen(const PrHtmlDialogElement* element);
void prHtmlDialogElementSetOpen(PrHtmlDialogElement* element, bool value);

// Returns the dialog's return value, which is the empty string until something sets it (html/#dom-dialog-returnvalue).
PrStringView prHtmlDialogElementReturnValue(const PrHtmlDialogElement* element);
void prHtmlDialogElementSetReturnValue(PrHtmlDialogElement* element, PrStringView value);

// Opens the dialog as a non-modal dialog (html/#dom-dialog-show). Returns InvalidState if the dialog is open and modal.
PrStatus prHtmlDialogElementShow(PrHtmlDialogElement* element);

// Opens the dialog as a modal dialog (html/#dom-dialog-showmodal). Returns InvalidState if the dialog is open and not
// modal, or is not connected.
PrStatus prHtmlDialogElementShowModal(PrHtmlDialogElement* element);

// Closes an open dialog, and fires close in a separate task (html/#close-the-dialog). Sets the return value to `result`
// unless `result` is the null view.
void prHtmlDialogElementClose(PrHtmlDialogElement* element, PrStringView result);

// Fires cancel at an open dialog, and closes it with `result` if no listener canceled the event
// (html/#dom-dialog-requestclose).
void prHtmlDialogElementRequestClose(PrHtmlDialogElement* element, PrStringView result);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLIFrameElement : HTMLElement (html)

// An HTMLIFrameElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlIFrameElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlIFrameElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlIFrameElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlIFrameElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLIFrameElement, or null if it is not one.
PrHtmlIFrameElement* PR_NULLABLE prHtmlIFrameElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the window of the browsing context that the iframe contains (html/#dom-iframe-contentwindow). The browsing
// context of the iframe's document owns that window. Returns null if the iframe contains no browsing context.
PrHtmlWindow* PR_NULLABLE prHtmlIFrameElementContentWindow(PrHtmlIFrameElement* element);

// Returns the document of that window, or null if there is no window.
PrDomDocument* PR_NULLABLE prHtmlIFrameElementContentDocument(PrHtmlIFrameElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLImageElement : HTMLElement (html)
//
// An image loads when its source changes, and fires a load or error event in a separate task.

// An HTMLImageElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlImageElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlImageElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlImageElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlImageElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLImageElement, or null if it is not one.
PrHtmlImageElement* PR_NULLABLE prHtmlImageElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the state of the current request (html/#current-request).
PrDomImageState prHtmlImageElementState(const PrHtmlImageElement* element);

// Returns true if the image has no source, or if its current request is completely available or broken
// (html/#dom-img-complete).
bool prHtmlImageElementComplete(const PrHtmlImageElement* element);

// Returns the URL of the current request, or the empty string if there is none (html/#dom-img-currentsrc).
PrStringView prHtmlImageElementCurrentSrc(const PrHtmlImageElement* element);

// Returns the natural size (html/#dom-img-naturalwidth), or zero if the image is not available or has no natural size.
uint32_t prHtmlImageElementNaturalWidth(const PrHtmlImageElement* element);
uint32_t prHtmlImageElementNaturalHeight(const PrHtmlImageElement* element);

// Returns the rendered size if the image is rendered, and otherwise its natural size (html/#dom-img-width).
uint32_t prHtmlImageElementWidth(const PrHtmlImageElement* element);
uint32_t prHtmlImageElementHeight(const PrHtmlImageElement* element);

// Sets the width or height content attribute, as a reflected non-negative integer.
void prHtmlImageElementSetWidth(PrHtmlImageElement* element, uint32_t value);
void prHtmlImageElementSetHeight(PrHtmlImageElement* element, uint32_t value);

// Writes the image of the current request, as its orientation displays it, as a source that a canvas context draws
// (html/#canvasimagesource), with one reference to the image for the caller. Returns false, and writes no image, if the
// request has no decoded image, as for an image that is not completely available.
bool prHtmlImageElementImageSource(PrHtmlImageElement* element, PrCanvasImageSource* out);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLCanvasElement : HTMLElement (html)
//
// A canvas element shows the bitmap of its rendering context (pr/canvas.h), as the context has drawn it at each
// rendering update. Its context renders with its window's canvas renderer (PrHtmlHost::canvas), and draws nothing in a
// document without a window.

// An HTMLCanvasElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlCanvasElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlCanvasElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlCanvasElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlCanvasElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLCanvasElement, or null if it is not one.
PrHtmlCanvasElement* PR_NULLABLE prHtmlCanvasElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Get and set the width and height content attributes, which reflect with the defaults 300 and 150
// (html/#dom-canvas-width). Setting either resizes the bitmap of a 2D context and resets the context. Setting returns
// InvalidState, and leaves the attribute, for a placeholder canvas.
uint32_t prHtmlCanvasElementWidth(const PrHtmlCanvasElement* element);
uint32_t prHtmlCanvasElementHeight(const PrHtmlCanvasElement* element);
PrStatus prHtmlCanvasElementSetWidth(PrHtmlCanvasElement* element, uint32_t value);
PrStatus prHtmlCanvasElementSetHeight(PrHtmlCanvasElement* element, uint32_t value);

// The kind of rendering context that a canvas has (html/#concept-canvas-context-mode): none, a 2D context, or none
// because an OffscreenCanvas draws for it.
enum {
    PrHtmlCanvasContextMode_None,
    PrHtmlCanvasContextMode_2d,
    PrHtmlCanvasContextMode_Placeholder,
};
typedef uint8_t PrHtmlCanvasContextMode;

// Returns the kind of rendering context that the canvas has.
PrHtmlCanvasContextMode prHtmlCanvasElementContextMode(const PrHtmlCanvasElement* element);

// Writes the canvas's 2D rendering context, which the canvas owns, creating it with `settings` if the canvas has no
// context (html/#dom-canvas-getcontext). Null `settings` means the defaults. Writes null for a canvas with a context of
// another kind, and returns InvalidState, writing null, for a placeholder canvas.
PrStatus prHtmlCanvasElementGetContext2D(PrHtmlCanvasElement* element, const PrCanvasSettings* PR_NULLABLE settings,
    PrCanvas* PR_NULLABLE* PR_NONNULL out);

// Writes a data: URL of the bitmap serialized as an image of the MIME type `type`, or as PNG if the window's codec does
// not encode that type (html/#dom-canvas-todataurl). A bitmap without pixels is "data:,". Returns NotAllowed, and
// writes the null string, if the bitmap is not origin-clean.
PrStatus prHtmlCanvasElementToDataUrl(PrHtmlCanvasElement* element, PrStringView type, PrString* out);

// Writes the bitmap as a source that a context draws (html/#canvasimagesource), with one reference to its image for the
// caller. A canvas without a context has a transparent bitmap of its size. Returns false, and writes no image, for a
// bitmap without pixels.
bool prHtmlCanvasElementImageSource(PrHtmlCanvasElement* element, PrCanvasImageSource* out);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLAnchorElement and HTMLAreaElement : HTMLElement, with HTMLHyperlinkElementUtils (html)

// An HTMLAnchorElement or an HTMLAreaElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlAnchorElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlAnchorElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlAnchorElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlAnchorElement, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlAreaElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlAreaElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlAreaElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlAreaElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLAnchorElement or an HTMLAreaElement, or null if it is not one.
PrHtmlAnchorElement* PR_NULLABLE prHtmlAnchorElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlAreaElement* PR_NULLABLE prHtmlAreaElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns a component of the URL of a hyperlink, which is an a or an area element (html/#api-for-a-and-area-elements).
// The URL is the element's href attribute parsed against the document base URL. If the attribute does not parse,
// `PrUrlComponent_Href` is the attribute's value, or the empty string if there is no attribute,
// `PrUrlComponent_Protocol` is ":", and every other component is the empty string.
PrString prHtmlHyperlinkComponent(const PrHtmlElement* element, PrUrlComponent component);

// Sets a component of the URL of a hyperlink. `PrUrlComponent_Href` sets the href attribute. Any other component
// changes the URL as the URL interface's setter does, and then sets the href attribute to the URL. Does nothing if the
// href attribute does not parse. Returns ReadOnly for `PrUrlComponent_Origin`.
PrStatus prHtmlHyperlinkSetComponent(PrHtmlElement* element, PrUrlComponent component, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLBaseElement : HTMLElement (html)

// An HTMLBaseElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlBaseElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlBaseElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlBaseElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlBaseElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLBaseElement, or null if it is not one.
PrHtmlBaseElement* PR_NULLABLE prHtmlBaseElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the href attribute parsed against the document's URL, or the attribute's value if it does not parse
// (html/#dom-base-href). A missing attribute is the empty string.
PrString prHtmlBaseElementHref(const PrHtmlBaseElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLLinkElement : HTMLElement (html)

// An HTMLLinkElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlLinkElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlLinkElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlLinkElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlLinkElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLLinkElement, or null if it is not one.
PrHtmlLinkElement* PR_NULLABLE prHtmlLinkElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the style sheet that the link loaded, or null if it has none.
PrCssStyleSheet* PR_NULLABLE prHtmlLinkElementSheet(PrHtmlLinkElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLMetaElement : HTMLElement (html)

// An HTMLMetaElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlMetaElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlMetaElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlMetaElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlMetaElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLMetaElement, or null if it is not one.
PrHtmlMetaElement* PR_NULLABLE prHtmlMetaElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLPictureElement : HTMLElement (html)

// An HTMLPictureElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlPictureElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlPictureElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlPictureElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlPictureElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLPictureElement, or null if it is not one.
PrHtmlPictureElement* PR_NULLABLE prHtmlPictureElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLSourceElement : HTMLElement (html)

// An HTMLSourceElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlSourceElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlSourceElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlSourceElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlSourceElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLSourceElement, or null if it is not one.
PrHtmlSourceElement* PR_NULLABLE prHtmlSourceElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLMediaElement : HTMLElement (html)
//
// The interface of the video and audio elements (html/#htmlvideoelement, html/#htmlaudioelement). The engine plays a
// media resource as a timeline, which the host's media codec reads (PrHtmlHost::media). A resource is supported if the
// codec reads it. Its media data is available as soon as it is fetched. A video element shows the frame at its playback
// position, where the codec decodes frames, or else its poster frame. A media element plays only if its document is its
// window's document.

// An HTMLMediaElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlMediaElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlMediaElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlMediaElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlMediaElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLMediaElement, or null if it is not one.
PrHtmlMediaElement* PR_NULLABLE prHtmlMediaElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns true if the media element is a video element, and false if it is an audio element.
bool prHtmlMediaElementIsVideo(const PrHtmlMediaElement* element);

// The network states of a media element (html/#dom-media-networkstate).
enum {
    PrHtmlMediaNetworkState_Empty,
    PrHtmlMediaNetworkState_Idle,
    PrHtmlMediaNetworkState_Loading,
    PrHtmlMediaNetworkState_NoSource,
};
typedef uint8_t PrHtmlMediaNetworkState;

// The ready states of a media element (html/#dom-media-readystate).
enum {
    PrHtmlMediaReadyState_HaveNothing,
    PrHtmlMediaReadyState_HaveMetadata,
    PrHtmlMediaReadyState_HaveCurrentData,
    PrHtmlMediaReadyState_HaveFutureData,
    PrHtmlMediaReadyState_HaveEnoughData,
};
typedef uint8_t PrHtmlMediaReadyState;

// The error codes of a media element (html/#dom-mediaerror-code), and None for no error.
enum {
    PrHtmlMediaError_None,
    PrHtmlMediaError_Aborted,
    PrHtmlMediaError_Network,
    PrHtmlMediaError_Decode,
    PrHtmlMediaError_SrcNotSupported,
};
typedef uint8_t PrHtmlMediaError;

// How likely a media element is to play a type (html/#canplaytyperesult).
enum {
    PrHtmlCanPlay_No,
    PrHtmlCanPlay_Maybe,
    PrHtmlCanPlay_Probably,
};
typedef uint8_t PrHtmlCanPlay;

// The time ranges of a media element (html/#timeranges).
enum {
    PrHtmlMediaRanges_Buffered,
    PrHtmlMediaRanges_Played,
    PrHtmlMediaRanges_Seekable,
};
typedef uint8_t PrHtmlMediaRanges;

// The promise that play() returns (html/#dom-media-play). The element settles it once: resolved if `rejection` is the
// null view, and otherwise rejected with the DOMException that `rejection` names. The element releases the promise
// after settling it, or releases it unsettled if the element is destroyed first.
typedef struct PrHtmlPlayPromise {
    void* PR_NULLABLE context;
    void (*PR_NONNULL settle)(void* PR_NULLABLE context, PrStringView rejection);
    void (*PR_NONNULL release)(void* PR_NULLABLE context);
} PrHtmlPlayPromise;

// Returns the code of the element's error, or None if there is no error (html/#dom-media-error).
PrHtmlMediaError prHtmlMediaElementError(const PrHtmlMediaElement* element);

// Returns the message of the element's error, or the empty string (html/#dom-mediaerror-message).
PrStringView prHtmlMediaElementErrorMessage(const PrHtmlMediaElement* element);

// Returns the URL of the current media resource, or the empty string if there is none (html/#dom-media-currentsrc).
PrStringView prHtmlMediaElementCurrentSrc(const PrHtmlMediaElement* element);

// Returns the element's network state and ready state.
PrHtmlMediaNetworkState prHtmlMediaElementNetworkState(const PrHtmlMediaElement* element);
PrHtmlMediaReadyState prHtmlMediaElementReadyState(const PrHtmlMediaElement* element);

// Resets the element and selects a media resource again (html/#dom-media-load).
void prHtmlMediaElementLoad(PrHtmlMediaElement* element);

// Returns how likely the element is to play a resource of the MIME type `type`, as its window's media codec answers
// (html/#dom-navigator-canplaytype).
PrHtmlCanPlay prHtmlMediaElementCanPlayType(const PrHtmlMediaElement* element, PrStringView type);

// Returns true if the element is seeking.
bool prHtmlMediaElementSeeking(const PrHtmlMediaElement* element);

// Returns the playback position in seconds (html/#dom-media-currenttime). prHtmlMediaElementSetCurrentTime and
// prHtmlMediaElementFastSeek seek to `seconds`.
double prHtmlMediaElementCurrentTime(PrHtmlMediaElement* element);
void prHtmlMediaElementSetCurrentTime(PrHtmlMediaElement* element, double seconds);
void prHtmlMediaElementFastSeek(PrHtmlMediaElement* element, double seconds);

// Returns the length of the media resource in seconds, or NaN if it is not known (html/#dom-media-duration).
double prHtmlMediaElementDuration(const PrHtmlMediaElement* element);

// prHtmlMediaElementPaused returns true if playback is paused, and prHtmlMediaElementEnded returns true if playback has
// ended.
bool prHtmlMediaElementPaused(const PrHtmlMediaElement* element);
bool prHtmlMediaElementEnded(PrHtmlMediaElement* element);

// Returns the rate that playback starts at, which is 1 at first (html/#dom-media-defaultplaybackrate).
double prHtmlMediaElementDefaultPlaybackRate(const PrHtmlMediaElement* element);
void prHtmlMediaElementSetDefaultPlaybackRate(PrHtmlMediaElement* element, double rate);

// Returns the rate that playback runs at, as a multiple of normal speed (html/#dom-media-playbackrate).
double prHtmlMediaElementPlaybackRate(const PrHtmlMediaElement* element);

// Sets the playback rate. Returns NotSupported for a rate that the element cannot play at.
PrStatus prHtmlMediaElementSetPlaybackRate(PrHtmlMediaElement* element, double rate);

// Returns true if playback at another rate keeps the audio's pitch (html/#dom-media-preservespitch).
bool prHtmlMediaElementPreservesPitch(const PrHtmlMediaElement* element);
void prHtmlMediaElementSetPreservesPitch(PrHtmlMediaElement* element, bool preserves);

// Writes up to `capacity` of the ranges of `which` to `starts` and `ends`, in seconds. Returns the total number of
// ranges.
uint32_t prHtmlMediaElementRanges(PrHtmlMediaElement* element, PrHtmlMediaRanges which,
    double* PR_NULLABLE starts, double* PR_NULLABLE ends, uint32_t capacity);

// Starts playback, and takes ownership of `promise` (html/#dom-media-play). A failure that the element reports settles
// the promise immediately. The element reports NotSupported if its source is not supported.
void prHtmlMediaElementPlay(PrHtmlMediaElement* element, const PrHtmlPlayPromise* promise);

// Pauses playback (html/#dom-media-pause).
void prHtmlMediaElementPause(PrHtmlMediaElement* element);

// Returns the volume, from 0 to 1 (html/#dom-media-volume).
double prHtmlMediaElementVolume(const PrHtmlMediaElement* element);

// Sets the volume. Returns OutOfRange for a volume outside [0, 1].
PrStatus prHtmlMediaElementSetVolume(PrHtmlMediaElement* element, double volume);

// Returns true if the audio is muted (html/#dom-media-muted). Muting is independent of the volume.
bool prHtmlMediaElementMuted(const PrHtmlMediaElement* element);
void prHtmlMediaElementSetMuted(PrHtmlMediaElement* element, bool muted);

// Returns the natural size of the video, or zero if it is not known (html/#dom-video-videowidth and
// html/#dom-video-videoheight).
uint32_t prHtmlMediaElementVideoWidth(const PrHtmlMediaElement* element);
uint32_t prHtmlMediaElementVideoHeight(const PrHtmlMediaElement* element);

// The kinds of a media element's tracks.
enum {
    PrHtmlTrackKind_Text,       // html/#text-track
    PrHtmlTrackKind_Audio,      // html/#audiotrack
    PrHtmlTrackKind_Video,      // html/#videotrack
};
typedef uint8_t PrHtmlTrackKind;

// Returns the list of the element's tracks of `kind` (html/#dom-media-texttracks, html/#dom-media-audiotracks,
// html/#dom-media-videotracks). The list is a part of the element: it lives as long as the element, and a reference to
// it is a reference to the element.
PrHtmlTrackList* prHtmlMediaElementTracks(PrHtmlMediaElement* element, PrHtmlTrackKind kind);

// ---------------------------------------------------------------------------------------------------------------------
// TextTrackList, AudioTrackList and VideoTrackList : EventTarget (html)
//
// A media element's tracks of one kind (html/#texttracklist, html/#audiotracklist, html/#videotracklist). The list
// fires addtrack and removetrack as the element's tracks change, and change as a track is enabled, selected or its mode
// changes.

// A track list as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlTrackList, DomEventTarget, EventTarget)

// Returns `object` as a track list, or null if it is not one.
PrHtmlTrackList* PR_NULLABLE prHtmlTrackListQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the kind of the tracks that the list holds.
PrHtmlTrackKind prHtmlTrackListKind(const PrHtmlTrackList* list);

// Returns the number of tracks in the list.
uint32_t prHtmlTrackListLength(const PrHtmlTrackList* list);

// Returns the index of the selected track of a list of video tracks, or -1 if none is selected
// (html/#dom-videotracklist-selectedindex).
int32_t prHtmlTrackListSelectedIndex(const PrHtmlTrackList* list);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLSlotElement : HTMLElement (html)
//
// A slot (dom/#concept-slot). The slottables of its shadow root are assigned to it by its name or by a script
// (html/#the-slot-element).

// An HTMLSlotElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlSlotElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlSlotElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlSlotElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlSlotElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLSlotElement, or null if it is not one.
PrHtmlSlotElement* PR_NULLABLE prHtmlSlotElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Writes up to `capacity` of the nodes assigned to the slot to `items` (html/#dom-slot-assignednodes). If `flatten` is
// set, each slot among them is replaced by its own assigned nodes, and a slot with none by its fallback content
// (dom/#find-flattened-slotables). Returns the total number of nodes.
uint32_t prHtmlSlotElementAssignedNodes(PrHtmlSlotElement* element, bool flatten, PrDomNode* PR_NONNULL* PR_NULLABLE items,
    uint32_t capacity);

// Sets the nodes that a manually assigning shadow root assigns to the slot (html/#dom-slot-assign). Each node must be
// an element or a text node. A node is removed from any other slot that it was assigned to.
void prHtmlSlotElementAssign(PrHtmlSlotElement* element, PrDomNode* const PR_NONNULL* PR_NULLABLE nodes, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLScriptElement : HTMLElement (html)

// An HTMLScriptElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlScriptElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlScriptElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlScriptElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlScriptElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLScriptElement, or null if it is not one.
PrHtmlScriptElement* PR_NULLABLE prHtmlScriptElementQuery(PrDomEventTarget* PR_NULLABLE object);

// html/#dom-script-async: true if the element is async because script created it and nothing has said otherwise
// (html/#script-force-async), or because it has an async attribute. Setting it makes the attribute say.
bool prHtmlScriptElementAsync(const PrHtmlScriptElement* element);
void prHtmlScriptElementSetAsync(PrHtmlScriptElement* element, bool value);

// html/#dom-script-text: the element's child text content, which setting replaces with one text node.
PrString prHtmlScriptElementText(const PrHtmlScriptElement* element);
void prHtmlScriptElementSetText(PrHtmlScriptElement* element, PrStringView value);

// html/#dom-script-supports: true for the types of script that the engine runs, "classic" and "module".
bool prHtmlScriptElementSupports(PrStringView type);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLStyleElement : HTMLElement (html)

// An HTMLStyleElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlStyleElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlStyleElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlStyleElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlStyleElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLStyleElement, or null if it is not one.
PrHtmlStyleElement* PR_NULLABLE prHtmlStyleElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the style sheet that the element's text defines, or null if it has none.
PrCssStyleSheet* PR_NULLABLE prHtmlStyleElementSheet(PrHtmlStyleElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTemplateElement : HTMLElement (html)

// An HTMLTemplateElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlTemplateElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlTemplateElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlTemplateElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlTemplateElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLTemplateElement, or null if it is not one.
PrHtmlTemplateElement* PR_NULLABLE prHtmlTemplateElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the template contents (html/#template-contents), which the element's document owns.
PrDomDocumentFragment* prHtmlTemplateElementContent(PrHtmlTemplateElement* element);

// Writes the state of the shadowrootmode attribute to `out` (html/#dom-template-shadowrootmode). Returns false if the
// attribute names neither open nor closed.
bool prHtmlTemplateElementShadowRootMode(const PrHtmlTemplateElement* element, PrDomShadowRootMode* out);

// Returns the shadowrootslotassignment attribute, limited to its known values
// (html/#dom-template-shadowrootslotassignment). Returns Named for a missing or invalid attribute.
PrDomSlotAssignmentMode prHtmlTemplateElementShadowRootSlotAssignment(const PrHtmlTemplateElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// ErrorEvent : Event (html)

// An ErrorEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlErrorEvent, DomEvent, Event)

// The members of ErrorEventInit (html/#erroreventinit): the error's message, the URL of the script it occurred in, and
// its line and column there.
typedef struct PrHtmlErrorEventInit {
    PrDomEventInit event;
    PrStringView message;
    PrStringView filename;
    uint32_t lineno;
    uint32_t colno;
} PrHtmlErrorEventInit;

// Returns an init with every member false, empty or zero.
static inline PrHtmlErrorEventInit prHtmlErrorEventInitDefault(void) {
    PrHtmlErrorEventInit value;
    value.event = prDomEventInitDefault();
    value.message = PR_SV("");
    value.filename = PR_SV("");
    value.lineno = 0;
    value.colno = 0;
    return value;
}

// Returns `object` as an ErrorEvent, or null if it is not one.
PrHtmlErrorEvent* PR_NULLABLE prHtmlErrorEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates an ErrorEvent of `type`. The caller holds the one reference. The event copies the strings of `init`.
PrHtmlErrorEvent* prHtmlErrorEventCreate(PrStringView type, const PrHtmlErrorEventInit* PR_NULLABLE init);

// Returns the members that the event was initialized with (PrHtmlErrorEventInit).
PrStringView prHtmlErrorEventMessage(const PrHtmlErrorEvent* event);
PrStringView prHtmlErrorEventFilename(const PrHtmlErrorEvent* event);
uint32_t prHtmlErrorEventLineno(const PrHtmlErrorEvent* event);
uint32_t prHtmlErrorEventColno(const PrHtmlErrorEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// MessageEvent : Event (html)
//
// An event carries its data as the bytes that a script serialized the message to (html/#structuredserialize). The data
// of an event without bytes is a value of the caller's language, which the binding holds. The source names a window but
// does not keep it alive.

// A MessageEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlMessageEvent, DomEvent, Event)

// The members of MessageEventInit (html/#messageeventinit), without the data and the ports. `origin` is the origin of
// the sender, `lastEventId` the ID of the last event of a server-sent event stream, and `source` the window that sent
// the message.
typedef struct PrHtmlMessageEventInit {
    PrDomEventInit event;
    PrStringView origin;
    PrStringView lastEventId;
    PrDomEventTarget* PR_NULLABLE source;
} PrHtmlMessageEventInit;

// Returns an init with every member false or empty, and no source.
static inline PrHtmlMessageEventInit prHtmlMessageEventInitDefault(void) {
    PrHtmlMessageEventInit value;
    value.event = prDomEventInitDefault();
    value.origin = PR_SV("");
    value.lastEventId = PR_SV("");
    value.source = NULL;
    return value;
}

// Returns `object` as a MessageEvent, or null if it is not one.
PrHtmlMessageEvent* PR_NULLABLE prHtmlMessageEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a MessageEvent of `type` that carries no bytes. The caller holds the one reference. The event copies the
// strings of `init`.
PrHtmlMessageEvent* prHtmlMessageEventCreate(PrStringView type, const PrHtmlMessageEventInit* PR_NULLABLE init);

// Returns the bytes that the message was serialized to, or the null view if the event carries none.
PrStringView prHtmlMessageEventSerializedData(const PrHtmlMessageEvent* event);

// Returns the members that the event was initialized with (PrHtmlMessageEventInit).
PrStringView prHtmlMessageEventOrigin(const PrHtmlMessageEvent* event);
PrStringView prHtmlMessageEventLastEventId(const PrHtmlMessageEvent* event);
PrDomEventTarget* PR_NULLABLE prHtmlMessageEventSource(PrHtmlMessageEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// PageRevealEvent : Event (html)
//
// The event that fires when a navigation reveals a document. Its view transition is always null.

// A PageRevealEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlPageRevealEvent, DomEvent, Event)

// Returns `object` as a PageRevealEvent, or null if it is not one.
PrHtmlPageRevealEvent* PR_NULLABLE prHtmlPageRevealEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a PageRevealEvent of `type`. The caller holds the one reference.
PrHtmlPageRevealEvent* prHtmlPageRevealEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init);

// ---------------------------------------------------------------------------------------------------------------------
// PromiseRejectionEvent : Event (html)
//
// The caller holds the promise and the reason, which are values of the caller's language.

// A PromiseRejectionEvent as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlPromiseRejectionEvent, DomEvent, Event)

// Returns `object` as a PromiseRejectionEvent, or null if it is not one.
PrHtmlPromiseRejectionEvent* PR_NULLABLE prHtmlPromiseRejectionEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a PromiseRejectionEvent of `type`. The caller holds the one reference.
PrHtmlPromiseRejectionEvent* prHtmlPromiseRejectionEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init);

// ---------------------------------------------------------------------------------------------------------------------
// TimerHandler (html)

// Called when the timer fires.
typedef void (*PrHtmlTimerHandlerFn)(void* PR_NULLABLE context);

// The function that a timer calls, with its context. A timer keeps a copy of its handler, and calls `retain` and
// `release` on the copy's context if they are given.
struct PrHtmlTimerHandler {
    PrHtmlTimerHandlerFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
};

// ---------------------------------------------------------------------------------------------------------------------
// FrameRequestCallback (html)

// Called at a rendering opportunity. `now` is the time of the frame, on the same clock as prHtmlPerformanceNow.
typedef void (*PrHtmlFrameRequestFn)(void* PR_NULLABLE context, double now);

// The function that an animation frame request calls, with its context. A request keeps a copy of its callback, and
// calls `retain` and `release` on the copy's context if they are given.
struct PrHtmlFrameRequestCallback {
    PrHtmlFrameRequestFn invoke;
    void* PR_NULLABLE context;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
};

// ---------------------------------------------------------------------------------------------------------------------
// PreferenceObject : EventTarget (mediaqueries)
//
// A user preference that a script can override (mediaqueries-5/#auto-pref). A query then sees the overridden value of
// its media feature. The window owns its preference objects.

// The preferences that a script can override, each named for its prefers- media feature.
enum {
    PrHtmlPreference_ColorScheme,
    PrHtmlPreference_Contrast,
    PrHtmlPreference_ReducedMotion,
    PrHtmlPreference_ReducedTransparency,
    PrHtmlPreference_ReducedData,
    PrHtmlPreference_Count,
};
typedef uint8_t PrHtmlPreference;

// The values of the preferences' media features, each named by its keyword, and None for no value.
enum {
    PrHtmlPreferenceValue_None,
    PrHtmlPreferenceValue_Light,
    PrHtmlPreferenceValue_Dark,
    PrHtmlPreferenceValue_NoPreference,
    PrHtmlPreferenceValue_More,
    PrHtmlPreferenceValue_Less,
    PrHtmlPreferenceValue_Custom,
    PrHtmlPreferenceValue_Reduce,
    PrHtmlPreferenceValue_Count,
};
typedef uint8_t PrHtmlPreferenceValue;

// Returns the keyword of `value`, or the empty string for None. The text lives as long as the process.
PrStringView prHtmlPreferenceValueName(PrHtmlPreferenceValue value);

// Returns the value whose keyword is `text`, or None if `text` matches no keyword.
PrHtmlPreferenceValue prHtmlPreferenceValueFromName(PrStringView text);

// A PreferenceObject as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlPreferenceObject, DomEventTarget, EventTarget)

// Returns `object` as a PreferenceObject, or null if it is not one.
PrHtmlPreferenceObject* PR_NULLABLE prHtmlPreferenceObjectQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the override, or None if there is none.
PrHtmlPreferenceValue prHtmlPreferenceObjectOverride(const PrHtmlPreferenceObject* preference);

// Returns the value that a query sees, which is the override if there is one.
PrHtmlPreferenceValue prHtmlPreferenceObjectValue(const PrHtmlPreferenceObject* preference);

// Writes up to `capacity` of the values that an override may take to `items`. Returns the total number of values.
uint32_t prHtmlPreferenceObjectValidValues(const PrHtmlPreferenceObject* preference,
    PrHtmlPreferenceValue* PR_NULLABLE items, uint32_t capacity);

// Removes any override and fires change (mediaqueries-5/#dom-preferenceobject-clearoverride).
void prHtmlPreferenceObjectClearOverride(PrHtmlPreferenceObject* preference);

// Overrides the preference with `value` and fires change, or clears the override for None
// (mediaqueries-5/#dom-preferenceobject-requestoverride). Returns InvalidData for a value that is not one of the valid
// values, and changes nothing.
PrStatus prHtmlPreferenceObjectRequestOverride(PrHtmlPreferenceObject* preference, PrHtmlPreferenceValue value);

// ---------------------------------------------------------------------------------------------------------------------
// Performance : EventTarget (hr-time)
//
// The clock of a window's scripts, which the window owns. Its times are milliseconds since the window's time origin
// (hr-time-3/#sec-time-origin). A new document resets the origin.

// A Performance as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlPerformance, DomEventTarget, EventTarget)

// Returns `object` as a Performance, or null if it is not one.
PrHtmlPerformance* PR_NULLABLE prHtmlPerformanceQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the current time, coarsened (hr-time-3/#dfn-coarsen-time) (hr-time-3/#dom-performance-now).
double prHtmlPerformanceNow(const PrHtmlPerformance* performance);

// Returns the time origin, in milliseconds since the Unix epoch (hr-time-3/#dom-performance-timeorigin).
double prHtmlPerformanceTimeOrigin(const PrHtmlPerformance* performance);

// ---------------------------------------------------------------------------------------------------------------------
// ScreenOrientation : EventTarget (screen-orientation)
//
// The orientation of a window's screen (screen-orientation/#screenorientation-interface), which the window owns. The
// screen's current orientation is its natural one, so the type follows the screen's extent and the angle is zero. The
// window fires change at the orientation in the rendering update after the type changes
// (screen-orientation/#dfn-screen-orientation-change-steps). A document cannot lock the orientation.

// The orientations of a screen (screen-orientation/#dom-orientationtype).
enum {
    PrHtmlOrientationType_PortraitPrimary,
    PrHtmlOrientationType_PortraitSecondary,
    PrHtmlOrientationType_LandscapePrimary,
    PrHtmlOrientationType_LandscapeSecondary,
};
typedef uint8_t PrHtmlOrientationType;

// Returns the keyword of `type`, as "landscape-primary". The text lives as long as the process.
PrStringView prHtmlOrientationTypeName(PrHtmlOrientationType type);

// A ScreenOrientation as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlScreenOrientation, DomEventTarget, EventTarget)

// Returns `object` as a ScreenOrientation, or null if it is not one.
PrHtmlScreenOrientation* PR_NULLABLE prHtmlScreenOrientationQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the screen's orientation (screen-orientation/#dom-screenorientation-type): landscape unless the screen is
// taller than it is wide.
PrHtmlOrientationType prHtmlScreenOrientationType(const PrHtmlScreenOrientation* orientation);

// Returns the screen's rotation from its natural orientation in degrees
// (screen-orientation/#dom-screenorientation-angle).
uint16_t prHtmlScreenOrientationAngle(const PrHtmlScreenOrientation* orientation);

// ---------------------------------------------------------------------------------------------------------------------
// DocumentTimeline : AnimationTimeline (web-animations)
//
// A document's timeline (web-animations-1/#document-timelines). Its time is the time of its window's latest rendering
// update minus the timeline's origin time. The timeline refers to its document, but does not keep it alive.

// Creates a timeline of `document` whose origin time is `originTime`, in milliseconds on the clock of
// prHtmlPerformanceNow. The caller owns the timeline.
PrHtmlDocumentTimeline* prHtmlDocumentTimelineCreate(PrDomDocument* document, double originTime);

// Frees the timeline.
void prHtmlDocumentTimelineDestroy(PrHtmlDocumentTimeline* PR_NULLABLE timeline);

// Writes the timeline's time to `out` (web-animations-1/#dom-animationtimeline-currenttime). Returns false if the
// timeline is inactive (web-animations-1/#inactive-timeline), such as the timeline of a document that no browsing
// context shows.
bool prHtmlDocumentTimelineCurrentTime(const PrHtmlDocumentTimeline* timeline, double* out);

// ---------------------------------------------------------------------------------------------------------------------
// Window : EventTarget (html)

// A Window as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlWindow, DomEventTarget, EventTarget)

// Returns `object` as a Window, or null if it is not one.
PrHtmlWindow* PR_NULLABLE prHtmlWindowQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the window's document.
PrDomDocument* prHtmlWindowDocument(PrHtmlWindow* window);

// Returns the agent whose event loop runs the window.
PrDomAgent* prHtmlWindowAgent(PrHtmlWindow* window);

// Sets the custom element registry of the window's scripts (html/#dom-window-customelements), or null for none. The
// registry defines the custom elements of each document that the window holds. The window refers to the registry from
// then on.
void prHtmlWindowSetCustomElementRegistry(PrHtmlWindow* window, const PrDomCustomElementRegistry* PR_NULLABLE registry);

// Returns the window's object for `preference`.
PrHtmlPreferenceObject* prHtmlWindowPreference(PrHtmlWindow* window, PrHtmlPreference preference);

// Returns the window's clock (hr-time-3/#the-performance-attribute).
PrHtmlPerformance* prHtmlWindowPerformance(PrHtmlWindow* window);

// Returns the orientation of the window's screen (screen-orientation/#dom-screen-orientation).
PrHtmlScreenOrientation* prHtmlWindowScreenOrientation(PrHtmlWindow* window);

// Returns the window whose document holds the iframe of this window's browsing context, or null for a top-level
// browsing context (html/#dom-parent).
PrHtmlWindow* PR_NULLABLE prHtmlWindowParent(PrHtmlWindow* window);

// Queues a message event at the window from `source` (html/#window-post-message-steps). The event's data is
// `serialized`, the bytes that a script serialized the message to. `targetOrigin` is "*" for any origin, "/" for the
// origin of `source`, or a URL whose origin the window's document must have. The event is dropped if the document's
// origin does not match. Returns Syntax if `targetOrigin` is none of those.
PrStatus prHtmlWindowPostMessage(PrHtmlWindow* window, PrHtmlWindow* source, PrStringView serialized,
    PrStringView targetOrigin);

// Returns a component of the URL of the window's document (html/#the-location-interface).
PrString prHtmlWindowLocation(const PrHtmlWindow* window, PrUrlComponent component);

// How a navigation changes the session history (html/#navigationhistorybehavior).
enum {
    PrHtmlHistoryHandling_Push,     // adds an entry after the current one, and drops the entries after it
    PrHtmlHistoryHandling_Replace,  // replaces the current entry
};
typedef uint8_t PrHtmlHistoryHandling;

// Sets a component of that URL by the rules of the setters of Location, as `handling` says. A URL that differs from the
// document's URL only in its fragment is a fragment navigation (html/#navigate-fragid): its entry becomes current, and
// popstate fires, then hashchange if the fragment changed. A navigation to the URL that the document has already
// replaces its entry. Any other URL navigates the window's page to another document from a task (prHtmlPageNavigate),
// whose entry the document's commit pushes or replaces. An href is parsed against the document's URL. Returns Syntax if
// it does not parse, and NotSupported for a URL of another document if the page's host has no fetcher.
PrStatus prHtmlWindowSetLocation(PrHtmlWindow* window, PrUrlComponent component, PrStringView value,
    PrHtmlHistoryHandling handling);

// Returns the first element in tree order that is a named object for `name` (html/#named-access-on-the-window-object),
// or null if there is none.
PrDomElement* PR_NULLABLE prHtmlWindowNamedItem(PrHtmlWindow* window, PrStringView name);

// Creates a query list for `query`. The caller holds the one reference. The window must outlive the list.
PrCssMediaQueryList* prHtmlWindowMatchMedia(PrHtmlWindow* window, PrStringView query);

// Returns the extent of the window's viewport (cssom-view-1/#dom-window-innerwidth). This is the viewport of its
// document's render, or of its media environment if the document has no presentation.
int32_t prHtmlWindowInnerWidth(PrHtmlWindow* window);
int32_t prHtmlWindowInnerHeight(PrHtmlWindow* window);

// Returns window.devicePixelRatio (cssom-view-1/#dom-window-devicepixelratio): the target pixels per CSS pixel that the
// window's document renders at, its display's pixel ratio. A resolution set as a media feature does not change it.
double prHtmlWindowDevicePixelRatio(PrHtmlWindow* window);

// Returns the extent of the window's screen in CSS pixels (cssom-view-1/#dom-screen-width), which its media
// environment's device-width and device-height report. The whole screen is available to the window
// (cssom-view-1/#dom-screen-availwidth).
int32_t prHtmlWindowScreenWidth(PrHtmlWindow* window);
int32_t prHtmlWindowScreenHeight(PrHtmlWindow* window);

// Returns the bits per pixel of the window's screen (cssom-view-1/#dom-screen-colordepth): the bits of each of its
// three color components, as its media environment's color reports them.
int32_t prHtmlWindowScreenColorDepth(PrHtmlWindow* window);

// Returns how far the viewport is scrolled (cssom-view-1/#dom-window-scrollx).
double prHtmlWindowScrollX(PrHtmlWindow* window);
double prHtmlWindowScrollY(PrHtmlWindow* window);

// Scrolls the viewport to the offsets that `options` gives, clamped to its range (cssom-view-1/#dom-window-scroll), or
// by those offsets (cssom-view-1/#dom-window-scrollby). A scroll that moves the viewport fires scroll and scrollend at
// the document at the next rendering update. A non-finite offset is treated as zero.
void prHtmlWindowScroll(PrHtmlWindow* window, const PrCssScrollToOptions* options);
void prHtmlWindowScrollBy(PrHtmlWindow* window, const PrCssScrollToOptions* options);

// Runs `handler` after `timeout` milliseconds, and every `timeout` milliseconds after that if it `repeats`, and retains
// a copy of it (html/#dom-settimeout, html/#dom-setinterval). Returns the timer's id. prHtmlWindowClearTimeout cancels
// the timer with that id and releases its handler, and does nothing if there is none. An interval that its handler
// cancels does not run again.
int32_t prHtmlWindowSetTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, int32_t timeout, bool repeats);
void prHtmlWindowClearTimeout(PrHtmlWindow* window, int32_t id);

// Runs `handler` after `milliseconds`, and retains a copy of it until then (html/#run-steps-after-a-timeout). No id
// names the timer, so prHtmlWindowClearTimeout does not cancel it. A timer that is due runs after the timers that were
// due before it, and after those set before it that are due at the same time.
void prHtmlWindowRunAfterTimeout(PrHtmlWindow* window, const PrHtmlTimerHandler* handler, double milliseconds);

// Runs `callback` at the window's next rendering opportunity, and retains a copy of it
// (html/#dom-animationframeprovider-requestanimationframe). The callbacks run together, one frame after the last
// opportunity. Returns the request's handle. prHtmlWindowCancelAnimationFrame cancels the request with that handle and
// releases its callback.
uint32_t prHtmlWindowRequestAnimationFrame(PrHtmlWindow* window, const PrHtmlFrameRequestCallback* callback);
void prHtmlWindowCancelAnimationFrame(PrHtmlWindow* window, uint32_t handle);

// Dispatches `event` at the window as a trusted event (html/#report-an-exception). `event` must be an ErrorEvent that
// the caller made cancelable. Returns true if no listener canceled it, which means that the exception still needs to be
// reported.
bool prHtmlWindowReportError(PrHtmlWindow* window, PrHtmlErrorEvent* event);

// Dispatches `event` at the window as a trusted event (html/#notify-about-rejected-promises). Returns true if no
// listener canceled it, which means that the rejection still needs to be reported.
bool prHtmlWindowNotifyRejection(PrHtmlWindow* window, PrHtmlPromiseRejectionEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// Navigator (html)
//
// The Navigator of a window (html/#the-navigator-object), which describes the user agent to the window's scripts. Its
// values come from the window: the default User-Agent value of the window's fetcher, and the navigator compatibility
// mode of the window's host (PrHtmlHost::navigatorCompatibilityMode).

// The navigator compatibility mode (html/#concept-navigator-compatibility-mode), which selects among the values that
// HTML gives appVersion, productSub and vendor. A host chooses the mode that its User-Agent string claims.
enum {
    PrHtmlNavigatorCompatibilityMode_Chrome,
    PrHtmlNavigatorCompatibilityMode_Gecko,
    PrHtmlNavigatorCompatibilityMode_WebKit,
};
typedef uint8_t PrHtmlNavigatorCompatibilityMode;

// Returns the navigator compatibility mode of the window.
PrHtmlNavigatorCompatibilityMode prHtmlNavigatorCompatibilityMode(const PrHtmlWindow* window);

// Returns the default User-Agent value of the fetcher of the window's resources (prFetchNetworkUserAgent), which is
// navigator.userAgent (html/#dom-navigator-useragent), or the empty view for a window without a fetcher. The view is
// valid while the window lives.
PrStringView prHtmlNavigatorUserAgent(const PrHtmlWindow* window);

// Returns navigator.appVersion (html/#dom-navigator-appversion): the window's User-Agent after its "Mozilla/" prefix,
// or the empty string for a User-Agent that does not start with "Mozilla/5.0 (". PrHtmlNavigatorCompatibilityMode_Gecko
// shortens it to its platform. The caller releases the string with prStringReset.
PrString prHtmlNavigatorAppVersion(const PrHtmlWindow* window);

// Returns navigator.platform (html/#dom-navigator-platform), which names the system that the engine runs on. Each
// system has one string, which pages compare against to tell the systems apart.
PrStringView prHtmlNavigatorPlatform(const PrHtmlWindow* window);

// Return navigator.productSub (html/#dom-navigator-productsub) and navigator.vendor (html/#dom-navigator-vendor), as
// the window's navigator compatibility mode selects them.
PrStringView prHtmlNavigatorProductSub(const PrHtmlWindow* window);
PrStringView prHtmlNavigatorVendor(const PrHtmlWindow* window);

// ---------------------------------------------------------------------------------------------------------------------
// Rendering (html)

// Returns the user agent style sheets for HTML documents, and writes their number to `count`. Most of the rules come
// from HTML's rendering section (html/#rendering). The sheets live as long as the process.
const PrCssUserAgentSheet* prHtmlUserAgentSheets(uint32_t* count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
