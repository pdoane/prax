#include "css/src/css_math.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/string_ops.h"

#include "css/src/css_color.h"
#include "css/src/css_keyword.h"
#include "css/src/css_value_def.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Types (css-typed-om-1/#numeric-typing)

// The absence of a base type: the null percent hint, and the base of a category that no base type measures.
constexpr PrCssNumericBaseType kBaseTypeNone = PrCssNumericBaseType_Count;

// A type: an exponent for each base type, and a percent hint.
struct Type {
    int8_t exponent[PrCssNumericBaseType_Count];
    PrCssNumericBaseType hint;
    bool failure;
};

constexpr Type kNumberType = {{}, kBaseTypeNone, false};
constexpr Type kFailure = {{}, kBaseTypeNone, true};

PrCssNumericBaseType baseOf(CssUnitCategory category) {
    switch (category) {
        case CssUnitCategory_Length:     return PrCssNumericBaseType_Length;
        case CssUnitCategory_Angle:      return PrCssNumericBaseType_Angle;
        case CssUnitCategory_Time:       return PrCssNumericBaseType_Time;
        case CssUnitCategory_Frequency:  return PrCssNumericBaseType_Frequency;
        case CssUnitCategory_Resolution: return PrCssNumericBaseType_Resolution;
        case CssUnitCategory_Flex:       return PrCssNumericBaseType_Flex;
        case CssUnitCategory_Percent:    return PrCssNumericBaseType_Percent;
        default:                         return kBaseTypeNone;
    }
}

// Returns the category that a type measures. The type must resolve (resolves).
CssUnitCategory categoryOf(const Type& type) {
    constexpr CssUnitCategory kCategories[] = {
        CssUnitCategory_Length,    CssUnitCategory_Angle,      CssUnitCategory_Time,
        CssUnitCategory_Frequency, CssUnitCategory_Resolution, CssUnitCategory_Flex,
        CssUnitCategory_Percent,
    };
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i) {
        if (type.exponent[i] == 1)
            return kCategories[i];
    }
    return CssUnitCategory_Number;
}

Type typeOf(PrCssNumericBaseType base) {
    Type type = kNumberType;
    type.exponent[base] = 1;
    return type;
}

bool entriesEquals(const Type& a, const Type& b) {
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i) {
        if (a.exponent[i] != b.exponent[i])
            return false;
    }
    return true;
}

bool isNumberType(const Type& type) {
    return !type.failure && entriesEquals(type, kNumberType);
}

// css-typed-om-1/#apply-the-percent-hint
void applyHint(Type* type, PrCssNumericBaseType hint) {
    type->hint = hint;
    if (hint != PrCssNumericBaseType_Percent) {
        type->exponent[hint] = int8_t(type->exponent[hint] + type->exponent[PrCssNumericBaseType_Percent]);
        type->exponent[PrCssNumericBaseType_Percent] = 0;
    }
}

// Applies the hint of one type to the other, if the other has none. Returns false if both have hints and they differ.
bool shareHint(Type* a, Type* b) {
    if (a->hint != kBaseTypeNone && b->hint != kBaseTypeNone)
        return a->hint == b->hint;
    if (a->hint != kBaseTypeNone)
        applyHint(b, a->hint);
    else if (b->hint != kBaseTypeNone)
        applyHint(a, b->hint);
    return true;
}

// css-typed-om-1/#cssnumericvalue-add-two-types
Type addTypes(Type a, Type b) {
    if (a.failure || b.failure || !shareHint(&a, &b))
        return kFailure;
    if (entriesEquals(a, b))
        return a;

    bool percent = a.exponent[PrCssNumericBaseType_Percent] != 0 || b.exponent[PrCssNumericBaseType_Percent] != 0;
    bool other = false;
    for (uint32_t i = 0; i < PrCssNumericBaseType_Percent; ++i)
        other = other || a.exponent[i] != 0 || b.exponent[i] != 0;
    if (!percent || !other || a.hint != kBaseTypeNone)
        return kFailure;

    for (PrCssNumericBaseType hint = 0; hint < PrCssNumericBaseType_Percent; ++hint) {
        Type x = a;
        Type y = b;
        applyHint(&x, hint);
        applyHint(&y, hint);
        if (entriesEquals(x, y))
            return x;
    }
    return kFailure;
}

// css-typed-om-1/#cssnumericvalue-multiply-two-types
Type multiplyTypes(Type a, Type b) {
    if (a.failure || b.failure || !shareHint(&a, &b))
        return kFailure;
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i)
        a.exponent[i] = int8_t(a.exponent[i] + b.exponent[i]);
    return a;
}

// css-typed-om-1/#cssnumericvalue-invert-a-type
Type invertType(Type type) {
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i)
        type.exponent[i] = int8_t(-type.exponent[i]);
    return type;
}

// Returns true if a type matches one of the types that a math function resolves to: a <number>, or one base type with
// an exponent of 1 (css-values-4/#calc-type-checking).
bool resolves(const Type& type) {
    if (type.failure)
        return false;
    uint32_t ones = 0;
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i) {
        if (type.exponent[i] == 1)
            ++ones;
        else if (type.exponent[i] != 0)
            return false;
    }
    return ones <= 1;
}

// Returns true if a type matches what a slot accepts (css-typed-om-1/#cssnumericvalue-match).
bool matchesSlot(const Type& type, CssMathSlot slot) {
    if (type.failure)
        return false;
    if (slot.category == CssUnitCategory_Unknown)
        return resolves(type);
    PrCssNumericBaseType base = slot.category == CssUnitCategory_Number ? kBaseTypeNone : baseOf(slot.category);
    Type wanted = base == kBaseTypeNone ? kNumberType : typeOf(base);
    if (!entriesEquals(type, wanted))
        return false;
    if (type.hint == kBaseTypeNone)
        return true;
    return baseOf(slot.percentages) == type.hint;
}

// css-values-4/#css-make-a-type-consistent
Type madeConsistent(Type base, const Type& input) {
    if (input.failure)
        return kFailure;
    if (base.hint == kBaseTypeNone)
        base.hint = input.hint;
    return base;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building a tree (css-values-4/#parse-a-calculation)
//
// The matcher added a math function and its arguments in the order written: the numbers, dimensions and percentages,
// the operators and parentheses as literals, keywords, and the nested functions with their own arguments below them.
// The builder reads that by precedence into nodes, and types each node
// (css-values-4/#determine-the-type-of-a-calculation).

struct Builder {
    const CssValueNode* values;     // the values of the pool that the matcher added the function to
    CssMathScratch* scratch;
    CssMathSlot slot;
    uint32_t depth;
    uint32_t limit;                 // the depth that a tree built by a caller may reach

    // Each calculation of a math function must resolve to a type, as the value itself must, even if only the tree's
    // type is needed.
    bool strict;
};

// A run of sibling values, read from `at` up to `end`.
struct Cursor {
    uint32_t at;
    uint32_t end;
};

// A built node, and its type.
struct Built {
    uint32_t node;
    Type type;
};

constexpr Built kInvalid = {kCssMathNone, kFailure};

CssMathNode& nodeAt(Builder* b, uint32_t index) {
    return b->scratch->nodes.data[index];
}

uint32_t addNode(Builder* b, CssValueNode value) {
    CssMathNode node = {value, kCssMathNone, kCssMathNone, kCssMathNone};
    b->scratch->nodes.push(node);
    return b->scratch->nodes.count - 1;
}

// Adds a calc-operator node over `first` and the siblings after it.
uint32_t addOperator(Builder* b, PrCssCalc op, uint32_t first) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Calc;
    value.aux = op;
    uint32_t node = addNode(b, value);
    nodeAt(b, node).child = first;
    return node;
}

bool atLiteral(const Builder* b, const Cursor* c, char character) {
    return c->at < c->end && cssValueIsLiteral(&b->values[c->at], character);
}

void step(const Builder* b, Cursor* c) {
    c->at += b->values[c->at].next;
}

Built buildSum(Builder* b, Cursor* c);

// Sets `out` to the value of a <calc-keyword> (css-values-4/#calc-keywords), which is a <number>.
bool keywordNumber(PrCssKeyword keyword, double* out) {
    switch (keyword) {
        case PrCssKeyword_E:              *out = kEF64; return true;
        case PrCssKeyword_Pi:             *out = kPiF64; return true;
        case PrCssKeyword_Infinity:       *out = kInfinityF64; return true;
        case PrCssKeyword_MinusInfinity:  *out = -kInfinityF64; return true;
        case PrCssKeyword_Nan:            *out = kNanF64; return true;
        default:                        return false;
    }
}

// Returns true if the keyword is a relative color's component keyword: a <number> in a calculation whose value the
// origin color determines (css-color-5/#component-keyword). The matcher accepts one only in a relative color.
bool componentKeyword(PrCssKeyword keyword) {
    if (keyword == PrCssKeyword_Alpha)
        return true;
    for (const PrCssKeyword (&components)[3] : kCssColorComponents) {
        for (PrCssKeyword component : components) {
            if (component == keyword)
                return true;
        }
    }
    return false;
}

// Returns the type of a percentage: the slot's type if the slot resolves percentages against another type, and «[
// "percent" → 1 ]» otherwise.
Type percentType(CssMathSlot slot) {
    if (slot.category == CssUnitCategory_Unknown)
        return typeOf(PrCssNumericBaseType_Percent);
    PrCssNumericBaseType basis = baseOf(slot.percentages);
    Type type = typeOf(basis < PrCssNumericBaseType_Percent ? basis : PrCssNumericBaseType_Percent);
    type.hint = basis < PrCssNumericBaseType_Percent ? basis : PrCssNumericBaseType_Percent;
    return type;
}

// Adds a node for the value at `index` and the values below it, which the tree keeps unchanged.
uint32_t addLeaf(Builder* b, uint32_t index) {
    const CssValueNode* value = &b->values[index];
    uint32_t node = addNode(b, *value);
    nodeAt(b, node).leaf = b->scratch->leaves.count;
    b->scratch->leaves.append(value, value->next);
    return node;
}

// Builds a leaf for a function that is not a math function. An anchor function resolves to a <length>
// (css-anchor-position-1/#anchor-pos), and a tree-counting function to an <integer> (css-values-5/#tree-counting).
Built buildLeafFunction(Builder* b, uint32_t index) {
    const CssValueNode* fn = &b->values[index];
    Type type = kFailure;
    switch (cssValueKeyword(fn)) {
        case PrCssKeyword_Anchor:
        case PrCssKeyword_AnchorSize:
            type = typeOf(PrCssNumericBaseType_Length);
            break;
        case PrCssKeyword_SiblingIndex:
        case PrCssKeyword_SiblingCount:
            type = kNumberType;
            break;
        default:
            return kInvalid;
    }
    return Built{addLeaf(b, index), type};
}

Built buildFunction(Builder* b, uint32_t index);

// <calc-value>
Built buildValue(Builder* b, Cursor* c) {
    if (c->at >= c->end)
        return kInvalid;
    uint32_t index = c->at;
    CssValueNode value = b->values[index];
    step(b, c);
    switch (value.kind) {
        case PrCssValueKind_Number:
        case PrCssValueKind_Integer:
            value.kind = PrCssValueKind_Number;
            return Built{addNode(b, value), kNumberType};
        case PrCssValueKind_Percentage:
            return Built{addNode(b, value), percentType(b->slot)};
        case PrCssValueKind_Dimension: {
            PrCssNumericBaseType base = baseOf(cssUnitCategory(value.unit));
            if (base == kBaseTypeNone)
                return kInvalid;
            value.next = 1;
            return Built{addNode(b, value), typeOf(base)};
        }
        case PrCssValueKind_Keyword: {
            CssValueNode number = {};
            number.kind = PrCssValueKind_Number;
            if (keywordNumber(cssValueKeyword(&value), &number.number))
                return Built{addNode(b, number), kNumberType};
            if (componentKeyword(cssValueKeyword(&value)))
                return Built{addLeaf(b, index), kNumberType};
            return kInvalid;
        }
        case PrCssValueKind_Literal: {
            if (cssValueLiteral(&value) != '(' || ++b->depth > kCssMathMaxDepth)
                return kInvalid;
            Built sum = buildSum(b, c);
            --b->depth;
            if (!atLiteral(b, c, ')'))
                return kInvalid;
            step(b, c);
            return sum;
        }
        case PrCssValueKind_Function:
            return buildFunction(b, index);
        default:
            return kInvalid;
    }
}

// <calc-product>: values joined by '*' and '/'. The right side of a '/' goes under an Invert node, and all go under a
// Product node if there is more than one.
Built buildProduct(Builder* b, Cursor* c) {
    Built first = buildValue(b, c);
    if (first.node == kCssMathNone)
        return kInvalid;
    Type type = first.type;
    uint32_t last = first.node;
    bool several = false;
    while (atLiteral(b, c, '*') || atLiteral(b, c, '/')) {
        bool divide = atLiteral(b, c, '/');
        step(b, c);
        Built operand = buildValue(b, c);
        if (operand.node == kCssMathNone)
            return kInvalid;
        uint32_t node = operand.node;
        if (divide) {
            node = addOperator(b, PrCssCalc_Invert, node);
            operand.type = invertType(operand.type);
        }
        type = multiplyTypes(type, operand.type);
        nodeAt(b, last).sibling = node;
        last = node;
        several = true;
    }
    if (!several)
        return first;
    return Built{addOperator(b, PrCssCalc_Product, first.node), type};
}

// <calc-sum>: products joined by '+' and '-'. The right side of a '-' goes under a Negate node, and all go under a Sum
// node if there is more than one.
Built buildSum(Builder* b, Cursor* c) {
    Built first = buildProduct(b, c);
    if (first.node == kCssMathNone)
        return kInvalid;
    Type type = first.type;
    uint32_t last = first.node;
    bool several = false;
    while (atLiteral(b, c, '+') || atLiteral(b, c, '-')) {
        bool subtract = atLiteral(b, c, '-');
        step(b, c);
        Built operand = buildProduct(b, c);
        if (operand.node == kCssMathNone)
            return kInvalid;
        uint32_t node = subtract ? addOperator(b, PrCssCalc_Negate, operand.node) : operand.node;
        type = addTypes(type, operand.type);
        nodeAt(b, last).sibling = node;
        last = node;
        several = true;
    }
    if (!several)
        return first;
    return Built{addOperator(b, PrCssCalc_Sum, first.node), type};
}

// The arguments of a math function, as its node's children in the order written. The comma between two arguments is a
// literal. A keyword that the grammar accepts (`none`, a <rounding-strategy>, the parts of a <random-key>) is its own
// node. Each calculation is the root of its tree.
struct Arguments {
    uint32_t first;         // the first child, or kCssMathNone
    uint32_t last;
    uint32_t calculations;    // number of arguments that are calculations
    Type sum;                 // the sum of the calculations' types (css-values-4/#css-consistent-type)
    Type single;              // the type of the first calculation
    bool numbers;             // true if every calculation is a <number>
    PrCssKeyword keyword;     // the first keyword argument, or PrCssKeyword_Unknown
};

void addArgument(Builder* b, Arguments* args, uint32_t node) {
    if (args->first == kCssMathNone)
        args->first = node;
    else
        nodeAt(b, args->last).sibling = node;
    args->last = node;
}

bool buildArguments(Builder* b, uint32_t index, Arguments* args) {
    *args = Arguments{kCssMathNone, kCssMathNone, 0, kFailure, kFailure, true, PrCssKeyword_Unknown};
    Cursor c = {index + 1, index + b->values[index].next};
    while (c.at < c.end) {
        const CssValueNode* value = &b->values[c.at];
        double ignored;
        bool keyword = value->kind == PrCssValueKind_Keyword && !keywordNumber(cssValueKeyword(value), &ignored) &&
            !componentKeyword(cssValueKeyword(value));
        if (keyword || value->kind == PrCssValueKind_Ident || atLiteral(b, &c, ',')) {
            if (keyword && args->keyword == PrCssKeyword_Unknown)
                args->keyword = cssValueKeyword(value);
            addArgument(b, args, addLeaf(b, c.at));
            step(b, &c);
            // css-values-5/#random: the <number> after `fixed` is the random base value.
            if (keyword && cssValueKeyword(value) == PrCssKeyword_Fixed) {
                addArgument(b, args, addLeaf(b, c.at));
                step(b, &c);
            }
            continue;
        }
        Built calc = buildSum(b, &c);
        if (calc.node == kCssMathNone || !resolves(calc.type))
            return false;
        addArgument(b, args, calc.node);
        args->sum = args->calculations == 0 ? calc.type : addTypes(args->sum, calc.type);
        if (args->calculations == 0)
            args->single = calc.type;
        args->numbers = args->numbers && isNumberType(calc.type);
        ++args->calculations;
    }
    return true;
}

// Returns the type of a math function (css-values-4/#calc-type-checking), or failure if its arguments are not what its
// definition accepts.
Type functionType(CssValueDef def, const Arguments& args) {
    Type angle = typeOf(PrCssNumericBaseType_Angle);
    switch (def) {
        case CssValueDef_AbsFn:
            return args.single;
        case CssValueDef_MinFn:
        case CssValueDef_MaxFn:
        case CssValueDef_ClampFn:
        case CssValueDef_HypotFn:
        case CssValueDef_ModFn:
        case CssValueDef_RemFn:
            return args.sum;
        case CssValueDef_RoundFn:
            // css-values-4/#round-func: B may be omitted if A is a <number>, and also with line-width, in which case A
            // must be a <length>.
            if (args.keyword == PrCssKeyword_LineWidth &&
                    !entriesEquals(args.single, typeOf(PrCssNumericBaseType_Length)))
                return kFailure;
            if (args.calculations == 1 && args.keyword != PrCssKeyword_LineWidth && !args.numbers)
                return kFailure;
            return args.sum;
        case CssValueDef_SinFn:
        case CssValueDef_CosFn:
        case CssValueDef_TanFn:
            if (!args.numbers && !entriesEquals(args.single, angle))
                return kFailure;
            return madeConsistent(kNumberType, args.single);
        case CssValueDef_AsinFn:
        case CssValueDef_AcosFn:
        case CssValueDef_AtanFn:
            return args.numbers ? madeConsistent(angle, args.single) : kFailure;
        case CssValueDef_Atan2Fn:
            return madeConsistent(angle, args.sum);
        case CssValueDef_PowFn:
        case CssValueDef_SqrtFn:
        case CssValueDef_LogFn:
        case CssValueDef_ExpFn:
            return args.numbers ? madeConsistent(kNumberType, args.sum) : kFailure;
        case CssValueDef_SignFn:
            return madeConsistent(kNumberType, args.single);
        case CssValueDef_ProgressFn:
            return madeConsistent(kNumberType, args.sum);
        case CssValueDef_RandomFn:
            return args.sum;
        default:
            return kFailure;
    }
}

// Builds a math function: its node over its arguments. calc() is the tree of its calculation
// (css-values-4/#calc-internal).
Built buildFunction(Builder* b, uint32_t index) {
    CssValueNode fn = b->values[index];
    CssValueDef def = cssMathFunction(cssValueKeyword(&fn));
    if (def == CssValueDef_Count)
        return buildLeafFunction(b, index);
    Arguments args;
    if (!buildArguments(b, index, &args))
        return kInvalid;
    if (def == CssValueDef_CalcFn)
        return Built{args.first, args.single};

    Type type = functionType(def, args);
    if (!resolves(type))
        return kInvalid;
    fn.next = 1;
    uint32_t node = addNode(b, fn);
    nodeAt(b, node).child = args.first;
    return Built{node, type};
}

// ---------------------------------------------------------------------------------------------------------------------
// Adopting a tree
//
// A calculation that a caller built is already a tree: a numeric value, a calc-operator node over its operands, or a
// math function over its arguments. The arguments are calculations, the keywords its grammar accepts, and the commas
// between them. The builder reads that into nodes, and types each node as it does for a tree built from written values.

Built adoptNode(Builder* b, uint32_t index);

bool adoptArguments(Builder* b, uint32_t index, Arguments* args) {
    *args = Arguments{kCssMathNone, kCssMathNone, 0, kFailure, kFailure, true, PrCssKeyword_Unknown};
    uint32_t end = index + b->values[index].next;
    for (uint32_t at = index + 1; at < end; at += b->values[at].next) {
        const CssValueNode* value = &b->values[at];
        double ignored;
        bool keyword = value->kind == PrCssValueKind_Keyword && !keywordNumber(cssValueKeyword(value), &ignored) &&
            !componentKeyword(cssValueKeyword(value));
        if (keyword || value->kind == PrCssValueKind_Ident || cssValueIsLiteral(value, ',')) {
            if (keyword && args->keyword == PrCssKeyword_Unknown)
                args->keyword = cssValueKeyword(value);
            addArgument(b, args, addLeaf(b, at));
            // css-values-5/#random: the <number> after `fixed` is the random base value.
            if (keyword && cssValueKeyword(value) == PrCssKeyword_Fixed && at + value->next < end) {
                at += value->next;
                addArgument(b, args, addLeaf(b, at));
            }
            continue;
        }
        Built calc = adoptNode(b, at);
        if (calc.node == kCssMathNone || calc.type.failure || (b->strict && !resolves(calc.type)))
            return false;
        addArgument(b, args, calc.node);
        args->sum = args->calculations == 0 ? calc.type : addTypes(args->sum, calc.type);
        if (args->calculations == 0)
            args->single = calc.type;
        args->numbers = args->numbers && isNumberType(calc.type);
        ++args->calculations;
    }
    return true;
}

// Adopts a calc-operator node over its children, which are its operands. A Negate or an Invert has one operand, and the
// other operators have one or more.
Built adoptOperator(Builder* b, uint32_t index) {
    PrCssCalc op = cssValueCalcOperator(&b->values[index]);
    uint32_t end = index + b->values[index].next;
    uint32_t first = kCssMathNone;
    uint32_t last = kCssMathNone;
    uint32_t count = 0;
    Type type = kFailure;
    for (uint32_t at = index + 1; at < end; at += b->values[at].next) {
        Built operand = adoptNode(b, at);
        if (operand.node == kCssMathNone)
            return kInvalid;
        switch (op) {
            case PrCssCalc_Sum:
                type = count == 0 ? operand.type : addTypes(type, operand.type);
                break;
            case PrCssCalc_Product:
                type = count == 0 ? operand.type : multiplyTypes(type, operand.type);
                break;
            case PrCssCalc_Negate:  type = operand.type; break;
            default:                      type = invertType(operand.type); break;
        }
        if (first == kCssMathNone)
            first = operand.node;
        else
            nodeAt(b, last).sibling = operand.node;
        last = operand.node;
        ++count;
    }
    bool single = op == PrCssCalc_Negate || op == PrCssCalc_Invert;
    if (count == 0 || (single && count != 1) || type.failure)
        return kInvalid;
    return Built{addOperator(b, op, first), type};
}

Built adoptNode(Builder* b, uint32_t index) {
    if (b->depth == b->limit)
        return kInvalid;
    ++b->depth;
    CssValueNode value = b->values[index];
    Built built = kInvalid;
    switch (value.kind) {
        case PrCssValueKind_Number:
        case PrCssValueKind_Integer:
            value.kind = PrCssValueKind_Number;
            built = Built{addNode(b, value), kNumberType};
            break;
        case PrCssValueKind_Percentage:
            built = Built{addNode(b, value), percentType(b->slot)};
            break;
        case PrCssValueKind_Dimension: {
            PrCssNumericBaseType base = baseOf(cssUnitCategory(value.unit));
            if (base != kBaseTypeNone && value.next == 1)
                built = Built{addNode(b, value), typeOf(base)};
            break;
        }
        case PrCssValueKind_Keyword: {
            CssValueNode number = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 0);
            if (keywordNumber(cssValueKeyword(&value), &number.number))
                built = Built{addNode(b, number), kNumberType};
            else if (componentKeyword(cssValueKeyword(&value)))
                built = Built{addLeaf(b, index), kNumberType};
            break;
        }
        case PrCssValueKind_Calc:
            built = adoptOperator(b, index);
            break;
        case PrCssValueKind_Function: {
            CssValueDef def = cssMathFunction(cssValueKeyword(&value));
            if (def == CssValueDef_Count) {
                built = buildLeafFunction(b, index);
                break;
            }
            Arguments args;
            if (def == CssValueDef_CalcFn || !adoptArguments(b, index, &args))
                break;
            Type type = functionType(def, args);
            if (type.failure || (b->strict && !resolves(type)))
                break;
            value.next = 1;
            uint32_t node = addNode(b, value);
            nodeAt(b, node).child = args.first;
            built = Built{node, type};
            break;
        }
        default:
            break;
    }
    --b->depth;
    return built;
}

// ---------------------------------------------------------------------------------------------------------------------
// Simplification (css-values-4/#calc-simplification)
//
// The tree is simplified as a specified value: a dimension in a unit that has a fixed ratio to its canonical unit is
// converted to that unit, and a percentage is not resolved.

bool isNumeric(const CssMathNode& node) {
    return node.leaf == kCssMathNone && cssValueIsNumeric(node.value.kind);
}

bool isNumber(const CssMathNode& node) {
    return node.leaf == kCssMathNone && node.value.kind == PrCssValueKind_Number;
}

bool isOperator(const CssMathNode& node, PrCssCalc op) {
    return node.value.kind == PrCssValueKind_Calc && cssValueCalcOperator(&node.value) == op;
}

// Returns true if two numeric values are in the same unit. A number and a percentage each count as a unit of their own.
bool unitEquals(const CssValueNode& a, const CssValueNode& b) {
    return a.kind == b.kind && (a.kind != PrCssValueKind_Dimension || a.unit == b.unit);
}

// Converts a dimension to the canonical unit of its type, if the ratio between them is fixed.
void canonicalize(CssValueNode* value) {
    if (value->kind != PrCssValueKind_Dimension)
        return;
    double scale = cssUnitScale(value->unit);
    if (scale == 0)
        return;
    value->number *= scale;
    value->unit = cssUnitCanonical(cssUnitCategory(value->unit));
}

CssValueNode degrees(double radians) {
    return cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, radians * (180 / kPiF64));
}

// Unlinks the sibling after `node`.
void unlinkNext(Builder* b, uint32_t node) {
    nodeAt(b, node).sibling = nodeAt(b, nodeAt(b, node).sibling).sibling;
}

// Replaces each child of `parent` that is an operator node `op` with that node's children.
void flatten(Builder* b, uint32_t parent, PrCssCalc op) {
    uint32_t* link = &nodeAt(b, parent).child;
    while (*link != kCssMathNone) {
        uint32_t child = *link;
        if (!isOperator(nodeAt(b, child), op)) {
            link = &nodeAt(b, child).sibling;
            continue;
        }
        uint32_t first = nodeAt(b, child).child;
        uint32_t last = first;
        while (nodeAt(b, last).sibling != kCssMathNone)
            last = nodeAt(b, last).sibling;
        nodeAt(b, last).sibling = nodeAt(b, child).sibling;
        *link = first;
    }
}

// Clears the node's sibling link and returns the node.
uint32_t alone(Builder* b, uint32_t node) {
    nodeAt(b, node).sibling = kCssMathNone;
    return node;
}

// Returns true if the calculation's percentages resolve against another type. Such a percentage is not compared with
// another, or computed by a math function, before it is resolved (css-values-4/#simplify-a-calculation-tree).
bool comparesPercentages(const Builder* b) {
    return baseOf(b->slot.percentages) < PrCssNumericBaseType_Percent;
}

// Combines each numeric child of `parent` with later children in the same unit, using `combine`. Percentages are
// combined only if `percentages` is set.
template <typename Combine>
void combineUnits(Builder* b, uint32_t parent, bool percentages, Combine combine) {
    for (uint32_t child = nodeAt(b, parent).child; child != kCssMathNone; child = nodeAt(b, child).sibling) {
        if (!isNumeric(nodeAt(b, child)) || (!percentages && nodeAt(b, child).value.kind == PrCssValueKind_Percentage)) {
            continue;
        }
        uint32_t before = child;
        while (nodeAt(b, before).sibling != kCssMathNone) {
            CssMathNode& other = nodeAt(b, nodeAt(b, before).sibling);
            if (isNumeric(other) && unitEquals(other.value, nodeAt(b, child).value)) {
                nodeAt(b, child).value.number = combine(nodeAt(b, child).value.number, other.value.number);
                unlinkNext(b, before);
            } else {
                before = nodeAt(b, before).sibling;
            }
        }
    }
}

// Negates a numeric value for a Negate node: 0 - value.
void negate(CssValueNode* value) {
    value->number = 0.0 - value->number;
}

uint32_t simplifyNegate(Builder* b, uint32_t node) {
    uint32_t child = nodeAt(b, node).child;
    if (isNumeric(nodeAt(b, child))) {
        negate(&nodeAt(b, child).value);
        return child;
    }
    if (isOperator(nodeAt(b, child), PrCssCalc_Negate))
        return nodeAt(b, child).child;
    if (!isOperator(nodeAt(b, child), PrCssCalc_Sum))
        return node;

    uint32_t previous = kCssMathNone;
    for (uint32_t grandchild = nodeAt(b, child).child; grandchild != kCssMathNone;) {
        uint32_t next = nodeAt(b, grandchild).sibling;
        uint32_t negated = grandchild;
        if (isNumeric(nodeAt(b, grandchild)))
            negate(&nodeAt(b, grandchild).value);
        else if (isOperator(nodeAt(b, grandchild), PrCssCalc_Negate))
            negated = nodeAt(b, grandchild).child;
        else
            negated = addOperator(b, PrCssCalc_Negate, alone(b, grandchild));
        nodeAt(b, negated).sibling = next;
        if (previous == kCssMathNone)
            nodeAt(b, child).child = negated;
        else
            nodeAt(b, previous).sibling = negated;
        previous = negated;
        grandchild = next;
    }
    return child;
}

uint32_t simplifyInvert(Builder* b, uint32_t node) {
    uint32_t child = nodeAt(b, node).child;
    if (isNumber(nodeAt(b, child))) {
        nodeAt(b, child).value.number = 1 / nodeAt(b, child).value.number;
        return child;
    }
    if (isOperator(nodeAt(b, child), PrCssCalc_Invert))
        return nodeAt(b, child).child;
    return node;
}

// Returns true if a child of a Product node is a factor that folding accepts: a numeric value, or an Invert node over
// one. If `numbers` is set, only a number is accepted.
bool isFactor(Builder* b, uint32_t child, bool numbers) {
    if (numbers)
        return isNumber(nodeAt(b, child));
    if (isOperator(nodeAt(b, child), PrCssCalc_Invert))
        child = nodeAt(b, child).child;
    return isNumeric(nodeAt(b, child));
}

// Computes the product of the factors of a Product node, if its type matches a numeric type: the dimensions of each
// base type are in one unit, and at most one base type is left with an exponent, which is 1. Returns false otherwise,
// or if there are no factors.
bool foldFactors(Builder* b, uint32_t node, bool numbers, CssValueNode* out) {
    int32_t exponent[PrCssNumericBaseType_Count] = {};
    PrCssUnit unit[PrCssNumericBaseType_Count] = {};
    double product = 1;
    bool any = false;
    for (uint32_t child = nodeAt(b, node).child; child != kCssMathNone; child = nodeAt(b, child).sibling) {
        if (!isFactor(b, child, numbers))
            continue;
        any = true;
        bool inverted = isOperator(nodeAt(b, child), PrCssCalc_Invert);
        const CssValueNode& factor = nodeAt(b, inverted ? nodeAt(b, child).child : child).value;
        product = inverted ? product / factor.number : product * factor.number;
        if (factor.kind == PrCssValueKind_Number)
            continue;
        PrCssNumericBaseType base = PrCssNumericBaseType_Percent;
        if (factor.kind != PrCssValueKind_Percentage)
            base = baseOf(cssUnitCategory(factor.unit));
        if (exponent[base] != 0 && unit[base] != factor.unit)
            return false;
        unit[base] = factor.unit;
        exponent[base] += inverted ? -1 : 1;
    }

    PrCssNumericBaseType left = kBaseTypeNone;
    for (PrCssNumericBaseType base = 0; base < PrCssNumericBaseType_Count; ++base) {
        if (exponent[base] == 0)
            continue;
        if (left != kBaseTypeNone || exponent[base] != 1)
            return false;
        left = base;
    }
    *out = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, product);
    if (left == PrCssNumericBaseType_Percent) {
        out->kind = PrCssValueKind_Percentage;
    } else if (left != kBaseTypeNone) {
        out->kind = PrCssValueKind_Dimension;
        out->unit = unit[left];
    }
    return any;
}

// Replaces the factors of a Product node with one numeric value, `value`, in the node of the first factor. Returns that
// node.
uint32_t replaceFactors(Builder* b, uint32_t node, bool numbers, CssValueNode value) {
    uint32_t first = nodeAt(b, node).child;
    while (!isFactor(b, first, numbers))
        first = nodeAt(b, first).sibling;
    for (uint32_t child = first; nodeAt(b, child).sibling != kCssMathNone;) {
        if (isFactor(b, nodeAt(b, child).sibling, numbers))
            unlinkNext(b, child);
        else
            child = nodeAt(b, child).sibling;
    }
    nodeAt(b, first).value = value;
    nodeAt(b, first).child = kCssMathNone;
    return first;
}

// Simplifies a Product node. A product of numbers, or of numeric values, folds into one value
// (css-values-4/#calc-simplification). So do the numeric factors of a product that also has other nodes.
uint32_t simplifyProduct(Builder* b, uint32_t node) {
    flatten(b, node, PrCssCalc_Product);

    CssValueNode folded;
    uint32_t factor = kCssMathNone;
    if (foldFactors(b, node, false, &folded))
        factor = replaceFactors(b, node, false, folded);
    else if (foldFactors(b, node, true, &folded))
        factor = replaceFactors(b, node, true, folded);

    // A number and a Sum of numeric values: the number is distributed over the Sum.
    uint32_t first = nodeAt(b, node).child;
    uint32_t second = nodeAt(b, first).sibling;
    if (second == kCssMathNone)
        return first;
    if (factor == kCssMathNone || !isNumber(nodeAt(b, factor)) || nodeAt(b, second).sibling != kCssMathNone)
        return node;
    uint32_t sum = first == factor ? second : first;
    bool numeric = isOperator(nodeAt(b, sum), PrCssCalc_Sum);
    for (uint32_t term = nodeAt(b, sum).child; numeric && term != kCssMathNone; term = nodeAt(b, term).sibling)
        numeric = isNumeric(nodeAt(b, term));
    if (!numeric)
        return node;
    for (uint32_t term = nodeAt(b, sum).child; term != kCssMathNone; term = nodeAt(b, term).sibling)
        nodeAt(b, term).value.number *= nodeAt(b, factor).value.number;
    return sum;
}

// Rounds `a` to a multiple of `step` by `strategy`, for round() (css-values-4/#round-func). The nearest strategy rounds
// a tie up, and infinities are handled as css-values-4/#round-infinities specifies.
double roundBy(PrCssKeyword strategy, double a, double step) {
    switch (strategy) {
        case PrCssKeyword_Up:     return roundToMultipleF64<RoundMode_Up>(a, step);
        case PrCssKeyword_Down:   return roundToMultipleF64<RoundMode_Down>(a, step);
        case PrCssKeyword_ToZero: return roundToMultipleF64<RoundMode_TowardZero>(a, step);
        default:                return roundToMultipleF64<RoundMode_Nearest>(a, step);
    }
}

// Returns the random base value that a `fixed` key gives (css-values-5/#random), from the values of its <number>: a
// number, or a math function whose tree is a number. Returns NaN if it is neither.
double baseValue(const CssValueNode* values) {
    const CssValueNode* number = values->kind == PrCssValueKind_Math ? values + 1 : values;
    return number->kind == PrCssValueKind_Number ? number->number : kNanF64;
}

// Returns the value between `min` and `max` at `base` (css-values-5/#random). With a step, it is the multiple of the
// step above `min` that `base` picks among the multiples up to `max`.
double randomValue(double base, double min, double max, double step) {
    if (step <= 0)
        return min + base * (max - min);
    double steps = floorF64((max - min) / step);
    double picked = floorF64(base * (steps + 1));
    return min + step * (picked < steps ? picked : steps);
}

// Computes the value of a math function whose calculations are numeric values in one unit, in the canonical unit of its
// type. Returns false otherwise, and for round(line-width, ...), which depends on the target pixel.
bool computeFunction(Builder* b, uint32_t node, CssValueNode* out) {
    CssValueDef def = cssMathFunction(cssValueKeyword(&nodeAt(b, node).value));
    PrCssKeyword strategy = PrCssKeyword_Nearest;
    CssValueNode first = {};
    bool numeric = false;       // true once `first` holds the first calculation
    double v[3] = {};
    bool present[3] = {};
    uint32_t count = 0;         // number of arguments so far, `none` included
    double acc = 0;
    bool fixed = false;         // random(): true if the key is `fixed`; `base` is then its value
    double base = kNanF64;
    for (uint32_t child = nodeAt(b, node).child; child != kCssMathNone; child = nodeAt(b, child).sibling) {
        const CssMathNode& argument = nodeAt(b, child);
        const CssValueNode& value = argument.value;
        if (argument.leaf != kCssMathNone) {
            if (cssValueIsKeyword(&value, PrCssKeyword_None))
                ++count;
            else if (cssValueIsKeyword(&value, PrCssKeyword_Fixed))
                fixed = true;
            else if (value.kind == PrCssValueKind_Keyword)
                strategy = cssValueKeyword(&value);
            else if (fixed && isNanF64(base))
                base = baseValue(b->scratch->leaves.data + argument.leaf);
            continue;
        }
        if (!isNumeric(argument) || (numeric && !unitEquals(first, value)) ||
                (value.kind == PrCssValueKind_Percentage && comparesPercentages(b))) {
            return false;
        }
        if (!numeric)
            first = value;
        numeric = true;
        double x = value.number;
        switch (def) {
            case CssValueDef_MinFn:     acc = count == 0 ? x : minimumF64(acc, x); break;
            case CssValueDef_MaxFn:     acc = count == 0 ? x : maximumF64(acc, x); break;
            case CssValueDef_HypotFn:
                // Any NaN gives NaN (css-values-4/#calc-ieee), although hypot would let an infinity win.
                acc = count == 0 ? absF64(x) : isNanF64(acc) || isNanF64(x) ? kNanF64 : hypotF64(acc, x);
                break;
            default:
                if (count < 3) {
                    v[count] = x;
                    present[count] = true;
                }
                break;
        }
        ++count;
    }
    if (strategy == PrCssKeyword_LineWidth)
        return false;
    // A number from a dimension is the ratio of the dimension to its unit. That ratio is unknown for a unit that
    // depends on context, and for a percentage.
    if ((def == CssValueDef_SignFn || def == CssValueDef_Atan2Fn || def == CssValueDef_ProgressFn) &&
            (first.kind == PrCssValueKind_Percentage ||
                (first.kind == PrCssValueKind_Dimension && cssUnitScale(first.unit) == 0))) {
        return false;
    }

    const double toRadians = first.kind == PrCssValueKind_Dimension ? kPiF64 / 180 : 1;
    CssValueNode same = first;
    switch (def) {
        case CssValueDef_MinFn:
        case CssValueDef_MaxFn:
        case CssValueDef_HypotFn:
            same.number = acc;
            break;
        case CssValueDef_ClampFn: {
            double x = v[1];
            if (present[2])
                x = minimumF64(x, v[2]);
            if (present[0])
                x = maximumF64(v[0], x);
            same.number = x;
            break;
        }
        case CssValueDef_RoundFn:
            same.number = roundBy(strategy, v[0], present[1] ? v[1] : 1);
            break;
        case CssValueDef_ModFn:     same.number = floorModF64(v[0], v[1]); break;
        case CssValueDef_RemFn:     same.number = fmodF64(v[0], v[1]); break;
        case CssValueDef_AbsFn:     same.number = absF64(v[0]); break;
        case CssValueDef_SinFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, sinF64(v[0] * toRadians));
            break;
        case CssValueDef_CosFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, cosF64(v[0] * toRadians));
            break;
        case CssValueDef_TanFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, tanF64(v[0] * toRadians));
            break;
        case CssValueDef_AsinFn:    same = degrees(asinF64(v[0])); break;
        case CssValueDef_AcosFn:    same = degrees(acosF64(v[0])); break;
        case CssValueDef_AtanFn:    same = degrees(atanF64(v[0])); break;
        case CssValueDef_Atan2Fn:   same = degrees(atan2F64(v[0], v[1])); break;
        case CssValueDef_PowFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, powF64(v[0], v[1]));
            break;
        case CssValueDef_SqrtFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, sqrtF64(v[0]));
            break;
        case CssValueDef_ExpFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, expF64(v[0]));
            break;
        case CssValueDef_LogFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number,
                present[1] ? logF64(v[0]) / logF64(v[1]) : logF64(v[0]));
            break;
        case CssValueDef_SignFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, v[0] > 0 ? 1 : v[0] < 0 ? -1 : v[0]);
            break;
        case CssValueDef_ProgressFn:
            same = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, (v[0] - v[1]) / (v[2] - v[1]));
            break;
        case CssValueDef_RandomFn:
            if (isNanF64(base))
                return false;
            same.number = randomValue(base, v[0], v[1], present[2] ? v[2] : 0);
            break;
        default:
            return false;
    }
    *out = same;
    return true;
}

uint32_t simplifySum(Builder* b, uint32_t node) {
    flatten(b, node, PrCssCalc_Sum);
    combineUnits(b, node, true, [](double x, double y) { return x + y; });
    uint32_t child = nodeAt(b, node).child;
    return nodeAt(b, child).sibling == kCssMathNone ? child : node;
}

// Removes `none` bounds from clamp() (css-values-4/#comp-func). clamp(MIN, VAL, none) is max(MIN, VAL), clamp(none,
// VAL, MAX) is min(VAL, MAX), and clamp(none, VAL, none) is VAL.
uint32_t unclamp(Builder* b, uint32_t node) {
    uint32_t low = nodeAt(b, node).child;
    uint32_t value = nodeAt(b, nodeAt(b, low).sibling).sibling;
    uint32_t high = nodeAt(b, nodeAt(b, value).sibling).sibling;
    bool noLow = nodeAt(b, low).value.kind == PrCssValueKind_Keyword;
    bool noHigh = nodeAt(b, high).value.kind == PrCssValueKind_Keyword;
    if (noLow && noHigh)
        return value;
    if (noLow) {
        nodeAt(b, node).value.aux = PrCssKeyword_Min;
        nodeAt(b, node).child = value;
    } else if (noHigh) {
        nodeAt(b, node).value.aux = PrCssKeyword_Max;
        nodeAt(b, value).sibling = kCssMathNone;
    }
    return node;
}

uint32_t simplifyFunction(Builder* b, uint32_t node) {
    CssValueNode value;
    if (computeFunction(b, node, &value)) {
        nodeAt(b, node).value = value;
        nodeAt(b, node).child = kCssMathNone;
        return node;
    }
    if (cssValueKeyword(&nodeAt(b, node).value) == PrCssKeyword_Clamp) {
        uint32_t unclamped = unclamp(b, node);
        if (unclamped != node || cssValueKeyword(&nodeAt(b, node).value) == PrCssKeyword_Clamp)
            return unclamped;
    }
    PrCssKeyword name = cssValueKeyword(&nodeAt(b, node).value);
    if (name != PrCssKeyword_Min && name != PrCssKeyword_Max)
        return node;
    bool larger = name == PrCssKeyword_Max;
    combineUnits(b, node, !comparesPercentages(b),
        [larger](double x, double y) { return larger ? maximumF64(x, y) : minimumF64(x, y); });

    // The commas of the arguments that were combined away are removed with them.
    for (uint32_t child = nodeAt(b, node).child; child != kCssMathNone; child = nodeAt(b, child).sibling) {
        uint32_t next = nodeAt(b, child).sibling;
        while (next != kCssMathNone && cssValueIsLiteral(&nodeAt(b, next).value, ',') &&
                (nodeAt(b, next).sibling == kCssMathNone || cssValueIsLiteral(&nodeAt(b, nodeAt(b, next).sibling).value, ','))) {
            unlinkNext(b, child);
            next = nodeAt(b, child).sibling;
        }
    }
    uint32_t child = nodeAt(b, node).child;
    return nodeAt(b, child).sibling == kCssMathNone ? child : node;
}

// Simplifies the tree at `node` (css-values-4/#simplify-a-calculation-tree). Returns the node that replaces `node`.
uint32_t simplify(Builder* b, uint32_t node) {
    CssMathNode& root = nodeAt(b, node);
    if (isNumeric(root)) {
        canonicalize(&root.value);
        return node;
    }
    if (root.leaf != kCssMathNone || root.value.kind == PrCssValueKind_Keyword)
        return node;

    uint32_t previous = kCssMathNone;
    for (uint32_t child = root.child; child != kCssMathNone;) {
        uint32_t next = nodeAt(b, child).sibling;
        uint32_t simplified = simplify(b, child);
        nodeAt(b, simplified).sibling = next;
        if (previous == kCssMathNone)
            nodeAt(b, node).child = simplified;
        else
            nodeAt(b, previous).sibling = simplified;
        previous = simplified;
        child = next;
    }

    if (nodeAt(b, node).value.kind == PrCssValueKind_Function)
        return simplifyFunction(b, node);
    switch (cssValueCalcOperator(&nodeAt(b, node).value)) {
        case PrCssCalc_Negate:    return simplifyNegate(b, node);
        case PrCssCalc_Invert:    return simplifyInvert(b, node);
        case PrCssCalc_Sum:       return simplifySum(b, node);
        default:                        return simplifyProduct(b, node);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing the tree to the pool

void emit(Builder* b, CssValuePool* pool, uint32_t node) {
    const CssMathNode& n = nodeAt(b, node);
    if (n.leaf != kCssMathNone) {
        const CssValueNode* leaves = b->scratch->leaves.data + n.leaf;
        pool->values.append(leaves, leaves[0].next);
        return;
    }
    uint32_t at = cssValuePoolAdd(pool, n.value);
    for (uint32_t child = n.child; child != kCssMathNone; child = nodeAt(b, child).sibling)
        emit(b, pool, child);
    pool->values.data[at].next = pool->values.count - at;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sum values (css-typed-om-1/#create-a-sum-value)

constexpr uint32_t kSumMaxUnits = 8;

struct UnitPower {
    PrCssUnit unit;
    int32_t power;
};

// One term of a sum value: a number and its units, sorted. No unit is raised to the power zero.
struct SumItem {
    double value;
    UnitPower units[kSumMaxUnits];
    uint32_t count;
};

typedef Array<SumItem> SumValue;

bool unitsEquals(const SumItem& a, const SumItem& b) {
    if (a.count != b.count)
        return false;
    for (uint32_t i = 0; i < a.count; ++i) {
        if (a.units[i].unit != b.units[i].unit || a.units[i].power != b.units[i].power)
            return false;
    }
    return true;
}

// Multiplies the units of `item` by `unit` raised to `power`. Returns false if the item would hold too many units.
bool multiplyUnit(SumItem* item, PrCssUnit unit, int32_t power) {
    uint32_t at = 0;
    while (at < item->count && item->units[at].unit < unit)
        ++at;
    if (at < item->count && item->units[at].unit == unit) {
        item->units[at].power += power;
        if (item->units[at].power == 0) {
            for (uint32_t i = at + 1; i < item->count; ++i)
                item->units[i - 1] = item->units[i];
            --item->count;
        }
        return true;
    }
    if (item->count == kSumMaxUnits)
        return false;
    for (uint32_t i = item->count; i > at; --i)
        item->units[i] = item->units[i - 1];
    item->units[at] = UnitPower{unit, power};
    ++item->count;
    return true;
}

// Returns the unit that a value node holds for `unit`. A number and a percentage both hold Number, and the node's kind
// tells them apart.
PrCssUnit engineUnit(PrCssUnit unit) {
    return unit == PrCssUnit_Percent ? PrCssUnit_Number : unit;
}

// Finds the canonical unit among a unit's compatible units, and the value in that unit. A unit without one, such as a
// relative length, is its own canonical unit.
void canonicalUnit(PrCssUnit unit, double value, PrCssUnit* canonical, double* canonicalValue) {
    PrCssUnit engine = engineUnit(unit);
    double scale = engine != PrCssUnit_Number ? cssUnitScale(engine) : 0;
    if (scale == 0) {
        *canonical = unit;
        *canonicalValue = value;
        return;
    }
    *canonical = PrCssUnit(cssUnitCanonical(cssUnitCategory(engine)));
    *canonicalValue = value * scale;
}

// Adds each item of `add` to the item of `sum` with the same units, or appends it as a separate item.
void addItems(SumValue* sum, const SumValue& add) {
    for (uint32_t i = 0; i < add.count; ++i) {
        uint32_t j = 0;
        while (j < sum->count && !unitsEquals(sum->data[j], add.data[i]))
            ++j;
        if (j < sum->count)
            sum->data[j].value += add.data[i].value;
        else
            sum->push(add.data[i]);
    }
}

bool sumValue(CssValueRun run, uint32_t index, SumValue* out);

// Computes the sum values of the calculations among a math function's arguments: the operands of min(), max() and
// clamp(). Each must be a single item, and all must be in the same units.
bool singleItems(CssValueRun run, uint32_t index, SumValue* items) {
    for (uint32_t at = index + 1; at < index + run.values[index].next; at += run.values[at].next) {
        if (cssValueIsLiteral(&run.values[at], ','))
            continue;
        SumValue sum;
        bool ok = sumValue(run, at, &sum) && sum.count == 1 &&
            (items->count == 0 || unitsEquals(items->data[0], sum.data[0]));
        if (ok)
            items->push(sum.data[0]);
        sum.reset();
        if (!ok)
            return false;
    }
    return items->count != 0;
}

bool sumValue(CssValueRun run, uint32_t index, SumValue* out) {
    const CssValueNode* value = &run.values[index];
    uint32_t end = index + value->next;
    out->clear();
    PrCssUnit unit;
    if (cssNumericUnit(value, &unit)) {
        SumItem item = {};
        PrCssUnit canonical;
        canonicalUnit(unit, value->number, &canonical, &item.value);
        if (canonical != PrCssUnit_Number)
            multiplyUnit(&item, canonical, 1);
        out->push(item);
        return true;
    }
    if (value->kind == PrCssValueKind_Math)
        return value->next > 1 && sumValue(run, index + 1, out);
    if (value->kind == PrCssValueKind_Function) {
        PrCssKeyword name = cssValueKeyword(value);
        bool minimum = name == PrCssKeyword_Min;
        if (!minimum && name != PrCssKeyword_Max && name != PrCssKeyword_Clamp)
            return false;
        SumValue items;
        bool ok = singleItems(run, index, &items) && (cssValueKeyword(value) != PrCssKeyword_Clamp || items.count == 3);
        if (ok && cssValueKeyword(value) == PrCssKeyword_Clamp) {
            SumItem result = items.data[1];
            double lower = items.data[0].value;
            double upper = items.data[2].value;
            result.value = result.value < lower ? lower : result.value;
            result.value = result.value > upper ? upper : result.value;
            result.value = lower > result.value ? lower : result.value;
            out->push(result);
        } else if (ok) {
            SumItem result = items.data[0];
            for (uint32_t i = 1; i < items.count; ++i) {
                double x = items.data[i].value;
                result.value = minimum ? (x < result.value ? x : result.value) : (x > result.value ? x : result.value);
            }
            out->push(result);
        }
        items.reset();
        return ok;
    }
    if (value->kind != PrCssValueKind_Calc)
        return false;
    switch (cssValueCalcOperator(value)) {
        case PrCssCalc_Sum:
            for (uint32_t at = index + 1; at < end; at += run.values[at].next) {
                SumValue term;
                bool ok = sumValue(run, at, &term);
                if (ok)
                    addItems(out, term);
                term.reset();
                if (!ok)
                    return false;
            }
            return true;
        case PrCssCalc_Negate:
            if (!sumValue(run, index + 1, out))
                return false;
            for (uint32_t i = 0; i < out->count; ++i)
                out->data[i].value = -out->data[i].value;
            return true;
        case PrCssCalc_Product: {
            out->push(SumItem{1, {}, 0});
            for (uint32_t at = index + 1; at < end; at += run.values[at].next) {
                SumValue factor;
                SumValue product;
                bool ok = sumValue(run, at, &factor);
                for (uint32_t i = 0; ok && i < out->count; ++i) {
                    for (uint32_t j = 0; ok && j < factor.count; ++j) {
                        SumItem item = out->data[i];
                        item.value *= factor.data[j].value;
                        for (uint32_t k = 0; ok && k < factor.data[j].count; ++k)
                            ok = multiplyUnit(&item, factor.data[j].units[k].unit, factor.data[j].units[k].power);
                        product.push(item);
                    }
                }
                if (ok) {
                    out->clear();
                    addItems(out, product);
                }
                factor.reset();
                product.reset();
                if (!ok)
                    return false;
            }
            return true;
        }
        default:
            if (!sumValue(run, index + 1, out) || out->count != 1)
                return false;
            out->data[0].value = 1 / out->data[0].value;
            for (uint32_t i = 0; i < out->data[0].count; ++i)
                out->data[0].units[i].power = -out->data[0].units[i].power;
            return true;
    }
}

// Gets the unit of a sum value item (css-typed-om-1/#create-a-cssunitvalue-from-a-sum-value-item). Returns false for an
// item in more than one unit, or in one unit raised to a power other than one.
bool unitOfItem(const SumItem& item, PrCssUnit* unit) {
    if (item.count == 0) {
        *unit = PrCssUnit_Number;
        return true;
    }
    if (item.count > 1 || item.units[0].power != 1)
        return false;
    *unit = item.units[0].unit;
    return true;
}

// Converts `value` in `from` to a number in `to`, if the two units are compatible
// (css-typed-om-1/#convert-a-cssunitvalue).
bool convert(double value, PrCssUnit from, PrCssUnit to, double* out) {
    if (from == to) {
        *out = value;
        return true;
    }
    PrCssUnit a = engineUnit(from);
    PrCssUnit b = engineUnit(to);
    if (a == PrCssUnit_Number || b == PrCssUnit_Number || cssUnitCategory(a) != cssUnitCategory(b))
        return false;
    double scaleA = cssUnitScale(a);
    double scaleB = cssUnitScale(b);
    if (scaleA == 0 || scaleB == 0)
        return false;
    *out = value * scaleA / scaleB;
    return true;
}

// Returns the type of a value in `unit` (css-typed-om-1/#cssnumericvalue-create-a-type).
Type unitType(PrCssUnit unit) {
    if (unit == PrCssUnit_Number)
        return kNumberType;
    if (unit == PrCssUnit_Percent)
        return typeOf(PrCssNumericBaseType_Percent);
    PrCssNumericBaseType base = baseOf(cssUnitCategory(engineUnit(unit)));
    return base != kBaseTypeNone ? typeOf(base) : kFailure;
}

// Replaces the pool's values from `first` on with a PrCssValueKind_Math value over the tree at `root`, simplified.
void replaceWithTree(Builder* b, CssValuePool* pool, uint32_t first, Built root) {
    uint32_t tree = simplify(b, root.node);
    pool->values.count = first;
    CssValueNode math = {};
    math.kind = PrCssValueKind_Math;
    math.aux = b->slot.category != CssUnitCategory_Unknown ? b->slot.category : categoryOf(root.type);
    uint32_t at = cssValuePoolAdd(pool, math);
    emit(b, pool, tree);
    pool->values.data[at].next = pool->values.count - at;
}

Type typeFrom(const PrCssNumericType& type) {
    Type typed = kNumberType;
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i)
        typed.exponent[i] = int8_t(type.exponents[i]);
    typed.hint = type.hasPercentHint ? PrCssNumericBaseType(type.percentHint) : kBaseTypeNone;
    return typed;
}

PrCssNumericType numericTypeOf(const Type& type) {
    PrCssNumericType out = {};
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i)
        out.exponents[i] = type.exponent[i];
    out.hasPercentHint = type.hint != kBaseTypeNone;
    out.percentHint = out.hasPercentHint ? PrCssNumericBaseType(type.hint) : PrCssNumericBaseType(0);
    return out;
}

} // namespace

void cssMathScratchReset(CssMathScratch* scratch) {
    scratch->nodes.reset();
    scratch->leaves.reset();
}

bool cssMathReplace(CssValuePool* pool, uint32_t first, CssMathSlot slot, CssMathScratch* scratch) {
    scratch->nodes.count = 0;
    scratch->leaves.count = 0;
    Builder b = {pool->values.data, scratch, slot, 0, kCssMathMaxDepth, true};
    Cursor c = {first, pool->values.count};
    Built root = buildValue(&b, &c);
    if (root.node == kCssMathNone || c.at != c.end || !matchesSlot(root.type, slot))
        return false;
    replaceWithTree(&b, pool, first, root);
    return true;
}

bool cssMathAdopt(CssValuePool* pool, uint32_t first, CssMathSlot slot, CssMathScratch* scratch) {
    PR_ASSERT(pool->values.data[first].kind == PrCssValueKind_Math);
    PR_ASSERT(first + pool->values.data[first].next == pool->values.count);
    scratch->nodes.count = 0;
    scratch->leaves.count = 0;
    Builder b = {pool->values.data, scratch, slot, 0, kCssMathMaxDepth, true};
    uint32_t root = first + 1;
    if (root == pool->values.count || root + pool->values.data[root].next != pool->values.count)
        return false;
    Built built = adoptNode(&b, root);
    if (built.node == kCssMathNone || !matchesSlot(built.type, slot))
        return false;
    replaceWithTree(&b, pool, first, built);
    return true;
}

void cssMathWrap(CssValueNode value, CssValueNode out[PR_NONNULL 2]) {
    canonicalize(&value);
    value.next = 1;
    value.kind = value.kind == PrCssValueKind_Integer ? PrCssValueKind_Number : value.kind;
    CssValueNode math = {};
    math.kind = PrCssValueKind_Math;
    if (value.kind == PrCssValueKind_Dimension)
        math.aux = cssUnitCategory(value.unit);
    else if (value.kind == PrCssValueKind_Percentage)
        math.aux = CssUnitCategory_Percent;
    else
        math.aux = CssUnitCategory_Number;
    math.next = 2;
    out[0] = math;
    out[1] = value;
}

void cssMathAddValue(CssValuePool* pool, CssValueNode value) {
    CssValueNode math[2];
    cssMathWrap(value, math);
    uint32_t at = cssValuePoolAdd(pool, math[0]);
    cssValuePoolAdd(pool, math[1]);
    pool->values.data[at].next = 2;
}

bool cssNumericUnit(const CssValueNode* value, PrCssUnit* out) {
    switch (value->kind) {
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:     *out = PrCssUnit_Number; return true;
        case PrCssValueKind_Percentage: *out = PrCssUnit_Percent; return true;
        case PrCssValueKind_Dimension:
            if (value->unit == PrCssUnit_Number || value->unit == PrCssUnit_Unknown)
                return false;
            *out = PrCssUnit(value->unit);
            return true;
        default:
            return false;
    }
}

CssValueNode cssNumericValue(double number, PrCssUnit unit) {
    if (unit == PrCssUnit_Number)
        return cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, number);
    if (unit == PrCssUnit_Percent)
        return cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, number);
    return cssNumberValue(PrCssValueKind_Dimension, engineUnit(unit), number);
}

PrStringView cssNumericUnitName(PrCssUnit unit) {
    switch (unit) {
        case PrCssUnit_Number: return "number";
        case PrCssUnit_Percent: return "percent";
        case PrCssUnit_Unknown: return PrStringView{nullptr, 0};
        // A unit is written lowercased, except for these, which the engine spells differently.
        case PrCssUnit_Q: return "q";
        case PrCssUnit_Hz: return "hz";
        case PrCssUnit_KHz: return "khz";
        case PrCssUnit_DB: return "db";
        default: return cssUnitName(engineUnit(unit));
    }
}

bool cssNumericUnitFromName(PrStringView name, PrCssUnit* out) {
    if (asciiEqualsFold(name, "number")) {
        *out = PrCssUnit_Number;
        return true;
    }
    if (asciiEqualsFold(name, "percent")) {
        *out = PrCssUnit_Percent;
        return true;
    }
    PrCssUnit unit = cssUnitFromText(name);
    if (unit == PrCssUnit_Number || unit == PrCssUnit_Unknown)
        return false;
    *out = PrCssUnit(unit);
    return true;
}

bool cssMathType(CssValueRun run, uint32_t index, PrCssNumericType* out) {
    CssMathScratch scratch = {};
    Builder b = {
        run.values, &scratch,   CssMathSlot{CssUnitCategory_Unknown, CssUnitCategory_Percent},
        0,          UINT32_MAX, false,
    };
    // A Math's type is its tree's type.
    const CssValueNode* value = &run.values[index];
    bool math = value->kind == PrCssValueKind_Math;
    Built built = !math             ? adoptNode(&b, index) : value->next > 1 ? adoptNode(&b, index + 1) : kInvalid;
    cssMathScratchReset(&scratch);
    if (built.node == kCssMathNone || built.type.failure)
        return false;
    *out = numericTypeOf(built.type);
    return true;
}

CssUnitCategory cssMathCategory(const PrCssNumericType& type) {
    Type typed = typeFrom(type);
    return resolves(typed) ? categoryOf(typed) : CssUnitCategory_Unknown;
}

bool cssNumericTypeOfUnit(PrCssUnit unit, PrCssNumericType* out) {
    Type type = unitType(unit);
    if (!type.failure)
        *out = numericTypeOf(type);
    return !type.failure;
}

bool cssNumericTypeAdd(const PrCssNumericType& a, const PrCssNumericType& b, PrCssNumericType* out) {
    Type sum = addTypes(typeFrom(a), typeFrom(b));
    if (!sum.failure)
        *out = numericTypeOf(sum);
    return !sum.failure;
}

bool cssNumericTypeMultiply(const PrCssNumericType& a, const PrCssNumericType& b, PrCssNumericType* out) {
    Type product = multiplyTypes(typeFrom(a), typeFrom(b));
    if (!product.failure)
        *out = numericTypeOf(product);
    return !product.failure;
}

PrCssNumericType cssNumericTypeInvert(const PrCssNumericType& type) {
    return numericTypeOf(invertType(typeFrom(type)));
}

bool cssMathTo(CssValueRun run, uint32_t index, PrCssUnit unit, double* out) {
    SumValue sum;
    PrCssUnit from;
    bool ok = sumValue(run, index, &sum) && sum.count == 1 && unitOfItem(sum.data[0], &from) &&
        convert(sum.data[0].value, from, unit, out);
    sum.reset();
    return ok;
}

bool cssMathToSum(CssValueRun run, uint32_t index, const PrCssUnit* PR_NULLABLE units, uint32_t count, CssValuePool* out) {
    SumValue sum;
    Array<SumItem> values;
    bool ok = sumValue(run, index, &sum);
    // Each item as a unit value: its units must be only one unit to the first power, or no unit for a number.
    for (uint32_t i = 0; ok && i < sum.count; ++i) {
        PrCssUnit unit;
        ok = unitOfItem(sum.data[i], &unit);
        if (ok)
            values.push(SumItem{sum.data[i].value, {UnitPower{unit, 1}}, 1});
    }
    if (ok && count == 0) {
        // Sort in code point order of the units' names.
        for (uint32_t i = 1; i < values.count; ++i) {
            for (uint32_t j = i; j > 0 && stringCompare(cssNumericUnitName(values.data[j].units[0].unit),
                    cssNumericUnitName(values.data[j - 1].units[0].unit)) < 0; --j) {
                SumItem swap = values.data[j];
                values.data[j] = values.data[j - 1];
                values.data[j - 1] = swap;
            }
        }
    } else if (ok) {
        // The units' types are added, as for a sum of values in those units.
        Type type = unitType(units[0]);
        for (uint32_t u = 1; u < count; ++u)
            type = addTypes(type, unitType(units[u]));
        ok = !type.failure;
        // The total of the items that convert to each unit. Every item must convert to one of the units.
        Array<SumItem> totals;
        for (uint32_t u = 0; ok && u < count; ++u) {
            double total = 0;
            for (uint32_t i = 0; i < values.count;) {
                double converted;
                if (convert(values.data[i].value, values.data[i].units[0].unit, units[u], &converted)) {
                    total += converted;
                    values.erase(i);
                } else {
                    ++i;
                }
            }
            totals.push(SumItem{total, {UnitPower{units[u], 1}}, 1});
        }
        ok = ok && values.count == 0;
        values.reset();
        values = totals;
    }
    ok = ok && values.count != 0;
    if (ok) {
        CssValueNode math = {};
        math.kind = PrCssValueKind_Math;
        uint32_t at = cssValuePoolAdd(out, math);
        CssValueNode node = {};
        node.kind = PrCssValueKind_Calc;
        node.aux = PrCssCalc_Sum;
        uint32_t operands = cssValuePoolAdd(out, node);
        for (uint32_t i = 0; i < values.count; ++i)
            cssValuePoolAdd(out, cssNumericValue(values.data[i].value, values.data[i].units[0].unit));
        out->values.data[operands].next = out->values.count - operands;
        out->values.data[at].next = out->values.count - at;
    }
    sum.reset();
    values.reset();
    return ok;
}

} // namespace pr_impl
