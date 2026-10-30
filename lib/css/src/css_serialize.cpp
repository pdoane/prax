#include "css/src/css_serialize.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/color.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "css/src/css_color_value.h"
#include "css/src/css_keyword.h"
#include "css/src/css_path.h"
#include "css/src/css_selector.h"
#include "css/src/css_value_def.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Code points

// The code points of a value, decoded from its bytes. A token's bytes go through cssValueNext, which resolves escapes
// and preprocessing. A value that is already resolved decodes as UTF-8.
struct CodePoints {
    PrStringView text;
    uint32_t pos;
    bool escaped;
};

CodePoints resolved(PrStringView text) {
    return CodePoints{text, 0, false};
}

// Returns the value of the token at `index`.
CodePoints tokenValue(const CssTokenStream* ts, uint32_t index) {
    bool plain = (cssTokenFlags(ts->tokens, index) & CssTokenFlag_PlainText) != 0;
    return CodePoints{cssTokenValueText(ts->tokens, index), 0, !plain};
}

// Steps over one code point. Returns false at the end.
bool next(CodePoints* it, uint32_t* codePoint) {
    if (it->escaped)
        return cssValueNext(it->text, &it->pos, codePoint);

    uint32_t width = prUtf8DecodeScalar(it->text, it->pos, codePoint);
    it->pos += width;
    return width != 0;
}

bool atEnd(const CodePoints* it) {
    return it->pos == it->text.size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Identifiers (cssom-1/#serialize-an-identifier)

// Writes `\` and the code point in lowercase hexadecimal, followed by a space.
void appendHexEscape(TextBuffer* out, uint32_t codePoint) {
    char digits[kU64TextMaxBytes];
    size_t count = formatU64(codePoint, digits, 16);

    textAppendChar(out, '\\');
    textAppend(out, digits, uint32_t(count));
    textAppendChar(out, ' ');
}

// Writes a code point in uppercase hexadecimal, with no leading zeros.
void appendHexUpper(TextBuffer* out, uint32_t codePoint) {
    char digits[kU64TextMaxBytes];
    size_t count = formatU64(codePoint, digits, 16);
    for (size_t i = 0; i < count; ++i)
        digits[i] = asciiUpper(digits[i]);
    textAppend(out, digits, uint32_t(count));
}

// Returns true if an identifier can hold the code point without an escape. These are the ident code points of
// css-syntax-3/#ident-code-point, which include every code point above ASCII.
bool isIdentSafe(uint32_t codePoint) {
    return codePoint >= 0x80 || (kCssByte.of[codePoint] & CssByteFlag_Ident) != 0;
}

// Returns true if `text` already equals what serializing it as a name would write, which holds if every byte is an
// ASCII ident code point. A backslash starts an escape. A null and the two bytes that preprocessing rewrites are not
// ident code points. A byte above ASCII needs decoding. So text that passes is also its own value, whether it is a
// token's bytes or a resolved value.
bool isPlainName(PrStringView text, bool firstPosition) {
    if (text.size == 0)
        return false;

    if (firstPosition) {
        if (asciiIsDigit(text.data[0]))
            return false;
        if (text.data[0] == '-' && (text.size == 1 || asciiIsDigit(text.data[1])))
            return false;
    }

    for (uint32_t i = 0; i < text.size; ++i) {
        CssByteFlags bits = kCssByte.of[uint8_t(text.data[i])];
        if ((bits & (CssByteFlag_Ident | CssByteFlag_Decode)) != CssByteFlag_Ident)
            return false;
    }
    return true;
}

// Returns true if `text`, as a string's interior, already equals what serializing it would write. The bytes that need
// escaping in a string are also the bytes that would need decoding.
bool isPlainString(PrStringView text) {
    for (uint32_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if ((kCssByte.of[uint8_t(c)] & (CssByteFlag_Decode | CssByteFlag_Control)) != 0 || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

// Writes an identifier, escaped so that it tokenizes back to `value` (cssom-1/#serialize-an-identifier).
// `firstPosition` applies the rules for a name's first two characters, which do not apply to the rest of a name.
void serializeName(TextBuffer* out, CodePoints value, bool verbatim, bool firstPosition) {
    if (verbatim || isPlainName(value.text, firstPosition)) {
        textAppend(out, value.text);
        return;
    }

    uint32_t index = 0;
    bool openedWithDash = false;
    uint32_t codePoint = 0;
    while (next(&value, &codePoint)) {
        bool first = firstPosition && index == 0;
        bool second = firstPosition && index == 1;
        if (index == 0)
            openedWithDash = codePoint == '-';
        ++index;

        if (cssIsControl(codePoint)) {
            appendHexEscape(out, codePoint);
        } else if (first && asciiIsDigit(asciiOf(codePoint))) {
            appendHexEscape(out, codePoint);
        } else if (second && openedWithDash && asciiIsDigit(asciiOf(codePoint))) {
            appendHexEscape(out, codePoint);
        } else if (first && openedWithDash && atEnd(&value)) {
            textAppend(out, "\\-");
        } else if (isIdentSafe(codePoint)) {
            textAppendCodePoint(out, codePoint);
        } else {
            textAppendChar(out, '\\');
            textAppendCodePoint(out, codePoint);
        }
    }
}

// Writes a dimension's unit. `1e5` reads back as a number, so a unit cannot start with text that the preceding number
// would read as an exponent.
void serializeUnit(TextBuffer* out, CodePoints value, bool verbatim) {
    if (verbatim) {
        textAppend(out, value.text);
        return;
    }

    CodePoints look = value;
    uint32_t first = 0;
    bool exponent = next(&look, &first) && asciiLower(asciiOf(first)) == 'e' && !atEnd(&look);
    if (exponent) {
        CodePoints after = look;
        uint32_t second = 0;
        uint32_t third = 0;
        if (!(next(&after, &second) && asciiIsDigit(asciiOf(second)))) {
            exponent = second == '-' && next(&after, &third) && asciiIsDigit(asciiOf(third));
        }
    }
    if (!exponent) {
        serializeName(out, value, false, true);
        return;
    }

    appendHexEscape(out, first);
    serializeName(out, look, false, false);
}

// Writes a string, quoted with '"' (cssom-1/#serialize-a-string).
void serializeString(TextBuffer* out, CodePoints value, bool verbatim) {
    textAppendChar(out, '"');
    if (verbatim || isPlainString(value.text)) {
        textAppend(out, value.text);
        textAppendChar(out, '"');
        return;
    }

    uint32_t codePoint = 0;
    while (next(&value, &codePoint)) {
        if (cssIsControl(codePoint)) {
            appendHexEscape(out, codePoint);
        } else if (codePoint == '"' || codePoint == '\\') {
            textAppendChar(out, '\\');
            textAppendChar(out, char(codePoint));
        } else {
            textAppendCodePoint(out, codePoint);
        }
    }
    textAppendChar(out, '"');
}

// Writes a url token's value, escaped so that it tokenizes back to a url token. css-syntax-3/#consume-url-token ends
// the token at a quote, a paren, whitespace or a non-printable code point, so each of those gets a backslash.
void serializeUrlValue(TextBuffer* out, CodePoints value) {
    uint32_t codePoint = 0;
    while (next(&value, &codePoint)) {
        if (cssIsControl(codePoint) || codePoint == ' ') {
            appendHexEscape(out, codePoint);
        } else if (codePoint == '"' || codePoint == '\'' || codePoint == '(' || codePoint == ')' || codePoint == '\\') {
            textAppendChar(out, '\\');
            textAppendChar(out, char(codePoint));
        } else {
            textAppendCodePoint(out, codePoint);
        }
    }
}

// Writes a hash token's value. The value is written as an identifier if it is one. Otherwise the same escaping is used,
// without the rules for a name's first character (cssom-1/#serialize-an-identifier).
void serializeHashValue(TextBuffer* out, CodePoints value) {
    uint32_t codePoint = 0;
    while (next(&value, &codePoint)) {
        if (isIdentSafe(codePoint))
            textAppendCodePoint(out, codePoint);
        else
            appendHexEscape(out, codePoint);
    }
}

// Returns true if a token's value, written back unchanged, is the token's serialization.
bool tokenIsVerbatim(const CssTokenStream* ts, uint32_t index) {
    return (cssTokenFlags(ts->tokens, index) & CssTokenFlag_Verbatim) != 0;
}

// Returns the code point that a delim token holds, or 0 for any other token. needsComment uses it to recognize pairs.
uint32_t delimOf(const CssTokenStream* ts, uint32_t index) {
    return cssTokenKind(ts->tokens, index) == CssTokenKind_Delim ? cssTokenDelim(ts->tokens, index) : 0;
}

// Returns true if a token of `type` starts with an identifier.
bool startsWithIdent(CssTokenKind type) {
    return type == CssTokenKind_Ident || type == CssTokenKind_Function || type == CssTokenKind_Url ||
        type == CssTokenKind_BadUrl;
}

bool isNumeric(CssTokenKind type) {
    return type == CssTokenKind_Number || type == CssTokenKind_Percentage || type == CssTokenKind_Dimension;
}

// Returns true if the two tokens form a pair that would read back as one token with nothing between them
// (css-syntax-3/#serialization).
bool needsComment(const CssTokenStream* ts, uint32_t first, uint32_t second) {
    CssTokenKind a = cssTokenKind(ts->tokens, first);
    CssTokenKind b = cssTokenKind(ts->tokens, second);
    uint32_t delimA = delimOf(ts, first);
    uint32_t delimB = delimOf(ts, second);

    bool name = a == CssTokenKind_Ident || a == CssTokenKind_AtKeyword || a == CssTokenKind_Hash ||
        a == CssTokenKind_Dimension || delimA == '#' || delimA == '-';
    if (name && (startsWithIdent(b) || isNumeric(b) || b == CssTokenKind_Cdc || delimB == '-'))
        return true;
    if (a == CssTokenKind_Ident && b == CssTokenKind_OpenParen)
        return true;
    if (delimA == '@' && (startsWithIdent(b) || b == CssTokenKind_Cdc || delimB == '-'))
        return true;
    if (a == CssTokenKind_Number && (startsWithIdent(b) || isNumeric(b) || b == CssTokenKind_Cdc || delimB == '%'))
        return true;
    if ((delimA == '.' || delimA == '+') && isNumeric(b))
        return true;
    if (delimA == '/' && delimB == '*')
        return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Value trees

// A run of values being serialized.
struct Values {
    TextBuffer* out;
    CssValueRun run;
    bool urls;                                   // true if a <string> of the run itself is a URL
    const PrCssSelectors* PR_NULLABLE selectors; // the compiled form of the run's complex selectors
    uint32_t* PR_NULLABLE selector;              // the next compiled selector to write
};

// Returns true if a space goes between `before` and `after`. No space goes before a ',', a ':' or a closing bracket,
// between the name and the '(' of a <function-token>, or after an opening bracket, a '!' or a '@'.
bool spaced(const CssValueNode* before, const CssValueNode* after) {
    if (cssValueIsLiteral(after, ',') || cssValueIsLiteral(after, ':') ||
            cssValueIsLiteral(after, ')') || cssValueIsLiteral(after, ']'))
        return false;
    if (before->kind == PrCssValueKind_Ident && cssValueIsLiteral(after, '('))
        return false;
    return !cssValueIsLiteral(before, '(') && !cssValueIsLiteral(before, '[') &&
        !cssValueIsLiteral(before, '!') && !cssValueIsLiteral(before, '@');
}

void serializeValue(const Values* v, uint32_t index, CssValueDef parent);

// Returns true if the value at `index` is a value of the run itself rather than a child of one.
bool isTopLevel(const Values* v, uint32_t index) {
    uint32_t i = 0;
    while (i < index)
        i += v->run.values[i].next;
    return i == index;
}

// Writes the values in [first, end), the children of `parent`. `parent` is CssValueDef_Count outside a structural
// production. A production that matched nothing writes nothing, and no space. Inside a production written without
// whitespace, the parenthesized contents, such as a functional pseudo-class's argument, are spaced like any other run.
void serializeRun(const Values* v, uint32_t first, uint32_t end, CssValueDef parent) {
    bool together = parent != CssValueDef_Count && bitsetTest(kCssValueDefNoWhitespace, parent);
    uint32_t depth = 0;     // number of parentheses open in the run
    const CssValueNode* previous = nullptr;
    for (uint32_t i = first; i < end; i += v->run.values[i].next) {
        const CssValueNode* value = &v->run.values[i];
        uint32_t mark = v->out->text.count;
        if (previous != nullptr && (!together || depth != 0) && spaced(previous, value))
            textAppendChar(v->out, ' ');
        uint32_t start = v->out->text.count;
        serializeValue(v, i, parent);
        if (v->out->text.count == start)
            v->out->text.count = mark;
        else
            previous = value;
        if (cssValueIsLiteral(value, '('))
            ++depth;
        else if (cssValueIsLiteral(value, ')') && depth != 0)
            --depth;
    }
}

// cssom-1/#serialize-a-media-query: a media type of `all` is omitted before a condition, unless the query is negated.
void serializeMediaQuery(const Values* v, uint32_t index) {
    const CssValueNode* values = v->run.values;
    uint32_t first = index + 1;
    uint32_t end = index + values[index].next;
    if (first == end) {
        textAppend(v->out, "not all");
        return;
    }
    const CssValueNode* type = &values[first];
    if (cssValueIsProduction(type, CssValueDef_MediaType) && first + type->next < end &&
            asciiEqualsFold(cssValueText(v->run, &values[first + 1]), "all")) {
        first += type->next;
        first += values[first].next;     // `and`
    }
    serializeRun(v, first, end, CssValueDef_MediaQuery);
}

// Writes an ident, in ASCII lowercase if cssom-1/#serialize-a-media-query requires it: for a media type, and for a
// media feature's name. The exception is a <dashed-ident>, which is written as is
// (mediaqueries/mq-unknown-feature-custom-property.html). An <id-selector>'s ident is the name of its <hash-token>,
// after a '#'.
void serializeIdent(const Values* v, const CssValueNode* value, CssValueDef parent) {
    if (parent == CssValueDef_IdSelector)
        textAppendChar(v->out, '#');
    uint32_t at = v->out->text.count;
    PrStringView text = cssValueText(v->run, value);
    cssSerializeIdent(v->out, text);
    if ((parent == CssValueDef_MediaType || parent == CssValueDef_MfName) && !stringStartsWith(text, "--"))
        asciiLower(v->out->text.data + at, v->out->text.count - at);
}

// Writes an alpha in [0, 1] stored in 8 bits, rounded as css-color-4/#serializing-legacy-alpha-values requires. The
// alpha is written in hundredths if they round to the same 8 bits, and in thousandths otherwise. Each rounds to the
// nearest integer, with halves rounding up.
void serializeAlpha(TextBuffer* out, double alpha) {
    uint32_t bits = uint32_t(alpha * 255.0 + 0.5);
    uint32_t hundredths = (bits * 200 + 255) / 510;
    if ((hundredths * 510 + 100) / 200 == bits) {
        cssSerializeNumber(out, hundredths / 100.0);
        return;
    }
    cssSerializeNumber(out, ((bits * 2000 + 255) / 510) / 1000.0);
}

// Writes a color channel in the space's units, to six significant figures (css-color-4/#serializing-color-values).
void serializeChannel(TextBuffer* out, double channel) {
    char text[kF64TextMaxBytes];
    textAppend(out, text, uint32_t(formatF64Significant(channel, 6, text)));
}

// Rounds an sRGB channel in [0, 1] to an integer in [0, 255], with halves rounding up.
double byteOf(double channel) {
    channel = channel < 0.0 ? 0.0 : channel > 1.0 ? 1.0 : channel;
    return floorF64(channel * 255.0 + 0.5);
}

// Returns the channel `i` of a color in the units of its space, or 0 if it is written `none`.
double channelValue(const Values* v, const CssColorChannels& channels, uint32_t i) {
    if ((channels.missing & (1u << i)) != 0)
        return 0.0;
    return cssColorChannelValue(&v->run.values[channels.at[i]], *channels.meaning[i]);
}

// Writes a color written with rgb(), rgba(), hsl(), hsla() or hwb() whose channels are all numbers, in the legacy comma
// form of css-color-4/#css-serialization-of-srgb. The function is rgb() if the alpha is 1 and rgba() otherwise, and a
// `none` channel is written as 0.
void serializeLegacyColor(const Values* v, PrColorSpace space, const CssColorChannels& channels) {
    double c[4];
    for (uint32_t i = 0; i < 4; ++i)
        c[i] = channelValue(v, channels, i);
    double channel[3] = {c[0], c[1], c[2]};
    if (space != PrColorSpace_Srgb) {
        PrColor rgb =
            space == PrColorSpace_Hsl ? prColorFromHsl(c[0], c[1], c[2], c[3]) : prColorFromHwb(c[0], c[1], c[2], c[3]);
        channel[0] = double(rgb.r);
        channel[1] = double(rgb.g);
        channel[2] = double(rgb.b);
    }
    bool opaque = c[3] >= 1.0;
    textAppend(v->out, opaque ? "rgb(" : "rgba(");
    for (uint32_t i = 0; i < 3; ++i) {
        if (i != 0)
            textAppend(v->out, ", ");
        cssSerializeNumber(v->out, byteOf(channel[i]));
    }
    if (!opaque) {
        textAppend(v->out, ", ");
        serializeAlpha(v->out, c[3]);
    }
    textAppendChar(v->out, ')');
}

// Writes a color in the modern form of its function (css-color-4/#serializing-color-values). The function is the one
// named for the color's space if there is one, rgb() for rgb() and rgba(), and color() for the rest. The channels are
// in the space's units, and the alpha is written if it is not 1.
void serializeModernColor(const Values* v, PrColorSpace space, bool legacy, const CssColorChannels& channels) {
    PrStringView name = prColorSpaceName(space);
    PrColorModel model = prColorSpaceModel(space);
    if (legacy) {
        textAppend(v->out, "rgb(");
    } else if (model == PrColorModel_Rgb || model == PrColorModel_Xyz) {
        textAppend(v->out, "color(");
        textAppend(v->out, name);
        textAppendChar(v->out, ' ');
    } else {
        textAppend(v->out, name);
        textAppendChar(v->out, '(');
    }

    for (uint32_t i = 0; i < 4; ++i) {
        bool missing = (channels.missing & (1u << i)) != 0;
        bool calculated = (channels.calculated & (1u << i)) != 0;
        double channel = missing || calculated ? 0.0 : channelValue(v, channels, i);
        if (i == 3) {
            if (!missing && !calculated && channel >= 1.0)
                break;
            textAppend(v->out, " / ");
        } else if (i != 0) {
            textAppendChar(v->out, ' ');
        }

        if (missing)
            textAppend(v->out, "none");
        else if (calculated)
            serializeValue(v, channels.at[i], CssValueDef_Count);
        else
            serializeChannel(v->out, legacy && i != 3 ? channel * 255.0 : channel);
    }
    textAppendChar(v->out, ')');
}

// ---------------------------------------------------------------------------------------------------------------------
// Math Functions (css-values-4/#calc-serialize)

// Returns a numeric value's unit: "%" for a percentage, and the empty string for a number.
PrStringView unitText(const CssValueNode* value) {
    if (value->kind == PrCssValueKind_Percentage)
        return "%";
    return value->kind == PrCssValueKind_Dimension ? cssUnitName(value->unit) : PrStringView{""};
}

// Writes a numeric value of a calculation. A value that is infinite or NaN is written as its keyword, times one of its
// unit (css-values-4/#calc-error-constants).
void serializeNumeric(TextBuffer* out, const CssValueNode* value) {
    double number = value->number;
    PrStringView unit = unitText(value);
    if (isFiniteF64(number)) {
        cssSerializeNumber(out, number);
        textAppend(out, unit);
        return;
    }
    textAppend(out, isNanF64(number) ? "NaN" : number > 0     ? "infinity" : "-infinity");
    if (unit.size != 0) {
        textAppend(out, " * 1");
        textAppend(out, unit);
    }
}

void serializeTree(const Values* v, uint32_t index);

// Removes the parentheses around a calculation written from `mark` on, if it starts with "(" and ends with ")"
// (css-values-4/#serialize-a-math-function).
void stripParentheses(TextBuffer* out, uint32_t mark) {
    char* text = out->text.data + mark;
    uint32_t size = out->text.count - mark;
    if (size < 2 || text[0] != '(' || text[size - 1] != ')')
        return;
    memMove(text, text + 1, size - 2);
    out->text.count -= 2;
}

// Writes a math function node: its name and its arguments. Arguments are separated by a comma and a space, and the
// parts of one argument by a space.
void serializeMathNode(const Values* v, uint32_t index) {
    const CssValueNode* node = &v->run.values[index];
    textAppend(v->out, cssKeywordName(cssValueKeyword(node)));
    textAppendChar(v->out, '(');
    bool spaced = false;
    for (uint32_t i = index + 1; i < index + node->next; i += v->run.values[i].next) {
        if (cssValueIsLiteral(&v->run.values[i], ',')) {
            textAppend(v->out, ", ");
            spaced = false;
            continue;
        }
        if (spaced)
            textAppendChar(v->out, ' ');
        uint32_t mark = v->out->text.count;
        serializeTree(v, i);
        stripParentheses(v->out, mark);
        spaced = true;
    }
    textAppendChar(v->out, ')');
}

// Writes a color in the form that css-color-4/#serializing-color-values gives the function that it was written with.
// That is the legacy rgb() form if the color is sRGB and can use it, and the modern form of its own function otherwise.
// rgb() and rgba() use the modern form only if a channel is a math function.
void serializeColor(const Values* v, uint32_t index) {
    CssColorChannels channels = cssColorChannelsFromValue(v->run, index);
    PrColorSpace space = channels.space;
    bool legacy = channels.legacy;
    bool srgb = legacy || space == PrColorSpace_Hsl || space == PrColorSpace_Hwb;
    if (srgb && channels.calculated == 0 && (legacy || channels.missing == 0))
        serializeLegacyColor(v, space, channels);
    else
        serializeModernColor(v, space, legacy, channels);
}

// Returns the sort rank of a child in css-values-4/#sort-a-calculations-children: a number, then a percentage, then
// dimensions by unit, then the rest.
uint32_t sortRank(const CssValueNode* value) {
    switch (value->kind) {
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:       return 0;
        case PrCssValueKind_Percentage:   return 1;
        case PrCssValueKind_Dimension:    return 2;
        default:                        return 3;
    }
}

// Returns true if the child at `a` sorts before the child at `b`.
bool sortsBefore(const Values* v, uint32_t a, uint32_t b) {
    const CssValueNode* x = &v->run.values[a];
    const CssValueNode* y = &v->run.values[b];
    uint32_t rankX = sortRank(x);
    uint32_t rankY = sortRank(y);
    if (rankX != rankY)
        return rankX < rankY;
    if (rankX == 2) {
        int order = asciiCompareFold(cssUnitName(x->unit), cssUnitName(y->unit));
        if (order != 0)
            return order < 0;
    }
    return a < b;
}

// Returns the child of the Sum or Product node at `index` that sorts next after `after`, or the end of the node if
// there is none. Pass `index` as `after` to get the first child.
uint32_t nextSorted(const Values* v, uint32_t index, uint32_t after) {
    uint32_t end = index + v->run.values[index].next;
    uint32_t best = end;
    for (uint32_t i = index + 1; i < end; i += v->run.values[i].next) {
        if ((after == index || sortsBefore(v, after, i)) && (best == end || sortsBefore(v, i, best)))
            best = i;
    }
    return best;
}

// Writes a Sum or Product node, with its children sorted and joined by their operators.
void serializeOperands(const Values* v, uint32_t index, bool sum) {
    uint32_t end = index + v->run.values[index].next;
    textAppendChar(v->out, '(');
    bool first = true;
    for (uint32_t i = nextSorted(v, index, index); i < end; i = nextSorted(v, index, i)) {
        const CssValueNode* child = &v->run.values[i];
        PrCssCalc inverse = sum ? PrCssCalc_Negate : PrCssCalc_Invert;
        if (first) {
            serializeTree(v, i);
        } else if (child->kind == PrCssValueKind_Calc && cssValueCalcOperator(child) == inverse) {
            textAppend(v->out, sum ? " - " : " / ");
            serializeTree(v, i + 1);
        } else if (sum && cssValueIsNumeric(child->kind) && child->number < 0) {
            CssValueNode negated = *child;
            negated.number = -child->number;
            textAppend(v->out, " - ");
            serializeNumeric(v->out, &negated);
        } else {
            textAppend(v->out, sum ? " + " : " * ");
            serializeTree(v, i);
        }
        first = false;
    }
    textAppendChar(v->out, ')');
}

// Serializes the calculation tree at `index` (css-values-4/#serialize-a-calculation-tree).
void serializeTree(const Values* v, uint32_t index) {
    const CssValueNode* node = &v->run.values[index];
    if (cssValueIsNumeric(node->kind)) {
        serializeNumeric(v->out, node);
        return;
    }
    if (node->kind == PrCssValueKind_Function && cssMathFunction(cssValueKeyword(node)) != CssValueDef_Count) {
        serializeMathNode(v, index);
        return;
    }
    if (node->kind != PrCssValueKind_Calc) {
        serializeValue(v, index, CssValueDef_Count);
        return;
    }
    switch (cssValueCalcOperator(node)) {
        case PrCssCalc_Negate:
            textAppend(v->out, "(-1 * ");
            serializeTree(v, index + 1);
            textAppendChar(v->out, ')');
            break;
        case PrCssCalc_Invert:
            textAppend(v->out, "(1 / ");
            serializeTree(v, index + 1);
            textAppendChar(v->out, ')');
            break;
        default:
            serializeOperands(v, index, cssValueCalcOperator(node) == PrCssCalc_Sum);
            break;
    }
}

// Serializes the PrCssValueKind_Math value at `index` (css-values-4/#serialize-a-math-function).
void serializeMath(const Values* v, uint32_t index) {
    uint32_t root = index + 1;
    const CssValueNode* node = &v->run.values[root];
    if (node->kind == PrCssValueKind_Function && cssMathFunction(cssValueKeyword(node)) != CssValueDef_Count) {
        serializeMathNode(v, root);
        return;
    }
    // A leaf that is not a number, such as anchor(), is written without calc(). A relative color's component keyword is
    // a number that the origin color decides.
    if (node->kind != PrCssValueKind_Calc && !cssValueIsNumeric(node->kind) && node->kind != PrCssValueKind_Keyword) {
        serializeValue(v, root, CssValueDef_Count);
        return;
    }
    textAppend(v->out, "calc(");
    uint32_t mark = v->out->text.count;
    serializeTree(v, root);
    stripParentheses(v->out, mark);
    textAppendChar(v->out, ')');
}

// Serializes the value at `index` as a component value (cssom-1/#serialize-a-css-component-value).
void serializeValue(const Values* v, uint32_t index, CssValueDef parent) {
    const CssValueNode* value = &v->run.values[index];
    switch (value->kind) {
        case PrCssValueKind_Keyword: {
            // css-animations-1/#dom-csskeyframerule-keytext: `from` is 0% and `to` is 100%.
            PrCssKeyword keyword = cssValueKeyword(value);
            if (parent == CssValueDef_KeyframeSelector && keyword == PrCssKeyword_From)
                textAppend(v->out, "0%");
            else if (parent == CssValueDef_KeyframeSelector && keyword == PrCssKeyword_To)
                textAppend(v->out, "100%");
            else
                textAppend(v->out, cssKeywordName(keyword));
            break;
        }
        case PrCssValueKind_Ident:
            serializeIdent(v, value, parent);
            break;
        case PrCssValueKind_String:
            if ((value->aux & CssValueAuxFlag_FamilyIdents) != 0)
                textAppend(v->out, cssValueText(v->run, value));
            else if (v->urls && parent == CssValueDef_Count && isTopLevel(v, index))
                cssSerializeUrl(v->out, cssValueText(v->run, value));
            else
                cssSerializeString(v->out, cssValueText(v->run, value));
            break;
        case PrCssValueKind_Url:
            cssSerializeUrl(v->out, cssValueText(v->run, value));
            break;
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
            cssSerializeNumber(v->out, value->number);
            break;
        case PrCssValueKind_Percentage:
            cssSerializeNumber(v->out, value->number);
            textAppendChar(v->out, '%');
            break;
        case PrCssValueKind_Dimension:
            cssSerializeNumber(v->out, value->number);
            if (value->unit == PrCssUnit_Unknown)
                cssSerializeIdent(v->out, cssValueText(v->run, &v->run.values[index + 1]));
            else
                textAppend(v->out, cssUnitName(value->unit));
            break;
        case PrCssValueKind_Color:
            serializeColor(v, index);
            break;
        case PrCssValueKind_Function:
            if (cssValueKeyword(value) == PrCssKeyword_Path) {
                cssSerializePath(v->out, v->run, index);
                break;
            }
            textAppend(v->out, cssKeywordName(cssValueKeyword(value)));
            textAppendChar(v->out, '(');
            serializeRun(v, index + 1, index + value->next, CssValueDef_Count);
            textAppendChar(v->out, ')');
            break;
        case PrCssValueKind_Math:
            serializeMath(v, index);
            break;
        case PrCssValueKind_UnicodeRange:
            cssSerializeUnicodeRange(v->out, value->range.first, value->range.last);
            break;
        case PrCssValueKind_Tokens:
            textAppend(v->out, cssValueText(v->run, value));
            break;
        case PrCssValueKind_Production:
            if (v->selectors != nullptr && cssIsComplexSelectorNode(value))
                cssSerializeSelectorRange(v->out, v->selectors, (*v->selector)++, 1);
            else if (cssValueProduction(value) == CssValueDef_MediaQuery)
                serializeMediaQuery(v, index);
            else if (cssValueProduction(value) == CssValueDef_ANPlusB)
                cssSerializeNth(v->out, int32_t(v->run.values[index + 1].number),
                    int32_t(v->run.values[index + 2].number));
            else
                serializeRun(v, index + 1, index + value->next, cssValueProduction(value));
            break;
        case PrCssValueKind_Literal:
            textAppendChar(v->out, cssValueLiteral(value));
            break;
        case PrCssValueKind_Longhand:
            serializeRun(v, index + 1, index + value->next, CssValueDef_Count);
            break;
        default:
            break;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Values

void cssSerializeNumber(TextBuffer* out, double value) {
    // css-values-4/#calc-internal: a value outside the range an implementation supports is clamped to that range, so
    // the serialized value is always a finite number.
    if (isNanF64(value))
        value = 0.0;
    else if (value > kF64Max)
        value = kF64Max;
    else if (value < -kF64Max)
        value = -kF64Max;

    char text[kF64TextMaxBytes];
    size_t length = formatF64(value, text);
    textAppend(out, text, uint32_t(length));
}

void cssSerializeIdent(TextBuffer* out, PrStringView text) {
    serializeName(out, resolved(text), false, true);
}

bool cssIsBareIdent(PrStringView text) {
    // serializeName escapes code points that are not ident code points, a digit at the start of the name or after its
    // leading '-', and a lone '-'.
    CodePoints value = resolved(text);
    uint32_t codePoint = 0;
    uint32_t first = 0;
    uint32_t count = 0;
    bool bare = true;
    while (next(&value, &codePoint)) {
        bool digit = asciiIsDigit(asciiOf(codePoint));
        bool opening = count == 0 ? digit : count == 1 && first == '-' && digit;
        bare = bare && !opening && isIdentSafe(codePoint);
        first = count++ == 0 ? codePoint : first;
    }
    return bare && count != 0 && !(count == 1 && first == '-');
}

void cssSerializeString(TextBuffer* out, PrStringView text) {
    serializeString(out, resolved(text), false);
}

void cssSerializeUrl(TextBuffer* out, PrStringView text) {
    textAppend(out, "url(");
    cssSerializeString(out, text);
    textAppendChar(out, ')');
}

void cssSerializeUnicodeRange(TextBuffer* out, uint32_t first, uint32_t last) {
    textAppend(out, "U+");
    appendHexUpper(out, first);
    if (last != first) {
        textAppendChar(out, '-');
        appendHexUpper(out, last);
    }
}

void cssSerializeNth(TextBuffer* out, int32_t a, int32_t b) {
    char digits[kU64TextMaxBytes];

    if (a != 0) {
        if (a < 0)
            textAppendChar(out, '-');
        uint32_t magnitude = uint32_t(a < 0 ? -int64_t(a) : a);
        if (magnitude != 1)
            textAppend(out, digits, uint32_t(formatU64(magnitude, digits)));
        textAppendChar(out, 'n');

        if (b == 0)
            return;
        textAppendChar(out, b < 0 ? '-' : '+');
    } else if (b < 0) {
        textAppendChar(out, '-');
    }

    uint32_t magnitude = uint32_t(b < 0 ? -int64_t(b) : b);
    textAppend(out, digits, uint32_t(formatU64(magnitude, digits)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Value trees

void cssSerializeValues(TextBuffer* out, CssValueRun run) {
    Values v = {out, run, false, nullptr, nullptr};
    serializeRun(&v, 0, run.count, CssValueDef_Count);
}

void cssSerializePreludeValues(TextBuffer* out, CssValueRun run, bool urls,
        const PrCssSelectors* PR_NULLABLE selectors, uint32_t first) {
    Values v = {out, run, urls, selectors, &first};
    serializeRun(&v, 0, run.count, CssValueDef_Count);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

void cssSerializeToken(TextBuffer* out, const CssTokenStream* ts, uint32_t index) {
    CssTokenKind kind = cssTokenKind(ts->tokens, index);
    switch (kind) {
        case CssTokenKind_Eof:
            break;

        case CssTokenKind_Ident:
            serializeName(out, tokenValue(ts, index), tokenIsVerbatim(ts, index), true);
            break;

        case CssTokenKind_Function:
            serializeName(out, tokenValue(ts, index), tokenIsVerbatim(ts, index), true);
            textAppendChar(out, '(');
            break;

        case CssTokenKind_AtKeyword:
            textAppendChar(out, '@');
            serializeName(out, tokenValue(ts, index), tokenIsVerbatim(ts, index), true);
            break;

        case CssTokenKind_Hash:
            textAppendChar(out, '#');
            if ((cssTokenFlags(ts->tokens, index) & CssTokenFlag_Id) != 0)
                serializeName(out, tokenValue(ts, index), tokenIsVerbatim(ts, index), true);
            else
                serializeHashValue(out, tokenValue(ts, index));
            break;

        case CssTokenKind_String:
            serializeString(out, tokenValue(ts, index), tokenIsVerbatim(ts, index));
            break;

        case CssTokenKind_Url:
            textAppend(out, "url(");
            serializeUrlValue(out, tokenValue(ts, index));
            textAppendChar(out, ')');
            break;

        // A bad string and a bad url have no serialization that tokenizes back to them, so the source bytes are written
        // instead (css-syntax-3/#serialization).
        case CssTokenKind_BadString:
        case CssTokenKind_BadUrl:
            textAppend(out, cssTokenText(ts->tokens, index));
            break;

        case CssTokenKind_Delim:
            if (cssTokenDelim(ts->tokens, index) == '\\')
                textAppend(out, "\\\n");
            else
                textAppendCodePoint(out, cssTokenDelim(ts->tokens, index));
            break;

        case CssTokenKind_Number:
            cssSerializeNumber(out, cssTokenNumber(ts->tokens, index));
            break;
        case CssTokenKind_Percentage:
            cssSerializeNumber(out, cssTokenNumber(ts->tokens, index));
            textAppendChar(out, '%');
            break;
        case CssTokenKind_Dimension: {
            cssSerializeNumber(out, cssTokenNumber(ts->tokens, index));
            bool plain = (cssTokenFlags(ts->tokens, index) & CssTokenFlag_PlainText) != 0;
            CodePoints unit = {cssTokenUnitText(ts->tokens, index), 0, !plain};
            serializeUnit(out, unit, tokenIsVerbatim(ts, index));
            break;
        }

        case CssTokenKind_UnicodeRange: {
            uint32_t first = 0;
            uint32_t last = 0;
            cssTokenRange(ts->tokens, index, &first, &last);
            cssSerializeUnicodeRange(out, first, last);
            break;
        }

        case CssTokenKind_Whitespace:
            textAppendChar(out, ' ');
            break;
        case CssTokenKind_Cdo:
            textAppend(out, "<!--");
            break;
        case CssTokenKind_Cdc:
            textAppend(out, "-->");
            break;

        case CssTokenKind_Colon:
        case CssTokenKind_Semicolon:
        case CssTokenKind_Comma:
        case CssTokenKind_OpenBracket:
        case CssTokenKind_CloseBracket:
        case CssTokenKind_OpenParen:
        case CssTokenKind_CloseParen:
        case CssTokenKind_OpenBrace:
        case CssTokenKind_CloseBrace:
            textAppendChar(out, kCssPunctuation.byteOf[kind]);
            break;
    }
}

void cssSerializeTokens(TextBuffer* out, const CssTokenStream* ts, CssTokenSpan span) {
    // Bytes that tokenized as two tokens tokenize as two again, so a run of source contains none of the pairs that
    // needsComment separates.
    PrStringView source;
    if (cssSpanIsVerbatim(ts->tokens, span, &source)) {
        textAppend(out, source);
        return;
    }

    for (uint32_t i = 0; i < span.count; ++i) {
        uint32_t index = span.start + i;
        if (i != 0 && needsComment(ts, index - 1, index))
            textAppend(out, "/**/");
        cssSerializeToken(out, ts, index);
    }
}

} // namespace pr_impl
