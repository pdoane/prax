// Parsing
//
// css-color-4 - https://drafts.csswg.org/css-color-4/
// css-color-5 - https://drafts.csswg.org/css-color-5/
// css-fonts-4 - https://drafts.csswg.org/css-fonts-4/
// css-images-4 - https://drafts.csswg.org/css-images-4/
// css-values-4 - https://drafts.csswg.org/css-values-4/
// selectors-4 - https://drafts.csswg.org/selectors-4/
//
// One matcher runs every grammar that webref provides. A property's value is flattened to the values that it matched.
// Inside a structural production, such as a selector, a prelude or a query, the meaning of a token depends on the
// production that matched it. There, every type with a grammar keeps a node that names it, and every literal keeps a
// value. A rule that a spec states in prose is implemented as a terminal's parser, or as a check that a production's
// match must pass.

#pragma once

#include "css/src/css_at_rule.h"
#include "css/src/css_grammar.h"
#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "css/src/css_property.h"
#include "css/src/css_pseudo.h"
#include "css/src/css_syntax.h"
#include "css/src/css_terminal.h"
#include "css/src/css_value.h"
#include "css/src/css_value_def.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A sheet's parsed rules (css_rule.h).
struct CssSheetData;

// The inputs that matching a structural production needs besides its tokens.
struct CssParseContext {
    // The sheet whose @namespace rules declare the prefixes that a selector uses (css-namespaces-3/#declaration). If
    // null, no prefixes are declared.
    const CssSheetData* PR_NULLABLE sheet;
};

// ---------------------------------------------------------------------------------------------------------------------
// Matching
//
// The matcher runs its frames in a loop rather than on the C stack, so the state holds them. The frames grow to the
// longest value seen, and are reused from then on.

// What a frame matches.
enum {
    CssMatchFrameKind_Siblings,     // the children of a sequence, in order
    CssMatchFrameKind_Repeat,       // one node under its multiplier
    CssMatchFrameKind_OneOf,        // the alternatives of a |, in order
    CssMatchFrameKind_AnyOrder,     // the children of a || or &&, in any order
    CssMatchFrameKind_Item,         // the end of a list item, matched against a narrowed stream
    CssMatchFrameKind_Function,     // the ')' closing a function, and its span over its arguments
    CssMatchFrameKind_Production,   // the end of a structural production, and its node
    CssMatchFrameKind_Longhand,     // the end of a shorthand's component, and its node
    CssMatchFrameKind_Fold,         // the end of a production that a reference folds, and its start
    CssMatchFrameKind_End,          // the whole stream, which the outermost frame requires
};
typedef uint8_t CssMatchFrameKind;

// The state of a frame. The comment of a flag that only one kind of frame uses starts with that kind.
enum {
    CssMatchFlag_Produced = 1 << 0,      // Siblings: an item has matched
    CssMatchFlag_SinceComma = 1 << 1,    // Siblings: an item has matched since the last comma
    CssMatchFlag_CommaOmitted = 1 << 2,  // Siblings: a comma was dropped
    CssMatchFlag_CommaTaken = 1 << 3,    // Siblings: a comma matched
    CssMatchFlag_Structural = 1 << 4,    // the frame is inside a structural production, or matches one
    CssMatchFlag_Required = 1 << 5,      // Siblings: a ! group, which must produce a value
    CssMatchFlag_SpaceFirst = 1 << 6,    // Production: whitespace may come before the first component

    // AnyOrder: a child was taken after a child that the grammar writes after it.
    CssMatchFlag_Unordered = 1 << 7,
};
typedef uint8_t CssMatchFlags;

// What remains to match after the node being tried.
struct CssMatchFrame {
    CssMatchFrameKind kind;
    CssMatchFlags flags;

    // For a sequence, its next child. For a production, its CssValueDef. For any other kind, the node itself.
    CssGrammarIndex node;

    // For a sequence, its end, one past its last child. For a || or &&, the child taken last.
    CssGrammarIndex end;

    uint16_t done;          // number of repetitions, or of children of a || or && taken
    uint32_t pos;           // stream position when the frame was pushed

    // For a repetition, a ! group, the child of a || or && taken last, or a production that a reference folds: the pool
    // size before it. For a function, production or component: the index of its value. For a list item: the stream's
    // end before it was narrowed.
    uint32_t anchor;

    uint32_t outer;         // the frame to continue at
    uint32_t production;    // the structural production that the frame is inside, or the End frame

    // For a || or &&, the children already taken, one bit for each position. For a list item or a function, the number
    // of choices when it was pushed. For a repetition, the pool size before its last repetition.
    uint64_t taken;
};

// A point that the matcher returns to when an attempt fails. The alternative to try is stored here rather than in the
// frame. The same frame is also resumed when an earlier choice matched a different prefix, and it then starts again
// from the first alternative.
struct CssMatchChoice {
    uint32_t frame;     // the frame to resume
    uint32_t state;     // the alternative to resume the frame with
    uint32_t frames;    // height to which the frames are unwound
    uint32_t pos;       // stream position
    uint32_t count;     // the stream's end, which a list item narrows
    uint32_t values;    // number of values in the pool
    uint32_t text;      // size of the pool's text
    uint32_t edits;     // number of edits
};

// A run of values that a match added to the pool, to be put in canonical form (cssom-1/#serialize-a-css-value) once no
// choice can undo it. A record for a || or && is followed by one for each child taken, in the order taken.
struct CssMatchEdit {
    uint32_t start;         // index of the first value
    uint32_t end;           // one past the last value
    CssGrammarIndex node;   // the node that the values match, or kCssMatchCalculate
    uint16_t parts;         // number of records that follow for the children
};

// The node of an edit that wraps one numeric value in a calculation.
constexpr CssGrammarIndex kCssMatchCalculate = CssGrammarRoot_None;

// The state that a match keeps while it runs. It holds nothing between values. A caller keeps one across values, so the
// arrays grow only once.
struct CssMatchState {
    Array<CssMatchFrame> frames;
    Array<CssMatchChoice> choices;
    Array<CssMatchEdit> edits;
    CssMathScratch math;        // the calculation trees of a value's math functions

    // If set, a given number (cssValueTokens) is wrapped in a calculation if its place does not accept it as is,
    // because it is out of range or not an integer. A written number is never wrapped.
    bool calculate;

    // True while the parser reads a user agent sheet. The selectors of such a sheet may name the engine's private
    // pseudo-classes.
    bool userAgent;
};

// Frees the state's memory.
void cssMatchStateReset(CssMatchState* state);

// ---------------------------------------------------------------------------------------------------------------------
// Values

// Returns true if `keyword` is one of the CSS-wide keywords, which every property accepts
// (css-values-4/#common-keywords).
bool cssIsCssWideKeyword(PrCssKeyword keyword);

// Parses the value of a declaration of `property`, which is the whole of `ts`. A CSS-wide keyword matches every
// property (css-values-4/#common-keywords). A value that contains a substitution function is kept as tokens
// (css-values-5/#arbitrary-substitution).
bool cssParsePropertyValue(PrCssProperty property, CssTokenStream* ts, CssValuePool* pool,
                           CssMatchState* state, CssValueSpan* out);

// Parses the value of a declaration of `descriptor`, which is the whole of `ts`. The value must match the grammar and
// satisfy the prose. On failure, the stream and pool are unmodified.
bool cssParseDescriptorValue(PrCssDescriptor descriptor, CssTokenStream* ts, CssValuePool* pool,
                             CssMatchState* state, CssValueSpan* out);

// Keeps the whole of `ts` as tokens. `ts` must be a <declaration-value> or nothing. A custom property's value is kept
// this way (css-variables-1/#defining-variables), as is a value that contains a substitution function
// (css-values-5/#arbitrary-substitution). On failure, the stream and pool are unmodified.
bool cssParseTokens(CssTokenStream* ts, CssValuePool* pool, CssValueSpan* out);

// Adds to the pool the values that a grammar matches (css-values-4/#value-defs). The whole of `ts` must match. On
// failure, the stream and pool are unmodified.
bool cssParseValue(CssGrammarIndex root, CssTokenStream* ts, CssValuePool* pool,
                   CssMatchState* state, CssValueSpan* out);

// Parses a numeric value of any type that Typed OM has (cssParseTypedNumeric). The value must be the whole of `ts`
// apart from surrounding whitespace. On failure, the stream and pool are unmodified.
bool cssParseNumericValue(CssTokenStream* ts, CssValuePool* pool, CssMatchState* state,
                          CssValueSpan* out);

// Matches the structural production `production` against the whole of `ts`. Adds the match to the pool as the
// production's node, or as its children if the production keeps no node (kCssValueDefKeepsNode). On failure, the stream
// and pool are unmodified.
bool cssParseProduction(CssValueDef production, CssTokenStream* ts,
                        const CssParseContext* context, CssValuePool* pool, CssMatchState* state,
                        CssValueSpan* out);

// Matches the prelude of an at-rule or a qualified rule, whose grammar is `root` (css_at_rule.h), against the whole of
// `ts`. The match is structural: every type with a grammar keeps a node. On failure, the stream and pool are
// unmodified.
bool cssParsePrelude(CssGrammarIndex root, CssTokenStream* ts, const CssParseContext* context,
                     CssValuePool* pool, CssMatchState* state, CssValueSpan* out);

// Returns the pseudo-element that the matched <pseudo-element-selector> at `node` names.
CssPseudoElement cssMatchedPseudoElement(const CssValuePool* pool, uint32_t node);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
