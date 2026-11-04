// CSS declaration and value parsing.
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/

#include "css/src/css_parse.h"

#include "pr/search.h"

#include "css/src/css_color.h"
#include "css/src/css_grid.h"
#include "css/src/css_match.h"
#include "css/src/css_parse_terminal.h"

namespace pr_impl {

namespace {

// Matches a CSS-wide keyword that is the whole stream (css-values-4/#common-keywords). On failure, the stream and pool
// are unmodified.
bool matchCssWideKeyword(CssTokenStream* ts, CssValuePool* pool) {
    if (cssPeek(ts) != CssTokenKind_Ident)
        return false;
    PrCssKeyword keyword = cssTokenKeyword(ts->tokens, ts->pos);
    CssTokenStream rest = *ts;
    ++rest.pos;
    if (!cssIsCssWideKeyword(keyword) || !cssAtEnd(&rest))
        return false;
    ++ts->pos;
    cssValuePoolAdd(pool, cssKeywordValue(keyword));
    return true;
}

// How the arbitrary substitution functions in a stream affect its parse (css-values-5/#arbitrary-substitution).
enum {
    Substitution_None,      // the stream contains none
    Substitution_Valid,     // the stream contains one, and each is syntactically valid
    Substitution_Invalid,   // one of them is not syntactically valid
};
typedef uint8_t Substitution;

// Returns true if a function named `keyword` is an arbitrary substitution function.
bool isSubstitution(PrCssKeyword keyword) {
    return cssSubstitutionFunction(keyword) != CssValueDef_Count;
}

// Returns true if the tokens of `span` contain an arbitrary substitution function. If they do, what the grammar would
// match is unknown until substitution.
bool holdsSubstitution(const CssTokenStream* ts, CssTokenSpan span) {
    for (uint32_t i = span.start; i < span.start + span.count; ++i) {
        if (cssTokenKind(ts->tokens, i) == CssTokenKind_Function && isSubstitution(cssTokenKeyword(ts->tokens, i)))
            return true;
    }
    return false;
}

Substitution substitution(const CssTokenStream* ts, CssValuePool* pool, CssMatchState* state) {
    Substitution found = Substitution_None;
    for (uint32_t i = ts->pos; i < ts->end; ++i) {
        if (cssTokenKind(ts->tokens, i) != CssTokenKind_Function)
            continue;
        PrCssKeyword keyword = cssTokenKeyword(ts->tokens, i);
        if (!isSubstitution(keyword))
            continue;
        found = Substitution_Valid;
        CssGrammarIndex root = kCssValueDefGrammarRoot[cssSubstitutionFunction(keyword)];

        CssTokenStream scan = cssSubStream(ts, CssTokenSpan{i, ts->end - i});
        CssTokenSpan span;
        cssConsumeComponentValue(&scan, &span);
        if (holdsSubstitution(ts, CssTokenSpan{span.start + 1, span.count - 1}))
            continue;
        CssTokenStream function = cssSubStream(ts, span);
        uint32_t values = pool->values.count;
        uint32_t text = pool->text.count;
        CssValueSpan matched;
        bool valid = cssParseValue(root, &function, pool, state, &matched);
        pool->values.count = values;
        pool->text.count = text;
        if (!valid)
            return Substitution_Invalid;
    }
    return found;
}

// Folds feature tag values (css-fonts-4/#feature-tag-value). A feature tag's value of 1 or `on` is its default when
// alone, so it is omitted, and `off` becomes 0. The span covers the pool's values from its start to the end.
void foldFeatureTagValues(CssValuePool* pool, CssValueSpan* span) {
    for (uint32_t i = span->start; i < pool->values.count;) {
        CssValueNode* value = &pool->values.data[i];
        if ((value->kind == PrCssValueKind_Integer && value->number == 1) || cssValueIsKeyword(value, PrCssKeyword_On)) {
            pool->values.erase(i);
            --span->count;
            continue;
        }
        if (cssValueIsKeyword(value, PrCssKeyword_Off)) {
            *value = cssNumberValue(PrCssValueKind_Integer, PrCssUnit_Number, 0);
            value->next = 1;
        }
        i += value->next;
    }
}

// Folds a language system tag (css-fonts-4/#font-language-override-prop). A tag shorter than four characters is padded
// with spaces at the end, so trailing spaces are omitted.
void foldLanguageTag(CssValuePool* pool, CssValueSpan span) {
    CssValueNode* tag = &pool->values.data[span.start];
    if (tag->kind != PrCssValueKind_String)
        return;
    while (tag->text.length != 0 && pool->text.data[tag->text.start + tag->text.length - 1] == ' ')
        --tag->text.length;
}

// Returns true if the colors of a run are absolute colors: the run contains no currentcolor, system color,
// light-dark(), contrast-color() or device-cmyk() (css-color-5/#absolute-color).
bool absoluteColors(CssValueRun run) {
    bool absolute = true;
    for (uint32_t i = 0; i < run.count; ++i) {
        const CssValueNode* value = &run.values[i];
        bool keyword = value->kind == PrCssValueKind_Keyword;
        PrCssKeyword name = cssValueKeyword(value);
        absolute = absolute && !(keyword && (name == PrCssKeyword_Currentcolor || cssIsSystemColor(name))) &&
            !cssValueIsFunction(value, PrCssKeyword_LightDark) && !cssValueIsFunction(value, PrCssKeyword_ContrastColor) &&
            !cssValueIsFunction(value, PrCssKeyword_DeviceCmyk);
    }
    return absolute;
}

// Returns true if the integers that the items of a comma-separated value give, at `at` of each item, satisfy `ordered`
// with the previous item's. An item's integer is the first or the second of its integer or infinite values, as `at`
// says, and an infinite value or a calculation, which resolves only later, is passed as null. `ordered` takes the
// previous item's and this item's, or the item's two values for a range.
template <typename Ordered>
bool itemsOrdered(CssValueRun run, Ordered ordered) {
    const CssValueNode* item[2] = {};
    uint32_t count = 0;
    const CssValueNode* previous = nullptr;
    bool first = true;
    for (uint32_t i = 0; i <= run.count; i = i < run.count ? i + run.values[i].next : i + 1) {
        if (i == run.count || cssValueIsLiteral(&run.values[i], ',')) {
            if (!ordered(first ? nullptr : previous, item[0], count > 1 ? item[1] : nullptr))
                return false;
            previous = item[0];
            first = false;
            item[0] = item[1] = nullptr;
            count = 0;
        } else if (run.values[i].kind == PrCssValueKind_Integer ||
                cssValueIsKeyword(&run.values[i], PrCssKeyword_Infinite)) {
            if (count < 2)
                item[count] = run.values[i].kind == PrCssValueKind_Integer ? &run.values[i] : nullptr;
            ++count;
        }
    }
    return true;
}

// Checks the requirements that the prose places on a descriptor's value beyond its grammar. The value is the pool's
// values from the span's start to the end. The value is stored in the single form the prose gives for values with the
// same meaning.
bool checkDescriptor(PrCssDescriptor descriptor, CssValuePool* pool, CssValueSpan* span) {
    switch (descriptor) {
        case PrCssDescriptor_FontFaceFontFeatureSettings:
            foldFeatureTagValues(pool, span);
            return true;
        case PrCssDescriptor_FontFaceFontLanguageOverride:
            foldLanguageTag(pool, *span);
            return true;
        case PrCssDescriptor_FontPaletteValuesOverrideColors:
            // css-fonts-4/#descdef-font-palette-values-override-colors: each color must be an absolute color.
            return absoluteColors(cssValuePoolRun(pool, *span));
        case PrCssDescriptor_CounterStyleAdditiveSymbols:
            // css-counter-styles-3/#counter-style-symbols: the weights must strictly decrease.
            return itemsOrdered(cssValuePoolRun(pool, *span), [](const CssValueNode* PR_NULLABLE previous,
                const CssValueNode* PR_NULLABLE weight, const CssValueNode* PR_NULLABLE) {
                    return previous == nullptr || weight == nullptr || weight->number < previous->number;
                });
        case PrCssDescriptor_CounterStyleRange:
            // css-counter-styles-3/#counter-style-range: no range's lower bound is above its upper bound. An infinite
            // lower bound is below every bound, and an infinite upper bound above.
            return itemsOrdered(cssValuePoolRun(pool, *span), [](const CssValueNode* PR_NULLABLE,
                const CssValueNode* PR_NULLABLE low, const CssValueNode* PR_NULLABLE high) {
                    return low == nullptr || high == nullptr || low->number <= high->number;
                });
        default:
            return true;
    }
}

// Returns a matcher over the whole of `ts`, for a value of `property` if `ts` is one.
CssMatcher matcherOf(CssTokenStream* ts, CssValuePool* pool, CssMatchState* state,
        const CssParseContext* PR_NULLABLE context, PrCssProperty property) {
    CssMatcher m = {};
    m.ts = ts;
    m.pool = pool;
    m.match = state;
    m.context = context;
    m.argument = CssMatchArgument{CssPseudoClass_None, CssPseudoElement_None, false};
    m.anchors = property == PrCssProperty_Count ? 0 : kCssPropertyAnchorFunctions[property];
    m.property = property;
    m.start = ts->pos;
    return m;
}

// Replaces each item of a value of `property`, from `first` on in the pool, that consists of keywords that the
// property's prose says are equal to fewer keywords. The item is replaced with those fewer keywords (kCssKeywordForms).
void holdKeywordForms(PrCssProperty property, CssValuePool* pool, uint32_t first) {
    CssKeywordFormRun run = kCssPropertyKeywordForms[property];
    if (run.count == 0)
        return;

    RawArray<CssValueNode>* values = &pool->values;
    for (uint32_t item = first; item < values->count; ++item) {
        uint64_t written = 0;
        uint32_t keywords = 0;
        bool plain = true;
        uint32_t end = item;
        for (; end < values->count && !cssValueIsLiteral(&values->data[end], ','); end += values->data[end].next) {
            plain = plain && values->data[end].kind == PrCssValueKind_Keyword;
            written = written << 16 | cssValueKeyword(&values->data[end]);
            ++keywords;
        }
        if (plain && keywords <= 3) {
            for (uint32_t i = keywords; i < 3; ++i)
                written = written << 16 | PrCssKeyword_Unknown;
            const CssKeywordForm* forms = kCssKeywordForms + run.start;
            uint32_t at = lowerBound(Span(forms, run.count), written);
            if (at < run.count && forms[at].written == written) {
                uint32_t held = 0;
                for (; held < 3 && forms[at].held[held] != PrCssKeyword_Unknown; ++held)
                    values->data[item + held].aux = forms[at].held[held];
                values->erase(item + held, keywords - held);
                end = item + held;
            }
        }
        item = end;
    }
}

// Stores a rotate value from `first` on in the pool in the form that
// css-transforms-2/#individual-transform-serialization requires. A rotation about the z axis "must serialize as just an
// <angle>", and one about an axis "parallel with the x or y axes [...] as the appropriate keyword". The angle is
// negated if the axis points the other way.
void foldRotate(CssValuePool* pool, uint32_t first) {
    RawArray<CssValueNode>* values = &pool->values;
    uint32_t count = values->count - first;
    CssValueNode* angle = &values->data[values->count - 1];
    if (count == 2 && cssValueIsKeyword(&values->data[first], PrCssKeyword_Z)) {
        values->erase(first);
        return;
    }
    if (count != 4 || angle->kind != PrCssValueKind_Dimension)
        return;

    const CssValueNode* axis = &values->data[first];
    uint32_t along = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        if (axis[i].kind != PrCssValueKind_Number && axis[i].kind != PrCssValueKind_Integer)
            return;
        if (axis[i].number != 0.0)
            along = along == 3 ? i : 4;
    }
    if (along >= 3)
        return;
    if (axis[along].number < 0.0)
        angle->number = -angle->number;
    if (along == 2) {
        values->erase(first, 3);
        return;
    }
    values->data[first] = cssKeywordValue(along == 0 ? PrCssKeyword_X : PrCssKeyword_Y);
    values->erase(first + 1, 2);
}

// Stores a value of `property`, from `first` on in the pool, in the single form that its prose gives for values with
// the same meaning.
void foldProperty(PrCssProperty property, CssValuePool* pool, uint32_t first) {
    holdKeywordForms(property, pool, first);
    CssValueSpan span = {first, pool->values.count - first};
    switch (property) {
        case PrCssProperty_Rotate:
            foldRotate(pool, first);
            break;
        case PrCssProperty_FontFeatureSettings:
            foldFeatureTagValues(pool, &span);
            break;
        case PrCssProperty_FontLanguageOverride:
            foldLanguageTag(pool, span);
            break;
        case PrCssProperty_FontSizeAdjust:
            // css-fonts-5/#propdef-font-size-adjust: the metric defaults to ex-height.
            if (span.count > 1 && cssValueIsKeyword(&pool->values.data[first], PrCssKeyword_ExHeight))
                pool->values.erase(first);
            break;
        case PrCssProperty_OverflowClipMargin:
            // An omitted <visual-box> is padding-box, as csswg-drafts#13185 resolved. A value with nothing else is 0px.
            if (cssValueIsKeyword(&pool->values.data[first], PrCssKeyword_PaddingBox)) {
                pool->values.erase(first);
                if (pool->values.count == first)
                    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, 0));
            }
            break;
        case PrCssProperty_FontStyle:
            // `oblique 0deg` is `normal`, as csswg-drafts#11430 resolved.
            if (span.count == 2 && cssValueIsKeyword(&pool->values.data[first], PrCssKeyword_Oblique) &&
                    pool->values.data[first + 1].kind == PrCssValueKind_Dimension && pool->values.data[first + 1].number == 0) {
                pool->values.count = first;
                cssValuePoolAdd(pool, cssKeywordValue(PrCssKeyword_Normal));
            }
            break;
        default:
            break;
    }
}

// Matches the whole of the matcher's stream, with the arguments of cssMatchWhole.
bool matchAll(CssMatcher* m, CssGrammarIndex root, CssValueDef production, bool structural, CssValueSpan* out) {
    uint32_t values = m->pool->values.count;
    if (!cssMatchWhole(m, root, production, structural))
        return false;
    *out = CssValueSpan{values, m->pool->values.count - values};
    return true;
}

// Returns the <whole-value> function that the stream consists of, if a single function is all that remains
// (css-values-5/#whole-value). Otherwise returns CssValueDef_Count.
CssValueDef wholeValueFunction(const CssTokenStream* ts) {
    if (cssPeek(ts) != CssTokenKind_Function)
        return CssValueDef_Count;
    CssTokenStream rest = *ts;
    rest.pos = cssTokenBlockEnd(ts->tokens, ts->pos);
    if (rest.pos > rest.end || !cssAtEnd(&rest))
        return CssValueDef_Count;
    return cssWholeValueFunction(cssTokenKeyword(ts->tokens, ts->pos));
}

// Returns true if a <whole-value> function at the stream's position may be the value of `property`. cycle() may not
// contain cycle(), attr() or calc() (css-values-5/#cycle-notation). The values in interpolate() must be animatable,
// which they are if the property is animatable (css-values-5/#funcdef-interpolate).
bool wholeValueAllowed(const CssTokenStream* ts, CssValueDef function, PrCssProperty property) {
    if (function == CssValueDef_InterpolateFn)
        return bitsetTest(kCssPropertyAnimatable, property);
    if (function != CssValueDef_CycleFn)
        return true;
    uint32_t end = cssTokenBlockEnd(ts->tokens, ts->pos);
    for (uint32_t i = ts->pos + 1; i < end; ++i) {
        if (cssTokenKind(ts->tokens, i) != CssTokenKind_Function)
            continue;
        switch (cssTokenKeyword(ts->tokens, i)) {
            case PrCssKeyword_Cycle:
            case PrCssKeyword_Attr:
            case PrCssKeyword_Calc:
                return false;
            default:
                break;
        }
    }
    return true;
}

} // namespace

void cssMatchStateReset(CssMatchState* state) {
    state->frames.reset();
    state->choices.reset();
    state->edits.reset();
    cssMathScratchReset(&state->math);
}

bool cssIsCssWideKeyword(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Initial:
        case PrCssKeyword_Inherit:
        case PrCssKeyword_Unset:
        case PrCssKeyword_Revert:
        case PrCssKeyword_RevertLayer:
        case PrCssKeyword_RevertRule:
            return true;
        default:
            return false;
    }
}

bool cssParsePropertyValue(PrCssProperty property, CssTokenStream* ts, CssValuePool* pool,
        CssMatchState* state, CssValueSpan* out) {
    uint32_t pos = ts->pos;
    uint32_t values = pool->values.count;
    uint32_t text = pool->text.count;
    cssSkipWhitespace(ts);

    // A property accepts a <whole-value> function as its entire value, in the same way that it accepts a CSS-wide
    // keyword (css-values-5/#whole-value).
    PrCssProperty resolved = kCssPropertyAlias[property];
    CssValueDef whole = wholeValueFunction(ts);
    Substitution substituted = substitution(ts, pool, state);
    bool matched;
    if (whole != CssValueDef_Count && !wholeValueAllowed(ts, whole, resolved)) {
        matched = false;
    } else if (substituted != Substitution_None) {
        matched = substituted == Substitution_Valid && cssParseTokens(ts, pool, out);
    } else if (matchCssWideKeyword(ts, pool)) {
        matched = true;
    } else if (whole != CssValueDef_Count) {
        CssMatcher m = matcherOf(ts, pool, state, nullptr, resolved);
        matched = matchAll(&m, kCssValueDefGrammarRoot[whole], CssValueDef_Count, false, out);
    } else {
        CssMatcher m = matcherOf(ts, pool, state, nullptr, resolved);
        matched = matchAll(&m, kCssPropertyGrammarRoot[resolved], CssValueDef_Count, false, out);
        // The strings of grid-template-areas must form a template, which the grammar alone does not check.
        if (matched && resolved == PrCssProperty_GridTemplateAreas)
            matched = cssGridAreasFold(pool, values, pool->values.count - values);
        if (matched)
            foldProperty(resolved, pool, values);
    }

    if (!matched) {
        ts->pos = pos;
        pool->values.count = values;
        pool->text.count = text;
        return false;
    }
    *out = CssValueSpan{values, pool->values.count - values};
    return true;
}

bool cssParseDescriptorValue(PrCssDescriptor descriptor, CssTokenStream* ts, CssValuePool* pool,
        CssMatchState* state, CssValueSpan* out) {
    uint32_t pos = ts->pos;
    uint32_t text = pool->text.count;
    CssMatcher m = matcherOf(ts, pool, state, nullptr, PrCssProperty_Count);
    if (!matchAll(&m, kCssDescriptorGrammarRoot[descriptor], CssValueDef_Count, false, out))
        return false;
    if (checkDescriptor(descriptor, pool, out))
        return true;
    ts->pos = pos;
    pool->values.count = out->start;
    pool->text.count = text;
    return false;
}

bool cssParseTokens(CssTokenStream* ts, CssValuePool* pool, CssValueSpan* out) {
    uint32_t pos = ts->pos;
    uint32_t values = pool->values.count;
    uint32_t text = pool->text.count;
    cssSkipWhitespace(ts);
    if (cssAtEnd(ts)) {
        cssValuePoolAddTokens(pool, ts, CssTokenSpan{ts->pos, 0});
        *out = CssValueSpan{values, 1};
        return true;
    }
    CssTokenSpan span;
    if (!cssConsumeDeclarationValue(ts, &span) || !cssAtEnd(ts)) {
        ts->pos = pos;
        pool->values.count = values;
        pool->text.count = text;
        return false;
    }
    cssValuePoolAddTokens(pool, ts, span);
    *out = CssValueSpan{values, pool->values.count - values};
    return true;
}

bool cssParseValue(CssGrammarIndex root, CssTokenStream* ts, CssValuePool* pool, CssMatchState* state, CssValueSpan* out) {
    CssMatcher m = matcherOf(ts, pool, state, nullptr, PrCssProperty_Count);
    return matchAll(&m, root, CssValueDef_Count, false, out);
}

bool cssParseNumericValue(CssTokenStream* ts, CssValuePool* pool, CssMatchState* state, CssValueSpan* out) {
    uint32_t pos = ts->pos;
    uint32_t values = pool->values.count;
    uint32_t text = pool->text.count;
    CssMatcher m = matcherOf(ts, pool, state, nullptr, PrCssProperty_Count);
    cssSkipWhitespace(ts);
    bool matched = cssParseTypedNumeric(&m);
    cssSkipWhitespace(ts);
    if (!matched || !cssAtEnd(ts)) {
        ts->pos = pos;
        pool->values.count = values;
        pool->text.count = text;
        return false;
    }
    *out = CssValueSpan{values, pool->values.count - values};
    return true;
}

bool cssParseProduction(CssValueDef production, CssTokenStream* ts,
        const CssParseContext* context, CssValuePool* pool, CssMatchState* state, CssValueSpan* out) {
    CssMatcher m = matcherOf(ts, pool, state, context, PrCssProperty_Count);
    return matchAll(&m, CssGrammarRoot_None, production, true, out);
}

bool cssParsePrelude(CssGrammarIndex root, CssTokenStream* ts, const CssParseContext* context,
        CssValuePool* pool, CssMatchState* state, CssValueSpan* out) {
    CssMatcher m = matcherOf(ts, pool, state, context, PrCssProperty_Count);
    return matchAll(&m, root, CssValueDef_Count, true, out);
}

CssPseudoElement cssMatchedPseudoElement(const CssValuePool* pool, uint32_t node) {
    // The legacy form is the production, a ':' and a keyword. The other form is a ':' and a <pseudo-class-selector>,
    // whose name follows its own ':'.
    const CssValueNode* values = pool->values.data;
    if (cssValueIsProduction(&values[node + 1], CssValueDef_LegacyPseudoElementSelector))
        return cssPseudoElement(cssKeywordName(cssValueKeyword(&values[node + 3])), false);
    return CssPseudoElement(values[node + 4].aux);
}

} // namespace pr_impl
