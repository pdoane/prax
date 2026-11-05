// CSS Value Representation
//
// css-color-5 - https://drafts.csswg.org/css-color-5/
// css-conditional-5 - https://drafts.csswg.org/css-conditional-5/
// css-grid-1 - https://drafts.csswg.org/css-grid-1/
// css-values-4 - https://drafts.csswg.org/css-values-4/
//
// A value is a preorder tree of nodes. A node's `next` is the distance to the node after its subtree, so a subtree can
// be copied or moved without fixups.

#pragma once

#include "pr/color.h"
#include "pr/css.h"
#include "pr/css_descriptors.h"
#include "pr/css_properties.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "css/src/css_syntax.h"
#include "css/src/css_unit.h"
#include "css/src/css_value_def.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Aux

// The fields that a node's `aux` holds. Each member's comment names the value kind that it applies to. The members of
// different kinds overlap.
enum {
    CssValueAuxFlag_Keyword = 0xffff,         // keyword, function: the PrCssKeyword it names

    // Ident of an <mf-name>: the PrCssDescriptor that it names. PrCssDescriptor_None if the feature or its value is
    // unknown.
    CssValueAuxFlag_Feature = 0x3fff,

    // Ident of an <mf-name>: written with the min- or the max- prefix (mediaqueries-5/#mq-min-max).
    CssValueAuxFlag_FeatureMin = 1 << 14,
    CssValueAuxFlag_FeatureMax = 1 << 15,

    // Ident of a <declaration>'s name: set if a style rule accepts the declaration (css-conditional-3/#dfn-support).
    CssValueAuxFlag_Supported = 1 << 0,

    // Tokens: set if they hold an attr() (css-values-5/#attr-notation), which reads the attributes of the element
    // styled.
    CssValueAuxFlag_ReadsAttributes = 1 << 0,

    // String of a <font-family-name>: set if the name reads back the same when written as identifiers. Such a name
    // serializes as identifiers (css-fonts-4/#font-family-name-syntax).
    CssValueAuxFlag_FamilyIdents = 1 << 0,

    // Ident naming a <pseudo-class-selector>: the CssPseudoClass. Inside a <pseudo-element-selector>, the
    // CssPseudoElement.
    CssValueAuxFlag_Pseudo = 0xffff,

    CssValueAuxFlag_Production = 0xffff,      // production: the CssValueDef that it matched
    CssValueAuxFlag_Literal = 0xffff,         // literal: the character
    CssValueAuxFlag_Longhand = 0xffff,        // longhand: the PrCssProperty that the component sets
    CssValueAuxFlag_ColorSpace = 0x00ff,      // color: PrColorSpace

    // Color: set for sRGB written as a hex color, rgb() or rgba() rather than color().
    CssValueAuxFlag_ColorLegacy = 1 << 8,

    CssValueAuxFlag_MathCategory = 0x000f,    // math: the CssUnitCategory that its type resolves to
    CssValueAuxFlag_CalcOperator = 0x0003,    // calc: the PrCssCalc
};
typedef uint16_t CssValueAuxFlags;

// ---------------------------------------------------------------------------------------------------------------------
// Value nodes

// One node in a value tree.
struct CssValueNode {
    PrCssValueKind kind;

    // Dimension only. For PrCssUnit_Unknown, an Ident child holds the unit's text.
    PrCssUnit unit;

    CssValueAuxFlags aux;
    uint32_t next;              // the number of values from here to the one after this subtree
    union {
        struct {
            uint32_t start;     // index into CssValuePool.text
            uint32_t length;
        } text;                 // Ident, String, Url, Tokens
        double number;          // Integer, Number, Percentage, Dimension
        struct {
            uint32_t first;     // the code point that the range starts at
            uint32_t last;      // the code point that the range ends at
        } range;                // UnicodeRange
        struct {
            uint32_t start;     // the first token of the match, including whitespace before it
            uint32_t count;
        } tokens;               // Production
    };
};

static_assert(sizeof(CssValueNode) == 16);

// ---------------------------------------------------------------------------------------------------------------------
// Value pools

// A contiguous preorder tree of values and the text that they reference.
struct CssValuePool {
    RawArray<CssValueNode> values;
    RawArray<char> text;           // the text that values reference by `text.start`
};

// ---------------------------------------------------------------------------------------------------------------------
// Value spans

// A range of a pool's values. The tree is preorder, so a value's children are inside it.
struct CssValueSpan {
    uint32_t start;
    uint32_t count;
};

// Returns a keyword value with no children.
inline CssValueNode cssKeywordValue(PrCssKeyword keyword) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Keyword;
    value.aux = keyword;
    value.next = 1;
    return value;
}

// Returns an integer, a number, a percentage or a dimension value, with no children.
inline CssValueNode cssNumberValue(PrCssValueKind kind, PrCssUnit unit, double number) {
    CssValueNode value = {};
    value.kind = kind;
    value.unit = unit;
    value.next = 1;
    value.number = number;
    return value;
}

// Returns a function value named `name` with no arguments yet. A caller that adds its arguments after it sets its
// `next`.
inline CssValueNode cssFunctionValue(PrCssKeyword name) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Function;
    value.aux = name;
    value.next = 1;
    return value;
}

// Returns a literal character value with no children.
inline CssValueNode cssLiteralValue(char character) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Literal;
    value.aux = uint8_t(character);
    value.next = 1;
    return value;
}

// The accessors below read `aux` in the form that the value's kind defines (CssValueAuxFlags).

// Returns a keyword's value, or a function's name.
inline PrCssKeyword cssValueKeyword(const CssValueNode* value) {
    return PrCssKeyword(value->aux);
}

// Returns a production's definition.
inline CssValueDef cssValueProduction(const CssValueNode* value) {
    return CssValueDef(value->aux);
}

// Returns a longhand's property.
inline PrCssProperty cssValueLonghand(const CssValueNode* value) {
    return PrCssProperty(value->aux);
}

// Returns the descriptor of an <mf-name>.
inline PrCssDescriptor cssValueFeature(const CssValueNode* value) {
    return PrCssDescriptor(value->aux & CssValueAuxFlag_Feature);
}

// Every color space fits in a color's aux.
static_assert(PrColorSpace_Count <= CssValueAuxFlag_ColorSpace + 1);

// Returns a color's space.
inline PrColorSpace cssValueColorSpace(const CssValueNode* value) {
    return PrColorSpace(value->aux & CssValueAuxFlag_ColorSpace);
}

// Returns true if a color is sRGB written as a hex color, rgb() or rgba().
inline bool cssValueColorLegacy(const CssValueNode* value) {
    return (value->aux & CssValueAuxFlag_ColorLegacy) != 0;
}

// Returns a math value's category.
inline CssUnitCategory cssValueMathCategory(const CssValueNode* value) {
    return CssUnitCategory(value->aux & CssValueAuxFlag_MathCategory);
}

// Returns the operator of a calc-operator node.
inline PrCssCalc cssValueCalcOperator(const CssValueNode* value) {
    return PrCssCalc(value->aux & CssValueAuxFlag_CalcOperator);
}

// Returns a literal's character.
inline char cssValueLiteral(const CssValueNode* value) {
    return char(value->aux);
}

// Returns true if a value is the literal `character`.
inline bool cssValueIsLiteral(const CssValueNode* value, char character) {
    return value->kind == PrCssValueKind_Literal && value->aux == uint16_t(character);
}

// Returns true if a value is the keyword `keyword`.
inline bool cssValueIsKeyword(const CssValueNode* value, PrCssKeyword keyword) {
    return value->kind == PrCssValueKind_Keyword && cssValueKeyword(value) == keyword;
}

// Returns true if a value is the function that `keyword` names.
inline bool cssValueIsFunction(const CssValueNode* value, PrCssKeyword keyword) {
    return value->kind == PrCssValueKind_Function && cssValueKeyword(value) == keyword;
}

// Returns true if a value is a match of the structural production `production`.
inline bool cssValueIsProduction(const CssValueNode* value, CssValueDef production) {
    return value->kind == PrCssValueKind_Production && cssValueProduction(value) == production;
}

// A run of values and the text that they reference. Each value's `text.start` indexes the text.
struct CssValueRun {
    const CssValueNode* PR_NULLABLE values;
    uint32_t count;
    const char* PR_NULLABLE text;
};

// Returns the text of a value of the run, for a value that holds text.
inline PrStringView cssValueText(CssValueRun run, const CssValueNode* value) {
    return PrStringView{run.text + value->text.start, value->text.length};
}

// Returns the keyword of a run that is a single keyword, or PrCssKeyword_Count otherwise.
inline PrCssKeyword cssValueRunKeyword(CssValueRun run) {
    if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword)
        return cssValueKeyword(run.values);
    return PrCssKeyword_Count;
}

// Returns the keyword that a run begins with, or PrCssKeyword_Count if its first value is no keyword.
inline PrCssKeyword cssValueRunFirstKeyword(CssValueRun run) {
    if (run.count != 0 && run.values[0].kind == PrCssValueKind_Keyword)
        return cssValueKeyword(run.values);
    return PrCssKeyword_Count;
}

// Returns the value at `index` of the run, with its children.
inline CssValueRun cssValueSubtree(CssValueRun run, uint32_t index) {
    return CssValueRun{run.values + index, run.values[index].next, run.text};
}

// Writes the index of the first value of each argument of the function at `at` of the run, which commas separate, to
// `starts`, up to `capacity` of them. Returns the number of arguments.
inline uint32_t cssValueArguments(CssValueRun run, uint32_t at, uint32_t* starts, uint32_t capacity) {
    uint32_t count = 0;
    uint32_t end = at + run.values[at].next;
    bool fresh = true;
    for (uint32_t i = at + 1; i < end; i += run.values[i].next) {
        if (cssValueIsLiteral(&run.values[i], ',')) {
            fresh = true;
        } else if (fresh) {
            if (count < capacity)
                starts[count] = i;
            ++count;
            fresh = false;
        }
    }
    return count;
}

// Returns the children of the value at `index` of the run.
inline CssValueRun cssValueChildren(CssValueRun run, uint32_t index) {
    return CssValueRun{run.values + index + 1, run.values[index].next - 1, run.text};
}

// Calls `component(vertical, index, offset)` for each axis that a computed <position> sets (css-values-4/#position):
// one value, two values, or edge keywords, of which one or two are followed by an offset. `index` is the axis's value,
// and `offset` is the index of the offset from its edge keyword, or kArrayMissing. An axis that the position does not
// set is centered.
template <typename Component>
void cssPositionAxes(CssValueRun run, Component component) {
    uint32_t items[4];
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count && count < 4; i += run.values[i].next)
        items[count++] = i;
    auto keyword = [&](uint32_t item) {
        const CssValueNode* value = &run.values[items[item]];
        return value->kind == PrCssValueKind_Keyword ? cssValueKeyword(value) : PrCssKeyword_Count;
    };
    auto vertical = [&](uint32_t item) {
        return keyword(item) == PrCssKeyword_Top || keyword(item) == PrCssKeyword_Bottom;
    };
    auto horizontal = [&](uint32_t item) {
        return keyword(item) == PrCssKeyword_Left || keyword(item) == PrCssKeyword_Right;
    };
    if (count == 1) {
        component(vertical(0), items[0], kArrayMissing);
    } else if (count == 2) {
        bool swapped = vertical(0) || horizontal(1);
        component(false, items[swapped ? 1 : 0], kArrayMissing);
        component(true, items[swapped ? 0 : 1], kArrayMissing);
    } else {
        // Each keyword, with the offset that follows it. center has none, and takes the axis that the other keyword
        // leaves.
        bool axes[4] = {};
        uint32_t offsets[4];
        uint32_t keywords[4];
        bool centered[4] = {};
        uint32_t found = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (keyword(i) == PrCssKeyword_Count)
                continue;
            keywords[found] = items[i];
            axes[found] = vertical(i);
            centered[found] = keyword(i) == PrCssKeyword_Center;
            offsets[found] = i + 1 < count && keyword(i + 1) == PrCssKeyword_Count ? items[i + 1] : kArrayMissing;
            ++found;
        }
        if (found == 2 && centered[0])
            axes[0] = !axes[1];
        if (found == 2 && centered[1])
            axes[1] = !axes[0];
        for (uint32_t i = 0; i < found; ++i)
            component(axes[i], keywords[i], offsets[i]);
    }
}

// Returns the run for a span of the pool.
inline CssValueRun cssValuePoolRun(const CssValuePool* pool, CssValueSpan span) {
    return CssValueRun{pool->values.data + span.start, span.count, pool->text.data};
}

// Returns the text of a value of the pool, for a value that holds text.
inline PrStringView cssValuePoolText(const CssValuePool* pool, const CssValueNode* value) {
    return PrStringView{pool->text.data + value->text.start, value->text.length};
}

// Returns true if a value of `kind` holds text.
inline bool cssValueHasText(PrCssValueKind kind) {
    return kind == PrCssValueKind_Ident || kind == PrCssValueKind_String || kind == PrCssValueKind_Url ||
        kind == PrCssValueKind_Tokens;
}

// Returns true if a value of `kind` is a number: an integer, a number, a percentage or a dimension.
inline bool cssValueIsNumeric(PrCssValueKind kind) {
    return kind == PrCssValueKind_Integer || kind == PrCssValueKind_Number ||
        kind == PrCssValueKind_Percentage || kind == PrCssValueKind_Dimension;
}

// Returns true if two runs hold the same values.
bool cssValueRunEquals(CssValueRun a, CssValueRun b);

// Writes the text that a run references, as one range. The range runs from the smallest `text.start` of the run's
// values to the largest end.
void cssValueRunText(CssValueRun run, uint32_t* start, uint32_t* size);

// Copies a run to `values`, and the text that it references to `text`. The `text.start` of each copy indexes a buffer
// that holds `text` at `offset`. Returns the number of text bytes copied.
uint32_t cssValueRunCopy(CssValueRun run, CssValueNode* values, char* text, uint32_t offset);

// A run of values and its text, held in one allocation: the values, then the text. The empty list holds no allocation.
struct CssValueList {
    CssValueNode* PR_NULLABLE values;
    uint32_t count;
    uint32_t textSize;
};

// Creates a list that holds a copy of `run`.
CssValueList cssValueListCopy(CssValueRun run);

// Returns the list's values and text as a run.
inline CssValueRun cssValueListRun(const CssValueList* list) {
    return CssValueRun{list->values, list->count, reinterpret_cast<const char*>(list->values + list->count)};
}

// Frees what the list holds and leaves it empty.
void cssValueListReset(CssValueList* list);

// Frees what the pool holds and leaves it empty.
void cssValuePoolReset(CssValuePool* pool);

// Adds a value with no children and returns its index. A parent sets its own `next` once its children are added.
uint32_t cssValuePoolAdd(CssValuePool* pool, CssValueNode value);

// Copies a run and the text that it references onto the end of the pool. Returns the span of the copy.
CssValueSpan cssValuePoolAddRun(CssValuePool* pool, CssValueRun run);

// Copies a span of the pool, and the text that it references, onto the end of the pool.
CssValueSpan cssValuePoolRepeat(CssValuePool* pool, CssValueSpan span);

// Copies `text` onto the end of the pool's text. Returns the index of the first byte.
uint32_t cssValuePoolAddText(CssValuePool* pool, PrStringView text);

// Copies the value of the text token at `index`, with escapes resolved, onto the end of the pool's text. Writes its
// length to `length`, and returns the index of its first byte.
uint32_t cssValuePoolAddTokenText(CssValuePool* pool, const CssTokenStream* ts, uint32_t index, uint32_t* length);

// Copies the value of `text`, with escapes resolved as cssValueNext reads them, onto the end of the pool's text. Writes
// its length to `length`, and returns the index of its first byte.
uint32_t cssValuePoolAddValueText(CssValuePool* pool, PrStringView text, uint32_t* length);

// Adds a Tokens value for `span`, kept as the source that it covers, including comments and whitespace. It serializes
// exactly as the author wrote it (css-variables-1/#serializing-custom-props). A last token that the input ended inside,
// such as `foo\` or `"foo`, is written as it was read (css-syntax-3/#consume-escaped-code-point).
void cssValuePoolAddTokens(CssValuePool* pool, const CssTokenStream* ts, CssTokenSpan span);

// ---------------------------------------------------------------------------------------------------------------------
// Values (PrCssValue)

// A node open in a value.
struct CssValueOpen {
    uint32_t node;
    uint32_t text;              // the offset where the text of the node and its children starts
};

// A sequence of trees that a caller builds one node at a time. A node is opened, given its children, and closed. An
// open node has a `next` of one.
struct CssValue {
    CssValuePool pool;
    RawArray<CssValueOpen> open;   // innermost last, so the count is the depth at which a node is added
};

// Returns the value's trees as a run.
inline CssValueRun cssValueRun(const CssValue* value) {
    return CssValueRun{value->pool.values.data, value->pool.values.count, value->pool.text.data};
}

// Frees the value's memory and empties it.
void cssValueReset(PrCssValue* value);

// Empties the value and keeps its memory.
void cssValueClear(PrCssValue* value);

// Returns true if the children of the value at `index` are what its type requires. A leaf has no children, except a
// dimension in a unit that no table names, which has its unit's text as an ident. A color has channels and an alpha,
// each of which is numeric, a keyword or a calculation. A math value has one calculation. A Negate or an Invert node
// has one operand, and a Sum or a Product has one or more.
bool cssValueShapeValid(CssValueRun run, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Given values
//
// A value that a caller gives rather than writes is matched again through the tokens that it would be written as. The
// exceptions are values whose tokens would lose information: a numeric value, a calculation, a string, a url and a
// unicode range. Those become CssTokenKind_Value tokens, which the terminals that accept them match by type
// (css_parse_terminal.cpp).

// Returns the value, with its children, that the CssTokenKind_Value token at `index` stands for. The token's closer is
// the value's index in the array's `values`.
inline CssValueRun cssTokenValue(const CssTokenArray* tokens, uint32_t index) {
    const CssValueNode* value = tokens->values + tokens->closers.data[index];
    return CssValueRun{value, value->next, tokens->valueText};
}

// Replaces what `tokens` held with the tokens of `run`. A keyword, an ident, a function and a literal become the tokens
// that they are written as. A color becomes the function that it folds from, and a path() the string of its path data.
// The components of a shorthand become their values. The bytes of the tokens go to `source`, and a CssTokenKind_Value
// token covers none of them. The run must outlive the tokens. If cssValuePoolAddTokens keeps a run of these tokens as
// tokens, each token becomes the text that it is written as.
void cssValueTokens(CssTokenArray* tokens, PrTextBuffer* source, CssValueRun run);

} // namespace pr_impl

// C opaque types
struct PrCssValue : pr_impl::CssValue {};

PR_ASSUME_NONNULL_END
