// CSS grammar terminal parsing.
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/

#include "css/src/css_parse_terminal.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "css/src/css_color.h"
#include "css/src/css_color_value.h"
#include "css/src/css_match.h"
#include "css/src/css_path.h"
#include "css/src/css_serialize.h"
#include "css/src/css_supports.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Given values
//
// A value given as a value rather than written (cssValueTokens) is accepted wherever a terminal accepts the tokens that
// it would be written as. It is accepted by its type rather than its spelling, so a number is never a length, however
// it would be written.

// Returns the value that the CssTokenKind_Value token at the read point stands for, or null for any other token.
const CssValueNode* PR_NULLABLE givenValue(const CssTokenStream* ts) {
    return cssPeek(ts) == CssTokenKind_Value ? cssTokenValue(ts->tokens, ts->pos).values : nullptr;
}

// Moves the given value at the read point into the pool unchanged.
void takeGiven(CssTokenStream* ts, CssValuePool* pool) {
    cssValuePoolAddRun(pool, cssTokenValue(ts->tokens, ts->pos));
    ++ts->pos;
}

// Moves the given numeric value at the read point into the pool as a value of `kind`. If `wrap` is set, because the
// place does not accept the number as is, the value is wrapped in a calculation
// (css-typed-om-1/#create-an-internal-representation). That happens for a value that is not an integer if an integer is
// required. A calculation accepts any number, and stores it as the place resolves it.
void takeGivenNumber(CssTokenStream* ts, CssValuePool* pool, PrCssValueKind kind, bool wrap) {
    CssValueNode value = *givenValue(ts);
    value.kind = kind;
    ++ts->pos;
    if (!wrap) {
        cssValuePoolAdd(pool, value);
        return;
    }
    cssMathAddValue(pool, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared shapes

// Matches one token whose value is text and copies it into the pool, or a given value of `kind`.
bool textToken(CssTokenStream* ts, CssValuePool* pool, CssTokenKind tokenType, PrCssValueKind kind) {
    if (const CssValueNode* given = givenValue(ts)) {
        if (given->kind != kind)
            return false;
        takeGiven(ts, pool);
        return true;
    }
    if (cssPeek(ts) != tokenType)
        return false;

    CssValueNode value = {};
    value.kind = kind;
    value.text.start = cssValuePoolAddTokenText(pool, ts, ts->pos, &value.text.length);
    ++ts->pos;
    cssValuePoolAdd(pool, value);
    return true;
}

// Matches one dimension token that measures `category`, or a given dimension that measures it.
bool dimension(CssTokenStream* ts, CssValuePool* pool, CssUnitCategory category) {
    uint32_t at = ts->pos;
    if (const CssValueNode* given = givenValue(ts)) {
        if (given->kind != PrCssValueKind_Dimension || cssUnitCategory(given->unit) != category)
            return false;
        takeGiven(ts, pool);
        return true;
    }
    if (cssPeek(ts) != CssTokenKind_Dimension)
        return false;

    PrCssUnit unit = cssTokenUnit(ts->tokens, at);
    if (cssUnitCategory(unit) != category)
        return false;

    ++ts->pos;
    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, unit, cssTokenNumber(ts->tokens, at)));
    return true;
}

// Matches one number token, or a given number. If `integerOnly` is set, the token must be written without a fractional
// part or an exponent. A given number that is not an integer is wrapped in a calculation if an integer is required and
// the match calculates.
bool number(CssTokenStream* ts, CssValuePool* pool, bool integerOnly, bool calculate = false) {
    uint32_t at = ts->pos;
    if (const CssValueNode* given = givenValue(ts)) {
        if (given->kind != PrCssValueKind_Integer && given->kind != PrCssValueKind_Number)
            return false;
        bool whole = given->number == floorF64(given->number) && isFiniteF64(given->number);
        if (integerOnly && !whole && !calculate)
            return false;
        takeGivenNumber(ts, pool, integerOnly && whole ? PrCssValueKind_Integer : PrCssValueKind_Number, integerOnly && !whole);
        return true;
    }
    if (cssPeek(ts) != CssTokenKind_Number)
        return false;
    if (integerOnly && (cssTokenFlags(ts->tokens, at) & CssTokenFlag_Integer) == 0)
        return false;

    ++ts->pos;
    cssValuePoolAdd(pool, cssNumberValue(integerOnly ? PrCssValueKind_Integer : PrCssValueKind_Number,
        PrCssUnit_Number, cssTokenNumber(ts->tokens, at)));
    return true;
}

// Matches an ident whose keyword `takes` accepts, and stores it as that keyword.
template <typename Takes>
bool keywordOf(const CssMatcher* m, Takes takes) {
    if (cssPeek(m->ts) != CssTokenKind_Ident)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(m->ts->tokens, m->ts->pos);
    if (!takes(keyword))
        return false;
    ++m->ts->pos;
    cssValuePoolAdd(m->pool, cssKeywordValue(keyword));
    return true;
}

// Returns true if the grammar at `index` accepts the ident `keyword`, directly or through the types that it names. The
// arguments of a function count as the function's own.
bool takesKeyword(CssGrammarIndex index, PrCssKeyword keyword) {
    CssGrammarNode node = kCssGrammarNodes[index];
    uint32_t value = cssGrammarValue(node);
    switch (cssGrammarType(node)) {
        case CssGrammarType_Keyword:
            return value == keyword;
        case CssGrammarType_Type:
            return kCssValueDefGrammarRoot[value] != CssGrammarRoot_None && takesKeyword(kCssValueDefGrammarRoot[value], keyword);
        case CssGrammarType_OneOf:
        case CssGrammarType_AnyOf:
        case CssGrammarType_AllOf:
        case CssGrammarType_Sequence:
        case CssGrammarType_KeywordSet:
        case CssGrammarType_Longhand: {
            CssGrammarIndex end = cssGrammarAfter(index);
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < end; child = cssGrammarAfter(child)) {
                if (takesKeyword(child, keyword))
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Numeric function matching (css-values-4/#functional-numeric)

// Returns the grammar of a numeric function named `keyword` that a match may accept: a math function's grammar, or an
// anchor function's grammar if the property accepts that function. Returns CssGrammarRoot_None for any other name.
CssGrammarIndex numericGrammar(const CssMatcher* m, PrCssKeyword keyword) {
    CssValueDef def = cssMathFunction(keyword);
    if (def != CssValueDef_Count)
        return kCssValueDefGrammarRoot[def];
    if (keyword == PrCssKeyword_Anchor && (m->anchors & CssAnchorFunction_Anchor) != 0)
        return kCssValueDefGrammarRoot[CssValueDef_AnchorFn];
    if (keyword == PrCssKeyword_AnchorSize && (m->anchors & CssAnchorFunction_AnchorSize) != 0)
        return kCssValueDefGrammarRoot[CssValueDef_AnchorSizeFn];
    return CssGrammarRoot_None;
}

// Returns true if a value is part of a <random-key> (css-values-5/#random): one of its keywords, or an ident.
bool isRandomKey(const CssValueNode* value) {
    if (value->kind == PrCssValueKind_Ident)
        return true;
    if (value->kind != PrCssValueKind_Keyword)
        return false;
    switch (cssValueKeyword(value)) {
        case PrCssKeyword_Auto:
        case PrCssKeyword_ElementScoped:
        case PrCssKeyword_PropertyScoped:
        case PrCssKeyword_PropertyIndexScoped:
        case PrCssKeyword_Fixed:
            return true;
        default:
            return false;
    }
}

// Returns the <random-ua-ident> of the random function that the token at `at` names: ua-PROPERTY, or ua-PROPERTY-INDEX
// if `indexed` is set. INDEX counts the value's random functions up to and including this one.
CssValueNode randomUaIdent(const CssMatcher* m, uint32_t at, bool indexed) {
    CssValueNode ident = {};
    ident.kind = PrCssValueKind_Ident;
    ident.text.start = cssValuePoolAddText(m->pool, "ua-");
    cssValuePoolAddText(m->pool, cssPropertyName(m->property));
    if (indexed) {
        uint64_t index = 0;
        for (uint32_t i = m->start; i <= at; ++i) {
            index += cssTokenKind(m->ts->tokens, i) == CssTokenKind_Function &&
                cssTokenKeyword(m->ts->tokens, i) == PrCssKeyword_Random;
        }
        char digits[kU64TextMaxBytes];
        size_t count = formatU64(index, digits, 10);
        cssValuePoolAddText(m->pool, "-");
        cssValuePoolAddText(m->pool, PrStringView{digits, count});
    }
    ident.text.length = m->pool->text.count - ident.text.start;
    return ident;
}

// Puts a random function's key in canonical form (css-values-5/#random-simplify). A <random-key> that is `auto`, or
// omitted, is `element-scoped property-index-scoped`. `property-scoped` and `property-index-scoped` are replaced by the
// <random-ua-ident>s they name. The parts of the key are then put in the order its grammar writes them
// (css-values-4/#serialize-a-functional-notation). The random function is the value at `function`, which the token at
// `at` starts. Returns false if the key names a <random-ua-ident> that does not start with `ua-`.
bool canonicalRandomKey(const CssMatcher* m, uint32_t function, uint32_t at) {
    CssValuePool* pool = m->pool;
    uint32_t first = function + 1;
    uint32_t end = first;
    while (end < pool->values.count && isRandomKey(&pool->values.data[end]))
        end += pool->values.data[end].next;

    bool omitted = end == first;
    const CssValueNode* dashed = nullptr;
    const CssValueNode* named = nullptr;
    bool element = omitted;
    PrCssKeyword scoped = omitted ? PrCssKeyword_PropertyIndexScoped : PrCssKeyword_Unknown;
    for (uint32_t i = first; i < end; i += pool->values.data[i].next) {
        const CssValueNode* part = &pool->values.data[i];
        if (part->kind == PrCssValueKind_Ident) {
            PrStringView text = cssValuePoolText(pool, part);
            if (dashed == nullptr && stringStartsWith(text, "--")) {
                dashed = part;
                continue;
            }
            if (!stringStartsWith(text, "ua-"))
                return false;
            named = part;
            continue;
        }
        switch (cssValueKeyword(part)) {
            case PrCssKeyword_Fixed:
                return true;
            case PrCssKeyword_Auto:
                element = true;
                scoped = PrCssKeyword_PropertyIndexScoped;
                break;
            case PrCssKeyword_ElementScoped:
                element = true;
                break;
            default:
                scoped = cssValueKeyword(part);
                break;
        }
    }

    CssValueNode key[4];
    uint32_t count = 0;
    if (dashed != nullptr)
        key[count++] = *dashed;
    if (element)
        key[count++] = cssKeywordValue(PrCssKeyword_ElementScoped);
    if (named != nullptr)
        key[count++] = *named;
    else if (scoped != PrCssKeyword_Unknown && m->property != PrCssProperty_Count)
        key[count++] = randomUaIdent(m, at, scoped == PrCssKeyword_PropertyIndexScoped);
    else if (scoped != PrCssKeyword_Unknown)
        key[count++] = cssKeywordValue(scoped);
    if (omitted)
        key[count++] = cssLiteralValue(',');
    for (uint32_t i = 0; i < count; ++i)
        key[i].next = 1;

    uint32_t removed = end - first;
    pool->values.erase(first, removed);
    for (uint32_t i = 0; i < count; ++i)
        pool->values.insert(first + i, key[i]);
    pool->values.data[function].next = pool->values.data[function].next - removed + count;
    return true;
}

// Matches a function that is not a math function but resolves to a number: sibling-index() or sibling-count()
// (css-values-5/#tree-counting), which take no arguments. They count an element's siblings, so they are rejected in a
// value that belongs to no element, such as a descriptor's value (css-fonts/test_font_feature_values_parsing.html).
bool matchTreeCounting(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    if (m->property == PrCssProperty_Count)
        return false;
    CssTokenStream scan = *ts;
    PrCssKeyword keyword = cssTokenKeyword(ts->tokens, ts->pos);
    ++scan.pos;
    cssSkipWhitespace(&scan);
    if (cssPeek(&scan) != CssTokenKind_CloseParen)
        return false;
    ts->pos = scan.pos + 1;
    CssValueNode value = {};
    value.kind = PrCssValueKind_Function;
    value.aux = keyword;
    cssValuePoolAdd(m->pool, value);
    return true;
}

// Matches a numeric function by its grammar, and puts a random function's key in canonical form.
bool matchNumericFunction(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    if (cssPeek(ts) != CssTokenKind_Function)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(ts->tokens, ts->pos);
    if (keyword == PrCssKeyword_SiblingIndex || keyword == PrCssKeyword_SiblingCount)
        return matchTreeCounting(m);
    CssGrammarIndex root = numericGrammar(m, keyword);
    if (root == CssGrammarRoot_None || m->depth == kCssMathMaxDepth)
        return false;

    CssTokenStream scan = *ts;
    CssTokenSpan span;
    cssConsumeComponentValue(&scan, &span);
    CssTokenStream function = cssSubStream(ts, span);
    CssMatcher nested = cssMatchNested(m, &function);
    ++nested.depth;
    uint32_t first = m->pool->values.count;
    if (!cssMatchWhole(&nested, root, CssValueDef_Count, false))
        return false;
    if (keyword == PrCssKeyword_Random && !canonicalRandomKey(m, first, ts->pos))
        return false;
    ts->pos = scan.pos;
    return true;
}

// Returns true if whitespace surrounds every + and - operator in `span` (css-values-4/#calc-syntax). Whitespace is
// required on both sides of these operators, which are all the + and - delims among a math function's tokens.
bool operatorsSpaced(const CssTokenStream* ts, CssTokenSpan span) {
    bool spaced = true;
    for (uint32_t i = span.start + 1; i + 1 < span.start + span.count; ++i) {
        if (cssTokenKind(ts->tokens, i) != CssTokenKind_Delim)
            continue;
        uint32_t delim = cssTokenDelim(ts->tokens, i);
        if (delim != '+' && delim != '-')
            continue;
        spaced = spaced && cssTokenKind(ts->tokens, i - 1) == CssTokenKind_Whitespace &&
            cssTokenKind(ts->tokens, i + 1) == CssTokenKind_Whitespace;
    }
    return spaced;
}

// Matches a numeric function in the place of a value that `slot` describes, and stores it as its calculation tree. Also
// matches a given calculation of a type that the place accepts.
bool numericFunction(const CssMatcher* m, CssMathSlot slot) {
    uint32_t pos = m->ts->pos;
    uint32_t values = m->pool->values.count;
    uint32_t text = m->pool->text.count;
    if (const CssValueNode* given = givenValue(m->ts)) {
        if (given->kind != PrCssValueKind_Math)
            return false;
        takeGiven(m->ts, m->pool);
        if (cssMathAdopt(m->pool, values, slot, &m->match->math))
            return true;
        m->ts->pos = pos;
        m->pool->values.count = values;
        m->pool->text.count = text;
        return false;
    }
    if (!matchNumericFunction(m))
        return false;
    if ((m->depth != 0 || operatorsSpaced(m->ts, CssTokenSpan{pos, m->ts->pos - pos})) &&
            cssMathReplace(m->pool, values, slot, &m->match->math)) {
        return true;
    }
    m->ts->pos = pos;
    m->pool->values.count = values;
    m->pool->text.count = text;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Numeric Data Types (css-values-4/#numeric-types)
//
// <number> and <integer> differ only in how the token was written. Dimensions accept the units of their category, and a
// unitless zero if the spec allows one. A numeric function can replace any of them (css-values-4/#math). It must be a
// math function whose type resolves to the type, and a <number> may stand for an <integer>
// (css-values-4/#calc-type-checking). Its percentages resolve against the same basis as the type's percentages.

// Matches a <percentage-token>, or a given percentage.
bool percentage(CssTokenStream* ts, CssValuePool* pool) {
    uint32_t at = ts->pos;
    if (const CssValueNode* given = givenValue(ts)) {
        if (given->kind != PrCssValueKind_Percentage)
            return false;
        takeGiven(ts, pool);
        return true;
    }
    if (cssPeek(ts) != CssTokenKind_Percentage)
        return false;

    ++ts->pos;
    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, cssTokenNumber(ts->tokens, at)));
    return true;
}

// Matches a length token: a dimension in a unit of length, or a zero. A zero length may be written without a unit, and
// zero is the same length in every unit (css-values-4/#lengths).
bool length(CssTokenStream* ts, CssValuePool* pool) {
    if (dimension(ts, pool, CssUnitCategory_Length))
        return true;

    uint32_t at = ts->pos;
    if (cssPeek(ts) != CssTokenKind_Number || cssTokenNumber(ts->tokens, at) != 0)
        return false;
    ++ts->pos;
    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, 0));
    return true;
}

// Matches a dimension of `category`, or a numeric function in its place.
bool dimensionOf(const CssMatcher* m, CssUnitCategory category) {
    return dimension(m->ts, m->pool, category) || numericFunction(m, CssMathSlot{category, CssUnitCategory_Unknown});
}

// Matches a dimension of `category` or a percentage of one (css-values-4/#mixed-percentages), or a numeric function in
// its place.
bool dimensionPercentage(const CssMatcher* m, CssUnitCategory category) {
    return dimension(m->ts, m->pool, category) || percentage(m->ts, m->pool) ||
        numericFunction(m, CssMathSlot{category, category});
}

// ---------------------------------------------------------------------------------------------------------------------
// Textual Data Types (css-values-4/#textual-values)
//
// <custom-ident> excludes the CSS-wide keywords. <dashed-ident> is the --foo form.

// Returns true if the token at `index` is a reserved ident (css-values-4/#custom-idents). A <custom-ident> is not one
// of the CSS-wide keywords, and `default` is reserved for future ones.
bool isReservedIdent(const CssTokenStream* ts, uint32_t index) {
    if (cssTokenKind(ts->tokens, index) != CssTokenKind_Ident)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(ts->tokens, index);
    return keyword == PrCssKeyword_Default || cssIsCssWideKeyword(keyword);
}

// Returns true if the ident at the read point is a keyword that the matched property's prose excludes from its
// <custom-ident>s (kCssExcludedIdents).
bool isExcludedIdent(const CssMatcher* m) {
    if (m->property == PrCssProperty_Count || cssPeek(m->ts) != CssTokenKind_Ident)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(m->ts->tokens, m->ts->pos);
    const PrCssKeyword* excluded = kCssExcludedIdents + kCssPropertyExcludedIdents[m->property];
    while (*excluded != PrCssKeyword_Unknown && *excluded != keyword)
        ++excluded;
    return *excluded != PrCssKeyword_Unknown;
}

// ---------------------------------------------------------------------------------------------------------------------
// Family names (css-fonts-4/#font-family-name-syntax, css-speech-1/#propdef-voice-family)

// Returns true if `count` identifiers, starting with `first`, form a name of the kind that a family name production
// accepts.
typedef bool (*FamilyNamesFn)(PrCssKeyword first, uint32_t count);

// Returns true if `count` identifiers, starting with `first`, form a font family name. No keyword of the font-family
// grammar may start a font family name, and a CSS-wide keyword or `default` may appear only among other identifiers
// (css-fonts-4/#font-family-name-syntax).
bool isFontFamilyName(PrCssKeyword first, uint32_t count) {
    if (count == 1 && (first == PrCssKeyword_Default || cssIsCssWideKeyword(first)))
        return false;
    return first == PrCssKeyword_Unknown || !takesKeyword(kCssPropertyGrammarRoot[PrCssProperty_FontFamily], first);
}

// Returns true if `count` identifiers, starting with `first`, form a voice name. A single identifier must not be a
// gender keyword, a CSS-wide keyword, `preserve` or `default` (css-speech-1/#typedef-voice-family-voice-family-name).
bool isVoiceName(PrCssKeyword first, uint32_t count) {
    if (count != 1)
        return true;
    switch (first) {
        case PrCssKeyword_Male:
        case PrCssKeyword_Female:
        case PrCssKeyword_Neutral:
        case PrCssKeyword_Preserve:
        case PrCssKeyword_Default:
            return false;
        default:
            return !cssIsCssWideKeyword(first);
    }
}

// Returns true if a family name reads back the same when its text is written as identifiers: words that are each an
// identifier with no escape, separated by single spaces, and that form a name that `names` accepts.
bool readsAsIdents(PrStringView name, FamilyNamesFn names) {
    PrCssKeyword first = PrCssKeyword_Unknown;
    uint32_t words = 0;
    size_t start = 0;
    for (size_t i = 0; i <= name.size; ++i) {
        if (i != name.size && name.data[i] != ' ')
            continue;
        PrStringView word = stringSlice(name, start, i - start);
        if (!cssIsBareIdent(word))
            return false;
        if (words++ == 0)
            first = cssKeywordFromText(word);
        start = i + 1;
    }
    return names(first, words);
}

// Matches a <string>, or a run of identifiers that `names` accepts, and stores it as the string of their values joined
// by single spaces. The string is marked CssValueAuxFlag_FamilyIdents if it reads back the same when written as
// identifiers, unless `converted` is set. A converted name stays the string that it converts to.
bool familyName(const CssMatcher* m, FamilyNamesFn names, bool converted) {
    CssTokenStream* ts = m->ts;
    CssValuePool* pool = m->pool;
    CssValueNode name = {};
    name.kind = PrCssValueKind_String;
    if (const CssValueNode* given = givenValue(ts)) {
        if (given->kind != PrCssValueKind_String)
            return false;
        PrStringView text = cssValueText(cssTokenValue(ts->tokens, ts->pos), given);
        name.text.start = cssValuePoolAddText(pool, text);
        name.text.length = uint32_t(text.size);
        ++ts->pos;
    } else if (cssPeek(ts) == CssTokenKind_String) {
        name.text.start = cssValuePoolAddTokenText(pool, ts, ts->pos, &name.text.length);
        ++ts->pos;
    } else if (cssPeek(ts) == CssTokenKind_Ident) {
        uint32_t start = ts->pos;
        uint32_t words = 0;
        name.text.start = pool->text.count;
        for (CssTokenStream scan = *ts; cssPeek(&scan) == CssTokenKind_Ident; cssSkipWhitespace(&scan)) {
            if (words++ != 0)
                cssValuePoolAddText(pool, " ");
            uint32_t length;
            cssValuePoolAddTokenText(pool, ts, scan.pos, &length);
            ts->pos = ++scan.pos;
        }
        if (!names(cssTokenKeyword(ts->tokens, start), words)) {
            ts->pos = start;
            pool->text.count = name.text.start;
            return false;
        }
        name.text.length = pool->text.count - name.text.start;
    } else {
        return false;
    }
    if (!converted && readsAsIdents(cssValuePoolText(pool, &name), names))
        name.aux = CssValueAuxFlag_FamilyIdents;
    cssValuePoolAdd(pool, name);
    return true;
}

// Returns true if the UA supports the format that a format() argument names. A string acts as the keyword that
// css-fonts-4/#font-face-src-parsing assigns to it, and also as tech(variations) if that spec says so.
bool fontFormatSupported(const CssValuePool* pool, const CssValueNode* format) {
    if (format->kind == PrCssValueKind_Keyword)
        return cssFontFormatSupported(cssValueKeyword(format));
    PrCssKeyword keyword;
    bool variations;
    return cssFontFormatFromString(cssValuePoolText(pool, format), &keyword, &variations) &&
        cssFontFormatSupported(keyword) && (!variations || cssFontTechSupported(PrCssKeyword_Variations));
}

// Returns true if the UA supports the format and every tech of a <font-src>, whose values are the pool's values from
// `first` on (css-fonts-4/#font-face-src-parsing).
bool fontSrcSupported(const CssValuePool* pool, uint32_t first) {
    const CssValueNode* values = pool->values.data;
    bool supported = true;
    for (uint32_t i = first; i < pool->values.count; i += values[i].next) {
        if (cssValueIsFunction(&values[i], PrCssKeyword_Format))
            supported = supported && fontFormatSupported(pool, &values[i + 1]);
        if (!cssValueIsFunction(&values[i], PrCssKeyword_Tech))
            continue;
        for (uint32_t j = i + 1; j < i + values[i].next; ++j) {
            if (values[j].kind == PrCssValueKind_Keyword)
                supported = supported && cssFontTechSupported(cssValueKeyword(&values[j]));
        }
    }
    return supported;
}

// Adds a node for a match of `production` over `span`. The node's children follow it, and closeNode sets their extent
// once they are added.
uint32_t addProduction(CssValuePool* pool, CssValueDef production, CssTokenSpan span) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Production;
    value.aux = production;
    value.tokens.start = span.start;
    value.tokens.count = span.count;
    return cssValuePoolAdd(pool, value);
}

void closeNode(CssValuePool* pool, uint32_t node) {
    pool->values.data[node].next = pool->values.count - node;
}

// Matches a feature's name, a ':' and a value that its grammar accepts, running to the end of the block, for one of the
// @container features that `production` queries. The match is stored like the <mf-plain> of a media feature's syntax,
// and its <mf-value> is the whole value (mediaqueries-5/#mq-features).
bool featureDeclaration(const CssMatcher* m, CssValueDef production) {
    CssTokenStream* ts = m->ts;
    uint32_t end = cssBlockEnd(ts);
    CssTokenSpan whole = cssTrimWhitespace(ts->tokens, CssTokenSpan{ts->pos, end - ts->pos});
    CssTokenStream run = cssSubStream(ts, whole);
    if (cssPeek(&run) != CssTokenKind_Ident)
        return false;
    uint32_t name = run.pos++;
    cssSkipWhitespace(&run);
    if (cssPeek(&run) != CssTokenKind_Colon)
        return false;
    ++run.pos;
    CssTokenSpan span = cssTrimWhitespace(ts->tokens, CssTokenSpan{run.pos, run.end - run.pos});

    CssValuePool* pool = m->pool;
    uint32_t values = pool->values.count;
    uint32_t text = pool->text.count;
    uint32_t plain = addProduction(pool, CssValueDef_MfPlain, whole);
    uint32_t nameNode = addProduction(pool, CssValueDef_MfName, CssTokenSpan{name, 1});
    CssValueNode ident = {};
    ident.kind = PrCssValueKind_Ident;
    ident.text.start = cssValuePoolAddTokenText(pool, ts, name, &ident.text.length);
    PrCssDescriptor descriptor = cssDescriptorFromText(CssAtRule_Container, cssValuePoolText(pool, &ident));
    ident.aux = CssValueAuxFlags(descriptor);
    cssValuePoolAdd(pool, ident);
    closeNode(pool, nameNode);
    cssValuePoolAdd(pool, cssLiteralValue(':'));
    uint32_t valueNode = addProduction(pool, CssValueDef_MfValue, span);
    CssTokenStream value = cssSubStream(ts, span);
    CssValueSpan matched;
    if (descriptor == PrCssDescriptor_None || kCssDescriptorFeature[descriptor] != production || span.count == 0 ||
            !cssParseDescriptorValue(descriptor, &value, pool, m->match, &matched)) {
        pool->values.count = values;
        pool->text.count = text;
        return false;
    }
    closeNode(pool, valueNode);
    closeNode(pool, plain);
    ts->pos = end;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Function arguments (css-values-5/#component-function-commas)

// Braces delimit an argument that contains commas. If the whole of `span` is a {} block, narrows `span` to the contents
// of the braces, and sets `needed` to whether the braces are needed to read the value back. They are needed if the
// contents contain a comma or a brace, and the value is then serialized in braces. Returns false if `span` is not a
// block.
bool unbrace(const CssTokenStream* ts, CssTokenSpan* span, bool* needed) {
    *needed = false;
    if (span->count == 0 || cssTokenKind(ts->tokens, span->start) != CssTokenKind_OpenBrace ||
            cssTokenBlockEnd(ts->tokens, span->start) != span->start + span->count) {
        return false;
    }
    uint32_t closer = cssTokenCloser(ts->tokens, span->start);
    uint32_t last = closer < span->start + span->count ? closer : span->start + span->count;
    *span = cssTrimWhitespace(ts->tokens, CssTokenSpan{span->start + 1, last - span->start - 1});
    CssTokenStream inner = cssSubStream(ts, *span);
    CssTokenSpan component;
    while (inner.pos < inner.end && !*needed) {
        *needed = cssPeek(&inner) == CssTokenKind_Comma || cssPeek(&inner) == CssTokenKind_OpenBrace;
        if (!cssConsumeComponentValue(&inner, &component))
            break;
    }
    return true;
}

// Returns true if the match is of a function's arguments. The frames that the match continues to must reach a
// function's frame before the end of the stream. The stream's block must be that function's block, rather than a block
// that its arguments open, as `:is(` does in selector().
bool inFunctionArguments(const CssMatcher* m) {
    const CssMatchFrame* frames = m->match->frames.data;
    uint32_t frame = m->frame;
    while (frames[frame].kind != CssMatchFrameKind_Function) {
        if (frames[frame].kind == CssMatchFrameKind_End)
            return false;
        frame = frames[frame].outer;
    }
    uint32_t closer = cssTokenCloser(m->ts->tokens, frames[frame].pos - 1);
    return cssBlockEnd(m->ts) == (closer < m->ts->end ? closer : m->ts->end);
}

// Returns true if the last token before `index` that is not whitespace is a comma.
bool afterComma(const CssTokenStream* ts, uint32_t index) {
    while (index > 0 && cssTokenKind(ts->tokens, index - 1) == CssTokenKind_Whitespace)
        --index;
    return index > 0 && cssTokenKind(ts->tokens, index - 1) == CssTokenKind_Comma;
}

// Matches a free-form production as a function's argument (css-values-5/#component-function-commas): the contents of a
// {} block that is the whole argument, or a run up to the next comma that contains no {} block. `consume` must consume
// the whole run, as the production does. An empty argument after a comma matches the production as an empty sequence
// (css-values-5/#typedef-inherit-args).
bool freeFormArgument(const CssMatcher* m, bool (*consume)(CssTokenStream*, CssTokenSpan*)) {
    CssTokenStream* ts = m->ts;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, cssBlockEnd(ts) - ts->pos});
    CssTokenSpan component;
    bool block = false;
    while (run.pos < run.end && cssPeek(&run) != CssTokenKind_Comma) {
        block = block || cssPeek(&run) == CssTokenKind_OpenBrace;
        if (!cssConsumeComponentValue(&run, &component))
            return false;
    }
    CssTokenSpan span = cssTrimWhitespace(ts->tokens, CssTokenSpan{ts->pos, run.pos - ts->pos});
    if (span.count == 0 && afterComma(ts, ts->pos)) {
        cssValuePoolAddTokens(m->pool, ts, span);
        ts->pos = run.pos;
        return true;
    }
    bool braces;
    if (unbrace(ts, &span, &braces) != block)
        return false;
    CssTokenStream value = cssSubStream(ts, span);
    CssTokenSpan taken;
    if (!consume(&value, &taken) || !cssAtEnd(&value))
        return false;
    if (braces)
        cssValuePoolAdd(m->pool, cssLiteralValue('{'));
    cssValuePoolAddTokens(m->pool, ts, span);
    if (braces)
        cssValuePoolAdd(m->pool, cssLiteralValue('}'));
    ts->pos = run.pos;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries (mediaqueries-5/#mq-syntax, css-conditional-5/#at-supports-ext)

// Returns true if a query, from `first` on in the pool, has a value that is known when it is parsed: every feature that
// it names exists and has a value that the feature accepts, and it contains no <general-enclosed>. If `production` is
// not CssValueDef_Count, each feature must also be one that `production` queries.
bool queryKnown(const CssValuePool* pool, uint32_t first, CssValueDef production) {
    for (uint32_t i = first; i < pool->values.count; ++i) {
        const CssValueNode* value = &pool->values.data[i];
        if (cssValueIsProduction(value, CssValueDef_GeneralEnclosed))
            return false;
        if (!cssValueIsProduction(value, CssValueDef_MfName))
            continue;
        PrCssDescriptor feature = cssValueFeature(&pool->values.data[i + 1]);
        if (feature == PrCssDescriptor_None ||
                (production != CssValueDef_Count && kCssDescriptorFeature[feature] != production)) {
            return false;
        }
    }
    return true;
}

// Returns true if the token at `index` is an <mf-lt>, <mf-gt> or <mf-eq> delim (mediaqueries-5/#mq-syntax).
bool isComparisonDelim(const CssTokenArray* tokens, uint32_t index) {
    if (cssTokenKind(tokens, index) != CssTokenKind_Delim)
        return false;
    uint32_t delim = cssTokenDelim(tokens, index);
    return delim == '<' || delim == '>' || delim == '=';
}

// Matches a media feature's syntax, running to the end of the block, whose feature is one of the @container features
// that `production` queries (css-conditional-5/#container-features). An unknown feature does not match.
bool containerFeature(const CssMatcher* m, CssValueDef production) {
    CssTokenStream* ts = m->ts;
    uint32_t end = cssBlockEnd(ts);
    uint32_t values = m->pool->values.count;
    uint32_t text = m->pool->text.count;
    if (!cssMatchQuery(m, CssTokenSpan{ts->pos, end - ts->pos}, CssValueDef_MediaFeature, CssAtRule_Container)) {
        return false;
    }
    if (!queryKnown(m->pool, values, production)) {
        m->pool->values.count = values;
        m->pool->text.count = text;
        return false;
    }
    ts->pos = end;
    return true;
}

// Adds the media query `not all`, which mediaqueries-5/#error-handling substitutes for an entry at `span` that is not a
// media query. It is stored as a <media-query> with nothing in it.
void addNotAll(CssValuePool* pool, CssTokenSpan span) {
    CssValueNode query = {};
    query.kind = PrCssValueKind_Production;
    query.aux = CssValueDef_MediaQuery;
    query.tokens.start = span.start;
    query.tokens.count = span.count;
    cssValuePoolAdd(pool, query);
}

// Returns the end of the current entry in a comma-separated list of component values
// (css-syntax-3/#parse-comma-separated-list-of-component-values): its next comma outside a block, or the end of the
// stream.
uint32_t entryEnd(const CssTokenStream* ts) {
    CssTokenStream scan = *ts;
    CssTokenSpan ignored;
    while (cssPeek(&scan) != CssTokenKind_Comma && cssConsumeComponentValue(&scan, &ignored)) {
    }
    return scan.pos;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scoping (css-cascade-6/#scope-syntax)

// Matches the selectors that run to the end of the block, as `production`.
bool scopeSelectors(const CssMatcher* m, CssValueDef production) {
    CssTokenStream* ts = m->ts;
    uint32_t end = cssBlockEnd(ts);
    if (!cssMatchArgument(m, CssTokenSpan{ts->pos, end - ts->pos}, CssGrammarRoot_None, production,
            CssMatchArgument{CssPseudoClass_None, CssPseudoElement_None, false})) {
        return false;
    }
    ts->pos = end;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Numeric Data Types

// <number>
bool cssParseNumber(const CssMatcher* m) {
    return number(m->ts, m->pool, false) ||
        numericFunction(m, CssMathSlot{CssUnitCategory_Number, CssUnitCategory_Unknown});
}

// <integer>
bool cssParseInteger(const CssMatcher* m) {
    return number(m->ts, m->pool, true, m->match->calculate) ||
        numericFunction(m, CssMathSlot{CssUnitCategory_Number, CssUnitCategory_Unknown});
}

// <percentage>
bool cssParsePercentage(const CssMatcher* m) {
    return percentage(m->ts, m->pool) ||
        numericFunction(m, CssMathSlot{CssUnitCategory_Percent, CssUnitCategory_Percent});
}

// <zero>
bool cssParseZero(const CssMatcher* m) {
    uint32_t at = m->ts->pos;
    if (cssPeek(m->ts) != CssTokenKind_Number || cssTokenNumber(m->ts->tokens, at) != 0)
        return false;
    ++m->ts->pos;
    cssValuePoolAdd(m->pool, cssNumberValue(PrCssValueKind_Integer, PrCssUnit_Number, 0));
    return true;
}

// <dimension>. A unit that no table names is kept as the text it was written with, in an ident below the dimension.
bool cssParseDimension(const CssMatcher* m) {
    uint32_t at = m->ts->pos;
    if (const CssValueNode* given = givenValue(m->ts)) {
        if (given->kind != PrCssValueKind_Dimension)
            return false;
        takeGiven(m->ts, m->pool);
        return true;
    }
    if (cssPeek(m->ts) != CssTokenKind_Dimension)
        return false;

    ++m->ts->pos;
    PrCssUnit unit = cssTokenUnit(m->ts->tokens, at);
    uint32_t dimension = cssValuePoolAdd(m->pool, cssNumberValue(PrCssValueKind_Dimension, unit,
        cssTokenNumber(m->ts->tokens, at)));
    if (unit != PrCssUnit_Unknown)
        return true;

    CssValueNode text = {};
    text.kind = PrCssValueKind_Ident;
    text.text.start = cssValuePoolAddValueText(m->pool, cssTokenUnitText(m->ts->tokens, at), &text.text.length);
    cssValuePoolAdd(m->pool, text);
    m->pool->values.data[dimension].next = 2;
    return true;
}

// <number-token>
bool cssParseNumberToken(const CssMatcher* m) {
    return number(m->ts, m->pool, false);
}

bool cssParseNumberLiteral(const CssMatcher* m, CssGrammarNumber literal) {
    uint32_t at = m->ts->pos;
    uint32_t values = m->pool->values.count;
    bool matched;
    if (literal.unit == PrCssUnit_Number)
        matched = number(m->ts, m->pool, false);
    else
        matched = dimension(m->ts, m->pool, cssUnitCategory(literal.unit));
    if (matched) {
        // A unit with a fixed ratio to its category's canonical unit compares by its value in the canonical unit. Any
        // other unit matches only the same unit.
        const CssValueNode& value = m->pool->values.data[values];
        double scale = literal.unit == PrCssUnit_Number ? 1 : cssUnitScale(value.unit);
        double literalScale = literal.unit == PrCssUnit_Number ? 1 : cssUnitScale(literal.unit);
        double canonical = literal.value * literalScale;
        bool same;
        if (scale != 0 && literalScale != 0)
            same = absF64(value.number * scale - canonical) <= 1e-9 * absF64(canonical);
        else
            same = value.unit == literal.unit && value.number == literal.value;
        if (same)
            return true;
    }
    m->ts->pos = at;
    m->pool->values.count = values;
    return false;
}

// <dimension-token>
bool cssParseDimensionToken(const CssMatcher* m) {
    return cssParseDimension(m);
}

// <percentage-token>
bool cssParsePercentageToken(const CssMatcher* m) {
    return percentage(m->ts, m->pool);
}

// <numeric-function>: a function that a calculation accepts as an operand (css-values-4/#calc-func). An enclosing math
// function stores it as it matched.
bool cssParseNumericFunction(const CssMatcher* m) {
    return matchNumericFunction(m);
}

// <length>
bool cssParseLength(const CssMatcher* m) {
    return length(m->ts, m->pool) || numericFunction(m, CssMathSlot{CssUnitCategory_Length, CssUnitCategory_Unknown});
}

// <length-percentage> (css-values-4/#mixed-percentages)
bool cssParseLengthPercentage(const CssMatcher* m) {
    return length(m->ts, m->pool) || percentage(m->ts, m->pool) ||
        numericFunction(m, CssMathSlot{CssUnitCategory_Length, CssUnitCategory_Length});
}

// <angle>
bool cssParseAngle(const CssMatcher* m) {
    return dimensionOf(m, CssUnitCategory_Angle);
}

// <angle-percentage>
bool cssParseAnglePercentage(const CssMatcher* m) {
    return dimensionPercentage(m, CssUnitCategory_Angle);
}

// <time>
bool cssParseTime(const CssMatcher* m) {
    return dimensionOf(m, CssUnitCategory_Time);
}

// <frequency>
bool cssParseFrequency(const CssMatcher* m) {
    return dimensionOf(m, CssUnitCategory_Frequency);
}

// <resolution>
bool cssParseResolution(const CssMatcher* m) {
    return dimensionOf(m, CssUnitCategory_Resolution);
}

// <flex>
bool cssParseFlex(const CssMatcher* m) {
    return dimensionOf(m, CssUnitCategory_Flex);
}

bool cssParseTypedNumeric(const CssMatcher* m) {
    if (number(m->ts, m->pool, false) || percentage(m->ts, m->pool))
        return true;
    if (cssPeek(m->ts) != CssTokenKind_Dimension)
        return numericFunction(m, CssMathSlot{CssUnitCategory_Unknown, CssUnitCategory_Percent});
    CssUnitCategory category = cssUnitCategory(cssTokenUnit(m->ts->tokens, m->ts->pos));
    switch (category) {
        case CssUnitCategory_Length:
        case CssUnitCategory_Angle:
        case CssUnitCategory_Time:
        case CssUnitCategory_Frequency:
        case CssUnitCategory_Resolution:
        case CssUnitCategory_Flex:
            return dimension(m->ts, m->pool, category);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Speech Units

// <decibel>
bool cssParseDecibel(const CssMatcher* m) {
    return dimension(m->ts, m->pool, CssUnitCategory_Decibel);
}

// <semitones>
bool cssParseSemitones(const CssMatcher* m) {
    return dimension(m->ts, m->pool, CssUnitCategory_Semitones);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textual Data Types

// <ident>
bool cssParseIdent(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_Ident, PrCssValueKind_Ident);
}

// <custom-ident>
bool cssParseCustomIdent(const CssMatcher* m) {
    if (isReservedIdent(m->ts, m->ts->pos) || isExcludedIdent(m))
        return false;
    return textToken(m->ts, m->pool, CssTokenKind_Ident, PrCssValueKind_Ident);
}

// <dashed-ident>
bool cssParseDashedIdent(const CssMatcher* m) {
    if (!cssTokenIsDashedIdent(m->ts->tokens, m->ts->pos))
        return false;
    return textToken(m->ts, m->pool, CssTokenKind_Ident, PrCssValueKind_Ident);
}

// <string>
bool cssParseString(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_String, PrCssValueKind_String);
}

// <path-data>: a string of SVG path data (SVG2/paths.html#PathDataBNF), held as its commands up to the first error
// (SVG2/paths.html#PathDataErrorHandling), as css_path.h describes.
bool cssParsePathData(const CssMatcher* m) {
    Array<char> text;
    PrStringView data{};
    if (const CssValueNode* given = givenValue(m->ts)) {
        if (given->kind != PrCssValueKind_String)
            return false;
        data = cssValueText(cssTokenValue(m->ts->tokens, m->ts->pos), given);
    } else if (cssPeek(m->ts) == CssTokenKind_String) {
        PrStringView token = cssTokenValueText(m->ts->tokens, m->ts->pos);
        text.resize(cssValue(token, nullptr, 0));
        data = PrStringView{text.data, cssValue(token, text.data, text.count)};
    } else {
        return false;
    }
    Array<PrPathDataCommand> commands;
    commands.resize(prPathDataParse(data, nullptr, 0));
    prPathDataParse(data, commands.data, commands.count);
    cssValuePoolAddPathData(m->pool, commands.data, commands.count);
    ++m->ts->pos;
    commands.reset();
    text.reset();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Named Identifiers

// <counter-name>: a <custom-ident> other than `none` (css-lists-3/#typedef-counter-name). It names a counter, either an
// author's or one CSS defines, such as list-item.
bool cssParseCounterName(const CssMatcher* m) {
    if (cssPeek(m->ts) == CssTokenKind_Ident && cssTokenKeyword(m->ts->tokens, m->ts->pos) == PrCssKeyword_None)
        return false;
    return cssParseCustomIdent(m);
}

// <counter-style-name>: a <custom-ident> other than `none` (css-counter-styles-3/#typedef-counter-style-name), matched
// in the same way as a <counter-name>.
bool cssParseCounterStyleName(const CssMatcher* m) {
    return cssParseCounterName(m);
}

// <voice-family-name>: a <string>, or a run of identifiers stored as the string of their values joined by single spaces
// (css-speech-1/#typedef-voice-family-voice-family-name).
bool cssParseVoiceFamilyName(const CssMatcher* m) {
    return familyName(m, isVoiceName, false);
}

// <size-keyword>: a sizing keyword of the property that contains calc-size(), which is a keyword that the property
// accepts other than `none` (css-values-5/#typedef-calc-size-size-keyword).
bool cssParseSizeKeyword(const CssMatcher* m) {
    if (m->property == PrCssProperty_Count)
        return false;
    CssGrammarIndex root = kCssPropertyGrammarRoot[m->property];
    return keywordOf(m, [root](PrCssKeyword keyword) {
        return keyword != PrCssKeyword_None && takesKeyword(root, keyword);
    });
}

// <id>: an ID selector (css-ui-4/#typedef-id), which is a <hash-token> whose value is an identifier
// (selectors-4/#id-selectors), stored as an <id-selector>.
bool cssParseId(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    if (cssPeek(ts) != CssTokenKind_Hash || (cssTokenFlags(ts->tokens, ts->pos) & CssTokenFlag_Id) == 0)
        return false;
    uint32_t selector = addProduction(m->pool, CssValueDef_IdSelector, CssTokenSpan{ts->pos, 1});
    textToken(ts, m->pool, CssTokenKind_Hash, PrCssValueKind_Ident);
    closeNode(m->pool, selector);
    return true;
}

// <view-transition-type>: a <custom-ident> that is not `none` and does not start with `-ua-`
// (css-view-transitions-2/#typedef-view-transition-type).
bool cssParseViewTransitionType(const CssMatcher* m) {
    if (cssPeek(m->ts) == CssTokenKind_Ident && cssTokenKeyword(m->ts->tokens, m->ts->pos) == PrCssKeyword_None)
        return false;
    uint32_t text = m->pool->text.count;
    if (!cssParseCustomIdent(m))
        return false;
    CssValueNode* type = &m->pool->values.last();
    if (!stringStartsWith(cssValuePoolText(m->pool, type), "-ua-"))
        return true;
    m->pool->values.pop();
    m->pool->text.count = text;
    --m->ts->pos;
    return false;
}

// <outline-line-style>: a <line-style> other than `hidden` (css-ui-4/#typedef-outline-line-style).
bool cssParseOutlineLineStyle(const CssMatcher* m) {
    return keywordOf(m, [](PrCssKeyword keyword) {
        return keyword != PrCssKeyword_Hidden && takesKeyword(kCssValueDefGrammarRoot[CssValueDef_LineStyle], keyword);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Fonts

// <font-family-name>: a <string>, or a run of identifiers stored as the string of their values joined by single spaces
// (css-fonts-4/#font-family-name-syntax). The argument of local() is kept as the string that it converts to
// (css-fonts-4/#local-font-fallback).
bool cssParseFontFamilyName(const CssMatcher* m) {
    const CssMatchFrame& outer = m->match->frames.data[m->frame];
    bool local = outer.kind == CssMatchFrameKind_Function &&
        cssValueIsFunction(&m->pool->values.data[outer.anchor], PrCssKeyword_Local);
    return familyName(m, isFontFamilyName, local);
}

// <font-src-list>: the <font-src>s of a comma-separated list, excluding each one that does not parse or that names a
// format or tech that the UA does not support. A list with none left is invalid (css-fonts-4/#font-face-src-parsing).
bool cssParseFontSrcList(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    CssValuePool* pool = m->pool;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, cssBlockEnd(ts) - ts->pos});
    uint32_t values = pool->values.count;
    for (;;) {
        uint32_t comma = run.end;
        bool more = cssNextComma(&run, &comma);
        CssTokenSpan entry = cssTrimWhitespace(ts->tokens, CssTokenSpan{run.pos, comma - run.pos});
        uint32_t item = pool->values.count;
        uint32_t text = pool->text.count;
        if (item != values)
            cssValuePoolAdd(pool, cssLiteralValue(','));
        uint32_t first = pool->values.count;
        if (!cssMatchArgument(m, entry, kCssValueDefGrammarRoot[CssValueDef_FontSrc], CssValueDef_Count,
                CssMatchArgument{CssPseudoClass_None, CssPseudoElement_None, false}) || !fontSrcSupported(pool, first)) {
            pool->values.count = item;
            pool->text.count = text;
        }
        if (!more)
            break;
        run.pos = comma + 1;
    }
    if (pool->values.count == values)
        return false;
    ts->pos = run.end;
    return true;
}

// <opentype-tag>: a <string> of four characters, each in U+20-7E (css-fonts-4/#typedef-opentype-tag).
bool cssParseOpentypeTag(const CssMatcher* m) {
    char tag[4];
    uint32_t size = 0;
    if (const CssValueNode* given = givenValue(m->ts)) {
        if (given->kind != PrCssValueKind_String)
            return false;
        PrStringView text = cssValueText(cssTokenValue(m->ts->tokens, m->ts->pos), given);
        size = uint32_t(text.size);
        if (size == sizeof(tag))
            memCopy(tag, text.data, sizeof(tag));
    } else if (cssPeek(m->ts) == CssTokenKind_String) {
        size = cssValue(cssTokenValueText(m->ts->tokens, m->ts->pos), tag, uint32_t(sizeof(tag)));
    } else {
        return false;
    }
    bool printable = size == sizeof(tag);
    for (uint32_t i = 0; i < sizeof(tag) && printable; ++i)
        printable = asciiIsPrintable(tag[i]);
    return printable && textToken(m->ts, m->pool, CssTokenKind_String, PrCssValueKind_String);
}

// ---------------------------------------------------------------------------------------------------------------------
// Color

// <hex-color>
bool cssParseHexColor(const CssMatcher* m) {
    if (cssPeek(m->ts) != CssTokenKind_Hash)
        return false;

    char digits[8];
    uint32_t count = cssValue(cssTokenValueText(m->ts->tokens, m->ts->pos), digits, uint32_t(sizeof(digits)));
    if (count != 3 && count != 4 && count != 6 && count != 8)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!asciiIsHexDigit(digits[i]))
            return false;
    }
    ++m->ts->pos;

    // #rgb and #rgba repeat each digit. #rrggbb and #rrggbbaa read them in pairs.
    PrColor8 color = {0, 0, 0, 0xff};
    uint8_t* channels[] = {&color.r, &color.g, &color.b, &color.a};
    uint32_t width = count <= 4 ? 1u : 2u;
    for (uint32_t i = 0; i < count / width; ++i) {
        uint32_t byte = asciiHexValue(digits[i * width]);
        byte = width == 1 ? byte * 0x11 : byte * 16 + asciiHexValue(digits[i * width + 1]);
        *channels[i] = uint8_t(byte);
    }

    cssValuePoolAddColor(m->pool, color);
    return true;
}

// <named-color>, which is kept as the keyword, because css-color-4/#resolving-sRGB-values serializes its specified
// value as the keyword.
bool cssParseNamedColor(const CssMatcher* m) {
    PrColor8 color;
    if (cssPeek(m->ts) != CssTokenKind_Ident)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(m->ts->tokens, m->ts->pos);
    if (!cssNamedColor(keyword, &color))
        return false;
    ++m->ts->pos;
    cssValuePoolAdd(m->pool, cssKeywordValue(keyword));
    return true;
}

// <system-color>
bool cssParseSystemColor(const CssMatcher* m) {
    return keywordOf(m, cssIsSystemColor);
}

// ---------------------------------------------------------------------------------------------------------------------
// Token Productions

// <ident-token>
bool cssParseIdentToken(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_Ident, PrCssValueKind_Ident);
}

// <url-token>
bool cssParseUrlToken(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_Url, PrCssValueKind_Url);
}

// <unicode-range-token>. css-fonts-4/#unicode-range-desc: a bound above the maximum code point, or an end before the
// start, is not a valid range.
bool cssParseUnicodeRangeToken(const CssMatcher* m) {
    if (const CssValueNode* given = givenValue(m->ts)) {
        if (given->kind != PrCssValueKind_UnicodeRange || given->range.last > kPrUtf8MaxCodePoint ||
                given->range.first > given->range.last)
            return false;
        takeGiven(m->ts, m->pool);
        return true;
    }
    if (cssPeek(m->ts) != CssTokenKind_UnicodeRange)
        return false;
    uint32_t first = 0;
    uint32_t last = 0;
    cssTokenRange(m->ts->tokens, m->ts->pos, &first, &last);
    if (last > kPrUtf8MaxCodePoint || first > last)
        return false;

    CssValueNode value = {};
    value.kind = PrCssValueKind_UnicodeRange;
    value.range.first = first;
    value.range.last = last;
    ++m->ts->pos;
    cssValuePoolAdd(m->pool, value);
    return true;
}

// <hash-token>, as the name after its '#'.
bool cssParseHashToken(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_Hash, PrCssValueKind_Ident);
}

// <string-token>
bool cssParseStringToken(const CssMatcher* m) {
    return textToken(m->ts, m->pool, CssTokenKind_String, PrCssValueKind_String);
}

// <function-token>, as the name before its '(' and a '(' literal.
bool cssParseFunctionToken(const CssMatcher* m) {
    if (!textToken(m->ts, m->pool, CssTokenKind_Function, PrCssValueKind_Ident))
        return false;
    cssValuePoolAdd(m->pool, cssLiteralValue('('));
    return true;
}

// <at-keyword-token>, as a '@' literal and the name after it.
bool cssParseAtKeywordToken(const CssMatcher* m) {
    if (cssPeek(m->ts) != CssTokenKind_AtKeyword)
        return false;
    cssValuePoolAdd(m->pool, cssLiteralValue('@'));
    return textToken(m->ts, m->pool, CssTokenKind_AtKeyword, PrCssValueKind_Ident);
}

// ---------------------------------------------------------------------------------------------------------------------
// Token Runs

// <declaration-value>. As a function's argument, it is free-form.
bool cssParseDeclarationValue(const CssMatcher* m) {
    if (inFunctionArguments(m))
        return freeFormArgument(m, cssConsumeDeclarationValue);
    CssTokenSpan span;
    if (!cssConsumeDeclarationValue(m->ts, &span))
        return false;

    cssValuePoolAddTokens(m->pool, m->ts, span);
    return true;
}

// <any-value> (css-syntax-3/#typedef-any-value). As a function's argument, it is free-form.
bool cssParseAnyValue(const CssMatcher* m) {
    if (inFunctionArguments(m))
        return freeFormArgument(m, cssConsumeAnyValue);
    CssTokenSpan span;
    if (!cssConsumeAnyValue(m->ts, &span))
        return false;

    cssValuePoolAddTokens(m->pool, m->ts, span);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// An+B

// <a-n-plus-b>, as its node over its two coefficients (css-syntax-3/#anb-microsyntax).
bool cssParseANPlusB(const CssMatcher* m) {
    uint32_t start = m->ts->pos;
    int32_t a = 0;
    int32_t b = 0;
    if (!cssConsumeAnPlusB(m->ts, &a, &b))
        return false;
    CssValueNode node = {};
    node.kind = PrCssValueKind_Production;
    node.aux = CssValueDef_ANPlusB;
    node.tokens.start = start;
    node.tokens.count = m->ts->pos - start;
    uint32_t at = cssValuePoolAdd(m->pool, node);
    cssValuePoolAdd(m->pool, cssNumberValue(PrCssValueKind_Integer, PrCssUnit_Number, a));
    cssValuePoolAdd(m->pool, cssNumberValue(PrCssValueKind_Integer, PrCssUnit_Number, b));
    m->pool->values.data[at].next = 3;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rules

// <declaration>: what css-syntax-3/#consume-declaration consumes from the rest of the block: a name, a ':' and a value,
// with !important if written (css-conditional-5/#at-supports-ext). The value is parsed as the property's value if the
// name is a property that accepts it, and as tokens otherwise. The name's ident is marked CssValueAuxFlag_Supported if
// a style rule accepts the declaration: a property with a value that it accepts, or a custom property
// (css-conditional-3/#dfn-support).
bool cssParseDeclaration(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, cssBlockEnd(ts) - ts->pos});
    CssSyntaxDeclaration declaration;
    if (!cssParseDeclaration(&run, &declaration) || !cssAtEnd(&run))
        return false;

    CssValuePool* pool = m->pool;
    CssValueNode name = {};
    name.kind = PrCssValueKind_Ident;
    name.text.start = cssValuePoolAddTokenText(pool, ts, declaration.name, &name.text.length);
    PrCssProperty property = cssPropertyFromText(cssValuePoolText(pool, &name));
    uint32_t nameAt = cssValuePoolAdd(pool, name);
    cssValuePoolAdd(pool, cssLiteralValue(':'));

    CssTokenStream value = cssSubStream(ts, declaration.value);
    CssValueSpan matched;
    bool parsed = property != PrCssProperty_Count && cssParsePropertyValue(property, &value, pool, m->match, &matched);
    if (!parsed)
        cssValuePoolAddTokens(pool, ts, declaration.value);
    if (parsed || (property == PrCssProperty_Count && cssTokenIsDashedIdent(ts->tokens, declaration.name)))
        pool->values.data[nameAt].aux = CssValueAuxFlag_Supported;
    if (declaration.important) {
        cssValuePoolAdd(pool, cssLiteralValue('!'));
        cssValuePoolAdd(pool, cssKeywordValue(PrCssKeyword_Important));
    }
    ts->pos = run.end;
    return true;
}

// <dashed-function>
bool cssParseDashedFunction(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    if (cssPeek(ts) != CssTokenKind_Function)
        return false;
    char prefix[2];
    uint32_t length = cssValue(cssTokenValueText(ts->tokens, ts->pos), prefix, uint32_t(sizeof(prefix)));
    if (length <= 2 || prefix[0] != '-' || prefix[1] != '-')
        return false;

    uint32_t end = cssTokenBlockEnd(ts->tokens, ts->pos);
    uint32_t close = cssTokenCloser(ts->tokens, ts->pos);
    if (end > ts->end)
        end = ts->end;
    if (close > ts->end)
        close = ts->end;
    CssTokenStream args = cssSubStream(ts, CssTokenSpan{ts->pos + 1, close - ts->pos - 1});
    cssParseFunctionToken(m);
    cssSkipWhitespace(&args);
    while (args.pos < args.end) {
        uint32_t first = args.pos;
        CssTokenSpan component;
        while (args.pos < args.end && cssPeek(&args) != CssTokenKind_Comma) {
            if (!cssConsumeComponentValue(&args, &component))
                return false;
        }
        CssTokenSpan span = cssTrimWhitespace(ts->tokens, CssTokenSpan{first, args.pos - first});
        CssTokenStream value = cssSubStream(ts, span);
        CssTokenSpan valid;
        if (span.count != 0 && (!cssConsumeDeclarationValue(&value, &valid) || !cssAtEnd(&value)))
            return false;
        bool braces;
        unbrace(ts, &span, &braces);
        if (braces)
            cssValuePoolAdd(m->pool, cssLiteralValue('{'));
        cssValuePoolAddTokens(m->pool, ts, span);
        if (braces)
            cssValuePoolAdd(m->pool, cssLiteralValue('}'));
        if (args.pos == args.end)
            break;
        ++args.pos;
        cssValuePoolAdd(m->pool, cssLiteralValue(','));
        cssSkipWhitespace(&args);
        if (args.pos == args.end)
            cssValuePoolAddTokens(m->pool, ts, CssTokenSpan{args.pos, 0});
    }
    cssValuePoolAdd(m->pool, cssLiteralValue(')'));
    ts->pos = end;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries

// <media-query-list>: the entries of the rest of the stream, split at its commas, each a <media-query>
// (mediaqueries-5/#mq-syntax). An entry that is not a media query becomes `not all` (mediaqueries-5/#error-handling).
// An entry whose value is unknown is kept as written and evaluates to false, as csswg-drafts#7595 resolved. A stream of
// only whitespace is the empty list.
bool cssParseMediaQueryList(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, ts->end - ts->pos});
    if (cssTrimWhitespace(ts->tokens, cssStreamAll(&run)).count == 0) {
        ts->pos = run.end;
        return true;
    }

    CssValuePool* pool = m->pool;
    for (;;) {
        uint32_t comma = entryEnd(&run);
        bool more = comma != run.end;
        CssTokenSpan entry = cssTrimWhitespace(ts->tokens, CssTokenSpan{run.pos, comma - run.pos});
        uint32_t values = pool->values.count;
        uint32_t text = pool->text.count;
        if (!cssMatchQuery(m, entry, CssValueDef_MediaQuery, CssAtRule_Media)) {
            pool->values.count = values;
            pool->text.count = text;
            addNotAll(pool, entry);
        }
        run.pos = comma;
        if (!more)
            break;
        ++run.pos;
        cssValuePoolAdd(pool, cssLiteralValue(','));
    }
    ts->pos = run.end;
    return true;
}

// <size-feature>: a media feature's syntax whose feature is a size feature (css-conditional-5/#size-container).
bool cssParseSizeFeature(const CssMatcher* m) {
    return containerFeature(m, CssValueDef_SizeFeature);
}

// <scroll-state-feature>: a media feature's syntax whose feature is a scroll-state feature
// (css-conditional-5/#typedef-scroll-state-feature).
bool cssParseScrollStateFeature(const CssMatcher* m) {
    return containerFeature(m, CssValueDef_ScrollStateFeature);
}

// <anchored-feature>: a media feature's syntax whose feature is an anchored feature. Alternatively, such a feature's
// name, a ':' and a value of more than one token, which an <mf-value> cannot hold but the feature's grammar accepts, as
// in `fallback: --a flip-block` (css-anchor-position-2/#anchored-container).
bool cssParseAnchoredFeature(const CssMatcher* m) {
    return containerFeature(m, CssValueDef_AnchoredFeature) || featureDeclaration(m, CssValueDef_AnchoredFeature);
}

// <style-feature-name>: a property that the UA supports, or a <custom-property-name>
// (css-conditional-5/#typedef-style-feature-name).
bool cssParseStyleFeatureName(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    if (cssPeek(ts) != CssTokenKind_Ident)
        return false;
    uint32_t text = m->pool->text.count;
    if (!textToken(ts, m->pool, CssTokenKind_Ident, PrCssValueKind_Ident))
        return false;
    const CssValueNode* name = &m->pool->values.last();
    if (cssTokenIsDashedIdent(ts->tokens, ts->pos - 1) ||
            cssPropertyFromText(cssValuePoolText(m->pool, name)) != PrCssProperty_Count) {
        return true;
    }
    m->pool->values.pop();
    m->pool->text.count = text;
    --ts->pos;
    return false;
}

// <style-feature-value>: a <declaration-value> that contains no <mf-lt>, <mf-gt> or <mf-eq>
// (css-conditional-5/#typedef-style-feature-value). It runs to the first of those in the block, or to the block's end.
bool cssParseStyleFeatureValue(const CssMatcher* m) {
    CssTokenStream* ts = m->ts;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, cssBlockEnd(ts) - ts->pos});
    CssTokenSpan component;
    while (run.pos < run.end && !isComparisonDelim(ts->tokens, run.pos))
        cssConsumeComponentValue(&run, &component);
    CssTokenSpan span = cssTrimWhitespace(ts->tokens, CssTokenSpan{ts->pos, run.pos - ts->pos});
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        if (isComparisonDelim(ts->tokens, i))
            return false;
    }
    CssTokenStream value = cssSubStream(ts, span);
    CssTokenSpan valid;
    if (span.count == 0 || !cssConsumeDeclarationValue(&value, &valid) || !cssAtEnd(&value))
        return false;
    cssValuePoolAddTokens(m->pool, ts, span);
    ts->pos = span.start + span.count;
    return true;
}

// <whole-value>: a value of the whole property that the match is for (css-values-5/#whole-value). It runs to the next
// comma of the enclosing function, or to its end, and may be written in {}.
bool cssParseWholeValue(const CssMatcher* m) {
    if (m->property == PrCssProperty_Count)
        return false;
    CssTokenStream* ts = m->ts;
    CssTokenStream run = cssSubStream(ts, CssTokenSpan{ts->pos, cssBlockEnd(ts) - ts->pos});
    CssTokenSpan component;
    while (run.pos < run.end && cssPeek(&run) != CssTokenKind_Comma) {
        if (!cssConsumeComponentValue(&run, &component))
            return false;
    }
    uint32_t comma = run.pos;
    CssTokenSpan span = cssTrimWhitespace(ts->tokens, CssTokenSpan{ts->pos, comma - ts->pos});
    bool braces;
    unbrace(ts, &span, &braces);

    CssValuePool* pool = m->pool;
    uint32_t values = pool->values.count;
    uint32_t text = pool->text.count;
    if (braces)
        cssValuePoolAdd(pool, cssLiteralValue('{'));
    CssTokenStream value = cssSubStream(ts, span);
    CssValueSpan matched;
    if (span.count == 0 || !cssParsePropertyValue(m->property, &value, pool, m->match, &matched)) {
        pool->values.count = values;
        pool->text.count = text;
        return false;
    }
    if (braces)
        cssValuePoolAdd(pool, cssLiteralValue('}'));
    ts->pos = comma;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scoping

// <scope-start>: a <selector-list> with no pseudo-element (css-cascade-6/#scope-syntax).
bool cssParseScopeStart(const CssMatcher* m) {
    return scopeSelectors(m, CssValueDef_ComplexRealSelectorList);
}

// <scope-end>: a <selector-list> with no pseudo-element. Its :scope and & behave as in a scoped style rule, which
// accepts a <relative-selector-list> (css-cascade-6/#scope-syntax).
bool cssParseScopeEnd(const CssMatcher* m) {
    return scopeSelectors(m, CssValueDef_RelativeRealSelectorList);
}

// ---------------------------------------------------------------------------------------------------------------------
// Selector Arguments

// <level>: a <number-token> with its type flag set to "integer".
bool cssParseLevel(const CssMatcher* m) {
    return number(m->ts, m->pool, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Numeric Functions

// css-transforms-1/#transform-functions: each function has its own generated grammar.
bool cssParseTransformFunction(const CssMatcher* m) {
    if (cssPeek(m->ts) != CssTokenKind_Function)
        return false;
    CssValueDef production;
    switch (cssTokenKeyword(m->ts->tokens, m->ts->pos)) {
        case PrCssKeyword_Matrix: production = CssValueDef_MatrixFn; break;
        case PrCssKeyword_Matrix3d: production = CssValueDef_Matrix3dFn; break;
        case PrCssKeyword_Perspective: production = CssValueDef_PerspectiveFn; break;
        case PrCssKeyword_Rotate: production = CssValueDef_RotateFn; break;
        case PrCssKeyword_Rotate3d: production = CssValueDef_Rotate3dFn; break;
        case PrCssKeyword_Rotatex: production = CssValueDef_RotateXFn; break;
        case PrCssKeyword_Rotatey: production = CssValueDef_RotateYFn; break;
        case PrCssKeyword_Rotatez: production = CssValueDef_RotateZFn; break;
        case PrCssKeyword_Scale: production = CssValueDef_ScaleFn; break;
        case PrCssKeyword_Scale3d: production = CssValueDef_Scale3dFn; break;
        case PrCssKeyword_Scalex: production = CssValueDef_ScaleXFn; break;
        case PrCssKeyword_Scaley: production = CssValueDef_ScaleYFn; break;
        case PrCssKeyword_Scalez: production = CssValueDef_ScaleZFn; break;
        case PrCssKeyword_Skew: production = CssValueDef_SkewFn; break;
        case PrCssKeyword_Skewx: production = CssValueDef_SkewXFn; break;
        case PrCssKeyword_Skewy: production = CssValueDef_SkewYFn; break;
        case PrCssKeyword_Translate: production = CssValueDef_TranslateFn; break;
        case PrCssKeyword_Translate3d: production = CssValueDef_Translate3dFn; break;
        case PrCssKeyword_Translatex: production = CssValueDef_TranslateXFn; break;
        case PrCssKeyword_Translatey: production = CssValueDef_TranslateYFn; break;
        case PrCssKeyword_Translatez: production = CssValueDef_TranslateZFn; break;
        default: return false;
    }
    CssTokenStream scan = *m->ts;
    CssTokenSpan span;
    cssConsumeComponentValue(&scan, &span);
    CssTokenStream function = cssSubStream(m->ts, span);
    CssMatcher nested = cssMatchNested(m, &function);
    if (!cssMatchWhole(&nested, kCssValueDefGrammarRoot[production], CssValueDef_Count, false))
        return false;
    m->ts->pos = scan.pos;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Color Functions

// <color-component-keyword>: in a relative color, one of the origin color's components in the color model of the
// function, or its alpha (css-color-5/#relative-colors).
bool cssParseColorComponentKeyword(const CssMatcher* m) {
    if (cssPeek(m->ts) != CssTokenKind_Ident)
        return false;
    const PrCssKeyword* components = cssMatchRelativeComponents(m);
    if (components == nullptr)
        return false;

    PrCssKeyword keyword = cssTokenKeyword(m->ts->tokens, m->ts->pos);
    bool named = keyword == PrCssKeyword_Alpha;
    for (uint32_t i = 0; i < 3; ++i)
        named = named || components[i] == keyword;
    if (!named)
        return false;
    ++m->ts->pos;
    cssValuePoolAdd(m->pool, cssKeywordValue(keyword));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Dispatch

// Dispatches to the parser of a terminal at a leaf of the grammar.
bool cssParseTerminal(const CssMatcher* m, CssTerminal terminal) {
    switch (terminal) {

        // Numeric Data Types
        case CssTerminal_Number:                      return cssParseNumber(m);
        case CssTerminal_Integer:                     return cssParseInteger(m);
        case CssTerminal_Percentage:                  return cssParsePercentage(m);
        case CssTerminal_Zero:                        return cssParseZero(m);
        case CssTerminal_Dimension:                   return cssParseDimension(m);
        case CssTerminal_NumberToken:                 return cssParseNumberToken(m);
        case CssTerminal_DimensionToken:              return cssParseDimensionToken(m);
        case CssTerminal_PercentageToken:             return cssParsePercentageToken(m);
        case CssTerminal_NumericFunction:             return cssParseNumericFunction(m);
        case CssTerminal_Length:                      return cssParseLength(m);
        case CssTerminal_LengthPercentage:            return cssParseLengthPercentage(m);
        case CssTerminal_Angle:                       return cssParseAngle(m);
        case CssTerminal_AnglePercentage:             return cssParseAnglePercentage(m);
        case CssTerminal_Time:                        return cssParseTime(m);
        case CssTerminal_Frequency:                   return cssParseFrequency(m);
        case CssTerminal_Resolution:                  return cssParseResolution(m);
        case CssTerminal_Flex:                        return cssParseFlex(m);

        // Speech Units
        case CssTerminal_Decibel:                     return cssParseDecibel(m);
        case CssTerminal_Semitones:                   return cssParseSemitones(m);

        // Textual Data Types
        case CssTerminal_Ident:                       return cssParseIdent(m);
        case CssTerminal_CustomIdent:                 return cssParseCustomIdent(m);
        case CssTerminal_DashedIdent:                 return cssParseDashedIdent(m);
        case CssTerminal_String:                      return cssParseString(m);
        case CssTerminal_PathData:                    return cssParsePathData(m);

        // Named Identifiers
        case CssTerminal_CounterName:                 return cssParseCounterName(m);
        case CssTerminal_CounterStyleName:            return cssParseCounterStyleName(m);
        case CssTerminal_Id:                          return cssParseId(m);
        case CssTerminal_VoiceFamilyName:             return cssParseVoiceFamilyName(m);
        case CssTerminal_SizeKeyword:                 return cssParseSizeKeyword(m);
        case CssTerminal_ViewTransitionType:          return cssParseViewTransitionType(m);
        case CssTerminal_OutlineLineStyle:            return cssParseOutlineLineStyle(m);

        // Fonts
        case CssTerminal_FontFamilyName:              return cssParseFontFamilyName(m);
        case CssTerminal_FontSrcList:                 return cssParseFontSrcList(m);
        case CssTerminal_OpentypeTag:                 return cssParseOpentypeTag(m);

        // Color
        case CssTerminal_HexColor:                    return cssParseHexColor(m);
        case CssTerminal_NamedColor:                  return cssParseNamedColor(m);
        case CssTerminal_SystemColor:                 return cssParseSystemColor(m);

        // Token Productions
        case CssTerminal_IdentToken:                  return cssParseIdentToken(m);
        case CssTerminal_UrlToken:                    return cssParseUrlToken(m);
        case CssTerminal_UnicodeRangeToken:           return cssParseUnicodeRangeToken(m);
        case CssTerminal_HashToken:                   return cssParseHashToken(m);
        case CssTerminal_StringToken:                 return cssParseStringToken(m);
        case CssTerminal_FunctionToken:               return cssParseFunctionToken(m);
        case CssTerminal_AtKeywordToken:              return cssParseAtKeywordToken(m);

        // Token Runs
        case CssTerminal_DeclarationValue:            return cssParseDeclarationValue(m);
        case CssTerminal_AnyValue:                    return cssParseAnyValue(m);

        // An+B
        case CssTerminal_ANPlusB:                     return cssParseANPlusB(m);

        // Rules
        case CssTerminal_Declaration:                 return cssParseDeclaration(m);
        case CssTerminal_DashedFunction:              return cssParseDashedFunction(m);

        // Queries
        case CssTerminal_MediaQueryList:              return cssParseMediaQueryList(m);
        case CssTerminal_SizeFeature:                 return cssParseSizeFeature(m);
        case CssTerminal_ScrollStateFeature:          return cssParseScrollStateFeature(m);
        case CssTerminal_AnchoredFeature:             return cssParseAnchoredFeature(m);
        case CssTerminal_StyleFeatureName:            return cssParseStyleFeatureName(m);
        case CssTerminal_StyleFeatureValue:           return cssParseStyleFeatureValue(m);
        case CssTerminal_WholeValue:                  return cssParseWholeValue(m);

        // Scoping
        case CssTerminal_ScopeStart:                  return cssParseScopeStart(m);
        case CssTerminal_ScopeEnd:                    return cssParseScopeEnd(m);

        // Selector Arguments
        case CssTerminal_Level:                       return cssParseLevel(m);

        // Numeric Functions
        case CssTerminal_TransformFunction:           return cssParseTransformFunction(m);

        // Color Functions
        case CssTerminal_ColorComponentKeyword:       return cssParseColorComponentKeyword(m);

        default:
            return false;
    }
}

} // namespace pr_impl
