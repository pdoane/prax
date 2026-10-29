// Parsers
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/
//
// A parser holds the arrays that a parse works in. They are reset by count between parses rather than freed, so they
// grow to the largest parse and are reused from then on.

#pragma once

#include "pr/css.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "css/src/css_parse.h"
#include "css/src/css_rule.h"
#include "css/src/css_selector.h"
#include "css/src/css_syntax_rule.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The memory that a parse runs on.
struct CssParser {
    CssTokenScratch scratch;                 // data that the tokenizer stores alongside the tokens
    CssTokenArray tokens;                    // tokens of the source

    // A text tokenized separately from the source: a value that is read again with unicode ranges allowed
    // (css-syntax-3/#parse-list-of-component-values), or the initial value of a longhand.
    CssTokenScratch asideScratch;
    CssTokenArray asideTokens;

    // The tokens of a value given rather than written (cssValueTokens), and the bytes of their text.
    CssTokenArray givenTokens;
    TextBuffer givenSource;

    // The arrays whose runs a CssBlock names.
    Array<CssItem> items;
    Array<CssSyntaxDeclaration> declarations;
    Array<CssSyntaxRule> rules;

    CssValuePool pool;                       // the values that a match produces
    CssMatchState match;                     // the frames and choices that a match runs on
    CssSelectorPool selectors;               // the records that a selector list compiles to

    // The declarations being added to a block. Their values and names are in `pool`.
    Array<CssDeclaration> declared;

    // The declarations that one declaration makes: itself, or the longhands of a shorthand.
    Array<CssDeclaration> incoming;

    Array<CssValueSpan> expanded;            // the values of a shorthand's longhands, in `pool`
    CssValuePool initials;                   // the initial values of longhands, parsed on first use
    Array<CssValueSpan> initialOf;           // each property's initial value in `initials`, empty until parsed
    Array<uint64_t> filter;                  // the names that a block already holds

    // The name of a declaration or an at-rule, with escapes resolved.
    TextBuffer names;
};

// Initializes `parser` as empty, whatever it held before.
void cssParserInit(CssParser* parser);

// The parser that a parse runs on. It is the parser that a slot keeps, created on first use, or the scope's own parser
// if there is no slot. `parser` may point to `local`, so a scope must not move after it is begun.
struct CssParserScope {
    CssParserSlot slot;
    CssParser* parser;
    CssParser local;
};

// Begins a scope on the parser that `slot` keeps, creating the parser if the slot is empty. If the slot is null, the
// scope uses its own parser.
void cssParserBegin(CssParserScope* scope, CssParserSlot slot);

// Releases the scope's own parser. Also releases a slot's parser if the source it last held exceeded kVmWorthwhile. The
// released parser stays usable.
void cssParserEnd(CssParserScope* scope);

// Tokenizes `text` into the parser's tokens, and returns a stream over all of them.
CssTokenStream cssParserTokenize(CssParser* parser, PrStringView text);

// Frees everything the parser owns. The parser stays usable.
void cssParserReset(CssParser* parser);

// Drops the elements that `block` names. They are the last elements of the arrays.
void cssParserDrop(CssParser* parser, CssBlock block);

// The kind of change that a prepared edit stages (css_rule.h, "Edits").
enum {
    CssEditKind_None,
    CssEditKind_Set,         // the parser's `incoming`, set in a block
    CssEditKind_Remove,      // `name`, removed from a block
    CssEditKind_Selectors,   // `selectors`, for a style rule
    CssEditKind_Prelude,     // `staged`, a rule's prelude
    CssEditKind_Media,       // `staged`, a media list's queries
    CssEditKind_Insert,      // `rule`, inserted at `index`
};
typedef uint8_t CssEditKind;

// A change to parsed CSS, prepared against the data that it changes. The edit holds what it stages in that data and in
// its parser until it is committed or ended. Ending the edit restores the parser's arrays.
struct CssEdit {
    CssParserScope scope;
    CssEditKind kind;
    CssDeclaredName name;
    CssValueSpan staged;                   // in the parser's pool

    // The selectors of a style rule, or the selectors that a prelude holds (cssCompilePreludeSelectors). The edit owns
    // them until it is committed.
    PrCssSelectors* PR_NULLABLE selectors;

    CssRuleData* PR_NULLABLE rule;         // owned by the edit until it is committed
    uint32_t index;                        // index at which `rule` is inserted
    uint32_t values;                       // number of values in the parser's pool when the edit began
    uint32_t text;                         // size of the text in the parser's pool when the edit began
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
