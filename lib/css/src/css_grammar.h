// Value Definition Syntax
//
// css-values-4 - https://drafts.csswg.org/css-values-4/
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// The grammars of every property, descriptor, type and prelude, as one generated array of nodes. Each grammar is a
// preorder tree in the array.

#pragma once

#include "pr/base.h"

#include "css/src/css_keyword.h"
#include "css/src/css_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// The type of a grammar node. A comment beside a type gives an example as a grammar writes it.
enum {
    // An ident literal. `value` is a PrCssKeyword, which the token's value, with escapes resolved, must name.
    CssGrammarType_Keyword,

    CssGrammarType_Literal,     // punctuation, such as ',' '/' '(' '{', with the character in `value`

    // A numeric literal, such as 0deg or 90: a number or a dimension of that value. `value` indexes kCssGrammarNumbers.
    CssGrammarType_Number,

    CssGrammarType_AtKeyword,   // @location, with a PrCssKeyword in `value`

    // <@stylistic>: a whole at-rule, which cannot appear in a run of component values. `value` is a CssAtRule.
    CssGrammarType_AtRule,

    // <length>. `value` is a CssValueDef. A type with a child is parameterized, as in <boolean-expr[ <if-test> ]>
    // (css-values-5/#boolean).
    CssGrammarType_Type,

    CssGrammarType_Property,    // <'border-width'>, with a PrCssProperty in `value`

    // A production that the specs write in prose. `value` is a CssTerminal (css_terminal.h).
    CssGrammarType_Terminal,

    // Combinators, in order of increasing precedence (css-values-4/#component-combinators)
    CssGrammarType_OneOf,       // a | b
    CssGrammarType_AnyOf,       // a || b
    CssGrammarType_AllOf,       // a && b
    CssGrammarType_Sequence,    // a b

    // A functional notation. `value` is its name as a PrCssKeyword. The one child is the grammar of the arguments
    // between the parentheses.
    CssGrammarType_Function,

    // A | whose alternatives are all bare keywords. The children are those keywords.
    CssGrammarType_KeywordSet,

    // A component of a shorthand's grammar that sets a longhand, or the longhands of a shorthand within it
    // (css-cascade-5/#shorthand-property). `value` is that PrCssProperty. The one child is the component.
    CssGrammarType_Longhand,
};
typedef uint8_t CssGrammarType;

// How a node repeats, and how its match is stored.
enum {
    CssGrammarFlag_Comma = 1 << 0,       // # multiplier: repetitions are comma separated
    CssGrammarFlag_Required = 1 << 1,    // ! multiplier: the group must produce at least one value

    // # multiplier: no item can contain a comma of its own, so the commas separate the items.
    CssGrammarFlag_Delimited = 1 << 2,

    // A reference to a production whose match is stored in a form that the production's prose defines.
    CssGrammarFlag_Folds = 1 << 3,
};
typedef uint8_t CssGrammarFlags;

// The value of `max` if a multiplier has no upper bound.
constexpr uint16_t kCssGrammarUnbounded = 0xffff;

// Everything that modifies a reference: how many times it may repeat, and the bounds on its value. Modifiers are
// interned, and a node holds the index of its own.
struct CssGrammarModifier {
    uint16_t min;
    uint16_t max;
    CssGrammarFlags flags;
    uint8_t range;         // index into kCssGrammarRanges
};

static_assert(sizeof(CssGrammarModifier) == 6);

// The modifier of a node that is taken once, with no flags and no range.
enum { CssGrammarModifier_None = 0 };

// The interned modifiers.
extern const CssGrammarModifier kCssGrammarModifiers[];
extern const uint32_t kCssGrammarModifierCount;

// Index into kCssGrammarNodes.
typedef uint16_t CssGrammarIndex;

// One node of a preorder tree, packed into 32 bits. Its fields, from the lowest bits up:
//
//   type      CssGrammarType
//   modifier  index into kCssGrammarModifiers
//   next      the distance from this node to the node after its subtree
//   value     what the type says it holds
//
// A node has children if `next` is greater than one. The first child follows the node, and each child's `next` leads to
// the one after it. `next` is relative, so a subtree can be copied or moved without fixups.
typedef uint32_t CssGrammarNode;

// The widths of a node's fields. The value takes the bits above the others.
constexpr uint32_t kCssGrammarTypeBits = 4;
constexpr uint32_t kCssGrammarModifierBits = 6;
constexpr uint32_t kCssGrammarNextBits = 8;

// The positions of a node's fields, and the node made of them.
constexpr uint32_t kCssGrammarShiftModifier = kCssGrammarTypeBits;
constexpr uint32_t kCssGrammarShiftNext = kCssGrammarShiftModifier + kCssGrammarModifierBits;
constexpr uint32_t kCssGrammarShiftValue = kCssGrammarShiftNext + kCssGrammarNextBits;
#define CSS_GRAMMAR_NODE(type, modifier, next, value)                    \
    (uint32_t(type) | (uint32_t(modifier) << kCssGrammarShiftModifier) \
     | (uint32_t(next) << kCssGrammarShiftNext)                        \
     | (uint32_t(value) << kCssGrammarShiftValue))

// Returns the fields of a node.
inline CssGrammarType cssGrammarType(CssGrammarNode node) {
    return CssGrammarType(node & ((1u << kCssGrammarTypeBits) - 1));
}
inline uint32_t cssGrammarNext(CssGrammarNode node) {
    return (node >> kCssGrammarShiftNext) & ((1u << kCssGrammarNextBits) - 1);
}
inline uint32_t cssGrammarValue(CssGrammarNode node) {
    return node >> kCssGrammarShiftValue;
}
inline uint32_t cssGrammarModifierIndex(CssGrammarNode node) {
    return (node >> kCssGrammarShiftModifier) & ((1u << kCssGrammarModifierBits) - 1);
}

// Returns the node's modifier.
inline const CssGrammarModifier* cssGrammarModifier(CssGrammarNode node) {
    return &kCssGrammarModifiers[cssGrammarModifierIndex(node)];
}

// The root of a definition with no grammar, such as a prose production, a legacy alias, or a type that a spec
// references but never defines. Node 0 is empty.
enum { CssGrammarRoot_None = 0 };

// Every node of every grammar. The roots are in css_property.h and css_value_def.h, beside the enums that index them.
extern const CssGrammarNode kCssGrammarNodes[];
extern const uint32_t kCssGrammarNodeCount;

// Returns the node after the subtree at `index`, which is where its next sibling starts.
inline CssGrammarIndex cssGrammarAfter(CssGrammarIndex index) {
    return CssGrammarIndex(index + cssGrammarNext(kCssGrammarNodes[index]));
}

// ---------------------------------------------------------------------------------------------------------------------
// First sets
//
// How a single match of a node can begin: the kinds of token, the literal characters, and whether it can match nothing.
// A node that cannot begin with the next token and cannot be empty is not worth trying there.

// Bits of CssGrammarFirst::kinds beside the token kinds.
constexpr uint32_t kCssGrammarFirstEmpty = 1u << 31;     // the node can match nothing
constexpr uint32_t kCssGrammarFirstAny = (1u << 31) - 2; // every kind of token but the end

// How one match of a node can begin.
struct CssGrammarFirst {
    uint32_t kinds;         // a bit 1 << CssTokenKind for each kind, and kCssGrammarFirstEmpty
    uint64_t literals;      // the literal characters, as bits from kCssGrammarLiteralBit
};

// The interned first sets, and the index of each node's own.
extern const CssGrammarFirst kCssGrammarFirsts[];
extern const uint16_t kCssGrammarFirstOf[];

// The bit for a literal character in CssGrammarFirst::literals, or zero for a character that no grammar uses.
extern const uint64_t kCssGrammarLiteralBit[128];

// Properties of a node's shape that the generator works out and that the matcher uses for shortcuts.
enum {
    // The next token decides whether a multiplied node is taken once more. Nothing that could begin the node can follow
    // it, or, for a # list past its first item, no comma can follow it.
    CssGrammarShape_Decided = 1 << 0,

    CssGrammarShape_Leaves = 1 << 1,     // a sequence of leaves, each taken once and none a comma

    // Taken once, with no frame to count it: the node has no multiplier, or is a ! sequence, whose own frame checks
    // that it produced a value.
    CssGrammarShape_Once = 1 << 2,

    CssGrammarShape_Optional = 1 << 3,   // may be taken no times, and is not a ! group

    // A || or && whose children mean something else in another order. A value keeps them as written.
    CssGrammarShape_Written = 1 << 4,

    // A value may omit what the node matches under its multiplier, as the node's entry in kCssGrammarOmissions says.
    CssGrammarShape_Omissible = 1 << 5,

    // A || whose children a value may omit, as their entries in kCssGrammarOmissions say.
    CssGrammarShape_OmitsChild = 1 << 6,

    // A <percentage> that a value stores as a <number>, one hundredth of the percentage.
    CssGrammarShape_Fraction = 1 << 7,
};
typedef uint8_t CssGrammarShape;

// The shape of each node.
extern const CssGrammarShape kCssGrammarShapes[];

// ---------------------------------------------------------------------------------------------------------------------
// Numeric literals

// The value of a numeric literal: a number if `unit` is PrCssUnit_Number, and a dimension otherwise.
struct CssGrammarNumber {
    double value;
    PrCssUnit unit;
};

// The numeric literals that the grammars write.
extern const CssGrammarNumber kCssGrammarNumbers[];

// ---------------------------------------------------------------------------------------------------------------------
// Ranges

// The bounds of a range that are infinite.
enum {
    CssGrammarRangeFlag_MinUnbounded = 1 << 0,
    CssGrammarRangeFlag_MaxUnbounded = 1 << 1,
};
typedef uint8_t CssGrammarRangeFlags;

// The bracketed bounds of a type: <integer [1,∞]>. Bounds are in the type's canonical unit.
struct CssGrammarRange {
    double min;
    double max;
    CssGrammarRangeFlags flags;
    uint8_t _pad[7];
};

static_assert(sizeof(CssGrammarRange) == 24);

// The range of a type with no bounds.
enum { CssGrammarRange_None = 0 };

// The interned ranges.
extern const CssGrammarRange kCssGrammarRanges[];
extern const uint32_t kCssGrammarRangeCount;

// ---------------------------------------------------------------------------------------------------------------------
// Omissions (cssom-1/#serialize-a-css-value)
//
// The value that the prose implies for a component when a value omits it. A value is stored with a component omitted
// when the component matches its implied value.

// What an implied value is.
enum {
    CssGrammarImpliedKind_Keyword,      // `keyword`
    CssGrammarImpliedKind_Number,       // an <integer> or a <number> of `number`
    CssGrammarImpliedKind_Percentage,   // a <percentage> of `number`
    CssGrammarImpliedKind_Zero,         // a zero, in any unit or none
};
typedef uint8_t CssGrammarImpliedKind;

// One value that a component implies.
struct CssGrammarImpliedValue {
    CssGrammarImpliedKind kind;
    PrCssKeyword keyword;
    double number;
};

// The `copies` value for a component that implies its own values.
constexpr uint8_t kCssGrammarImpliedOwn = 0xff;

// What a component, or a repetition of one, implies: the value of an earlier repetition, or values from
// kCssGrammarImpliedValues.
struct CssGrammarImplied {
    uint8_t copies;         // the repetition whose value is copied, or kCssGrammarImpliedOwn
    uint8_t first;          // index of the first of the values in kCssGrammarImpliedValues
    uint8_t count;          // 0 if the component is never left out
    bool alternatives;      // true if each value is implied alone rather than all as a run
};

// The maximum number of repetitions of a component whose repetitions after the first a value may omit.
constexpr uint32_t kCssGrammarOmissionMaxRepeats = 4;

// What a value may omit from a node's match: all of it, or its repetitions after the first.
struct CssGrammarOmission {
    CssGrammarIndex node;
    CssGrammarImplied whole;
    CssGrammarImplied repetitions[kCssGrammarOmissionMaxRepeats - 1];   // what each repetition after the first implies
};

// The omissions of every node that a value may omit, sorted by node.
extern const CssGrammarOmission kCssGrammarOmissions[];
extern const uint32_t kCssGrammarOmissionCount;

// The values that omissions imply.
extern const CssGrammarImpliedValue kCssGrammarImpliedValues[];

// Orders an omission against a node, for a search of kCssGrammarOmissions.
inline bool operator<(const CssGrammarOmission& omission, CssGrammarIndex node) {
    return omission.node < node;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
