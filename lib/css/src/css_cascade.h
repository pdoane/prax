// The Cascade
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-cascade-6 - https://drafts.csswg.org/css-cascade-6/
//
// A rule set holds the style rules of a document's sheets. Each complex selector is an entry, filed under the most
// selective feature of its subject compound: an ID, a class, or a type. An element is matched against the entries filed
// under its own features and the entries filed under none. The declarations of the entries that it matches are ordered
// by origin, importance, layer, specificity and order of appearance (css-cascade-5/#cascade-sort), and the last
// declaration of each property wins.

#pragma once

#include "pr/array.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/hash_index.h"
#include "pr/string_view.h"

#include "css/src/css_counter.h"
#include "css/src/css_media.h"
#include "css/src/css_rule.h"
#include "css/src/css_selector_match.h"
#include "css/src/css_style.h"
#include "dom/src/dom_element_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssStyleSheet;

// The layer of a declaration outside every layer. For normal declarations, it wins over every layer
// (css-cascade-5/#layer-ordering).
constexpr uint16_t kCssUnlayered = 0xffff;

// The number of ancestor features that an entry keeps. An element's ancestors must carry all of them for the element to
// match.
constexpr uint32_t kCssAncestorHashes = 4;

// The number of types that an entry keeps of which an element's ancestors must carry one, as for the ancestor compound
// of `:is(ol, ul) li`.
constexpr uint32_t kCssAncestorAlternatives = 6;

// One complex selector of a style rule, and the declarations it applies.
struct CssRuleEntry {
    const PrCssSelectors* selectors;
    const CssDeclarationBlock* block;
    uint32_t complex;               // index of the complex selector among the selectors' records
    uint32_t matcher;               // index of the selectors among the rule set's lists
    CssSpecificity specificity;
    uint32_t order;                 // order of appearance across the rule set
    uint16_t layer;
    CssOrigin origin;
    bool attributes;                // the declarations read attributes through attr() (css-values-5/#attr-notation)
    CssPseudo pseudo;               // what the selector styles
    CssAppearance appearance;       // the used appearance that the entry applies under, or None for every element
    const CssSheetData* sheet;      // the sheet that holds the rule, whose URL its url()s resolve against

    // The root of the tree whose sheet holds the rule: the document, or a shadow root. A selector matches the elements
    // of that tree (css-shadow-1/#selectors-data-model), the tree's host through :host, and the elements assigned to
    // the tree's slots through ::slotted(). Null for a rule of the user agent's, which matches in every tree.
    const PrDomNode* PR_NULLABLE scope;
    uint32_t depth;                 // the number of shadow roots around the scope, which orders encapsulation contexts

    // The entry's place in the cascade before its order: its origin, its encapsulation context, its layer and its
    // specificity, compared as one number.
    uint64_t rank;

    // Features of the compounds that the subject reaches through descendant and child combinators alone, and 0 past the
    // last. A feature is an id, a class, an attribute name or a type. The element's ancestors must carry these
    // features. If no ancestor carries one of them, the element fails to match without a walk of its ancestors. This is
    // an ancestor filter.
    uint32_t ancestors[kCssAncestorHashes];

    // The types of an ancestor compound that requires one of several types (typeAlternatives), and 0 past the last. The
    // element's ancestors must carry at least one of them.
    uint32_t anyAncestor[kCssAncestorAlternatives];
};

// The entries filed under one feature.
struct CssRuleBucket {
    uint32_t hash;                  // hash of the feature's kind and its lowercased name
    Array<uint32_t> entries;
};

// The entries that style one kind of target, filed for matching against an element: the element itself, or one of its
// pseudo-elements.
struct CssRuleIndex {
    Array<CssRuleBucket> buckets;
    HashIndex index;
    Array<uint32_t> universal;      // the entries filed under no feature
};

// The kinds of feature that a selector reads from an element. Entries are filed by these features, and a change to one
// of them invalidates styles.
enum {
    CssFeature_Id = 1,
    CssFeature_Class = 2,
    CssFeature_Type = 3,
    CssFeature_Attribute = 4,       // the presence or value of an attribute, by the attribute's name
    CssFeature_PseudoClass = 5,     // a state that is not decided by attributes, by the pseudo-class's name
    CssFeature_State = 6,           // any of the states that attributes decide, under the empty name
};
typedef uint8_t CssFeature;

// Returns the hash that a feature of `kind` named `name` is filed and invalidated under, computed from the ASCII
// lowercase form of the name. Never returns 0.
uint32_t cssFeatureHash(CssFeature kind, PrStringView name);

// An element whose :has() argument reads a feature. A change to the feature on the element's descendants or later
// siblings can make the :has() match or stop matching. The anchor records the feature that the element carries, and
// what a change to whether it matches restyles.
struct CssHasAnchor {
    uint32_t feature;               // the feature of the compound that holds the :has(), or 0 for any element
    bool self;
    bool laterSiblings;
    bool siblings;
    bool subtree;
    uint32_t descendant;            // a feature of the descendants restyled, like those in CssInvalidation::descendants
    bool previous;                  // true if the argument reaches later siblings, so earlier siblings can be anchors
};

// A set of :has() anchors, each once, indexed by the feature of the compound that holds each. An element finds the
// anchors that it can be by its own features, so that the cost of finding them grows with the element's features, not
// with the number of anchors.
struct CssHasAnchors {
    Array<CssHasAnchor> anchors;
    Array<uint32_t> nextOfFeature;  // by index: the next anchor with the same feature, or kHashMissing
    HashIndex byFeature;            // the first anchor of each feature, by the feature
    Array<uint32_t> anyElement;     // the anchors without a feature, which any element can be
    Array<uint32_t> previous;       // the anchors whose arguments reach later siblings (CssHasAnchor::previous)
};

// Adds `anchor` to the set, unless the set holds it.
void cssHasAnchorsAdd(CssHasAnchors* set, const CssHasAnchor& anchor);

// Frees the set's storage and leaves it empty.
void cssHasAnchorsReset(CssHasAnchors* set);

// Calls `visit` with each anchor of the set that an element carrying the features `features` (cssFeatureHash) can be.
// For an element that is an earlier sibling of the change, which only an anchor whose argument reaches later siblings
// reads, `sibling` is true.
template <typename Visit>
void cssHasAnchorsEach(const CssHasAnchors& set, const uint32_t* features, uint32_t count, bool sibling, Visit visit) {
    auto carried = [&](uint32_t feature) {
        for (uint32_t i = 0; i < count; ++i) {
            if (features[i] == feature)
                return true;
        }
        return false;
    };
    if (sibling) {
        for (uint32_t i = 0; i < set.previous.count; ++i) {
            const CssHasAnchor& anchor = set.anchors.data[set.previous.data[i]];
            if (anchor.feature == 0 || carried(anchor.feature))
                visit(anchor);
        }
        return;
    }
    for (uint32_t i = 0; i < set.anyElement.count; ++i)
        visit(set.anchors.data[set.anyElement.data[i]]);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t first = set.byFeature.find(features[i], [&](uint32_t at) {
            return set.anchors.data[at].feature == features[i];
        });
        for (uint32_t at = first; at != kHashMissing; at = set.nextOfFeature.data[at])
            visit(set.anchors.data[at]);
    }
}

// What a change to one feature of an element can restyle, as an invalidation set. It covers the element itself, its
// siblings with their subtrees, and its descendants: either every descendant, or those that carry one of `descendants`.
// It also covers the anchors among the element's ancestors and earlier siblings whose :has() arguments read the
// feature. A state's invalidations are filed by the feature of the compound that reads the state, so that a change of
// state restyles what it reaches only from an element that carries that feature. The invalidations of one state form a
// chain.
struct CssInvalidation {
    uint32_t hash;                  // hash of the feature (cssFeatureHash)
    uint32_t qualifier;             // a feature that the changed element must carry, or 0 for any element
    uint32_t next;                  // the next invalidation of the same feature, or kHashMissing
    bool self;
    bool laterSiblings;             // the siblings after the element
    bool siblings;                  // every sibling, before and after the element
    bool subtree;                   // every descendant, in place of `descendants`
    Array<uint32_t> descendants;    // the features of the descendants restyled, each a cssFeatureHash
    CssHasAnchors anchors;
};

// What the selectors of a rule set read beyond the features that invalidations are filed under.
struct CssRuleReads {
    bool structure;                 // structural pseudo-classes: a child added or removed can restyle its siblings
    bool states;                    // pseudo-classes of states that attributes decide: any attribute can be read
    bool global;                    // a combinator across the tree: a change can restyle any element
    bool direction;                 // :dir(): text under an element whose direction is auto can restyle it
    bool attributes;                // an attr() whose name another function gives: any attribute can restyle
};

// A @keyframes rule that animations name (css-animations-1/#keyframes).
struct CssKeyframes {
    PrString name;
    const CssRuleData* rule;
};

// A media query list that the build of a rule set evaluated, and its result.
struct CssMediaAnswer {
    const CssMediaQueries* list;    // the list, in a sheet that the build filed
    bool matched;
};

// The style rules of a document's sheets, filed for matching. A selector of a pseudo-element is filed by the features
// of its originating element, which matching tests, and its features restyle the originating element.
struct CssRuleSet {
    Array<CssRuleEntry> entries;
    CssRuleIndex filed[CssPseudo_Count];           // the entries of each target, by the originating element's features

    // The entries of ::before and ::after whose declarations set content, also in `filed`. A pseudo-element that
    // matches none of them generates no box, which most elements' pseudo-elements find without matching every entry.
    CssRuleIndex generating;

    // The entries whose subject ends in ::slotted(), which match an element through the slots it is assigned to, apart
    // from `filed`.
    Array<uint32_t> slotted;
    Array<const PrCssSelectors* PR_NONNULL> lists; // the selectors of each style rule, each list with its own matcher
    const PrDomDocument* PR_NULLABLE document;     // the document whose elements the set matches
    uint32_t generation;                           // incremented by each build; the matchers of a scratch follow it
    Array<PrString> layers;                        // the name of each layer, in layer order
    Array<CssKeyframes> keyframes;                 // the last @keyframes rule of each name

    // The @counter-style rules, in cascade order, and the counter styles that they define.
    Array<const CssRuleData* PR_NONNULL> counterStyleRules;
    CssCounterStyles counterStyles;
    Array<CssInvalidation> invalidations;
    HashIndex invalidationIndex;    // `invalidations` by feature
    CssHasAnchors anchors;          // every :has() anchor, which a child added or removed below it can change
    Array<CssMediaAnswer> media;    // the media query lists that decided what the build filed
    CssRuleReads reads;

    // True if the document had a shadow tree when the set was built, so that matching tests each entry's scope. A
    // document's first shadow root builds the set again.
    bool scoped;
};

// Returns true if a declaration of the block reads attributes through attr() (css-values-5/#attr-notation).
bool cssBlockReadsAttributes(const CssDeclarationBlock* block);

// Returns the @keyframes rule that `name` names, or null if none does.
const CssRuleData* PR_NULLABLE cssRuleSetKeyframes(const CssRuleSet* set, PrStringView name);

// Returns what a change to the feature that hashes to `hash` invalidates: the first invalidation of its chain
// (CssInvalidation::next). Returns null if no selector reads the feature.
const CssInvalidation* PR_NULLABLE cssRuleSetInvalidation(const CssRuleSet* set, uint32_t hash);

// Appends to `out` the features of the elements that `rule` can style: the most selective feature of the subject of
// each complex selector of its style rules, in the rule itself or in the conditional and layer rules that it holds.
// Returns false if any element can be styled: for a subject with no feature, or a rule that is not of those kinds.
bool cssRuleSubjectFeatures(const CssRuleData* rule, Array<uint32_t>* out);

// A user agent's sheet (PrCssUserAgentSheet), parsed: the document's conditions that it applies under, and the used
// appearance of the elements that it styles, or None for every element. A sheet for one appearance declares neither
// `appearance` nor a custom property, because the cascade decides the used appearance before it applies the sheet's
// declarations.
struct CssParsedUserAgentSheet {
    CssSheetData* data;
    PrCssSheetConditions conditions;
    CssAppearance appearance;
};

// Files the style rules of the user agent's sheets that apply under the document's `conditions` for `document`, in
// order, with their media conditions evaluated against `media`. The rule set is the base that the document's rule set
// builds on (cssRuleSetBuild).
void cssRuleSetBuildUserAgent(CssRuleSet* set, const PrDomDocument* document,
    const CssParsedUserAgentSheet* PR_NULLABLE userAgent, uint32_t userAgentCount,
    PrCssSheetConditions conditions, const CssMediaEnvironment* PR_NULLABLE media);

// The sheets of one tree of a document: the document's own, or a shadow root's
// (cssom-1/#documentorshadowroot-document-or-shadow-root-css-style-sheets).
struct CssTreeSheets {
    const PrDomNode* tree;          // the document or the shadow root
    uint32_t depth;                 // the number of shadow roots around the tree, which is 0 for the document
    const PrCssStyleSheet* const PR_NONNULL* PR_NULLABLE sheets;
    uint32_t count;
};

// Files the style rules of each tree's sheets in order, after those of `base`, with their conditions evaluated against
// `media`. Each entry's scope is its tree. The set starts as a copy of `base`, which costs less than filing the user
// agent's sheets again.
void cssRuleSetBuild(CssRuleSet* set, const CssRuleSet* base, const CssTreeSheets* PR_NULLABLE trees,
    uint32_t treeCount, const CssMediaEnvironment* PR_NULLABLE media);

// Returns true if a media query list that decided what the rule set filed gives a different result in `media`. If so,
// the set is out of date even for the same sheets.
bool cssRuleSetMediaChanged(const CssRuleSet* set, const CssMediaEnvironment* media);

// Frees what the rule set holds.
void cssRuleSetReset(CssRuleSet* set);

// A declaration that applies to an element, and the block that holds it.
struct CssCascaded {
    const CssDeclaration* declaration;
    const CssDeclarationBlock* block;
    PrCssProperty longhand;                // the longhand declared; stays flow-relative in its physical counterpart's
                                           // slot
    CssOrigin origin;                      // the origin of the declaration; an inline style's is the author's
    uint32_t order;                        // position in the order that the element's declarations apply; the later
                                           // wins
    const CssSheetData* PR_NULLABLE sheet; // the sheet of the rule that declares it, or null for no rule
};

// A slot of a scratch's matched properties cache, which holds computed styles keyed by the entries that the element
// matched. An element that matched the same entries, under a parent with the same inherited style, computes the same
// style, so it uses the cached one. Each slot holds what its style was computed relative to. It holds a reference to
// each of the parent's groups and custom properties that it compares, so that no address is reused while the slot holds
// the style. The style of an element whose own declarations change in place, such as an inline style or presentational
// hints, is never cached.
struct CssMatchedStyle {
    uint32_t hash;                  // hash of `matched`; 0 if the slot is empty
    Array<uint32_t> matched;        // the entries, in the order they apply
    const CssComputedStyle* PR_NULLABLE style;
    const CssStyleValues* PR_NULLABLE parentGroups[CssStyleGroup_Count];   // null for the root
    const CssCustomProperties* PR_NULLABLE parentCustom;
    CssFontBases rootFont;          // the root element's font bases, which root-relative lengths resolve against
    CssViewport viewport;           // the viewport, whose extent is compared if the style read it
    bool readsViewport;
    bool root;
    bool parentItems;               // true if the parent's children are items (CssComputedStyle::childrenAreItems)
    CssPseudo pseudo;               // what the style is of, which decides some computed values, such as content's

    // What the used appearance is decided from besides the entries: the element's kind of widget, or for a
    // pseudo-element, the used appearance of what originates it.
    DomWidget widget;
    CssAppearance parentAppearance;

    uint32_t used;                  // when the slot was last stored or taken, on the scratch's count of lookups
    uint64_t sharedHints;           // the number of the shared hints that applied (cssSharedHints), or 0 for none

    // The inherited groups that the style took from its parent unchanged, one bit each by CssStyleGroup. Another
    // element whose parent has other such groups takes the style with its own parent's groups in their place. The
    // style's other groups do not depend on them, unless `readsFont` is set: the style resolved a length relative to
    // its font, or mapped a flow-relative property by its writing mode, which the font group holds.
    uint16_t passThrough;
    bool readsFont;
};

// The number of slots in the cache, and the number that one hash may use: a set of kCssMatchedWays slots. A new style
// evicts the least recently used slot of its set. An element that alternates between two sets of entries whose hashes
// map to the same set has a style cached for each.
constexpr uint32_t kCssMatchedStyles = 1024;
constexpr uint32_t kCssMatchedWays = 4;

// A computed group, cached by what it was computed from: the kind of group, the declarations that won its properties,
// and what of the element's surroundings its values can read. Elements that differ elsewhere share it, as the items of
// several lists that each declare an inherited property share their font group.
struct CssCachedGroup {
    uint64_t hash;                  // hash of `key`; 0 if the slot is empty
    Array<uint64_t> key;            // what the group was computed from (cssCascadeStyle)
    const CssStyleValues* PR_NULLABLE values;

    // What computing the group read beyond its key, and what it left in the cascade's state, which a hit replays.
    const CssStyleValues* PR_NULLABLE parentValues; // the parent's group of the same kind, read if `inheritsReset`
    CssViewport viewport;                           // the viewport, whose extent is compared if `readsViewport`
    CssFontBases font;                              // the bases of font-relative lengths once the group is computed
    CssFontBases root;                              // the bases of root-relative lengths once the group is computed
    bool inheritsReset;                             // a property that does not inherit took the parent's value
    bool staticInline;                              // CssCascadeScratch::staticInline once the group is computed
    bool readsViewport;                             // a value resolved a length relative to the viewport's extent
    bool readsFont;                                 // a value resolved a length relative to the element's font
    bool readsMetrics;                              // a value read a font's metrics, so the group is not reused
};

// The number of slots in the cache of computed groups. A group evicts the group whose key hashes to the same slot.
constexpr uint32_t kCssCachedGroups = 4096;

// The size of the Bloom filter of the features of an element's ancestors, in 64-bit words.
constexpr uint32_t kCssAncestorFilterWords = 64;

// What the cascade of one element reuses from the previous one, on one thread. Threads that compute styles concurrently
// each have their own scratch.
struct CssCascadeScratch {
    Array<CssSelectorMatcher> matchers; // one for each of the rule set's lists, reused across a restyle's elements
    Array<uint32_t> epochs;             // for each matcher, the epoch in which it last learned of the tree
    uint32_t epoch;                     // incremented as the tree changes, past every epoch a matcher learned in
    const CssRuleSet* PR_NULLABLE set;  // the rule set that the matchers belong to, at `generation`
    uint32_t generation;
    Array<uint32_t> candidates;     // the entries that the last element matched, in the order they apply

    // What the matches read of nodes' children (CssSelectorMatcher::structure) since the scratch last forgot the tree.
    Array<CssStructureRead> structure;

    Array<CssCascaded> winners;     // for each property, the declaration that wins so far
    Array<PrCssProperty> declared;  // the properties that have a winner

    // A bit for each style group (1 << CssStyleGroup) that a property of `declared` belongs to. A group whose bit is
    // clear has no winner.
    uint32_t declaredGroups;

    uint32_t applied;               // number of declarations applied to the element so far, which orders them

    // The candidates that the cascade applies, in order: `candidates` itself, or for an element that matched entries
    // for one appearance, those of `candidates` that apply under `appearance`, copied to `filtered`. The cascade
    // decides an element's used appearance with `appearance` None, which no entry for one appearance can change.
    CssAppearance appearance;
    const uint32_t* PR_NULLABLE applying;
    uint32_t applyingCount;
    Array<uint32_t> filtered;

    Array<CssCascaded> custom;      // the custom properties declared; the last of each name wins
    HashIndex customIndex;          // positions in `custom` by cssCustomHash of the name
    CssValuePool substituted;       // values with their substitutions made
    CssValuePool urls;              // a value with its url()s resolved against its sheet's URL
    PrTextBuffer text = {};

    // The element's last shorthand declaration with a substitution, its substitutions made and its longhands parsed,
    // which each of its longhands reads. Null if the element has read none yet.
    const CssDeclaration* PR_NULLABLE shorthand;
    CssDeclarationBlock shorthandBlock;
    bool shorthandTainted;          // the shorthand's substitutions replaced an attr() (css-values-5/#attr-security)

    // The shorthand and the text that `shorthandBlock` was last parsed from, across elements. A shorthand that
    // substitutes to the same text, as one that reads the same custom properties does on every element, is not parsed
    // again.
    PrCssProperty parsedShorthand = PrCssProperty_Count;
    PrTextBuffer parsedText = {};

    // The element being styled, or the originating element of the pseudo-element being styled, whose attributes attr()
    // reads.
    const PrDomElement* PR_NULLABLE element;
    bool inheritsReset;             // true if the style uses the parent's value of a property that does not inherit
    uint16_t passThrough;           // the inherited groups that the style takes from its parent with no declaration
    bool staticInline;              // true if the element is out of flow and was blockified from an inline-level box

    // True if a style computed since the scratch last forgot the tree resolved a length relative to the viewport.
    bool readsViewport;

    // A Bloom filter of the features of the ancestors of the children of `ancestorsOf`, as the tree was at
    // `ancestorsEpoch`. It holds the ids, classes and types of every ancestor. If an element can match an entry, none
    // of the entry's `ancestors` is missing from the filter.
    uint64_t ancestorBits[kCssAncestorFilterWords];
    const PrDomNode* PR_NULLABLE ancestorsOf;
    uint32_t ancestorsEpoch;

    // The bucket hashes of the element matched last, which the matches of its pseudo-elements reuse: its id's, its
    // classes', its type's and its attributes'. They hold for `featuresOf` while the tree is as it was at
    // `featuresEpoch`.
    Array<uint32_t> featureHashes;
    const PrDomElement* PR_NULLABLE featuresOf;
    uint32_t featuresEpoch;

    // The cache of computed groups (CssCachedGroup), for the rule set at `generation`, created on first use. It is used
    // while `cacheGroups` is set, which holds while the element's declarations all come from the rule set.
    Array<CssCachedGroup> cachedGroups;
    Array<uint64_t> groupKey;       // the key of the group being computed
    uint64_t groupSalt;             // the number of the element's shared hints (cssSharedHints), or 0 for none
    bool cacheGroups;

    // The matched properties cache, for the rule set at `generation`, created on first use.
    Array<CssMatchedStyle> matchedStyles;
    uint32_t matchedLookups;        // number of lookups, which orders the slots of a set by use
    uint32_t matchedHits;           // number of styles taken from the cache; the caller reads and resets it
};

// What an element's style was last computed from: the entries that it matched, the parent's style, the root's font and
// the viewport, of one rule set. A restyle that finds the same reuses the style whole, whatever the matched properties
// cache holds, so an element whose style is costly to compute, such as a root that declares many custom properties,
// does not depend on the cache keeping it. The memo holds a reference to the style and to the parent's style. A style
// whose declarations do not all come from the rule set, or that measured a font, is not kept.
struct CssCascadeMemo {
    const CssComputedStyle* PR_NULLABLE style;  // null if no style is kept
    const CssComputedStyle* PR_NULLABLE parent;
    Array<uint32_t> matched;                    // the entries, in the order they apply
    uint32_t generation;                        // the generation of the rule set (CssRuleSet::generation)
    CssFontBases rootFont;
    CssViewport viewport;                       // compared if the style read it
    bool readsViewport;
    DomWidget widget;
    uint64_t sharedHints;                       // the number of the shared hints that applied, or 0 for none
};

// Releases the styles that a memo holds, and empties it. It keeps the storage of its entries.
void cssCascadeMemoClear(CssCascadeMemo* memo);

// Frees what a memo holds.
void cssCascadeMemoReset(CssCascadeMemo* memo);

// The inputs that style an element relative to its surroundings.
struct CssCascadeInput {
    const PrDomElement* element;                // the element, or the originating element of `pseudo`
    const CssComputedStyle* PR_NULLABLE parent; // the parent element's style, or null for the root
    CssFontBases rootFont;                      // the root element's font bases; unused by the root's own style
    CssViewport viewport;
    bool fresh;                     // true to compute from the declarations, bypassing the matched properties cache

    // The entries that the element matched when its style was last computed, in the order they apply, if nothing that
    // it is matched against has changed since. The cascade uses them in place of matching. Null to match.
    const Array<uint32_t>* PR_NULLABLE matched;

    // The pseudo-element of `element` to style, or CssPseudo_None for the element. A pseudo-element inherits from what
    // originates it (css-pseudo-4/#treelike), whose style is `parent`: the element's, or for a marker of a ::before or
    // an ::after, that pseudo-element's.
    CssPseudo pseudo;

    // True to apply the user agent's declarations alone, which a revert in the animation origin rolls back to
    // (css-cascade-5/#default). Such a style is computed whole, with no cache.
    bool userAgentOnly;

    // The element's memo of its last style, which the cascade reads and updates, or null.
    CssCascadeMemo* PR_NULLABLE memo;
};

// Returns the computed style of an element, or of its pseudo-element, with one reference that the caller holds. The
// style comes from the element's memo (CssCascadeInput::memo), from the scratch's matched properties cache
// (CssMatchedStyle), or is computed and cached. The element's inline style is parsed if it has a style attribute
// (cssElementInlineStyle). Safe on several threads at once, each with its own builder and scratch, while the document
// and the rule set do not change.
const CssComputedStyle* cssCascadeStyle(const CssRuleSet* set, CssStyleBuilder* builder, CssCascadeScratch* scratch,
    const CssCascadeInput& input);

// Returns the pseudo-elements among ::first-line and ::first-letter that `element` originates, as bits by CssPseudo. An
// element originates one if an entry of `set` for that pseudo-element matches it. If `set` has no such entries, returns
// 0 without matching.
uint32_t cssCascadeOriginating(const CssRuleSet* set, CssCascadeScratch* scratch, const PrDomElement* element);

// Returns the pseudo-elements of `element` that may generate a box, as bits by CssPseudo: ::before and ::after where
// they match an entry whose declarations set content. Without one, content computes to none, which generates no box
// (css-content-3/#valdef-content-none), so a rule such as `*::before { box-sizing: border-box }`, which matches every
// element, costs no element a style.
uint32_t cssCascadeGenerating(const CssRuleSet* set, CssCascadeScratch* scratch, const PrDomElement* element);

// Forgets what the matchers learned of the tree, which has changed since, and what the styles read of the structure and
// the viewport. Each matcher forgets when it is next used.
void cssCascadeScratchForget(CssCascadeScratch* scratch);

// Frees what the scratch holds.
void cssCascadeScratchReset(CssCascadeScratch* scratch);

// Builds what the cascade reads of the element besides the rules: its inline style, parsed if it has a style attribute,
// and its presentational hints (css_hints.h). The cascade cannot build them while it runs on several threads at once,
// so the code that computes an element's style calls this first, on one thread.
void cssStylePrepare(PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
