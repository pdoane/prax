// CSS grammar matcher state and nested matches.

#pragma once

#include "css/src/css_parse.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The next action of the match loop.
enum {
    CssMatchAction_Resume,   // continue at `frame`
    CssMatchAction_Once,     // match `node` once, then continue at `frame`
    CssMatchAction_Fail,     // undo back to the last choice
    CssMatchAction_Done,     // the whole stream matched
};
typedef uint8_t CssMatchAction;

// The argument of the pseudo-class or pseudo-element that a match is for. A logical combination pseudo-class passes on
// to its argument whatever its own position allows (selectors-4/#logical-combination).
struct CssMatchArgument {
    CssPseudoClass of;          // the pseudo-class, or CssPseudoClass_None for a pseudo-element

    // The pseudo-element that the selectors follow. They may contain only its pseudo-classes
    // (selectors-4/#pseudo-element-states).
    CssPseudoElement after;

    bool compound;              // true if each selector must be a compound selector
};

// One walk of the compiled grammar over the tokens of `ts`. The walk adds what it matches to `pool`. Its frames and
// choices live in `match`.
struct CssMatcher {
    CssTokenStream* ts;
    CssValuePool* pool;
    CssMatchState* match;
    const CssParseContext* PR_NULLABLE context;  // null for a value
    const CssMatcher* PR_NULLABLE parent;        // the match that a check started this one from
    CssMatchArgument argument;                   // the argument that this match is for
    CssAtRule features;                          // the at-rule whose descriptors a query's features name
    CssAnchorFunctions anchors;                  // the anchor functions that the property accepts
    PrCssProperty property;                      // the property whose value this matches, or PrCssProperty_Count
    uint32_t start;                              // first token of the property's value
    uint32_t depth;                              // number of functions that enclose a numeric function
    uint32_t choices;                            // number of choices below this match's own, which are its parent's
    uint32_t edits;                              // number of edits below this match's own
    uint32_t floor;                              // number of frames below this match's last choice; they may not change
    uint32_t steps;
    CssMatchAction action;
    uint32_t frame;
    uint32_t state;                              // the alternative that the resumed frame starts from
    CssGrammarIndex node;
};

// Matches the whole of the matcher's stream, from `production`, or from `root` if `production` is CssValueDef_Count.
// The match is structural if `structural` is set. It runs above any frames and choices that the current match already
// holds, because a check in the middle of a match can start it, and it leaves them unchanged. On failure, the stream
// and pool are unmodified.
bool cssMatchWhole(CssMatcher* m, CssGrammarIndex root, CssValueDef production, bool structural);

// Returns a matcher over `ts` for a match nested inside the one that `m` is running. The new matcher takes its context
// and the features of its queries from `m`.
CssMatcher cssMatchNested(const CssMatcher* m, CssTokenStream* ts);

// Matches the whole of `span`, from `production` or `root` as in cssMatchWhole, for the argument of a pseudo-class or
// pseudo-element whose check `m` is running.
bool cssMatchArgument(const CssMatcher* m, CssTokenSpan span, CssGrammarIndex root,
                      CssValueDef production, CssMatchArgument argument);

// Matches the whole of `span` as `production`, a query whose features are descriptors of `features`.
bool cssMatchQuery(const CssMatcher* m, CssTokenSpan span, CssValueDef production,
                   CssAtRule features);

// Returns the component keywords of the relative color whose arguments contain the match, as kCssColorComponents lists
// them. For alpha(), which accepts only `alpha`, each keyword is PrCssKeyword_Unknown. Returns null outside a relative
// color.
const PrCssKeyword* PR_NULLABLE cssMatchRelativeComponents(const CssMatcher* m);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
