#include "css/src/css_syntax_definition.h"

#include "pr/string_ops.h"

#include "css/src/css_parser.h"

namespace pr_impl {
namespace {

// The data type names that a syntax component may use (css-properties-values-api-1/#supported-names).
struct SupportedName {
    PrStringView name;
    CssValueDef type;
};

constexpr SupportedName kSupportedNames[] = {
    {"angle", CssValueDef_Angle},
    {"color", CssValueDef_Color},
    {"custom-ident", CssValueDef_CustomIdent},
    {"image", CssValueDef_Image},
    {"integer", CssValueDef_Integer},
    {"length", CssValueDef_Length},
    {"length-percentage", CssValueDef_LengthPercentage},
    {"number", CssValueDef_Number},
    {"percentage", CssValueDef_Percentage},
    {"resolution", CssValueDef_Resolution},
    {"string", CssValueDef_String},
    {"time", CssValueDef_Time},
    {"url", CssValueDef_Url},
    {"transform-function", CssValueDef_TransformFunction},
    {"transform-list", CssValueDef_TransformList},
};

// Returns true if a token follows the one before it with nothing between them, not even a comment.
bool adjacent(const CssTokenArray* tokens, uint32_t index) {
    return (cssTokenFlags(tokens, index) & CssTokenFlag_Contiguous) != 0;
}

bool isDelim(const CssTokenArray* tokens, uint32_t index, uint32_t codePoint) {
    return cssTokenKind(tokens, index) == CssTokenKind_Delim && cssTokenDelim(tokens, index) == codePoint;
}

// Consumes a data type name from the '<' at `*pos` (css-properties-values-api-1/#consume-a-data-type-name).
bool consumeDataTypeName(const CssTokenArray* tokens, uint32_t* pos, uint32_t end, CssValueDef* out) {
    uint32_t at = *pos + 1;
    if (at + 1 >= end || cssTokenKind(tokens, at) != CssTokenKind_Ident || !adjacent(tokens, at) ||
            (cssTokenFlags(tokens, at) & CssTokenFlag_PlainText) == 0 ||
            !isDelim(tokens, at + 1, '>') || !adjacent(tokens, at + 1))
        return false;
    PrStringView name = cssTokenValueText(tokens, at);
    for (const SupportedName& supported : kSupportedNames) {
        if (name == supported.name) {
            *out = supported.type;
            *pos = at + 2;
            return true;
        }
    }
    return false;
}

// Consumes a syntax component at `*pos` (css-properties-values-api-1/#consume-a-syntax-component).
bool consumeComponent(const CssTokenArray* tokens, uint32_t* pos, uint32_t end, CssSyntaxDefinition* out) {
    CssSyntaxComponent component = {CssValueDef_Count, CssSyntaxMultiplier_None, 0, 0};
    if (isDelim(tokens, *pos, '<')) {
        if (!consumeDataTypeName(tokens, pos, end, &component.type))
            return false;
    } else if (cssTokenKind(tokens, *pos) == CssTokenKind_Ident) {
        // A CSS-wide keyword and `default` cannot name a component.
        PrCssKeyword keyword = cssTokenKeyword(tokens, *pos);
        if (cssIsCssWideKeyword(keyword) || keyword == PrCssKeyword_Default)
            return false;
        component.identStart = out->idents.size;
        cssValueAppend(&out->idents, cssTokenValueText(tokens, *pos));
        component.identSize = out->idents.size - component.identStart;
        ++*pos;
    } else {
        return false;
    }
    if (*pos < end && adjacent(tokens, *pos) && (isDelim(tokens, *pos, '+') || isDelim(tokens, *pos, '#'))) {
        // A list type is already multiplied (css-properties-values-api-1/#pre-multiplied-data-type-name).
        if (component.type == CssValueDef_TransformList)
            return false;
        component.multiplier = isDelim(tokens, *pos, '+') ? CssSyntaxMultiplier_Space : CssSyntaxMultiplier_Comma;
        ++*pos;
    }
    out->components.push(component);
    return true;
}

// Returns true if the tokens of `span` are a value that does not depend on the element for any registration. Such a
// value has no substitution function, and no length relative to a font or a container.
bool computationallyIndependent(const CssTokenArray* tokens, CssTokenSpan span) {
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        CssTokenKind kind = cssTokenKind(tokens, i);
        if (kind == CssTokenKind_Function) {
            PrCssKeyword keyword = cssTokenKeyword(tokens, i);
            if (keyword == PrCssKeyword_Var || keyword == PrCssKeyword_Attr)
                return false;
        } else if (kind == CssTokenKind_Dimension) {
            PrCssUnit unit = cssTokenUnit(tokens, i);
            if ((unit >= PrCssUnit_Em && unit <= PrCssUnit_Rlh) || (unit >= PrCssUnit_Cqw && unit <= PrCssUnit_Cqmax))
                return false;
        }
    }
    return true;
}

// Returns true if the whole of `span` is one `component`, matched once.
bool matchesOnce(CssParser* parser, const CssSyntaxDefinition* definition,
        const CssSyntaxComponent& component, CssTokenSpan span) {
    const CssTokenArray* tokens = &parser->tokens;
    if (span.count == 0)
        return false;
    if (component.type == CssValueDef_Count) {
        if (span.count != 1 || cssTokenKind(tokens, span.start) != CssTokenKind_Ident)
            return false;
        PrTextBuffer value = {};
        cssValueAppend(&value, cssTokenValueText(tokens, span.start));
        PrStringView ident = {definition->idents.data + component.identStart, component.identSize};
        bool equal = prTextBufferView(&value) == ident;
        prTextBufferReset(&value);
        return equal;
    }
    CssTokenStream stream = {tokens, span.start, span.start + span.count};
    CssParseContext context = {nullptr};
    uint32_t values = parser->pool.values.count;
    uint32_t text = parser->pool.text.count;
    CssValueSpan out = {};
    bool matched = cssParseProduction(component.type, &stream, &context, &parser->pool, &parser->match, &out);
    parser->pool.values.count = values;
    parser->pool.text.count = text;
    return matched;
}

// Returns true if the whole of `span` matches `component` under its multiplier.
bool matches(CssParser* parser, const CssSyntaxDefinition* definition, const CssSyntaxComponent& component, CssTokenSpan span) {
    const CssTokenArray* tokens = &parser->tokens;
    if (component.multiplier == CssSyntaxMultiplier_None)
        return matchesOnce(parser, definition, component, span);
    CssTokenStream ts = {tokens, span.start, span.start + span.count};
    bool any = false;
    while (true) {
        cssSkipWhitespace(&ts);
        if (ts.pos >= ts.end)
            return any;
        CssTokenSpan item = {};
        if (component.multiplier == CssSyntaxMultiplier_Space) {
            if (!cssConsumeComponentValue(&ts, &item))
                return false;
        } else {
            uint32_t comma = ts.end;
            if (!cssNextComma(&ts, &comma))
                comma = ts.end;
            item = cssTrimWhitespace(tokens, CssTokenSpan{ts.pos, comma - ts.pos});
            ts.pos = comma;
            if (comma < ts.end) {
                ++ts.pos;
                cssSkipWhitespace(&ts);
                if (ts.pos >= ts.end)
                    return false;
            }
        }
        if (!matchesOnce(parser, definition, component, item))
            return false;
        any = true;
    }
}

} // namespace

void cssSyntaxDefinitionReset(CssSyntaxDefinition* definition) {
    definition->components.reset();
    prTextBufferReset(&definition->idents);
    definition->universal = false;
}

bool cssSyntaxDefinitionParse(CssParser* parser, PrStringView text, CssSyntaxDefinition* out) {
    cssSyntaxDefinitionReset(out);
    cssTokenize(&parser->tokens, &parser->scratch, text);
    const CssTokenArray* tokens = &parser->tokens;
    CssTokenSpan span = cssTrimWhitespace(tokens, CssTokenSpan{0, cssTokenCount(tokens)});
    if (span.count == 0)
        return false;
    if (span.count == 1 && isDelim(tokens, span.start, '*')) {
        out->universal = true;
        return true;
    }
    uint32_t pos = span.start;
    uint32_t end = span.start + span.count;
    for (;;) {
        if (!consumeComponent(tokens, &pos, end, out))
            break;
        if (pos < end && cssTokenKind(tokens, pos) == CssTokenKind_Whitespace)
            ++pos;
        if (pos == end)
            return true;
        if (!isDelim(tokens, pos, '|'))
            break;
        ++pos;
        if (pos < end && cssTokenKind(tokens, pos) == CssTokenKind_Whitespace)
            ++pos;
        if (pos == end)
            break;
    }
    cssSyntaxDefinitionReset(out);
    return false;
}

bool cssSyntaxInitialValueValid(CssParser* parser, const CssSyntaxDefinition* definition, PrStringView value) {
    cssTokenize(&parser->tokens, &parser->scratch, value);
    const CssTokenArray* tokens = &parser->tokens;
    CssTokenSpan span = cssTrimWhitespace(tokens, CssTokenSpan{0, cssTokenCount(tokens)});
    return computationallyIndependent(tokens, span) && cssSyntaxValueMatches(parser, definition, value);
}

bool cssSyntaxValueMatches(CssParser* parser, const CssSyntaxDefinition* definition, PrStringView value) {
    cssTokenize(&parser->tokens, &parser->scratch, value);
    const CssTokenArray* tokens = &parser->tokens;
    CssTokenSpan span = cssTrimWhitespace(tokens, CssTokenSpan{0, cssTokenCount(tokens)});
    if (span.count == 1 && cssTokenKind(tokens, span.start) == CssTokenKind_Ident &&
            cssIsCssWideKeyword(cssTokenKeyword(tokens, span.start)))
        return false;
    if (definition->universal) {
        // css-syntax-3/#typedef-declaration-value, with nothing left over and no bad token.
        CssTokenStream ts = {tokens, span.start, span.start + span.count};
        CssTokenSpan consumed = {};
        if (span.count != 0 && (!cssConsumeDeclarationValue(&ts, &consumed) || ts.pos != ts.end))
            return false;
        for (uint32_t i = span.start; i < span.start + span.count; ++i) {
            CssTokenKind kind = cssTokenKind(tokens, i);
            if (kind == CssTokenKind_BadString || kind == CssTokenKind_BadUrl ||
                    (cssTokenFlags(tokens, i) & CssTokenFlag_Unmatched) != 0)
                return false;
        }
        return true;
    }
    for (uint32_t i = 0; i < definition->components.count; ++i) {
        if (matches(parser, definition, definition->components.data[i], span))
            return true;
    }
    return false;
}

} // namespace pr_impl
