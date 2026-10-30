#include "css/src/css_value.h"

#include "pr/color.h"
#include "pr/color_space.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "css/src/css_keyword.h"
#include "css/src/css_path.h"
#include "css/src/css_serialize.h"

namespace pr_impl {

namespace {

// Adds a run of the tokens of given values (cssValueTokens) as tokens. If the whole run is one value kept as tokens,
// that value is added as is. Otherwise the text of each value is added, with a space between each two.
void addGivenTokens(CssValuePool* pool, const CssTokenStream* ts, CssTokenSpan span) {
    const CssTokenArray* tokens = ts->tokens;
    if (span.count == 1 && cssTokenKind(tokens, span.start) == CssTokenKind_Value &&
            cssTokenValue(tokens, span.start).values->kind == PrCssValueKind_Tokens) {
        cssValuePoolAddRun(pool, cssTokenValue(tokens, span.start));
        return;
    }
    TextBuffer text;
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        if (i != span.start)
            textAppendChar(&text, ' ');
        if (cssTokenKind(tokens, i) == CssTokenKind_Value)
            cssSerializeValues(&text, cssTokenValue(tokens, i));
        else
            cssSerializeToken(&text, ts, i);
    }
    CssValueNode value = {};
    value.kind = PrCssValueKind_Tokens;
    if (stringContains(textView(&text), "attr("))
        value.aux = CssValueAuxFlag_ReadsAttributes;
    value.text.start = cssValuePoolAddText(pool, textView(&text));
    value.text.length = text.text.count;
    cssValuePoolAdd(pool, value);
    textReset(&text);
}

// Returns true if `value` can be a color's channel: a numeric value, a keyword or a calculation.
bool isChannel(const CssValueNode* value) {
    return cssValueIsNumeric(value->kind) || value->kind == PrCssValueKind_Keyword ||
        value->kind == PrCssValueKind_Math;
}

// The state for writing the tokens of a run.
struct TokenWriter {
    CssTokenArray* tokens;
    TextBuffer* source;
    CssValueRun run;
    uint32_t count;
};

// Adds a token of `kind` written as `bytes`, whose value is those bytes. Returns the token's index.
uint32_t addToken(TokenWriter* w, CssTokenKind kind, PrStringView bytes) {
    CssTokenArray* tokens = w->tokens;
    uint32_t index = w->count++;
    tokens->starts.data[index] = w->source->text.count;
    textAppend(w->source, bytes);
    tokens->ends.data[index] = w->source->text.count;
    tokens->kinds.data[index] = kind;
    tokens->flags.data[index] = CssTokenFlag_PlainText;
    tokens->units.data[index] = PrCssUnit_Number;
    tokens->closers.data[index] = index;
    return index;
}

// Adds a function token named `name`, for the arguments that follow it.
uint32_t addFunction(TokenWriter* w, PrStringView name) {
    uint32_t index = addToken(w, CssTokenKind_Function, name);
    textAppendChar(w->source, '(');
    w->tokens->ends.data[index] = w->source->text.count;
    return index;
}

// Adds the ')' that closes the function token at `function`.
void closeFunction(TokenWriter* w, uint32_t function) {
    w->tokens->closers.data[function] = addToken(w, CssTokenKind_CloseParen, ")");
}

// Adds a literal character.
void addLiteral(TokenWriter* w, uint32_t character) {
    char byte = char(character);
    CssTokenKind kind = character < 0x80 ? kCssPunctuation.kindOf[uint8_t(byte)] : CssTokenKind_Delim;
    addToken(w, kind, PrStringView{&byte, 1});
}

// Adds a CssTokenKind_Value token for the value at `index`, covering none of the source.
void addGiven(TokenWriter* w, uint32_t index) {
    CssTokenArray* tokens = w->tokens;
    uint32_t token = w->count++;
    tokens->starts.data[token] = w->source->text.count;
    tokens->ends.data[token] = w->source->text.count;
    tokens->kinds.data[token] = CssTokenKind_Value;
    tokens->flags.data[token] = 0;
    tokens->units.data[token] = w->run.values[index].unit;
    tokens->closers.data[token] = index;
}

// Returns the function that a color in `space` folds from, if it is not color(). That is the function that names the
// space, or rgb() for sRGB written in the legacy form.
PrCssKeyword colorFunction(PrColorSpace space, bool legacy) {
    switch (space) {
        case PrColorSpace_Srgb:
            return legacy ? PrCssKeyword_Rgb : PrCssKeyword_Color;
        case PrColorSpace_Hsl:   return PrCssKeyword_Hsl;
        case PrColorSpace_Hwb:   return PrCssKeyword_Hwb;
        case PrColorSpace_Lab:   return PrCssKeyword_Lab;
        case PrColorSpace_Lch:   return PrCssKeyword_Lch;
        case PrColorSpace_Oklab: return PrCssKeyword_Oklab;
        case PrColorSpace_Oklch: return PrCssKeyword_Oklch;
        case PrColorSpace_Ictcp: return PrCssKeyword_Ictcp;
        case PrColorSpace_Jzazbz: return PrCssKeyword_Jzazbz;
        case PrColorSpace_Jzczhz: return PrCssKeyword_Jzczhz;
        default:                 return PrCssKeyword_Color;
    }
}

void writeTree(TokenWriter* w, uint32_t index);

void writeChildren(TokenWriter* w, uint32_t index) {
    const CssValueNode* value = &w->run.values[index];
    for (uint32_t child = index + 1; child < index + value->next; child += w->run.values[child].next)
        writeTree(w, child);
}

// Writes a color as the function that it folds from, its channels, a '/' and its alpha.
void writeColor(TokenWriter* w, uint32_t index) {
    const CssValueNode* color = &w->run.values[index];
    PrColorSpace space = cssValueColorSpace(color);
    PrCssKeyword function = colorFunction(space, cssValueColorLegacy(color));
    uint32_t open = addFunction(w, cssKeywordName(function));
    if (function == PrCssKeyword_Color)
        addToken(w, CssTokenKind_Ident, prColorSpaceName(space));
    uint32_t channel = 0;
    for (uint32_t child = index + 1; child < index + color->next; child += w->run.values[child].next) {
        if (channel++ == 3)
            addLiteral(w, '/');
        writeTree(w, child);
    }
    closeFunction(w, open);
}

// Writes a path() as it is written: its fill rule, and its path data as a string token. Path data as prPathDataWrite
// writes it holds no quote, backslash or newline, so the string's bytes are its value.
void writePath(TokenWriter* w, uint32_t index) {
    uint32_t open = addFunction(w, "path");
    uint32_t start = cssPathDataStart(w->run, index);
    for (uint32_t child = index + 1; child < start; child += w->run.values[child].next)
        writeTree(w, child);
    Array<PrPathDataCommand> commands;
    cssPathCommands(w->run, index, &commands);
    PrString data = prStringNull();
    prStringAppend(&data, "\"");
    prPathDataWrite(commands.data, commands.count, &data);
    prStringAppend(&data, "\"");
    addToken(w, CssTokenKind_String, prStringView(&data));
    closeFunction(w, open);
    prStringReset(&data);
    commands.reset();
}

void writeTree(TokenWriter* w, uint32_t index) {
    const CssValueNode* value = &w->run.values[index];
    switch (value->kind) {
        case PrCssValueKind_Keyword:
            addToken(w, CssTokenKind_Ident, cssKeywordName(cssValueKeyword(value)));
            break;
        case PrCssValueKind_Ident:
            addToken(w, CssTokenKind_Ident, cssValueText(w->run, value));
            break;
        case PrCssValueKind_Literal:
            addLiteral(w, uint8_t(cssValueLiteral(value)));
            break;
        case PrCssValueKind_Function: {
            if (cssValueKeyword(value) == PrCssKeyword_Path) {
                writePath(w, index);
                break;
            }
            uint32_t open = addFunction(w, cssKeywordName(cssValueKeyword(value)));
            writeChildren(w, index);
            closeFunction(w, open);
            break;
        }
        case PrCssValueKind_Color:
            writeColor(w, index);
            break;
        case PrCssValueKind_Longhand:
            writeChildren(w, index);
            break;
        default:
            addGiven(w, index);
            break;
    }
}

} // namespace

bool cssValueRunEquals(CssValueRun a, CssValueRun b) {
    if (a.count != b.count)
        return false;
    for (uint32_t i = 0; i < a.count; ++i) {
        const CssValueNode* x = &a.values[i];
        const CssValueNode* y = &b.values[i];
        if (x->kind != y->kind || x->unit != y->unit || x->aux != y->aux || x->next != y->next)
            return false;
        // A production's tokens record where it was matched, which says nothing about what it holds, so they are not
        // compared.
        if (cssValueHasText(x->kind)) {
            if (cssValueText(a, x) != cssValueText(b, y))
                return false;
        } else if (cssValueIsNumeric(x->kind)) {
            if (x->number != y->number)
                return false;
        } else if (x->kind != PrCssValueKind_Production && !memEquals(&x->number, &y->number, sizeof(double))) {
            return false;
        }
    }
    return true;
}

void cssValueRunText(CssValueRun run, uint32_t* start, uint32_t* size) {
    uint32_t first = UINT32_MAX;
    uint32_t end = 0;
    for (uint32_t i = 0; i < run.count; ++i) {
        const CssValueNode* value = &run.values[i];
        if (!cssValueHasText(value->kind))
            continue;
        uint32_t after = value->text.start + value->text.length;
        first = value->text.start < first ? value->text.start : first;
        end = after > end ? after : end;
    }
    *start = end != 0 ? first : 0;
    *size = end != 0 ? end - first : 0;
}

uint32_t cssValueRunCopy(CssValueRun run, CssValueNode* values, char* text, uint32_t offset) {
    uint32_t start = 0;
    uint32_t size = 0;
    cssValueRunText(run, &start, &size);
    if (size != 0)
        memCopy(text, run.text + start, size);

    for (uint32_t i = 0; i < run.count; ++i) {
        CssValueNode value = run.values[i];
        if (cssValueHasText(value.kind))
            value.text.start = value.text.start - start + offset;
        values[i] = value;
    }
    return size;
}

CssValueList cssValueListCopy(CssValueRun run) {
    CssValueList list = {};
    if (run.count == 0)
        return list;

    uint32_t start = 0;
    cssValueRunText(run, &start, &list.textSize);
    list.count = run.count;
    list.values = static_cast<CssValueNode*>(prMemoryAlloc(sizeof(CssValueNode) * run.count + list.textSize));
    cssValueRunCopy(run, list.values, reinterpret_cast<char*>(list.values + run.count), 0);
    return list;
}

void cssValueListReset(CssValueList* list) {
    prMemoryFree(list->values);
    *list = CssValueList{};
}

void cssValuePoolReset(CssValuePool* pool) {
    pool->values.reset();
    pool->text.reset();
}

uint32_t cssValuePoolAdd(CssValuePool* pool, CssValueNode value) {
    value.next = 1;
    pool->values.push(value);
    return pool->values.count - 1;
}

CssValueSpan cssValuePoolAddRun(CssValuePool* pool, CssValueRun run) {
    uint32_t start = 0;
    uint32_t size = 0;
    cssValueRunText(run, &start, &size);
    pool->values.reserve(pool->values.count + run.count);
    pool->text.reserve(pool->text.count + size);

    CssValueSpan span = {pool->values.count, run.count};
    uint32_t copied = cssValueRunCopy(run, pool->values.data + span.start,
        pool->text.data + pool->text.count, pool->text.count);
    pool->values.count += run.count;
    pool->text.count += copied;
    return span;
}

CssValueSpan cssValuePoolRepeat(CssValuePool* pool, CssValueSpan span) {
    uint32_t start = 0;
    uint32_t size = 0;
    cssValueRunText(cssValuePoolRun(pool, span), &start, &size);
    // The run is read from the arrays that it is copied onto, which must not reallocate during the copy.
    pool->values.reserve(pool->values.count + span.count);
    pool->text.reserve(pool->text.count + size);
    return cssValuePoolAddRun(pool, cssValuePoolRun(pool, span));
}

uint32_t cssValuePoolAddText(CssValuePool* pool, PrStringView text) {
    uint32_t start = pool->text.count;
    if (text.size != 0)
        pool->text.append(text.data, uint32_t(text.size));
    return start;
}

uint32_t cssValuePoolAddTokenText(CssValuePool* pool, const CssTokenStream* ts, uint32_t index, uint32_t* length) {
    PrStringView text = cssTokenValueText(ts->tokens, index);
    if ((cssTokenFlags(ts->tokens, index) & CssTokenFlag_PlainText) == 0)
        return cssValuePoolAddValueText(pool, text, length);

    *length = uint32_t(text.size);
    return cssValuePoolAddText(pool, text);
}

uint32_t cssValuePoolAddValueText(CssValuePool* pool, PrStringView text, uint32_t* length) {
    uint32_t start = pool->text.count;
    uint32_t pos = 0;
    uint32_t codePoint = 0;
    while (cssValueNext(text, &pos, &codePoint)) {
        char encoded[kPrUtf8MaxBytes];
        uint32_t size = prUtf8Encode(codePoint, encoded);
        pool->text.append(encoded, size);
    }
    *length = pool->text.count - start;
    return start;
}

// Adds a run of tokens, kept as the source that it covers. css-variables-1/#serializing-custom-props writes it exactly
// as the author did, including comments and whitespace. A last token that the input ended inside, such as `foo\` or
// `"foo`, is written as it was read (css-syntax-3/#consume-escaped-code-point).
void cssValuePoolAddTokens(CssValuePool* pool, const CssTokenStream* ts, CssTokenSpan span) {
    const CssTokenArray* tokens = ts->tokens;
    if (tokens->values != nullptr) {
        addGivenTokens(pool, ts, span);
        return;
    }
    CssValueNode value = {};
    value.kind = PrCssValueKind_Tokens;
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        if (cssTokenKind(tokens, i) == CssTokenKind_Function && cssTokenKeyword(tokens, i) == PrCssKeyword_Attr)
            value.aux = CssValueAuxFlag_ReadsAttributes;
    }
    uint32_t text = pool->text.count;
    if (span.count != 0) {
        uint32_t last = span.start + span.count - 1;
        bool cut = tokens->ends.data[last] == tokens->source.size &&
            (cssTokenFlags(tokens, last) & CssTokenFlag_Verbatim) == 0;
        uint32_t written = cut ? span.count - 1 : span.count;
        if (written != 0)
            cssValuePoolAddText(pool, cssSpanText(tokens, CssTokenSpan{span.start, written}));
        if (cut) {
            TextBuffer read;
            cssSerializeToken(&read, ts, last);
            cssValuePoolAddText(pool, textView(&read));
            textReset(&read);
        }
    }
    value.text.start = text;
    value.text.length = pool->text.count - text;
    cssValuePoolAdd(pool, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Values

void cssValueReset(PrCssValue* value) {
    cssValuePoolReset(&value->pool);
    value->open.reset();
}

void cssValueClear(PrCssValue* value) {
    value->pool.values.clear();
    value->pool.text.clear();
    value->open.clear();
}

bool cssValueShapeValid(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    uint32_t end = index + value->next;
    uint32_t count = 0;
    bool channels = true;
    for (uint32_t child = index + 1; child < end; child += run.values[child].next) {
        channels = channels && isChannel(&run.values[child]);
        ++count;
    }
    switch (value->kind) {
        case PrCssValueKind_Dimension:
            return count == 0 || (value->unit == PrCssUnit_Unknown && count == 1 &&
                run.values[index + 1].kind == PrCssValueKind_Ident);
        case PrCssValueKind_Color:
            return count == 4 && channels;
        case PrCssValueKind_Math:
            return count == 1;
        case PrCssValueKind_Calc: {
            PrCssCalc op = cssValueCalcOperator(value);
            return op == PrCssCalc_Negate || op == PrCssCalc_Invert ? count == 1 : count != 0;
        }
        case PrCssValueKind_Function:
        case PrCssValueKind_Longhand:
        case PrCssValueKind_Production:
            return true;
        default:
            return count == 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Given values

void cssValueTokens(CssTokenArray* tokens, TextBuffer* source, CssValueRun run) {
    // A value is one token, except a function, which is two tokens around its arguments, and a color, which is at most
    // four around its channels.
    uint32_t entries = 3 * run.count + 1;
    tokens->starts.prepare(entries);
    tokens->ends.prepare(entries);
    tokens->kinds.prepare(entries);
    tokens->flags.prepare(entries);
    tokens->units.prepare(entries);
    tokens->closers.prepare(entries);
    tokens->starts.commitTo(entries);
    tokens->ends.commitTo(entries);
    tokens->kinds.commitTo(entries);
    tokens->flags.commitTo(entries);
    tokens->units.commitTo(entries);
    tokens->closers.commitTo(entries);
    textClear(source);

    TokenWriter w = {tokens, source, run, 0};
    for (uint32_t i = 0; i < run.count; i += run.values[i].next)
        writeTree(&w, i);

    uint32_t count = w.count;
    uint32_t size = source->text.count;
    tokens->starts.data[count] = size;
    tokens->ends.data[count] = size;
    tokens->kinds.data[count] = CssTokenKind_Eof;
    tokens->flags.data[count] = 0;
    tokens->units.data[count] = PrCssUnit_Number;
    tokens->closers.data[count] = count;
    tokens->starts.count = count;
    tokens->ends.count = count;
    tokens->kinds.count = count;
    tokens->flags.count = count;
    tokens->units.count = count;
    tokens->closers.count = count;
    tokens->source = textView(source);
    tokens->values = run.values;
    tokens->valueText = run.text;
}

} // namespace pr_impl
