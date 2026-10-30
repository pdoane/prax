#include "css/src/css_syntax_rule.h"

#include "css/src/css_parser.h"

namespace pr_impl {
namespace {

bool isWhitespaceToken(const CssTokenStream* ts, uint32_t index) {
    return cssTokenKind(ts->tokens, index) == CssTokenKind_Whitespace;
}

// Returns the span from `start` to the stream's current position.
CssTokenSpan since(const CssTokenStream* ts, uint32_t start) {
    return CssTokenSpan{start, ts->pos - start};
}

// Consumes a block and returns the span that its brackets enclose (css-syntax-3/#consume-block). The stream must be on
// the opener.
CssTokenSpan consumeBlock(CssTokenStream* ts, bool* eofTerminated) {
    uint32_t opener = ts->pos;
    uint32_t closer = cssTokenCloser(ts->tokens, opener);

    *eofTerminated = closer >= ts->end;
    ts->pos = *eofTerminated ? ts->end : closer + 1;

    uint32_t end = *eofTerminated ? ts->pos : ts->pos - 1;
    return CssTokenSpan{opener + 1, end - opener - 1};
}

// Consumes an at-rule whose at-keyword has already been consumed (css-syntax-3/#consume-at-rule).
CssSyntaxRule consumeAtRule(CssTokenStream* ts, uint32_t name, bool nested) {
    CssSyntaxRule rule = {};
    rule.type = CssSyntaxRuleType_At;
    rule.name = name;

    uint32_t start = ts->pos;
    for (;;) {
        if (cssPeek(ts) == CssTokenKind_Eof || (cssPeek(ts) == CssTokenKind_CloseBrace && nested)) {
            rule.flags |= CssSyntaxRuleFlag_EofTerminated;
            break;
        }
        if (cssPeek(ts) == CssTokenKind_Semicolon) {
            rule.prelude = since(ts, start);
            ++ts->pos;
            return rule;
        }
        if (cssPeek(ts) == CssTokenKind_OpenBrace) {
            rule.prelude = since(ts, start);
            bool eofTerminated = false;
            rule.block = consumeBlock(ts, &eofTerminated);
            rule.flags |= CssSyntaxRuleFlag_HasBlock;
            if (eofTerminated)
                rule.flags |= CssSyntaxRuleFlag_EofTerminated;
            return rule;
        }
        CssTokenSpan ignored;
        cssConsumeComponentValue(ts, &ignored);
    }

    rule.prelude = since(ts, start);
    return rule;
}

// Returns true if a prelude starts in the same way as a custom property declaration: `--foo :`.
bool preludeLooksLikeDeclaration(const CssTokenStream* ts, CssTokenSpan prelude) {
    CssTokenSpan span = cssTrimWhitespace(ts->tokens, prelude);
    if (span.count == 0 || !cssTokenIsDashedIdent(ts->tokens, span.start))
        return false;

    for (uint32_t i = span.start + 1; i < span.start + span.count; ++i) {
        if (isWhitespaceToken(ts, i))
            continue;
        return cssTokenKind(ts->tokens, i) == CssTokenKind_Colon;
    }
    return false;
}

// Skips what is left of a declaration that did not parse (css-syntax-3/#consume-declaration).
void consumeBadDeclarationRemnants(CssTokenStream* ts, bool nested) {
    for (;;) {
        if (cssPeek(ts) == CssTokenKind_Eof)
            return;
        if (cssPeek(ts) == CssTokenKind_Semicolon) {
            ++ts->pos;
            return;
        }
        if (cssPeek(ts) == CssTokenKind_CloseBrace) {
            if (nested)
                return;
            ++ts->pos;
            continue;
        }
        CssTokenSpan ignored;
        cssConsumeComponentValue(ts, &ignored);
    }
}

// Consumes a qualified rule (css-syntax-3/#consume-qualified-rule). A qualified rule needs a block. `stop` ends the
// rule without one. A `stop` of CssTokenKind_Eof means that there is no stop token.
bool consumeQualifiedRule(CssTokenStream* ts, bool nested, CssTokenKind stop, CssSyntaxRule* out) {
    CssSyntaxRule rule = {};
    rule.type = CssSyntaxRuleType_Qualified;

    uint32_t start = ts->pos;
    for (;;) {
        CssTokenKind kind = cssPeek(ts);
        if (kind == CssTokenKind_Eof || kind == stop)
            return false;
        if (cssPeek(ts) == CssTokenKind_CloseBrace) {
            if (nested)
                return false;
            ++ts->pos;   // part of the prelude, and a parse error
            continue;
        }
        if (cssPeek(ts) == CssTokenKind_OpenBrace) {
            rule.prelude = since(ts, start);

            // A prelude shaped like a custom property declaration is consumed as one.
            if (preludeLooksLikeDeclaration(ts, rule.prelude)) {
                if (nested) {
                    consumeBadDeclarationRemnants(ts, true);
                } else {
                    bool ignored = false;
                    consumeBlock(ts, &ignored);
                }
                return false;
            }

            bool eofTerminated = false;
            rule.block = consumeBlock(ts, &eofTerminated);
            rule.flags |= CssSyntaxRuleFlag_HasBlock;
            if (eofTerminated)
                rule.flags |= CssSyntaxRuleFlag_EofTerminated;
            *out = rule;
            return true;
        }
        CssTokenSpan ignored;
        cssConsumeComponentValue(ts, &ignored);
    }
}

// Removes the `!important` at the end of a declaration's value, if present. Returns the span without it.
CssTokenSpan takeImportant(const CssTokenStream* ts, CssTokenSpan value, bool* important) {
    *important = false;

    // The last two tokens that are not whitespace: `!` and `important`.
    uint32_t end = value.start + value.count;
    uint32_t last = end;
    while (last > value.start && isWhitespaceToken(ts, last - 1))
        --last;
    if (last == value.start)
        return value;

    uint32_t keyword = last - 1;
    if (cssTokenKind(ts->tokens, keyword) != CssTokenKind_Ident ||
            cssTokenKeyword(ts->tokens, keyword) != PrCssKeyword_Important) {
        return value;
    }

    uint32_t bang = keyword;
    while (bang > value.start && isWhitespaceToken(ts, bang - 1))
        --bang;
    if (bang == value.start)
        return value;
    --bang;

    if (cssTokenKind(ts->tokens, bang) != CssTokenKind_Delim || cssTokenDelim(ts->tokens, bang) != '!')
        return value;

    *important = true;
    return cssTrimWhitespace(ts->tokens, CssTokenSpan{value.start, bang - value.start});
}

// Returns true if a value holds a top-level {}-block alongside anything else. css-syntax-3/#consume-declaration allows
// this only for a custom property.
bool hasPartialBlock(const CssTokenStream* ts, CssTokenSpan value) {
    CssTokenStream scan = cssSubStream(ts, value);
    bool block = false;
    uint32_t others = 0;

    CssTokenSpan component;
    while (cssConsumeComponentValue(&scan, &component)) {
        CssTokenKind type = cssTokenKind(ts->tokens, component.start);
        if (type == CssTokenKind_Whitespace)
            continue;
        if (type == CssTokenKind_OpenBrace)
            block = true;
        else
            ++others;
    }
    return block && others != 0;
}

// Appends an item to the parser and counts it as part of the block, together with whatever the declaration or rule that
// it names added.
void addItem(CssParser* parser, CssBlock* block, const CssItem& item) {
    parser->items.push(item);
    block->items.count = parser->items.count - block->items.start;
    block->declarations.count = parser->declarations.count - block->declarations.start;
    block->rules.count = parser->rules.count - block->rules.start;
}

// Consumes a declaration (css-syntax-3/#consume-declaration). On failure, the stream may be anywhere in the
// declaration, not past the remnants.
bool consumeDeclaration(CssTokenStream* ts, bool nested, CssSyntaxDeclaration* out) {
    CssSyntaxDeclaration declaration = {};

    if (cssPeek(ts) != CssTokenKind_Ident)
        return false;
    declaration.name = ts->pos;
    ++ts->pos;

    cssSkipWhitespace(ts);
    if (cssPeek(ts) != CssTokenKind_Colon)
        return false;
    ++ts->pos;
    cssSkipWhitespace(ts);

    // css-syntax-3/#consume-list-of-components, with <semicolon-token> as the stop token. A property's value that holds
    // a {}-block after anything else is not a declaration, whatever follows. The list stops at the block.
    bool custom = cssTokenIsDashedIdent(ts->tokens, declaration.name);
    bool written = false;     // true once a component that is not whitespace has been read
    uint32_t start = ts->pos;
    for (;;) {
        CssTokenKind kind = cssPeek(ts);
        if (kind == CssTokenKind_Eof || kind == CssTokenKind_Semicolon)
            break;
        if (kind == CssTokenKind_CloseBrace && nested)
            break;
        if (kind == CssTokenKind_OpenBrace && written && !custom)
            return false;
        written = written || kind != CssTokenKind_Whitespace;
        CssTokenSpan ignored;
        cssConsumeComponentValue(ts, &ignored);
    }

    CssTokenSpan value = cssTrimWhitespace(ts->tokens, since(ts, start));
    declaration.value = takeImportant(ts, value, &declaration.important);

    if (!custom && hasPartialBlock(ts, declaration.value))
        return false;

    *out = declaration;
    return true;
}

} // namespace

bool cssParseDeclaration(CssTokenStream* ts, CssSyntaxDeclaration* out) {
    cssSkipWhitespace(ts);
    return consumeDeclaration(ts, false, out);
}

void cssParseBlockContents(CssTokenStream* ts, CssParser* parser, CssBlock* out) {
    *out = CssBlock{{parser->items.count, 0}, {parser->declarations.count, 0}, {parser->rules.count, 0}};

    for (;;) {
        CssTokenKind kind = cssPeek(ts);
        if (kind == CssTokenKind_Eof || kind == CssTokenKind_CloseBrace)
            return;
        if (kind == CssTokenKind_Whitespace || kind == CssTokenKind_Semicolon) {
            ++ts->pos;
            continue;
        }

        uint32_t start = ts->pos;
        if (cssPeek(ts) == CssTokenKind_AtKeyword) {
            uint32_t name = ts->pos;
            ++ts->pos;
            // The rule's own rules come first, so its index is read after it is consumed.
            CssSyntaxRule rule = consumeAtRule(ts, name, true);
            uint32_t index = parser->rules.count;
            parser->rules.push(rule);
            addItem(parser, out, CssItem{CssItemType_Rule, index, since(ts, start)});
            continue;
        }

        CssSyntaxDeclaration declaration;
        if (consumeDeclaration(ts, true, &declaration)) {
            uint32_t index = parser->declarations.count;
            parser->declarations.push(declaration);
            addItem(parser, out, CssItem{CssItemType_Declaration, index, since(ts, start)});
            continue;
        }

        // Not a declaration, so the same tokens are read again as a rule.
        ts->pos = start;
        CssSyntaxRule rule;
        if (consumeQualifiedRule(ts, true, CssTokenKind_Semicolon, &rule)) {
            uint32_t index = parser->rules.count;
            parser->rules.push(rule);
            addItem(parser, out, CssItem{CssItemType_Rule, index, since(ts, start)});
        } else {
            addItem(parser, out, CssItem{CssItemType_Invalid, 0, since(ts, start)});
        }
    }
}

bool cssParseRule(CssTokenStream* ts, CssSyntaxRule* out) {
    cssSkipWhitespace(ts);

    if (cssPeek(ts) == CssTokenKind_Eof)
        return false;

    if (cssPeek(ts) == CssTokenKind_AtKeyword) {
        uint32_t name = ts->pos;
        ++ts->pos;
        *out = consumeAtRule(ts, name, false);
        return true;
    }
    return consumeQualifiedRule(ts, false, CssTokenKind_Eof, out);
}

void cssParseStylesheetContents(CssTokenStream* ts, CssParser* parser, CssBlock* out) {
    *out = CssBlock{{parser->items.count, 0}, {parser->declarations.count, 0}, {parser->rules.count, 0}};

    for (;;) {
        if (cssPeek(ts) == CssTokenKind_Eof)
            return;
        CssTokenKind kind = cssPeek(ts);
        if (kind == CssTokenKind_Whitespace || kind == CssTokenKind_Cdo || cssPeek(ts) == CssTokenKind_Cdc) {
            ++ts->pos;
            continue;
        }

        uint32_t start = ts->pos;
        if (cssPeek(ts) == CssTokenKind_AtKeyword) {
            uint32_t name = ts->pos;
            ++ts->pos;
            CssSyntaxRule rule = consumeAtRule(ts, name, false);
            uint32_t index = parser->rules.count;
            parser->rules.push(rule);
            addItem(parser, out, CssItem{CssItemType_Rule, index, since(ts, start)});
            continue;
        }

        CssSyntaxRule rule;
        if (consumeQualifiedRule(ts, false, CssTokenKind_Eof, &rule)) {
            uint32_t index = parser->rules.count;
            parser->rules.push(rule);
            addItem(parser, out, CssItem{CssItemType_Rule, index, since(ts, start)});
        } else {
            addItem(parser, out, CssItem{CssItemType_Invalid, 0, since(ts, start)});
        }
    }
}

} // namespace pr_impl
