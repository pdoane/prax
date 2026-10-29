// Rules and Declarations
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/

#pragma once

#include "css/src/css_syntax.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

// A declaration, such as `color: red !important`.
struct CssSyntaxDeclaration {
    uint32_t name;        // the ident token that names the property
    CssTokenSpan value;   // the component values, with the whitespace and !important removed
    bool important;
};

// ---------------------------------------------------------------------------------------------------------------------
// Rules

// The two forms of rule.
enum {
    CssSyntaxRuleType_Qualified,   // a prelude and a block: `a > b { ... }`
    CssSyntaxRuleType_At,          // an at-keyword, a prelude, and a block or a semicolon
};
typedef uint8_t CssSyntaxRuleType;

// The properties of one rule.
enum {
    CssSyntaxRuleFlag_EofTerminated = 1 << 0, // ended at the end of input, not at its own terminator
    CssSyntaxRuleFlag_HasBlock = 1 << 1,
};
typedef uint8_t CssSyntaxRuleFlags;

// A rule, as spans of its tokens.
struct CssSyntaxRule {
    CssSyntaxRuleType type;
    CssSyntaxRuleFlags flags;
    uint32_t name;          // an at-rule's at-keyword token
    CssTokenSpan prelude;   // everything before the block, as written
    CssTokenSpan block;     // the block's contents, braces excluded
};

// ---------------------------------------------------------------------------------------------------------------------
// Block contents
//
// A block holds declarations and rules interleaved, so their order is part of the result.

// The type of one item of a block.
enum {
    CssItemType_Declaration,
    CssItemType_Rule,
    CssItemType_Invalid,   // tokens that did not parse
};
typedef uint8_t CssItemType;

// What a block may hold (css-syntax-3/#block-contents). Each production that names a block in a grammar is a
// combination of the first three.
enum {
    CssContents_Declarations = 1 << 0,
    CssContents_QualifiedRules = 1 << 1,
    CssContents_AtRules = 1 << 2,

    CssContents_DeclarationList = CssContents_Declarations,                            // <declaration-list>
    CssContents_QualifiedRuleList = CssContents_QualifiedRules,                        // <qualified-rule-list>
    CssContents_AtRuleList = CssContents_AtRules,                                      // <at-rule-list>
    CssContents_DeclarationRuleList = CssContents_Declarations | CssContents_AtRules,  // <declaration-rule-list>
    CssContents_RuleList = CssContents_QualifiedRules | CssContents_AtRules,           // <rule-list>

    // <block-contents>
    CssContents_BlockContents = CssContents_Declarations | CssContents_QualifiedRules | CssContents_AtRules,
};
typedef uint8_t CssContents;

// One item of a block, in written order.
struct CssItem {
    CssItemType type;
    uint32_t index;     // index into the parser's declarations or rules, unless invalid
    CssTokenSpan span;  // the tokens that the item covers
};

// A run of one of the parser's arrays.
struct CssRange {
    uint32_t start;
    uint32_t count;
};

// The result of parsing a block, as runs of the parser's arrays. `items` records the order in which the declarations
// and rules were written.
struct CssBlock {
    CssRange items;
    CssRange declarations;
    CssRange rules;
};

// The state of a parse (css_parser.h).
struct CssParser;

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

// Parses one declaration from everything that is left (css-syntax-3/#parse-declaration). Returns false for anything
// that is not a declaration.
bool cssParseDeclaration(CssTokenStream* ts, CssSyntaxDeclaration* out);

// Parses the declarations and rules that a block holds (css-syntax-3/#parse-block-contents), in written order. Tokens
// that fail to parse become a CssItemType_Invalid item rather than nothing. A rule's own block stays a token span, and
// parsing a nested block takes a second call.
void cssParseBlockContents(CssTokenStream* ts, CssParser* parser, CssBlock* out);

// Parses one rule (css-syntax-3/#parse-rule). Returns false if the tokens that follow are not a rule at all. The tokens
// after the rule are left in the stream.
bool cssParseRule(CssTokenStream* ts, CssSyntaxRule* out);

// Parses every rule in the stream (css-syntax-3/#parse-stylesheet-contents). CDO and CDC tokens between rules are
// discarded.
void cssParseStylesheetContents(CssTokenStream* ts, CssParser* parser, CssBlock* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
