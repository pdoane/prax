// Terminals
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/
//
// A terminal is a production that the matcher hands to code rather than to a grammar. Each function below matches its
// production at the matcher's position, adds what it matched to the pool and steps the stream past it. It returns false
// if the tokens there are not the production. A comment names the production, and says what the code adds to its
// grammar or how it stores the match.

#pragma once

#include "css/src/css_grammar.h"
#include "css/src/css_terminal.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A match in progress (css_match.h).
struct CssMatcher;

// ---------------------------------------------------------------------------------------------------------------------
// Numeric Data Types

// <number>
bool cssParseNumber(const CssMatcher* m);

// <integer>
bool cssParseInteger(const CssMatcher* m);

// <percentage>
bool cssParsePercentage(const CssMatcher* m);

// <zero>
bool cssParseZero(const CssMatcher* m);

// <dimension>. A unit that no table names is kept as written, in an ident below the dimension.
bool cssParseDimension(const CssMatcher* m);

// <number-token>
bool cssParseNumberToken(const CssMatcher* m);

// A numeric literal of a grammar, such as 0deg (CssGrammarType_Number): a number, or a dimension of the literal's
// category, whose value equals the literal's value. The match is stored as the number or the dimension written.
bool cssParseNumberLiteral(const CssMatcher* m, CssGrammarNumber literal);

// <dimension-token>
bool cssParseDimensionToken(const CssMatcher* m);

// <percentage-token>
bool cssParsePercentageToken(const CssMatcher* m);

// <numeric-function>: a function that a calculation accepts as an operand (css-values-4/#calc-func). An enclosing math
// function stores it as it matched.
bool cssParseNumericFunction(const CssMatcher* m);

// <length>
bool cssParseLength(const CssMatcher* m);

// <length-percentage> (css-values-4/#mixed-percentages)
bool cssParseLengthPercentage(const CssMatcher* m);

// <angle>
bool cssParseAngle(const CssMatcher* m);

// <angle-percentage>
bool cssParseAnglePercentage(const CssMatcher* m);

// <time>
bool cssParseTime(const CssMatcher* m);

// <frequency>
bool cssParseFrequency(const CssMatcher* m);

// <resolution>
bool cssParseResolution(const CssMatcher* m);

// <flex>
bool cssParseFlex(const CssMatcher* m);

// A numeric value of any type that Typed OM has (css-typed-om-1/#dom-cssnumericvalue-parse): a number, a percentage, a
// dimension in a unit of a base type, or a math function of any type that it resolves to.
bool cssParseTypedNumeric(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Speech Units

// <decibel>
bool cssParseDecibel(const CssMatcher* m);

// <semitones>
bool cssParseSemitones(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Textual Data Types

// <ident>
bool cssParseIdent(const CssMatcher* m);

// <custom-ident>
bool cssParseCustomIdent(const CssMatcher* m);

// <dashed-ident>
bool cssParseDashedIdent(const CssMatcher* m);

// <string>
bool cssParseString(const CssMatcher* m);

// <path-data>
bool cssParsePathData(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Named Identifiers

// <counter-name>: a <custom-ident> other than `none` (css-lists-3/#typedef-counter-name).
bool cssParseCounterName(const CssMatcher* m);

// <counter-style-name>: a <custom-ident> other than `none` (css-counter-styles-3/#typedef-counter-style-name).
bool cssParseCounterStyleName(const CssMatcher* m);

// <voice-family-name>: a <string>, or a run of identifiers stored as the string of their values joined by single spaces
// (css-speech-1/#typedef-voice-family-voice-family-name).
bool cssParseVoiceFamilyName(const CssMatcher* m);

// <size-keyword>: a sizing keyword of the property that contains calc-size()
// (css-values-5/#typedef-calc-size-size-keyword).
bool cssParseSizeKeyword(const CssMatcher* m);

// <id>: an ID selector, stored as an <id-selector> (css-ui-4/#typedef-id).
bool cssParseId(const CssMatcher* m);

// <view-transition-type>: a <custom-ident> that is not `none` and does not start with `-ua-`
// (css-view-transitions-2/#typedef-view-transition-type).
bool cssParseViewTransitionType(const CssMatcher* m);

// <outline-line-style>: a <line-style> other than `hidden` (css-ui-4/#typedef-outline-line-style).
bool cssParseOutlineLineStyle(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Fonts

// <font-family-name>: a <string>, or a run of identifiers stored as the string of their values joined by single spaces
// (css-fonts-4/#font-family-name-syntax). Outside local(), the string is marked CssValueAuxFlag_FamilyIdents if it
// reads back the same when written as identifiers. Inside local(), the string is never marked, because it serializes as
// a string (css-fonts-4/#local-font-fallback).
bool cssParseFontFamilyName(const CssMatcher* m);

// <font-src-list>: the <font-src>s of a comma-separated list, excluding each one that does not parse or that names a
// format or tech that the UA does not support. A list with none left is invalid (css-fonts-4/#font-face-src-parsing).
bool cssParseFontSrcList(const CssMatcher* m);

// <opentype-tag>: a <string> of four characters, each in U+20-7E (css-fonts-4/#typedef-opentype-tag).
bool cssParseOpentypeTag(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Color

// <hex-color>
bool cssParseHexColor(const CssMatcher* m);

// <named-color>, kept as the keyword. css-color-4/#resolving-sRGB-values serializes its specified value as the keyword.
bool cssParseNamedColor(const CssMatcher* m);

// <system-color>
bool cssParseSystemColor(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Token Productions

// <ident-token>
bool cssParseIdentToken(const CssMatcher* m);

// <url-token>
bool cssParseUrlToken(const CssMatcher* m);

// <unicode-range-token>. A range with a bound above the maximum code point, or with its end before its start, does not
// match (css-fonts-4/#unicode-range-desc).
bool cssParseUnicodeRangeToken(const CssMatcher* m);

// <hash-token>, as the name after its '#'.
bool cssParseHashToken(const CssMatcher* m);

// <string-token>
bool cssParseStringToken(const CssMatcher* m);

// <function-token>, as the name before its '(' and a '(' literal.
bool cssParseFunctionToken(const CssMatcher* m);

// <at-keyword-token>, as a '@' literal and the name after it.
bool cssParseAtKeywordToken(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Token Runs

// <declaration-value>. As a function's argument it is free-form: the contents of a {} block that is the whole argument,
// or a run up to the next comma that contains no {} block (css-values-5/#component-function-commas).
bool cssParseDeclarationValue(const CssMatcher* m);

// <any-value> (css-syntax-3/#typedef-any-value). As a function's argument it is free-form, like <declaration-value>.
bool cssParseAnyValue(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// An+B

// <a-n-plus-b>, stored as its node over its two coefficients, each an integer (css-syntax-3/#anb-microsyntax).
bool cssParseANPlusB(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Rules

// <declaration>: the name, ':', value and optional !important that css-syntax-3/#consume-declaration consumes from the
// rest of the block (css-conditional-5/#at-supports-ext). The value is parsed as the property's value if the name is a
// property that accepts it, and as tokens otherwise. The name's ident is marked CssValueAuxFlag_Supported if a style
// rule accepts the declaration: a property with a value that it accepts, or a custom property
// (css-conditional-3/#dfn-support).
bool cssParseDeclaration(const CssMatcher* m);

// <dashed-function>: a function whose name starts with two hyphens. Each comma-separated argument must be a
// <declaration-value>, and is kept as tokens.
bool cssParseDashedFunction(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Queries

// <media-query-list>: the entries of the rest of the stream, split at its commas, each a <media-query>
// (mediaqueries-5/#mq-syntax). An entry that is not a media query becomes `not all` (mediaqueries-5/#error-handling).
// An entry whose value is unknown is kept as written and evaluates to false, as csswg-drafts#7595 resolved. A stream of
// only whitespace is the empty list.
bool cssParseMediaQueryList(const CssMatcher* m);

// <size-feature>: a media feature's syntax, running to the end of the block, whose feature is a size feature
// (css-conditional-5/#size-container).
bool cssParseSizeFeature(const CssMatcher* m);

// <scroll-state-feature>: a media feature's syntax, running to the end of the block, whose feature is a scroll-state
// feature (css-conditional-5/#scroll-state-container).
bool cssParseScrollStateFeature(const CssMatcher* m);

// <anchored-feature>: a media feature's syntax, running to the end of the block, whose feature is an anchored feature
// (css-anchor-position-2/#anchored-container). The feature's value may be longer than the single token of an
// <mf-value>, as its grammar allows.
bool cssParseAnchoredFeature(const CssMatcher* m);

// <style-feature-name>: a property the UA supports, or a <custom-property-name>
// (css-conditional-5/#typedef-style-feature-name).
bool cssParseStyleFeatureName(const CssMatcher* m);

// <style-feature-value>: a <declaration-value> that contains no <mf-lt>, <mf-gt> or <mf-eq>. It runs to the first of
// those in the block, or to the block's end (css-conditional-5/#typedef-style-feature-value).
bool cssParseStyleFeatureValue(const CssMatcher* m);

// <whole-value>: a value of the whole property that the match is for. It runs to the next comma of the enclosing
// function, or to its end, and is written in {} if it contains a comma (css-values-5/#whole-value).
bool cssParseWholeValue(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Scoping

// <scope-start>: a <selector-list> with no pseudo-element (css-cascade-6/#scope-syntax).
bool cssParseScopeStart(const CssMatcher* m);

// <scope-end>: a <selector-list> with no pseudo-element. Its :scope and & behave as in a scoped style rule, which
// accepts a <relative-selector-list> (css-cascade-6/#scope-syntax).
bool cssParseScopeEnd(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Selector Arguments

// <level>: a <number-token> whose type flag is "integer".
bool cssParseLevel(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Numeric Functions

// <transform-function>, matched by the generated grammar of the function that the token names
// (css-transforms-1/#transform-functions).
bool cssParseTransformFunction(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Color Functions

// <color-component-keyword>: in a relative color, one of the origin color's components in the color model of the
// function, or its alpha (css-color-5/#relative-colors).
bool cssParseColorComponentKeyword(const CssMatcher* m);

// ---------------------------------------------------------------------------------------------------------------------
// Terminals

// Matches `terminal` at the matcher's position, through its function above.
bool cssParseTerminal(const CssMatcher* m, CssTerminal terminal);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
