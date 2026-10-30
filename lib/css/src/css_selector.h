// Selectors
//
// selectors-4 - https://drafts.csswg.org/selectors-4/
// selectors-5 - https://drafts.csswg.org/selectors-5/
// css-nesting-1 - https://drafts.csswg.org/css-nesting-1/
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// A selector list is compiled to records, which matching reads.

#pragma once

#include "pr/array.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "css/src/css_pseudo.h"
#include "css/src/css_syntax.h"
#include "css/src/css_value.h"
#include "dom/src/dom_tag.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssParser;
struct CssSheetData;

// The index that a record field holds if it refers to nothing.
constexpr uint32_t kCssSelectorNone = 0xffffffffu;

// The name that a field holds if the selector writes `*` for it.
constexpr uint32_t kCssSelectorAll = 0xfffffffeu;

// ---------------------------------------------------------------------------------------------------------------------
// Records

// A name that a simple selector compares against: an identifier or a string, with escapes resolved.
struct CssSelectorName {
    uint32_t text;                  // the offset in the records' text of the name as written
    uint32_t lowered;               // the offset of the ASCII lowercase form, or `text` if the two are the same
    uint32_t size;                  // size in bytes, which is the same for both forms
    uint32_t hash;                  // domAttributeHash of the name, the same hash as DomAttr::hash
    bool string;                    // true if written as a <string>
};

// A namespace prefix and the namespace that an @namespace rule declares for it (css-namespaces-3/#namespace-prefix).
// The default namespace has the null prefix.
struct CssSelectorNamespace {
    uint32_t prefix;                // a name, or kCssSelectorNone for the default namespace
    uint32_t url;                   // a name that holds the namespace

    // The namespace, if the specs name it, which is the same in every document, so a match compares it with a node's
    // without reading the URL. PrDomNamespace_Other for any other.
    PrDomNamespace named;
};

// What a type selector, a universal selector or an attribute selector writes before its `|` (selectors-4/#type-nmsp,
// selectors-4/#attrnmsp).
enum {
    // No `|`. For an element: the default namespace if the sheet declares one, and any namespace otherwise. For an
    // attribute: no namespace.
    CssNamespacePrefix_None,

    CssNamespacePrefix_Any,         // `*|`
    CssNamespacePrefix_Empty,       // `|`: no namespace
    CssNamespacePrefix_Named,       // `ns|`
};
typedef uint8_t CssNamespacePrefix;

// The kinds of simple selector (selectors-4/#structure).
enum {
    CssSimpleKind_Type,            // `div` (selectors-4/#type-selectors)
    CssSimpleKind_Universal,       // `*` (selectors-4/#the-universal-selector)
    CssSimpleKind_Id,              // `#a` (selectors-4/#id-selectors)
    CssSimpleKind_Class,           // `.a` (selectors-4/#class-html)
    CssSimpleKind_Attribute,       // `[a]`, `[a=b i]` (selectors-4/#attribute-selectors)
    CssSimpleKind_PseudoClass,     // `:hover`, `:is(a, b)` (selectors-4/#pseudo-classes)
    CssSimpleKind_PseudoElement,   // `::before`, `:before`, `::part(a)` (selectors-4/#pseudo-elements)
    CssSimpleKind_Nesting,         // `&` (css-nesting-1/#nest-selector)
};
typedef uint8_t CssSimpleKind;

// How an attribute selector compares the attribute's value (selectors-4/#attribute-selectors).
enum {
    CssAttributeMatch_Exists,       // `[a]`
    CssAttributeMatch_Equals,       // `[a=b]`
    CssAttributeMatch_Includes,     // `[a~=b]`: b is one of the value's whitespace-separated words
    CssAttributeMatch_DashMatch,    // `[a|=b]`: b, or b followed by a hyphen and the rest
    CssAttributeMatch_Prefix,       // `[a^=b]`
    CssAttributeMatch_Suffix,       // `[a$=b]`
    CssAttributeMatch_Substring,    // `[a*=b]`
};
typedef uint8_t CssAttributeMatch;

// The case sensitivity of an attribute selector's value comparison (selectors-4/#attribute-case).
enum {
    CssAttributeCase_Default,       // no flag: compared exactly

    // No flag, and a name listed in html/#case-sensitivity-of-selectors. The comparison is ASCII case-insensitive for
    // an attribute in no namespace of an HTML element, and exact for any other attribute.
    CssAttributeCase_Html,

    CssAttributeCase_Insensitive,   // `i`: compared ASCII case-insensitively
    CssAttributeCase_Sensitive,     // `s`: compared exactly
};
typedef uint8_t CssAttributeCase;

// One simple selector. Each field serves the kinds named in its comment.
struct CssSimpleSelector {
    CssSimpleKind kind;
    CssNamespacePrefix prefix;      // Type, Universal, Attribute
    CssAttributeMatch match;        // Attribute
    CssAttributeCase letterCase;    // Attribute
    CssPseudoClass pseudoClass;     // PseudoClass
    CssPseudoElement pseudoElement; // PseudoElement

    // Type: the tag of the name's ASCII lowercase form, or PrDomTag_Unknown if it has none.
    PrDomTag tag;

    // Type, Universal, Attribute: the namespace that the prefix resolves to, or kCssSelectorNone if it names none.
    uint32_t space;

    // Type, Id, Class, Attribute: a name. PseudoElement: the name of an unknown -webkit- pseudo-element
    // (selectors-4/#unknown--webkit--pseudo-elements).
    uint32_t name;

    uint32_t value;                 // Attribute: the name to compare against, unless `match` is Exists
    uint32_t argument;              // a functional PseudoClass or PseudoElement: the argument
    uint32_t written;               // the position in the compound as written, counted from zero
};

// A run of complex selectors (selectors-4/#selector-list).
struct CssSelectorList {
    uint32_t first;
    uint32_t count;
};

// The argument of a functional pseudo-class or pseudo-element. The pseudo determines which fields the argument uses.
struct CssSelectorArgument {
    // The selector list of a pseudo that takes one, such as :is() or ::slotted(), or the `of` list of :nth-child(). The
    // list is empty if it is not written.
    CssSelectorList selectors;

    int32_t a;                      // An+B (css-syntax-3/#anb-microsyntax)
    int32_t b;

    // The index of the argument's first identifier or string in the names, or of its first integer in the numbers. For
    // a <pt-name-and-class-selector>, the index of its first class.
    uint32_t items;

    uint32_t itemCount;

    // The name of a <pt-name-and-class-selector> (css-view-transitions-2/#pseudo-element-class-additions).
    // kCssSelectorAll for its `*` or for the `*` of ::scroll-button(), and kCssSelectorNone if there is neither.
    uint32_t name;
};

// What joins a compound selector to the one written before it (selectors-4/#combinators).
enum {
    CssCombinator_None,              // no combinator: the compound is the leftmost
    CssCombinator_Descendant,        // whitespace
    CssCombinator_Child,             // `>`
    CssCombinator_NextSibling,       // `+`
    CssCombinator_SubsequentSibling, // `~`
    CssCombinator_Column,            // `||` (selectors-5/#the-column-combinator)
    CssCombinator_Reference,         // `/attr/` (selectors-5/#idref-combinators)
};
typedef uint8_t CssCombinator;

// A compound selector: its simple selectors, and how the compound relates to the compound written before it. The
// leftmost compound of a relative selector relates to the selector's anchor instead (selectors-4/#relative).
struct CssCompoundSelector {
    // The index of the first of the `simpleCount` simple selectors, in test order. Cheaper selectors come first, then
    // the selectors that read structure, then the selectors after a pseudo-element in written order.
    uint32_t simple;

    uint32_t simpleCount;

    // Reference: the attribute selector that names the attribute. kCssSelectorNone for every other combinator.
    uint32_t reference;

    CssCombinator combinator;

    // The `placedCount` simple selectors from `placed` read the element's position among its siblings or its children.
    // They are the structural pseudo-classes, and the pseudo-classes whose arguments hold a structural pseudo-class.
    // They follow the other selectors that come before any pseudo-element. A match tests them only after the compounds
    // left of this one match, so it reads the structure only if the structure decides the match.
    uint8_t placedCount;
    uint16_t placed;
};

// A specificity (selectors-4/#specificity-rules) as one integer, which compares in the same order as specificities. It
// packs the counts of ID selectors, class-like selectors and type-like selectors, in kCssSpecificityCountBits bits
// each. Each count saturates.
typedef uint32_t CssSpecificity;

// The width of each count, and the value at which each count saturates.
constexpr uint32_t kCssSpecificityCountBits = 10;
constexpr uint32_t kCssSpecificityMaxCount = (1u << kCssSpecificityCountBits) - 1;

// Returns the specificity of `ids` ID selectors, `classes` class-like selectors and `types` type-like selectors.
constexpr CssSpecificity cssSpecificity(uint32_t ids, uint32_t classes, uint32_t types) {
    return (ids < kCssSpecificityMaxCount ? ids : kCssSpecificityMaxCount) << (2 * kCssSpecificityCountBits) |
        (classes < kCssSpecificityMaxCount ? classes : kCssSpecificityMaxCount) << kCssSpecificityCountBits |
        (types < kCssSpecificityMaxCount ? types : kCssSpecificityMaxCount);
}

// Properties of a complex selector beyond its compounds.
enum {
    // The selector starts with a combinator, which relates it to an anchor (selectors-4/#relative).
    CssComplexFlag_Relative = 1 << 0,

    CssComplexFlag_ImpliedNesting = 1 << 1, // the anchor is an unwritten `&` (css-nesting-1/#nesting)

    // The anchor is a `:scope` that the source did not write (css-nesting-1/#nesting-at-scope).
    CssComplexFlag_ImpliedScope = 1 << 2,

    CssComplexFlag_Nesting = 1 << 3,        // the selector holds an `&`, written or implied, at any depth

    // An item of a forgiving selector list that is not a selector. It is kept as written, and matches nothing
    // (selectors-4/#forgiving-selector, csswg-drafts#8356).
    CssComplexFlag_Invalid = 1 << 4,
};
typedef uint8_t CssComplexFlags;

// The positions that a selector's combinators allow for its subject, when the selector is matched relative to an anchor
// (selectors-4/#relational).
enum {
    CssReach_Descendant,            // a descendant of the anchor
    CssReach_Sibling,               // a later sibling of the anchor, or a descendant of one
    CssReach_Tree,                  // anywhere in the anchor's tree, beyond a reference combinator
};
typedef uint8_t CssReach;

// The count for a reach that has no bound.
constexpr uint32_t kCssReachAny = 0xffffffffu;

// A complex selector: compound selectors joined by combinators (selectors-4/#structure).
struct CssComplexSelector {
    // The index of the first of the `compoundCount` compounds, which is the subject's compound. The compounds run right
    // to left.
    uint32_t compound;

    uint32_t compoundCount;
    CssSpecificity specificity;

    // What the selector selects, or CssPseudoElement_None for an element (selectors-4/#pseudo-element-attachment).
    CssPseudoElement pseudoElement;

    CssComplexFlags flags;

    // The positions that the subject may have when the selector is matched relative to an anchor. A leftmost compound
    // without a combinator counts as having a descendant combinator.
    CssReach reach;

    uint32_t siblings;              // Sibling: which later sibling, counted from one, or kCssReachAny

    // Descendant, Sibling: the exact distance below the anchor or the sibling, or kCssReachAny.
    uint32_t depth;

    uint32_t text;                  // Invalid: the name that holds the item as written
};

// The records of a selector list during compilation.
struct CssSelectorPool {
    Array<CssComplexSelector> complex;
    Array<CssCompoundSelector> compounds;
    Array<CssSimpleSelector> simple;
    Array<CssSelectorArgument> arguments;
    Array<CssSelectorNamespace> namespaces;
    Array<CssSelectorName> names;
    Array<int32_t> numbers;         // integer arguments, such as the levels of :heading()
    TextBuffer text;                // the bytes of the names
};

// Frees everything that the pool owns and leaves it usable.
void cssSelectorPoolReset(CssSelectorPool* pool);

// A selector list and its records, in one allocation. Each complex selector is a run of compound selectors from the
// subject leftward, and each compound selector is a run of simple selectors. Matching reads the compounds right to
// left, the order in which they are stored. It reads a compound's simple selectors in stored order, which is not the
// order in which they were written. An argument that is itself a selector list, as in `:is(a, b)`, is a CssSelectorList
// of the same records. A list is compiled into a CssSelectorPool before it is copied into one allocation.
struct CssSelectors {
    CssSelectorList list;           // the list, which starts at the first of the complex selectors
    uint32_t size;                  // the size in bytes of the allocation that holds the list and its records
    uint32_t complexCount;          // number of complex selectors in the list and its arguments
    bool readsHost;                 // a selector of the list or of its arguments uses :host, :host() or :host-context()
    CssComplexSelector* complex;
    CssCompoundSelector* compounds;
    CssSimpleSelector* simple;
    CssSelectorArgument* arguments;
    CssSelectorNamespace* namespaces;
    CssSelectorName* names;
    int32_t* numbers;
    char* text;
};

// What a style rule's selector styles: an element, one of the pseudo-elements whose boxes the presentation generates
// (css-pseudo-4/#treelike), or the first line or first letter of a block container's text
// (css-pseudo-4/#first-line-pseudo, css-pseudo-4/#first-letter-pseudo). A marker belongs to an element, a ::before or
// an ::after (css-lists-3/#marker-pseudo). ::field-content is the part of a text control that holds its editable text
// (css-forms-1).
enum {
    CssPseudo_None,                 // the element itself
    CssPseudo_Before,
    CssPseudo_After,
    CssPseudo_Marker,
    CssPseudo_FieldContent,
    CssPseudo_BeforeMarker,
    CssPseudo_AfterMarker,
    CssPseudo_FirstLine,
    CssPseudo_FirstLetter,
    CssPseudo_Count,
};
typedef uint8_t CssPseudo;

// Returns true if `pseudo` is a ::marker: an element's, a ::before's or an ::after's.
inline bool cssPseudoIsMarker(CssPseudo pseudo) {
    return pseudo == CssPseudo_Marker || pseudo == CssPseudo_BeforeMarker || pseudo == CssPseudo_AfterMarker;
}

// The pseudo-element as a selector names it, such as "::before::marker", or the empty view for CssPseudo_None.
PrStringView cssPseudoName(CssPseudo pseudo);

// Returns what the complex selector at `complex` styles, or CssPseudo_Count if it styles nothing that the cascade
// computes: a pseudo-element that the engine does not generate, or a pseudo-class that follows a pseudo-element.
CssPseudo cssSelectorPseudo(const PrCssSelectors* selectors, uint32_t complex);

// Frees the selectors.
void cssSelectorsDestroy(PrCssSelectors* PR_NULLABLE selectors);

// Copies the selectors into an allocation of their own.
PrCssSelectors* cssSelectorsCopy(const PrCssSelectors* selectors);

// Returns a name from the records, as it was written or in ASCII lowercase.
PrStringView cssSelectorName(const PrCssSelectors* selectors, uint32_t name);
PrStringView cssSelectorNameLowered(const PrCssSelectors* selectors, uint32_t name);

// Sets the specificity of every complex selector (selectors-4/#specificity-rules). `parent` is the list of the nearest
// style rule that encloses the selectors' own rule, and `&` represents it. If there is none, `&` counts as :scope
// (css-nesting-1/#nest-selector).
void cssSelectorsSpecify(PrCssSelectors* selectors, const PrCssSelectors* PR_NULLABLE parent);

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

// The context in which a selector list is written. The context decides what the list is relative to.
enum {
    CssSelectorNesting_None,        // a top-level style rule, or a string the DOM matches

    // A style rule nested in a style rule. The list is relative, with `&` implied if the source writes none
    // (css-nesting-1/#nesting).
    CssSelectorNesting_Style,

    // A style rule directly inside @scope. The list is relative, with `:scope` implied if the source writes no `&`
    // (css-nesting-1/#nesting-at-scope).
    CssSelectorNesting_Scope,
};
typedef uint8_t CssSelectorNesting;

// What parsing a selector list depends on beyond its tokens.
struct CssSelectorContext {
    // The sheet whose @namespace rules declare the prefixes (css-namespaces-3/#at-ruledef-namespace). If null, no
    // prefixes are declared (dom/#scope-match-a-selectors-string).
    const CssSheetData* PR_NULLABLE sheet;

    // The selectors that `&` represents. If null, `&` is `:scope` (css-nesting-1/#nest-selector).
    const PrCssSelectors* PR_NULLABLE parent;

    CssSelectorNesting nesting;
};

// Parses `span` as a <selector-list>, or as a <relative-selector-list> if `context` nests it
// (selectors-4/#parse-a-selector), and sets the specificities. The parse uses the scratch of `parser`. Returns null for
// tokens that are not a valid selector list (selectors-4/#invalid).
PrCssSelectors* PR_NULLABLE cssParseSelectors(CssParser* parser, const CssTokenStream* ts,
    CssTokenSpan span, const CssSelectorContext* context);

// Tokenizes `text` into the scratch of `parser`, then parses it in the same way as cssParseSelectors
// (cssom-1/#parse-a-group-of-selectors).
PrCssSelectors* PR_NULLABLE cssParseSelectorText(CssParser* parser, PrStringView text, const CssSelectorContext* context);

// Returns true if a node of a parse is a complex selector of a list, relative or not. A prelude holds its selectors as
// such nodes.
bool cssIsComplexSelectorNode(const CssValueNode* value);

// Compiles into one list, in order, the complex selectors that the parsed prelude `span` holds in the pool of `parser`.
// Returns null if the prelude holds none. The prelude's own records stay as the parse left them.
PrCssSelectors* PR_NULLABLE cssCompilePreludeSelectors(CssParser* parser, CssValueSpan span,
    const CssSheetData* PR_NULLABLE sheet);

// ---------------------------------------------------------------------------------------------------------------------
// Serializing

// Serializes the selector list, joined with ", " (cssom-1/#serialize-a-group-of-selectors). A relative selector with an
// implied `&` is written with the `&` (css-nesting-1/#cssom).
void cssSerializeSelectors(TextBuffer* out, const PrCssSelectors* selectors);

// Serializes `count` complex selectors of the list, starting at `first`, in the same form as cssSerializeSelectors.
void cssSerializeSelectorRange(TextBuffer* out, const PrCssSelectors* selectors, uint32_t first, uint32_t count);

} // namespace pr_impl

// C opaque types
struct PrCssSelectors : pr_impl::CssSelectors {};

PR_ASSUME_NONNULL_END
