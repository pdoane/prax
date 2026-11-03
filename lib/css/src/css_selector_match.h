// Selector matching
//
// selectors-4 - https://drafts.csswg.org/selectors-4/
// selectors-5 - https://drafts.csswg.org/selectors-5/
// dom - https://dom.spec.whatwg.org/
// html - https://html.spec.whatwg.org/
//
// Matches a selector list's records against the node tree, right to left. A compound is tested against an element, and
// its combinator names the elements that the compound to its left is tested against next. An element's state is read
// from its attributes and the surrounding tree each time a selector needs it, except for what a matcher keeps
// (CssSelectorMatcher).

#pragma once

#include "pr/dom.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "css/src/css_selector.h"
#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses `text` as the DOM parses a selectors string (selectors-4/#parse-a-selector). No namespace prefix is declared,
// and `&` is :scope. Returns null for text that is not a selector list.
PrCssSelectors* PR_NULLABLE cssParseDomSelectors(PrStringView text);

// A selector list that a document's queries parsed, keyed by the list's text.
struct CssCachedSelectors {
    uint32_t hash;
    uint64_t used;              // the cache's use count at the entry's most recent use
    PrRawString text;
    PrCssSelectors* selectors;
};

// The selector lists that a document keeps for its later queries.
struct CssSelectorCache {
    RawArray<CssCachedSelectors> entries;
    HashIndex index;
    uint64_t uses;
};

// The largest number of siblings that a count walks back toward the element that it counted last. If the walk would be
// longer, the count counts all the siblings instead.
constexpr uint32_t kCssNthWalk = 32;

// The number of ancestors that a descendant combinator passes before it keeps what it learns about each ancestor.
constexpr uint32_t kCssShallowWalk = 4;

// The number of selector lists that a document keeps.
constexpr uint32_t kCssSelectorCacheSize = 64;

// Returns the list that cssParseDomSelectors parses from `text`. The list comes from the document's cache if the
// document parsed `text` before. The list belongs to the document, and stays valid until the next call.
const PrCssSelectors* PR_NULLABLE cssDomSelectors(const PrDomDocument* document, PrStringView text);

// Frees the cache and the lists that it keeps.
void cssSelectorCacheDestroy(CssSelectorCache* PR_NULLABLE cache);

// What a matcher has learned about one element: the element's position among its siblings for a count of them, or the
// result of a search or a walk from the element.
struct CssMatchMemo {
    const PrDomElement* PR_NULLABLE element; // null for a slot that holds none
    uint32_t key;                            // identifies the count, search or walk
    uint32_t value;
};

// The memos of a matcher, open-addressed by element and key.
struct CssMatchMemos {
    CssMatchMemo* PR_NULLABLE slots;
    uint32_t mask;
    uint32_t count;
};

// The element whose position a count of siblings counted last, while a matcher visits the siblings in order.
struct CssNthRun {
    uint32_t key;               // identifies the count, in the form of a memo's key
    const PrDomElement* element;
    uint32_t position;          // counted from one at the first sibling
    uint32_t count;             // number of the element's siblings that the count counts, or zero until requested
};

// The last walk that a descendant combinator took from one of its compounds: the ancestor that the walk started at, and
// the walk's result.
struct CssAncestorWalk {
    uint32_t key;
    const PrDomElement* start;
    uint32_t result;
};

// A class name of a selector list, and its atom in the document being matched. kAtomNone if no element of the document
// has that class.
struct CssClassAtom {
    uint32_t name;
    Atom atom;
};

// A lowered local name of a selector list, and its tag in the document being matched. PrDomTag_Unknown if no element of
// the document has that name.
struct CssLocalName {
    uint32_t name;
    DomTagId tag;
};

// The tag that a subject filter holds if its compound names no type that the filter can test.
constexpr DomTagId kCssAnyTag = 0xffffffffu;

// What one selector of a list says about the elements that it can match. An element must pass the filter before the
// selector is matched against it. Outside quirks mode, IDs and classes compare exactly, and the filter tests them using
// the ID map and the atoms that the document keeps.
struct CssSubjectFilter {
    uint32_t complex;           // index of the selector among the records' complex selectors

    // The ID that the selector's subject compound names. The null view if it names none, or if IDs fold case.
    PrStringView id;

    // The atom of a class that the subject compound names. kAtomNone if it names none, or if classes fold case.
    Atom atom;

    // The tag that the type selector of the subject compound names, which an HTML element of an HTML document must
    // have. kCssAnyTag if there is none.
    DomTagId tag;

    // The selector is one compound that the filter tests in full, so an element that passes the filter matches.
    bool whole;
};

// A node, and what a match read of its children.
struct CssStructureRead {
    const PrDomNode* node;
    DomStructureFlags flags;
};

// The consumer of a match's result. It decides what the match may see of an element's state.
enum {
    CssMatchPurpose_Script,   // a script's query, which sees every link as unvisited (selectors-4/#link)
    CssMatchPurpose_Style,    // the style of the document's rendering
};
typedef uint8_t CssMatchPurpose;

// Matches a selector list against the elements of one tree. A matcher caches what it learns about an element for the
// elements matched after it. The cache is valid only while the tree does not change, so a matcher lives no longer than
// one query, during which nothing changes the tree.
struct CssSelectorMatcher {
    const PrCssSelectors* selectors;
    CssMatchPurpose purpose;
    const PrDomDocument* document;           // the document whose elements the matcher matches

    // The scoping root (selectors-4/#the-scope-pseudo), or null if :scope is :root.
    const PrDomElement* PR_NULLABLE scope;

    // The shadow host of the tree that the selectors match in, or null for a document's tree. In a shadow tree's
    // context the host is the parent of the tree's top elements, and it is featureless: only :host, :host() and
    // :host-context() match it (css-shadow-1/#host-element-in-tree). The host is null unless the selectors read it,
    // since no other selector can match it, and a walk of ancestors that stops at the shadow root finds the same.
    const PrDomElement* PR_NULLABLE host;
    bool readsHost;                 // the selectors use :host, :host() or :host-context()
    bool hostArgument;              // true while the argument of :host() or :host-context() matches the host itself

    // True if the document is in quirks mode, in which IDs and classes compare ASCII case-insensitively.
    bool quirks;

    // One for each selector of the list that can match an element of the document, in the list's order.
    RawArray<CssSubjectFilter> subjects;

    // An ID that a compound left of the subject names, if the list is one selector and only child and descendant
    // combinators join the two compounds. The subject is a descendant of the element with that ID. The null view
    // otherwise.
    PrStringView scopeId;

    CssMatchMemos memos;

    // The ancestors beyond kCssShallowWalk that a descendant combinator has passed, in the order that it passed them,
    // and the last walk from each compound that follows a descendant combinator.
    RawArray<const PrDomElement* PR_NONNULL> walk;
    RawArray<CssAncestorWalk> walks;

    RawArray<CssNthRun> runs;                 // the last run of each count, and of each type for a count of a type
    RawArray<CssClassAtom> classes;           // the class names the matcher has compared, added on first use

    // The local names that the matcher has compared against an element with no tag, added on first use.
    RawArray<CssLocalName> localNames;

    // The array where a match records what it read of a node's children. A match records reads of the parent of an
    // element that a structural pseudo-class or a sibling combinator tested, and of an element that :empty tested. Null
    // to record nothing.
    RawArray<CssStructureRead>* PR_NULLABLE structure;

    // The element that the running match is for. A read on behalf of another element marks it
    // (DomStructureFlag_Descendants).
    const PrDomElement* PR_NULLABLE target;

    // True while the matcher matches selectors of a pseudo-element against its originating element, which the subject
    // compound's selectors before the pseudo-element test. The pseudo-element selectors then match. The caller has
    // chosen selectors of the pseudo-element being styled. Otherwise a pseudo-element selector matches no element.
    bool pseudoElements;
};

// Initializes a matcher for a script's query, which matches in the tree of `tree`: in a shadow tree's context if its
// root is a shadow root (css-shadow-1/#selectors-data-model). A null `tree` matches in the document's tree.
void cssSelectorMatcherInit(CssSelectorMatcher* matcher, const PrCssSelectors* selectors,
    const PrDomElement* PR_NULLABLE scope, const PrDomDocument* document, const PrDomNode* PR_NULLABLE tree);

// Frees what the matcher caches.
void cssSelectorMatcherReset(CssSelectorMatcher* matcher);

// Forgets what the matcher learned of the tree and its document, which have changed since. Keeps what it knows about
// its selectors alone.
void cssSelectorMatcherForget(CssSelectorMatcher* matcher);

// Returns true if `element` matches a selector of the list (selectors-4/#match-a-selector-against-an-element).
bool cssSelectorMatcherMatches(CssSelectorMatcher* matcher, const PrDomElement* element);

// Returns true if `element` matches the selector at `complex` among the list's records.
bool cssSelectorMatcherMatchesComplex(CssSelectorMatcher* matcher, uint32_t complex, const PrDomElement* element);

// Returns true if `element`, assigned to `slot` directly or through slots assigned to slots, matches the selector at
// `complex`, whose subject ends in ::slotted() (css-shadow-1/#slotted-pseudo): `slot` matches the rest of the selector
// in the matcher's tree, and `element` matches the argument of ::slotted().
bool cssSelectorMatcherMatchesSlotted(CssSelectorMatcher* matcher, uint32_t complex, const PrDomElement* element,
    const PrDomElement* slot);

// Returns the first element after `after` in tree order, among the descendants of `root`, that matches a selector of
// the list. Returns null if none does (dom/#scope-match-a-selectors-string). `after` is `root` or one of its
// descendants.
PrDomElement* PR_NULLABLE cssSelectorMatcherNext(CssSelectorMatcher* matcher, PrDomNode* root, PrDomNode* after);

// Returns true if `element` matches a selector of the list, using a matcher of its own. :scope is `scope`, or :root if
// `scope` is null.
bool cssSelectorsMatch(const PrCssSelectors* selectors, const PrDomElement* element, const PrDomElement* PR_NULLABLE scope);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
