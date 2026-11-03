#include "css/src/css_parser.h"

#include "pr/alloc.h"
#include "pr/reserved_array.h"

namespace pr_impl {

void cssParserInit(CssParser* parser) {
    *parser = CssParser{};
}

void cssParserBegin(CssParserScope* scope, CssParserSlot slot) {
    scope->slot = slot;
    if (slot == nullptr) {
        cssParserInit(&scope->local);
        scope->parser = &scope->local;
        return;
    }
    if (*slot == nullptr) {
        *slot = alloc<CssParser>();
        cssParserInit(*slot);
    }
    scope->parser = *slot;
}

void cssParserEnd(CssParserScope* scope) {
    if (scope->slot == nullptr || scope->parser->tokens.source.size > kVmWorthwhile)
        cssParserReset(scope->parser);
}

CssTokenStream cssParserTokenize(CssParser* parser, PrStringView text) {
    cssTokenize(&parser->tokens, &parser->scratch, text);
    return cssStream(&parser->tokens);
}

void cssParserReset(CssParser* parser) {
    cssTokenArrayReset(&parser->tokens);
    cssTokenScratchReset(&parser->scratch);
    cssTokenArrayReset(&parser->asideTokens);
    cssTokenScratchReset(&parser->asideScratch);
    cssTokenArrayReset(&parser->givenTokens);
    prTextBufferReset(&parser->givenSource);

    parser->items.reset();
    parser->declarations.reset();
    parser->rules.reset();

    cssValuePoolReset(&parser->pool);
    cssMatchStateReset(&parser->match);
    cssSelectorPoolReset(&parser->selectors);
    parser->declared.reset();
    parser->incoming.reset();
    parser->expanded.reset();
    cssValuePoolReset(&parser->initials);
    parser->initialOf.reset();
    parser->filter.reset();

    prTextBufferReset(&parser->names);
}

void cssParserDrop(CssParser* parser, CssBlock block) {
    parser->items.count = block.items.start;
    parser->declarations.count = block.declarations.start;
    parser->rules.count = block.rules.start;
}

} // namespace pr_impl
