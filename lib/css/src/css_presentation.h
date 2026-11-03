// Rendering documents: the computed style of each element, projected onto a layout tree.
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// A presentation keeps an entry for each node of its document that it styled or presents. The entry holds an element's
// computed style, and the layout node of a node that generates a box. The document marks its nodes (DomNode::style) as
// they change, and an update visits only the marked nodes. It restyles an element that changed, the descendants that a
// change reaches, and the children of an element whose inherited values changed. It changes the layout tree only as
// much as those restyles require.

#pragma once

#include "pr/array.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/hash_index.h"
#include "pr/layout_worker.h"
#include "pr/mutex.h"
#include "pr/string_view.h"
#include "pr/task.h"
#include "pr/text_buffer.h"

#include "css/src/css_animate.h"
#include "css/src/css_cascade.h"
#include "css/src/css_presentation_svg.h"
#include "dom/src/dom_agent.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomNode;

// What applying the box operations that a restyle queued does to an entry's box, besides making or erasing it.
enum {
    CssPresentationBoxOp_Style = 1u << 0,    // sets the box's record from the entry's style
    CssPresentationBoxOp_Text = 1u << 1,     // sets the box's text from the node's data
    CssPresentationBoxOp_Children = 1u << 2, // puts the boxes of the children under the box, in order
    CssPresentationBoxOp_Queued = 1u << 3,   // the entry's node is in the queue
    CssPresentationBoxOp_Content = 1u << 4,  // sets a replaced box's natural size and content from its element

    // Erases the box and makes it again, because the kind of box that the node generates has changed.
    CssPresentationBoxOp_Remake = 1u << 5,
};
typedef uint8_t CssPresentationBoxOps;

// The directionality that an element was last styled with (html/#the-directionality).
enum {
    CssPresentationDirection_Unknown,
    CssPresentationDirection_Ltr,
    CssPresentationDirection_Rtl,
};
typedef uint8_t CssPresentationDirection;

// The state of an image that a style names.
enum {
    CssPresentationImageState_Loading,
    CssPresentationImageState_Loaded,

    // The fetch failed, or the bytes are not an image that the browsing context decodes.
    CssPresentationImageState_Broken,
};
typedef uint8_t CssPresentationImageState;

// Where the gradient stops and filter items of a projected image start in the presentation's scratch arrays. The arrays
// move as they grow, so an image's pointers into them are set once every image of a value is resolved.
struct CssPresentationImageSpans {
    uint32_t stops;
    uint32_t filters;
};

// An image that a style names by URL (css-images-3/#url-notation), which paints once it has loaded.
struct CssPresentationImage {
    PrString url;                               // absolute
    CssPresentationImageState state;

    // The nodes whose records named it while it loaded.
    Array<const PrDomNode*> waiting;

    // The decoded image, arranged as its orientation displays it, and how it was arranged from its stored pixels. The
    // presentation holds a reference.
    const PrDrawImage* PR_NULLABLE image;
    PrImageOrientation orientation;

    // The decoded image as it is stored, made the first time that image-orientation asks for it. The presentation holds
    // a reference.
    const PrDrawImage* PR_NULLABLE stored;

    PrLayoutPainter vector;                     // an SVG image, whose context the presentation holds a reference to
    PrLayoutNaturalSize natural;                // as the image's orientation displays it
};

// How an element draws the images that it shows.
struct CssPresentationImageDrawing {
    float limit;                                // the cap under its dynamic-range-limit, or zero for none
    PrLayoutImageRendering rendering;           // the resampling that its image-rendering asks for
    bool oriented;                              // true if its image-orientation applies each image's orientation

    bool operator==(const CssPresentationImageDrawing&) const = default;
};

// A fetch of an image for a presentation. The presentation is null once it is destroyed.
struct CssPresentationImageFetch {
    PrCssPresentation* PR_NULLABLE presentation;
    uint32_t image;                             // index into PrCssPresentation::images
};

// The texts that counters give the content of a pseudo-element: the text of each counter() and counters() of its
// content, in order. For a marker whose content is normal, the only text is its marker string.
struct CssCountingTexts {
    Array<char> text;
    Array<uint32_t> ends;           // the end of each text in `text`
};

// A pseudo-element whose box the presentation generates (css-pseudo-4/#generated-content), as a node that the
// presentation owns. Its parent is its originating element, or the ::before or ::after whose marker it is, and no
// children list it, so neither the document nor a script reaches it. The presentation keys its entry and its box by it,
// as it does an element's, so that a point on its box or on the anonymous boxes of its content hits the originating
// element.
struct CssPresentationPseudo : PrDomNode {
    CssPseudo pseudo;
    CssCountingTexts counters;    // as the last counter pass set them

    // For a marker that shows its marker string: true if the pass made the string from `markerValue`, the list-item
    // value, so that a pass that finds the same value keeps the string. A restyle of the marker clears the flag, since
    // the marker's list style may have changed.
    bool markerKept;
    int32_t markerValue;

    bool outside;                   // for a marker: whether its record was last projected as an outside marker's
};

// The depth of a node among the boxes that the counter pass walks: 0 for the root, and one more for each element around
// it that has a box of its own.
typedef uint32_t CssCountingDepth;

// A counter in the scope of the node that the counter pass is at (css-lists-3/#css-counters-set). Its name is a view of
// the computed value that instantiated it, which outlives the pass.
struct CssCounting {
    PrStringView name;
    bool reversed;
    int32_t value;

    // The depth of the node that instantiated the counter. Its scope ends where the pass leaves that node's parent, or
    // that node itself if `narrow` is set.
    CssCountingDepth depth;
    bool narrow;
};

// A step of the counter pass, in tree order: a node that may change or read counters, or the end of an element's
// subtree.
struct CssCountingStep {
    const PrDomNode* PR_NULLABLE node;          // an element or a pseudo-element, or null for the end of a subtree
    const CssComputedStyle* PR_NULLABLE style;  // the node's style
    CssCountingDepth depth;                     // the node's depth, or the depth of the element whose subtree ends
    bool listItem;                              // the node is a list item (CssPresentationEntry::listItem)
};

// The subtree of an element with style containment, inside which counter-increment and counter-set do not reach the
// counters from outside (css-contain-2/#containment-style).
struct CssCountingBoundary {
    CssCountingDepth depth;       // the element's depth

    // The number of counters in scope after the element's own counter properties applied. The counters from this index
    // on were instantiated inside the subtree.
    uint32_t counters;
};

// What the counter pass reuses from one run to the next.
struct CssCountingScratch {
    Array<CssCountingStep> steps;
    Array<CssCounting> counters;           // innermost last
    Array<CssCountingBoundary> boundaries; // innermost last
    CssCountingTexts texts;
    PrTextBuffer text = {};
    Array<char> names;              // the names of the counters that a run took from a point (CssCountingPoint)
    Array<uint32_t> points;         // by CssCountingDepth: the step of each point that a run keeps
};

// The counters and containment boundaries in scope at a point of the counter pass, and the quote depth there.
struct CssCountingPoint {
    Array<CssCounting> counters;
    Array<CssCountingBoundary> boundaries;
    Array<char> names;              // the text that the counters' names view
    uint32_t quoteDepth;
};

// Where the next run of the counter pass may start other than at the root: the last element that the last run walked
// and its ancestors, and the state of the pass where each of them closes, after its content that the run walked and
// before its ::after.
struct CssCountingResume {
    Array<const PrDomElement*> chain; // by CssCountingDepth; empty if the next run starts at the root
    Array<CssCountingPoint> points;   // by CssCountingDepth
};

// The pseudo-elements whose boxes the presentation generates, by index of CssPresentationEntry::pseudos, which is the
// pseudo-element's CssPseudo less CssPseudo_Before.
constexpr uint32_t kCssPresentationPseudos = 4;
constexpr uint32_t kBeforePseudo = CssPseudo_Before - CssPseudo_Before;
constexpr uint32_t kAfterPseudo = CssPseudo_After - CssPseudo_Before;
constexpr uint32_t kMarkerPseudo = CssPseudo_Marker - CssPseudo_Before;
constexpr uint32_t kFieldContentPseudo = CssPseudo_FieldContent - CssPseudo_Before;

// What a typography group asks of shaping, or null for the shaper's defaults (CssPresentation::featureSets).
struct CssPresentationFeatures {
    const CssStyleValues* group;
    PrTextFeatureSet* PR_NULLABLE set;
};

// The styles of an element's ::first-line and ::first-letter (css-pseudo-4/#first-line-pseudo,
// css-pseudo-4/#first-letter-pseudo), each null if no rule styles it, and what they give its boxes. The first line
// inherits from the element, and the first letter from the first line, as text directly in the element finds them. The
// presentation holds a reference to each style.
struct CssPresentationFirsts {
    const PrDomElement* element;
    const CssComputedStyle* PR_NULLABLE line;
    const CssComputedStyle* PR_NULLABLE letter;

    // True if the element's styles or something in its subtree changed since its boxes last took them (applyFirsts).
    bool stale;

    // The boxes that hold a record for the first formatted line (prLayoutNodeSetFirstLineStyle).
    Array<PrLayoutNode> lined;

    // The box of the first letter, an inline box or a float around the first-letter text, or none. It sits before
    // `letterSource`, the text box that the letter was taken from. `letterFull` is the text that box held before the
    // letter was taken, and the box keeps the part of it after `letterEnd`.
    PrLayoutNode letterBox;
    PrLayoutNode letterSource;
    Array<char> letterFull;
    uint32_t letterEnd;
};

// A node of the document that the presentation styled or generated a box for.
struct CssPresentationEntry {
    const PrDomNode* PR_NULLABLE node;         // null for an entry that is free
    PrLayoutNode layoutNode;                   // kPrLayoutNodeNone if the node has no box in the tree

    // An element's computed style, with its animations. The presentation holds a reference.
    const CssComputedStyle* PR_NULLABLE style;

    const CssComputedStyle* PR_NULLABLE base;  // style without animations if they change it, else null
    bool animated;                             // the element has a transition or a CSS animation
    bool composited;                           // an animation is composited, and the element's box is a layer
    bool boxed;                                // the node generates a box, as the last restyle said, and the tree has
                                               // the box once the box operations apply

    // True if the node is an element of display contents whose parent's box holds its children's boxes in its place, as
    // the last restyle said.
    bool contents;

    CssPresentationBoxOps ops;                 // what applying the queued box operations does to the box
    bool replaced;                             // the box is a replaced element's, as the last restyle said
    bool semiReplaced;                         // the box is a semi-replaced element's, as the last restyle said

    // The box's record read state outside its style: a color resolved for the display's headroom or the user's
    // preferred scheme. A change to that state projects the record again.
    bool readsOutside;
    bool reprojectPending;                     // the record projects again at the next restyle, its style unchanged
    bool animationReadsOutside;                // the element's animated colors resolved for the display's headroom

    // The pseudo-element's content read the display's headroom or the user's preferred scheme: the colors of an image
    // of its content, or the scheme that picks its marker image. A change to that state makes the content again.
    bool contentReadsOutside;

    CssPresentationImageDrawing imageDrawing;           // how the replaced box's image content was last drawn

    // What the styles that the presentation computed have read about the node's children since the entry was made. A
    // read is never cleared.
    DomStructureFlags structure;

    // For an element whose direction is auto, if a style reads :dir(): the directionality that the element was last
    // styled with, which the next restyle compares with after a change to the text under it, or
    // CssPresentationDirection_Unknown if it was not noted. Also records whether such a change is queued
    // (PrCssPresentation::directionQueue).
    CssPresentationDirection direction;
    bool directionQueued;

    // The entries of the rule set at `matchedGeneration` that an element's style was computed from. A restyle that
    // cannot change what the element matches uses them instead of matching (CssCascadeInput::matched). The generation
    // is 0 if there are none.
    Array<uint32_t> matched;
    uint32_t matchedGeneration;

    // With `matched`: the element's pseudo-elements that may generate a box (cssCascadeGenerating), which a restyle
    // that takes what the element matched last takes too, or UINT32_MAX until a restyle finds them.
    uint32_t generating;

    // What the element's base style was last computed from on the presentation's own thread, which a restyle that finds
    // the same reuses (CssCascadeMemo).
    CssCascadeMemo memo;

    // An element's ::before, ::after and ::marker, once each has generated a box, or the ::marker of a ::before or an
    // ::after that is a list item. Each has an entry of its own.
    CssPresentationPseudo* PR_NULLABLE pseudos[kCssPresentationPseudos];

    // Whether the node takes part in the counter pass, and whether an element's pseudo-elements or descendants may
    // (dom_counter.h). A pass clears `countsBelow` of the subtrees that it finds hold no participant. `counterChanged`
    // says that the element is listed among the places where participants changed since the last pass
    // (PrCssPresentation::counterChanges).
    bool counts;
    bool countsBelow;
    bool counterChanged;

    // The display of the element, or of the ::before or ::after, makes a list item, as the last restyle said.
    bool listItem;

    bool originates;                // the element has a ::first-line or ::first-letter style (CssPresentation::firsts)
};

// A feature by which the descendants of an element that carry it are restyled at the next update. The features of one
// element form a chain.
struct CssPresentationInvalidation {
    const PrDomNode* element;
    uint32_t feature;               // a cssFeatureHash
    uint32_t next;                  // next feature of the element, or kHashMissing
};

// The state with which one thread of a restyle computes styles. The presentation's own thread is the first.
struct CssPresentationThread {
    CssStyleBuilder builder;
    CssCascadeScratch scratch;
};

// An element of a subtree whose styles a restyle computes ahead, on several threads, before it walks the subtree.
struct CssPresentationPrefetch {
    PrDomElement* element;
    uint32_t parent;                           // index of the parent element, or kHashMissing for the subtree root
    uint32_t end;                              // index past the element's descendants
    const CssComputedStyle* PR_NULLABLE style; // computed style, referenced by the prefetch until the walk takes it

    // The parent style that `style` was computed under. This is the style of the subtree root, or the prefetched style
    // of the parent. It is the style that the cascade gives, not what a running animation presents.
    const CssComputedStyle* PR_NULLABLE parentStyle;

    uint32_t entry;                             // the element's entry, or kHashMissing for an element that is new
    const Array<uint32_t>* PR_NULLABLE matched; // what the element matched last, if that still holds
    bool rematch;                               // true if what the descendants match may have changed

    // The element's pseudo-elements that may generate a box (cssCascadeGenerating), found with its style.
    uint32_t generating;
};

// A text font that the presentation's projections made, keyed by its families, style and locale. The key is spelled as
// the bytes at `keyStart` of CssPresentation::textFontKeys.
struct CssPresentationTextFont {
    uint32_t keyStart;
    uint32_t keyLength;
    PrTextFont* font;               // the presentation holds a reference
};

// Text fonts, each made once for its families, style and locale, so that equal fonts are one text font.
struct CssPresentationTextFonts {
    Array<CssPresentationTextFont> fonts;
    Array<char> keys;
    HashIndex index;
    Array<char> key;                // scratch: a key being looked up
    PrTextBuffer families = {};     // scratch: names of a font's families
    Array<PrFontFamily> familyViews;
};

// A face of the document's fonts that the presentation's font set holds, with a key that the presentation issues and
// the status that the set has for the face.
struct CssPresentationFontFace {
    PrCssFontFace* face;            // the entry holds a reference
    uint64_t key;                   // the set's key for the face, unique for the life of the presentation
    PrCssFontFaceStatus status;
    bool described;                 // true if the set holds the face, because it names a family
};

// The rendering of a document through layout.
struct CssPresentation {
    PrDomDocument* document;
    PrFontSet* fonts;               // the document's fonts, layered over the set of the tree's default font

    // The text fonts the projections made, so that equal fonts project to one font group.
    CssPresentationTextFonts textFonts;

    // How the cascade measures fonts for ex, cap, ch and ic (CssFontMeasure), on any restyle thread, in text fonts of
    // its own that `measuring` guards. `readsMetrics` is set once a style has read a metric. From then on, a change of
    // the fonts restyles the elements whose styles read one.
    CssFontMeasure fontMeasure;
    CssPresentationTextFonts measured;
    PrMutex measuring;
    bool readsMetrics;

    Array<CssPresentationFontFace> fontFaces;
    KeyIndex fontFaceKeys;          // `fontFaces` by their keys
    KeyIndex fontFaceAddresses;     // `fontFaces` by the addresses of their faces
    uint64_t nextFontFaceKey;       // key for the next face, starting from 1
    uint64_t fontsVersion;          // the document's fonts version when the set was last brought up to date

    // The number of the document's faces that had not started loading at the last sync, and the document's
    // fontSetVersion at that time. A face that has started loading since then leaves the count too high, which delays
    // nothing past the next sync.
    uint32_t unloadedFaces;
    uint64_t unloadedFacesVersion;

    PrLayoutTree* tree;
    PrLayoutNode root;               // the box of the document's root, which the viewport lays out, or none
    PrTextShaper* shaper;            // what the tree shapes with, which the content of svg elements shapes with too
    Array<CssParsedUserAgentSheet> userAgent;
    PrCssSheetConditions conditions; // the document's conditions when the rule set was last built
    CssRuleSet userAgentRules;       // the rules of the user agent's sheets that apply, which `rules` builds on
    CssRuleSet rules;
    CssStyleTable styles;                      // style groups of every thread
    PrDomAgent* agent;                         // agent whose threads the presentation's work runs on
    Array<CssPresentationThread> threads;      // one for each of the agent's restyle threads, made by the first restyle
    Array<CssPresentationPrefetch> prefetched; // in tree order
    uint32_t prefetchNext;                     // first prefetched element that the walk has not taken
    Array<uint32_t> tasks;                     // scratch: roots of the subtrees that the pool computes
    CssAnimateState animations;
    Array<CssPresentationEntry> entries;
    Array<uint32_t> free;           // free entries
    Array<PrLayoutCalcNode> calc;   // scratch: a length's calculation

    // Scratch: a track list's tracks, the names of its lines and of the lines of its automatic repetition, and a grid
    // template's areas.
    Array<PrLayoutGridTrack> gridTracks;
    Array<PrLayoutGridLineName> gridNames;
    Array<PrLayoutGridLineName> gridRepeatNames;
    Array<PrLayoutGridArea> gridAreas;

    // The images that styles name by URL, fetched once each and kept for the presentation's life.
    Array<CssPresentationImage> images;
    HashIndex imageIndex;                           // `images` by hash of the URL
    Array<CssPresentationImageFetch*> imageFetches; // fetches in flight, whose presentation its destruction clears

    // Scratch: an element's background layers, their images' gradient stops and filter items, the sources that the
    // filter items name, which the presentation holds a reference to until the tree holds its own, and where each
    // layer's stops and items start.
    Array<PrLayoutBackgroundLayer> backgroundLayers;
    Array<PrLayoutColorStop> backgroundStops;
    Array<PrLayoutFilter> imageFilters;
    Array<PrLayoutSource> imageSources;
    Array<CssPresentationImageSpans> backgroundSpans; // by index of the layer

    // Scratch: an element's mask layers, where each layer's stops and filter items start, a basic shape's points, and a
    // filter list.
    Array<PrLayoutMaskLayer> maskLayers;
    Array<CssPresentationImageSpans> maskSpans;       // by index of the layer
    Array<PrLayoutLength> shapePoints;
    Array<PrLayoutFilter> filters;

    PrLayoutVec extent;                   // viewport of the last update
    PrLayoutNode canvasRoot;              // boxes of the root and the body when the canvas was last projected
    PrLayoutNode canvasBody;
    CssFontBases rootFont;                   // font bases of the root at the last restyle, for root-relative lengths
    uint64_t version;                        // incremented by each update that changes something
    bool changed;                            // a node is marked, or the document changed, since the last update
    bool restyleAll;                         // the rule set is out of date, or what every style is relative to changed
    bool rulesStale;                         // the rule set is out of date, except for the rules a change restyles by
                                             // feature
    bool layoutStale;                        // an update changed styles or boxes that no layout has laid out since
    bool sceneStale;                         // a layout on the caller's thread changed what the last scene shows
    bool readsViewport;                      // a computed style resolved a length relative to the viewport's extent
    bool readsOutside;                       // an entry's readsOutside is set
    const PrDomNode* PR_NULLABLE projecting; // the node whose record is being projected, which an image waits on
    float headroom;                          // the display's headroom in stops that styles were last projected for

    // The user's preferred scheme and the page's supported schemes that styles were last projected for.
    bool prefersDark;
    CssColorSchemeFlags pageSchemes;
    bool schemesStale;                       // the document's meta elements changed since pageSchemes was read

    PrCssPresentationEmbedding embedding;

    bool mediaStale;                         // the media environment changed since the rule set's media queries were
                                             // answered
    bool holding;                            // the last restyle held new content back for a later restyle
    PrCssPresentationStats stats;            // stats of the last update
    PrCssPresentationStats accrued;          // stats of the work done since the last update
    double jobBudget;                        // target duration of a job, in milliseconds, or 0 for no limit
    double jobRate;                          // new elements taken in per millisecond by the jobs so far
    double restyleRate;                      // new elements taken in per millisecond by the restyles ahead of jobs so
                                             // far
    uint32_t newLeft;                        // new elements that the updates before the next job may still take in
    uint32_t newTaken;                       // new elements taken in since the last job started
    uint32_t jobTaken;                       // new elements that the last started job took in
    Array<PrLayoutNode> erasing;             // layout nodes whose nodes left the document, which the next update erases
    Array<PrLayoutNode> unmapping;           // those of `erasing` removed while the agent's worker held the tree

    // The nodes whose boxes changed in the restyles since the tree was last brought up to date. The presentation's own
    // thread applies the changes while it holds the tree. A listed node may have left the document since, and the
    // entry's own state says what applying it does.
    Array<const PrDomNode*> boxQueue;

    // The queued nodes whose entries were dropped, each with a reference, so that the queue's pointers stay valid until
    // it is applied.
    Array<const PrDomNode*> boxQueueDropped;

    // A participant of the counter pass was added, removed or restyled since the last pass, which the next update runs.
    // The elements where the participants changed are listed, each with CssPresentationEntry::counterChanged set,
    // unless a change has no such element, such as a removal, which `countersFromRoot` notes. The next pass then starts
    // at the root.
    bool countersStale;
    Array<const PrDomNode*> counterChanges;
    bool countersFromRoot;

    CssCountingScratch counterScratch;
    CssCountingResume counterResume;

    // Pseudo-elements whose entries were dropped. Each is freed once no queue or box refers to it: when the tree is
    // next brought up to date.
    Array<CssPresentationPseudo*> retired;

    // The records of the outermost svg elements that the presentation has given boxes (css_presentation_svg.h). A
    // record that its element's entry left is retired, and freed with its content once the tree has erased the box
    // whose painter reads it.
    Array<CssPresentationSvg*> svgs;
    Array<CssPresentationSvg*> retiredSvgs;

    // What lib/svg keeps for the presentation, and what the presentation notes for it while it does.
    void* PR_NULLABLE svgState;
    Array<CssPresentationSvgNote> svgNotes;
    bool svgInvalid;

    // What resolves the clipPaths, masks and filters that the boxes' effects name by URL, and the nodes whose records
    // name one of them. Those nodes take their sources again when what the sources draw may have changed.
    const CssPresentationReferences* PR_NULLABLE references;
    Array<const PrDomNode*> referencing;

    // What the font properties of each typography group (CssStyleGroup_Typography) other than the initial one ask of
    // shaping, made when a projection first reads the group (cssFontFeatureSet), so that equal groups give one set.
    // Holds a reference to each group and set.
    Array<CssPresentationFeatures> featureSets;
    HashIndex featureSetIndex;      // `featureSets` by group

    // The styles of the ::first-line and ::first-letter of each element that originates either, which few do.
    Array<CssPresentationFirsts> firsts;
    HashIndex firstsIndex;          // `firsts` by element
    uint32_t foreignBoxes;          // the boxes of foreignObject elements, each a root of the layout tree

    Array<CssPresentationInvalidation> invalidations; // invalidations of the elements marked DomStyleFlag_Features
    HashIndex invalidationIndex;                      // first of each element's `invalidations`
    Array<uint32_t> features;                         // scratch: features by which an update restyles the descendants
                                                      // of an element
    Array<uint32_t> directionQueue;                   // entries whose directionality a change to the text under them
                                                      // may have changed

    // The nodes at which scroll and scrollend events are due at the next run of the scroll steps: elements, or the
    // document for the viewport. Each entry holds its node (domNodeHold).
    Array<PrDomNode*> scrolled;

    // What scroll anchoring moved each container by since the last run of the scroll steps, summed over the layouts in
    // between. A container that layouts moved and moved back has not scrolled, and gets no scroll event.
    Array<PrLayoutScrollAdjustment> anchorMoves;

    // The host's scrolls made while the presentation's thread held the tree. They apply in order when the presentation
    // takes the tree back.
    Array<PrLayoutSceneScroll> pendingScrolls;

    // The offsets that the document's own scrolls set since the host last took them (prCssPresentationTakeScrolls),
    // listed once for each container.
    Array<PrLayoutSceneScroll> documentScrolls;
};

// Returns the entry of `node`, or kHashMissing if the presentation has none.
inline uint32_t cssPresentationNodeEntry(const CssPresentation* presentation, const PrDomNode* node) {
    uint32_t at = node->presentationEntry - 1;
    bool own = node->presentationEntry != 0 && at < presentation->entries.count && presentation->entries.data[at].node == node;
    return own ? at : kHashMissing;
}

// Records that the document changed in a way that can change any element's style, such as its sheets or its media
// environment.
inline void cssPresentationChanged(CssPresentation* presentation) {
    presentation->changed = true;
    presentation->restyleAll = true;
}

// Records that the document's meta elements changed, which can change the page's supported color schemes.
inline void cssPresentationMetaChanged(CssPresentation* presentation) {
    presentation->changed = true;
    presentation->schemesStale = true;
}

// Records that the document's media environment changed. This restyles every element if a media query of the document's
// sheets now gives a different answer.
inline void cssPresentationMediaChanged(CssPresentation* presentation) {
    presentation->changed = true;
    presentation->mediaStale = true;
}

// Records that a rule of one of the document's sheets was inserted, removed, or had its declarations changed. The rule
// set is out of date. The elements that carry a feature that the rule's selectors are filed under are restyled. If some
// selector of the rule can style any element, every element is restyled.
void cssPresentationRuleChanged(PrCssPresentation* presentation, const CssRuleData* rule);

// Records that a resource document that the document's references name has loaded (dom_resource.h). What the IDs of the
// references name may have changed, and the uses that reference it are synchronized at the next restyle.
void cssPresentationResourceLoaded(PrCssPresentation* presentation);

// Notify the presentation of changes to its document. Each function is called for a node that is connected. An
// attribute change is reported with the old value and the new value, each the null view for none. A state change names
// the pseudo-classes whose state changed.
void cssPresentationInserted(PrCssPresentation* presentation, PrDomNode* node);
void cssPresentationRemoving(PrCssPresentation* presentation, PrDomNode* node);
void cssPresentationAttributeChanged(PrCssPresentation* presentation, PrDomElement* element, DomNamespaceId space,
    PrStringView name, PrStringView oldValue, PrStringView value);
void cssPresentationDataChanged(PrCssPresentation* presentation, PrDomNode* node);
void cssPresentationContentChanged(PrCssPresentation* presentation, PrDomElement* element);
void cssPresentationStateChanged(PrCssPresentation* presentation, PrDomElement* element, const CssPseudoClass* states,
    uint32_t count);

// Notify the presentation of changes to the flat tree (css-shadow-1/#flattening), each of a node that is connected. A
// slot's assigned nodes changed from `before`: the nodes that it lost leave the flat tree unless another slot took
// them, and its fallback content leaves or returns. An element became a shadow host, and its children left the flat
// tree.
void cssPresentationSlotChanged(PrCssPresentation* presentation, PrDomElement* slot,
    PrDomNode* const PR_NONNULL* PR_NULLABLE before, uint32_t beforeCount);
void cssPresentationShadowAttached(PrCssPresentation* presentation, PrDomElement* host);

// Projects the record of the element's box again, because the element gained or lost a wheel listener that is not
// passive (PrLayoutWheelEvents).
void cssPresentationWheelListenersChanged(PrCssPresentation* presentation, PrDomElement* element);

// Restyles the elements that a change to the element's states that no attribute decides can reach. Those states are
// whether the element is defined as a custom element (:defined), and its custom states (:state()).
void cssPresentationStatesChanged(PrCssPresentation* presentation, PrDomElement* element);

// Tells the presentation that the value of the text control `element` changed, so that its ::field-content shows it.
void cssPresentationFieldChanged(PrCssPresentation* presentation, PrDomElement* element);

// Restyles the elements that a change to the host's answers for `states` can reach (DomDocumentHost::elementState).
// Links are invalidated as a change of their href is, because their :link and :visited depend on it.
void cssPresentationHostStatesChanged(PrCssPresentation* presentation, PrDomHostState states);

// Brings the presentation's styles, its tree and the tree's layout up to date with the document, on the caller's
// thread, as a synchronous query needs. The document's host does it if it brings up to date what it draws in the boxes
// (DomDocumentHost::updatePresentation). Otherwise the presentation is updated alone, at the extent of the host's
// viewport or of its last update.
void cssPresentationFlush(PrCssPresentation* presentation);

// The two steps of prCssPresentationUpdate, between which the browsing context brings up to date what it draws in the
// boxes, such as the content of svg elements (css_presentation_svg.h). The first brings the styles and the boxes up to
// date. The second lays the boxes out if anything changed, and returns true if it did.
void cssPresentationUpdateBoxes(PrCssPresentation* presentation, PrLayoutVec extent);
bool cssPresentationUpdateLayout(PrCssPresentation* presentation, PrLayoutVec extent);

// Takes the tree back from the layout thread, and returns true if the content of an svg element is stale for its box's
// new extent, which the next update brings in.
bool cssPresentationSvgsResized(PrCssPresentation* presentation);

// Returns true if lib/svg has nothing to bring up to date in the presentation: it keeps no state for it, or the
// presentation has no outermost svg element with a box, no notes, and nothing invalidated.
inline bool cssPresentationSvgIdle(const CssPresentation* presentation) {
    return presentation->svgState == nullptr ||
        (presentation->svgs.count == 0 && presentation->svgNotes.count == 0 && !presentation->svgInvalid);
}

// Returns the layout node that presents `node`, which is an element or a text node, as of the last update, or
// kPrLayoutNodeNone.
PrLayoutNode cssPresentationBox(const PrCssPresentation* presentation, const PrDomNode* node);

// Returns the node of the document that a layout node presents, or null.
PrDomNode* PR_NULLABLE cssPresentationNodeFromLayout(const PrCssPresentation* presentation, PrLayoutNode node);

// Appends the nodes under `point`, in viewport coordinates as of the last layout, to `out`, topmost first and each
// once. These are the nodes whose boxes are hit (prCssPresentationHitTest), up to `limit` of the topmost boxes. Before
// an outermost svg element come the elements of its content that are hit there, and the nodes of a foreignObject's
// content come before the foreignObject. An element of a use element's shadow tree stands for itself.
void cssPresentationHits(PrCssPresentation* presentation, PrLayoutVec point, uint32_t limit, Array<PrDomNode*>* out);

// Returns the element's computed style as of the last update, or null if the presentation did not style it.
const CssComputedStyle* PR_NULLABLE cssPresentationStyle(const PrCssPresentation* presentation, const PrDomElement* element);

// Returns true if the element is being rendered (html/#being-rendered) and the user can interact with it: it generates
// a box, is an element of display contents, or is SVG content or a canvas's fallback content that is displayed, its
// visibility is visible, and it is not inert (css-ui-4/#inertness). The presentation first styles the document, every
// new element included, as a synchronous query does.
bool cssPresentationIsInteractive(PrCssPresentation* presentation, const PrDomElement* element);

// Returns the node of the document that the box `box` presents, or null for an anonymous box.
const PrDomNode* PR_NULLABLE cssPresentationBoxNode(const PrCssPresentation* presentation, PrLayoutNode box);

// Returns the style of `element` matched where it is, but inheriting from `parent`, as an element of a use element's
// shadow tree inherits (SVG2/struct.html#UseElement). `element` may be outside the document, as such a tree's elements
// are. The caller holds one reference. Called on the presentation's thread, outside a restyle.
const CssComputedStyle* cssPresentationStyleUnder(PrCssPresentation* presentation, const PrDomElement* element,
    const CssComputedStyle* PR_NULLABLE parent);

// Returns the style of the ::first-line or ::first-letter `pseudo` of `element`, inheriting from `parent`, or null if
// no rule styles it. The caller owns a reference to the style. Called on the presentation's thread, outside a restyle.
const CssComputedStyle* PR_NULLABLE cssPresentationPseudoStyleUnder(PrCssPresentation* presentation,
    const PrDomElement* element, CssPseudo pseudo, const CssComputedStyle* PR_NULLABLE parent);

// Projects again, at the next update, the boxes that name a source of the references (CssPresentationReferences),
// because what a source draws may have changed.
void cssPresentationReprojectReferencing(PrCssPresentation* presentation);

// Returns what the presentation's colors resolve for: the display's headroom, the user's preferred scheme and the
// page's supported schemes, as of the last update.
CssColorEnvironment cssPresentationColorEnvironment(const PrCssPresentation* presentation);

// Returns the context that colors on `element` resolve in once the presentation's styles are brought up to date, as a
// query of computed values brings them: the element's color and its computed style, or the initial values for an
// element that the presentation does not style. The style that the context names is valid until the presentation next
// restyles. Called on the presentation's thread, outside a restyle.
CssColorContext cssPresentationColorContext(PrCssPresentation* presentation, const PrDomElement* element);

// Returns the element's computed style once the presentation's styles are brought up to date, as a query of computed
// values brings them, or null for an element that the presentation does not style. The style is valid until the
// presentation next restyles. Called on the presentation's thread, outside a restyle.
const CssComputedStyle* PR_NULLABLE cssPresentationStyleCurrent(PrCssPresentation* presentation, const PrDomElement* element);

// Returns the tree, which the caller's thread owns until the presentation next starts a worker.
PrLayoutTree* cssPresentationOwnTree(PrCssPresentation* presentation);

// Maps `count` points in place from the content box of the box of the replaced element `element`, from its top left, to
// viewport coordinates as of the last layout, through every transform and scroll offset around the box. Points are in
// CSS pixels. Returns false if the element has no box, or a projection puts a point behind the eye.
bool cssPresentationMapFromContent(PrCssPresentation* presentation, const PrDomElement* element, PrDVec2* points,
    uint32_t count);

// Scrolls the scroll container `node`, or the viewport for kPrLayoutNodeNone, to `offset` within its range as the last
// layout left it (cssom-view-1/#perform-a-scroll). If the offset changes, scroll and scrollend events are due at the
// next run of the scroll steps, at the element or, for the viewport, at the document. Does nothing for a node that is
// not a scroll container.
void cssPresentationScrollTo(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec offset);

// Fires the scroll events, then the scrollend events, that are due (cssom-view-1/#document-run-the-scroll-steps). An
// event at the document bubbles.
void cssPresentationRunScrollSteps(PrCssPresentation* presentation);

// Fires the events of the presentation's transitions and CSS animations that are due at its current time, and the
// events of those canceled since the last call, in the order in which they happened
// (web-animations-1/#update-animations-and-send-events).
void cssPresentationSendAnimationEvents(PrCssPresentation* presentation);

// Returns true if the presentation has work for the next rendering update: events of scrolls or of animations to fire,
// a change of the document whose styles it has yet to compute, or a layout to make that may request fonts
// (cssPresentationAwaitsFontRequests).
bool cssPresentationHasWork(const PrCssPresentation* presentation);

// Returns true if a layout of the presentation may request fonts that are not loaded. This is the case if the document
// has a face that has not started loading, and the presentation's text has not been laid out since the document or its
// fonts changed. Until that layout, the document's FontFaceSet is pending on the environment
// (css-font-loading-3/#fontfaceset-pending-on-the-environment), and a rendering update lays the document out.
bool cssPresentationAwaitsFontRequests(const PrCssPresentation* presentation);

// Brings the presentation's styles up to date with the document, as a rendering update does before its host lays out
// the document. Style changes start their transitions here. New content beyond the next job's share waits, in the same
// way as in a job's restyle (prCssPresentationSetJobBudget). The tree takes the changes when the host next starts a job
// or updates, so this function does not wait for the presentation's thread. Then the document's FontFaceSet switches to
// loaded if it was waiting only for this update (cssFontsReadinessChanged).
void cssPresentationUpdateStyle(PrCssPresentation* presentation);

// Fills `out` with the computed style of the element, or of its pseudo-element `pseudo`, as declarations of the
// resolved value of each property that the engine renders (cssom-1/#resolved-values), in the order of their names,
// followed by its custom properties, after updating the presentation. Leaves `out` unchanged if `*filler` and
// `*version` say that this presentation filled it at its current version. Otherwise records them.
void cssPresentationComputedBlock(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
    CssDeclarationBlock* out, const void* PR_NULLABLE* PR_NONNULL filler, uint64_t* version);

// Serializes the resolved value of the longhand `property` of the computed style of the element, or of its
// pseudo-element `pseudo`, to `out`, after updating the presentation's styles, and also its layout if the value is a
// used value. The result matches the declaration of the property in cssPresentationComputedBlock. Writes nothing for a
// property that the engine does not render, or an element that it did not style.
bool cssPresentationComputedValue(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
    PrCssProperty property, PrTextBuffer* out);

// Serializes the computed value of the custom property `name` of the element, or of its pseudo-element `pseudo`, to
// `out`, after updating the presentation's styles. The result matches the declaration of the property in
// cssPresentationComputedBlock. Writes nothing for a property without a valid value, or an element that it did not
// style.
bool cssPresentationComputedCustomValue(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
    PrStringView name, PrTextBuffer* out);

// The two halves of prCssPresentationStart. The first brings the styles and the tree up to date and waits for the
// agent's layout thread. The second posts the job, and then restyles the next job's content ahead. A host that presents
// several documents can then take in the changes of each document before it posts any job, so that the jobs run back to
// back, and brings up to date what it draws in the boxes in between.
void cssPresentationPrepareJob(PrCssPresentation* presentation, PrLayoutVec extent);
void cssPresentationPostJob(PrCssPresentation* presentation, PrLayoutVec extent);

// Returns true if the element's used color scheme is dark (css-color-adjust-1/#color-scheme-prop), as of the
// presentation's last update. An element that the presentation has not styled uses the page's scheme.
bool cssPresentationUsesDarkScheme(const PrCssPresentation* presentation, const PrDomElement* element);

} // namespace pr_impl

// C opaque types
struct PrCssPresentation : pr_impl::CssPresentation {};

PR_ASSUME_NONNULL_END
