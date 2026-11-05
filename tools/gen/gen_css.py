#!/usr/bin/env python3
"""Code generator for the CSS property/value tables.

Reads the W3C webref "ed" (editor's drafts) curated CSS extracts and emits:

    css_property.h/.cpp     enum of every CSS property, its name, grammar and alias target
    css_style_group.h/.cpp  the groups of the computed style, and the slot of each property (STYLE_GROUPS)
    css_at_rule.h/.cpp      enum of every at-rule, its prelude grammars, what its block holds, and its descriptors
    css_value_def.h/.cpp    enum of every named value type / function, its name and grammar
    css_keyword.h/.cpp      enum of every ident appearing in a grammar
    css_unit.cpp            the tables behind the unit enum, which css_unit.h declares
    css_pseudo.h/.cpp       the pseudo-classes and pseudo-elements, and their argument grammars
    css_color.h/.cpp        the channels of every <named-color>, and the <system-color> set
    css_terminal.h/.cpp     enum and names of the prose productions that need a parser
    css_grammar.cpp         every grammar, parsed into one flat node array

Usage: python3 -m tools.gen.gen_css [--webref DIR] [--out DIR]
"""

import argparse
import collections
import copy
import json
import math
import os
import re
import sys
import textwrap

from tools.gen import gen_common
from tools.gen import name_table
from tools.gen.gen_common import bitset, c_string, enum_members, namespaced, table, write

# ----------------------------------------------------------------------------------------------------------------------
# Spec ranking
#
# A property or value can be defined by more than one spec. The winner is chosen by
# (priority, level, name), highest first:
#
#   priority  the table below; anything not listed is 0
#   level     the trailing number of the spec shortname (css-backgrounds-4 -> 4), 0 if absent
#
# Legacy monolithic specs always lose to a module.
LEGACY_SPECS = {"CSS", "SVG", "compat"}

# Series that overlap. Listing a spec here makes it win against the module named in the comment, or
# lose with a negative priority. Re-check these when webref updates. The generator warns about any
# conflicting pair this table does not resolve.
SPEC_PRIORITY = {
    "css-borders": 2,               # beats css-backgrounds (border-*) and css-logical
    "css-position": 2,              # beats css-logical (inset-*)
    "css-sizing": 2,                # beats css-logical (block-size/inline-size), css-multicol
    "css-align": 2,                 # beats css-flexbox (justify-*/align-*)
    "fill-stroke": 2,               # beats svg-strokes
    "css-content": 2,               # beats css-gcpm (content())
    "css-color-hdr": 2,             # beats css-color (<color-function>, <predefined-rgb>)
    "css-shapes": 2,                # beats css-round-display
    "css-images": 2,                # beats css-gcpm (element())
    "scroll-animations": 2,         # beats pointer-animations (<axis>)
    "css-logical": 1,
    "css-conditional-values": -1,   # loses to css-values (if()); css-values-5's if() replaced it
}

# ----------------------------------------------------------------------------------------------------------------------
# At-rules
#
# webref gives an at-rule a grammar that covers the whole rule: `@media <media-query-list> {
# <rule-list> }`. What comes before the block is the prelude, and the production between the
# braces says what the block holds. The tables below hold the facts that the specs give in prose.

# The grammars of at-rules for which webref has none, as the prose writes them.
AT_RULE_GRAMMARS = {
    # css-transitions-2/#defining-before-change-style: a grouping rule of style rules.
    "@starting-style": "@starting-style { <rule-list> }",
}

# At-rules that webref lists but that are not rules. css-syntax-3/#charset-rule: there is no
# @charset rule, and one is dropped as an unrecognized at-rule.
NOT_AT_RULES = {"@charset"}

# Legacy aliases, which parse as the rule they alias. compat/#at-ruledef--webkit-keyframes.
AT_RULE_ALIASES = {"@-webkit-keyframes": "@keyframes"}

# The descriptors whose two values are the bounds of a range. They serialize as one value if the
# bounds are the same (css-fonts-4/#serializing-at-rules). They are the ranges a font face covers
# (css-fonts-4/#font-prop-desc, css-fonts-5/#font-size-desc).
BOUNDED_DESCRIPTORS = {("@font-face", "font-weight"), ("@font-face", "font-width"),
                       ("@font-face", "font-style"), ("@font-face", "font-size")}

# Legacy descriptor names. Each declares the descriptor of its at-rule that it is an alias of
# (css-cascade-5/#legacy-name-alias). css-fonts-4/#font-stretch-desc: font-stretch is an alias of
# font-width.
DESCRIPTOR_ALIASES = {("@font-face", "font-stretch"): "font-width"}

# At-rules whose declarations accept properties besides the at-rule's own descriptors: every CSS
# property, or only custom properties.
AT_RULE_PROPERTIES = {
    # css-mixins-1/#function-body: custom properties are a function's local variables.
    "@function": "CssPropertiesIn_Custom",
    # css-page-3/#syntax-page-selector: the page context and the margin contexts accept properties.
    "@page": "CssPropertiesIn_All", "@top-left-corner": "CssPropertiesIn_All",
    "@top-left": "CssPropertiesIn_All", "@top-center": "CssPropertiesIn_All",
    "@top-right": "CssPropertiesIn_All", "@top-right-corner": "CssPropertiesIn_All",
    "@bottom-left-corner": "CssPropertiesIn_All", "@bottom-left": "CssPropertiesIn_All",
    "@bottom-center": "CssPropertiesIn_All", "@bottom-right": "CssPropertiesIn_All",
    "@bottom-right-corner": "CssPropertiesIn_All", "@left-top": "CssPropertiesIn_All",
    "@left-middle": "CssPropertiesIn_All", "@left-bottom": "CssPropertiesIn_All",
    "@right-top": "CssPropertiesIn_All", "@right-middle": "CssPropertiesIn_All",
    "@right-bottom": "CssPropertiesIn_All",
    # css-anchor-position-1/#fallback-rule: a position option is a set of positioning properties.
    "@position-try": "CssPropertiesIn_All",
    # css-animations-1/#keyframes: a keyframe block's declarations are properties.
    "@keyframes": "CssPropertiesIn_All",
}

# At-rules whose declarations may not be !important. An !important declaration is invalid and
# ignored.
AT_RULE_NO_IMPORTANT = {
    "@keyframes",       # css-animations-1/#keyframes, for the declarations of a keyframe block
    "@position-try",    # css-anchor-position-1/#fallback-rule
}

# The at-rule that an at-rule is valid only inside. The block-holding descriptors that webref lists
# under an at-rule, as @font-feature-values lists @stylistic, belong inside it too.
AT_RULE_WITHIN = {
    # css-page-3/#margin-at-rules: margin at-rules are in the page context.
    "@top-left-corner": "@page", "@top-left": "@page", "@top-center": "@page",
    "@top-right": "@page", "@top-right-corner": "@page", "@bottom-left-corner": "@page",
    "@bottom-left": "@page", "@bottom-center": "@page", "@bottom-right": "@page",
    "@bottom-right-corner": "@page", "@left-top": "@page", "@left-middle": "@page",
    "@left-bottom": "@page", "@right-top": "@page", "@right-middle": "@page",
    "@right-bottom": "@page",
    # css-mixins-1/#contents-rule: outside of a mixin body, @contents is invalid.
    "@contents": "@mixin",
}

# At-rules whose block holds style rules but that are not valid inside a style rule, although
# css-nesting-1/#nested-group-rules would otherwise allow them. Each defines a name for the whole
# sheet.
NOT_NESTED_AT_RULES = {"@mixin", "@supports-condition", "@custom-media", "@custom-selector"}

# At-rules with no block of style rules that are valid inside a style rule:
# css-mixins-1/#apply-rule, and css-mixins-1/#contents-rule, whose mixin body is parsed as one.
NESTED_STATEMENTS = {"@apply", "@contents"}

# At-rules whose declarations the author names, each with the value that all of them take.
# css-fonts-4/#font-feature-values-syntax: a feature value block maps any identifier to a list of
# one or more non-negative integers. The list holds one integer in @stylistic, @swash, @ornaments
# and @annotation, and one or two in @character-variant. The multi-valued definitions
# (css-fonts-4/#multi-value-features) accept any index without a syntax error.
AT_RULE_NAMED_VALUES = {
    "@stylistic": "<integer [0,∞]>", "@swash": "<integer [0,∞]>", "@ornaments": "<integer [0,∞]>",
    "@annotation": "<integer [0,∞]>", "@character-variant": "<integer [0,∞]>{1,2}",
    "@styleset": "<integer [0,∞]>+", "@historical-forms": "<integer [0,∞]>+",
}

# At-rules whose block adds its declarations to an earlier block of the same at-rule in the same
# parent. css-fonts-4/#font-feature-values-syntax: the feature value blocks of one type are
# cascaded together.
AT_RULE_MERGED = set(AT_RULE_NAMED_VALUES)

# The production that the qualified rules in an at-rule's block match, including the block, if they
# are not style rules. css-animations-1/#keyframes: @keyframes holds only <keyframe-block>s.
AT_RULE_QUALIFIED_RULES = {"@keyframes": "<keyframe-block>"}

# The CSSRule type constant (cssom-1/#the-cssrule-interface) of the object that each at-rule creates. A rule named here
# has the type that its constant gives. Any other rule has type zero.
CSSOM_RULE_TYPES = {
    "@counter-style": "CounterStyle",
    "@font-face": "FontFace",
    "@font-feature-values": "FontFeatureValues",
    "@import": "Import",
    "@keyframes": "Keyframes",
    "@media": "Media",
    "@namespace": "Namespace",
    "@page": "Page",
    "@supports": "Supports",
    # css-page-3/#margin-at-rules: every margin at-rule is a CSSMarginRule.
    "@top-left-corner": "Margin", "@top-left": "Margin", "@top-center": "Margin",
    "@top-right": "Margin", "@top-right-corner": "Margin",
    "@bottom-left-corner": "Margin", "@bottom-left": "Margin",
    "@bottom-center": "Margin", "@bottom-right": "Margin",
    "@bottom-right-corner": "Margin", "@left-top": "Margin", "@left-middle": "Margin",
    "@left-bottom": "Margin", "@right-top": "Margin", "@right-middle": "Margin",
    "@right-bottom": "Margin",
}

# The productions that css-syntax-3/#block-contents gives a block, as the CssContents flags for what
# each allows.
BLOCK_CONTENTS = {
    "<block-contents>": "CssContents_BlockContents",
    "<declaration-list>": "CssContents_DeclarationList",
    "<qualified-rule-list>": "CssContents_QualifiedRuleList",
    "<at-rule-list>": "CssContents_AtRuleList",
    "<declaration-rule-list>": "CssContents_DeclarationRuleList",
    "<rule-list>": "CssContents_RuleList",
}


def unit_member(name):
    """Returns the PrCssUnit member of a unit as CSS_UNITS spells it, or the member for no unit if `name` is None."""
    return "PrCssUnit_" + (name[0].upper() + name[1:] if name else "Number")


# The units that css_unit.h numbers from PrCssUnit_Em, in that order. Each entry gives what the unit
# measures, and the size of one unit in the canonical unit of its type (css-values-4/#canonical-unit).
# The canonical unit is the first of its type with a scale of 1. A relative length has no scale. The
# text is the unit's canonical spelling, which a dimension serializes with
# (cssom-1/#serialize-a-css-component-value), and which is matched with ASCII case folded.
CSS_UNITS = [
    ("em", "Length", None),
    ("rem", "Length", None),
    ("ex", "Length", None),
    ("rex", "Length", None),
    ("cap", "Length", None),
    ("rcap", "Length", None),
    ("ch", "Length", None),
    ("rch", "Length", None),
    ("ic", "Length", None),
    ("ric", "Length", None),
    ("lh", "Length", None),
    ("rlh", "Length", None),
    ("vw", "Length", None),
    ("svw", "Length", None),
    ("lvw", "Length", None),
    ("dvw", "Length", None),
    ("vh", "Length", None),
    ("svh", "Length", None),
    ("lvh", "Length", None),
    ("dvh", "Length", None),
    ("vi", "Length", None),
    ("svi", "Length", None),
    ("lvi", "Length", None),
    ("dvi", "Length", None),
    ("vb", "Length", None),
    ("svb", "Length", None),
    ("lvb", "Length", None),
    ("dvb", "Length", None),
    ("vmin", "Length", None),
    ("svmin", "Length", None),
    ("lvmin", "Length", None),
    ("dvmin", "Length", None),
    ("vmax", "Length", None),
    ("svmax", "Length", None),
    ("lvmax", "Length", None),
    ("dvmax", "Length", None),
    ("cqw", "Length", None),
    ("cqh", "Length", None),
    ("cqi", "Length", None),
    ("cqb", "Length", None),
    ("cqmin", "Length", None),
    ("cqmax", "Length", None),
    ("cm", "Length", 96 / 2.54),
    ("mm", "Length", 96 / 25.4),
    ("Q", "Length", 96 / 101.6),
    ("in", "Length", 96),
    ("pc", "Length", 16),
    ("pt", "Length", 4 / 3),
    ("px", "Length", 1),
    ("deg", "Angle", 1),
    ("grad", "Angle", 0.9),
    ("rad", "Angle", 180 / math.pi),
    ("turn", "Angle", 360),
    ("s", "Time", 1),
    ("ms", "Time", 0.001),
    ("Hz", "Frequency", 1),
    ("kHz", "Frequency", 1000),
    ("dpi", "Resolution", 1 / 96),
    ("dpcm", "Resolution", 2.54 / 96),
    ("dppx", "Resolution", 1),
    ("x", "Resolution", 1),
    ("fr", "Flex", 1),
    ("dB", "Decibel", 1),
    ("st", "Semitones", 1),
]


# The characters that css-syntax-3/#tokenization gives a token kind of their own. Every other
# literal character is a <delim-token>.
PUNCTUATION_TOKENS = {
    ":": "CssTokenKind_Colon", ";": "CssTokenKind_Semicolon", ",": "CssTokenKind_Comma",
    "[": "CssTokenKind_OpenBracket", "]": "CssTokenKind_CloseBracket",
    "(": "CssTokenKind_OpenParen", ")": "CssTokenKind_CloseParen",
    "{": "CssTokenKind_OpenBrace", "}": "CssTokenKind_CloseBrace",
}

# css-values-4/#mixed-percentages: a <length-percentage> is a <length>, or a <percentage> that
# resolves against one. A math function in its place mixes the two
# (css-values-4/#calc-type-checking), which no single alternative of the grammar accepts.
MIXED_PERCENTAGES = {"<length-percentage>", "<angle-percentage>", "<time-percentage>",
                     "<frequency-percentage>"}

# Productions that webref gives a grammar but that the engine parses as prose. They are the mixed
# percentages, and <font-family-name>. css-fonts-4/#font-family-name-syntax restricts the latter's
# identifiers by what the surrounding font-family grammar could read instead, and its value is the
# string they join to. They also include <opentype-tag>, a <string> of four characters in U+20-7E
# (css-fonts-4/#typedef-opentype-tag). Finally, <a-n-plus-b>, whose grammar is written in the
# tokens' flags and units (css-syntax-3/#the-anb-type), is parsed by css-syntax-3/#anb-microsyntax.
PROSE_PRODUCTIONS = MIXED_PERCENTAGES | {"<font-family-name>", "<opentype-tag>", "<a-n-plus-b>"}

# A production that a spec defines in prose is a terminal, a parser in css_parse_terminal.cpp, only
# if its prose says something a grammar cannot. Prose that is a grammar in words is written here as
# that grammar. Examples are a list of keywords, another production under a new name, and a
# restricted form of a function. A production that only names another resolves to it
# (resolve_aliases).
PROSE_GRAMMARS = {
    # css-ui-4/#typedef-target-name: a <string>. A string that starts with "_" is still a target, as
    # the error handling reads it.
    "<target-name>": "<string>",
    # css-ui-4/#typedef-cursor-url-set: image-set() with its <image> restricted to <url>.
    "<url-set>": "image-set( [ [ <url> | <string> ] [ <resolution> || type( <string> ) ]? ]# )",
    # css-speech-1/#typedef-voice-family-age, css-speech-1/#typedef-voice-family-gender
    "<age>": "child | young | old",
    "<gender>": "male | female | neutral",
    # scroll-animations-1/#typedef-timeline-range-name
    "<timeline-range-name>": "cover | contain | entry | exit | entry-crossing | exit-crossing",
    # css-page-3/#typedef-page-size-page-size
    "<page-size>": "a5 | a4 | a3 | b5 | b4 | jis-b5 | jis-b4 | letter | legal | ledger",
    # css-fonts-4/#typedef-font-palette-palette-identifier,
    # css-variables-1/#typedef-custom-property-name, css-extensions-1/#custom-selectors,
    # css-conditional-5/#at-ruledef-supports-condition
    "<palette-identifier>": "<dashed-ident>",
    "<custom-property-name>": "<dashed-ident>",
    "<extension-name>": "<dashed-ident>",
    "<supports-condition-name>": "<extension-name>",
    # css-conditional-5/#typedef-boolean-condition: boolean algebra with media() and supports() as
    # its leaves. css-values-5/#typedef-boolean-expr writes it generically.
    "<boolean-condition>": "<boolean-expr[ <media()> | <supports()> ]>",
}

# Productions that a grammar names with a parameter, such as <boolean-expr[ <if-test> ]>. Each entry
# gives the parameter's name in their definitions, and the other productions those definitions name
# that also take the parameter (css-values-5/#typedef-boolean-expr). Each parameter that a grammar
# passes makes a production of its own.
GENERIC_PRODUCTIONS = {"<boolean-expr>": ("<test>", ["<boolean-expr-group>"])}

# The grammars of properties whose value a spec's property table gives but webref does not read, as the table writes
# them: SVG 2's stop properties (svg2-draft/pservers.html#StopColorProperty,
# svg2-draft/pservers.html#StopOpacityProperty).
PROPERTY_GRAMMARS = {
    "stop-color": "<color>",
    "stop-opacity": "<'opacity'>",
}

# The grammars of properties that their spec amends in prose, by property: the grammar that webref reads, and the
# grammar with the amendment. Where a grammar takes a <number> beside a length, the <number> comes first, since a 0
# that could be either parses as a <number> (css-values-4/#lengths).
PROPERTY_AMENDMENTS = {
    # SVG 2 makes a negative radius or stroke width an error (svg2-draft/geometry.html#R, svg2-draft/geometry.html#RX,
    # svg2-draft/painting.html#StrokeWidthProperty), which the corpus tests as invalid (svg/geometry/parsing,
    # svg/painting/parsing).
    "r": ("<length-percentage>", "<length-percentage [0,∞]>"),
    "rx": ("<length-percentage> | auto", "<length-percentage [0,∞]> | auto"),
    "ry": ("<length-percentage> | auto", "<length-percentage [0,∞]> | auto"),
    "stroke-width": ("<length-percentage> | <number>", "<number [0,∞]> | <length-percentage [0,∞]>"),
    # css-values-4/#lengths: a 0 that could be a <number> or a <length> parses as a <number>, which the alternatives
    # try first.
    "stroke-dashoffset": ("<length-percentage> | <number>", "<number> | <length-percentage>"),
    # w3c/svgwg#320 resolved that the d property takes path() alone, and that the attribute takes a path string, which
    # svg2-draft/paths.html#TheDProperty does not say yet.
    "d": ("none | <string>", "none | <path()>"),
    # css-inline-3/#alignment-baseline-svg-legacy: SVG content's text-before-edge and text-after-edge, which compute to
    # themselves and align as text-top and text-bottom. SVG 1.1's before-edge and after-edge, which Chrome and Safari
    # align in the same way, are taken too.
    # css-masking-1/#funcdef-clip-rect: the legacy rect() of clip, whose edges "User agents must support separation
    # with commas, but may also support separation without commas (but not a combination)". webref names css-shapes'
    # rect() instead.
    "clip": ("<rect()> | auto",
             "rect( [ <length> | auto ]#{4} ) | rect( [ <length> | auto ]{4} ) | auto"),
    # svg2-draft/painting.html#MarkerShorthand: "The value of the 'marker' is used directly for all three of the
    # corresponding longhand properties", whose grammar this is, so the shorthand's grammar names one of them.
    "marker": ("none | <marker-ref>", "<'marker-start'>"),
    "alignment-baseline": ("baseline | <baseline-metric>",
                           "baseline | <baseline-metric> | text-before-edge | text-after-edge | before-edge "
                           "| after-edge"),
    # css-writing-modes-4/#svg-writing-mode: SVG 1.1's values, which the cascade computes to horizontal-tb and
    # vertical-rl.
    # svg2-draft/interact.html#PointerEventsProperty: SVG's values, which css-ui-4/#pointer-events-control leaves to
    # SVG 2, and which the corpus tests as valid (svg/interact/parsing).
    "pointer-events": ("auto | none",
                       "auto | bounding-box | visiblePainted | visibleFill | visibleStroke | visible | painted "
                       "| fill | stroke | all | none"),
    # svg2-draft/coords.html#VectorEffectProperty: "These values can be enumerated. Thereby, the effect which has these
    # characteristics simultaneously can be specified", which the property's grammar of single keywords does not say.
    "vector-effect": ("none | non-scaling-stroke | non-scaling-size | non-rotation | fixed-position",
                      "none | [ non-scaling-stroke || non-scaling-size || non-rotation || fixed-position ]"),
    # css-gcpm-3/#running-elements: the element() that content names is gcpm's, which places a running element by its
    # name in a page-margin box. webref names css-images-4's, which takes an id.
    "content": ("normal | none | [ <content-replacement> | <content-list> ] [/ [ <string> | <counter> | <attr()> ]+ ]? "
                "| <element()>",
                "normal | none | [ <content-replacement> | <content-list> ] [/ [ <string> | <counter> | <attr()> ]+ ]? "
                "| element( <custom-ident> , [ first | start | last | first-except ]? )"),
    # css-gcpm-3/#setting-named-strings-the-string-set-pro: a named string's value is a content list, which content(),
    # counters and attr() make from the element. css-content-3's grammar, which webref reads, takes strings alone.
    "string-set": ("none | [ <custom-ident> <string>+ ]#", "none | [ <custom-ident> <content-list> ]#"),
    "writing-mode": ("horizontal-tb | vertical-rl | vertical-lr | sideways-rl | sideways-lr",
                     "horizontal-tb | vertical-rl | vertical-lr | sideways-rl | sideways-lr | lr | lr-tb | rl "
                     "| rl-tb | tb | tb-rl"),
}

# Definitions that webref reports missing, because a spec writes grammars that name a production it
# does not define. Each entry is a new production's definition, or `... | x` extending one.
MISSING_DEFINITIONS = {
    # css-values-5/#typedef-boolean-expr
    ("css-values-5", "<boolean-expr-group>"),
    # css-anchor-position-2/#container-rule-anchored: anchored( <anchored-query> ) in @container.
    ("css-anchor-position-2", "<query-in-parens>"),
}

# The math functions (css-values-4/#math, css-values-5/#progress, css-values-5/#random). Each can
# take the place of a numeric value, so every numeric type's terminal reaches their grammars.
MATH_FUNCTIONS = ["calc()", "min()", "max()", "clamp()", "round()", "mod()", "rem()", "sin()",
                  "cos()", "tan()", "asin()", "acos()", "atan()", "atan2()", "pow()", "sqrt()",
                  "hypot()", "log()", "exp()", "abs()", "sign()", "progress()", "random()"]

# The functions that every property accepts as its entire value, as it accepts a CSS-wide keyword
# (css-values-5/#whole-value). The specs define cycle(), first-valid() and interpolate() as such
# functions (css-values-5/#cycle-notation, css-values-5/#first-valid,
# css-values-5/#funcdef-interpolate).
WHOLE_VALUE_FUNCTIONS = ["cycle()", "first-valid()", "interpolate()"]

# The arbitrary substitution functions, which every property accepts anywhere in its value. Each
# entry gives the argument grammar that the function is checked against when the value is parsed,
# before substitution (css-values-5/#arbitrary-substitution): css-variables-2/#typedef-var-args,
# css-values-5/#typedef-attr-args, css-env-1/#typedef-env-args, css-values-5/#typedef-if-args,
# css-values-5/#typedef-inherit-args, css-values-5/#typedef-ident-args,
# css-values-5/#typedef-random-item-args.
SUBSTITUTION_FUNCTIONS = {"var()": "<var-args>", "attr()": "<attr-args>", "env()": "<env-args>",
                          "if()": "<if-args>", "inherit()": "<inherit-args>",
                          "ident()": "<ident-args>", "random-item()": "<random-item-args>"}

# The numeric functions that are not math functions and that no grammar names
# (css-values-4/#functional-numeric): css-values-5/#tree-counting writes sibling-index() and
# sibling-count() in prose, as an <integer> anywhere one goes.
TREE_COUNTING_FUNCTIONS = ["sibling-index()", "sibling-count()"]

# The anchor functions resolve to a <length> (css-anchor-position-1/#anchor-pos,
# css-anchor-position-1/#anchor-size-fn). A property that accepts one accepts it wherever a <length>
# goes, including inside a math function. The new values that css-anchor-position-1 adds to the
# grammars are left out of them. Instead, these tables name the properties each function is valid
# in.
ANCHOR_NEW_VALUES = {"<anchor()>", "<anchor-size()>"}

# css-anchor-position-1/#anchor-pos: anchor() is only allowed in the inset properties
# (css-position-3/#inset-properties).
INSET_PROPERTIES = {"top", "right", "bottom", "left", "inset", "inset-block", "inset-inline",
                    "inset-block-start", "inset-block-end", "inset-inline-start",
                    "inset-inline-end"}

# css-anchor-position-1/#sizing: anchor-size() is only allowed in the accepted @position-try
# properties (css-anchor-position-1/#accepted-position-try-properties) that accept a length. Those
# are the inset, margin (css-box-4/#margin-physical, css-logical-1/#margin-properties) and sizing
# (css-sizing-3/#sizing-property) properties.
ANCHOR_SIZE_PROPERTIES = INSET_PROPERTIES | {
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left", "margin-block",
    "margin-block-start", "margin-block-end", "margin-inline", "margin-inline-start",
    "margin-inline-end",
    "width", "height", "min-width", "min-height", "max-width", "max-height", "block-size",
    "inline-size", "min-block-size", "min-inline-size", "max-block-size", "max-inline-size"}

# Keywords that the prose of a property or production excludes from its <custom-ident>s, beyond
# the ones css-values-4/#custom-idents excludes everywhere. A property whose value reaches the
# production excludes them from every <custom-ident> of its value. So the generator exits if the
# property reaches a <custom-ident> that the production does not hold.
CUSTOM_IDENT_EXCLUSIONS = {
    # css-grid-2/#typedef-grid-row-start-grid-line: <custom-ident> additionally excludes the
    # keywords span and auto.
    "<grid-line>": ["span", "auto"],
    # css-color-adjust-1/#propdef-color-scheme: "The normal, light, dark, and only keywords are not
    # valid <custom-ident>s in this property."
    "color-scheme": ["normal", "light", "dark", "only"],
}

# The numeric types whose place a math function or another numeric function may take
# (css-values-4/#math).
NUMERIC_TERMINALS = {"<number>", "<integer>", "<percentage>", "<length>", "<angle>", "<time>",
                     "<frequency>", "<resolution>", "<flex>"} | MIXED_PERCENTAGES

# The tokens that a prose production starts with, if its prose says which. A production named for a
# token starts with that token. A production this table does not list may start with anything. A
# numeric type may start with a function, which is a math function in its place.
PROSE_TOKENS = {
    # css-values-4/#numbers, css-values-4/#percentages, css-values-4/#dimensions
    "<number>": ["Number", "Function"], "<integer>": ["Number", "Function"], "<zero>": ["Number"],
    "<percentage>": ["Percentage", "Function"], "<dimension>": ["Dimension"],
    # css-values-4/#lengths: a zero length may be written as a number.
    "<length>": ["Dimension", "Number", "Function"],
    "<angle>": ["Dimension", "Function"], "<time>": ["Dimension", "Function"],
    "<frequency>": ["Dimension", "Function"], "<resolution>": ["Dimension", "Function"],
    # css-values-4/#mixed-percentages
    "<length-percentage>": ["Dimension", "Number", "Percentage", "Function"],
    "<angle-percentage>": ["Dimension", "Percentage", "Function"],
    "<time-percentage>": ["Dimension", "Percentage", "Function"],
    "<frequency-percentage>": ["Dimension", "Percentage", "Function"],
    # css-values-4/#calc-func: a function as the operand of a calculation
    "<numeric-function>": ["Function"],
    # css-grid-1/#fr-unit
    "<flex>": ["Dimension", "Function"],
    # css-speech-1/#mixing-props-voice-volume, css-speech-1/#voice-props-voice-pitch
    "<decibel>": ["Dimension"], "<semitones>": ["Dimension"],
    # css-values-4/#identifier-value, css-values-4/#custom-idents, css-values-4/#dashed-idents,
    # css-values-4/#strings
    "<ident>": ["Ident"], "<custom-ident>": ["Ident"], "<dashed-ident>": ["Ident"],
    "<string>": ["String"],
    # css-color-4/#hex-notation, css-color-4/#named-colors, css-color-4/#css-system-colors
    "<hex-color>": ["Hash"], "<named-color>": ["Ident"], "<system-color>": ["Ident"],
    # css-color-5/#relative-colors
    "<color-component-keyword>": ["Ident"],
    # css-fonts-4/#font-family-name-syntax, css-fonts-4/#typedef-opentype-tag
    "<font-family-name>": ["Ident", "String"], "<opentype-tag>": ["String"],
}

# The query productions whose features are the ones that sections of a spec define, as descriptors
# of the at-rule the query is in. They are @container's size, scroll-state and anchored features
# (css-conditional-5/#size-container, css-conditional-5/#scroll-state-container,
# css-anchor-position-2/#anchored-container). A feature from another section is not accepted.
FEATURE_SECTIONS = {
    ("css-conditional-5", "size-container"): "<size-feature>",
    ("css-conditional-5", "scroll-state-container"): "<scroll-state-feature>",
    ("css-anchor-position-2", "anchored-container"): "<anchored-feature>",
}

# Productions that a spec defines by listing their keywords, which webref extracts as their values.
# The grammar is a choice among them. css-overflow-5/#typedef-scroll-button-direction,
# pointer-animations-1/#typedef-timeline-range-center-subject,
# animation-triggers-1/#typedef-animation-action
KEYWORD_PRODUCTIONS = {"<scroll-button-direction>", "<timeline-range-center-subject>",
                       "<animation-action>"}

# Prose productions that may match no tokens at all, which their first sets record.
# mediaqueries-5/#mq-syntax: <media-query-list> is intentionally the empty list for no tokens.
# css-values-5/#typedef-inherit-args: a comma with nothing after it passes a <declaration-value> as
# an empty sequence, which its parser accepts only there.
PROSE_EMPTY = {"<media-query-list>", "<declaration-value>"}

# Definitions, by spec and name, that yield to the definition of the next spec. fill-stroke-3/#fill-shorthand makes
# fill and stroke shorthands, and gives their value only in prose ("<'background'> with modifications"), with its own
# <paint> for their images. SVG 2's fill, stroke and <paint>, which every engine implements, remain until fill-stroke-3
# gives them a grammar. fill-stroke-3's and svg-strokes' stroke-width, stroke-dasharray and stroke-dashoffset leave out
# the number alone, in pixels, that SVG 2 allows them, which every engine parses and the corpus tests
# (svg/painting/parsing).
DEFERRED_DEFINITIONS = {
    ("fill-stroke", "fill"),
    ("fill-stroke", "stroke"),
    ("fill-stroke", "<paint>"),
    ("fill-stroke", "stroke-width"),
    ("fill-stroke", "stroke-dasharray"),
    ("fill-stroke", "stroke-dashoffset"),
    ("svg-strokes", "stroke-width"),
    ("svg-strokes", "stroke-dasharray"),
    ("svg-strokes", "stroke-dashoffset"),
}

# Grammars that a spec amends in prose, as the value definition the amendment produces.
GRAMMAR_AMENDMENTS = {
    # css-nesting-1/#nest-selector: `&` is a simple selector allowed anywhere in a compound
    # selector, even before a type selector.
    "<compound-selector>": "[ '&'* <type-selector>? [ <subclass-selector> | '&' ]* ]!",
    # selectors-4/#forgiving-selector: the argument of :is() and :where() is <any-value>?, so a
    # functional pseudo-class may be written with nothing between its parentheses.
    "<pseudo-class-selector>": ": <ident-token> | : <function-token> <any-value>? )",
    # css-anchor-position-1/#anchor-pos: anchor() has three arguments: the anchor name and the side
    # in either order before the comma, and the fallback after it. As the grammar is written,
    # juxtaposition binds tighter than && (css-values-4/#component-combinators), which would put the
    # comma and the fallback inside the && group.
    "anchor()": "anchor( [ <anchor-name>? && <anchor-side> ], <length-percentage>? )",
    # css-values-4/#parse-a-calculation reads a calculation's numbers, dimensions and percentages
    # as the tokens they are. css-values-4/#calc-func accepts other math functions and numeric
    # functions (css-values-4/#functional-numeric) as its operands. In a relative color, a
    # component keyword is a <number> that a math function may accept (css-color-5/#component-keyword).
    # Outside a relative color, its terminal matches nothing.
    "<calc-value>": "<number-token> | <dimension-token> | <percentage-token> | <calc-keyword> "
                    "| <color-component-keyword> | ( <calc-sum> ) | <numeric-function>",
    # css-values-4/#math: a math function may take the place of the <number> or <dimension> of a
    # media feature's value. The feature's own grammar checks its type.
    "<mf-value>": "<number> | <dimension> | <ident> | <ratio> | <numeric-function>",
    # css-values-5/#request-url-modifiers: the <url-modifier>s that url() and src() accept are the
    # <request-url-modifier>s. Each sets a part of the request, once. The WPT url modifier tests
    # expect a repeated modifier to be invalid, and the modifiers to serialize in the order written
    # here.
    "url()": "url( <string> [ <cross-origin-modifier> || <integrity-modifier> || "
             "<referrer-policy-modifier> ]? ) | <url-token>",
    "src()": "src( <string> [ <cross-origin-modifier> || <integrity-modifier> || "
             "<referrer-policy-modifier> ]? )",
    # scroll-animations-1/#named-range-keyframes: a keyframe selector may name a timeline range.
    "<keyframe-selector>": "from | to | <percentage [0,100]> | <timeline-range-name> <percentage>",
    # filter-effects-1/#supported-filter-functions: "Negative values are not allowed" for the argument of each filter
    # function but hue-rotate(), and for drop-shadow()'s standard deviation, which makes a negative argument invalid,
    # as css/filter-effects/parsing/filter-parsing-invalid.html tests.
    "blur()": "blur( <length [0,∞]>? )",
    "brightness()": "brightness( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "contrast()": "contrast( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "grayscale()": "grayscale( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "invert()": "invert( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "opacity()": "opacity( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "saturate()": "saturate( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "sepia()": "sepia( [ <number [0,∞]> | <percentage [0,∞]> ]? )",
    "drop-shadow()": "drop-shadow( [ <color>? && [ <length>{2} <length [0,∞]>? ] ] )",
    # svg2-draft/painting.html#StrokeDasharrayProperty: a negative value is an error, which the corpus tests as invalid
    # (svg/painting/parsing/stroke-dasharray-invalid.svg).
    "<dasharray>": "[ [ <number [0,∞]> | <length-percentage [0,∞]> ]+ ]#",
    # css-shapes-1/#funcdef-basic-shape-path: the string of path() is path data (svg2-draft/paths.html#PathDataBNF),
    # which <path-data> parses into its commands (css_path.h).
    "path()": "path( <'fill-rule'>? , <path-data> )",
    # css-shapes-1/#funcdef-basic-shape-circle takes css-images-4's <radial-size> (css-images-4/#radial-size), whose
    # single size may be a percentage, and says that "Two <length-percentage> values are invalid", so a circle takes
    # one extent or one size. css-images-3's <radial-size>, which webref reads, takes no percentage alone.
    "circle()": "circle( [ <radial-extent> | <length-percentage [0,∞]> ]? [ at <position> ]? )",
    # css-shapes-1/#funcdef-basic-shape-ellipse: css-images-4's <radial-size> for an ellipse, whose two radii are each
    # an extent or a size (css/css-shapes/shape-functions/ellipse-function-valid.html).
    "ellipse()": "ellipse( [ [ <radial-extent> | <length-percentage [0,∞]> ]{2} ]? [ at <position> ]? )",
}

# Definitions that a spec writes as placeholders. They are left out, so the previous level defines
# the value, or nothing does if there is no previous level.
PLACEHOLDER_DEFINITIONS = {
    # css-color-6/#funcdef-contrast-color: the tbd-fg and tbd-bg keywords are placeholders.
    ("contrast-color()", "css-color-6"),
    # css-backgrounds-4/#propdef-background-tbd: the name is a placeholder.
    ("background-tbd", "css-backgrounds-4"),
}

# Alternatives that specs add to a production in prose, appended to the production's grammar: the spec and the
# alternative's grammar, for each.
GRAMMAR_EXTENSIONS = {
    # css-color-6/#color-layers: color-layers() returns a <color>.
    "<color>": [("css-color-6", "<color-layers()>")],
    # css-color-5/#light-dark: the image form of light-dark() returns one of its images, so it is an <image> wherever
    # an image is written. filter-effects-1/#FilterCSSImageValue: "This specification introduces the <image> filter()
    # function".
    "<image>": [("css-color-5", "<light-dark-image>"), ("filter-effects-1", "<filter()>")],
    # css-gcpm-3/#selectordef-nth: :nth() is a page pseudo-class, written where css-page's are.
    "<pseudo-page>": [("css-gcpm", ": nth( <a-n-plus-b> [ of <custom-ident> ]? )")],
}

# The relative color productions. css-color-5/#relative-colors: within a relative color function,
# the component keywords are the origin color's components. Each is a <number> wherever a component
# is written, which is wherever the grammar accepts `none`.
RELATIVE_COLOR_PRODUCTIONS = [
    "<modern-rgb-syntax>", "<modern-rgba-syntax>", "<modern-hsl-syntax>", "<modern-hsla-syntax>",
    "hwb()", "lab()", "lch()", "oklab()", "oklch()", "alpha()", "color()",
    "<predefined-rgb-params>", "<xyz-params>",
]

# The prose production that a relative color's components also accept.
COMPONENT_KEYWORD = "<color-component-keyword>"

# The members of PrColorModel in pr/color.h, in order.
COLOR_MODELS = ["PrColorModel_Gray", "PrColorModel_Rgb", "PrColorModel_Xyz", "PrColorModel_Lab",
                "PrColorModel_Lch", "PrColorModel_Hsl", "PrColorModel_Hwb"]

# The component keywords of each color model, in the order its channels are written
# (css-color-5/#relative-RGB, css-color-5/#relative-HSL, css-color-5/#relative-HWB,
# css-color-5/#relative-Lab, css-color-5/#relative-LCH, css-color-5/#relative-color-function).
# `alpha` is the alpha of every model.
COMPONENT_KEYWORDS = {
    "PrColorModel_Rgb": ["r", "g", "b"],
    "PrColorModel_Xyz": ["x", "y", "z"],
    "PrColorModel_Lab": ["l", "a", "b"],
    "PrColorModel_Lch": ["l", "c", "h"],
    "PrColorModel_Hsl": ["h", "s", "l"],
    "PrColorModel_Hwb": ["h", "w", "b"],
}

# Productions whose components are written with no whitespace between them, as the prose says.
NO_WHITESPACE = {
    # selectors-4/#grammar, selectors-5/#grammar: whitespace in a <complex-selector> is its
    # descendant combinator, and no whitespace appears inside the compound selectors it joins.
    "<compound-selector>", "<complex-selector-unit>", "<pseudo-compound-selector>",
    "<type-selector>", "<class-selector>", "<pseudo-class-selector>", "<pseudo-element-selector>",
    "<legacy-pseudo-element-selector>", "<wq-name>", "<ns-prefix>", "<attr-matcher>",
    "<combinator>",
    # css-cascade-5/#layer-names: the idents of a layer name and the periods between them.
    "<layer-name>",
    # css-page-3/#syntax-page-selector
    "<page-selector>", "<pseudo-page>",
    # mediaqueries-5/#mq-syntax: the '<' or '>' and the '=' after it.
    "<mf-lt>", "<mf-gt>",
    # css-properties-values-api-1/#consume-syntax-component: a data type name is the name
    # between a '<' and a '>'.
    "<syntax-single-component>",
    # css-view-transitions-2/#pseudo-element-class-additions: white space is forbidden between
    # the name and the classes, and between the components of the classes.
    "<pt-name-and-class-selector>", "<pt-class-selector>",
    # css-extensions-1/#custom-selectors: a custom selector is a pseudo-class, its ':' and name
    # written together.
    "<custom-selector>",
}

# css-fonts-4/#font-face-src-parsing: the <string>s that format() accepts for backwards
# compatibility, each with the <font-format> keyword it acts as, and whether it adds
# tech(variations).
FONT_FORMAT_STRINGS = [
    ("woff2", "woff2", False), ("woff", "woff", False), ("truetype", "truetype", False),
    ("opentype", "opentype", False), ("collection", "collection", False),
    ("woff2-variations", "woff2", True), ("woff-variations", "woff", True),
    ("truetype-variations", "truetype", True), ("opentype-variations", "opentype", True),
]

# Selectors that webref lists but the engine leaves out. selectors-4/#selectordef-matches: a UA
# may implement :matches() as a legacy alias of :is(), and none does.
OMITTED_SELECTORS = {":matches()"}

# Pseudo-classes that a spec defines only for pages. <pseudo-page> accepts them and an element
# selector does not (GRAMMAR_EXTENSIONS). css-gcpm-3/#selectordef-nth
PAGE_PSEUDO_CLASSES = {":nth()"}

# The engine's private pseudo-classes, which parse only in user agent sheets. Each one states a condition that HTML's
# rendering section puts on its rules (html/#rendering) and that no standard selector states: a select rendered as a
# list box. The enum lists them after webref's pseudo-classes.
PRIVATE_PSEUDO_CLASSES = ["-pr-list-box"]

# Legacy pseudo-class names, each parsing as the pseudo-class it is an alias of.
# html/#selector-webkit-autofill.
PSEUDO_CLASS_ALIASES = {"-webkit-autofill": "autofill"}

# selectors-4/#pseudo-element-states: any pseudo-element may be followed by the logical
# combination pseudo-classes (selectors-4/#logical-combination) and the user action
# pseudo-classes (selectors-4/#useraction-pseudos).
LOGICAL_COMBINATION = [":is()", ":where()", ":not()"]
USER_ACTION = [":hover", ":active", ":focus", ":focus-visible", ":focus-within"]

# The pseudo-elements after which every engine rejects the user action pseudo-classes, as the corpus
# tests: the highlight pseudo-elements (css-pseudo/parsing/highlight-pseudos.html,
# css-highlight-api/highlight-pseudo-parsing.html), ::column
# (css-multicol/parsing/column-pseudo-invalid.html), ::slotted()
# (selectors/parsing/parse-slotted.html) and the view transition pseudo-elements
# (css-view-transitions/parsing/pseudo-elements-invalid.html).
NO_USER_ACTION = [
    "::selection", "::target-text", "::spelling-error", "::grammar-error", "::search-text",
    "::highlight()", "::column", "::slotted()", "::view-transition", "::view-transition-group()",
    "::view-transition-image-pair()", "::view-transition-old()", "::view-transition-new()",
    "::view-transition-group-children()",
]

# css-pseudo-4/#element-backed: every pseudo-class and pseudo-element may follow an element-backed
# pseudo-element. css-pseudo-4/#details-content-pseudo says ::details-content is element-backed,
# and css-pseudo-4/#element-backed names ::part(). ::picker(select) is the select popover
# (html/#the-select-element-2), a real element.
ELEMENT_BACKED = ["::details-content", "::part()", "::picker()"]

# The pseudo-classes that may not follow an element-backed pseudo-element. The parser rejects them,
# as csswg-drafts#10786 resolved: the tree-structural pseudo-classes
# (selectors-4/#structural-pseudos), and :has(), :scope, :host, :host() and :host-context(), as
# csswg-drafts#10787 resolved.
NOT_AFTER_ELEMENT_BACKED = [
    ":root", ":empty", ":nth-child()", ":nth-last-child()", ":first-child", ":last-child",
    ":only-child", ":nth-of-type()", ":nth-last-of-type()", ":first-of-type", ":last-of-type",
    ":only-of-type", ":has()", ":scope", ":host", ":host()", ":host-context()",
]

# The tree-abiding pseudo-elements (css-pseudo-4/#treelike). The element-backed ones are
# tree-abiding too, and so is every fully styleable pseudo-element (css-pseudo-4/#fully-styleable).
TREE_ABIDING = ELEMENT_BACKED + [
    # css-pseudo-4/#generated-content, css-pseudo-4/#marker-pseudo,
    # css-pseudo-4/#placeholder-pseudo, css-pseudo-4/#file-selector-button-pseudo
    "::before", "::after", "::marker", "::placeholder", "::file-selector-button",
    # css-forms-1/#selectordef-picker-icon, css-forms-1/#selectordef-checkmark,
    # css-forms-1/#selectordef-slider-thumb, css-forms-1/#selectordef-step-control: fully styleable
    "::picker-icon", "::checkmark", "::slider-thumb", "::slider-track", "::slider-fill",
    "::step-control", "::step-up", "::step-down",
    # css-overflow-5/#selectordef-scroll-marker-group, css-overflow-5/#scroll-buttons: fully
    # styleable
    "::scroll-marker-group", "::scroll-button()",
    # css-view-transitions-2/#pseudo-root: the view transition pseudo-elements form a tree of
    # tree-abiding pseudo-elements.
    "::view-transition", "::view-transition-group()", "::view-transition-image-pair()",
    "::view-transition-old()", "::view-transition-new()", "::view-transition-group-children()",
]

# The pseudo-classes that a spec allows after a pseudo-element, beyond the logical combination and
# the user action pseudo-classes.
PSEUDO_ELEMENT_CLASSES = {
    # css-pseudo-4/#selectordef-search-text: :current may be combined with ::search-text.
    "::search-text": [":current"],
    # css-overflow-5/#scroll-buttons: :disabled can apply to ::scroll-button().
    "::scroll-button()": [":disabled"],
    # css-view-transitions-2/#pseudo-root: :only-child matches a descendant of a pseudo-element
    # root with no siblings.
    "::view-transition-group()": [":only-child"],
    "::view-transition-image-pair()": [":only-child"],
    "::view-transition-old()": [":only-child"],
    "::view-transition-new()": [":only-child"],
    "::view-transition-group-children()": [":only-child"],
}

# The sub-pseudo-elements a spec defines (selectors-4/#sub-pseudo-elements), by the
# pseudo-element they follow.
SUB_PSEUDO_ELEMENTS = {
    # css-pseudo-4/#marker-pseudo: ::before::marker and ::after::marker are valid.
    "::before": ["::marker"],
    "::after": ["::marker"],
    # css-shadow-1/#slotted-pseudo: ::slotted() can be followed by a tree-abiding pseudo-element.
    "::slotted()": TREE_ABIDING,
    # css-multicol-2/#column-pseudo: a ::column can have a ::scroll-marker of its own.
    "::column": ["::scroll-marker"],
}

# ----------------------------------------------------------------------------------------------------------------------
# Animation (web-animations-1/#animation-type)

# The properties that animate discretely although webref gives another animation type, with the reason.
DISCRETE = {
    # css-lists-3 animates the counter properties by computed value type, which interpolates the integers of a list of
    # counter names. WPT's css-lists-no-interpolation, and every browser, switch them discretely.
    "counter-increment", "counter-reset", "counter-set",
}

# The properties that animate as a repeatable list although webref gives another animation type, with the reason.
REPEATABLE_LIST = {
    # SVG2/painting.html#StrokeDashing gives the type in prose: both lists repeat to the least common multiple of their
    # lengths and combine item by item, which fill-stroke-3 names a repeatable list.
    "stroke-dasharray",
}

# ----------------------------------------------------------------------------------------------------------------------
# Style groups

# A group of the computed style (css_style_group.h): the longhands whose computed values it holds, in the order of
# their slots, whether they inherit, which part of the engine reads them, and why they are a group of their own. The
# groups partition the longhands that the engine computes. Which group a property joins is the engine's choice: values
# that change together, and that the same part of the engine reads, share a group, so that a style that changes none of
# them shares the group with its parent or the initial style. Within a group, related properties sit together, since
# what reads one tends to read the others.
StyleGroup = collections.namedtuple("StyleGroup", "name inherited reader note properties")

STYLE_GROUPS = [
    StyleGroup(
        "Font", True, "read by layout",
        "",
        [
            "font-family", "font-size", "font-style", "font-weight", "font-width", "line-height", "dominant-baseline",
            "writing-mode", "direction", "text-orientation", "white-space-collapse", "text-wrap-mode",
            "text-align-all", "text-align-last", "text-justify", "text-indent", "letter-spacing", "word-spacing",
            "text-transform",
        ]),
    StyleGroup(
        "Color", True, "read by painting, and by hit testing",
        ("The inherited properties that painting reads, and those that decide how a box takes the pointer: whether it "
         "is a target, and the cursor that it shows."),
        [
            "color", "accent-color", "caret-color", "color-scheme", "dynamic-range-limit", "image-rendering",
            "image-orientation", "visibility", "interactivity", "pointer-events", "cursor",
        ]),
    StyleGroup(
        "Box", False, "read by layout",
        "",
        [
            "display", "position", "float", "clear", "top", "right", "bottom", "left", "box-sizing", "width", "height",
            "min-width", "min-height", "max-width", "max-height", "aspect-ratio", "field-sizing",
            "contain-intrinsic-width", "contain-intrinsic-height", "margin-top", "margin-right", "margin-bottom",
            "margin-left", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
            "padding-top", "padding-right", "padding-bottom", "padding-left", "overflow-x", "overflow-y",
            "text-overflow", "overflow-anchor", "overscroll-behavior-x", "overscroll-behavior-y", "contain",
            "content-visibility", "container-type", "baseline-source", "alignment-baseline", "baseline-shift",
            "unicode-bidi", "table-layout", "input-security",
        ]),
    StyleGroup(
        "Flex", False, "read by layout: flex layout and box alignment",
        "",
        [
            "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis", "order", "row-gap", "column-gap",
            "justify-content", "justify-items", "justify-self", "align-content", "align-items", "align-self",
        ]),
    StyleGroup(
        "Decoration", False, "read by painting",
        "",
        [
            "border-top-color", "border-right-color", "border-bottom-color", "border-left-color", "border-top-style",
            "border-right-style", "border-bottom-style", "border-left-style", "object-fit", "object-position",
            "outline-color", "outline-style", "outline-width", "outline-offset", "text-decoration-line",
            "text-decoration-style", "text-decoration-color", "text-decoration-thickness",
        ]),
    StyleGroup(
        "Background", False, "read by painting: the background's color and layers, and corners",
        ("Most elements declare no background, so the background's longhands are a group of their own, apart from the "
         "borders and outlines that more elements declare. The corners' radii, which round the background, join it, "
         "since few elements declare them either."),
        [
            "background-color", "background-clip", "background-image", "background-position", "background-size",
            "background-repeat", "background-attachment", "background-origin", "border-top-left-radius",
            "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius",
            "corner-top-left-shape", "corner-top-right-shape", "corner-bottom-right-shape", "corner-bottom-left-shape",
            "background-blend-mode",
        ]),
    StyleGroup(
        "Effect", False, "read by compositing: opacity, transforms and stacking",
        "",
        [
            "opacity", "z-index", "will-change", "transform", "translate", "rotate", "scale", "transform-origin",
            "transform-box", "perspective", "perspective-origin", "backface-visibility",
        ]),
    StyleGroup(
        "Compositing", False, "read by compositing: clip paths, masks, filters and blending",
        ("Few elements clip, mask, filter or blend, so clip paths, mask layers, mask borders, filters and blending are "
         "a group of their own (css-masking-1, filter-effects-1, compositing-1), apart from the effects that more "
         "elements declare."),
        [
            "clip-path", "clip", "filter", "backdrop-filter", "mix-blend-mode", "isolation", "mask-image", "mask-mode",
            "mask-repeat", "mask-position", "mask-size", "mask-origin", "mask-clip", "mask-composite",
            "mask-border-source", "mask-border-mode", "mask-border-slice", "mask-border-width", "mask-border-outset",
            "mask-border-repeat",
        ]),
    StyleGroup(
        "Animation", False, "which the presentation runs",
        "",
        [
            "transition-property", "transition-duration", "transition-timing-function", "transition-delay",
            "transition-behavior", "animation-name", "animation-duration", "animation-timing-function",
            "animation-delay", "animation-iteration-count", "animation-direction", "animation-fill-mode",
            "animation-play-state", "animation-composition",
        ]),
    StyleGroup(
        "Generated", False, "read by the presentation: generated content, counters, appearance, selection",
        ("Few elements declare generated content, counters or list styles, so these are groups of their own, which most "
         "styles share with their parent or the initial style. Appearance joins the generated content, since only "
         "controls declare it and the presentation reads it to generate their parts. So does user-select, which few "
         "elements declare either, and which decides whether the user can select an element's text."),
        [
            "content", "counter-reset", "counter-increment", "counter-set", "string-set", "appearance", "user-select",
        ]),
    StyleGroup(
        "List", True, "read by the presentation: list markers and quotes",
        "",
        [
            "list-style-type", "list-style-position", "list-style-image", "quotes",
        ]),
    StyleGroup(
        "Svg", True, "read by SVG: fills, strokes, markers and how shapes render",
        ("SVG's inherited properties: how shapes and text are filled, stroked, marked and rendered. An HTML element "
         "declares none of them, so its style shares its parent's group."),
        [
            "fill", "fill-rule", "fill-opacity", "clip-rule", "stroke", "stroke-opacity", "stroke-width",
            "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset",
            "paint-order", "marker-start", "marker-mid", "marker-end", "color-interpolation",
            "color-interpolation-filters", "shape-rendering", "text-rendering", "text-anchor",
        ]),
    StyleGroup(
        "SvgGeometry", False, "read by SVG: geometry, and the properties of resource elements",
        ("The geometry properties (SVG2/geometry.html), and the properties that only one kind of element reads: a "
         "gradient stop's color, a filter primitive's flood and light, a mask's type, and a graphic's vector effect. "
         "Each shape declares its own geometry, so these groups are seldom shared between shapes, and are kept apart "
         "from the inherited ones."),
        [
            "x", "y", "cx", "cy", "r", "rx", "ry", "d", "path-length", "vector-effect", "stop-color", "stop-opacity",
            "flood-color", "flood-opacity", "lighting-color", "mask-type",
        ]),
    StyleGroup(
        "Table", True, "read by layout: how a table's borders and captions are laid out",
        ("The inherited properties of tables. Few elements declare them, so most styles share their parent's group."),
        [
            "border-collapse", "border-spacing", "caption-side", "empty-cells",
        ]),
    StyleGroup(
        "Typography", True, "read by text and breaks: font features, word breaks, orphans and widows",
        ("The inherited properties that choose a font's features, let a word break (css-fonts-4/#font-rend-props, "
         "css-text-4/#word-break-property, css-text-4/#overflow-wrap-property), and keep lines together at a "
         "fragmentation break (css-break-4/#widows-orphans). Few elements declare them, so most styles share their "
         "parent's group."),
        [
            "font-feature-settings", "font-kerning", "font-variant-ligatures", "font-variant-caps",
            "font-variant-alternates", "font-variant-numeric", "font-variant-east-asian", "font-variant-position",
            "font-synthesis-small-caps", "font-synthesis-position", "word-break", "overflow-wrap", "orphans", "widows",
        ]),
    StyleGroup(
        "Grid", False, "read by layout: a container's templates and an item's placement",
        ("The grid properties (css-grid-1): a grid container's templates and automatic placement, and an item's "
         "placement. Few elements declare them, so most styles take the initial group."),
        [
            "grid-template-columns", "grid-template-rows", "grid-template-areas", "grid-auto-columns",
            "grid-auto-rows", "grid-auto-flow", "grid-column-start", "grid-column-end", "grid-row-start",
            "grid-row-end",
        ]),
    StyleGroup(
        "Fragmentation", False, "read by layout: breaks, columns and pages",
        ("The properties of fragmentation (css-break-4), multi-column layout (css-multicol-1) and named pages "
         "(css-page-3/#using-named-pages): where a box breaks across fragmentainers, what a multi-column container's "
         "columns are and how its rules paint, and which page a box goes on. Few elements declare them, so most styles "
         "take the initial group."),
        [
            "break-before", "break-after", "break-inside", "box-decoration-break", "column-count", "column-width",
            "column-fill", "column-span", "column-rule-width", "column-rule-style", "column-rule-color", "page",
            "page-break-before", "page-break-after", "page-break-inside",
        ]),
]

# The longhands that the engine parses but does not compute: their declarations are dropped from the computed style
# until a group holds them. gen_css.py fails if a longhand is in no group, is not flow-relative, and is not here, so a
# property that webref adds is placed on purpose.
NOT_COMPUTED = {
    "-webkit-box-align", "-webkit-box-flex", "-webkit-box-ordinal-group", "-webkit-box-orient", "-webkit-box-pack",
    "-webkit-line-clamp", "-webkit-text-fill-color", "-webkit-text-stroke-color", "-webkit-text-stroke-width",
    "-webkit-user-select", "anchor-name", "anchor-scope", "animation-delay-end", "animation-delay-start",
    "animation-range", "animation-range-center", "animation-range-end", "animation-range-start", "animation-timeline",
    "animation-trigger", "background-position-x", "background-position-y", "background-repeat-x", "background-repeat-y",
    "block-ellipsis", "block-step-align", "block-step-insert", "block-step-round", "block-step-size", "bookmark-label",
    "bookmark-level", "bookmark-state", "border-bottom-clip", "border-boundary", "border-image-outset",
    "border-image-repeat", "border-image-slice", "border-image-source", "border-image-width", "border-left-clip",
    "border-limit", "border-right-clip", "border-shape", "border-top-clip", "box-shadow", "box-shadow-blur",
    "box-shadow-color", "box-shadow-offset", "box-shadow-position", "box-shadow-spread", "box-snap", "caret-animation",
    "caret-shape", "column-height", "column-rule-break", "column-rule-inset-cap-end", "column-rule-inset-cap-start",
    "column-rule-inset-junction-end", "column-rule-inset-junction-start", "column-rule-visibility-items", "column-wrap",
    "container-name", "continue", "copy-into", "corner", "corner-block-end", "corner-block-start", "corner-bottom",
    "corner-bottom-left", "corner-bottom-right", "corner-end-end", "corner-end-start", "corner-inline-end",
    "corner-inline-start", "corner-left", "corner-right", "corner-start-end", "corner-start-start", "corner-top",
    "corner-top-left", "corner-top-right", "cue-after", "cue-before", "event-trigger", "event-trigger-name",
    "event-trigger-source", "fill-break", "fill-color", "fill-image", "fill-origin", "fill-position", "fill-repeat",
    "fill-size", "flex-line-count", "float-defer", "float-offset", "float-reference", "flow-from", "flow-into",
    "flow-tolerance", "font-language-override", "font-optical-sizing", "font-palette", "font-size-adjust",
    "font-synthesis-style", "font-synthesis-weight", "font-variant-emoji", "font-variation-settings",
    "footnote-display", "footnote-policy", "forced-color-adjust", "frame-sizing", "hanging-punctuation",
    "hyphenate-character", "hyphenate-limit-chars", "hyphenate-limit-last", "hyphenate-limit-lines",
    "hyphenate-limit-zone", "hyphens", "image-animation", "image-resolution", "initial-letter", "initial-letter-align",
    "initial-letter-wrap", "inline-sizing", "interest-delay-end", "interest-delay-start", "interpolate-size",
    "line-break", "line-clamp", "line-fit-edge", "line-grid", "line-height-step", "line-padding", "line-snap",
    "link-parameters", "margin-break", "margin-trim", "marker-side", "math-depth", "math-shift", "math-style",
    "max-lines", "min-intrinsic-sizing", "nav-down", "nav-left", "nav-right", "nav-up", "object-view-box",
    "offset-anchor", "offset-distance", "offset-path", "offset-position", "offset-rotate", "overflow-clip-margin",
    "overflow-clip-margin-block", "overflow-clip-margin-bottom", "overflow-clip-margin-inline",
    "overflow-clip-margin-left", "overflow-clip-margin-right", "overflow-clip-margin-top", "overlay", "pause-after",
    "pause-before", "pointer-timeline-axis", "pointer-timeline-name", "position-anchor", "position-area",
    "position-try-fallbacks", "position-try-order", "position-visibility", "print-color-adjust", "reading-flow",
    "reading-order", "region-fragment", "resize", "rest-after", "rest-before", "row-rule-break", "row-rule-color",
    "row-rule-inset-cap-end", "row-rule-inset-cap-start", "row-rule-inset-junction-end",
    "row-rule-inset-junction-start", "row-rule-style", "row-rule-visibility-items", "row-rule-width", "ruby-align",
    "ruby-merge", "ruby-overhang", "ruby-position", "rule-overlap", "scroll-axis-lock", "scroll-behavior",
    "scroll-initial-target", "scroll-margin-bottom", "scroll-margin-left", "scroll-margin-right", "scroll-margin-top",
    "scroll-marker-group", "scroll-padding-bottom", "scroll-padding-left", "scroll-padding-right", "scroll-padding-top",
    "scroll-snap-align", "scroll-snap-stop", "scroll-snap-type", "scroll-target-group", "scroll-timeline-axis",
    "scroll-timeline-name", "scrollbar-color", "scrollbar-gutter", "scrollbar-width", "shape-image-threshold",
    "shape-inside", "shape-margin", "shape-outside", "shape-padding", "slider-orientation", "spatial-navigation-action",
    "spatial-navigation-contain", "spatial-navigation-function", "speak", "speak-as", "stroke-align",
    "stroke-alignment", "stroke-break", "stroke-color", "stroke-dash-corner", "stroke-dash-justify",
    "stroke-dashadjust", "stroke-dashcorner", "stroke-image", "stroke-origin", "stroke-position", "stroke-repeat",
    "stroke-size", "tab-size", "text-autospace", "text-box-edge", "text-box-trim", "text-combine-upright",
    "text-decoration-inset", "text-decoration-skip", "text-decoration-skip-box", "text-decoration-skip-ink",
    "text-decoration-skip-self", "text-decoration-skip-spaces", "text-emphasis-color", "text-emphasis-position",
    "text-emphasis-skip", "text-emphasis-style", "text-fit", "text-group-align", "text-shadow", "text-size-adjust",
    "text-spacing", "text-spacing-trim", "text-underline-offset", "text-underline-position", "text-wrap-style",
    "timeline-scope", "timeline-trigger", "timeline-trigger-activation-range", "timeline-trigger-activation-range-end",
    "timeline-trigger-activation-range-start", "timeline-trigger-active-range", "timeline-trigger-active-range-end",
    "timeline-trigger-active-range-start", "timeline-trigger-name", "timeline-trigger-source", "touch-action",
    "transform-style", "trigger-scope", "view-timeline-axis", "view-timeline-inset", "view-timeline-name",
    "view-transition-class", "view-transition-group", "view-transition-name", "view-transition-scope", "voice-balance",
    "voice-duration", "voice-family", "voice-pitch", "voice-range", "voice-rate", "voice-stress", "voice-volume",
    "white-space-trim", "window-drag", "word-space-transform", "wrap-after", "wrap-before", "wrap-flow", "wrap-inside",
    "wrap-through", "zoom",
}

# ----------------------------------------------------------------------------------------------------------------------
# Presentation attributes (SVG2/styling.html#PresentationAttributes)

# The properties that every SVG element has a presentation attribute of, spelled as the property. SVG2 gives them in a
# table that webref does not extract. The geometry properties and transform, whose presentation attributes only some
# elements have or spell otherwise, are left to the code that maps them.
PRESENTATION = {
    "alignment-baseline", "baseline-shift", "clip-path", "clip-rule", "color", "color-interpolation",
    "color-interpolation-filters", "cursor", "direction", "display", "dominant-baseline", "fill", "fill-opacity",
    "fill-rule", "filter", "flood-color", "flood-opacity", "font-family", "font-size", "font-size-adjust",
    "font-stretch", "font-style", "font-variant", "font-weight", "glyph-orientation-vertical", "image-rendering",
    "letter-spacing", "lighting-color", "marker-end", "marker-mid", "marker-start", "mask", "mask-type", "opacity",
    "overflow", "paint-order", "pointer-events", "shape-rendering", "stop-color", "stop-opacity", "stroke",
    "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
    "stroke-opacity", "stroke-width", "text-anchor", "text-decoration", "text-overflow", "text-rendering",
    "transform-origin", "unicode-bidi", "vector-effect", "visibility", "white-space", "word-spacing", "writing-mode",
}

# ----------------------------------------------------------------------------------------------------------------------
# Canonical form (cssom-1/#serialize-a-css-value)
#
# A value is written with its components in canonical order. webref records it for a property as
# "per grammar": the children of a || or && in the order the grammar writes them. If a property's
# prose gives another order, it is written here. The generator exits on a property whose order is
# neither.

# The canonical order that a property's prose gives, as webref records it, where it matches the
# grammar's order.
CANONICAL_ORDER_PROSE = {
    # css-images-3/#propdef-object-position: the <position> grammar writes the horizontal
    # component first, and css-values-4/#position-serialization serializes it first.
    "object-position": ("the horizontal component of the <position>, "
                        "followed by the vertical component"),
}

# The || and && groups whose children mean something different in another order. A value keeps them
# as written. Keyed by the production each is the grammar of, or by the property whose grammar holds
# the one group.
WRITTEN_ORDER = {
    # css-anchor-position-1/#valdef-position-try-fallbacks-try-tactic: "If multiple keywords are
    # given, the transformations are composed in order".
    "<try-tactic>",
    # SVG2/painting.html#PaintOrder: "the order of the paint operations for painting the element is
    # as given, from left to right".
    "paint-order",
}

# The most times a component with omissions repeats, as kCssGrammarOmissionMaxRepeats is in css_grammar.h.
OMISSION_MAX_REPEATS = 4

# The value that a grammar component implies when a value omits it, as the prose says. Keyed by the
# production or property whose grammar holds it. The component is written as its grammar writes it,
# and is a child of a || or a node under a ? multiplier. If a value's component matches the implied
# value, the value is kept with the component omitted (cssom-1/#serialize-a-css-value), unless it is
# all that a required || holds. The implied value is a run of keywords and numbers, or one of several
# separated by |. A dimension is a zero, which is the same in every unit.
#
# A component repeated up to OMISSION_MAX_REPEATS times implies a list instead, with an entry for each repetition
# after the first. Each entry is the index of the repetition whose value it takes if omitted, or the
# implied value. A value omits its trailing repetitions while each equals what omitting it would
# give.
OMISSIONS = {
    # css-align-3/#typedef-baseline-position: "either “first” or “last”, respectively, defaulting
    # to “first”".
    "<baseline-position>": [("[ first | last ]?", "first")],
    # css-grid-2/#propdef-grid-auto-flow: "If neither row nor column is provided, row is assumed."
    "grid-auto-flow": [("[ row | column ]", "row")],
    # css-grid-2/#grid-placement-span-int: "If the <integer> is omitted, it defaults to 1."
    "<grid-line>": [("<integer [1,∞]>", "1")],
    # css-overflow-4/#propdef-overflow-clip-margin: the <length> "Defaults to zero if omitted."
    "overflow-clip-margin": [("<length>", "0px")],
    # css-scroll-snap-1/#propdef-scroll-snap-type: "If no strictness value is specified, proximity
    # is assumed."
    "scroll-snap-type": [("[ mandatory | proximity ]?", "proximity")],
    # css-scroll-snap-1/#propdef-scroll-snap-align: "If only one value is specified, the second
    # value defaults to the same value."
    "scroll-snap-align": [("[ none | start | end | center ]{1,2}", [0])],
    # css-images-4/#valdef-object-fit-scale-down: "Equivalent to contain scale-down."
    "object-fit": [("[ contain | cover ]", "contain")],
    # css-fonts-5/#propdef-font-size-adjust: the font metric to normalize, "defaulting to
    # ex-height".
    "font-size-adjust": [("[ ex-height | cap-height | ch-width | ic-width | ic-height ]?",
                          "ex-height")],
    # css-text-decor-4/#propdef-text-emphasis-position: "If [ right | left ] is omitted, it
    # defaults to right."
    "text-emphasis-position": [("[ right | left ]?", "right")],
    # css-flexbox-2/#valdef-flex-wrap-balance: "If neither are specified, it behaves as if wrap was
    # specified."
    "flex-wrap": [("[ wrap | wrap-reverse ]", "wrap")],
    # css-text-4/#propdef-hyphenate-limit-chars: "If the third value is missing, it is the same as
    # the second. If the second value is missing, then it is auto."
    "hyphenate-limit-chars": [("[ auto | <integer [0,∞]> ]{1,3}", ["auto", 1])],
    # css-backgrounds-3/#propdef-border-image-repeat: "If the second keyword is absent, it is
    # assumed to be the same as the first."
    "border-image-repeat": [("[ stretch | repeat | round | space ]{1,2}", [0])],
    # css-backgrounds-4/#typedef-repeat-style: "space Computes to space space", and so for each
    # <repetition>.
    "<repeat-style>": [("<repetition>{1,2}", [0])],
    # css-transforms-2/#propdef-translate: "When the second or third values are missing, they
    # default to 0px."
    "translate": [("<length>?", "0px"), ("[ <length-percentage> <length>? ]?", "0px")],
    # css-transforms-2/#propdef-scale: "If the Y value is not given, then it defaults to being the
    # same as the X value. If the Z value is not given, then it defaults to 1", and "There is no
    # difference in behavior between the third value being omitted and the third value being 1 or
    # 100%."
    "scale": [("[ <number> | <percentage> ]{1,3}", [0, "1 | 100%"])],
    # css-easing-2/#serialize-a-steps-functions: "If the function’s step position is end or
    # jump-end, append ")" to s and return s."
    "steps()": [("<step-position>?", "end | jump-end")],
    # css-counter-styles-3/#funcdef-symbols: "or symbolic if the system was omitted".
    "symbols()": [("<symbols-type>?", "symbolic")],
    # css-images-3/#linear-gradient-syntax: "It may be omitted; if so, it defaults to to bottom."
    "<linear-gradient-syntax>": [("[ <angle> | <zero> | to <side-or-corner> ]", "to bottom")],
    # css-images-3/#radial-gradient-syntax: the <radial-size> "If omitted it defaults to
    # farthest-corner", and the position "If this argument is omitted, it defaults to center",
    # which a <position> holds as two components (css-values-4/#position-serialization).
    "<radial-gradient-syntax>": [("<radial-size>", "farthest-corner"),
                                 ("[ at <position> ]?", "at center center")],
    # css-images-4/#conic-gradient-syntax: the <angle> "If omitted, defaults to 0deg", and the
    # position "If this argument is omitted, it defaults to center."
    "<conic-gradient-syntax>": [("[ from [ <angle> | <zero> ] ]?", "from 0deg"),
                                ("[ at <position> ]?", "at center center")],
    # css-shapes-1/#funcdef-basic-shape-circle, css-shapes-1/#funcdef-basic-shape-ellipse: the
    # <radial-size> "If omitted it defaults to closest-side."
    "circle()": [("[ <radial-extent> | <length-percentage [0,∞]> ]?", "closest-side")],
    # css-shapes-1/#funcdef-basic-shape-polygon: the <'fill-rule'> "Defaults to nonzero if omitted."
    "polygon()": [("<'fill-rule'>?", "nonzero")],
    # css-shapes-1/#funcdef-basic-shape-inset: "an omitted second or third value defaults to the
    # first, and an omitted fourth value defaults to the second."
    "inset()": [("<length-percentage>{1,4}", [0, 0, 1])],
    # css-shapes-1/#typedef-shape-arc-command: the angle "If omitted, this defaults to 0deg", the
    # <arc-sweep> "If omitted, this defaults to ccw", and the <arc-size> "If omitted, this
    # defaults to small."
    "<arc-command>": [("[ rotate <angle> ]?", "rotate 0deg"), ("<arc-sweep>?", "ccw"),
                      ("<arc-size>?", "small")],
    # css-fonts-4/#feature-tag-value: "A value of on is synonymous with 1", and "If the value is
    # omitted, a value of 1 is assumed."
    "<feature-tag-value>": [("[ <integer [0,∞]> | on | off ]?", "1 | on")],
    # motion-1/#typedef-ray-size: "If no <ray-size> is specified it defaults to closest-side."
    "ray()": [("<ray-size>?", "closest-side")],
}

# The productions whose match a value holds in a form that their prose gives. The matcher folds
# every reference to them into that form (CssGrammarFlag_Folds).
FOLDED_PRODUCTIONS = {
    # css-values-4/#position-serialization: "If only one component is specified: The implied center
    # keyword is added, and a 2-component value is serialized."
    "<position-one>",
    # css-values-4/#ratios: "<ratio> is always serialized with both components."
    "<ratio>",
    # mediaqueries-5/#mq-features: an <mf-value> is a value of the feature it is compared with, held
    # as that feature's grammar matches it.
    "<mf-plain>", "<mf-range>",
}

# The <percentage>s that a value holds as an equivalent <number> (the percentage divided by 100), if the prose says
# that a <number> is written in their place. Keyed by the production or property whose grammar holds them.
FRACTIONS = {
    # css-color-4/#serializing-opacity-values: "If the specified value for an opacity value matches
    # a literal <percentage-token> (i.e. does not use calc()) it should be serialized as the
    # equivalent <number>".
    "<opacity-value>",
    # css-transforms-2/#propdef-scale: "A <percentage> is equivalent to a <number>, [...] Numbers
    # are used during serialization of specified and computed values."
    "scale",
}

# Keyword sequences that a property's value may be written as, each with the shorter sequence that the value is held
# as, if the prose says that the two are the same value (cssom-1/#serialize-a-css-value). Each form is written in
# canonical order. The generator exits on a form that its property's grammar does not match in that order. A property
# whose value is a list holds each of its items this way.
KEYWORD_FORMS = {
    # css-box-4/#valdef-margin-trim-block: "Computes to block-start block-end", and "computed values
    # equivalent to block will serialize as that keyword".
    "margin-trim": [("block-start block-end", "block")],
    # css-backgrounds-4/#typedef-repeat-style: repeat-x "Computes to repeat no-repeat", and
    # repeat-y "Computes to no-repeat repeat".
    "background-repeat": [("repeat no-repeat", "repeat-x"), ("no-repeat repeat", "repeat-y")],
    "mask-repeat": [("repeat no-repeat", "repeat-x"), ("no-repeat repeat", "repeat-y")],
    # css-inline-3/#typedef-text-edge: "If only one value is specified, both edges are assigned that
    # same keyword if possible".
    "text-box-edge": [("text text", "text"), ("ideographic ideographic", "ideographic"),
                      ("ideographic-ink ideographic-ink", "ideographic-ink")],
}

# css-display-3/#the-display-properties: "serialization of equivalent display values uses the
# “Short display” column" of its table, keyed by the full display value.
DISPLAY_SHORT = {
    "block flow": "block", "block flow-root": "flow-root", "inline flow": "inline",
    "inline flow-root": "inline-block", "run-in flow": "run-in",
    "block flow list-item": "list-item", "inline flow list-item": "inline list-item",
    "block flex": "flex", "inline flex": "inline-flex", "block grid": "grid",
    "inline grid": "inline-grid", "inline ruby": "ruby", "block ruby": "block ruby",
    "block table": "table", "inline table": "inline-table",
}


def display_forms():
    """Returns every display value written with an outer or inner display type, or list-item, with the form that it is
    held as. The form is the Short display of its full display, or the full display without the types that it would
    default to. css-display-3/#typedef-display-outside: an omitted inner display type "defaults to flow".
    css-display-3/#typedef-display-inside: an omitted outer display type "defaults to block—except for ruby, which
    defaults to inline". css-display-3/#typedef-display-listitem: list-item's types default to flow and block."""
    forms = []
    for outside in (None, "block", "inline", "run-in"):
        for inside in (None, "flow", "flow-root", "table", "flex", "grid", "ruby"):
            for item in (False, True):
                if ((outside, inside, item) == (None, None, False)
                        or (item and inside not in (None, "flow", "flow-root"))):
                    continue
                inner = inside or "flow"
                default = "inline" if inner == "ruby" and not item else "block"
                outer = outside or default
                full = " ".join([outer, inner] + (["list-item"] if item else []))
                held = DISPLAY_SHORT.get(full)
                if held is None:
                    held = " ".join(([outer] if outer != default else [])
                                    + ([inner] if inner != "flow" else [])
                                    + (["list-item"] if item else []))
                written = " ".join(part
                                   for part in (outside, inside, "list-item" if item else None)
                                   if part)
                if written != held:
                    forms.append((written, held))
    return forms


def position_area_forms(tree):
    """css-anchor-position-1/#position-area-syntax: "If only a single keyword is given, it
    behaves as if the second keyword is span-all if the given keyword is unambigous about its axis;
    otherwise, it behaves as if the given keyword was repeated." Returns each two-keyword form with the single keyword
    that it equals, found from the || and repeated keywords of `tree`, the <position-area> grammar."""
    forms = []
    for node in walk(tree):
        if node.type == "anyof" and len(node.children) == 2:
            first, second = ([child.value for child in part.children] for part in node.children)
            both = set(first) & set(second)
            forms += [("%s span-all" % keyword, keyword) for keyword in first
                      if keyword not in both]
            forms += [("span-all %s" % keyword, keyword) for keyword in second
                      if keyword not in both]
            forms += [("%s %s" % (keyword, keyword), keyword) for keyword in first
                      if keyword in both]
        elif node.type == "oneof" and node.max == 2 and all(child.type == "keyword"
                                                            for child in node.children):
            forms += [("%s %s" % (child.value, child.value), child.value)
                      for child in node.children]
    return forms


def matches_in_order(g, tree, words):
    """Returns true if `tree` matches the keywords `words`, taking the children of a || or && in the order that the
    grammar writes them."""
    def ends(node, i, seen):
        once_ends = lambda start: ends_once(node, start, seen)
        out, frontier, taken = set(), {i}, 0
        if node.min == 0:
            out.add(i)
        while frontier and taken < min(node.max, len(words) + 1):
            frontier = {end for start in frontier for end in once_ends(start)}
            taken += 1
            if taken >= node.min:
                out |= frontier
        return out

    def ends_once(node, i, seen):
        if node.type == "keyword":
            return {i + 1} if i < len(words) and words[i] == node.value else set()
        if node.type in ("type", "property"):
            key = (node.type, normalize_ref(node.value) if node.type == "type" else node.value)
            tree = g.trees.get(key)
            return ends(tree, i, seen | {key}) if tree is not None and key not in seen else set()
        if node.type == "oneof":
            return {end for child in node.children for end in ends(child, i, seen)}
        if node.type in ("seq", "allof", "anyof"):
            at = {i}
            for child in node.children:
                after = {end for start in at for end in ends(child, start, seen)}
                at = after | at if node.type == "anyof" else after
            return at - ({i} if node.type == "anyof" else set())
        return set()

    return len(words) in ends(tree, 0, frozenset())


def keyword_forms(g):
    """Returns KEYWORD_FORMS with the forms of display and position-area added, each checked against its property's
    grammar."""
    forms = dict(KEYWORD_FORMS)
    forms["display"] = display_forms()
    forms["position-area"] = position_area_forms(g.trees[("type", "<position-area>")])
    for name, pairs in forms.items():
        tree = g.trees.get(("property", name))
        if tree is None:
            sys.exit("KEYWORD_FORMS: %s has no grammar" % name)
        for written, held in pairs:
            for form in (written, held):
                if len(form.split()) > 3 or not matches_in_order(g, tree, form.split()):
                    sys.exit("KEYWORD_FORMS: %s does not match %s in canonical order"
                             % (name, form))
            if len(held.split()) > len(written.split()):
                sys.exit("KEYWORD_FORMS: %s holds %s as more keywords than it is written with"
                         % (name, written))
    return forms

# ----------------------------------------------------------------------------------------------------------------------
# Shorthands (css-cascade-5/#shorthand)
#
# webref carries a shorthand's grammar but not the longhands that it sets, which its prose names. The longhands are
# written here in canonical order, with the form that the prose gives for which value each longhand takes. The forms
# are the members of the CssShorthand enum, and the comment on each member says what that form means.

SHORTHAND_FORMS = [
    ("Box", "One to four values, for the top, right, bottom and left. An omitted bottom or left takes the value "
            "of the side opposite it, and an omitted right takes the top's (css-box-4/#propdef-margin)."),
    ("Pair", "One or two values. An omitted second value copies the first "
             "(css-logical-1/#propdef-margin-block)."),
    ("Mirror", "The value of the one property that the grammar names. It sets the first run of longhands, and "
               "each further run of as many longhands takes the same values "
               "(css-logical-1/#propdef-border-block)."),
    ("Components", "Each component sets the longhand that the grammar marks it with, or the longhands of the "
                   "shorthand it is marked with. A longhand that no component sets takes its initial value "
                   "(css-cascade-5/#shorthand-property)."),
    ("Layers", "Components in comma-separated layers. A longhand takes one entry per layer: the value of the "
               "component that sets it, or its initial value where no component does "
               "(css-backgrounds-3/#layering)."),
    ("Background", "css-backgrounds-3/#propdef-background"),
    ("Mask", "css-masking-1/#the-mask"),
    ("ListStyle", "css-lists-3/#propdef-list-style"),
    ("BorderRadius", "css-backgrounds-3/#propdef-border-radius"),
    ("Flex", "css-flexbox-1/#propdef-flex"),
    ("Font", "css-fonts-4/#propdef-font"),
    ("FontSynthesis", "css-fonts-4/#propdef-font-synthesis"),
    ("FontVariant", "css-fonts-4/#propdef-font-variant"),
    ("GridArea", "css-grid-2/#propdef-grid-area"),
    ("GridLine", "css-grid-2/#propdef-grid-row"),
    ("GridTemplate", "css-grid-2/#propdef-grid-template"),
    ("Grid", "css-grid-2/#propdef-grid"),
    ("Place", "css-align-3/#propdef-place-content"),
    ("TextBox", "css-inline-3/#propdef-text-box"),
    ("WhiteSpace", "css-text-4/#propdef-white-space"),
    ("TextAlign", "css-text-3/#propdef-text-align"),
    ("GlyphOrientationVertical", "css-writing-modes-4/#glyph-orientation"),
    ("Same", "one value, which every longhand takes (css-gaps-1/#propdef-column-rule-inset-start)"),
    ("RuleInset", "css-gaps-1/#propdef-column-rule-inset"),
    ("GapRule", "css-gaps-1/#propdef-column-rule"),
    ("All", "every longhand but direction and unicode-bidi (css-cascade-5/#propdef-all)"),
]

# The forms whose expansion reads its values by position. A value that groups several values is one
# CssGrammarType_Longhand node, naming the first longhand.
POSITIONAL_FORMS = {"Box", "Pair"}

# The forms whose expansion reads which longhand each component of the grammar sets. The grammar
# of a shorthand of one of these forms wraps each such component in a CssGrammarType_Longhand node.
MARKED_FORMS = {"Mirror", "Components", "Layers", "Background", "Mask", "ListStyle", "Flex", "Font",
                "FontVariant", "GridTemplate", "Grid", "Place", "TextBox", "WhiteSpace", "RuleInset",
                "GapRule"}


def sides(prefix, suffix=""):
    return [prefix + side + suffix for side in ("top", "right", "bottom", "left")]


def corners(prefix, suffix, side=None):
    """Returns the corners clockwise from the top left, or the two corners of one side in the order that the side's
    shorthand takes them."""
    all = ["top-left", "top-right", "bottom-right", "bottom-left"]
    pairs = {"top": ["top-left", "top-right"], "right": ["top-right", "bottom-right"],
             "bottom": ["bottom-left", "bottom-right"], "left": ["top-left", "bottom-left"]}
    return [prefix + corner + suffix for corner in (pairs[side] if side else all)]


def ends(prefix, suffix=""):
    return [prefix + end + suffix for end in ("start", "end")]


def border_side(side):
    return ["border-%s-%s" % (side, part) for part in ("width", "style", "color")]


BORDER_IMAGE = ["border-image-source", "border-image-slice", "border-image-width",
                "border-image-outset", "border-image-repeat"]

MASK_BORDER = ["mask-border-source", "mask-border-slice", "mask-border-width", "mask-border-outset",
               "mask-border-repeat", "mask-border-mode"]

FONT_VARIANT = ["font-variant-ligatures", "font-variant-caps", "font-variant-alternates",
                "font-variant-numeric", "font-variant-east-asian", "font-variant-position",
                "font-variant-emoji"]

SHORTHANDS = {
    # css-box-4/#propdef-margin
    "margin": ("Box", sides("margin-")),
    # css-box-4/#propdef-padding
    "padding": ("Box", sides("padding-")),
    # css-position-3/#propdef-inset
    "inset": ("Box", sides("")),
    # css-backgrounds-3/#propdef-border-width
    "border-width": ("Box", sides("border-", "-width")),
    # css-backgrounds-3/#propdef-border-style
    "border-style": ("Box", sides("border-", "-style")),
    # css-backgrounds-3/#propdef-border-color
    "border-color": ("Box", sides("border-", "-color")),
    # css-scroll-snap-1/#propdef-scroll-margin
    "scroll-margin": ("Box", sides("scroll-margin-")),
    # css-scroll-snap-1/#propdef-scroll-padding
    "scroll-padding": ("Box", sides("scroll-padding-")),

    # css-logical-1/#propdef-margin-block
    "margin-block": ("Pair", ends("margin-block-")),
    # css-logical-1/#propdef-margin-inline
    "margin-inline": ("Pair", ends("margin-inline-")),
    # css-logical-1/#propdef-padding-block
    "padding-block": ("Pair", ends("padding-block-")),
    # css-logical-1/#propdef-padding-inline
    "padding-inline": ("Pair", ends("padding-inline-")),
    # css-position-3/#propdef-inset-block
    "inset-block": ("Pair", ends("inset-block-")),
    # css-position-3/#propdef-inset-inline
    "inset-inline": ("Pair", ends("inset-inline-")),
    # css-logical-1/#propdef-border-block-width
    "border-block-width": ("Pair", ends("border-block-", "-width")),
    # css-logical-1/#propdef-border-block-style
    "border-block-style": ("Pair", ends("border-block-", "-style")),
    # css-logical-1/#propdef-border-block-color
    "border-block-color": ("Pair", ends("border-block-", "-color")),
    # css-logical-1/#propdef-border-inline-width
    "border-inline-width": ("Pair", ends("border-inline-", "-width")),
    # css-logical-1/#propdef-border-inline-style
    "border-inline-style": ("Pair", ends("border-inline-", "-style")),
    # css-logical-1/#propdef-border-inline-color
    "border-inline-color": ("Pair", ends("border-inline-", "-color")),
    # css-scroll-snap-1/#propdef-scroll-margin-block
    "scroll-margin-block": ("Pair", ends("scroll-margin-block-")),
    # css-scroll-snap-1/#propdef-scroll-margin-inline
    "scroll-margin-inline": ("Pair", ends("scroll-margin-inline-")),
    # css-scroll-snap-1/#propdef-scroll-padding-block
    "scroll-padding-block": ("Pair", ends("scroll-padding-block-")),
    # css-scroll-snap-1/#propdef-scroll-padding-inline
    "scroll-padding-inline": ("Pair", ends("scroll-padding-inline-")),
    # css-overflow-3/#propdef-overflow
    "overflow": ("Pair", ["overflow-x", "overflow-y"]),
    # css-overscroll-1/#propdef-overscroll-behavior
    "overscroll-behavior": ("Pair", ["overscroll-behavior-x", "overscroll-behavior-y"]),
    "gap": ("Pair", ["row-gap", "column-gap"]),                            # css-gaps-1/#propdef-gap
    "contain-intrinsic-size": ("Pair",
                               # css-sizing-4/#propdef-contain-intrinsic-size
                               ["contain-intrinsic-width", "contain-intrinsic-height"]),

    # css-logical-1/#propdef-border-block
    "border-block": ("Mirror", border_side("block-start") + border_side("block-end")),
    # css-logical-1/#propdef-border-inline
    "border-inline": ("Mirror", border_side("inline-start") + border_side("inline-end")),

    # css-backgrounds-3/#propdef-border-top
    "border-top": ("Components", border_side("top")),
    "border-right": ("Components", border_side("right")),
    "border-bottom": ("Components", border_side("bottom")),
    "border-left": ("Components", border_side("left")),
    # css-logical-1/#propdef-border-block-start
    "border-block-start": ("Components", border_side("block-start")),
    "border-block-end": ("Components", border_side("block-end")),
    "border-inline-start": ("Components", border_side("inline-start")),
    "border-inline-end": ("Components", border_side("inline-end")),
    # css-backgrounds-3/#propdef-border: the four sides, and border-image reset.
    "border": ("Components", sides("border-", "-width") + sides("border-", "-style")
               + sides("border-", "-color") + BORDER_IMAGE),
    # css-backgrounds-3/#propdef-border-image
    "border-image": ("Components", BORDER_IMAGE),
    # css-ui-4/#propdef-outline
    "outline": ("Components", ["outline-width", "outline-style", "outline-color"]),
    "list-style": ("ListStyle",
                   # css-lists-3/#propdef-list-style
                   ["list-style-position", "list-style-image", "list-style-type"]),
    "text-decoration": ("Components", ["text-decoration-line", "text-decoration-thickness",
                                       # css-text-decor-4/#propdef-text-decoration
                                       "text-decoration-style", "text-decoration-color"]),
    # css-text-decor-3/#propdef-text-emphasis
    "text-emphasis": ("Components", ["text-emphasis-style", "text-emphasis-color"]),
    # css-flexbox-1/#propdef-flex-flow
    "flex-flow": ("Components", ["flex-direction", "flex-wrap"]),
    # css-multicol-2/#propdef-columns
    "columns": ("Components", ["column-width", "column-count", "column-height"]),
    # css-conditional-5/#propdef-container
    "container": ("Components", ["container-name", "container-type"]),
    # css-anchor-position-1/#propdef-position-try
    "position-try": ("Components", ["position-try-order", "position-try-fallbacks"]),
    # css-text-4/#propdef-text-wrap
    "text-wrap": ("Components", ["text-wrap-mode", "text-wrap-style"]),
    # css-ui-4/#propdef-caret
    "caret": ("Components", ["caret-color", "caret-animation", "caret-shape"]),
    "offset": ("Components", ["offset-position", "offset-path", "offset-distance", "offset-rotate",
                              # motion-1/#propdef-offset
                              "offset-anchor"]),
    "-webkit-text-stroke": ("Components",
                            # compat/#propdef--webkit-text-stroke
                            ["-webkit-text-stroke-width", "-webkit-text-stroke-color"]),

    # css-animations-1/#propdef-animation: animation-timeline and the animation-range longhands are
    # reset-only (css-animations-2/#animation-shorthand).
    "animation": ("Layers", ["animation-duration", "animation-timing-function", "animation-delay",
                             "animation-iteration-count", "animation-direction",
                             "animation-fill-mode", "animation-play-state", "animation-name",
                             "animation-timeline", "animation-range-start", "animation-range-end"]),
    "transition": ("Layers",
                   ["transition-property", "transition-duration", "transition-timing-function",
                              # css-transitions-1/#propdef-transition
                              "transition-delay", "transition-behavior"]),
    # css-masking-1/#the-mask: the layers, and mask-border reset.
    "mask": ("Mask", ["mask-image", "mask-position", "mask-size", "mask-repeat", "mask-origin",
                      "mask-clip", "mask-composite", "mask-mode"] + MASK_BORDER),
    # css-masking-1/#propdef-mask-border
    "mask-border": ("Components", MASK_BORDER),
    # scroll-animations-1/#propdef-scroll-timeline
    "scroll-timeline": ("Layers", ["scroll-timeline-name", "scroll-timeline-axis"]),
    "view-timeline": ("Layers",
                      ["view-timeline-name", "view-timeline-axis",
                       "view-timeline-inset"]),  # scroll-animations-1/#propdef-view-timeline

    "background": ("Background", ["background-image", "background-position", "background-size",
                                  "background-repeat", "background-attachment", "background-origin",
                                  "background-clip", "background-color"]),
    "border-radius": ("BorderRadius", ["border-top-left-radius", "border-top-right-radius",
                                       "border-bottom-right-radius", "border-bottom-left-radius"]),
    # css-borders-4/#corner-sizing-side-shorthands: the two corners of one side.
    "border-top-radius": ("BorderRadius", corners("border-", "-radius", "top")),
    "border-right-radius": ("BorderRadius", corners("border-", "-radius", "right")),
    "border-bottom-radius": ("BorderRadius", corners("border-", "-radius", "bottom")),
    "border-left-radius": ("BorderRadius", corners("border-", "-radius", "left")),
    "border-block-start-radius": ("BorderRadius", ["border-start-start-radius", "border-start-end-radius"]),
    "border-block-end-radius": ("BorderRadius", ["border-end-start-radius", "border-end-end-radius"]),
    "border-inline-start-radius": ("BorderRadius", ["border-start-start-radius", "border-end-start-radius"]),
    "border-inline-end-radius": ("BorderRadius", ["border-start-end-radius", "border-end-end-radius"]),
    "flex": ("Flex", ["flex-grow", "flex-shrink", "flex-basis"]),
    # css-fonts-4/#font-prop: the longhands the grammar names, and those it resets.
    "font": ("Font", ["font-style"] + FONT_VARIANT + ["font-weight", "font-width", "font-size",
                                                       "line-height", "font-family",
                                                       "font-feature-settings", "font-kerning",
                                                       "font-language-override",
                                                       "font-optical-sizing", "font-size-adjust",
                                                       "font-variation-settings", "font-palette"]),
    "font-synthesis": ("FontSynthesis", ["font-synthesis-weight", "font-synthesis-style",
                                         "font-synthesis-small-caps", "font-synthesis-position"]),
    "font-variant": ("FontVariant", FONT_VARIANT), "grid-area": ("GridArea",
                  ["grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end"]),
    "grid-row": ("GridLine", ["grid-row-start", "grid-row-end"]),
    "grid-column": ("GridLine", ["grid-column-start", "grid-column-end"]),
    "grid-template": ("GridTemplate",
                      ["grid-template-rows", "grid-template-columns", "grid-template-areas"]),
    "grid": ("Grid", ["grid-template-rows", "grid-template-columns", "grid-template-areas",
                      "grid-auto-rows", "grid-auto-columns", "grid-auto-flow"]),
    "place-content": ("Place", ["align-content", "justify-content"]),
    "place-items": ("Place", ["align-items", "justify-items"]),
    "place-self": ("Place", ["align-self", "justify-self"]),
    "text-box": ("TextBox", ["text-box-trim", "text-box-edge"]),
    "white-space": ("WhiteSpace", ["white-space-collapse", "text-wrap-mode", "white-space-trim"]),
    "text-align": ("TextAlign", ["text-align-all", "text-align-last"]),
    # css-writing-modes-4/#glyph-orientation: the obsolete property is a shorthand of text-orientation.
    "glyph-orientation-vertical": ("GlyphOrientationVertical", ["text-orientation"]),
    # css-speech-1/#propdef-cue
    "cue": ("Pair", ["cue-before", "cue-after"]),
    # css-speech-1/#propdef-pause
    "pause": ("Pair", ["pause-before", "pause-after"]),
    # css-speech-1/#propdef-rest
    "rest": ("Pair", ["rest-before", "rest-after"]),
    # css-ui-4/#propdef-interest-delay
    "interest-delay": ("Pair", ["interest-delay-start", "interest-delay-end"]),
    # css-borders-4/#propdef-corner-shape
    "corner-shape": ("Box", corners("corner-", "-shape")),
    # css-borders-4/#propdef-corner-top-shape
    "corner-top-shape": ("Pair", corners("corner-", "-shape", "top")),
    # css-borders-4/#propdef-corner-right-shape
    "corner-right-shape": ("Pair", corners("corner-", "-shape", "right")),
    # css-borders-4/#propdef-corner-bottom-shape
    "corner-bottom-shape": ("Pair", corners("corner-", "-shape", "bottom")),
    # css-borders-4/#propdef-corner-left-shape
    "corner-left-shape": ("Pair", corners("corner-", "-shape", "left")),
    "corner-block-start-shape": ("Pair",
                                 # css-borders-4/#propdef-corner-block-start-shape
                                 ["corner-start-start-shape", "corner-start-end-shape"]),
    "corner-block-end-shape": ("Pair",
                               # css-borders-4/#propdef-corner-block-end-shape
                               ["corner-end-start-shape", "corner-end-end-shape"]),
    "corner-inline-start-shape": ("Pair",
                                  # css-borders-4/#propdef-corner-inline-start-shape
                                  ["corner-start-start-shape", "corner-end-start-shape"]),
    "corner-inline-end-shape": ("Pair",
                                # css-borders-4/#propdef-corner-inline-end-shape
                                ["corner-start-end-shape", "corner-end-end-shape"]),
    # css-borders-4/#propdef-border-clip
    "border-clip": ("Mirror", sides("border-", "-clip")),
    # css-borders-4/#propdef-border-block-clip
    "border-block-clip": ("Mirror", ends("border-block-", "-clip")),
    # css-borders-4/#propdef-border-block-clip
    "border-inline-clip": ("Mirror", ends("border-inline-", "-clip")),
    # css-color-adjust-1/#propdef-color-adjust
    "color-adjust": ("Mirror", ["print-color-adjust"]),
    # svg2-draft/painting.html#MarkerShorthand
    "marker": ("Mirror", ["marker-start", "marker-mid", "marker-end"]),
    "block-step": ("Components", ["block-step-size", "block-step-insert", "block-step-align",
                                  # css-rhythm-1/#propdef-block-step
                                  "block-step-round"]),
    "vertical-align": ("Components",
                       # css-inline-3/#propdef-vertical-align
                       ["baseline-source", "alignment-baseline", "baseline-shift"]),
    # pointer-animations-1/#propdef-pointer-timeline
    "pointer-timeline": ("Layers", ["pointer-timeline-name", "pointer-timeline-axis"]),
    "size": ("Pair", ["width", "height"]),                              # css-sizing-4/#propdef-size
    # css-sizing-4/#propdef-min-size
    "min-size": ("Pair", ["min-width", "min-height"]),
    # css-sizing-4/#propdef-max-size
    "max-size": ("Pair", ["max-width", "max-height"]),

    "all": ("All", None),
}


def gap_rules(axis):
    """Returns the shorthands of css-gaps-1 that set the decorations of the gaps in one axis."""
    rule = lambda part: "%s-rule-%s" % (axis, part)
    inset = lambda part: rule("inset-" + part)
    return {
        # css-gaps-1/#propdef-column-rule-inset-cap
        rule("inset-cap"): ("Pair", [inset("cap-start"), inset("cap-end")]),
        rule("inset-junction"): ("Pair",
                                 # css-gaps-1/#propdef-column-rule-inset-junction
                                 [inset("junction-start"), inset("junction-end")]),
        # css-gaps-1/#propdef-column-rule-inset-start
        rule("inset-start"): ("Same", [inset("cap-start"), inset("junction-start")]),
        # css-gaps-1/#propdef-column-rule-inset-end
        rule("inset-end"): ("Same", [inset("cap-end"), inset("junction-end")]),
        rule("inset"): ("RuleInset", [inset("cap-start"), inset("cap-end"), inset("junction-start"),
                                      # css-gaps-1/#propdef-column-rule-inset
                                      inset("junction-end")]),
        # css-gaps-1/#propdef-column-rule
        "%s-rule" % axis: ("GapRule", [rule("width"), rule("style"), rule("color")]),
    }


SHORTHANDS.update(gap_rules("column"))
SHORTHANDS.update(gap_rules("row"))
# css-gaps-1/#propdef-rule: each of these sets the column and the row decorations to the same value.
SHORTHANDS.update(("rule" + part,
                   ("Mirror",
                    SHORTHANDS.get("column-rule" + part, (None, ["column-rule" + part]))[1]
                    + SHORTHANDS.get("row-rule" + part, (None, ["row-rule" + part]))[1]))
                  for part in ("", "-inset", "-inset-cap", "-inset-junction", "-inset-start",
                               "-inset-end", "-break", "-color", "-style", "-width",
                               "-visibility-items"))

# css-cascade-5/#propdef-all: the longhands that all does not set.
NOT_IN_ALL = {"direction", "unicode-bidi"}

# Shorthands that a declaration block never uses to write its longhands.
# css-sizing-4/#sizing-properties: width and height never serialize as size.
NOT_WRITTEN = {"size", "min-size", "max-size"}

# The longhand that a component of a shorthand's grammar sets, if the component is not a reference to that longhand and
# no single longhand's grammar is or offers it. Each entry gives the component as the grammar writes it, and the
# longhand that each occurrence of the component sets, in turn.
SHORTHAND_COMPONENTS = {
    # css-backgrounds-3/#propdef-border: each of the three sets all four sides.
    "border": {"<line-width>": ["border-width"], "<line-style>": ["border-style"],
               "<color>": ["border-color"]},
    # css-animations-2/#animation-shorthand: the delay is animation-delay's value.
    "animation": {"<'animation-delay-start'>": ["animation-delay"]},
    # css-transitions-1/#transition-shorthand-property: the first time is the duration and the
    # second the delay.
    "transition": {"<time>": ["transition-duration", "transition-delay"],
                   "none | <single-transition-property>": ["transition-property"]},
    # css-gaps-1/#typedef-gap-rule: each <gap-rule> sets one entry of the three lists.
    "column-rule": {"<line-width>": ["column-rule-width"], "<line-style>": ["column-rule-style"],
                    "<color>": ["column-rule-color"]},
    "row-rule": {"<line-width>": ["row-rule-width"], "<line-style>": ["row-rule-style"],
                 "<color>": ["row-rule-color"]},
    # css-gaps-1/#propdef-row-rule-inset is written with the column properties' grammars, and sets
    # the row properties.
    "row-rule-inset": {"<'column-rule-inset-cap'>": ["row-rule-inset-cap"],
                       "<'column-rule-inset-junction'>": ["row-rule-inset-junction"]},
    # css-masking-1/#the-mask: the first <geometry-box> is the origin, and the second the clip.
    "mask": {"<geometry-box>": ["mask-origin"], "<geometry-box> | no-clip": ["mask-clip"]},
    # css-inline-3/#propdef-vertical-align: first and last are values of baseline-source.
    "vertical-align": {"first | last": ["baseline-source"]},
}

# The initial value of a longhand, as the value serializes, if webref's value is prose or is not the serialization.
INITIAL_VALUES = {
    "animation-delay": "0s",          # css-animations-1/#propdef-animation-delay
    "column-width": "auto",           # css-multicol-1/#propdef-column-width
    "font-family": "initial",         # css-fonts-4/#font-family-prop: depends on the user agent
    "text-emphasis-position": "over", # css-text-decor-4/#text-emphasis-position-property: `right` is implied
    "background-color": "transparent",
    "border-top-color": "currentcolor",
    "border-right-color": "currentcolor",
    "border-bottom-color": "currentcolor",
    "border-left-color": "currentcolor",
    # SVG 2's stop properties, whose property tables webref does not read (PROPERTY_GRAMMARS).
    "stop-color": "black",
    "stop-opacity": "1",
}

# css-values-4/#lengths: webref writes a zero length as the unitless 0, and it serializes in px.
INITIAL_VALUES.update((name, "0px") for name in
                      sides("margin-") + ends("margin-block-") + ends("margin-inline-")
                      + sides("padding-") + ends("padding-block-") + ends("padding-inline-")
                      + sides("scroll-margin-") + ends("scroll-margin-block-")
                      + ends("scroll-margin-inline-")
                      + ["border-top-left-radius", "border-top-right-radius",
                         "border-bottom-right-radius", "border-bottom-left-radius",
                         "offset-distance", "-webkit-text-stroke-width"]
                      + ["%s-rule-inset-%s-%s" % (axis, part, end) for axis in ("column", "row")
                         for part in ("cap", "junction") for end in ("start", "end")])


# Idents that the engine matches by name but that appear in no grammar, so nothing else would add
# them to the table. `important` is syntax rather than a value, and css-syntax-3/#consume-declaration
# matches it. `decimal` is one of the counter styles that css-counter-styles-3/#the-counter-style-rule
# forbids a @counter-style rule to define, and the only one no grammar writes. `odd` and `even` are
# the keywords of <a-n-plus-b>, which css-syntax-3/#anb-microsyntax parses.
EXTRA_KEYWORDS = ["important", "decimal", "odd", "even"]

# The <system-color> that each <deprecated-color> computes to (css-color-4/#deprecated-system-colors), which the prose
# of its definition gives and webref does not carry.
DEPRECATED_SYSTEM_COLORS = {
    "activeborder":        "buttonborder",
    "activecaption":       "canvas",
    "appworkspace":        "canvas",
    "background":          "canvas",
    "buttonhighlight":     "buttonface",
    "buttonshadow":        "buttonface",
    "captiontext":         "canvastext",
    "inactiveborder":      "buttonborder",
    "inactivecaption":     "canvas",
    "inactivecaptiontext": "graytext",
    "infobackground":      "canvas",
    "infotext":            "canvastext",
    "menu":                "canvas",
    "menutext":            "canvastext",
    "scrollbar":           "canvas",
    "threeddarkshadow":    "buttonborder",
    "threedface":          "buttonface",
    "threedhighlight":     "buttonborder",
    "threedlightshadow":   "buttonborder",
    "threedshadow":        "buttonborder",
    "window":              "canvas",
    "windowframe":         "buttonborder",
    "windowtext":          "canvastext",
}

# The sRGB value of each <named-color>. webref carries the names. css-color-4/#named-colors writes
# the values in a separate table that the extract does not cover.
NAMED_COLOR_RGB = {
    "aliceblue":            "f0f8ff",
    "antiquewhite":         "faebd7",
    "aqua":                 "00ffff",
    "aquamarine":           "7fffd4",
    "azure":                "f0ffff",
    "beige":                "f5f5dc",
    "bisque":               "ffe4c4",
    "black":                "000000",
    "blanchedalmond":       "ffebcd",
    "blue":                 "0000ff",
    "blueviolet":           "8a2be2",
    "brown":                "a52a2a",
    "burlywood":            "deb887",
    "cadetblue":            "5f9ea0",
    "chartreuse":           "7fff00",
    "chocolate":            "d2691e",
    "coral":                "ff7f50",
    "cornflowerblue":       "6495ed",
    "cornsilk":             "fff8dc",
    "crimson":              "dc143c",
    "cyan":                 "00ffff",
    "darkblue":             "00008b",
    "darkcyan":             "008b8b",
    "darkgoldenrod":        "b8860b",
    "darkgray":             "a9a9a9",
    "darkgreen":            "006400",
    "darkgrey":             "a9a9a9",
    "darkkhaki":            "bdb76b",
    "darkmagenta":          "8b008b",
    "darkolivegreen":       "556b2f",
    "darkorange":           "ff8c00",
    "darkorchid":           "9932cc",
    "darkred":              "8b0000",
    "darksalmon":           "e9967a",
    "darkseagreen":         "8fbc8f",
    "darkslateblue":        "483d8b",
    "darkslategray":        "2f4f4f",
    "darkslategrey":        "2f4f4f",
    "darkturquoise":        "00ced1",
    "darkviolet":           "9400d3",
    "deeppink":             "ff1493",
    "deepskyblue":          "00bfff",
    "dimgray":              "696969",
    "dimgrey":              "696969",
    "dodgerblue":           "1e90ff",
    "firebrick":            "b22222",
    "floralwhite":          "fffaf0",
    "forestgreen":          "228b22",
    "fuchsia":              "ff00ff",
    "gainsboro":            "dcdcdc",
    "ghostwhite":           "f8f8ff",
    "gold":                 "ffd700",
    "goldenrod":            "daa520",
    "gray":                 "808080",
    "green":                "008000",
    "greenyellow":          "adff2f",
    "grey":                 "808080",
    "honeydew":             "f0fff0",
    "hotpink":              "ff69b4",
    "indianred":            "cd5c5c",
    "indigo":               "4b0082",
    "ivory":                "fffff0",
    "khaki":                "f0e68c",
    "lavender":             "e6e6fa",
    "lavenderblush":        "fff0f5",
    "lawngreen":            "7cfc00",
    "lemonchiffon":         "fffacd",
    "lightblue":            "add8e6",
    "lightcoral":           "f08080",
    "lightcyan":            "e0ffff",
    "lightgoldenrodyellow": "fafad2",
    "lightgray":            "d3d3d3",
    "lightgreen":           "90ee90",
    "lightgrey":            "d3d3d3",
    "lightpink":            "ffb6c1",
    "lightsalmon":          "ffa07a",
    "lightseagreen":        "20b2aa",
    "lightskyblue":         "87cefa",
    "lightslategray":       "778899",
    "lightslategrey":       "778899",
    "lightsteelblue":       "b0c4de",
    "lightyellow":          "ffffe0",
    "lime":                 "00ff00",
    "limegreen":            "32cd32",
    "linen":                "faf0e6",
    "magenta":              "ff00ff",
    "maroon":               "800000",
    "mediumaquamarine":     "66cdaa",
    "mediumblue":           "0000cd",
    "mediumorchid":         "ba55d3",
    "mediumpurple":         "9370db",
    "mediumseagreen":       "3cb371",
    "mediumslateblue":      "7b68ee",
    "mediumspringgreen":    "00fa9a",
    "mediumturquoise":      "48d1cc",
    "mediumvioletred":      "c71585",
    "midnightblue":         "191970",
    "mintcream":            "f5fffa",
    "mistyrose":            "ffe4e1",
    "moccasin":             "ffe4b5",
    "navajowhite":          "ffdead",
    "navy":                 "000080",
    "oldlace":              "fdf5e6",
    "olive":                "808000",
    "olivedrab":            "6b8e23",
    "orange":               "ffa500",
    "orangered":            "ff4500",
    "orchid":               "da70d6",
    "palegoldenrod":        "eee8aa",
    "palegreen":            "98fb98",
    "paleturquoise":        "afeeee",
    "palevioletred":        "db7093",
    "papayawhip":           "ffefd5",
    "peachpuff":            "ffdab9",
    "peru":                 "cd853f",
    "pink":                 "ffc0cb",
    "plum":                 "dda0dd",
    "powderblue":           "b0e0e6",
    "purple":               "800080",
    "rebeccapurple":        "663399",
    "red":                  "ff0000",
    "rosybrown":            "bc8f8f",
    "royalblue":            "4169e1",
    "saddlebrown":          "8b4513",
    "salmon":               "fa8072",
    "sandybrown":           "f4a460",
    "seagreen":             "2e8b57",
    "seashell":             "fff5ee",
    "sienna":               "a0522d",
    "silver":               "c0c0c0",
    "skyblue":              "87ceeb",
    "slateblue":            "6a5acd",
    "slategray":            "708090",
    "slategrey":            "708090",
    "snow":                 "fffafa",
    "springgreen":          "00ff7f",
    "steelblue":            "4682b4",
    "tan":                  "d2b48c",
    "teal":                 "008080",
    "thistle":              "d8bfd8",
    "tomato":               "ff6347",
    "turquoise":            "40e0d0",
    "violet":               "ee82ee",
    "wheat":                "f5deb3",
    "white":                "ffffff",
    "whitesmoke":           "f5f5f5",
    "yellow":               "ffff00",
    "yellowgreen":          "9acd32",
}

# Punctuation that shows up in css-syntax token type names: <(-token>, <{-token>, ...
PUNCT_NAMES = {
    "(": "OpenParen", ")": "CloseParen",
    "[": "OpenBracket", "]": "CloseBracket",
    "{": "OpenBrace", "}": "CloseBrace",
}


def spec_rank(spec):
    m = re.fullmatch(r"(.*?)-(\d+)", spec)
    series, level = (m.group(1), int(m.group(2))) if m else (spec, 0)
    if spec in LEGACY_SPECS:
        return (-100, level, spec)
    return (SPEC_PRIORITY.get(series, 0), level, spec)


def series_of(spec):
    m = re.fullmatch(r"(.*?)-(\d+)", spec)
    return m.group(1) if m else spec


# ----------------------------------------------------------------------------------------------------------------------
# Name mangling

def camel(name):
    out = []
    for p in [p for p in name.split("-") if p]:
        out.append(PUNCT_NAMES[p] if p in PUNCT_NAMES else p[0].upper() + p[1:])
    return "".join(out)


def property_ident(name):
    # background-color -> PrCssProperty_BackgroundColor, -webkit-box-align ->
    # PrCssProperty_WebkitBoxAlign
    return "PrCssProperty_" + camel(name)


def at_rule_ident(name):
    # @font-face -> CssAtRule_FontFace
    return "CssAtRule_" + camel(name[1:])


def descriptor_ident(key):
    # ("@font-face", "unicode-range") -> PrCssDescriptor_FontFaceUnicodeRange
    rule, name = key
    return "PrCssDescriptor_" + camel(rule[1:]) + camel(name.lstrip("@"))


def value_ident(name, kind):
    # <bg-image> -> CssValueDef_BgImage, counter() -> CssValueDef_CounterFn. An instance of a
    # generic production is named for the names in its parameter: <boolean-expr[ <if-test> ]> ->
    # CssValueDef_BooleanExprOfIfTest
    instance = re.fullmatch(r"<([-\w]+)\[(.*)\]>", name)
    if instance:
        words = re.findall(r"[a-z][-a-z0-9]*", instance.group(2))
        return "CssValueDef_" + camel(instance.group(1)) + "Of" + "".join(camel(w) for w in words)
    if kind == "function":
        return "CssValueDef_" + camel(name[:-2]) + "Fn"
    return "CssValueDef_" + camel(name[1:-1])


# ----------------------------------------------------------------------------------------------------------------------
# Loading

class Entry:
    """One property or value, merged across every spec that defines it."""

    def __init__(self, name, kind):
        self.name = name
        self.kind = kind          # "property" | "type" | "function"
        self.decls = []           # [(rank, spec, dict)]
        self.new_values = []      # [(spec, str)]
        self.amendment = None     # a grammar from GRAMMAR_AMENDMENTS that replaces the spec's grammar

    def add(self, spec, d):
        self.decls.append((spec_rank(spec), spec, d))
        if "newValues" in d:
            self.new_values.append((spec, d["newValues"]))

    def _best(self):
        with_value = [d for d in self.decls if "value" in d[2]
                      and (d[1], self.name) not in DEFERRED_DEFINITIONS]
        return max(with_value, key=lambda t: t[0]) if with_value else None

    def grammar(self):
        """Returns the grammar string, or None if the spec defines the production in prose."""
        if self.amendment is not None:
            return self.amendment
        if self.name in PROSE_PRODUCTIONS and self.kind == "type":
            return None
        best = self._best()
        if best is None:
            return " | ".join(self.sub_values()) if self.name in KEYWORD_PRODUCTIONS else None
        value = best[2]["value"]
        # Fold in level deltas (`newValues`) contributed by other specs.
        for nv_spec, nv in self.new_values:
            if {part.strip() for part in nv.split("|")} <= ANCHOR_NEW_VALUES:
                continue
            if nv_spec == best[1] or nv in value:
                continue
            # css-values-5/#typedef-boolean-expr: <general-enclosed> is the expression that a UA does not know, so it
            # stays last, after the expressions that a level adds.
            if value.endswith("| <general-enclosed>"):
                value = value[:-len("<general-enclosed>")] + nv + " | <general-enclosed>"
            else:
                value += " | " + nv
        return value

    def source_spec(self):
        best = self._best()
        return best[1] if best else max(self.decls, key=lambda t: t[0])[1]

    def sub_values(self):
        """Returns the values that the spec lists under this production. A prose production enumerates its keywords
        this way."""
        for _, _, d in sorted(self.decls, key=lambda t: t[0], reverse=True):
            if d.get("values"):
                return [v["name"] for v in d["values"] if v.get("type") == "value"]
        return []

    def range_feature(self):
        """Returns true if webref types the feature as a range, which a query compares."""
        return any(d.get("type") == "range" for _, _, d in self.decls)

    def animatable(self):
        """Returns true if the property animates, according to webref's animation type. Every type except "not
        animatable" animates, and a shorthand's own entry defers to its longhands."""
        best = self._best()
        return (best[2] if best else {}).get("animationType") != "not animatable"

    def discrete(self):
        """Returns true if the property animates discretely, according to webref's animation type or DISCRETE."""
        best = self._best()
        return self.name in DISCRETE or (best[2] if best else {}).get("animationType") == "discrete"

    def repeatable_list(self):
        """Returns true if the property animates as a repeatable list, according to webref's animation type."""
        best = self._best()
        return self.name in REPEATABLE_LIST or (best[2] if best else {}).get("animationType", "").startswith(
            "repeatable list")

    def canonical_order(self):
        """Returns the canonical order that webref records for a property, in lowercase, or None."""
        best = self._best()
        order = best[2].get("canonicalOrder") if best else None
        return order.lower() if order else None

    def alias_of(self):
        for _, _, d in self.decls:
            if "legacyAliasOf" in d:
                return d["legacyAliasOf"]
        return None


def load(webref_dir):
    css_dir = os.path.join(webref_dir, "ed", "css")
    props = {}
    values = {}
    descs = {}
    selectors = {}
    rules = {}
    within = dict(AT_RULE_WITHIN)
    at_rule_keywords = set()
    conflicts = collections.defaultdict(set)
    missing = {}

    def want_at_rule(spec, d):
        e = rules.get(d["name"])
        if e is None:
            e = rules[d["name"]] = Entry(d["name"], "at-rule")
        e.add(spec, d)

    def want_value(spec, d):
        if (d["name"], spec) in PLACEHOLDER_DEFINITIONS:
            return
        e = values.get(d["name"])
        if e is None:
            e = values[d["name"]] = Entry(d["name"], d["type"])
        e.add(spec, d)
        for child in d.get("values", []):
            if child["type"] != "value":       # plain keywords need no entry of their own
                want_value(spec, child)

    files = sorted(f for f in os.listdir(css_dir) if f.endswith(".json"))
    if not files:
        sys.exit("no JSON found in %s" % css_dir)

    for fn in files:
        spec = fn[:-5]
        with open(os.path.join(css_dir, fn), encoding="utf-8") as f:
            d = json.load(f)
        for p in d.get("properties", []):
            if (p["name"], spec) in PLACEHOLDER_DEFINITIONS:
                continue
            e = props.get(p["name"])
            if e is None:
                e = props[p["name"]] = Entry(p["name"], "property")
            e.add(spec, p)
            for child in p.get("values", []):
                if child["type"] != "value":
                    want_value(spec, child)
        for v in d.get("values", []):
            want_value(spec, v)
        for warning in d.get("warnings", []):
            if (warning["msg"] == "Missing definition"
                    and (spec, warning["name"]) in MISSING_DEFINITIONS):
                missing[(spec, warning["name"])] = warning["value"]
        for s in d.get("selectors", []):
            e = selectors.get(s["name"])
            if e is None:
                e = selectors[s["name"]] = Entry(s["name"], "selector")
            e.add(spec, s)
            for child in s.get("values", []):
                # A production that a pseudo's argument names, such as <form-control-identifier> for ::picker().
                if child["type"] == "type":
                    want_value(spec, child)
        for rule in d.get("atrules", []):
            if rule["name"] in NOT_AT_RULES:
                continue
            want_at_rule(spec, rule)
            for child in rule.get("values", []):
                # A keyword that an at-rule's prose names, as @media names its media types.
                if child["type"] == "value":
                    if re.fullmatch(r"[-\w]+", child["name"]):
                        at_rule_keywords.add(child["name"].lower())
                else:
                    want_value(spec, child)
            for descriptor in rule.get("descriptors", []):
                # @font-feature-values lists the at-rules that its block holds among its descriptors. They are separate
                # at-rules, valid inside it.
                if descriptor["name"].startswith("@"):
                    want_at_rule(spec, descriptor)
                    within[descriptor["name"]] = rule["name"]
                    continue
                key = (rule["name"], descriptor["name"])
                e = descs.get(key)
                if e is None:
                    e = descs[key] = Entry(descriptor["name"], "descriptor")
                e.add(spec, descriptor)
                for child in descriptor.get("values", []):
                    if child["type"] != "value":
                        want_value(spec, child)

    for key in BOUNDED_DESCRIPTORS:
        if key not in descs:
            sys.exit("%s %s: BOUNDED_DESCRIPTORS is stale" % key)
    for key, target in DESCRIPTOR_ALIASES.items():
        if key in descs or (key[0], target) not in descs:
            sys.exit("%s %s: DESCRIPTOR_ALIASES is stale" % key)
        descs[key] = Entry(key[1], "descriptor")
        descs[key].add(descs[(key[0], target)].source_spec(),
                       {"name": key[1], "legacyAliasOf": target})

    # Report cross-module conflicts that the priority table does not resolve.
    for table in (props, values, descs, rules):
        for e in table.values():
            series = {series_of(s) for _, s, d in e.decls
                      if "value" in d and s not in LEGACY_SPECS}
            if len(series) > 1:
                ranks = sorted(SPEC_PRIORITY.get(s, 0) for s in series)
                if ranks[-1] == ranks[-2]:
                    conflicts[tuple(sorted(series))].add(e.name)
    for key in MISSING_DEFINITIONS:
        if key not in missing:
            sys.exit("%s %s: webref reports no missing definition, and MISSING_DEFINITIONS is stale"
                     % key)
        spec, name = key
        text = missing[key]
        if text.startswith("..."):
            values[name].new_values.append((spec, text.lstrip(". |")))
        elif name in values:
            sys.exit("%s: defined now, and MISSING_DEFINITIONS is stale" % name)
        else:
            values[name] = Entry(name, "type")
            values[name].add(spec, {"name": name, "type": "type", "value": text})
    for spec, name in DEFERRED_DEFINITIONS:
        entry = props.get(name) or values.get(name)
        if entry is None or not any(s == spec and "value" in d for _, s, d in entry.decls):
            sys.exit("%s %s: webref has no such definition now, and DEFERRED_DEFINITIONS is stale"
                     % (spec, name))
    for name, text in PROSE_GRAMMARS.items():
        if values[name].grammar() is not None:
            sys.exit("%s: webref has a grammar now, and PROSE_GRAMMARS is stale" % name)
        values[name].amendment = text
    for name, text in GRAMMAR_AMENDMENTS.items():
        values[name].amendment = text
    for name, text in PROPERTY_GRAMMARS.items():
        if props[name].grammar() is not None:
            sys.exit("%s: webref has a grammar now, and PROPERTY_GRAMMARS is stale" % name)
        props[name].amendment = text
    for name, (written, text) in PROPERTY_AMENDMENTS.items():
        if props[name].grammar() != written:
            sys.exit("%s: webref's grammar is %s now, and PROPERTY_AMENDMENTS is stale" % (name, props[name].grammar()))
        props[name].amendment = text
    for name, extensions in GRAMMAR_EXTENSIONS.items():
        for spec, text in extensions:
            values[name].new_values.append((spec, text))
    admit_component_keywords(values)
    # A spec may write an at-rule's grammar as a type: css-navigation-1 gives @location as
    # <location-rule>.
    for e in values.values():
        text = e.grammar()
        m = re.match(r"(@[-\w]+)\s", text or "")
        if m and m.group(1) in rules and rules[m.group(1)].grammar() is None:
            rules[m.group(1)].add(e.source_spec(), {"name": m.group(1), "value": text})
    for name in KEYWORD_PRODUCTIONS:
        if values[name]._best() is not None or not values[name].sub_values():
            sys.exit("%s: webref has a grammar now or no keywords, and KEYWORD_PRODUCTIONS is stale"
                     % name)
    for name, text in AT_RULE_GRAMMARS.items():
        if rules[name].grammar() is not None:
            sys.exit("%s: webref has a grammar now, and AT_RULE_GRAMMARS is stale" % name)
        rules[name].amendment = text
    return props, values, descs, selectors, AtRules(rules, within, at_rule_keywords), conflicts


def admit_component_keywords(values):
    """Amends each relative color production so that every component that accepts `none` also accepts a component
    keyword. A separate prose production parses the keyword."""
    for name in RELATIVE_COLOR_PRODUCTIONS:
        text = values[name].grammar()
        amended, count = re.subn(r"\|\s*none\s*\]", "| none | %s ]" % COMPONENT_KEYWORD, text)
        if count == 0 or count != len(re.findall(r"\bnone\b", text)):
            sys.exit("%s: a `none` is not the last alternative of a component\n  %s" % (name, text))
        values[name].amendment = amended
    entry = values[COMPONENT_KEYWORD] = Entry(COMPONENT_KEYWORD, "type")
    entry.add("css-color-5", {"name": COMPONENT_KEYWORD, "type": "type"})


# ----------------------------------------------------------------------------------------------------------------------
# Value definition syntax (css-values-4/#value-defs)
#
# Grammar strings are parsed here and emitted as a flat node array, so the engine never parses
# grammar text at runtime.

VDS_TOKEN = re.compile(r"""
      \s+
    | <'[-\w]+'>                        # property reference
    | <@[-\w]+>                         # at-rule reference
    | <[-\w(){}\[\]]+(?:\s*\[[^\]]*\])?> # type reference, with optional range
    | \|\| | &&                         # combinators
    | \{\s*\d+(?:\s*,\s*\d*)?\s*\}      # {A,B} multiplier
    | [-\w]+\(                          # function
    | @[-\w]+                           # at-keyword
    | '[^']*'                           # quoted literal
    | [-\w]+                            # keyword
    | .                                 # punctuation literal
""", re.X)

VDS_STOP = {"]", "|", "||", "&&", None}

INF = float("inf")


class Node:
    __slots__ = ("type", "value", "children", "min", "max", "comma", "required", "range")

    def __init__(self, type, value=None, children=()):
        # keyword|literal|atkeyword|atrule|type|property|oneof|anyof|allof|seq|function|longhand
        self.type = type
        self.value = value
        self.children = list(children)
        self.min = 1
        self.max = 1
        self.comma = False
        self.required = False
        self.range = None           # (min, max) with None for an unbounded side


def parse_bound(text):
    text = text.strip()
    if "∞" in text:
        return -INF if text.startswith("-") else INF
    m = re.match(r"[-+]?[\d.]+", text)
    return float(m.group(0))


class GrammarParser:
    def __init__(self, text):
        self.toks = [t for t in (m.group(0) for m in VDS_TOKEN.finditer(text)) if not t.isspace()]
        self.pos = 0
        self.depth = 0   # open function or parenthesis groups

    def peek(self):
        return self.toks[self.pos] if self.pos < len(self.toks) else None

    def next(self):
        t = self.peek()
        self.pos += 1
        return t

    def multiplier_next(self):
        tok = self.peek()
        return tok in ("?", "*", "+", "#", "!") or bool(
            tok and re.fullmatch(r"\{\s*\d+(?:\s*,\s*\d*)?\s*\}", tok))

    def expect(self, tok):
        got = self.next()
        if got != tok:
            raise SyntaxError("expected %r, got %r" % (tok, got))

    def parse(self):
        node = self.one_of()
        if self.peek() is not None:
            raise SyntaxError("trailing %r" % self.peek())
        return node

    def combinator(self, sep, type, operand):
        parts = [operand()]
        while self.peek() == sep:
            self.next()
            parts.append(operand())
        return parts[0] if len(parts) == 1 else Node(type, children=parts)

    def one_of(self):
        return self.combinator("|", "oneof", self.any_of)

    def any_of(self):
        return self.combinator("||", "anyof", self.all_of)

    def all_of(self):
        return self.combinator("&&", "allof", self.sequence)

    def sequence(self):
        parts = []
        # An unmatched ')' is a literal: <general-enclosed> writes one out, because
        # <function-token> carries the opening paren.
        stop = VDS_STOP | {")"} if self.depth else VDS_STOP
        while self.peek() not in stop:
            parts.append(self.term())
        if not parts:
            raise SyntaxError("empty sequence at %r" % self.peek())
        return parts[0] if len(parts) == 1 else Node("seq", children=parts)

    def term(self):
        node = self.primary()
        while True:
            tok = self.peek()
            if tok == "?":
                node.min = 0
                node.max = max(node.max, 1)
            elif tok == "*":
                node.min, node.max = 0, INF
            elif tok == "+":
                node.min, node.max = 1, INF
            elif tok == "#":
                node.comma = True
                if (node.min, node.max) == (1, 1):
                    node.max = INF
            elif tok == "!":
                node.required = True
            elif tok and re.fullmatch(r"\{\s*\d+(?:\s*,\s*\d*)?\s*\}", tok):
                bounds = [b.strip() for b in tok[1:-1].split(",")]
                node.min = int(bounds[0])
                node.max = (node.min if len(bounds) == 1
                            else INF if not bounds[1] else int(bounds[1]))
            else:
                return node
            self.next()

    def primary(self):
        tok = self.next()
        if tok is None:
            raise SyntaxError("unexpected end")
        if tok == "[":
            node = self.one_of()
            self.expect("]")
            # A multiplier on a group whose content has one of its own multiplies the group:
            # [ <x>? ]# is a list of optional items, not an optional list.
            if not once(node) and self.multiplier_next():
                node = Node("seq", children=[node])
            return node
        if tok == "(":
            # A literal parenthesis pair, as in ( <anchored-query> ). The parentheses are tokens that the value must
            # contain, unlike [ ], which only groups.
            self.depth += 1
            inner = self.one_of()
            self.depth -= 1
            self.expect(")")
            return Node("seq", children=[Node("literal", "("), inner, Node("literal", ")")])
        if tok.endswith("(") and len(tok) > 1:
            self.depth += 1
            args = self.one_of()
            self.depth -= 1
            self.expect(")")
            return Node("function", tok[:-1].lower(), [args])
        if tok.startswith("<'"):
            return Node("property", tok[2:-2])
        if tok.startswith("<@"):
            return Node("atrule", tok[1:-1].lower())
        if tok.startswith("<"):
            inner = tok[1:-1]
            range, parameter = None, None
            if "[" in inner:
                inner, bracketed = inner.split("[", 1)
                bracketed = bracketed.rstrip("]").strip()
                if "<" in bracketed:
                    # A parameterized type: <boolean-expr[ <if-test> ]> (css-values-5/#boolean).
                    parameter = GrammarParser(bracketed).parse()
                else:
                    lo, hi = bracketed.split(",")
                    range = (parse_bound(lo), parse_bound(hi))
            node = Node("type", "<%s>" % inner.strip())
            node.range = range
            if parameter:
                node.children.append(parameter)
            return node
        if tok.startswith("@"):
            return Node("atkeyword", tok[1:].lower())
        if tok.startswith("'"):
            literal = tok[1:-1]
            if len(literal) == 1:
                return Node("literal", literal)
            return Node("seq", children=[Node("literal", c) for c in literal])
        if re.fullmatch(r"\d+(?:\.\d+)?[a-zA-Z]*", tok):
            # A numeric literal: a value that the grammar allows by itself, as in `0deg | 90deg | 0 | 90`
            # (css-writing-modes-4/#glyph-orientation). It is a number or a dimension, not a keyword. Units are ASCII
            # case-insensitive (css-values-4/#dimensions).
            return Node("number", tok.lower())
        if re.fullmatch(r"[-\w]+", tok):
            # Keywords are ASCII case-insensitive (css-values-4/#keywords), so crispEdges and
            # crisp-edges are two keywords but crispEdges and crispedges are one.
            return Node("keyword", tok.lower())
        return Node("literal", tok)


# A few specs write the range outside the angle brackets: "<length> [0,∞]".
STRAY_RANGE = re.compile(r"(<[-\w]+)>\s*\[\s*([-+\d.∞][^\]]*)\]")


def parse_grammar(text):
    return GrammarParser(STRAY_RANGE.sub(r"\1 [\2]>", text)).parse()


SEPARATOR = {"oneof": ["|"], "anyof": ["||"], "allof": ["&&"], "seq": []}


def serialize(node, top=False):
    """Returns the grammar tokens that the node prints as. Groups are always bracketed, so a round trip through
    parse_grammar preserves structure even though the text is not verbatim."""
    multiplied = (node.min, node.max) != (1, 1) or node.comma or node.required
    if node.type in SEPARATOR:
        out = []
        for i, c in enumerate(node.children):
            if i:
                out += SEPARATOR[node.type]
            out += serialize(c)
        if not top or multiplied:
            out = ["["] + out + ["]"]
    elif node.type == "function":
        out = [node.value + "("] + serialize(node.children[0], top=True) + [")"]
    elif node.type == "longhand":
        out = serialize(node.children[0])
    elif node.type == "property":
        out = ["<'%s'>" % node.value]
    elif node.type == "atkeyword":
        out = ["@" + node.value]
    elif node.type == "atrule":
        out = ["<%s>" % node.value]
    elif node.type == "literal":
        out = ["'%s'" % node.value] if node.value not in ",/" else [node.value]
    elif node.type == "type" and node.range:
        lo, hi = ("∞" if b == INF else "-∞" if b == -INF else "%g" % b for b in node.range)
        out = ["<%s [%s,%s]>" % (node.value[1:-1], lo, hi)]
    elif node.type == "type" and node.children:
        out = ["<%s[ %s ]>" % (node.value[1:-1], " ".join(serialize(node.children[0], top=True)))]
    else:
        out = [node.value]

    if node.comma:
        out.append("#")
        if (node.min, node.max) == (0, INF):
            out.append("?")
        elif (node.min, node.max) != (1, INF):
            out.append("{%d,%s}" % (node.min, "" if node.max == INF else node.max))
    elif (node.min, node.max) == (0, 1):
        out.append("?")
    elif (node.min, node.max) == (0, INF):
        out.append("*")
    elif (node.min, node.max) == (1, INF):
        out.append("+")
    elif (node.min, node.max) != (1, 1):
        out.append("{%d,%s}" % (node.min, "" if node.max == INF else node.max))
    if node.required:
        out.append("!")
    return out


def same_tree(a, b):
    return (a.type == b.type and a.value == b.value and a.min == b.min and a.max == b.max
            and a.comma == b.comma and a.required == b.required and a.range == b.range
            and len(a.children) == len(b.children)
            and all(same_tree(x, y) for x, y in zip(a.children, b.children)))


def walk(node):
    yield node
    for c in node.children:
        yield from walk(c)


def once(node):
    return ((node.min, node.max) == (1, 1) and not node.comma and not node.required
            and node.range is None)


def factor(node):
    """Returns the same grammar, with the leaf that every alternative of a | starts with written once before the |.
    `':' <ident-token> | ':' <function-token> ...` becomes `':' [ <ident-token> | <function-token> ... ]`, so the next
    token after the leaf picks the alternative. The alternatives keep their order, and a sequence or | adds no value, so
    the match is the same."""
    node.children = [factor(child) for child in node.children]
    if node.type != "oneof" or len(node.children) < 2:
        return node
    alternatives = node.children
    if not all(a.type == "seq" and once(a) and len(a.children) >= 2 for a in alternatives):
        return node
    first = alternatives[0].children[0]
    if first.type not in ("literal", "keyword") or not once(first):
        return node
    if not all(same_tree(a.children[0], first) for a in alternatives):
        return node

    rests = []
    for a in alternatives:
        rest = a.children[1:]
        rests.append(rest[0] if len(rest) == 1 else Node("seq", children=rest))
    choice = factor(Node("oneof", children=rests))
    out = Node("seq", children=[first, choice])
    out.min, out.max, out.comma = node.min, node.max, node.comma
    out.required, out.range = node.required, node.range
    return out

def zero_as_number(node):
    """Returns the same grammar, with a <number> alternative of a | tried before a <length> alternative.
    css-values-4/#lengths parses a 0 that either could accept as the <number>. The two accept nothing in common except a
    0, so the match is otherwise the same."""
    node.children = [zero_as_number(child) for child in node.children]
    if node.type != "oneof":
        return node
    kind = lambda child: (child.type == "type" and not child.children
                          and child.value in ("<number>", "<length>") and child.value)
    numbers = [child for child in node.children if kind(child) == "<number>"]
    rest = [child for child in node.children if kind(child) != "<number>"]
    lengths = [i for i, child in enumerate(rest) if kind(child) == "<length>"]
    if lengths and numbers:
        node.children = rest[:lengths[0]] + numbers + rest[lengths[0]:]
    return node


# ----------------------------------------------------------------------------------------------------------------------
# Pseudo-classes and pseudo-elements (selectors-4/#pseudo-classes, selectors-4/#pseudo-elements)

class Pseudo:
    """One pseudo-class or pseudo-element, named without its colons or parentheses."""

    def __init__(self, kind, name, function, argument, entry):
        self.kind = kind            # "pseudo-class" | "pseudo-element"
        self.name = name
        self.function = function
        self.argument = argument    # the grammar of a functional pseudo's argument, or None for prose
        self.entry = entry

    def key(self):
        return (self.name, self.function)

    def ident(self):
        prefix = "CssPseudoClass_" if self.kind == "pseudo-class" else "CssPseudoElement_"
        return prefix + camel(self.name) + ("Fn" if self.function else "")


def grammar_keywords(values, name):
    return {n.value for n in walk(parse_grammar(values[name].grammar())) if n.type == "keyword"}


def argument_grammar(entry, name):
    """Returns the grammar of a functional pseudo's argument, from the syntax that webref quotes, or None if the spec
    writes it in prose. Some specs quote an informal notation there instead, such as :nth-child(An+B [of S]?), which
    names no production. That also counts as prose."""
    texts = [d["value"] for _, _, d in sorted(entry.decls, key=lambda t: t[0], reverse=True)
             if d.get("value")]
    if not texts:
        return None
    try:
        tree = parse_grammar(texts[0].lstrip(":"))
    except SyntaxError:
        return None
    if tree.type != "function" or tree.value != name:
        sys.exit("%s: webref's syntax is not a function named %s: %s"
                 % (entry.name, name, texts[0]))
    argument = tree.children[0]
    if not any(n.type == "type" for n in walk(argument)):
        return None
    return " ".join(serialize(argument, top=True))


def pseudos_of(selectors, values):
    """Returns the pseudo-classes and pseudo-elements that an element selector can name. CSS 2 writes the four
    pseudo-elements that <legacy-pseudo-element-selector> lists with one colon, and they are pseudo-elements. The page
    pseudo-classes that css-page's <pseudo-page> lists select pages, unless a later spec also defines one for
    elements."""
    legacy = grammar_keywords(values, "<legacy-pseudo-element-selector>")
    page = grammar_keywords(values, "<pseudo-page>")
    found = []
    for name, entry in sorted(selectors.items()):
        if not name.startswith(":") or name in OMITTED_SELECTORS or name in PAGE_PSEUDO_CLASSES:
            continue
        kind = "pseudo-element" if name.startswith("::") else "pseudo-class"
        bare = name.lstrip(":")
        function = bare.endswith("()")
        if function:
            bare = bare[:-2]
        specs = {spec for _, spec, _ in entry.decls}
        if kind == "pseudo-class" and (bare in legacy or (bare in page and specs <= LEGACY_SPECS)):
            continue
        argument = argument_grammar(entry, bare) if function else None
        found.append(Pseudo(kind, bare, function, argument, entry))

    elements = {p.name for p in found if p.kind == "pseudo-element" and not p.function}
    if legacy - elements:
        sys.exit("no pseudo-element for the legacy names %s" % sorted(legacy - elements))
    return sorted(found, key=lambda p: (p.kind, p.name, p.function)) + [
        Pseudo("pseudo-class", name, False, None, None) for name in PRIVATE_PSEUDO_CLASSES]


# ----------------------------------------------------------------------------------------------------------------------
# At-rules (css-syntax-3/#at-rule)

def is_literal(node, character):
    return node.type == "literal" and node.value == character and once(node)


def sequence_of(parts):
    """Returns a run of grammar nodes as one node: the node itself if there is one, a sequence if there are more, and
    None if there are none."""
    if not parts:
        return None
    if len(parts) == 1:
        return parts[0]
    return Node("seq", children=parts)


def block_contents(parts, label):
    """Returns the production that `{ X }` names, given `parts`, the three nodes of `{ X }`."""
    if (len(parts) != 3 or not is_literal(parts[0], "{") or not is_literal(parts[2], "}")
            or parts[1].type != "type" or parts[1].value not in BLOCK_CONTENTS
            or not once(parts[1])):
        sys.exit("%s: a block that is not { <block contents> }" % label)
    return parts[1].value


class AtRule:
    """One at-rule: its grammar, split into the prelude of each form that it accepts, and the production that its block
    holds."""

    def __init__(self, name, entry):
        self.name = name
        self.entry = entry
        self.block_prelude = None       # what comes before a block, or None if nothing does
        self.statement_prelude = None   # what comes before a ';', or None if nothing does
        self.statement = False          # the rule may end at a ';' rather than a block
        self.contents = None            # the block's production (<rule-list>, <declaration-list>, ...), or None if
                                        # the rule accepts no block
        text = entry.grammar()
        if text is None:
            return
        try:
            tree = parse_grammar(text)
        except SyntaxError as err:
            sys.exit("%s: %s\n  %s" % (name, err, text))
        if not same_tree(tree, parse_grammar(" ".join(serialize(tree, top=True)))):
            sys.exit("%s: does not survive a serialize/parse round trip\n  %s" % (name, text))

        blocks, statements, contents = [], [], set()
        alternatives = tree.children if tree.type == "oneof" and once(tree) else [tree]
        for alternative in alternatives:
            parts = (alternative.children if alternative.type == "seq" and once(alternative)
                     else [alternative])
            if parts[0].type != "atkeyword" or "@" + parts[0].value != name or not once(parts[0]):
                sys.exit("%s: a grammar that does not open with its at-keyword: %s" % (name, text))
            parts = parts[1:]
            end = next((i for i, part in enumerate(parts)
                        if is_literal(part, "{") or is_literal(part, ";")
                        or (part.type == "seq" and (part.min, part.max) == (0, 1) and part.children
                            and is_literal(part.children[0], "{"))), None)
            if end is None:
                sys.exit("%s: a grammar with neither a block nor a ';': %s" % (name, text))
            prelude, rest = parts[:end], parts[end:]
            if is_literal(rest[0], ";"):
                if len(rest) != 1:
                    sys.exit("%s: a grammar that goes on past its ';': %s" % (name, text))
                statements.append(sequence_of(prelude))
                continue
            # css-page-3/#syntax-page-selector writes a ';' after each margin rule's block.
            # css-syntax-3/#consume-at-rule never reads it as part of the rule.
            optional = not is_literal(rest[0], "{")
            block = rest[0].children if optional else rest[:3]
            after = rest[1:] if optional else rest[3:]
            if after and not (len(after) == 1 and is_literal(after[0], ";")):
                sys.exit("%s: a grammar that goes on past its block: %s" % (name, text))
            contents.add(block_contents(block, name))
            blocks.append(sequence_of(prelude))
            if optional:
                statements.append(sequence_of(prelude))

        if len(contents) > 1:
            sys.exit("%s: blocks that hold different productions: %s" % (name, sorted(contents)))
        if blocks:
            self.block_prelude = self.choice(blocks, text)
            self.contents = contents.pop()
        if statements:
            self.statement_prelude = self.choice(statements, text)
            self.statement = True

    def choice(self, preludes, text):
        """Returns the prelude that a form allows, or a choice if it allows several. None of several preludes may be
        empty."""
        if len(preludes) == 1:
            return preludes[0]
        if None in preludes:
            sys.exit("%s: an empty prelude among others: %s" % (self.name, text))
        return Node("oneof", children=preludes)


class QualifiedRule:
    """The qualified rules that an at-rule's block holds, if they are not style rules: the grammar of their prelude, and
    the production that their block holds."""

    def __init__(self, production, text):
        tree = parse_grammar(text)
        parts = tree.children if tree.type == "seq" and once(tree) else [tree]
        start = next((i for i, part in enumerate(parts) if is_literal(part, "{")), None)
        if start is None:
            sys.exit("%s: a qualified rule with no block: %s" % (production, text))
        self.prelude = sequence_of(parts[:start])
        self.contents = block_contents(parts[start:], production)


class AtRules:
    """Every at-rule, in the order of the CssAtRule enum, and where each one is valid."""

    def __init__(self, entries, within, keywords):
        self.keywords = keywords
        self.rules = [AtRule(name, entries[name]) for name in sorted(entries)]
        self.within = within
        names = {rule.name for rule in self.rules}
        for table in (AT_RULE_PROPERTIES, AT_RULE_WITHIN, CSSOM_RULE_TYPES, AT_RULE_QUALIFIED_RULES,
                      AT_RULE_ALIASES, AT_RULE_NAMED_VALUES,
                      dict.fromkeys(AT_RULE_NO_IMPORTANT, "")):
            for name in list(table) + [v for v in table.values() if v.startswith("@")]:
                if name not in names:
                    sys.exit("%s names %s, which webref does not list"
                             % ("a table in gen_css.py", name))
        for name in NOT_NESTED_AT_RULES | NESTED_STATEMENTS:
            if name not in names:
                sys.exit("a table in gen_css.py names %s, which webref does not list" % name)
        for rule in self.rules:
            if rule.entry.grammar() is None and rule.name not in AT_RULE_ALIASES:
                sys.exit("%s: webref gives no grammar, and AT_RULE_GRAMMARS none either"
                         % rule.name)

    def nests(self, rule):
        """Returns true if the rule is valid inside a style rule. css-nesting-1/#nested-group-rules allows any at-rule
        whose block holds style rules."""
        if rule.name in NESTED_STATEMENTS:
            return True
        return (rule.contents in ("<rule-list>", "<block-contents>")
                and rule.name not in NOT_NESTED_AT_RULES and rule.name not in self.within)


# ----------------------------------------------------------------------------------------------------------------------
# Emitting

BANNER = gen_common.banner(__file__, gen_common.WEBREF_SOURCE, gen_common.webref_url("css"))

TABLES_BANNER = gen_common.banner(__file__, "the hand-written tables in gen_css.py")


def view(s):
    """Returns a PrStringView over the literal. A name with no text is the null view."""
    return "{%s, %d}" % (c_string(s), len(s.encode("utf-8"))) if s else "{nullptr, 0}"


def public_header(path, title, spec, body):
    """Returns a public header for one generated enumeration."""
    head = [BANNER, "\n// %s\n//\n// %s\n" % (title, spec), "\n#pragma once\n",
            '\n#include "pr/base.h"\n']
    write(path, head + ["\nPR_ASSUME_NONNULL_BEGIN\n"] + body + ["\nPR_ASSUME_NONNULL_END\n"])


def emit_public_properties(path, props):
    members = [(property_ident(name), "alias of %s" % e.alias_of() if e.alias_of() else "")
               for name, e in props.items()]
    first_alias = next((index for index, e in enumerate(props.values()) if e.alias_of()), None)
    body = ["\n// Every CSS property, by the name that its defining spec gives it. The comment beside an alias names "
            "the\n// property it stands for.\nenum {\n"]
    body += enum_members(members, first_alias, "PrCssProperty_AliasFirst",
                         ["Legacy aliases, which occupy [PrCssProperty_AliasFirst, PrCssProperty_Count)."])
    body.append("\n    PrCssProperty_Count,\n};\n")
    body.append("typedef uint16_t PrCssProperty;\n")
    public_header(path, "Properties", "css-cascade-5 - https://drafts.csswg.org/css-cascade-5/",
                  body)


def emit_public_descriptors(path, descs):
    body = ["\n// Every descriptor of every at-rule. The comment beside each is its at-rule and its name. "
            "A name\n// that two at-rules declare is a descriptor of each, since their grammars may differ. The "
            "descriptors of\n// @media and @container are the features that their queries test.\nenum {\n"]
    body += enum_members([(descriptor_ident(key), "%s %s" % key) for key in descs])
    body.append("\n    PrCssDescriptor_Count,\n"
                "    PrCssDescriptor_None = PrCssDescriptor_Count,   // no descriptor\n};\n")
    body.append("typedef uint16_t PrCssDescriptor;\n")
    public_header(path, "Descriptors", "css-syntax-3 - https://drafts.csswg.org/css-syntax/", body)


def emit_public_keywords(path, g):
    body = ["\n// Every identifier and function name that a grammar holds. The comment beside each keyword is its "
            "name in\n// lowercase. Keywords match ASCII case-insensitively (css-values-4/#keywords).\nenum {\n"]
    body += enum_members([(keyword_ident(name), name)
                          for name in sorted(g.keywords, key=g.keywords.get)])
    body.append("\n    PrCssKeyword_Count,\n")
    body.append("    PrCssKeyword_Unknown = PrCssKeyword_Count,   // text that no keyword matches\n};\n")
    body.append("typedef uint16_t PrCssKeyword;\n")
    public_header(path, "Keywords", "css-values-4 - https://drafts.csswg.org/css-values-4/", body)


# The C struct, table and count of each shape of family of logical property groups.
FAMILY_TABLES = (("side", "CssSideFamily", "kCssSideFamilies", "kCssSideFamilyCount"),
                 ("axis", "CssAxisFamily", "kCssAxisFamilies", "kCssAxisFamilyCount"),
                 ("corner", "CssCornerFamily", "kCssCornerFamilies", "kCssCornerFamilyCount"))


def logical_mappings(props):
    """Returns the logical property group of each property that has one (css-logical-1/#logical-property-group), by
    name, as (group, logical), where `logical` says whether the property is flow-relative. Aliases are left out."""
    mappings = {}
    for name, entry in props.items():
        if entry.alias_of():
            continue
        groups = {d["logicalPropertyGroup"] for _, _, d in entry.decls
                  if d.get("logicalPropertyGroup")}
        if len(groups) > 1:
            sys.exit("%s: conflicting logical property groups: %s" % (name, sorted(groups)))
        if groups:
            # css-logical-1: axes, sides and corners name their flow-relative mapping.
            logical = bool(set(name.split("-")) & {"block", "inline", "start", "end"})
            mappings[name] = (groups.pop(), logical)
    return mappings


def logical_place(name):
    """Returns where a member of a logical property group sits in its family, from the words of its name, by which
    css-logical-1 maps it: (shape, logical, index), where a corner's index is its block or vertical side times 2 plus
    its inline or horizontal side. Returns None for a name that names no side, axis or corner."""
    words = name.split("-")
    for a, b in zip(words, words[1:]):
        if a in ("start", "end") and b in ("start", "end"):
            return "corner", True, (a == "end") * 2 + (b == "end")
        if a in ("top", "bottom") and b in ("left", "right"):
            return "corner", False, (a == "bottom") * 2 + (b == "right")
    for a, b in zip(words, words[1:]):
        if a in ("block", "inline") and b in ("start", "end"):
            return "side", True, (0 if a == "block" else 2) + (0 if b == "start" else 1)
    for side, index in (("top", 0), ("right", 1), ("bottom", 2), ("left", 3)):
        if side in words:
            return "side", False, index
    for axis, index in (("inline", 0), ("block", 1)):
        if axis in words:
            return "axis", True, index
    for axis, index in (("width", 0), ("x", 0), ("height", 1), ("y", 1)):
        if axis in words:
            return "axis", False, index
    return None


def logical_families(props):
    """Returns the logical property groups by shape, each a list of (group, flow-relative members, physical members)
    in the order of the members of the struct of the shape (FAMILY_TABLES). Exits if a member's name places it nowhere,
    if two members share a place, or if a group mixes shapes or leaves a place empty."""
    groups = collections.defaultdict(list)
    for name, (group, _) in logical_mappings(props).items():
        groups[group].append(name)
    sizes = {"side": 4, "axis": 2, "corner": 4}
    families = {shape: [] for shape in sizes}
    for group in sorted(groups):
        shapes = set()
        slots = {True: {}, False: {}}
        for name in groups[group]:
            place = logical_place(name)
            if place is None:
                sys.exit("%s: its name places it nowhere in the logical property group %s" % (name, group))
            shape, logical, index = place
            shapes.add(shape)
            if index in slots[logical]:
                sys.exit("%s and %s share a place in %s" % (name, slots[logical][index], group))
            slots[logical][index] = name
        if len(shapes) != 1:
            sys.exit("the logical property group %s mixes %s" % (group, " and ".join(sorted(shapes))))
        shape = shapes.pop()
        if any(len(slots[logical]) != sizes[shape] for logical in (True, False)):
            sys.exit("the logical property group %s leaves a place of its %s family empty" % (group, shape))
        families[shape].append((group, [slots[True][i] for i in range(sizes[shape])],
                                [slots[False][i] for i in range(sizes[shape])]))
    return families


def style_groups(props, shorthands):
    """Checks STYLE_GROUPS and NOT_COMPUTED against the longhands that webref lists, and returns the groups. Exits if a
    longhand is in two groups or in none and is neither flow-relative nor in NOT_COMPUTED, if NOT_COMPUTED names what a
    group holds or what is not a longhand, or if a group holds a property whose inheritance is not the group's."""
    longhands = {name for name, entry in props.items()
                 if not entry.alias_of() and name not in shorthands}
    flow_relative = {name for name, (_, logical) in logical_mappings(props).items() if logical}
    placed = {}
    for group in STYLE_GROUPS:
        if len(group.properties) > 255:
            sys.exit("the style group %s has more slots than uint8_t indexes" % group.name)
        for name in group.properties:
            if name not in longhands:
                sys.exit("the style group %s holds %s, which is not a longhand" % (group.name, name))
            if name in flow_relative:
                sys.exit("the style group %s holds %s, which is flow-relative" % (group.name, name))
            if name in placed:
                sys.exit("%s is in the style groups %s and %s" % (name, placed[name], group.name))
            placed[name] = group.name
            best = props[name]._best()
            inherits = (best[2] if best else {}).get("inherited", "no").startswith("yes")
            if inherits != group.inherited:
                sys.exit("%s %s, but the style group %s %s" % (name, "inherits" if inherits else "does not inherit",
                                                                group.name,
                                                                "does" if group.inherited else "does not"))
    for name in sorted(NOT_COMPUTED):
        if name not in longhands or name in placed or name in flow_relative:
            sys.exit("NOT_COMPUTED names %s, which is %s" % (name, placed.get(name) and "in the style group "
                                                             + placed[name] or "flow-relative" if name in flow_relative
                                                             else "not a longhand"))
    for name in sorted(longhands - set(placed) - flow_relative - NOT_COMPUTED):
        sys.exit("%s is a longhand in no style group: add it to one, or to NOT_COMPUTED" % name)
    return STYLE_GROUPS


def emit_style_group_header(path, groups):
    head = [BANNER, "\n// Style groups\n", "\n#pragma once\n", '\n#include "pr/css_properties.h"\n']
    members = [("CssStyleGroup_%s" % g.name, ("inherited" if g.inherited else "not inherited")
                + (", " + g.reader if g.reader else "")) for g in groups]
    body = ["\n// The kinds of group, which partition the properties that the engine computes (STYLE_GROUPS in gen_css.py).\n"
            "enum {\n"]
    body += enum_members(members)
    body.append("    CssStyleGroup_Count,\n};\ntypedef uint8_t CssStyleGroup;\n")
    body.append("\n// The location of a property's computed value: its group, and its position among the group's values. "
                "The group is\n// CssStyleGroup_Count for a property that the engine does not compute.\n"
                "struct CssStyleSlot {\n    CssStyleGroup group;\n    uint8_t index;\n};\n"
                "extern const CssStyleSlot kCssStyleSlots[PrCssProperty_Count];\n")
    body.append("\n// The properties of each group, in the order of their slots.\n"
                "struct CssStyleGroupProperties {\n    const PrCssProperty* properties;\n    uint32_t count;\n};\n"
                "extern const CssStyleGroupProperties kCssStyleGroupProperties[CssStyleGroup_Count];\n")
    mask = sum(1 << i for i, g in enumerate(groups) if g.inherited)
    body.append("\n// The groups whose properties inherit, a bit for each group.\n"
                "constexpr uint32_t kCssStyleGroupsInherited = 0x%xu;\n" % mask)
    write(path, namespaced(head, body))


def emit_style_group_source(path, props, groups):
    head = [BANNER, '\n#include "css/src/css_style_group.h"\n']
    body = ["namespace {\n"]
    slots = {}
    for index, group in enumerate(groups):
        body.append("\n" + "".join("// %s\n" % line for line in textwrap.wrap(group.note, 117, break_on_hyphens=False)))
        idents = [property_ident(name) for name in group.properties]
        body.append("constexpr PrCssProperty k%sProperties[] = {\n" % group.name)
        line = "   "
        for ident in idents:
            if len(line) + len(ident) + 2 > 120:
                body.append(line + "\n")
                line = "   "
            line += " %s," % ident
        body.append(line + "\n};\n")
        for slot, name in enumerate(group.properties):
            slots[name] = (index, slot)
    body.append("\n} // namespace\n")
    body.append(table("extern const CssStyleGroupProperties kCssStyleGroupProperties[CssStyleGroup_Count]",
                      ["{k%sProperties, %d}" % (g.name, len(g.properties)) for g in groups],
                      comments=[g.name for g in groups]))
    rows = []
    for name, entry in props.items():
        group, slot = slots.get(name, (None, 0))
        rows.append("{%s, %d}" % ("CssStyleGroup_Count" if group is None else "CssStyleGroup_" + groups[group].name,
                                  slot))
    body.append(table("extern const CssStyleSlot kCssStyleSlots[PrCssProperty_Count]", rows, comments=list(props)))
    write(path, namespaced(head, body))


def emit_property_header(path, props):
    head = [BANNER, "\n#pragma once\n",
            '\n#include "pr/css_properties.h"\n\n#include "pr/bitset.h"\n#include '
            '"css/src/css_grammar.h"\n#include "css/src/css_keyword.h"\n']
    body = []
    body.append("\n// The property's name, which is lowercase.\n"
                "PrStringView cssPropertyName(PrCssProperty property);\n")
    body.append("\n// The property that an alias resolves to. Every other property maps to itself.\n"
                "extern const PrCssProperty kCssPropertyAlias[PrCssProperty_Count];\n")
    body.append("\n// The root of each property's grammar, an index into kCssGrammarNodes.\n"
                "extern const CssGrammarIndex kCssPropertyGrammarRoot[PrCssProperty_Count];\n")
    body.append("\n// Whether the property animates (web-animations-1/#animation-type). A "
                "shorthand animates if each of\n// its longhands does, and an alias if the property "
                "it aliases does.\nextern const uint64_t "
                "kCssPropertyAnimatable[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// Whether the property animates discretely (web-animations-1/#discrete): a longhand whose "
                "animation type is\n// discrete.\nextern const uint64_t "
                "kCssPropertyDiscrete[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// Whether the property animates as a repeatable list (web-animations-1/#repeatable-list): two lists "
                "of different\n// lengths repeat to their least common multiple before they combine.\nextern const "
                "uint64_t kCssPropertyRepeatableList[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// Whether every SVG element has a presentation attribute of the property, spelled as the "
                "property is\n// (SVG2/styling.html#PresentationAttributes).\nextern const uint64_t "
                "kCssPropertyPresentation[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// Whether the property inherits (css-cascade-5/#inherited-property). A shorthand does not, and "
                "an alias\n// inherits if the property it aliases does.\nextern const uint64_t "
                "kCssPropertyInherited[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// The logical property group that each property belongs to "
                "(css-logical-1/#logical-property-group), or zero for\n// none. An alias belongs to the group of the "
                "property it aliases.\nextern const uint8_t kCssPropertyLogicalGroup[PrCssProperty_Count];\n")
    body.append("\n// Whether the property is a flow-relative member of a logical property group, rather than a "
                "physical one.\nextern const uint64_t kCssPropertyFlowRelative[bitsetWords(PrCssProperty_Count)];\n")
    comments = {
        "side": "// The logical property groups whose members are a box's sides "
                "(css-logical-1/#logical-property-group): the\n// flow-relative longhands by side, block-start, "
                "block-end, inline-start then inline-end, and the physical\n// longhands by side, top, right, bottom "
                "then left.\n",
        "axis": "// The logical property groups whose members are a box's axes: the flow-relative longhands by axis, "
                "inline then\n// block, and the physical longhands by axis, horizontal then vertical.\n",
        "corner": "// The logical property groups whose members are a box's corners: the flow-relative longhands by "
                  "their block-axis\n// side then their inline-axis side, start first, and the physical longhands by "
                  "their vertical side then their\n// horizontal side, top and left first.\n",
    }
    members = {"side": "[4]", "axis": "[2]", "corner": "[2][2]"}
    families = logical_families(props)
    for shape, struct, array, count in FAMILY_TABLES:
        body.append("\n%sstruct %s {\n    PrCssProperty logical%s;\n    PrCssProperty physical%s;\n};\n"
                    "constexpr uint32_t %s = %d;\nextern const %s %s[%s];\n"
                    % (comments[shape], struct, members[shape], members[shape], count, len(families[shape]), struct,
                       array, count))
    body.append("\n// The anchor functions a property accepts in place of a <length> "
                "(css-anchor-position-1/#anchor-pos,\n// css-anchor-position-1/#anchor-size-fn).\n"
                "enum {\n    CssAnchorFunction_Anchor = 1 << 0,       // anchor()\n"
                "    CssAnchorFunction_AnchorSize = 1 << 1,   // anchor-size()\n};\n"
                "typedef uint8_t CssAnchorFunctions;\n\n// The anchor functions each property accepts.\n"
                "extern const CssAnchorFunctions "
                "kCssPropertyAnchorFunctions[PrCssProperty_Count];\n")
    body.append("\n// The keywords a property's prose excludes from the <custom-ident>s of its "
                "value, beyond the\n// CSS-wide keywords (css-values-4/#custom-idents). They are the run of "
                "kCssExcludedIdents from its\n// entry up to PrCssKeyword_Unknown.\n"
                "extern const PrCssKeyword kCssExcludedIdents[];\nextern const uint8_t "
                "kCssPropertyExcludedIdents[PrCssProperty_Count];\n")
    body.append("\n// Keywords that a value may be written as, and the fewer keywords that it is held as. The "
                "prose says that\n// the two are the same value (cssom-1/#serialize-a-css-value).\n"
                "struct CssKeywordForm {\n    // The keywords written, 16 bits each, in the low 48 bits from the top down. "
                "PrCssKeyword_Unknown\n    // follows the last.\n    uint64_t written;\n\n"
                "    PrCssKeyword held[3];   // the keywords held, with PrCssKeyword_Unknown after the last\n};\n\n"
                "// A run of kCssKeywordForms.\nstruct CssKeywordFormRun {\n    uint16_t start;\n"
                "    uint16_t count;\n};\n\n// Each property's forms, sorted by what is written.\n"
                "extern const CssKeywordForm kCssKeywordForms[];\nextern const CssKeywordFormRun "
                "kCssPropertyKeywordForms[PrCssProperty_Count];\n\n"
                "// Whether the form sorts before the keywords `written`.\n"
                "inline bool operator<(const CssKeywordForm& form, uint64_t written) {\n"
                "    return form.written < written;\n}\n")
    body.append("\n// The property `text` names, or PrCssProperty_Count. Property names match "
                "ASCII\n// case-insensitively, as keywords do (css-values-4/#keywords).\n"
                "PrCssProperty cssPropertyFromText(PrStringView text);\n")

    body.append("\n// " + "-" * 117 + "\n"
                "// Shorthands (css-cascade-5/#shorthand)\n"
                "\n// How a shorthand's value gives its longhands their values.\nenum {\n")
    forms = [("CssShorthand_None", "a longhand")] + [("CssShorthand_" + form, comment) for form,
                                                     comment in SHORTHAND_FORMS]
    width = max(len(ident) for ident, _ in forms) + 1
    body += enum_members(forms)
    body.append("};\ntypedef uint8_t CssShorthand;\n")
    body.append("\n// A run of kCssPropertyRuns.\n"
                "struct CssPropertySpan {\n    uint16_t start;\n    uint16_t count;\n};\n")
    body.append("\n// The form of each property, or CssShorthand_None for a longhand.\n"
                "extern const CssShorthand kCssPropertyShorthand[PrCssProperty_Count];\n")
    body.append("\n// The longhands a shorthand sets, in canonical order, or the empty span for a "
                "longhand.\nextern const CssPropertySpan "
                "kCssPropertyLonghands[PrCssProperty_Count];\n")
    body.append("\n// The shorthands that set a longhand, in preferred order\n"
                "// (cssom-1/#concept-shorthands-preferred-order), or the empty span for a "
                "shorthand.\nextern const CssPropertySpan "
                "kCssPropertyShorthands[PrCssProperty_Count];\n")
    body.append("\n// The shorthands that a declaration block never uses to write its longhands.\n"
                "extern const uint64_t kCssPropertyUnwritten[bitsetWords(PrCssProperty_Count)];\n")
    body.append("\n// The properties the spans name, end to end.\n"
                "extern const PrCssProperty kCssPropertyRuns[];\n")
    body.append("\n// The initial value of a longhand, as it serializes (css-cascade-5/#initial-values). The null "
                "view for a\n// shorthand, an alias, and a longhand whose spec gives its initial value in prose.\n"
                "PrStringView cssPropertyInitial(PrCssProperty property);\n")
    write(path, namespaced(head, body))


def anchor_functions(name):
    """Returns the CssAnchorFunction flags of a property."""
    flags = [flag for flag, properties in (("CssAnchorFunction_Anchor", INSET_PROPERTIES),
                                           ("CssAnchorFunction_AnchorSize", ANCHOR_SIZE_PROPERTIES))
             if name in properties]
    return " | ".join(flags) if flags else "0"


def custom_ident_exclusions(props, g):
    """Returns the keywords that each property excludes from its <custom-ident>s, according to
    CUSTOM_IDENT_EXCLUSIONS."""
    trees = [("value", name, None, tree) for (kind, name), tree in g.trees.items()
             if kind == "type"]
    grammars = {name: tree for kind, name, _, tree in trees}
    for key in CUSTOM_IDENT_EXCLUSIONS:
        tree = grammars.get(key) or g.trees.get(("property", key))
        if tree is None or not (reaches_type(trees, tree, "<custom-ident>")
                                or "<custom-ident>" in type_refs(tree)):
            sys.exit("CUSTOM_IDENT_EXCLUSIONS: %s holds no <custom-ident>" % key)
    out = {}
    for name, e in props.items():
        tree = g.trees.get(("property", e.alias_of() or name))
        if (e.alias_of() or name) in CUSTOM_IDENT_EXCLUSIONS:
            out[name] = CUSTOM_IDENT_EXCLUSIONS[e.alias_of() or name]
            continue
        excluding = [p for p in CUSTOM_IDENT_EXCLUSIONS if tree is not None and p in grammars
                     and reaches_type(trees, tree, p)]
        if not excluding:
            continue
        # Every <custom-ident> of the value has to be one that the production holds.
        outside = Node("seq", children=[copy.deepcopy(tree)])
        stack = [outside]
        while stack:
            node = stack.pop()
            node.children = [child for child in node.children
                             if not (child.type == "type"
                                     and normalize_ref(child.value) in excluding)]
            stack += node.children
        if reaches_type(trees, outside, "<custom-ident>") or "<custom-ident>" in type_refs(outside):
            sys.exit("CUSTOM_IDENT_EXCLUSIONS: %s reaches a <custom-ident> outside %s"
                     % (name, ", ".join(excluding)))
        out[name] = [keyword for p in excluding for keyword in CUSTOM_IDENT_EXCLUSIONS[p]]
    return out


def emit_property_source(path, props, g, shorthands, values):
    head = [BANNER, '\n#include "css/src/css_property.h"\n\n#include "pr/name_table.h"\n']

    # The longhands every shorthand sets, then the shorthands that set every longhand.
    runs = []
    longhands = {}
    for name, shorthand in shorthands.items():
        longhands[name] = (len(runs), len(shorthand.longhands))
        runs += shorthand.longhands
    setters = collections.defaultdict(list)
    for name, shorthand in shorthands.items():
        for longhand in shorthand.longhands:
            setters[longhand].append(name)
    of = {}
    for longhand in props:
        if longhand in setters:
            of[longhand] = (len(runs), len(setters[longhand]))
            runs += preferred_order(setters[longhand], shorthands)
    if len(runs) > 0xffff:
        sys.exit("%d properties in kCssPropertyRuns: more than a CssPropertySpan reaches"
                 % len(runs))

    named = dict(values)
    named.update(("<'%s'>" % name, entry) for name, entry in props.items())

    # The initial values of the longhands. A shorthand's expansion gives them to the longhands that it leaves out, so
    # those initial values must be values. The initial value of any other longhand may be prose, which the cascade
    # cannot use.
    initial = {}
    for shorthand in shorthands.values():
        if shorthand.form != "All":
            for longhand in shorthand.longhands:
                initial[longhand] = initial_value(longhand, props[longhand], named)
    for name, entry in props.items():
        # A grammar that names the values of other properties belongs to a shorthand that SHORTHANDS does not list,
        # unless it is the values of one property and nothing else, as fill-opacity's is opacity's, or a list of them,
        # as background-blend-mode's is of mix-blend-mode's.
        grammar = (entry.grammar() or "").strip()
        borrowed = "<'" in grammar and not re.fullmatch(r"<'[a-z-]+'>#?", grammar)
        if name in initial or name in shorthands or entry.alias_of() or says_shorthand(entry) or borrowed:
            continue
        initial[name] = initial_value(name, entry, named, prose="")
    stale = sorted(set(INITIAL_VALUES) - set(initial))
    if stale:
        sys.exit("INITIAL_VALUES: %s is no longhand" % ", ".join(stale))
    initials = name_table.Blob("kCssPropertyInitial", [initial.get(name, "") for name in props])

    names = name_table.NameTable("kCssProperty", list(props), lower=True)
    body = ["namespace {\n", names.tables(), initials.tables(), "\n} // namespace\n"]

    body.append(table("extern const PrCssProperty kCssPropertyAlias[PrCssProperty_Count]",
                      [property_ident(e.alias_of() or name) for name, e in props.items()]))
    body.append(grammar_root_table(g, "property", props, "kCssPropertyGrammarRoot", list(props)))

    def animatable(name):
        name = props[name].alias_of() or name
        if name in shorthands:
            return all(animatable(longhand) for longhand in shorthands[name].longhands)
        if says_shorthand(props[name]):
            # A shorthand that SHORTHANDS does not list animates if the properties that its grammar names animate.
            return all(animatable(longhand)
                       for longhand in re.findall(r"<'([-\w]+)'>", props[name].grammar()))
        return props[name].animatable()
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyAnimatable[bitsetWords(PrCssProperty_Count)]",
                       [animatable(name) for name in props], names=list(props)))

    def discrete(name):
        name = props[name].alias_of() or name
        return name not in shorthands and not says_shorthand(props[name]) and props[name].discrete()
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyDiscrete[bitsetWords(PrCssProperty_Count)]",
                       [discrete(name) for name in props], names=list(props)))
    def repeatable(name):
        name = props[name].alias_of() or name
        return name not in shorthands and not says_shorthand(props[name]) and props[name].repeatable_list()
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyRepeatableList[bitsetWords(PrCssProperty_Count)]",
                       [repeatable(name) for name in props], names=list(props)))
    missing = PRESENTATION - set(props)
    if missing:
        sys.exit("gen_css: presentation attributes of unknown properties: " + ", ".join(sorted(missing)))
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyPresentation[bitsetWords(PrCssProperty_Count)]",
                       [name in PRESENTATION for name in props], names=list(props)))
    def inherited(name):
        entry = props[props[name].alias_of() or name]
        if name in shorthands or says_shorthand(entry):
            return False
        best = entry._best()
        return (best[2] if best else {}).get("inherited", "no").startswith("yes")
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyInherited[bitsetWords(PrCssProperty_Count)]",
                       [inherited(name) for name in props], names=list(props)))
    mappings = logical_mappings(props)
    groups = {name: i + 1 for i,
              name in enumerate(sorted({group for group, _ in mappings.values()}))}
    if len(groups) > 255:
        sys.exit("logical property groups do not fit in uint8_t")
    placed = [mappings.get(entry.alias_of() or name, (None, False)) for name, entry in props.items()]
    body.append(table("extern const uint8_t kCssPropertyLogicalGroup[PrCssProperty_Count]",
                      [groups.get(group, 0) for group, _ in placed], comments=list(props)))
    body.append(bitset("extern const uint64_t kCssPropertyFlowRelative[bitsetWords(PrCssProperty_Count)]",
                       [logical for _, logical in placed], names=list(props)))
    families = logical_families(props)
    for shape, struct, array, count in FAMILY_TABLES:
        rows = []
        for _, logical, physical in families[shape]:
            idents = [property_ident(m) for m in logical], [property_ident(m) for m in physical]
            if shape == "corner":
                rows.append("{%s}" % ", ".join("{{%s, %s}, {%s, %s}}" % tuple(members) for members in idents))
            else:
                rows.append("{%s}" % ", ".join("{%s}" % ", ".join(members) for members in idents))
        body.append(table("extern const %s %s[%s]" % (struct, array, count), rows,
                          comments=[group for group, _, _ in families[shape]]))
    for name in sorted((INSET_PROPERTIES | ANCHOR_SIZE_PROPERTIES) - set(props)):
        sys.exit("an anchor function table names %s, which webref does not list" % name)
    body.append(table("extern const CssAnchorFunctions "
                      "kCssPropertyAnchorFunctions[PrCssProperty_Count]",
                      [anchor_functions(e.alias_of() or name) for name, e in props.items()],
                      comments=list(props)))

    # The keywords each property excludes from its <custom-ident>s, as a run of kCssExcludedIdents
    # ended by PrCssKeyword_Unknown. Every other property points at that terminator.
    excluded = custom_ident_exclusions(props, g)
    runs_of = {}
    idents = ["PrCssKeyword_Unknown"]
    for name in props:
        if name in excluded:
            key = tuple(excluded[name])
            if key not in runs_of:
                runs_of[key] = len(idents)
                idents += [keyword_ident(keyword) for keyword in key] + ["PrCssKeyword_Unknown"]
    if len(idents) > 0xff:
        sys.exit("%d keywords in kCssExcludedIdents: more than a uint8_t reaches" % len(idents))
    body.append(table("extern const PrCssKeyword kCssExcludedIdents[]", idents))
    body.append(table("extern const uint8_t kCssPropertyExcludedIdents[PrCssProperty_Count]",
                      [runs_of.get(tuple(excluded.get(name, ())), 0) for name in props],
                      comments=list(props)))

    # Each property's keyword forms, sorted by the keywords written.
    forms = keyword_forms(g)
    unknown = len(g.keywords)
    idents = lambda words: [keyword_ident(word)
                            for word in words] + ["PrCssKeyword_Unknown"] * (3 - len(words))
    rows, spans = [], {}
    for name in props:
        if name not in forms:
            continue
        keyed = []
        for written, held in forms[name]:
            ids = [g.keywords[word]
                   for word in written.split()] + [unknown] * (3 - len(written.split()))
            keyed.append(((ids[0] << 32) | (ids[1] << 16) | ids[2], written, held))
        spans[name] = (len(rows), len(keyed))
        rows += ["{ 0x%012xull, { %s } }  // %s: %s"
                 % (key, ", ".join(idents(held.split())), written, held) for key, written,
                 held in sorted(keyed)]
    body.append("\nextern const CssKeywordForm kCssKeywordForms[] = {\n%s};\n"
                % "".join("    %s\n" % row.replace("}  //", "},  //", 1) for row in rows))
    body.append(table("extern const CssKeywordFormRun kCssPropertyKeywordForms[PrCssProperty_Count]",
                      ["{ %d, %d }" % spans.get(name, (0, 0)) for name in props],
                      comments=list(props)))

    body.append("\nPrStringView cssPropertyName(PrCssProperty property) {\n    return %s;\n}\n"
                % names.name("property"))
    body.append("\nPrCssProperty cssPropertyFromText(PrStringView text) {\n"
                + names.lookup("text", "property")
                + "    return %s\n        ? PrCssProperty(property) : PrCssProperty_Count;\n}\n"
                % names.confirm("text", "cssPropertyName(PrCssProperty(property))"))

    span = lambda pair: "{ %d, %d }" % pair
    body.append(table("extern const CssShorthand kCssPropertyShorthand[PrCssProperty_Count]",
                      ["CssShorthand_" + shorthands[name].form if name in shorthands
                       else "CssShorthand_None" for name in props], comments=list(props)))
    body.append(table("extern const CssPropertySpan kCssPropertyLonghands[PrCssProperty_Count]",
                      [span(longhands.get(name, (0, 0))) for name in props], comments=list(props)))
    body.append(table("extern const CssPropertySpan kCssPropertyShorthands[PrCssProperty_Count]",
                      [span(of.get(name, (0, 0))) for name in props], comments=list(props)))
    body.append(bitset("extern const uint64_t "
                       "kCssPropertyUnwritten[bitsetWords(PrCssProperty_Count)]",
                       [name in NOT_WRITTEN for name in props], names=list(props)))
    body.append(table("extern const PrCssProperty kCssPropertyRuns[]",
                      [property_ident(name) for name in runs], 4))
    body.append("\nPrStringView cssPropertyInitial(PrCssProperty property) {\n"
                "    PrStringView text = %s;\n    return text.size != 0 ? text : "
                "PrStringView{};\n}\n" % initials.view("property"))
    write(path, namespaced(head, body))


def emit_at_rule_header(path, at_rules, descs):
    head = [BANNER, "\n#pragma once\n",
            '\n#include "pr/css.h"\n\n#include "pr/bitset.h"\n#include '
            '"css/src/css_grammar.h"\n' '#include "css/src/css_syntax_rule.h"\n'
            '#include "css/src/css_value_def.h"\n']
    body = ["\n// Every at-rule. CssAtRule_None is zero and has the empty name. It stands for the block of a "
            "style rule or\n// of a style attribute, which holds CSS properties.\n"
            "enum {\n"]
    body += enum_members([("CssAtRule_None", "")]
                         + [(at_rule_ident(rule.name), rule.name) for rule in at_rules.rules])
    body.append("\n    CssAtRule_Count,\n};\n")
    body.append("typedef uint8_t CssAtRule;\n")
    body.append("\n// The at-rule's name in lowercase, without its '@'.\n"
                "PrStringView cssAtRuleName(CssAtRule rule);\n")
    body.append("\n// The at-rule that `text` names, or CssAtRule_None. `text` has no '@'. At-rule names "
                "match ASCII\n// case-insensitively, as keywords do (css-values-4/#keywords).\n"
                "CssAtRule cssAtRuleFromText(PrStringView text);\n")
    body.append("\n// The at-rule that a legacy alias parses as. Every other at-rule maps to itself.\n"
                "extern const CssAtRule kCssAtRuleAlias[CssAtRule_Count];\n")
    body.append("\n// The CSSRule constant of the object the at-rule creates "
                "(cssom-1/#the-cssrule-interface), or\n// zero for a rule with no constant.\n"
                "extern const PrCssRuleType kCssAtRuleType[CssAtRule_Count];\n")
    body.append("\n// What the rule's block holds, or zero for a rule that accepts no block.\n"
                "extern const CssContents kCssAtRuleContents[CssAtRule_Count];\n")
    body.append("\n// Whether the rule may end at a ';' rather than a block.\n"
                "extern const uint64_t kCssAtRuleStatement[bitsetWords(CssAtRule_Count)];\n")
    body.append("\n// The grammar of the prelude before a block, and before a ';'. "
                "CssGrammarRoot_None is a\n// prelude of no tokens.\n"
                "extern const CssGrammarIndex kCssAtRuleBlockPrelude[CssAtRule_Count];\n"
                "extern const CssGrammarIndex kCssAtRuleStatementPrelude[CssAtRule_Count];\n")
    body.append("\n// The prelude grammar and block contents of the qualified rules in the rule's block, where "
                "those are not\n// style rules. Contents of zero mean that the block's qualified rules are "
                "style rules, or that it holds none.\nextern const CssGrammarIndex "
                "kCssAtRuleQualifiedPrelude[CssAtRule_Count];\nextern const CssContents "
                "kCssAtRuleQualifiedContents[CssAtRule_Count];\n")
    body.append("\n// The properties a declaration in the rule's block may name besides its "
                "descriptors.\nenum {\n    CssPropertiesIn_None,\n    CssPropertiesIn_Custom,   // "
                "custom properties\n    CssPropertiesIn_All,      // every CSS property\n};\n"
                "typedef uint8_t CssPropertiesIn;\n\n// What each at-rule's block accepts besides its descriptors.\n"
                "extern const CssPropertiesIn "
                "kCssAtRuleProperties[CssAtRule_Count];\n")
    body.append("\n// The grammar of every declaration's value, for a rule whose declarations the author "
                "names\n// (css-fonts-4/#font-feature-values-syntax). CssGrammarRoot_None for any other "
                "rule.\nextern const CssGrammarIndex "
                "kCssAtRuleNamedValue[CssAtRule_Count];\n")
    body.append("\n// Whether the rule's block adds its declarations to an earlier rule of the same at-rule "
                "in\n// its parent, if there is one.\n"
                "extern const uint64_t kCssAtRuleMerged[bitsetWords(CssAtRule_Count)];\n")
    body.append("\n// Whether a declaration in the rule's block may be !important.\n"
                "extern const uint64_t kCssAtRuleImportant[bitsetWords(CssAtRule_Count)];\n")
    body.append("\n// The at-rule that the rule is valid only inside, or CssAtRule_None for a rule "
                "valid in a style\n// sheet.\nextern const CssAtRule "
                "kCssAtRuleWithin[CssAtRule_Count];\n")
    body.append("\n// Whether the rule is valid inside a style rule "
                "(css-nesting-1/#nested-group-rules).\nextern const uint64_t "
                "kCssAtRuleNests[bitsetWords(CssAtRule_Count)];\n")

    idents = [descriptor_ident(key) for key in descs] + ["PrCssDescriptor_Count"]
    spans = {}
    for at, (rule, _) in enumerate(descs):
        first, end = spans.get(rule, (at, at))
        if end != at:
            sys.exit("the descriptors of %s are not consecutive" % rule)
        spans[rule] = (first, at + 1)
    body.append("\n// The first descriptor of each at-rule, and one past its last. An at-rule's descriptors "
                "are consecutive.\n")
    for rule, (first, end) in spans.items():
        prefix = "kCssDescriptor" + camel(rule[1:])
        body.append("constexpr PrCssDescriptor %sFirst = %s;\nconstexpr PrCssDescriptor %sEnd = "
                    "%s;\n" % (prefix, idents[first], prefix, idents[end]))
    body.append("\n// The descriptor's name in lowercase.\n"
                "PrStringView cssDescriptorName(PrCssDescriptor descriptor);\n")
    body.append("\n// The descriptor that a legacy name alias declares (css-cascade-5/#legacy-name-alias). "
                "Every other\n// descriptor maps to itself.\nextern const PrCssDescriptor "
                "kCssDescriptorAlias[PrCssDescriptor_Count];\n")
    body.append("\n// The root of each descriptor's grammar, an index into kCssGrammarNodes.\n"
                "extern const CssGrammarIndex kCssDescriptorGrammarRoot[PrCssDescriptor_Count];\n")
    body.append("\n// Whether the descriptor's value is tokenized with unicode ranges allowed. "
                "css-syntax-3/#parse-list-of-component-values\n// allows them for a descriptor whose grammar "
                "holds a <unicode-range-token>.\nextern const uint64_t "
                "kCssDescriptorUnicodeRanges[bitsetWords(PrCssDescriptor_Count)];\n")
    body.append("\n// Whether the descriptor's two values are the bounds of a range. They "
                "serialize as one value\n// if they are the same "
                "(css-fonts-4/#serializing-at-rules).\nextern const uint64_t "
                "kCssDescriptorBounded[bitsetWords(PrCssDescriptor_Count)];\n")
    body.append("\n// Whether the descriptor is a range feature (mediaqueries-5/#mq-range-context). A query "
                "may compare it,\n// and its name accepts the min- and max- prefixes.\n"
                "extern const uint64_t kCssDescriptorRange[bitsetWords(PrCssDescriptor_Count)];\n")
    body.append("\n// The query production that the descriptor is a feature of, if its at-rule's "
                "queries hold\n// several, such as @container's size, scroll-state and anchored "
                "features. CssValueDef_Count\n// otherwise.\nextern const CssValueDef "
                "kCssDescriptorFeature[PrCssDescriptor_Count];\n")
    body.append("\n// The descriptor that `text` names in `rule`, or PrCssDescriptor_None. Descriptor "
                "names\n// match ASCII case-insensitively, as keywords do "
                "(css-values-4/#keywords).\nPrCssDescriptor cssDescriptorFromText(CssAtRule rule, "
                "PrStringView text);\n")
    write(path, namespaced(head, body))


def feature_productions(webref_dir, descs):
    """Returns the query production that FEATURE_SECTIONS gives each descriptor whose definition is in one of its
    sections. webref records the section by the heading that the definition is under."""
    out = {}
    for (spec, section), production in FEATURE_SECTIONS.items():
        with open(os.path.join(webref_dir, "ed", "headings", spec + ".json"),
                  encoding="utf-8") as f:
            headings = json.load(f)["headings"]
        number = next((h["number"] for h in headings if h["id"] == section), None)
        if number is None:
            sys.exit("FEATURE_SECTIONS: %s has no section %s" % (spec, section))
        with open(os.path.join(webref_dir, "ed", "dfns", spec + ".json"), encoding="utf-8") as f:
            dfns = json.load(f)["dfns"]
        found = 0
        for dfn in dfns:
            if dfn["type"] != "descriptor" or not dfn["heading"].get("number",
                                                                     "").startswith(number + "."):
                continue
            for rule in dfn["for"]:
                for name in dfn["linkingText"]:
                    if (rule, name) in descs:
                        out[(rule, name)] = production
                        found += 1
        if not found:
            sys.exit("FEATURE_SECTIONS: %s#%s defines no descriptor" % (spec, section))
    return out


def emit_at_rule_source(path, at_rules, descs, g, features):
    head = [BANNER, '\n#include "css/src/css_at_rule.h"\n\n#include "pr/name_table.h"\n']
    rules = at_rules.rules
    by_name = {rule.name: rule for rule in rules}
    idents = ["CssAtRule_None"] + [at_rule_ident(rule.name) for rule in rules]

    def resolved(rule):
        return by_name[AT_RULE_ALIASES.get(rule.name, rule.name)]

    def column(declaration, none, value):
        return table(declaration, [none] + [value(resolved(rule)) for rule in rules],
                     comments=idents)

    def flags(declaration, none, value):
        return bitset(declaration, [none] + [value(resolved(rule)) for rule in rules],
                      names=["none"] + [rule.name for rule in rules])

    def root(key):
        return "%d" % g.roots.get(key, 0)

    rule_names = name_table.NameTable("kCssAtRuleName", [rule.name[1:] for rule in rules],
                                      lower=True)
    body = ["namespace {\n", rule_names.tables(), "\n} // namespace\n"]
    body.append("\nPrStringView cssAtRuleName(CssAtRule rule) {\n"
                "    return rule == CssAtRule_None ? PrStringView{} : %s;\n}\n"
                % rule_names.name("rule - 1"))
    body.append("\nCssAtRule cssAtRuleFromText(PrStringView text) {\n"
                + rule_names.lookup("text", "index")
                + "    return %s\n        ? CssAtRule(index + 1) : CssAtRule_None;\n}\n"
                % rule_names.confirm("text", rule_names.name("index")))

    body.append(table("extern const CssAtRule kCssAtRuleAlias[CssAtRule_Count]",
                      ["CssAtRule_None"] + [at_rule_ident(AT_RULE_ALIASES.get(rule.name, rule.name))
                         for rule in rules],
                      comments=idents))
    body.append(column("extern const PrCssRuleType kCssAtRuleType[CssAtRule_Count]",
                       "PrCssRuleType(0)",
                       lambda rule: ("PrCssRuleType_" + CSSOM_RULE_TYPES[rule.name]
                                     if rule.name in CSSOM_RULE_TYPES else "PrCssRuleType(0)")))
    body.append(column("extern const CssContents kCssAtRuleContents[CssAtRule_Count]", "0",
                       lambda rule: BLOCK_CONTENTS[rule.contents] if rule.contents else "0"))
    body.append(flags("extern const uint64_t kCssAtRuleStatement[bitsetWords(CssAtRule_Count)]",
                      False, lambda rule: rule.statement))
    body.append(column("extern const CssGrammarIndex kCssAtRuleBlockPrelude[CssAtRule_Count]", "0",
                       lambda rule: root(("at-rule", (rule.name, "block")))))
    body.append(column("extern const CssGrammarIndex kCssAtRuleStatementPrelude[CssAtRule_Count]",
                       "0", lambda rule: root(("at-rule", (rule.name, "statement")))))
    body.append(column("extern const CssGrammarIndex kCssAtRuleQualifiedPrelude[CssAtRule_Count]",
                       "0", lambda rule: root(("qualified-rule", rule.name))))
    body.append(column("extern const CssContents kCssAtRuleQualifiedContents[CssAtRule_Count]", "0",
                       lambda rule: BLOCK_CONTENTS[g.qualified[rule.name].contents]
                       if rule.name in g.qualified else "0"))
    body.append(column("extern const CssPropertiesIn kCssAtRuleProperties[CssAtRule_Count]",
                       "CssPropertiesIn_All",
                       lambda rule: AT_RULE_PROPERTIES.get(rule.name, "CssPropertiesIn_None")))
    body.append(column("extern const CssGrammarIndex kCssAtRuleNamedValue[CssAtRule_Count]", "0",
                       lambda rule: root(("named-value", rule.name))))
    body.append(flags("extern const uint64_t kCssAtRuleMerged[bitsetWords(CssAtRule_Count)]", False,
                      lambda rule: rule.name in AT_RULE_MERGED))
    body.append(flags("extern const uint64_t kCssAtRuleImportant[bitsetWords(CssAtRule_Count)]",
                      True, lambda rule: rule.name not in AT_RULE_NO_IMPORTANT))
    body.append(column("extern const CssAtRule kCssAtRuleWithin[CssAtRule_Count]", "CssAtRule_None",
                       lambda rule: at_rule_ident(at_rules.within[rule.name])
                       if rule.name in at_rules.within else "CssAtRule_None"))
    body.append(flags("extern const uint64_t kCssAtRuleNests[bitsetWords(CssAtRule_Count)]", False,
                      lambda rule: at_rules.nests(rule)))

    salts = [idents.index(at_rule_ident(key[0])) for key in descs]
    names = name_table.NameTable("kCssDescriptor", [key[1] for key in descs], lower=True,
                                 salts=salts)
    body += ["\nnamespace {\n",
             names.tables(), table("constexpr CssAtRule kCssDescriptorRule[]",
                   [at_rule_ident(key[0]) for key in descs]),
             "\n} // namespace\n"]

    body.append("\nPrStringView cssDescriptorName(PrCssDescriptor descriptor) {\n"
                "    return %s;\n}\n" % names.name("descriptor"))
    body.append(table("extern const PrCssDescriptor kCssDescriptorAlias[PrCssDescriptor_Count]",
                      [descriptor_ident((key[0], e.alias_of() or key[1])) for key,
                       e in descs.items()],
                      comments=[descriptor_ident(key) for key in descs]))
    body.append(grammar_root_table(g, "descriptor", descs, "kCssDescriptorGrammarRoot",
                                   ["%s %s" % key for key in descs]))
    descriptor_names = ["%s %s" % key for key in descs]
    body.append(bitset("extern const uint64_t "
                       "kCssDescriptorUnicodeRanges[bitsetWords(PrCssDescriptor_Count)]",
                       [key in g.unicode_range_descriptors for key in descs],
                       names=descriptor_names))
    body.append(bitset("extern const uint64_t "
                       "kCssDescriptorBounded[bitsetWords(PrCssDescriptor_Count)]",
                       [key in BOUNDED_DESCRIPTORS for key in descs], names=descriptor_names))
    body.append(bitset("extern const uint64_t "
                       "kCssDescriptorRange[bitsetWords(PrCssDescriptor_Count)]",
                       [e.range_feature() for e in descs.values()], names=descriptor_names))
    body.append(table("extern const CssValueDef kCssDescriptorFeature[PrCssDescriptor_Count]",
                      [value_ident(features[key], "type") if key in features
                       else "CssValueDef_Count" for key in descs],
                      comments=[descriptor_ident(key) for key in descs]))

    body.append("\nPrCssDescriptor cssDescriptorFromText(CssAtRule rule, PrStringView text) {\n"
                + names.lookup("text", "descriptor", "rule")
                + "    if (kCssDescriptorRule[descriptor] != rule\n" + "        || !%s) {\n"
                % names.confirm("text", "cssDescriptorName(PrCssDescriptor(descriptor))")
                + "        return PrCssDescriptor_None;\n    }\n"
                  "    return PrCssDescriptor(descriptor);\n}\n")
    write(path, namespaced(head, body))


def emit_value_def_header(path, values):
    members = [(value_ident(name, e.kind), name) for name, e in values.items()]
    first_fn = next((index for index, e in enumerate(values.values()) if e.kind == "function"),
                    None)
    head = [BANNER, "\n#pragma once\n",
            '\n#include "pr/bitset.h"\n#include "css/src/css_grammar.h"\n#include '
            '"css/src/css_keyword.h"\n']
    body = ["\n// Every type and function that a grammar names, such as <length> and counter().\nenum {\n"]
    body += enum_members(members, first_fn, "CssValueDef_FunctionFirst",
                         ["Functions, which occupy [CssValueDef_FunctionFirst, CssValueDef_Count). Every "
                          "member before", "them is a type."])
    body.append("\n    CssValueDef_Count,\n};\n")
    body.append("typedef uint16_t CssValueDef;\n")
    body.append('\n// The name of each production as the specs write it, as in "<bg-image>" or "counter()".\n'
                "extern const PrStringView kCssValueDefName[CssValueDef_Count];\n")
    body.append("\n// The root of each production's grammar, an index into kCssGrammarNodes.\n"
                "extern const CssGrammarIndex kCssValueDefGrammarRoot[CssValueDef_Count];\n")
    body.append("\n// Whether the production's components are written with no whitespace between "
                "them.\nextern const uint64_t "
                "kCssValueDefNoWhitespace[bitsetWords(CssValueDef_Count)];\n")
    body.append("\n// Whether a structural match keeps a node for the production. A prose production keeps "
                "none, since its\n// terminal is a value of its own. Nor does a production whose node would add "
                "nothing to its children's: one\n// production, a choice of productions, or a run of one production "
                "that every reference takes as a whole\n// grammar.\n"
                "extern const uint64_t kCssValueDefKeepsNode[bitsetWords(CssValueDef_Count)];\n")
    body.append("\n// The math function that a function named `keyword` is (css-values-4/#math), or "
                "CssValueDef_Count\n// if no math function has the name.\n"
                "CssValueDef cssMathFunction(PrCssKeyword keyword);\n")
    body.append("\n// The <whole-value> function that a function named `keyword` is "
                "(css-values-5/#whole-value), or\n// CssValueDef_Count if none has the name.\n"
                "CssValueDef cssWholeValueFunction(PrCssKeyword keyword);\n")
    body.append("\n// The production that the arguments of the arbitrary substitution function named "
                "`keyword` must match\n// before substitution (css-values-5/#arbitrary-substitution). "
                "CssValueDef_Count if no such function\n// has the name.\nCssValueDef cssSubstitutionFunction(PrCssKeyword keyword);\n")
    write(path, namespaced(head, body))


def emit_value_def_source(path, values, g):
    for name in sorted(NO_WHITESPACE - set(values)):
        sys.exit("NO_WHITESPACE names %s, which webref does not define" % name)
    head = [BANNER, '\n#include "css/src/css_value_def.h"\n']
    body = [table("extern const PrStringView kCssValueDefName[CssValueDef_Count]",
                  [view(n) for n in values]),
            grammar_root_table(g, "value", values, "kCssValueDefGrammarRoot", list(values)),
            bitset("extern const uint64_t kCssValueDefNoWhitespace[bitsetWords(CssValueDef_Count)]",
                   [name in NO_WHITESPACE for name in values], names=list(values)),
            bitset("extern const uint64_t kCssValueDefKeepsNode[bitsetWords(CssValueDef_Count)]",
                   [g.keeps_node(name) for name in values], names=list(values))]
    for function, table_name, pairs in (
            ("cssMathFunction", "MATH_FUNCTIONS", [(name, name) for name in MATH_FUNCTIONS]),
            ("cssWholeValueFunction", "WHOLE_VALUE_FUNCTIONS",
             [(name, name) for name in WHOLE_VALUE_FUNCTIONS]),
            ("cssSubstitutionFunction", "SUBSTITUTION_FUNCTIONS",
             list(SUBSTITUTION_FUNCTIONS.items()))):
        for _, name in pairs:
            if name not in values or values[name].grammar() is None:
                sys.exit("%s names %s, which webref gives no grammar" % (table_name, name))
        width = max(len(keyword_ident(keyword[:-2])) for keyword, _ in pairs) + len("    case :")
        body.append("\nCssValueDef %s(PrCssKeyword keyword) {\n    switch (keyword) {\n" % function)
        for keyword, name in pairs:
            body.append("%s return %s;\n"
                        % (("    case %s:" % keyword_ident(keyword[:-2])).ljust(width),
                                              value_ident(name, values[name].kind)))
        body.append("    default:\n        return CssValueDef_Count;\n    }\n}\n")
    write(path, namespaced(head, body))


# ----------------------------------------------------------------------------------------------------------------------
# Grammar tables

# A leading dash is part of the keyword (-infinity is not infinity, -n is not n) except on a
# vendor prefix, where it is noise.
VENDOR_PREFIXES = ("-webkit-", "-moz-", "-ms-", "-o-", "-apple-", "-khtml-", "-epub-")


def keyword_ident(name):
    ident = camel(name)
    if name.startswith("-") and not name.startswith(VENDOR_PREFIXES):
        ident = "Minus" + ident
    if name.endswith("-"):
        ident += "Dash"
    return "PrCssKeyword_" + ident


def type_ident(name):
    # <foo> names a type, <foo()> names a function.
    inner = name[1:-1]
    if inner.endswith("()"):
        return value_ident(inner, "function")
    return value_ident("<%s>" % inner, "type")


def char_literal(c):
    return "'\\%s'" % c if c in "'\\" else "'%s'" % c


class Grammars:
    """Every grammar, parsed and flattened into one node array."""

    def type_value(self, name):
        self.referenced_types.add(name)
        return type_ident(name)

    def __init__(self):
        self.nodes = []          # rows, each a tuple of C expressions
        self.blocks = []         # (label, first node index) for readability
        # The index of every subtree written, keyed by everything that it emits (subtree_key). A definition whose
        # grammar is already written takes that subtree as its root instead of a copy.
        self.subtrees = {}
        self.ranges = [None]     # range 0 is unbounded
        self.numbers = []        # (value, unit) of each numeric literal, in order of first use
        # The trivial modifier is index 0: once, no flags, no range.
        self.modifiers = {("1", "1", "0", 0): 0}
        self.keywords = {}       # name -> PrCssKeyword index
        self.roots = {}          # (kind, name) -> node index
        self.referenced_types = set()   # <type> names that some grammar mentions
        self.trees = {}          # ("type" | "property", name) -> the tree a reference reaches
        self.prose = set()       # the value definitions the specs write in prose
        self.first = {}          # the same keys -> (first set, can be empty), once computed
        self.firsts = {}         # (kinds, can be empty) -> index into kCssGrammarFirsts
        self.node_firsts = []    # per node, its index into kCssGrammarFirsts
        self.node_shapes = []    # per node, the CssGrammarShape flags
        self.follow = {}         # id(node) -> the follow set: what can come after the node
        self.no_node = set()     # productions that a structural match keeps no node for
        self.written = set()     # id() of each || or && that WRITTEN_ORDER keeps as written
        self.omissions = {}      # id() of each node that OMISSIONS names -> what it implies
        self.omissible = set()   # id() of each such node that is under a ? or repeated
        self.omits_child = set() # id() of each || that has such a child, taken once
        self.omission_rows = []  # (node index, what it implies), in node order
        self.fractions = set()   # id() of each <percentage> that FRACTIONS names
        self.folded = set()      # id() of each reference to a production that FOLDED_PRODUCTIONS names
        self.unicode_range_descriptors = set()  # descriptors whose value holds a unicode range

    def range_index(self, bounds):
        if bounds is None:
            return 0
        if bounds not in self.ranges:
            self.ranges.append(bounds)
        return self.ranges.index(bounds)

    def number_index(self, literal):
        """Interns a numeric literal as its value and its unit, PrCssUnit_Number for a number."""
        match = re.fullmatch(r"(\d+(?:\.\d+)?)([a-z]*)", literal)
        key = (float(match.group(1)), match.group(2))
        if key not in self.numbers:
            self.numbers.append(key)
        return self.numbers.index(key)

    def modifier_index(self, node):
        """Interns (min, max, flags, range). Most nodes share the trivial modifier, so interning turns four fields into
        six bits."""
        flags = []
        if node.comma:
            flags.append("CssGrammarFlag_Comma")
            if not self.matches_comma(node, 0):
                flags.append("CssGrammarFlag_Delimited")
        if node.required:
            flags.append("CssGrammarFlag_Required")
        if id(node) in self.folded:
            flags.append("CssGrammarFlag_Folds")
        key = ("%d" % node.min,
               "kCssGrammarUnbounded" if node.max == INF else "%d" % node.max,
               " | ".join(flags) if flags else "0",
               self.range_index(node.range))
        if key not in self.modifiers:
            self.modifiers[key] = len(self.modifiers)
        return self.modifiers[key]

    # The depth to which a grammar is walked, looking for a comma, before the scan assumes that it can take one.
    COMMA_SCAN_DEPTH = 32

    def matches_comma(self, node, depth):
        """Returns true if matching `node` can consume a comma that the scan for a # list's separators would take
        instead. A function's arguments are inside its parentheses, so they never can."""
        if depth == self.COMMA_SCAN_DEPTH:
            return True
        if node.type == "literal":
            return node.value == ","
        if node.type == "function":
            return False
        if node.type in ("type", "property"):
            tree = self.trees.get((node.type, normalize_ref(node.value) if node.type == "type"
                                   else node.value))
            return tree is not None and self.matches_comma(tree, depth + 1)
        return any(self.matches_comma(child, depth + 1) for child in node.children)

    # A prose production named for a token starts with that token: <ident-token> is
    # CssTokenKind_Ident (css-syntax-3/#tokenization).
    TOKEN_PRODUCTION = re.compile(r"<([a-z]+(?:-[a-z]+)*)-token>")

    # The entry that a set holds for any token, and a first set that holds any token. The entry is not a character,
    # because '*' is a character that a grammar writes.
    ANY_TOKEN = "<any>"
    ANY = (frozenset([ANY_TOKEN]), frozenset([ANY_TOKEN]), False)

    def prose_first(self, name):
        m = self.TOKEN_PRODUCTION.fullmatch(name)
        kinds = [camel(m.group(1))] if m else PROSE_TOKENS.get(name)
        empty = name in PROSE_EMPTY
        if kinds is None:
            return self.ANY[0], self.ANY[1], empty
        return frozenset("CssTokenKind_" + kind for kind in kinds), frozenset(), empty

    def first_once(self, node):
        """Returns how a match of `node`, taken once, can start: the token kinds, as C enumerators, the literal
        characters, and whether it can match nothing. ANY_TOKEN in both means any token."""
        kind = node.type
        if kind == "keyword":
            return frozenset(["CssTokenKind_Ident"]), frozenset(), False
        if kind == "number":
            return frozenset(["CssTokenKind_Number", "CssTokenKind_Dimension"]), frozenset(), False
        if kind == "literal":
            return frozenset(), frozenset([node.value]), False
        if kind == "atkeyword":
            return frozenset(["CssTokenKind_AtKeyword"]), frozenset(), False
        if kind == "function":
            return frozenset(["CssTokenKind_Function"]), frozenset(), False
        if kind in ("type", "property"):
            key = (kind, normalize_ref(node.value) if kind == "type" else node.value)
            if key in self.trees:
                return self.first.get(key, (frozenset(), frozenset(), False))
            return self.prose_first(node.value) if kind == "type" else self.ANY
        if kind == "longhand":
            return self.first_of(node.children[0])

        # css-values-4/#comb-comma: a comma in a sequence is taken only after something before it
        # in the sequence, so it never starts one.
        children = node.children
        if kind == "seq":
            children = [child for child in children if not self.is_comma(child)]
        parts = [self.first_of(child) for child in children]
        kinds, literals = set(), set()
        if kind == "seq":
            for first, chars, empty in parts:
                kinds |= first
                literals |= chars
                if not empty:
                    return frozenset(kinds), frozenset(literals), False
            return frozenset(kinds), frozenset(literals), True
        for first, chars, _ in parts:
            kinds |= first
            literals |= chars
        empties = [empty for _, _, empty in parts]
        empty = all(empties) if kind == "allof" else any(empties)
        return frozenset(kinds), frozenset(literals), empty

    @staticmethod
    def is_comma(node):
        return node.type == "literal" and node.value == ","

    def first_of(self, node):
        """Returns first_once under the node's multiplier. A group marked ! must produce a value, so it cannot match
        nothing, however optional its members are."""
        kinds, literals, empty = self.first_once(node)
        return kinds, literals, (empty and not node.required) or node.min == 0

    def compute_first(self):
        """Computes the first sets of every type that a reference reaches, to a fixed point. The set of a grammar that
        refers to itself grows until nothing changes."""
        self.first = {}
        changed = True
        while changed:
            changed = False
            for key, tree in self.trees.items():
                value = self.first_of(tree)
                if self.first.get(key) != value:
                    self.first[key] = value
                    changed = True

    def first_index(self, node):
        """Interns the node's first set, taken once."""
        kinds, literals, empty = self.first_once(node)
        empty = empty and not node.required
        if self.ANY_TOKEN in kinds or self.ANY_TOKEN in literals:
            key = ((self.ANY_TOKEN,), (self.ANY_TOKEN,), empty)
        else:
            key = (tuple(sorted(kinds)), tuple(sorted(literals)), empty)
        if key not in self.firsts:
            self.firsts[key] = len(self.firsts)
        return self.firsts[key]

    # ------------------------------------------------------------------------------------------------------------------
    # Follow sets, and the multipliers that the next token decides

    EOF = (frozenset(["CssTokenKind_Eof"]), frozenset())

    @staticmethod
    def union(a, b):
        return a[0] | b[0], a[1] | b[1]

    @staticmethod
    def overlap(a, b):
        """Returns true if one token could start both sets: a kind in both, a character in both, or a character in one
        whose kind is in the other."""
        (ka, la), (kb, lb) = a, b
        if Grammars.ANY_TOKEN in ka | la | kb | lb:
            return True
        kind = lambda c: PUNCTUATION_TOKENS.get(c, "CssTokenKind_Delim")
        return bool(ka & kb or la & lb or {kind(c) for c in la} & kb or {kind(c) for c in lb} & ka)

    def compute_follow(self, trees):
        """Computes what can come after each node, over every grammar at once. A type's follow set is the union of the
        follow sets of its references. Every grammar may be matched on its own, so the end of input can follow each
        one."""
        self.follow = {}
        type_follow = {}
        changed = True

        def add(key, table, value):
            nonlocal changed
            old = table.get(key, (frozenset(), frozenset()))
            new = (old[0] | value[0], old[1] | value[1])
            if new != old:
                table[key] = new
                changed = True

        def visit(node, follow):
            add(id(node), self.follow, follow)
            # What comes after one repetition: the next repetition, or in a # list, the comma before it.
            inner = follow
            if node.comma:
                inner = self.union(inner, (frozenset(), frozenset([","])))
            elif node.max > 1:
                inner = self.union(inner, self.first_once(node)[:2])
            if node.type == "seq":
                for index, child in enumerate(node.children):
                    after = (frozenset(), frozenset())
                    empty = True
                    for rest in node.children[index + 1:]:
                        kinds, literals, can_empty = self.first_of(rest)
                        after = self.union(after, (kinds, literals))
                        # A comma is omitted if what it separates is omitted.
                        if not can_empty and not self.is_comma(rest):
                            empty = False
                            break
                    visit(child, self.union(after, inner) if empty else after)
            elif node.type in ("oneof", "anyof", "allof"):
                for child in node.children:
                    others = (frozenset(), frozenset())
                    if node.type != "oneof":
                        for other in node.children:
                            if other is not child:
                                others = self.union(others, self.first_of(other)[:2])
                    visit(child, self.union(inner, others))
            elif node.type == "function":
                visit(node.children[0], (frozenset(), frozenset([")"])))
            elif node.type == "longhand":
                visit(node.children[0], inner)
            elif node.type in ("type", "property"):
                key = (node.type, normalize_ref(node.value) if node.type == "type" else node.value)
                add(key, type_follow, inner)

        while changed:
            changed = False
            for kind, name, _, tree in trees:
                key = (("type", name) if kind == "value" else ("property", name)
                       if kind == "property" else (kind, name))
                visit(tree, self.union(type_follow.get(key, (frozenset(), frozenset())), self.EOF))

    def decided(self, node):
        """Returns true if the next token decides whether a multiplied node is taken once more. That holds if nothing
        that could start the node can follow it, and the node cannot match nothing. A # list takes one more item exactly
        when a comma comes next, which decides every item except the first."""
        if (node.min, node.max) == (1, 1) or id(node) not in self.follow:
            return False
        kinds, literals, empty = self.first_once(node)
        if empty and not node.required:
            return False
        follow = self.follow[id(node)]
        if node.comma:
            return not self.overlap((frozenset(), frozenset([","])), follow)
        return not self.overlap((kinds, literals), follow)

    # ------------------------------------------------------------------------------------------------------------------
    # The productions that a structural match keeps a node for

    def compute_nodes(self, trees):
        """Computes which productions a structural match keeps a node for. A prose production's terminal is a value of
        its own. A production that is one production, or a choice of productions that each produce one node naming
        itself, would add a name that nothing reads. A production that is one repeated reference would add the extent
        of the run, which every grammar that consists of it already has."""
        trivial = lambda node: ((node.min, node.max) == (1, 1) and not node.comma
                                and not node.required and node.range is None)
        reference = lambda node: node.type == "type" and not node.children

        partial = set()   # productions that some grammar refers to as only part of its value
        for kind, name, _, tree in trees:
            for node in walk(tree):
                if reference(node) and not (node is tree and trivial(node)):
                    partial.add(normalize_ref(node.value))

        runs = {name for (kind, name), tree in self.trees.items()
                if kind == "type" and reference(tree) and name not in partial}

        def names_itself(node):
            if not reference(node) or not trivial(node):
                return False
            name = normalize_ref(node.value)
            return ("type", name) in self.trees and name not in runs

        choices = set()
        for (kind, name), tree in self.trees.items():
            if kind != "type" or name in runs:
                continue
            alternatives = tree.children if tree.type == "oneof" and trivial(tree) else [tree]
            if all(names_itself(child) for child in alternatives):
                choices.add(name)
        self.no_node = self.prose | runs | choices

    def keeps_node(self, name):
        return name not in self.no_node

    LEAVES = ("keyword", "number", "literal", "atkeyword")

    def leaf(self, node):
        """Returns true if the matcher takes the node in one step: a keyword, a numeric literal, a literal, an
        at-keyword or a prose production's terminal, taken once."""
        if (node.min, node.max) != (1, 1) or node.comma or node.required or node.range:
            return False
        if node.type == "type":
            return not node.children and normalize_ref(node.value) in self.prose
        return node.type in self.LEAVES or self.keyword_set(node)

    def shape(self, node):
        flags = []
        if ((node.min, node.max) == (1, 1) and not node.comma and node.range is None
                and (not node.required or node.type == "seq")):
            flags.append("CssGrammarShape_Once")
        if node.min == 0 and not node.required:
            flags.append("CssGrammarShape_Optional")
        if self.decided(node):
            flags.append("CssGrammarShape_Decided")
        if (node.type == "seq" and not node.required
            and all(self.leaf(child) for child in node.children)
                and not any(child.type == "literal" and child.value == ","
                            for child in node.children)):
            flags.append("CssGrammarShape_Leaves")
        if id(node) in self.written:
            flags.append("CssGrammarShape_Written")
        if id(node) in self.omissible:
            flags.append("CssGrammarShape_Omissible")
        if id(node) in self.omits_child:
            flags.append("CssGrammarShape_OmitsChild")
        if id(node) in self.fractions:
            flags.append("CssGrammarShape_Fraction")
        return " | ".join(flags) if flags else "0"

    def keyword_set(self, node):
        """Returns true if the matcher can settle a | with a single keyword lookup: every alternative is a bare keyword,
        none has a multiplier, and no keyword appears twice. A | with a repeated keyword stays a plain |, in which the
        first matching alternative wins."""
        if node.type != "oneof" or len(node.children) < 2:
            return False
        for child in node.children:
            if child.type != "keyword" or child.children or self.modifier_index(child) != 0:
                return False
        values = [child.value for child in node.children]
        return len(set(values)) == len(values)

    def row(self, node, next, root=False):
        # A reference to a prose production is written as its terminal, so matching one takes no hop through the
        # production's root. A reference that is a whole grammar stays a reference, because the production that it
        # names is a production of its own, as in <id-selector> = <hash-token>.
        if node.type == "type" and not node.children and not root:
            name = normalize_ref(node.value)
            if name in self.prose:
                self.referenced_types.add(node.value)
                ident = terminal_ident(name, "function" if name.endswith("()") else "type")
                return ("CssGrammarType_Terminal", "%d" % self.modifier_index(node), "%d" % next,
                        ident)
        value = {
            "keyword": lambda: keyword_ident(node.value),
            "atkeyword": lambda: keyword_ident(node.value),
            "function": lambda: keyword_ident(node.value),
            "type": lambda: self.type_value(node.value),
            "property": lambda: property_ident(node.value),
            "literal": lambda: char_literal(node.value),
            "number": lambda: "%d" % self.number_index(node.value),
            "atrule": lambda: at_rule_ident(node.value),
            "longhand": lambda: property_ident(node.value),
        }.get(node.type, lambda: "0")()
        kind = {
            "keyword": "CssGrammarType_Keyword", "number": "CssGrammarType_Number",
            "literal": "CssGrammarType_Literal",
            "atkeyword": "CssGrammarType_AtKeyword", "type": "CssGrammarType_Type",
            "property": "CssGrammarType_Property", "oneof": "CssGrammarType_OneOf",
            "anyof": "CssGrammarType_AnyOf", "allof": "CssGrammarType_AllOf",
            "seq": "CssGrammarType_Sequence", "function": "CssGrammarType_Function",
            "atrule": "CssGrammarType_AtRule", "longhand": "CssGrammarType_Longhand",
        }[node.type]
        if self.keyword_set(node):
            kind = "CssGrammarType_KeywordSet"
        return (kind, "%d" % self.modifier_index(node), "%d" % next, value)

    def add(self, label, tree):
        """Writes a definition's grammar and returns its root. If an already written subtree emits the same rows, the
        definition shares it. Otherwise the root is the subtree written for the definition."""
        start = len(self.nodes)
        rows = len(self.omission_rows)
        added = []
        index = self._emit(tree, added, True)
        shared = self.subtrees.get(self.subtree_key(index))
        if shared is not None and shared < start:
            del self.nodes[start:]
            del self.node_firsts[start:]
            del self.node_shapes[start:]
            del self.omission_rows[rows:]
            for key in added:
                del self.subtrees[key]
            index = shared
        self.blocks.append((label, index))
        return index

    def subtree_key(self, index):
        """Returns everything that the subtree at `index` emits: its rows, first sets and shapes, and what the omissions
        within it imply, with their positions."""
        end = index + int(self.nodes[index][2])
        omitted = tuple((at - index, repr(implied)) for at, implied in self.omission_rows
                        if index <= at < end)
        return (tuple(self.nodes[index:end]), tuple(self.node_firsts[index:end]),
                tuple(self.node_shapes[index:end]), omitted)

    def _emit(self, node, added, root=False):
        """Appends the node and its subtree in preorder, and returns the node's index. Registers each subtree that it
        writes that was not already written, and adds the subtree's key to `added`."""
        index = len(self.nodes)
        self.nodes.append(None)
        self.node_firsts.append(None)
        self.node_shapes.append("0")
        for child in node.children:
            self._emit(child, added)
        # Once the subtree is written, the distance to the end is the span of this node.
        self.nodes[index] = self.row(node, len(self.nodes) - index, root)
        self.node_firsts[index] = self.first_index(node)
        self.node_shapes[index] = self.shape(node)
        if id(node) in self.omissions:
            self.omission_rows.append((index, self.omissions[id(node)]))
        key = self.subtree_key(index)
        if key not in self.subtrees:
            self.subtrees[key] = index
            added.append(key)
        return index


def type_refs(tree):
    return [n.value for n in walk(tree) if n.type == "type"]


def normalize_ref(ref):
    # <foo()> refers to the value def named foo().
    inner = ref[1:-1]
    return inner if inner.endswith("()") else ref


def reaches_type(trees, root, wanted):
    """Returns true if the tree at `root` names `wanted`, directly or through the types that it names."""
    grammars = {name: tree for kind, name, _, tree in trees if kind == "value"}
    queue = list(type_refs(root))
    seen = set()
    while queue:
        name = normalize_ref(queue.pop())
        if name in seen:
            continue
        seen.add(name)
        if name == wanted:
            return True
        if name in grammars:
            queue += type_refs(grammars[name])
    return False


def reachable_terminals(values, trees):
    """Returns the prose productions that a declaration's value can reach, directly or through the types that it
    names. The rest belong to at-rule preludes, selectors and queries, which are parsed by code that already knows what
    it is reading."""
    grammars = {name: tree for kind, name, _, tree in trees if kind == "value"}
    prose = {name for name, e in values.items() if e.grammar() is None}

    queue = [r for kind, _, _, tree in trees if kind in ("property", "descriptor")
             for r in type_refs(tree)]
    seen, found = set(), set()
    while queue:
        name = normalize_ref(queue.pop())
        if name in seen:
            continue
        seen.add(name)
        if name in prose:
            found.add(name)
        elif name in grammars:
            queue += type_refs(grammars[name])
        if name in NUMERIC_TERMINALS:
            queue += MATH_FUNCTIONS
    return found


# ----------------------------------------------------------------------------------------------------------------------
# Shorthands

class Shorthand:
    """A shorthand, its form and the longhands that it sets, in canonical order."""

    def __init__(self, name, form, longhands):
        self.name = name
        self.form = form
        self.longhands = longhands


def says_shorthand(e):
    """Returns true if webref's own entry calls the property a shorthand, by giving its initial value as that of its
    longhands."""
    best = e._best()
    initial = best[2].get("initial", "").lower() if best is not None else ""
    return "individual properties" in initial or "shorthand properties" in initial


def shorthands_of(props):
    """Returns SHORTHANDS, checked against the properties that webref has."""
    forms = {form for form, _ in SHORTHAND_FORMS}
    out = collections.OrderedDict()
    for name, (form, longhands) in SHORTHANDS.items():
        if name not in props or props[name].alias_of() is not None:
            sys.exit("SHORTHANDS: %s is not a property webref has" % name)
        if form not in forms:
            sys.exit("SHORTHANDS: %s has no form %s" % (name, form))
        if longhands is None:
            longhands = [p for p, e in props.items()
                         if e.alias_of() is None and p not in SHORTHANDS and p not in NOT_IN_ALL
                         and not says_shorthand(e)]
        for longhand in longhands:
            if longhand not in props or props[longhand].alias_of() is not None:
                sys.exit("SHORTHANDS: %s sets %s, which is not a property webref has"
                         % (name, longhand))
            if longhand in SHORTHANDS:
                sys.exit("SHORTHANDS: %s sets %s, which is a shorthand" % (name, longhand))
        if len(set(longhands)) != len(longhands):
            sys.exit("SHORTHANDS: %s names a longhand twice" % name)
        if form != "All" and len(longhands) > 64:
            sys.exit("SHORTHANDS: %s sets more longhands than the 64 a serialization tracks" % name)
        out[name] = Shorthand(name, form, longhands)
    return out


def preferred_order(names, shorthands):
    """cssom-1/#concept-shorthands-preferred-order"""
    names = sorted(names)
    names = [n for n in names if not n.startswith("-")] + [n for n in names if n.startswith("-")]
    names = ([n for n in names if not n.startswith("-") or n.startswith("-webkit-")]
             + [n for n in names if n.startswith("-") and not n.startswith("-webkit-")])
    return sorted(names, key=lambda n: -len(shorthands[n].longhands))


def takes_number(grammar, values, seen=None):
    """Returns true if the grammar, or a type or property that it names, takes a <number> or an <integer>. `values`
    holds the types by their names, and the properties as <'name'>."""
    seen = seen if seen is not None else set()
    if "<number" in grammar or "<integer" in grammar:
        return True
    for key in re.findall(r"<'[-\w]+'>|<[-\w]+(?=(?:\s*\[[^\]]*\])?>)", grammar):
        key = key if key.endswith(">") else key + ">"
        if key in seen or key not in values:
            continue
        seen.add(key)
        if takes_number(values[key].grammar() or "", values, seen):
            return True
    return False


def initial_value(name, e, values, prose=None):
    """Returns the initial value of a longhand, as it serializes. If webref's initial value is prose, returns `prose`,
    or exits if `prose` is None."""
    if name in INITIAL_VALUES:
        return INITIAL_VALUES[name]
    best = e._best()
    text = best[2].get("initial", "").strip() if best else ""
    # Prose is not a value: a value is keywords, numbers and dimensions.
    if not re.fullmatch(r"[-\w%.]+(?: [-\w%.]+)*",
                        text) or re.search(r"\b(see|as|the|of|depends)\b", text):
        if prose is not None:
            return prose
        sys.exit("%s: webref's initial value %r is prose; give it in INITIAL_VALUES" % (name, text))
    # css-values-4/#lengths: webref writes a zero length as the unitless 0, which serializes in px. A grammar that takes
    # no number reads the 0 as a length. Keywords serialize in lowercase (css-values-4/#keywords).
    if text == "0" and not takes_number(e.grammar() or "", values):
        return "0px"
    return " ".join(word if re.match(r"[-\d.]", word) else word.lower() for word in text.split())


def bare(node):
    """Returns a copy of the node without its multiplier."""
    out = Node(node.type, node.value, node.children)
    out.range = node.range
    return out


def alternatives(node):
    return node.children if node.type == "oneof" and once(node) else [node]


class Marker:
    """Wraps each component of a shorthand's grammar that sets a longhand in a "longhand" node that names the longhand.
    A component is one of these: a reference to one of the shorthand's longhands, a reference to a shorthand whose
    longhands it sets, a component that SHORTHAND_COMPONENTS lists, or a type, function or group whose alternatives
    one longhand's grammar is, or offers among its own. A type that holds components is inlined in place of its
    reference."""

    def __init__(self, shorthand, shorthands, props, types):
        self.shorthand = shorthand
        self.shorthands = shorthands
        self.props = props          # property name -> tree
        self.types = types          # type name -> tree
        self.table = SHORTHAND_COMPONENTS.get(shorthand.name, {})
        self.used = collections.Counter()
        self.longhands = set(shorthand.longhands)

    def text(self, node):
        return " ".join(serialize(bare(node), top=True))

    def sets(self, node, probe):
        """Returns the longhand or shorthand that the component `node` sets, or None."""
        text = self.text(node)
        if text in self.table:
            if probe:
                return text
            index = self.used[text]
            self.used[text] += 1
            if len(self.table[text]) == 1:
                return self.table[text][0]
            if index >= len(self.table[text]):
                sys.exit("SHORTHAND_COMPONENTS: %s writes %s more often than it says"
                         % (self.shorthand.name, text))
            return self.table[text][index]
        if node.type == "property":
            name = node.value
            if name in self.longhands:
                return name
            if name in self.shorthands and set(self.shorthands[name].longhands) <= self.longhands:
                return name
            if not probe:
                sys.exit("%s: its grammar names %s, which it does not set"
                         % (self.shorthand.name, name))
            return None
        if node.type not in ("type", "function", "oneof", "anyof", "allof", "seq"):
            return None
        offered = [l for l in self.shorthand.longhands if l in self.props
                   and self.offers(self.props[l], node)]
        if len(offered) > 1:
            sys.exit("%s: %s is offered by %s; say which it sets in SHORTHAND_COMPONENTS"
                     % (self.shorthand.name, text, ", ".join(offered)))
        if offered and text in self.table:
            sys.exit("SHORTHAND_COMPONENTS: %s %s is %s's, which the grammar says"
                     % (self.shorthand.name, text, offered[0]))
        return offered[0] if offered else None

    def offers(self, tree, node):
        """Returns true if a longhand whose grammar is `tree` accepts the component: if the component is one of the
        longhand's alternatives, or every alternative of the component is."""
        own = [bare(a) for a in alternatives(bare(tree))]
        has = lambda n: any(same_tree(bare(n), a) for a in own)
        if has(node):
            return True
        reference = node.type == "type" and not node.children
        inner = self.types.get(normalize_ref(node.value)) if reference else node
        return inner is not None and inner.type == "oneof" and all(has(a) for a in inner.children)

    def holds(self, node, depth=0):
        """Returns true if a component is somewhere in `node`."""
        if depth == 8:
            return False
        if self.sets(node, True) is not None:
            return True
        if node.type == "type" and normalize_ref(node.value) in self.types:
            return self.holds(self.types[normalize_ref(node.value)], depth + 1)
        return any(self.holds(child, depth + 1) for child in node.children)

    def mark(self, node):
        longhand = self.sets(node, False)
        if longhand is not None:
            # A component taken at most once carries its multiplier on the wrapper, so the wrapper is present only if
            # the component is.
            wrapper = Node("longhand", longhand, [node])
            if node.max == 1 and not node.comma:
                wrapper.min, wrapper.required = node.min, node.required
                node.min, node.required = 1, False
            return wrapper
        if node.type in ("oneof", "anyof", "allof", "seq", "function"):
            node.children = [self.mark(child) for child in node.children]
            return node
        if (node.type == "type" and not node.children and normalize_ref(node.value) in self.types
                and self.holds(node)):
            return self.mark(self.inline(node))
        return node

    def inline(self, node):
        """Returns the grammar of the type that `node` refers to, under the reference's multiplier."""
        if node.range is not None:
            sys.exit("%s: %s has a range, and a type holding components cannot"
                     % (self.shorthand.name, self.text(node)))
        tree = copy.deepcopy(self.types[normalize_ref(node.value)])
        if not once(node):
            if not once(tree):
                tree = Node("seq", children=[tree])
            tree.min, tree.max = node.min, node.max
            tree.comma, tree.required = node.comma, node.required
        return tree

    def finish(self):
        for text, longhands in self.table.items():
            if self.used[text] != len(longhands) and (len(longhands) != 1 or self.used[text] == 0):
                sys.exit("SHORTHAND_COMPONENTS: %s writes %s %d times, not %d"
                         % (self.shorthand.name, text, self.used[text], len(longhands)))


def mark_shorthands(trees, shorthands):
    """Returns the trees, with the components of each marked shorthand wrapped in "longhand" nodes."""
    props = {name: tree for kind, name, _, tree in trees if kind == "property"}
    types = {name: tree for kind, name, _, tree in trees if kind == "value"}
    out = []
    for kind, name, e, tree in trees:
        shorthand = shorthands.get(name) if kind == "property" else None
        if shorthand is not None and shorthand.form == "Mirror":
            # The grammar is a single property, whose value each run of longhands takes.
            runs = len(shorthands[tree.value].longhands) if tree.value in shorthands else 1
            if tree.type != "property" or not once(tree) or len(shorthand.longhands) % runs != 0:
                sys.exit("%s: a Mirror's grammar is one property, as many longhands as each run"
                         % name)
            tree = Node("longhand", tree.value, [tree])
        elif (shorthand is not None and shorthand.form in POSITIONAL_FORMS and tree.max > 1
              and tree.type in ("seq", "anyof", "allof")):
            # Each value of the box is a group of several values, and a component of its own.
            group = copy.deepcopy(tree)
            tree = Node("longhand", shorthand.longhands[0], [group])
            tree.min, tree.max = group.min, group.max
            group.min, group.max = 1, 1
        elif shorthand is not None and shorthand.form in MARKED_FORMS:
            marker = Marker(shorthand, shorthands, props, types)
            tree = marker.mark(copy.deepcopy(tree))
            marker.finish()
        out.append((kind, name, e, tree))
    return out


# The hand-written sources of the engine, which match values starting from the productions that they name.
ENGINE_SOURCES = [os.path.join("lib", "css", "src"), os.path.join("lib", "css", "quickjs", "src"),
                  os.path.join("lib", "dom", "src"), os.path.join("lib", "dom", "quickjs", "src")]


def engine_productions(values):
    """Returns the value definitions that the hand-written engine names as CssValueDef_ identifiers: the productions
    that it matches from, such as a selector list or a media query."""
    idents = {value_ident(name, e.kind): name for name, e in values.items()}
    found = set()
    for directory in ENGINE_SOURCES:
        path = os.path.join(gen_common.ROOT, directory)
        for fn in sorted(os.listdir(path)):
            if not fn.endswith((".cpp", ".h")):
                continue
            with open(os.path.join(path, fn), encoding="utf-8") as f:
                text = f.read()
            if text.startswith("// Generated by"):
                continue
            found |= {idents[ident] for ident in re.findall(r"CssValueDef_\w+", text)
                      if ident in idents}
    return found


def prune_unreachable(trees, values):
    """Drops the value definitions that nothing reaches, from the table and from the trees. The roots are every grammar
    that does not belong to a value definition, such as those of properties, descriptors, preludes and pseudo-class
    arguments. The functions that every property accepts (MATH_FUNCTIONS, WHOLE_VALUE_FUNCTIONS,
    SUBSTITUTION_FUNCTIONS) and the productions that the engine names are also roots."""
    grammars = {name: tree for kind, name, _, tree in trees if kind == "value"}
    queue = [normalize_ref(ref) for kind, _, _, tree in trees if kind != "value"
             for ref in type_refs(tree)]
    queue += MATH_FUNCTIONS + WHOLE_VALUE_FUNCTIONS + list(SUBSTITUTION_FUNCTIONS.values())
    queue += sorted(engine_productions(values))
    reached = set()
    while queue:
        name = queue.pop()
        if name in reached:
            continue
        reached.add(name)
        if name in grammars:
            queue += [normalize_ref(ref) for ref in type_refs(grammars[name])]
    for name in [name for name in values if name not in reached]:
        del values[name]
    return [entry for entry in trees if entry[0] != "value" or entry[1] in reached]


def resolve_aliases(trees):
    """Rewrites each reference to a production that PROSE_GRAMMARS writes as just another production, making it a
    reference to that other production. The spec names the production for its use, and its values are those of the
    other production, with no node of their own."""
    aliases = {name: text for name, text in PROSE_GRAMMARS.items()
               if re.fullmatch(r"<[-\w()]+>", text)}

    def target(name):
        while name in aliases:
            name = aliases[name]
        return name

    for _, _, _, tree in trees:
        for node in walk(tree):
            if node.type == "type" and not node.children and node.value in aliases:
                node.value = target(node.value)


def instance_name(production, parameter):
    return "%s[ %s ]>" % (production[:-1], " ".join(serialize(parameter, top=True)))


def instantiate_generics(trees, values):
    """Rewrites each reference to a production that GENERIC_PRODUCTIONS names with a parameter, making it a reference
    to the production that the parameter creates. Returns the trees without the generic definitions and with those
    productions. An instance of a production in the family names the family's instances for the same parameter in
    place of the family's members, and the parameter in place of the name that the definitions give it."""
    families = {}
    for production, (param_name, members) in GENERIC_PRODUCTIONS.items():
        for member in [production] + members:
            families[member] = (production, param_name, {production} | set(members))
    generics = {name: tree for kind, name, _, tree in trees if kind == "value" and name in families}
    kept = [entry for entry in trees if not (entry[0] == "value" and entry[1] in families)]
    made = []

    def instance(member, parameter):
        _, param_name, family = families[member]
        name = instance_name(member, parameter)
        if name in values:
            return name
        tree = copy.deepcopy(generics[member])
        entry = values[name] = Entry(name, "type")
        substitute(tree, parameter, param_name, family)
        entry.add(values[member].source_spec(),
                  {"name": name, "type": "type", "value": " ".join(serialize(tree, top=True))})
        made.append(("value", name, entry, tree))
        return name

    def substitute(tree, parameter, param_name, family):
        for node in list(walk(tree)):
            if node.type != "type":
                continue
            if node.value == param_name and not node.children:
                if not once(node):
                    sys.exit("%s: a parameter with a multiplier" % param_name)
                replacement = copy.deepcopy(parameter)
                for slot in Node.__slots__:
                    setattr(node, slot, getattr(replacement, slot))
            elif node.value in family and not node.children:
                node.value = instance(node.value, parameter)
            elif node.value in family and len(node.children) == 1:
                inner = node.children[0]
                if inner.type != "type" or inner.value != param_name or inner.children:
                    sys.exit("%s: an instance inside its own family" % node.value)
                node.value = instance(node.value, parameter)
                node.children = []

    for _, _, _, tree in kept:
        for node in list(walk(tree)):
            if (node.type == "type" and node.value in GENERIC_PRODUCTIONS
                    and len(node.children) == 1):
                node.value = instance(node.value, node.children[0])
                node.children = []
    return kept + made


def compile_grammars(props, values, descs, pseudos, at_rules, shorthands, extra_keywords=()):
    g = Grammars()
    g.blocks.append(("no grammar", 0))
    g.nodes.append(("CssGrammarType_Sequence", "0", "1", "0"))
    g.node_firsts.append(g.first_index(Node("seq")))
    g.node_shapes.append("0")

    tables = (("property", props), ("value", values), ("descriptor", descs))
    trees = []
    for kind, table in tables:
        for name, e in table.items():
            text = e.grammar()
            if text is None:
                continue
            try:
                tree = parse_grammar(text)
            except SyntaxError as err:
                sys.exit("%s: %s\n  %s" % (name, err, text))
            if not same_tree(tree, parse_grammar(" ".join(serialize(tree, top=True)))):
                sys.exit("%s: does not survive a serialize/parse round trip\n  %s" % (name, text))
            trees.append((kind, name, e, zero_as_number(factor(tree))))
    for pseudo in pseudos:
        if pseudo.argument is not None:
            trees.append((pseudo.kind, pseudo.key(), pseudo.entry,
                          factor(parse_grammar(pseudo.argument))))
    for rule in at_rules.rules:
        for form, prelude in (("block", rule.block_prelude), ("statement", rule.statement_prelude)):
            if prelude is not None:
                trees.append(("at-rule", (rule.name, form), rule.entry, factor(prelude)))
    for rule in at_rules.rules:
        if rule.name in AT_RULE_NAMED_VALUES:
            trees.append(("named-value", rule.name, rule.entry,
                          factor(parse_grammar(AT_RULE_NAMED_VALUES[rule.name]))))
    g.qualified = {}
    for name, production in AT_RULE_QUALIFIED_RULES.items():
        g.qualified[name] = QualifiedRule(production, values[production].grammar())
        if g.qualified[name].prelude is not None:
            trees.append(("qualified-rule", name, values[production],
                          factor(g.qualified[name].prelude)))
    resolve_aliases(trees)
    trees = prune_unreachable(instantiate_generics(trees, values), values)
    trees = mark_shorthands(trees, shorthands)

    # Keyword ids are assigned in sorted order so the enum reads alphabetically.
    names = sorted({n.value for _, _, _, t in trees for n in walk(t)
                    if n.type in ("keyword", "atkeyword", "function")}
                   | {name[:-2] for name in TREE_COUNTING_FUNCTIONS}
                   | set(EXTRA_KEYWORDS) | set(extra_keywords) | at_rules.keywords)
    g.keywords = {name: i for i, name in enumerate(names)}

    g.value_terminals = reachable_terminals(values, trees)
    for kind, name, e, tree in trees:
        if kind == "value":
            g.trees[("type", name)] = tree
        elif kind == "property":
            g.trees[("property", name)] = tree
    for name, e in props.items():
        if e.alias_of() is not None and ("property", e.alias_of()) in g.trees:
            g.trees[("property", name)] = g.trees[("property", e.alias_of())]
    g.prose = {name for name, e in values.items() if e.grammar() is None}
    check_canonical_order(props)
    for name in WRITTEN_ORDER:
        if name.startswith("<"):
            tree = g.trees.get(("type", name))
            groups = [tree] if tree is not None and tree.type in ("anyof", "allof") else []
        else:
            tree = g.trees.get(("property", name))
            groups = [node for node in walk(tree) if node.type in ("anyof", "allof")] if tree else []
        if len(groups) != 1:
            sys.exit("WRITTEN_ORDER: %s holds no single || or &&" % name)
        g.written.add(id(groups[0]))
    resolve_omissions(g)
    for name in FRACTIONS:
        tree = g.trees.get(("type", name) if name.startswith("<") else ("property", name))
        found = [node for node in walk(tree) if node.type == "type"
                 and node.value == "<percentage>" and once(node)] if tree else []
        if not found:
            sys.exit("FRACTIONS: %s holds no <percentage>" % name)
        g.fractions |= {id(node) for node in found}
    for name in FOLDED_PRODUCTIONS:
        found = [node for _, _, _, tree in trees for node in walk(tree)
                 if node.type == "type" and normalize_ref(node.value) == name and not node.children]
        if ("type", name) not in g.trees or not found:
            sys.exit("FOLDED_PRODUCTIONS: no grammar refers to %s" % name)
        g.folded |= {id(node) for node in found}
    g.compute_first()
    g.compute_follow(trees)
    g.compute_nodes(trees)
    g.unicode_range_descriptors = {
        name for kind, name, _, tree in trees
        if kind == "descriptor" and reaches_type(trees, tree, "<unicode-range-token>")}

    seen = {}
    for name in names:
        ident = keyword_ident(name)
        if ident in seen:
            sys.exit("keyword collision: %s <- %r, %r" % (ident, seen[ident], name))
        seen[ident] = name

    for kind, name, e, tree in trees:
        text = e.grammar()
        if kind == "descriptor":
            label = "%s %s" % (name[0], e.name)
        elif kind == "at-rule":
            label = "%s %s prelude" % name
            text = " ".join(serialize(tree, top=True))
        elif kind == "qualified-rule":
            label = "%s qualified rule prelude" % name
            text = " ".join(serialize(tree, top=True))
        elif kind == "named-value":
            label = "%s declaration value" % name
            text = AT_RULE_NAMED_VALUES[name]
        else:
            label = e.name
        source = e.source_spec()
        if kind == "value" and name in PROSE_GRAMMARS:
            source += " (from prose)"
        elif e.amendment is not None:
            source += " (amended)"
        g.roots[(kind, name)] = g.add("%s = %s  [%s]" % (label, text, source), tree)

    # A prose production that a grammar refers to gets a single-node root, so walking a <type>
    # never has to check whether the reference is a terminal.
    for name, e in find_terminals(values, g):
        g.blocks.append(("%s = prose" % name, len(g.nodes)))
        g.nodes.append(("CssGrammarType_Terminal", "0", "1", terminal_ident(name, e.kind)))
        g.node_firsts.append(g.first_index(Node("type", name)))
        g.node_shapes.append(g.shape(Node("type", name)))
        g.roots[("value", name)] = len(g.nodes) - 1

    for kind, table in tables:
        for name, e in table.items():
            if e.grammar() is not None and not g.roots.get((kind, name)):
                sys.exit("%s has a grammar but no root node" % name)

    for index, row in enumerate(g.nodes):
        if int(row[1]) > 0x3f:
            sys.exit("%d modifiers: more than the 6 bits a node has" % len(g.modifiers))
        if int(row[2]) > 0xff:
            sys.exit("node %d spans %s: more than the 8 bits a node has" % (index, row[2]))

    if len(g.nodes) > 0xffff:
        sys.exit("%d grammar nodes: CssGrammarIndex needs to be wider than uint16_t"
                 % len(g.nodes))
    return g


def check_canonical_order(props):
    """Exits on a property whose canonical order is neither its grammar's order nor the order in
    CANONICAL_ORDER_PROSE."""
    for name, e in props.items():
        order = e.canonical_order()
        if name in CANONICAL_ORDER_PROSE:
            if order != CANONICAL_ORDER_PROSE[name].lower():
                sys.exit("CANONICAL_ORDER_PROSE: webref gives %s the canonical order %r"
                         % (name, order))
        elif order not in (None, "per grammar", "n/a"):
            sys.exit("%s: canonical order %r is not the grammar's; add it to CANONICAL_ORDER_PROSE"
                     % (name, order))


def implied_values(text, keywords, numeric):
    """Returns the values that OMISSIONS gives a component as implied, as (alternatives, [(type, keyword, number)]).
    `keywords` are the keywords that the component matches, and `numeric` is true if it matches a number."""
    alternatives = "|" in text
    values = []
    for word in text.replace("|", " ").split():
        m = re.fullmatch(r"(-?[\d.]+)(%|[a-z]+)?", word)
        if m is None:
            if word not in keywords:
                sys.exit("OMISSIONS: %s is no keyword of the component that implies it" % word)
            values.append(("PrCssValueKind_Keyword", keyword_ident(word), 0))
            continue
        if not numeric:
            sys.exit("OMISSIONS: %s is a number the component does not match" % word)
        number, unit = float(m.group(1)), m.group(2)
        if unit and unit != "%" and number != 0:
            sys.exit("OMISSIONS: %s is a dimension other than a zero" % word)
        kind = "Percentage" if unit == "%" else "Dimension" if unit else "Number"
        values.append(("PrCssValueKind_" + kind, "PrCssKeyword_Unknown", number))
    return alternatives, values


def resolve_omissions(g):
    """Finds the node that each OMISSIONS entry names, and checks the implied value against that node."""
    def parents(node, parent=None):
        yield node, parent
        for child in node.children:
            yield from parents(child, node)

    def reach(node, seen):
        """Returns the keywords that a node matches, and whether it matches a number."""
        keywords, numeric = set(), False
        for n in walk(node):
            if n.type == "keyword":
                keywords.add(n.value)
            elif n.type in ("type", "property"):
                name = normalize_ref(n.value) if n.type == "type" else n.value
                numeric = numeric or name in NUMERIC_TERMINALS or name == "<zero>"
                tree = g.trees.get((n.type, name))
                if tree is not None and (n.type, name) not in seen:
                    seen.add((n.type, name))
                    more, number = reach(tree, seen)
                    keywords |= more
                    numeric = numeric or number
        return keywords, numeric

    for name, entries in OMISSIONS.items():
        kind = "type" if name.startswith("<") or name.endswith("()") else "property"
        tree = g.trees.get((kind, name))
        if tree is None:
            sys.exit("OMISSIONS: %s has no grammar" % name)
        for text, rule in entries:
            wanted = zero_as_number(factor(parse_grammar(text)))
            child = lambda node, parent: (parent is not None and parent.type == "anyof"
                                          and (node.min, node.max) == (1, 1)
                                          and not node.comma and not node.required)
            whole = lambda node, parent: (node.min, node.max) == (0, 1) or child(node, parent)
            found = [(node, parent) for node, parent in parents(tree) if same_tree(node, wanted)
                     and (whole(node, parent) if isinstance(rule, str) else node.max > 1)]
            if len(found) != 1:
                sys.exit("OMISSIONS: %s holds %s where a value may leave it out %d times"
                         % (name, text, len(found)))
            node, parent = found[0]
            keywords, numeric = reach(node, set())
            if isinstance(rule, str):
                if child(node, parent):
                    g.omits_child.add(id(parent))
                else:
                    g.omissible.add(id(node))
                g.omissions[id(node)] = (implied_values(rule, keywords, numeric), [])
                continue
            # Each repetition is one value: a keyword, a type, or a choice of them.
            single = lambda c: ((c.min, c.max) == (1, 1) and not c.comma and not c.required
                                and c.type in ("keyword", "type") and not c.children)
            if (node.max != len(rule) + 1 or node.max > OMISSION_MAX_REPEATS or node.min > 1 or node.comma
                    or not (node.type in ("keyword", "type") and not node.children
                            or node.type == "oneof" and all(single(c) for c in node.children))):
                sys.exit("OMISSIONS: %s in %s is no single value repeated up to %d times"
                         % (text, name, len(rule) + 1))
            repetitions = []
            for index, entry in enumerate(rule, 1):
                if isinstance(entry, int):
                    if entry >= index:
                        sys.exit("OMISSIONS: repetition %d of %s copies one after it"
                                 % (index, text))
                    repetitions.append((entry, (False, [])))
                else:
                    repetitions.append((None, implied_values(str(entry), keywords, numeric)))
            g.omissible.add(id(node))
            g.omissions[id(node)] = ((False, []), repetitions)


def terminal_ident(name, kind):
    if kind == "function":
        return "CssTerminal_" + camel(name[:-2]) + "Fn"
    return "CssTerminal_" + camel(name[1:-1])


def find_terminals(values, g):
    """Returns the prose productions that a grammar actually references: the parsers that css_terminal.cpp must supply.
    The remaining prose definitions are tokenizer-level, or dead CSS2 leftovers that nothing refers to."""
    referenced = set()
    for name in g.referenced_types:
        inner = name[1:-1]
        referenced.add(inner if inner.endswith("()") else name)
    terminals = [(name, e) for name, e in values.items()
                 if e.grammar() is None and name in referenced]
    return sorted(terminals, key=lambda pair: (pair[0] not in g.value_terminals, pair[0]))


def emit_terminal_header(path, terminals, value_terminals):
    members = [(terminal_ident(name, e.kind), name) for name, e in terminals]
    first_structural = next((index for index, (name, _) in enumerate(terminals)
                             if name not in value_terminals), None)
    head = [BANNER, "\n#pragma once\n", '\n#include "pr/base.h"\n']
    body = ["\n// Productions the specs write in prose instead of value definition syntax.\n",
            "enum {\n"]
    body += enum_members(members, first_structural, "CssTerminal_StructuralFirst",
                         ["Productions that no property value reaches, which occupy "
                          "[CssTerminal_StructuralFirst,", "CssTerminal_Count)."])
    body.append("\n    CssTerminal_Count,\n"
                "    CssTerminal_None = CssTerminal_Count,\n};\n")
    body.append("typedef uint8_t CssTerminal;\n")
    body.append('\n// The name of each production as the specs write it, as in "<length>".\n'
                "extern const PrStringView kCssTerminalName[CssTerminal_Count];\n")
    write(path, namespaced(head, body))


def emit_terminal_source(path, terminals):
    head = [BANNER, '\n#include "css/src/css_terminal.h"\n']
    body = [table("extern const PrStringView kCssTerminalName[CssTerminal_Count]",
                  [view(n) for n, _ in terminals])]
    write(path, namespaced(head, body))


def emit_keyword_header(path, g):
    head = [BANNER, "\n#pragma once\n", '\n#include "pr/css_keywords.h"\n']
    body = ["\n// The length of the longest keyword.\n"
            "constexpr uint32_t kCssKeywordMaxLength = %d;\n"
            % max(len(name) for name in g.keywords)]
    body.append("\n// The keyword's name in lowercase.\n"
                "PrStringView cssKeywordName(PrCssKeyword keyword);\n")
    body.append("\n// The keyword that `text` names, or PrCssKeyword_Unknown. Keywords are ASCII\n"
                "// case-insensitive (css-values-4/#keywords).\nPrCssKeyword "
                "cssKeywordFromText(PrStringView text);\n")
    body.append("\n// The <font-format> keyword that a format() string acts as, and whether "
                "it adds\n// tech(variations) (css-fonts-4/#font-face-src-parsing). False for a "
                "string that acts as none.\nbool cssFontFormatFromString(PrStringView text, "
                "PrCssKeyword* format, bool* variations);\n")
    write(path, namespaced(head, body))


def emit_keyword_source(path, g):
    # The sentinel shares the enum with the names taken from the keywords themselves.
    if "unknown" in g.keywords:
        sys.exit("PrCssKeyword_Unknown collides with the keyword `unknown`")

    head = [BANNER,
            '\n#include "css/src/css_keyword.h"\n\n#include "pr/bitset.h"\n#include '
            '"pr/name_table.h"\n']
    names = name_table.NameTable("kCssKeyword", sorted(g.keywords, key=g.keywords.get), lower=True)
    formats = name_table.NameTable("kCssFontFormatString", [row[0] for row in FONT_FORMAT_STRINGS],
                                   lower=False)
    body = ["namespace {\n", names.tables(), formats.tables(),
            table("constexpr PrCssKeyword kCssFontFormatStringKeyword[]",
                  [keyword_ident(row[1]) for row in FONT_FORMAT_STRINGS]),
            bitset("constexpr uint64_t kCssFontFormatStringVariations[bitsetWords(%d)]"
                   % len(FONT_FORMAT_STRINGS), [bool(row[2]) for row in FONT_FORMAT_STRINGS],
                   names=[row[0] for row in FONT_FORMAT_STRINGS]),
            "\n} // namespace\n"]

    body.append("\nPrStringView cssKeywordName(PrCssKeyword keyword) {\n    return %s;\n}\n"
                % names.name("keyword"))
    body.append("\nPrCssKeyword cssKeywordFromText(PrStringView text) {\n"
                + names.lookup("text", "keyword")
                + "    return %s\n        ? PrCssKeyword(keyword) : PrCssKeyword_Unknown;\n}\n"
                % names.confirm("text", "cssKeywordName(PrCssKeyword(keyword))"))
    body.append("\nbool cssFontFormatFromString(PrStringView text, PrCssKeyword* format, bool* "
                "variations) {\n" + formats.lookup("text", "index")
                + "    if (!%s)\n        return false;\n"
                % formats.confirm("text", formats.name("index"))
                + "    *format = kCssFontFormatStringKeyword[index];\n"
                  "    *variations = bitsetTest(kCssFontFormatStringVariations, index);\n"
                  "    return true;\n}\n")
    write(path, namespaced(head, body))


def emit_pseudo_header(path, pseudos):
    classes = [p for p in pseudos if p.kind == "pseudo-class"]
    elements = [p for p in pseudos if p.kind == "pseudo-element"]
    head = [BANNER,
            "\n// Pseudo-classes and pseudo-elements\n//\n// selectors-4 - "
            "https://drafts.csswg.org/selectors-4/\n", "\n#pragma once\n",
            '\n#include "pr/bitset.h"\n#include "css/src/css_grammar.h"\n']
    body = ["\n// The pseudo-classes an element selector can name (selectors-4/#pseudo-classes). A "
            "name\n// written as a function is a pseudo-class of its own.\nenum {\n"]
    body += enum_members([(p.ident(), ":%s%s" % (p.name, "()" if p.function else ""))
                          for p in classes])
    body.append("\n    CssPseudoClass_Count,\n"
                "    CssPseudoClass_None = CssPseudoClass_Count,\n\n"
                "    // The first of the engine's private pseudo-classes. Only user agent sheets may name them.\n"
                "    CssPseudoClass_FirstPrivate = %s,\n};\n"
                "typedef uint8_t CssPseudoClass;\n"
                % Pseudo("pseudo-class", PRIVATE_PSEUDO_CLASSES[0], False, None, None).ident())
    body.append("\n// The pseudo-elements (selectors-4/#pseudo-elements), or none for a selector "
                "that selects\n// an element.\nenum {\n")
    body += enum_members([("CssPseudoElement_None", "")]
                         + [(p.ident(), "::%s%s" % (p.name, "()" if p.function else ""))
                            for p in elements]
                         + [("CssPseudoElement_UnknownWebkit",
                             "Any other name that starts with ::-webkit- "
                             "(selectors-4/#unknown--webkit--pseudo-elements).")])
    body.append("\n    CssPseudoElement_Count,\n};\n"
                "typedef uint8_t CssPseudoElement;\n")
    for kind, prefix, none, example in (("pseudo-class", "CssPseudoClass", "CssPseudoClass_None", "nth-child"),
                                        ("pseudo-element", "CssPseudoElement", "CssPseudoElement_None", "part")):
        body.append("\n// The name of each %s, without its colons or parentheses, as in \"%s\".\n"
                    "extern const PrStringView k%sName[%s_Count];\n" % (kind, example, prefix, prefix))
        body.append("\n// Whether each %s is written as a function.\n"
                    "extern const uint64_t k%sFunction[bitsetWords(%s_Count)];\n"
                    % (kind, prefix, prefix))
        body.append("\n// The grammar of a functional %s's argument, or CssGrammarRoot_None if "
                    "the\n// spec gives it in prose.\nextern const CssGrammarIndex "
                    "k%sGrammarRoot[%s_Count];\n" % (kind, prefix, prefix))
        body.append("\n// The %s named `name`, ASCII case-insensitively, written as a function or "
                    "not.\n// %s if there is none.\n%s css%s(PrStringView name, bool "
                    "function);\n" % (kind, none, prefix, prefix[3:]))
    body.append("\n// The pseudo-classes that may follow each pseudo-element in its compound, one "
                "bit each\n// (selectors-4/#pseudo-element-states). Every pseudo-class may follow "
                "CssPseudoElement_None.\nextern const uint64_t "
                "kCssPseudoElementClasses[CssPseudoElement_Count][2];\n\n"
                "// The pseudo-elements that may follow each pseudo-element, one bit each\n"
                "// (selectors-4/#sub-pseudo-elements). Every pseudo-element may follow CssPseudoElement_None.\n"
                "extern const uint64_t kCssPseudoElementFollowers[CssPseudoElement_Count];\n\n"
                "// Whether `pseudo` may follow `element` in a compound.\n"
                "inline bool cssPseudoClassFollows(CssPseudoElement element, CssPseudoClass "
                "pseudo) {\n    return ((kCssPseudoElementClasses[element][pseudo >> 6] >> (pseudo "
                "& 63)) & 1) != 0;\n}\n"
                "inline bool cssPseudoElementFollows(CssPseudoElement "
                "element, CssPseudoElement pseudo) {\n    return "
                "((kCssPseudoElementFollowers[element] >> pseudo) & 1) != 0;\n}\n")
    write(path, namespaced(head, body))


def pseudo_selector(p):
    """Returns the name that webref gives a pseudo: `:hover`, `::part()`."""
    return (":" if p.kind == "pseudo-class" else "::") + p.name + ("()" if p.function else "")


def follower_tables(pseudos):
    """Returns the rows of kCssPseudoElementClasses and kCssPseudoElementFollowers, from none through an unknown
    -webkit- name. Each row holds, as bits, the pseudo-classes and pseudo-elements that may follow a pseudo-element."""
    classes = [pseudo_selector(p) for p in pseudos if p.kind == "pseudo-class"]
    elements = [pseudo_selector(p) for p in pseudos if p.kind == "pseudo-element"]
    rows = [None] + elements + ["-webkit-"]
    if len(classes) > 128 or len(rows) > 64:
        sys.exit("%d pseudo-classes and %d pseudo-elements: more than the follower tables hold"
                 % (len(classes), len(rows)))
    named = (set(LOGICAL_COMBINATION) | set(USER_ACTION) | set(NO_USER_ACTION) | set(ELEMENT_BACKED)
             | set(NOT_AFTER_ELEMENT_BACKED) | set(TREE_ABIDING)
             | set(PSEUDO_ELEMENT_CLASSES) | set(SUB_PSEUDO_ELEMENTS)
             | {c for names in PSEUDO_ELEMENT_CLASSES.values() for c in names}
             | {e for names in SUB_PSEUDO_ELEMENTS.values() for e in names})
    unknown = named - set(classes) - set(elements)
    if unknown:
        sys.exit("the follower tables name pseudos webref does not: %s"
                 % ", ".join(sorted(unknown)))

    class_rows = []
    element_rows = []
    for row in rows:
        if row is None:
            allowed_classes = classes
            allowed_elements = rows[1:]
        elif row in ELEMENT_BACKED:
            allowed_classes = [c for c in classes if c not in NOT_AFTER_ELEMENT_BACKED]
            allowed_elements = rows[1:]
        else:
            user_action = [] if row in NO_USER_ACTION else USER_ACTION
            allowed_classes = LOGICAL_COMBINATION + user_action + PSEUDO_ELEMENT_CLASSES.get(row,
                                                                                             [])
            allowed_elements = SUB_PSEUDO_ELEMENTS.get(row, [])
        bits = sum(1 << classes.index(c) for c in allowed_classes)
        class_rows.append("{0x%016xull, 0x%016xull}" % (bits & (2**64 - 1), bits >> 64))
        element_rows.append("0x%016xull" % sum(1 << rows.index(e) for e in allowed_elements))
    comments = ["CssPseudoElement_None"] + elements + ["CssPseudoElement_UnknownWebkit"]
    return (table("extern const uint64_t kCssPseudoElementClasses[CssPseudoElement_Count][2]",
                  class_rows, comments=comments),
            table("extern const uint64_t kCssPseudoElementFollowers[CssPseudoElement_Count]",
                  element_rows, comments=comments))


def emit_pseudo_source(path, pseudos, g):
    head = [BANNER, '\n#include "css/src/css_pseudo.h"\n\n#include "pr/name_table.h"\n']
    body = []
    for kind, prefix, none, offset in (("pseudo-class", "CssPseudoClass", "CssPseudoClass_None", 0),
                                       ("pseudo-element", "CssPseudoElement",
                                        "CssPseudoElement_None", 1)):
        rows = [p for p in pseudos if p.kind == kind]
        # A pseudo-element's table has a row for none before the names and one for an unknown
        # -webkit- name after them.
        padded = [None] * offset + rows + [None] * offset
        comments = [none] * offset + [p.ident()
                                      for p in rows] + ["CssPseudoElement_UnknownWebkit"] * offset
        # A legacy alias follows the names, and the lookup returns the pseudo that it is an alias of.
        aliases = PSEUDO_CLASS_ALIASES if kind == "pseudo-class" else {}
        targets = []
        for alias, target in aliases.items():
            found = [i for i, p in enumerate(rows) if p.name == target and not p.function]
            if not found:
                sys.exit("PSEUDO_CLASS_ALIASES names :%s, which webref does not list" % target)
            targets.append(rows[found[0]].ident())
        names = name_table.NameTable("k%sLookup" % prefix, [p.name for p in rows] + list(aliases),
                                     lower=True, salts=[2 if p.function else 1 for p in rows]
                                     + [1] * len(aliases))
        body += ["\nnamespace {\n", names.tables()]
        if targets:
            body.append(table("constexpr %s k%sAliasTarget[]" % (prefix, prefix), targets,
                              comments=[":" + alias for alias in aliases]))
        body.append("\n} // namespace\n")
        body.append(table("extern const PrStringView k%sName[%s_Count]" % (prefix, prefix),
                          [view(p.name) if p else "{nullptr, 0}" for p in padded]))
        body.append(bitset("extern const uint64_t k%sFunction[bitsetWords(%s_Count)]"
                           % (prefix, prefix), [bool(p and p.function) for p in padded],
                           names=[p.name if p else "" for p in padded]))
        body.append(table("extern const CssGrammarIndex k%sGrammarRoot[%s_Count]"
                          % (prefix, prefix),
                          [g.roots.get((kind, p.key()), 0) if p else 0 for p in padded],
                          comments=comments))
        body.append("\n%s css%s(PrStringView name, bool function) {\n" % (prefix, prefix[3:])
                    + names.lookup("name", "index", "uint64_t(function ? 2 : 1)")
                    + ("    %s pseudo = index < %d ? %s(index) : k%sAliasTarget[index - %d];\n"
                       % (prefix, len(rows), prefix, prefix, len(rows)) if targets else
                       "    %s pseudo = %s(%s);\n"
                       % (prefix, prefix, "index + %d" % offset if offset else "index"))
                    + "    if (bitsetTest(k%sFunction, pseudo) != function || !%s)\n"
                    % (prefix, names.confirm("name", names.name("index")))
                    + "        return %s;\n" % none + "    return pseudo;\n}\n")
    body += follower_tables(pseudos)
    write(path, namespaced(head, body))


def emit_unit_source(path):
    """Writes the tables behind css_unit.h, whose enum and prose stay written by hand."""
    names = [name for name, _, _ in CSS_UNITS]
    head = [BANNER,
            '\n#include "css/src/css_unit.h"\n\n#include "pr/count_of.h"\n#include '
            '"pr/name_table.h"\n']

    units = name_table.NameTable("kCssUnit", [name.lower() for name in names], lower=True)
    body = ["namespace {\n",
            units.tables(), table("constexpr CssUnitCategory kCssUnitCategory[]",
                  ["CssUnitCategory_" + category for _, category, _ in CSS_UNITS],
                  comments=[unit_member(name) for name in names]),
            table("// The enum numbers the units the way the tables do.\nconstexpr PrCssUnit "
                  "kCssUnitOrder[]", [unit_member(n) for n in names], 6),
            table("constexpr PrStringView kCssUnitSpelling[]", [view(n) for n in names], 6),
            table("constexpr double kCssUnitScale[]",
                  ["%.17g" % scale if scale else "0" for _, _, scale in CSS_UNITS],
                  comments=[unit_member(name) for name in names])]
    categories = ["Unknown", "Number",
                  "Percent"] + list(dict.fromkeys(category for _, category, _ in CSS_UNITS))
    canonical = {}
    for name, category, scale in CSS_UNITS:
        if scale == 1 and category not in canonical:
            canonical[category] = unit_member(name)
    body.append(table("constexpr PrCssUnit kCssUnitCanonical[]",
                      [canonical.get(category, "PrCssUnit_Number") for category in categories],
                      comments=["CssUnitCategory_" + category for category in categories]))
    body.append("\nconstexpr bool unitsInOrder() {\n"
                "    for (uint32_t i = 0; i < countOf(kCssUnitOrder); ++i) {\n"
                "        if (kCssUnitOrder[i] != PrCssUnit_Em + i)\n            return false;\n"
                "    }\n    return true;\n}\n")
    body.append("\nstatic_assert(unitsInOrder());\n"
                "static_assert(countOf(kCssUnitCategory) == PrCssUnit_Unknown - PrCssUnit_Em);\n"
                "static_assert(%d <= kCssUnitMaxLength);\n" % max(len(n) for n in names))
    body.append("\n} // namespace\n")

    body.append("\nPrCssUnit cssUnitFromText(PrStringView text) {\n"
                + units.lookup("text", "unit")
                + "    return %s\n        ? PrCssUnit(PrCssUnit_Em + unit) : PrCssUnit_Unknown;\n}\n"
                % units.confirm("text", units.name("unit")))
    body.append("\nPrStringView cssUnitName(PrCssUnit unit) {\n"
                "    return kCssUnitSpelling[unit - PrCssUnit_Em];\n}\n")
    body.append("\nCssUnitCategory cssUnitCategory(PrCssUnit unit) {\n"
                "    if (unit == PrCssUnit_Number)\n        return CssUnitCategory_Number;\n"
                "    if (unit < PrCssUnit_Em || unit >= PrCssUnit_Unknown)\n"
                "        return CssUnitCategory_Unknown;\n    return kCssUnitCategory[unit - "
                "PrCssUnit_Em];\n}\n")
    body.append("\ndouble cssUnitScale(PrCssUnit unit) {\n"
                "    if (unit < PrCssUnit_Em || unit >= PrCssUnit_Unknown)\n        return 0;\n"
                "    return kCssUnitScale[unit - PrCssUnit_Em];\n}\n")
    body.append("\nPrCssUnit cssUnitCanonical(CssUnitCategory category) {\n"
                "    return kCssUnitCanonical[category];\n}\n")
    # The viewport-relative lengths are one run of the enum, from vw to dvmax.
    viewport = [n for n in names if n.endswith(("vw", "vh", "vi", "vb", "vmin", "vmax"))]
    if names[names.index(viewport[0]):names.index(viewport[-1]) + 1] != viewport:
        raise SystemExit("gen_css: the viewport-relative units are not one run of CSS_UNITS")
    body.append("\nbool cssUnitViewportRelative(PrCssUnit unit) {\n"
                "    return unit >= %s && unit <= %s;\n}\n" % (unit_member(viewport[0]), unit_member(viewport[-1])))
    write(path, namespaced(head, body))


# ----------------------------------------------------------------------------------------------------------------------
# Predefined counter styles
#
# css-counter-styles-3/#predefined-counters defines the simple predefined counter styles, and the limited-range Japanese
# and Korean ones, as @counter-style rules in its stylesheet blocks. The generator reads them from the draft's source at
# the commit that dev.sh pins, resolves `extends`, and emits a table. The styles whose symbols the specification gives
# in prose (disclosure-open and disclosure-closed) have none in the table. The styles that it defines as algorithms
# (the longhand CJK styles over the extended range, and ethiopic-numeric) take their characters from the draft's
# tables, and their algorithms from systems of their own.

COUNTER_STYLES_SOURCE = os.path.join(gen_common.ROOT, "third_party", "drafts", "css-counter-styles-3.bs")
COUNTER_STYLES_URL = "https://github.com/w3c/csswg-drafts/blob/main/css-counter-styles-3/Overview.bs"

# The descriptors' initial values (css-counter-styles-3/#counter-style-system and the sections after it).
COUNTER_STYLE_DEFAULTS = {"negative": ["-", ""], "prefix": "", "suffix": ". ", "range": None, "pad": (0, ""),
                          "fallback": "decimal", "first": 1}

COUNTER_SYSTEMS = ["cyclic", "fixed", "symbolic", "alphabetic", "numeric", "additive"]

# The systems of the complex predefined styles, which no rule can name, by the enumerator that each takes.
COMPLEX_SYSTEMS = {"chinese-informal": "ChineseInformal", "chinese-formal": "ChineseFormal",
                   "japanese-informal": "JapaneseInformal", "japanese-formal": "JapaneseFormal",
                   "korean-informal": "KoreanInformal", "korean-formal": "KoreanFormal", "ethiopic": "Ethiopic",
                   "hebrew": "Hebrew"}


def code_points(text):
    """Returns the characters that the U+ code points in `text` name, in order."""
    return [chr(int(c, 16)) for c in re.findall(r"U\+([0-9A-Fa-f]{4,6})", text)]


# The algorithm of each longhand CJK style (css-counter-styles-3/#extended-range-optional), by the system of
# COMPLEX_SYSTEMS that it takes.
CJK_SYSTEMS = {
    "simp-chinese-informal": "chinese-informal", "simp-chinese-formal": "chinese-formal",
    "trad-chinese-informal": "chinese-informal", "trad-chinese-formal": "chinese-formal",
    "japanese-informal": "japanese-informal", "japanese-formal": "japanese-formal",
    "korean-hangul-formal": "korean-formal", "korean-hanja-informal": "korean-informal",
    "korean-hanja-formal": "korean-formal",
}


def cjk_characters(source):
    """Returns the characters of each longhand CJK style from the tables of
    css-counter-styles-3/#extended-range-optional, by name: its symbols, which are the digits 0 to 9, the second, third
    and fourth digit markers and the second, third and fourth group markers, and its negative sign."""
    section = re.search(r"<h4 id='extended-range-optional'>(.*?)<!--", source, re.S).group(1)
    labels = (["Digit %d" % d for d in range(10)]
              + ["%s Digit Marker" % n for n in ("Second", "Third", "Fourth")]
              + ["%s Group Marker" % n for n in ("Second", "Third", "Fourth")])
    out = {}
    for table in re.findall(r'<table class="data">(.*?)</table>', section, re.S):
        head, body = table.split("<tbody>", 1)
        names = re.findall(r"<th>([a-z-]+)", head)
        rows = {}
        for label, cells in re.findall(r'<th scope="row">([^<\n]+?)\s*</th>(.*?)(?=<tr>|$)', body, re.S):
            values = []
            for span, text in re.findall(r'<td(?: colspan="(\d+)")?>([^<]*)', cells):
                values += ["".join(code_points(text))] * int(span or 1)
            # A cell that spans fewer columns than the table has gives the rest its value.
            rows[label.strip()] = values + values[-1:] * (len(names) - len(values))
        for column, name in enumerate(names):
            if any(len(rows.get(label, [])) != len(names) or not rows[label][column] for label in labels):
                sys.exit("gen_css: the CJK table of %s does not give every character" % name)
            out[name] = ([rows[label][column] for label in labels], rows["Negative Sign"][column])
    if sorted(out) != sorted(CJK_SYSTEMS):
        sys.exit("gen_css: the extended CJK tables name %s" % ", ".join(sorted(out)))
    return out


def complex_counter_styles(source, rules):
    """Returns the predefined styles that css-counter-styles-3/#complex-predefined-counters defines by algorithms, and
    the Japanese and Korean styles of `rules`, which take the extended algorithm in place of their limited-range rules
    (css-counter-styles-3/#extended-range-optional) and keep their other descriptors, and hebrew, extended in the same
    way. Every longhand CJK style
    represents each value that a counter holds, which the extended range holds. ethiopic-numeric's symbols are the
    units 1 to 9, the tens 10 to 90, and the separators of the odd and the even groups."""
    characters = cjk_characters(source)
    replaced = []
    for style in rules:
        if style["name"] in CJK_SYSTEMS:
            symbols, _ = characters[style["name"]]
            style = dict(style, system=CJK_SYSTEMS[style["name"]], symbols=symbols, additive=[], range=None)
        elif style["name"] == "hebrew":
            # css-counter-styles-3/#simple-numeric lets hebrew reach past its rule's range, which the range then
            # reflects: the thousands written as a value below 1000 and a geresh, then the rest, up to 999999.
            style = dict(style, system="hebrew", range=[(1, 999999)])
        replaced.append(style)
    # The suffix and fallback that css-counter-styles-3/#limited-chinese gives every Chinese style in prose.
    styles = []
    for name in ["simp-chinese-informal", "simp-chinese-formal", "trad-chinese-informal", "trad-chinese-formal"]:
        symbols, negative = characters[name]
        style = dict(COUNTER_STYLE_DEFAULTS)
        style.update(name=name, system=CJK_SYSTEMS[name], symbols=symbols, additive=[], negative=[negative, ""],
                     suffix="\u3001", range=None, fallback="cjk-decimal")
        styles.append(style)
    # cjk-ideographic is identical to trad-chinese-informal.
    styles.append(dict(styles[2], name="cjk-ideographic"))

    ethiopic = re.search(r"<h3 id='ethiopic-numeric-counter-style'>(.*?)<!--", source, re.S).group(1)
    pairs = re.findall(r"<td>(\d+)\s*<td>\S+\s*<td>(U\+[0-9A-F]+)\s*<td>(\d+)\s*<td>\S+\s*<td>(U\+[0-9A-F]+)",
                       ethiopic)
    tens = {int(value): code_points(point)[0] for value, point, _, _ in pairs}
    units = {int(value): code_points(point)[0] for _, _, value, point in pairs}
    separators = code_points(" ".join(re.findall(r"append \S+ (U\+[0-9A-F]+)", ethiopic)))
    if sorted(units) != list(range(1, 10)) or sorted(tens) != list(range(10, 100, 10)) or len(separators) != 2:
        sys.exit("gen_css: the ethiopic-numeric table does not give every unit, ten and separator")
    style = dict(COUNTER_STYLE_DEFAULTS)
    style.update(name="ethiopic-numeric", system="ethiopic",
                 symbols=[units[u] for u in range(1, 10)] + [tens[t] for t in range(10, 100, 10)] + separators,
                 additive=[], range=[(1, None)], suffix="/ ")
    styles.append(style)
    return replaced + styles


def counter_tokens(text):
    """Returns the tokens of a declaration's value: ("string", text), ("ident", text), ("number", int) and
    (",", None), with CSS escapes decoded (css-syntax-3/#consume-escaped-code-point)."""
    tokens = []
    at = 0

    def escape(at):
        hex_digits = re.match(r"[0-9a-fA-F]{1,6}", text[at:])
        if not hex_digits:
            return text[at], at + 1
        at += len(hex_digits.group(0))
        if at < len(text) and text[at] in " \t\n":
            at += 1
        return chr(int(hex_digits.group(0), 16)), at

    while at < len(text):
        c = text[at]
        if c in " \t\n":
            at += 1
        elif c == ",":
            tokens.append((",", None))
            at += 1
        elif c in "'\"":
            out = []
            at += 1
            while text[at] != c:
                if text[at] == "\\":
                    decoded, at = escape(at + 1)
                    out.append(decoded)
                else:
                    out.append(text[at])
                    at += 1
            tokens.append(("string", "".join(out)))
            at += 1
        elif re.match(r"[-+]?[0-9]", text[at:]):
            number = re.match(r"[-+]?[0-9]+", text[at:]).group(0)
            tokens.append(("number", int(number)))
            at += len(number)
        else:
            out = []
            while at < len(text) and text[at] not in " \t\n,'\"":
                if text[at] == "\\":
                    decoded, at = escape(at + 1)
                    out.append(decoded)
                else:
                    out.append(text[at])
                    at += 1
            tokens.append(("ident", "".join(out)))
    return tokens


def counter_symbol(token):
    """Returns the text of a <symbol>, which the source writes as a string or an identifier."""
    if token[0] not in ("string", "ident"):
        sys.exit("gen_css: %r is not a counter symbol" % (token,))
    return token[1]


def load_counter_styles(path):
    """Returns the predefined counter styles of the draft's stylesheet blocks, in order, each a dict of its
    descriptors with `extends` resolved."""
    if not os.path.exists(path):
        sys.exit("gen_css: %s is missing; run ./dev.sh fetch drafts" % path)
    with open(path, encoding="utf-8") as f:
        source = f.read()
    rules = collections.OrderedDict()
    for block in re.findall(r"<pre class=['\"]?stylesheet['\"]?>(.*?)</pre>", source, re.S):
        block = re.sub(r"</?bdo[^>]*>", "", block)
        block = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
        for name, body in re.findall(r"@counter-style\s+([-\w]+)\s*\{(.*?)\}", block, re.S):
            rule = {}
            for declaration in body.split(";"):
                if ":" not in declaration:
                    continue
                key, value = declaration.split(":", 1)
                rule[key.strip()] = counter_tokens(value.strip())
            rules[name] = rule

    def resolve(name, seen=()):
        rule = rules[name]
        system = rule["system"]
        if system[0] == ("ident", "extends"):
            base = system[1][1]
            if base in seen:
                sys.exit("gen_css: the predefined counter styles extend in a cycle at %s" % name)
            style = dict(resolve(base, seen + (name,)))
        else:
            style = dict(COUNTER_STYLE_DEFAULTS)
            style["system"] = system[0][1]
            if style["system"] not in COUNTER_SYSTEMS:
                sys.exit("gen_css: %s has the unknown system %s" % (name, style["system"]))
            if style["system"] == "fixed" and len(system) > 1:
                style["first"] = system[1][1]
            style["symbols"] = [counter_symbol(t) for t in rule.get("symbols", [])]
            additive = []
            for weight, symbol in zip(*[iter([t for t in rule.get("additive-symbols", []) if t[0] != ","])] * 2):
                additive.append((weight[1], counter_symbol(symbol)))
            style["additive"] = additive
        style["name"] = name
        if "negative" in rule:
            symbols = [counter_symbol(t) for t in rule["negative"]]
            style["negative"] = symbols + [""] * (2 - len(symbols))
        for key in ("prefix", "suffix"):
            if key in rule:
                style[key] = counter_symbol(rule[key][0])
        if "range" in rule:
            bounds = [t for t in rule["range"] if t[0] != ","]
            if bounds[0] == ("ident", "auto"):
                style["range"] = None
            else:
                style["range"] = [(lo[1] if lo[0] == "number" else None, hi[1] if hi[0] == "number" else None)
                                  for lo, hi in zip(bounds[0::2], bounds[1::2])]
        if "pad" in rule:
            style["pad"] = (rule["pad"][0][1], counter_symbol(rule["pad"][1]))
        if "fallback" in rule:
            style["fallback"] = rule["fallback"][0][1]
        return style

    styles = complex_counter_styles(source, [resolve(name) for name in rules])
    names = {style["name"] for style in styles}
    for style in styles:
        if style["fallback"] not in names:
            sys.exit("gen_css: %s falls back to %s, which is not predefined" % (style["name"], style["fallback"]))
    return styles


def emit_counter_style_source(path, styles):
    """Writes the table of predefined counter styles that css_counter_style.h declares."""
    head = [gen_common.banner(__file__, "css-counter-styles-3's predefined counter styles", COUNTER_STYLES_URL),
            '\n#include "css/src/css_counter_style.h"\n\n#include "pr/count_of.h"\n#include '
            '"pr/name_table.h"\n']
    names = [style["name"] for style in styles]
    lookup = name_table.NameTable("kCounterStyle", names, lower=True)
    symbols = []
    additive = []
    ranges = []
    entries = []

    def bound(value, infinite):
        return infinite if value is None else str(value)

    for style in styles:
        symbol_at = len(symbols)
        symbols += style["symbols"]
        additive_at = len(additive)
        additive += style["additive"]
        range_at = len(ranges)
        ranges += style["range"] or []
        system = COMPLEX_SYSTEMS.get(style["system"], style["system"].capitalize())
        fields = [".name = %s" % view(style["name"]), ".system = CssCounterSystem_%s" % system]
        if style["system"] == "fixed":
            fields.append(".first = %d" % style["first"])
        if style["symbols"]:
            fields.append(".symbols = kSymbols + %d" % symbol_at)
            fields.append(".symbolCount = %d" % len(style["symbols"]))
        if style["additive"]:
            fields.append(".additive = kAdditive + %d" % additive_at)
            fields.append(".additiveCount = %d" % len(style["additive"]))
        fields.append(".negative = {%s, %s}" % (view(style["negative"][0]), view(style["negative"][1])))
        fields.append(".prefix = %s" % view(style["prefix"]))
        fields.append(".suffix = %s" % view(style["suffix"]))
        if style["range"]:
            fields.append(".ranges = kRanges + %d" % range_at)
            fields.append(".rangeCount = %d" % len(style["range"]))
        if style["pad"][0]:
            fields.append(".minimumLength = %d" % style["pad"][0])
            fields.append(".padSymbol = %s" % view(style["pad"][1]))
        fields.append(".fallback = %s" % view(style["fallback"]))
        lines = ["    {"]
        for field in fields:
            if len(lines[-1]) + len(field) + 2 > gen_common.LINE_LIMIT - 4:
                lines.append("     ")
            lines[-1] += field + ", " if field != fields[-1] else field
        entries.append("\n".join(line.rstrip() for line in lines) + "},\n")
    def packed(declaration, values):
        lines = ["    "]
        for value in values:
            if len(lines[-1]) + len(value) + 2 > gen_common.LINE_LIMIT:
                lines.append("    ")
            lines[-1] += value + ", "
        return "\n%s = {\n%s\n};\n" % (declaration, "\n".join(line.rstrip() for line in lines))

    body = ["namespace {\n",
            lookup.tables(),
            packed("constexpr PrStringView kSymbols[]", [view(s) for s in symbols]),
            packed("constexpr CssAdditiveTuple kAdditive[]",
                   ["{%d, %s}" % (weight, view(symbol)) for weight, symbol in additive]),
            packed("constexpr CssCounterRange kRanges[]",
                   ["{%s, %s}" % (bound(lo, "INT32_MIN"), bound(hi, "INT32_MAX")) for lo, hi in ranges]),
            "\nconstexpr CssCounterStyle kStyles[] = {\n" + "".join(entries) + "};\n",
            "\n} // namespace\n",
            "\nconst CssCounterStyle* PR_NULLABLE cssCounterStylePredefined(PrStringView name) {\n"
            + lookup.lookup("name", "index")
            + "    if (!%s)\n        return nullptr;\n    return &kStyles[index];\n}\n"
            % lookup.confirm("name", lookup.name("index"))]
    write(path, namespaced(head, body))


def emit_color_header(path):
    head = [BANNER, "\n#pragma once\n",
            '\n#include "pr/color.h"\n\n#include "css/src/css_keyword.h"\n']
    body = ["\n// The sRGB channels of a <named-color> (css-color-4/#named-colors). False for a\n"
            "// keyword that is not one.\nbool cssNamedColor(PrCssKeyword keyword, PrColor8* "
            "out);\n",
            "\n// Whether the keyword is a <system-color> (css-color-4/#css-system-colors) or a "
            "<deprecated-color>\n// (css-color-4/#deprecated-system-colors). Its channels are not "
            "known until the cascade resolves it.\nbool cssIsSystemColor(PrCssKeyword keyword);\n",
            "\n// The <system-color> that a system color computes to: a <deprecated-color>'s, and any "
            "other's own.\nPrCssKeyword cssSystemColorComputed(PrCssKeyword keyword);\n",
            "\n// The component keywords of each color model's channels, in the order they are "
            "written\n// (css-color-5/#relative-colors). PrCssKeyword_Unknown for a model that no "
            "relative color accepts.\nextern const PrCssKeyword "
            "kCssColorComponents[PrColorModel_Count][3];\n"]
    write(path, namespaced(head, body))


def emit_color_source(path, colors):
    head = [BANNER, '\n#include "css/src/css_color.h"\n']
    body = []

    width = max(len(keyword_ident(n)) for n in colors.named) + len("    case :")
    body.append("\nbool cssNamedColor(PrCssKeyword keyword, PrColor8* out) {\n"
                "    uint32_t rgba;\n    switch (keyword) {\n")
    for name in colors.named:
        label = "    case %s:" % keyword_ident(name)
        body.append("%s rgba = 0x%sff; break;\n" % (label.ljust(width), NAMED_COLOR_RGB[name]))
    body.append("    default:\n        return false;\n    }\n\n"
                "    *out = PrColor8{uint8_t(rgba >> 24), uint8_t(rgba >> 16),\n"
                "                    uint8_t(rgba >> 8), uint8_t(rgba)};\n    return true;\n}\n")

    body.append("\nbool cssIsSystemColor(PrCssKeyword keyword) {\n    switch (keyword) {\n")
    for name in sorted(colors.system + colors.deprecated):
        body.append("    case %s:\n" % keyword_ident(name))
    body.append("        return true;\n    default:\n        return false;\n    }\n}\n")

    width = max(len(keyword_ident(n)) for n in colors.deprecated) + len("    case :")
    body.append("\nPrCssKeyword cssSystemColorComputed(PrCssKeyword keyword) {\n    switch (keyword) {\n")
    for name in colors.deprecated:
        label = "    case %s:" % keyword_ident(name)
        body.append("%s return %s;\n" % (label.ljust(width), keyword_ident(DEPRECATED_SYSTEM_COLORS[name])))
    body.append("    default:%s return keyword;\n    }\n}\n" % (" " * (width - len("    default:"))))

    body.append("\nextern const PrCssKeyword kCssColorComponents[PrColorModel_Count][3] = {\n")
    for model in COLOR_MODELS:
        names = COMPONENT_KEYWORDS.get(model)
        cells = [keyword_ident(n) for n in names] if names else ["PrCssKeyword_Unknown"] * 3
        body.append("    {%s},  // %s\n" % (", ".join(cells), model))
    body.append("};\n")
    write(path, namespaced(head, body))


def emit_grammar_source(path, g):
    head = [BANNER,
            '\n#include "css/src/css_at_rule.h"\n'
            '#include "css/src/css_keyword.h"\n'
            '#include "css/src/css_property.h"\n'
            '#include "css/src/css_syntax.h"\n'
            '#include "css/src/css_terminal.h"\n'
            '#include "css/src/css_value_def.h"\n']
    body = ["\nextern const CssGrammarNode kCssGrammarNodes[] = {\n"]
    # The definitions whose grammar each node is the root of. Several share one root if their
    # grammars are the same. A comment names them together on one line, and gives the grammar once
    # on the next line with each source once: "a, b" then "= grammar  [spec, spec]".
    labels = collections.defaultdict(list)
    for label, first in g.blocks:
        labels[first].append(label)

    def comment(group):
        heads = collections.OrderedDict()
        for label in group:
            match = re.match(r"(.*?) = (.*?)(?:  \[(.*)\])?$", label, re.S)
            if match is None:
                heads.setdefault(label, ([], []))
                continue
            name, grammar, source = match.groups()
            names, sources = heads.setdefault(grammar, ([], []))
            names.append(name)
            if source and source not in sources:
                sources.append(source)
        lines = []
        for grammar, (names, sources) in heads.items():
            definition = "= %s" % grammar + ("  [%s]" % ", ".join(sources) if sources else "")
            if not names:
                lines.append(grammar)
            elif len(names) == 1:
                lines.append("%s %s" % (names[0], definition))
            else:
                lines += [", ".join(names), definition]
        return ["    // %s\n" % line for line in lines]

    for i, row in enumerate(g.nodes):
        if i in labels:
            body.append("\n" if i else "")
            body += comment(labels[i])
        body.append("    CSS_GRAMMAR_NODE(%s),\n" % ", ".join(row))
    body.append("};\n")
    body.append("\nextern const uint32_t kCssGrammarNodeCount = %d;\n" % len(g.nodes))

    # The literal characters that grammars write, each a bit of kCssGrammarFirst::literals.
    alphabet = sorted({c for key in g.firsts for c in key[1] if c != Grammars.ANY_TOKEN})
    if len(alphabet) > 64:
        sys.exit("%d literal characters: more than kCssGrammarFirst::literals holds"
                 % len(alphabet))
    bits = {c: 1 << i for i, c in enumerate(alphabet)}

    def first_expression(key):
        kinds, literals, empty = key
        if kinds == (Grammars.ANY_TOKEN,):
            mask = "kCssGrammarFirstAny"
            chars = "~0ull"
        else:
            mask = " | ".join("(1u << %s)" % k for k in kinds) or "0"
            chars = "0x%xull" % sum(bits[c] for c in literals)
        if empty:
            mask = "kCssGrammarFirstEmpty" if mask == "0" else mask + " | kCssGrammarFirstEmpty"
        return "{ %s, %s }" % (mask, chars)
    body.append(table("extern const CssGrammarFirst kCssGrammarFirsts[]",
                      [first_expression(key) for key in sorted(g.firsts, key=g.firsts.get)]))
    body.append(table("extern const uint16_t kCssGrammarFirstOf[]", g.node_firsts, 16))
    body.append(table("extern const CssGrammarShape kCssGrammarShapes[]", g.node_shapes, 8))
    body.append(table("extern const uint64_t kCssGrammarLiteralBit[128]",
                      ["0x%xull" % bits.get(chr(c), 0) for c in range(128)], 8))

    body.append(table("extern const CssGrammarModifier kCssGrammarModifiers[]",
                      ["{ %s, %s, %s, %d }" % key
                       for key in sorted(g.modifiers, key=g.modifiers.get)]))
    body.append("\nextern const uint32_t kCssGrammarModifierCount = %d;\n" % len(g.modifiers))

    body.append(table("extern const CssGrammarNumber kCssGrammarNumbers[]",
                      ["{ %g, %s }" % (value, unit_member(unit)) for value, unit in g.numbers]
                      or ["{ 0, PrCssUnit_Number }"]))

    body.append("\nextern const CssGrammarRange kCssGrammarRanges[] = {\n")
    for bounds in g.ranges:
        if bounds is None:
            body.append("    { 0, 0, CssGrammarRangeFlag_MinUnbounded | "
                        "CssGrammarRangeFlag_MaxUnbounded, {} },\n")
            continue
        lo, hi = bounds
        flags = []
        if lo == -INF:
            flags.append("CssGrammarRangeFlag_MinUnbounded")
        if hi == INF:
            flags.append("CssGrammarRangeFlag_MaxUnbounded")
        body.append("    { %g, %g, %s, {} },  // [%s,%s]\n"
                    % (0 if lo == -INF else lo, 0 if hi == INF else hi, " | ".join(flags) if flags
                       else "0", "-∞" if lo == -INF else "%g" % lo,
                       "∞" if hi == INF else "%g" % hi))
    body.append("};\n")
    body.append("\nextern const uint32_t kCssGrammarRangeCount = %d;\n" % len(g.ranges))

    # What each component that OMISSIONS names implies, with the values written end to end.
    values = []
    kinds = {"PrCssValueKind_Keyword": "CssGrammarImpliedKind_Keyword",
             "PrCssValueKind_Number": "CssGrammarImpliedKind_Number",
             "PrCssValueKind_Percentage": "CssGrammarImpliedKind_Percentage",
             "PrCssValueKind_Dimension": "CssGrammarImpliedKind_Zero"}

    def implied(copies, alternatives, run):
        first = len(values) if run else 0
        values.extend(run)
        return "{ %s, %d, %d, %s }" % ("kCssGrammarImpliedOwn" if copies is None else copies, first,
                                        len(run), "true" if alternatives else "false")

    rows = []
    for index, ((alternatives, run), repetitions) in sorted(g.omission_rows):
        cells = [implied(copies, alt, r) for copies, (alt, r) in repetitions]
        cells += [implied(None, False, [])] * (3 - len(cells))
        rows.append("{ %d, %s, { %s } }"
                    % (index, implied(None, alternatives, run), ", ".join(cells)))
    if len(values) > 0xff:
        sys.exit("%d values in kCssGrammarImpliedValues: more than a uint8_t reaches" % len(values))
    body.append(table("extern const CssGrammarOmission kCssGrammarOmissions[]", rows))
    body.append("\nextern const uint32_t kCssGrammarOmissionCount = %d;\n" % len(rows))
    body.append(table("extern const CssGrammarImpliedValue kCssGrammarImpliedValues[]",
                      ["{ %s, %s, %g }" % (kinds[kind], keyword, number) for kind, keyword,
                       number in values]))
    write(path, namespaced(head, body))


def grammar_root_table(g, kind, keys, name, comments):
    return table("// Grammar roots, CssGrammarRoot_None where there is no grammar.\nextern const "
                 "CssGrammarIndex %s[]" % name, [g.roots.get((kind, key), 0) for key in keys],
                 comments=comments)


def add_undefined_types(props, values, descs, pseudos, at_rules):
    """Adds the types that a grammar references but no spec defines. They become prose entries, so a dangling reference
    is a value that never matches instead of a generator failure."""
    referenced = set()
    texts = [e.grammar() for table in (props, values, descs) for e in table.values()]
    texts += [pseudo.argument for pseudo in pseudos]
    texts += [rule.entry.grammar() for rule in at_rules.rules]
    for text in texts:
        if text:
            for m in re.findall(r"<[-\w]+(?:\s*\[[^\]]*\])?>", text):
                referenced.add("<%s>" % re.sub(r"\s*\[[^\]]*\]", "", m)[1:-1].strip())
    parameters = {param_name for param_name, _ in GENERIC_PRODUCTIONS.values()}
    missing = sorted(referenced - set(values) - parameters)
    for name in missing:
        e = Entry(name, "type")
        e.add("undefined", {"name": name, "type": "type"})
        values[name] = e
    return missing


# ----------------------------------------------------------------------------------------------------------------------
# Color names

ColorNames = collections.namedtuple("ColorNames", "named system deprecated")


def color_names(values):
    """Returns the <named-color> and <system-color> keywords. No grammar refers to them by name, so the keyword table
    would not otherwise hold them."""
    # The specs write the system colors in camel case and the named colors in lowercase. Both are
    # ASCII case-insensitive. Lowercase keeps `AccentColor` from mangling to the same identifier as
    # the `accent-color` property keyword.
    named = [n.lower() for n in values["<named-color>"].sub_values()]
    system = [n.lower() for n in values["<system-color>"].sub_values()]
    deprecated = [n.lower() for n in values["<deprecated-color>"].sub_values()]
    if not named or not system:
        sys.exit("webref lists no <named-color> or <system-color> values")
    if sorted(deprecated) != sorted(DEPRECATED_SYSTEM_COLORS) or \
            any(t not in system for t in DEPRECATED_SYSTEM_COLORS.values()):
        sys.exit("DEPRECATED_SYSTEM_COLORS is out of date: webref lists %s" % deprecated)

    missing = [n for n in named if n not in NAMED_COLOR_RGB]
    stale = [n for n in NAMED_COLOR_RGB if n not in named]
    if missing or stale:
        sys.exit("NAMED_COLOR_RGB is out of date: missing %s, stale %s" % (missing, stale))
    return ColorNames(named, system, deprecated)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--webref", default=gen_common.WEBREF)
    ap.add_argument("--out", default=os.path.join(gen_common.ROOT, "lib", "css", "src"))
    args = ap.parse_args()

    props, values, descs, selectors, at_rules, conflicts = load(args.webref)
    pseudos = pseudos_of(selectors, values)
    undefined = add_undefined_types(props, values, descs, pseudos, at_rules)

    # A stable, human-scannable order: real properties first, with legacy aliases in a block at the
    # end, and value types before value functions.
    props = collections.OrderedDict(
        sorted(props.items(), key=lambda kv: (kv[1].alias_of() is not None, kv[0])))
    values = collections.OrderedDict(
        sorted(values.items(), key=lambda kv: (kv[1].kind != "type", kv[0].strip("<>()"))))

    # The descriptors of one at-rule are contiguous, in the order of the at-rule enum.
    descs = collections.OrderedDict(sorted(descs.items()))

    # Identifier collisions would be a silent miscompile; fail loudly instead.
    for table, ident in ((props, lambda n, e: property_ident(n)),
                         (values, lambda n, e: value_ident(n, e.kind)),
                         (descs, lambda n, e: descriptor_ident(n)),
                         ({rule.name: rule for rule in at_rules.rules}, lambda n,
                          e: at_rule_ident(n))):
        seen = {}
        for name, e in table.items():
            i = ident(name, e)
            if i in seen:
                sys.exit("identifier collision: %s <- %s, %s" % (i, seen[i], name))
            seen[i] = name

    colors = color_names(values)
    components = sorted({n for names in COMPONENT_KEYWORDS.values() for n in names})
    shorthands = shorthands_of(props)
    grammars = compile_grammars(props, values, descs, pseudos, at_rules, shorthands,
                                extra_keywords=colors.named + colors.system + colors.deprecated + components)

    # The instances of generic productions join the value definitions, in the same order.
    values = collections.OrderedDict(
        sorted(values.items(), key=lambda kv: (kv[1].kind != "type", kv[0].strip("<>()"))))
    terminals = find_terminals(values, grammars)
    public = os.path.join(gen_common.ROOT, "lib", "css", "include", "pr")
    emit_public_properties(os.path.join(public, "css_properties.h"), props)
    emit_public_descriptors(os.path.join(public, "css_descriptors.h"), descs)
    emit_public_keywords(os.path.join(public, "css_keywords.h"), grammars)
    emit_property_header(os.path.join(args.out, "css_property.h"), props)
    emit_property_source(os.path.join(args.out, "css_property.cpp"), props, grammars, shorthands, values)
    groups = style_groups(props, shorthands)
    emit_style_group_header(os.path.join(args.out, "css_style_group.h"), groups)
    emit_style_group_source(os.path.join(args.out, "css_style_group.cpp"), props, groups)
    emit_terminal_header(os.path.join(args.out, "css_terminal.h"), terminals,
                         grammars.value_terminals)
    emit_terminal_source(os.path.join(args.out, "css_terminal.cpp"), terminals)
    emit_at_rule_header(os.path.join(args.out, "css_at_rule.h"), at_rules, descs)
    emit_at_rule_source(os.path.join(args.out, "css_at_rule.cpp"), at_rules, descs, grammars,
                        feature_productions(args.webref, descs))
    emit_value_def_header(os.path.join(args.out, "css_value_def.h"), values)
    emit_value_def_source(os.path.join(args.out, "css_value_def.cpp"), values, grammars)
    emit_keyword_header(os.path.join(args.out, "css_keyword.h"), grammars)
    emit_keyword_source(os.path.join(args.out, "css_keyword.cpp"), grammars)
    emit_unit_source(os.path.join(args.out, "css_unit.cpp"))
    emit_pseudo_header(os.path.join(args.out, "css_pseudo.h"), pseudos)
    emit_pseudo_source(os.path.join(args.out, "css_pseudo.cpp"), pseudos, grammars)
    emit_color_header(os.path.join(args.out, "css_color.h"))
    emit_color_source(os.path.join(args.out, "css_color.cpp"), colors)
    emit_grammar_source(os.path.join(args.out, "css_grammar.cpp"), grammars)
    emit_counter_style_source(os.path.join(args.out, "css_counter_style.cpp"),
                              load_counter_styles(COUNTER_STYLES_SOURCE))

    print("\n%d properties (%d legacy aliases, %d without a grammar)"
          % (len(props),
             sum(1 for e in props.values() if e.alias_of()),
             sum(1 for e in props.values() if e.grammar() is None)))
    print("%d values: %d types, %d functions (%d prose-defined)"
          % (len(values),
             sum(1 for e in values.values() if e.kind == "type"),
             sum(1 for e in values.values() if e.kind == "function"),
             sum(1 for e in values.values() if e.grammar() is None)))
    print("%d at-rules (%d legacy aliases); %d descriptors across %d of them (%d legacy aliases, "
          "%d without a grammar)"
          % (len(at_rules.rules), len(AT_RULE_ALIASES), len(descs), len({key[0] for key in descs}),
             len(DESCRIPTOR_ALIASES), sum(1 for e in descs.values() if e.grammar() is None)))
    print("%d terminals: %d reachable from a property value, %d only from rules"
          % (len(terminals), len(grammars.value_terminals),
             len(terminals) - len(grammars.value_terminals)))
    functions = [p for p in pseudos if p.function]
    print("%d pseudo-classes and %d pseudo-elements; of %d functional, %d take prose: %s"
          % (sum(1 for p in pseudos if p.kind == "pseudo-class"),
             sum(1 for p in pseudos if p.kind == "pseudo-element"), len(functions),
             sum(1 for p in functions if p.argument is None),
             ", ".join(("::" if p.kind == "pseudo-element" else ":") + p.name + "()"
                       for p in functions if p.argument is None)))
    multiplied = sum(1 for row in grammars.nodes if row[1] != "0")
    print("%d of %d multiplied nodes decided by the next token, %d sequences of leaves"
          % (sum(1 for s in grammars.node_shapes if "Decided" in s), multiplied,
             sum(1 for s in grammars.node_shapes if "Leaves" in s)))
    print("%d grammar nodes, %d keywords, %d ranges"
          % (len(grammars.nodes), len(grammars.keywords), len(grammars.ranges)))
    if undefined:
        print("referenced but never defined: %s" % ", ".join(undefined))
    unlisted = sorted(name for name, e in props.items()
                      if e.alias_of() is None and says_shorthand(e) and name not in shorthands)
    if unlisted:
        print("shorthands webref names that SHORTHANDS does not: %s" % ", ".join(unlisted))
    if conflicts:
        print("\nunresolved cross-module conflicts (add to SPEC_PRIORITY):")
        for pair, names in sorted(conflicts.items()):
            print("  %s: %s" % (" vs ".join(pair), ", ".join(sorted(names)[:6])))


if __name__ == "__main__":
    main()
