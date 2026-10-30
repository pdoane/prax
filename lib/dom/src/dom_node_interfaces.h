// The node tree: the interfaces that derive Node directly.
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/byte_view.h"
#include "pr/charset_names.h"
#include "pr/compress.h"
#include "pr/dom.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_node_base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the hash by which an attribute is found from its name (DomAttr::hash). For a name known at compile time, the
// hash is computed at compile time.
constexpr uint32_t domAttributeHash(PrStringView name) {
    return uint32_t(nameHashLower(name.data, name.size, kNameSeed));
}

// An attribute (dom/#interface-attr).
struct DomAttr : PrDomNode {
    PrString name;                    // the local name
    PrString value;
    uint32_t hash;                    // domAttributeHash(name), for lookups that fold case and those that do not
    uint8_t prefix;                   // namespace prefix as 1 + its index in DomDocument::prefixes, or 0 for none
};

// An element's local name as a number, for comparing names. For a name that a spec defines as an element of its
// namespace, the number is the PrDomTag. From PrDomTag_Count up, the number is an atom of the document's local names.
typedef uint32_t DomTagId;

// An element (dom/#interface-element).
struct DomElement : PrDomNode {
    PrDomAttr* PR_NULLABLE firstAttribute;   // attributes, in the order they were added
    PrDomAttr* PR_NULLABLE lastAttribute;
    CssElementStyle* PR_NULLABLE style; // HTMLElement and HTMLStyleElement style data
    DomElementState* PR_NULLABLE state;
    uint32_t classes;                 // classes (dom/#concept-class) as a list of the document's, or kDomClassesNone
    DomTagId tag;                     // local name
};

// The base of the nodes that hold data (dom/#interface-characterdata).
struct DomCharacterData : PrDomNode {
    PrString data;
};

// A document fragment (dom/#documentfragment).
struct DomDocumentFragment : PrDomNode {
    // The host (dom/#concept-documentfragment-host). This is the template whose contents the fragment is, or the host
    // of a shadow root. The host owns the fragment.
    PrDomElement* PR_NULLABLE host;
};

// A doctype (dom/#interface-documenttype).
struct DomDocumentType : PrDomNode {
    PrString name;
    PrString publicId;                // the null string if the doctype has none
    PrString systemId;                // the null string if the doctype has none
};

// A document's mode (dom/#interface-document).
enum {
    DomQuirks_No,
    DomQuirks_Yes,
    DomQuirks_Limited,
};
typedef uint8_t DomQuirks;

// The purpose of a fetched resource (fetch/#concept-request-destination).
enum {
    DomFetchDestination_Document,
    DomFetchDestination_Script,
    DomFetchDestination_Style,
    DomFetchDestination_Image,
    DomFetchDestination_Font,
    DomFetchDestination_Media,
    DomFetchDestination_Object,
    DomFetchDestination_Empty,      // a script's fetch()
};
typedef uint8_t DomFetchDestination;

// A fetch that a document asks of its browsing context (html/#create-a-potential-cors-request): the absolute URL of the
// resource, without its fragment, what it is for, and the CORS settings that decide its mode and credentials. A fetch
// for an element takes the element's CORS settings attribute. A frame's or an object's document is fetched as a
// navigation, whatever its settings.
struct DomFetchRequest {
    PrStringView url;
    DomFetchDestination destination;
    DomCorsSettings cors;
};

// The result of a fetch: the resource's bytes and its MIME type, or a network error for a status other than Ok. The
// MIME type is the null view if none is known. The views are valid during the call that receives the result.
struct DomFetchResult {
    PrStatus status;
    uint16_t responseStatus;        // the response's status (fetch/#concept-response-status), or 0 for a network error
    PrStringView body;
    PrStringView contentType;       // the Content-Type header, or the null view for none
    PrStringView contentLanguage;   // the Content-Language header, or the null view for none
    PrStringView url;               // the response's URL after redirects, without its fragment, or the null view
    bool opaque;                    // true for an opaque response (fetch/#concept-filtered-response-opaque)
};

// Returns true if the fetch received a response whose status is ok (fetch/#ok-status).
inline bool domFetchIsOk(const DomFetchResult* result) {
    return result->status == PrStatus_Ok && result->responseStatus >= 200 && result->responseStatus < 300;
}

// Called once with the result of a fetch.
typedef void (*DomFetchFn)(void* PR_NULLABLE data, const DomFetchResult* result);

// An image that a document's browsing context decodes for a style (DomDocumentHost::decodeImage). One of `image` and
// `vector` is set.
struct DomDecodedImage {
    // The decoded image, arranged as its orientation displays it, and how it was arranged from its stored pixels.
    const PrDrawImage* PR_NULLABLE image;
    PrImageOrientation orientation;

    PrLayoutPainter vector;                     // an SVG image, which draws at the size it is shown; or no paint
    PrLayoutNaturalSize natural;                // as the image's orientation displays it
};

// The services that a document's browsing context provides for it (html/#fetching-resources, html/#queue-a-task). A
// document with no browsing context has no host, and fetches nothing.
struct DomDocumentHost {
    void* PR_NULLABLE context;

    // Fetches what `request` asks for, and calls `done` with `data` and the result. The call to `done` happens during
    // this call if the result is already available, and otherwise in a task once it arrives. The document's load event
    // waits for the fetch, except for a font or media. If the browsing context drops the fetch first, it calls `drop`
    // with `data` instead, if `drop` is given.
    void (*PR_NONNULL fetch)(void* PR_NULLABLE context, const DomFetchRequest* request, DomFetchFn done,
        void* PR_NULLABLE data, void (*PR_NULLABLE drop)(void* PR_NULLABLE data));

    // The codec that decompresses the compressed fonts that the document loads, or null for none.
    PrCompressCodec* PR_NULLABLE compress;

    // Decodes the image `bytes`, which a style names, into what draws it: a decoded image, or an SVG image's painter,
    // with a reference for the caller in either case. `fragment` is the fragment of the URL that named the image,
    // without its `#`, or the null view, which an SVG image takes its view from. Writes its natural dimensions, which
    // an SVG image may lack. Returns false if the bytes are not an image that the browsing context decodes. Null if the
    // browsing context decodes no images.
    bool (*PR_NULLABLE decodeImage)(void* PR_NULLABLE context, PrByteView bytes, PrStringView fragment, DomDecodedImage* out);

    // Brings the document's presentation up to date, as a read of its layout needs: its styles and boxes, the content
    // that the browsing context draws in them, such as that of svg elements, and their layout. Null if the
    // presentation's own update suffices (cssPresentationFlush).
    void (*PR_NULLABLE updatePresentation)(void* PR_NULLABLE context);

    // Writes the extent of the document's viewport in CSS pixels as it is now, and returns true. First brings up to
    // date what decides the extent: for a frame's document, the parent document's layout of the iframe. Returns false
    // if the viewport is the one that the document was last laid out in, as for a document whose viewport the host sets
    // when it paints. Null for a host that always returns false.
    bool (*PR_NULLABLE viewport)(void* PR_NULLABLE context, PrVec2* out);

    // Tells the browsing context that the document's sheets that block scripts or rendering have changed. A script that
    // waits for them may then run, or the document's rendering may start.
    void (*PR_NONNULL sheetsChanged)(void* PR_NULLABLE context);

    // Runs `run` with `data` in a task of its own, after the current one. If the task is discarded without running,
    // calls `drop` with `data` instead. If `drop` is null, nothing is done in that case.
    void (*PR_NONNULL queueTask)(void* PR_NULLABLE context, void (*PR_NONNULL run)(void* data),
        void* data, void (*PR_NULLABLE drop)(void* data));

    // Runs `run` in the same way as queueTask, in a task that the document's load event waits for
    // (html/#delay-the-load-event).
    void (*PR_NONNULL queueDelayingTask)(void* PR_NULLABLE context, void (*PR_NONNULL run)(void* data), void* data,
        void (*PR_NULLABLE drop)(void* data));

    // Runs `run` with `data` in a microtask (html/#queue-a-microtask). Returns false, and leaves `data` with the
    // caller, if the host runs no microtasks.
    bool (*PR_NONNULL queueMicrotask)(void* PR_NULLABLE context, void (*PR_NONNULL run)(void* data), void* data);

    // Compiles `body` into the callback of the event handler `handler` of `target`
    // (html/#getting-the-current-value-of-the-event-handler). `body` is the text of the event handler content attribute
    // of `element`. `out` holds one reference to the callback. Returns false, with the error reported, if `body` does
    // not compile.
    bool (*PR_NONNULL compile)(void* PR_NULLABLE context, PrDomEventTarget* target,
        PrDomElement* element, PrDomEventHandler handler, PrStringView body, PrDomEventHandlerCallback* out);

    // Called when an element for which domEmbeds is true is connected or disconnected, with the null view for
    // `attribute`, or when the element's attribute `attribute` changes.
    void (*PR_NONNULL embedChanged)(void* PR_NULLABLE context, PrDomElement* element, PrStringView attribute);

    // Prepares a script element that is not parser-inserted (html/#prepare-the-script-element), as its post-connection
    // steps do: once an insertion that connects it ends, when its children change while it is connected, and when its
    // source attribute is set while it is connected (html/#script-processing-model). Null for a host that runs no
    // scripts.
    void (*PR_NULLABLE prepareScript)(void* PR_NULLABLE context, PrDomElement* element);

    // Returns the states among `wanted` that the element is in, as the application knows them. Null if the application
    // reports no states.
    PrDomHostState (*PR_NULLABLE elementState)(void* PR_NULLABLE context, const PrDomElement* element, PrDomHostState wanted);

    // Returns true if the element has activation behavior (dom/#eventtarget-activation-behavior), which a click
    // dispatched at it or at a descendant runs. Null if no element has activation behavior.
    bool (*PR_NULLABLE activates)(void* PR_NULLABLE context, const PrDomElement* element);

    // Runs the element's activation behavior for `event`, which is a click that was dispatched at the element or at a
    // descendant and that nothing canceled.
    void (*PR_NULLABLE activate)(void* PR_NULLABLE context, PrDomElement* element, PrDomEvent* event);
};

// The type of a document (dom/#concept-document-type).
enum {
    DomDocumentKind_Xml,
    DomDocumentKind_Html,
};
typedef uint8_t DomDocumentKind;

// The readiness of a document (html/#current-document-readiness).
enum {
    DomReadiness_Complete,      // a document that no parser is loading
    DomReadiness_Loading,
    DomReadiness_Interactive,
};
typedef uint8_t DomReadiness;

// A subscriber of a document (dom_hooks.h): the changes that it follows, and the context that it takes.
struct DomDocumentSubscriber {
    const DomDocumentHooks* PR_NULLABLE hooks;
    void* PR_NULLABLE context;
};

// The number of subscribers that a document has room for: CSS, HTML, SVG, the document's presentation, and what SVG
// draws in the presentation.
constexpr uint32_t kDomDocumentSubscribers = 5;

// The number of entries of DomDocumentHooks (DomHook_Count).
constexpr uint32_t kDomDocumentHooks = 21;

// A document (dom/#interface-document).
struct DomDocument : PrDomNode {
    PrUrl* url;                       // dom/#concept-document-url

    // The content type (dom/#concept-document-content-type). It is an external string if the engine names the type, and
    // otherwise a copy of the type that a load or a caller gave.
    PrString contentType;

    // The sources of the default language (html/#pragma-set-default-language): the tag that the last content-language
    // pragma set, and the first tag of the Content-Language header that the document was loaded with. Each is the null
    // string if nothing gave one.
    PrString languagePragma;
    PrString languageHeader;

    PrDomNode* PR_NULLABLE detached;                            // roots of the document's other trees
    DomDocumentSubscriber subscribers[kDomDocumentSubscribers]; // what follows its changes (dom_hooks.h)
    CssRoot* PR_NULLABLE style;
    DomIds* PR_NULLABLE ids;           // created by the first connected element with an ID
    DomClasses* PR_NULLABLE classes;   // classes of the elements, created by the first class attribute
    AtomTable* PR_NULLABLE localNames; // local names of elements that no tag names, created by the first such name

    // The selector lists that the document's queries parsed, created on first use (css_selector_match.h).
    mutable CssSelectorCache* PR_NULLABLE selectors;

    // The environment of the browsing context, which the document's media queries are evaluated against.
    const CssMediaEnvironment* PR_NULLABLE media;

    // The Window of the document. An event reaches the Window after the document (html/#the-window-object).
    PrDomEventTarget* PR_NULLABLE global;

    const DomDocumentHost* PR_NULLABLE host;
    AtomTable* PR_NULLABLE namespaces;              // URLs of the namespaces from PrDomNamespace_Other on, if any
    AtomTable* PR_NULLABLE prefixes;                // prefixes that the document's attributes have, if any
    Array<PrDomNode* PR_NONNULL> holds;             // nodes that the engine holds (domNodeHold), each listed once
    Array<PrDomElement* PR_NONNULL> formAttributed; // listed elements with a form attribute (dom_control.h)
    DomMutationObservers* PR_NULLABLE observers;    // created by the first registration
    DomSlots* PR_NULLABLE slots;                    // created by the first shadow root
    DomUses* PR_NULLABLE uses;                      // created by the first connected use element (dom_use.h)
    DomResources* PR_NULLABLE resources;            // created by the first reference to another document
                                                    // (dom_resource.h)
    PrDomRange* PR_NULLABLE ranges;                 // live ranges (dom/#concept-live-range), kept current by the
                                                    // document's mutations
    PrDomSelection* PR_NULLABLE selection;          // created on first access

    // The registry of the document's window (dom/#document-custom-element-registry).
    const PrDomCustomElementRegistry* PR_NULLABLE customElements;

    // The focused area of the document (html/#focused-area-of-the-document) if it is an element, or null if it is the
    // viewport.
    PrDomElement* PR_NULLABLE focused;

    // The sequential focus navigation starting point (html/#sequential-focus-navigation-starting-point), or null if it
    // is unset. It is an element, or the position just after an element in tree order if `navigationStartAfter` is set.
    // Focus moves it to the element focused, and a press to the element pressed. When the tree loses the node that
    // holds it, the starting point moves to where that node was, after the element that preceded the node.
    PrDomElement* PR_NULLABLE navigationStart;

    // The elements with an autofocus attribute inserted into the document, in order, which its top-level document has
    // yet to flush (html/#autofocus-candidates). An element that leaves the document leaves the list. The top-level
    // document gathers the candidates of its frames' documents with its own.
    Array<PrDomElement* PR_NONNULL> autofocusCandidates;

    PrDomElement* PR_NULLABLE hovered;  // connected element that the user designates (html/#selector-hover)
    PrDomElement* PR_NULLABLE active;   // connected element being activated (html/#selector-active)
    PrDomElement* PR_NULLABLE target;   // connected element that the URL's fragment indicates (html/#target-element)

    // Incremented by each change to the children of the document's nodes and to the attributes of its elements. Live
    // collections and lists of its nodes answer from their caches until the version changes.
    uint64_t treeVersion;

    PrDomTreeCallback treeCallback;   // notified when the document's nodes move or are held
    DomDocumentKind kind;
    DomReadiness readiness;
    PrCharset charset;                      // dom/#concept-document-encoding
    uint32_t throwOnDynamicMarkupInsertion; // html/#throw-on-dynamic-markup-insertion-counter
    uint32_t baseElements;                  // number of connected HTML base elements, including those in shadow trees

    // True if the focused element shows that it has the focus (selectors-4/#the-focus-visible-pseudo).
    bool focusVisible;

    // True if the last input that the document took was a press, and false after a key.
    bool pressInput;

    bool autofocusProcessed;    // html/#autofocus-processed-flag, for a top-level document
    bool navigationStartAfter;  // the starting point is just after `navigationStart`, where a removed node was

    // True if the document has created an HTML option or selectedcontent element. From then on, changes to its trees
    // run the steps of those elements (dom_select.h).
    bool options;

    // True if the document has created a form-associated element or a form. From then on, changes to its trees run the
    // steps of controls (dom_control.h).
    bool controls;

    DomQuirks quirks;

    // By DomHook, the bits of the subscribers that have the entry (dom_hooks.h).
    uint8_t followers[kDomDocumentHooks];
};

} // namespace pr_impl

// C opaque types
struct PrDomAttr : pr_impl::DomAttr {};
struct PrDomElement : pr_impl::DomElement {};
struct PrDomCharacterData : pr_impl::DomCharacterData {};
struct PrDomDocumentFragment : pr_impl::DomDocumentFragment {};
struct PrDomDocumentType : pr_impl::DomDocumentType {};
struct PrDomDocument : pr_impl::DomDocument {};
struct PrDomImplementation : PrDomDocument {};

PR_ASSUME_NONNULL_END
