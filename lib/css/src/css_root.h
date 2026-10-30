// The CSS state of a document or a shadow root, and of an element
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
// css-properties-values-api-1 - https://drafts.css-houdini.org/css-properties-values-api-1/

#pragma once

#include "pr/css_intersection_observer.h"
#include "pr/css_resize_observer.h"
#include "pr/dom.h"
#include "pr/string_view.h"

#include "css/src/css_font_face.h"
#include "css/src/css_highlight.h"
#include "css/src/css_object.h"
#include "css/src/css_owner.h"
#include "css/src/css_pseudo.h"
#include "css/src/css_style.h"
#include "css/src/css_syntax_definition.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssBoxProvider;
struct CssHintProvider;
struct CssGivenHints;

// The namespaces that a document's providers are kept by: those up to SVG's, which take in MathML's.
constexpr uint32_t kCssProviderNamespaces = PrDomNamespace_Svg + 1;
struct CssInlineStyle;

// The computed style of an element or one of its pseudo-elements as declarations
// (cssom-1/#dom-window-getcomputedstyle). The presentation of the element's document fills the block as the
// declarations are read (dom_computed_style.h).
struct CssComputedDeclarations {
    CssDeclarationBlock block;
    PrCssStyleDeclaration declaration;
    const void* PR_NULLABLE filler;   // the presentation that filled the block, at `version`
    uint64_t version;
    CssPseudo pseudo;                 // what the style is of; CssPseudo_Count for nothing, which stays empty
};

// The CSS state of an element, created on first use.
struct CssElementStyle {
    CssInlineStyle* PR_NULLABLE inlineStyle;        // created on first use
    CssDeclarationBlock* PR_NULLABLE hints;         // presentational hints (css_hints.h), or null for none
    bool hintsBuilt;                                // true if `hints` reflects the attributes that it reads
    CssGivenHints* PR_NULLABLE given;               // hints that the element gives other elements (css_hints.h)

    // The computed styles of the element and its pseudo-elements, by CssComputedDeclarations::pseudo, each created on
    // first use.
    CssComputedDeclarations* PR_NULLABLE computed[CssPseudo_Count + 1];

    PrCssStyleSheet* PR_NULLABLE sheet;             // sheet of a style or link element, which owns it (css_object.h)
    CssRoot* PR_NULLABLE scope;                     // list of the document or shadow root that holds the sheet
    bool updatingAttribute;                         // true if the inline style is writing the style attribute

    // The steps of the sheet's owner once its sheets settle (cssOwnerFetch), or null for none.
    CssOwnerSettledFn* PR_NULLABLE settled;

    // Incremented each time the sheet of a style or link element is made again, so that the result of a fetch for an
    // earlier sheet is dropped.
    uint32_t generation;

    // The number of fetches that the sheet waits for: a link's sheet itself, and the sheets that it imports, at any
    // depth.
    uint32_t fetching;

    bool importFailed;                              // an import of the sheet did not load
    bool blocking;                                  // the element is in the document's set of blocking sheets
    bool blocksRendering;                           // it also blocks rendering, because the parser added it to a head
};

// Returns the element's style data, creating it on first use.
CssElementStyle* cssElementStyle(PrDomElement* element);

// Frees the element's style data, and the sheet that the element owns.
void cssElementStyleDestroy(CssElementStyle* PR_NULLABLE style);

// A custom property registered by CSS.registerProperty (css-properties-values-api-1/#registered-custom-property).
struct CssRegisteredProperty {
    PrString name;
    PrString syntax;                  // syntax as it was given
    PrString initialValue;            // the null string if none was given
    CssSyntaxDefinition definition;   // result of parsing `syntax`
    bool inherits;
};

// The functions through which CSS reaches the presentation of a document. The presentation gives them to the document's
// CSS state while it presents the document, and each takes the presentation.
struct CssPresentationHooks {
    // Something that every element's style depends on changed: the document's sheets, or its default language. Or a
    // rule of one of its sheets was inserted, removed, or had its declarations changed.
    void (*styleChanged)(void* presentation);
    void (*ruleChanged)(void* presentation, const CssRuleData* rule);

    // Return the element's color context and its computed style once the presentation's styles are brought up to date,
    // or the initial values and null for an element that the presentation does not style. Called outside a restyle.
    CssColorContext (*colorContext)(void* presentation, const PrDomElement* element);
    const CssComputedStyle* PR_NULLABLE (*PR_NONNULL styleCurrent)(void* presentation, const PrDomElement* element);

    // Returns true if the presentation will request fonts at its next layout, which keeps the document's FontFaceSet
    // pending on the environment (css-font-loading-3/#fontfaceset-pending-on-the-environment).
    bool (*awaitsFontRequests)(const void* presentation);
};

// The CSS state of a document or a shadow root, created on first use.
struct CssRoot {
    // The CSS style sheets of the document or shadow root
    // (cssom-1/#documentorshadowroot-document-or-shadow-root-css-style-sheets), in tree order of their owner nodes.
    PrCssStyleSheetList sheets;

    // The registered custom properties (css-properties-values-api-1/#dom-window-registeredpropertyset-slot), in the
    // order in which they were registered.
    Array<CssRegisteredProperty> registered;

    PrCssFontFaceSet* PR_NULLABLE fonts;            // css-font-loading-3/#document-font-face-set, created on first use
    PrCssFontCallback fontCallback;                 // notified of font loads, or a null faceStatus for none
    uint64_t fontsVersion;                          // incremented when a face's status or the set's faces change
    uint64_t fontSetVersion;                        // incremented when the faces added to the set change
    bool fontsReconcileQueued;                      // true if a task will bring the set up to date with its source
    Array<CssFontSource* PR_NONNULL> fontSources;   // sources that the faces load from, each holding a reference
    uint64_t fontSourceUses;                        // number of loads that used a source
    PrCssHighlightRegistry* PR_NULLABLE highlights; // css-highlight-api-1/#highlight-registry, created on first use

    // For a document, its resize observers in the order that they were created
    // (resize-observer-1/#dom-document-resizeobservers-slot). The list holds no references. Each observer holds a
    // reference to the document, and removes itself from the list when it is freed.
    Array<PrCssResizeObserver* PR_NONNULL> resizeObservers;
    bool resizeObservationsPending;                 // the next rendering update has an observation to deliver

    // For a document, its intersection observers in the order that they were created. The list holds no references.
    // Each observer holds a reference to the document, and removes itself from the list when it is freed.
    Array<PrCssIntersectionObserver* PR_NONNULL> intersectionObservers;
    bool intersectionTaskQueued;                    // intersection-observer/#document-intersectionobservertaskqueued

    // The sheets of the document's style elements that import no sheet, kept for reuse, least recently used first.
    Array<CssSheetCacheEntry> cache;

    // For a document, the sheets of its xml-stylesheet processing instructions, which they own
    // (cssXmlStylesheetInserted).
    Array<PrCssStyleSheet* PR_NONNULL> instructionSheets;

    // For a document, the style and link elements that its parser inserted and whose sheets it still fetches. They
    // block its scripts (html/#script-blocking-style-sheet-set) and its rendering (html/#render-blocking-element-set).
    Array<PrDomElement* PR_NONNULL> blocking;

    // For a document, the providers of the hints of its elements (css_hints.h) and of what they give their boxes
    // (css_boxes.h), by namespace, or null for none.
    const CssHintProvider* PR_NULLABLE hintProviders[kCssProviderNamespaces];
    const CssBoxProvider* PR_NULLABLE boxProviders[kCssProviderNamespaces];

    // For a document, its presentation and the hooks that the presentation gives it, or null if it has none.
    const CssPresentationHooks* PR_NULLABLE presentationHooks;
    void* PR_NULLABLE presentation;
};

// Returns the document's style data, creating it on first use.
CssRoot* cssDocumentRoot(PrDomDocument* document);

// Frees the style data. The owner nodes of the sheets still own them.
void cssRootDestroy(CssRoot* PR_NULLABLE style);

// Returns the document's list of style sheets.
PrCssStyleSheetList* cssDocumentStyleSheets(PrDomDocument* document);

// Registers the custom property `name` (css-properties-values-api-1/#register-a-custom-property). Returns Syntax for a
// name that is not a custom property name, a syntax that is not a syntax string, or an initial value that the syntax
// does not accept. Returns NotAllowed for a name that is already registered. A null `initialValue` means that none was
// given.
PrStatus cssRegisterProperty(PrDomDocument* document, PrStringView name, PrStringView syntax,
    bool inherits, PrStringView initialValue);

// Tells the presentation of the document that something that every element's style depends on has changed: its sheets,
// or its default language. Brings the document's FontFaceSet up to date with its sheets.
void cssDocumentStyleChanged(const PrDomDocument* PR_NULLABLE document);

// Tells the presentation of the document that a rule of one of its sheets was inserted, removed, or had its
// declarations changed. Brings the document's FontFaceSet up to date with its sheets.
void cssDocumentRuleChanged(const PrDomDocument* PR_NULLABLE document, const CssRuleData* rule);

// Returns the changes to a document that CSS follows (dom_hooks.h), to which prCssEnable subscribes it.
const DomDocumentHooks* cssDocumentHooks();

} // namespace pr_impl

PR_ASSUME_NONNULL_END
