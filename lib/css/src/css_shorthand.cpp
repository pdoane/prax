#include "css/src/css_shorthand.h"

#include "pr/assert.h"
#include "pr/string_ops.h"

#include "css/src/css_grid.h"
#include "css/src/css_keyword.h"
#include "css/src/css_parse.h"
#include "css/src/css_parser.h"
#include "css/src/css_serialize.h"
#include "css/src/css_unit.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Values

// The empty span, held by a longhand that no component has set yet.
constexpr CssValueSpan kUnset = {0, 0};

// The run of values in [first, end) of a pool, each with its subtree.
struct Items {
    uint32_t first;
    uint32_t end;
};

// Returns the children of the value at `index`.
Items childrenOf(const CssValuePool* pool, uint32_t index) {
    return Items{index + 1, index + pool->values.data[index].next};
}

CssValueSpan spanOf(Items items) {
    return CssValueSpan{items.first, items.end - items.first};
}

// Adds a value with no children to the pool.
CssValueSpan addValue(CssValuePool* pool, CssValueNode value) {
    return CssValueSpan{cssValuePoolAdd(pool, value), 1};
}

// ---------------------------------------------------------------------------------------------------------------------
// Expansion

// A shorthand being expanded. The value of each of its longhands is a span of the pool, and the spans start at `first`
// in the parser's `expanded`.
struct Expansion {
    CssParser* parser;
    PrCssProperty shorthand;
    CssPropertyList longhands;
    uint32_t first;
};

CssValuePool* poolOf(const Expansion* e) {
    return &e->parser->pool;
}

CssValueSpan& slot(const Expansion* e, uint32_t index) {
    return e->parser->expanded.data[e->first + index];
}

// Returns the index of `property` among the longhands, or the number of longhands if it is not one of them.
uint32_t indexOf(const Expansion* e, PrCssProperty property) {
    uint32_t i = 0;
    while (i < e->longhands.count && e->longhands.data[i] != property)
        ++i;
    return i;
}

// Returns the initial value of a longhand, copied onto the end of the pool. Initial values are parsed into the parser's
// `initials` the first time that one is requested.
CssValueSpan initialValue(CssParser* parser, PrCssProperty longhand) {
    if (parser->initialOf.count == 0) {
        parser->initialOf.reserve(PrCssProperty_Count);
        for (uint32_t i = 0; i < PrCssProperty_Count; ++i)
            parser->initialOf.push(kUnset);
    }
    CssValueSpan held = parser->initialOf.data[longhand];
    if (held.count == 0) {
        cssTokenize(&parser->asideTokens, &parser->asideScratch, cssPropertyInitial(longhand));
        CssTokenStream ts = cssStream(&parser->asideTokens);
        bool parsed =
            cssParsePropertyValue(longhand, &ts, &parser->initials, &parser->match, &held);
        PR_ASSERT(parsed);
        (void)parsed;
        parser->initialOf.data[longhand] = held;
    }
    return cssValuePoolAddRun(&parser->pool, cssValuePoolRun(&parser->initials, held));
}

// Sets each longhand that no component set to its initial value.
void fillInitial(const Expansion* e) {
    for (uint32_t i = 0; i < e->longhands.count; ++i) {
        if (slot(e, i).count == 0)
            slot(e, i) = initialValue(e->parser, e->longhands.data[i]);
    }
}

bool expand(const Expansion* e, Items value);

// Expands `value` into the values of the longhands of the shorthand `property`. Passes each value to `take` with its
// index among those longhands.
template <typename Take>
bool expandInner(const Expansion* e, PrCssProperty property, Items value, Take take) {
    CssParser* parser = e->parser;
    Expansion inner = {parser, property, cssPropertyLonghands(property), parser->expanded.count};
    for (uint32_t i = 0; i < inner.longhands.count; ++i)
        parser->expanded.push(kUnset);
    bool expanded = expand(&inner, value);
    for (uint32_t i = 0; expanded && i < inner.longhands.count; ++i)
        take(i, inner.longhands.data[i], slot(&inner, i));
    parser->expanded.count = inner.first;
    return expanded;
}

// Sets the property that the component names, which is a longhand or a shorthand within this one, to `value`.
bool set(const Expansion* e, PrCssProperty property, Items value) {
    if (!cssIsShorthand(property)) {
        PR_ASSERT(indexOf(e, property) < e->longhands.count);
        slot(e, indexOf(e, property)) = spanOf(value);
        return true;
    }
    return expandInner(e, property, value,
                       [e](uint32_t, PrCssProperty longhand, CssValueSpan span) {
                           PR_ASSERT(indexOf(e, longhand) < e->longhands.count);
                           slot(e, indexOf(e, longhand)) = span;
                       });
}

// Returns the number of values at the top level of `value`: components, and values that no component holds.
uint32_t countItems(const CssValuePool* pool, Items value) {
    uint32_t count = 0;
    for (uint32_t i = value.first; i < value.end; i += pool->values.data[i].next)
        ++count;
    return count;
}

// Returns the value of item `index` of `value`: a component's children, or the item itself.
Items itemAt(const CssValuePool* pool, Items value, uint32_t index) {
    uint32_t i = value.first;
    for (; index != 0; --index)
        i += pool->values.data[i].next;
    const CssValueNode* item = &pool->values.data[i];
    return item->kind == PrCssValueKind_Longhand ? childrenOf(pool, i) : Items{i, i + item->next};
}

// css-box-4/#propdef-margin: the values are top, right, bottom, left. An omitted side takes the value of the opposite
// side, and an omitted left takes the value of the top.
bool expandBox(const Expansion* e, Items value) {
    const CssValuePool* pool = poolOf(e);
    uint32_t count = countItems(pool, value);
    static constexpr uint8_t kSide[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (uint32_t side = 0; side < 4; ++side)
        slot(e, side) = spanOf(itemAt(pool, value, kSide[count - 1][side]));
    return true;
}

// css-logical-1/#propdef-margin-block: if the second value is omitted, it copies the first.
bool expandPair(const Expansion* e, Items value) {
    const CssValuePool* pool = poolOf(e);
    uint32_t count = countItems(pool, value);
    slot(e, 0) = spanOf(itemAt(pool, value, 0));
    slot(e, 1) = spanOf(itemAt(pool, value, count - 1));
    return true;
}

// css-logical-1/#propdef-border-block: the value is the value of the one property that the grammar names. Each further
// run of the same number of longhands takes that value as well.
bool expandMirror(const Expansion* e, Items value) {
    const CssValueNode* component = &poolOf(e)->values.data[value.first];
    PrCssProperty named = cssValueLonghand(component);
    Items inner = childrenOf(poolOf(e), value.first);
    uint32_t run = 1;
    if (!cssIsShorthand(named))
        slot(e, 0) = spanOf(inner);
    else if (!expandInner(e, named, inner,
                          [e](uint32_t i, PrCssProperty, CssValueSpan span) { slot(e, i) = span; }))
        return false;
    else
        run = cssPropertyLonghands(named).count;
    for (uint32_t i = run; i < e->longhands.count; ++i)
        slot(e, i) = slot(e, i % run);
    return true;
}

// css-cascade-5/#shorthand-property: each component sets what it names, and a longhand that no component sets takes its
// initial value.
bool setComponents(const Expansion* e, Items value) {
    const CssValuePool* pool = poolOf(e);
    for (uint32_t i = value.first; i < value.end; i += pool->values.data[i].next) {
        const CssValueNode* item = &pool->values.data[i];
        if (item->kind == PrCssValueKind_Longhand
            && !set(e, cssValueLonghand(item), childrenOf(pool, i)))
            return false;
    }
    return true;
}

bool expandComponents(const Expansion* e, Items value) {
    if (!setComponents(e, value))
        return false;
    fillInitial(e);
    return true;
}

// Returns the longhands that a component of the shorthand's grammar sets, as one bit per index among the longhands.
uint64_t componentsOf(PrCssProperty shorthand, CssPropertyList longhands) {
    CssGrammarIndex root = kCssPropertyGrammarRoot[shorthand];
    CssGrammarIndex end = cssGrammarAfter(root);
    uint64_t set = 0;
    for (CssGrammarIndex i = root; i < end; ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        if (cssGrammarType(node) != CssGrammarType_Longhand)
            continue;
        for (uint32_t j = 0; j < longhands.count; ++j) {
            if (longhands.data[j] == cssGrammarValue(node))
                set |= uint64_t(1) << j;
        }
    }
    return set;
}

// Reads the layer of `value` at `*at`, splitting at commas, and moves `*at` past it. Returns false past the last layer.
bool nextLayer(const CssValuePool* pool, Items value, uint32_t* at, Items* layer) {
    if (*at > value.end)
        return false;
    uint32_t i = *at;
    while (i < value.end && !cssValueIsLiteral(&pool->values.data[i], ','))
        i += pool->values.data[i].next;
    *layer = Items{*at, i};
    *at = i + 1;
    return true;
}

// Returns the component of `layer` that sets `longhand`, or empty items if there is none.
Items componentIn(const CssValuePool* pool, Items layer, PrCssProperty longhand) {
    for (uint32_t i = layer.first; i < layer.end; i += pool->values.data[i].next) {
        const CssValueNode* item = &pool->values.data[i];
        if (item->kind == PrCssValueKind_Longhand && cssValueLonghand(item) == longhand)
            return childrenOf(pool, i);
    }
    return Items{0, 0};
}

// Adds to the pool the entry that a layer gives a longhand. Returns false if the layer has none.
typedef bool (*LayerEntryFn)(const Expansion* e, Items layer, PrCssProperty longhand);

// Adds to the pool the entry that `layer` gives `longhand`, which is the value of the component that sets it. Returns
// false if the layer has no such component.
bool componentEntry(const Expansion* e, Items layer, PrCssProperty longhand) {
    Items component = componentIn(poolOf(e), layer, longhand);
    if (component.end == 0)
        return false;
    cssValuePoolRepeat(poolOf(e), spanOf(component));
    return true;
}

// css-backgrounds-3/#layering: a longhand that a component sets takes one entry per layer. The entry is what `entry`
// gives it, or its initial value. The other longhands take their initial values once. The longhands in `last` take the
// single entry of the last layer.
bool expandLayers(const Expansion* e, Items value, uint64_t last, LayerEntryFn entry) {
    CssValuePool* pool = poolOf(e);
    uint64_t components = componentsOf(e->shorthand, e->longhands);
    for (uint32_t j = 0; j < e->longhands.count; ++j) {
        PrCssProperty longhand = e->longhands.data[j];
        uint64_t bit = uint64_t(1) << j;
        if ((components & bit) == 0) {
            slot(e, j) = initialValue(e->parser, longhand);
            continue;
        }
        uint32_t start = pool->values.count;
        Items layer;
        for (uint32_t at = value.first; nextLayer(pool, value, &at, &layer);) {
            if ((last & bit) != 0 && layer.end != value.end)
                continue;
            if (pool->values.count != start)
                cssValuePoolAdd(pool, cssLiteralValue(','));
            if (!entry(e, layer, longhand))
                initialValue(e->parser, longhand);
        }
        slot(e, j) = CssValueSpan{start, pool->values.count - start};
    }
    return true;
}

// The longhands for the two boxes of a layered shorthand: the origin and the clip.
struct Boxes {
    PrCssProperty origin;
    PrCssProperty clip;
};

Boxes boxesOf(PrCssProperty shorthand) {
    return shorthand == PrCssProperty_Background
               ? Boxes{PrCssProperty_BackgroundOrigin, PrCssProperty_BackgroundClip}
               : Boxes{PrCssProperty_MaskOrigin, PrCssProperty_MaskClip};
}

// css-backgrounds-4/#background, css-masking-1/#the-mask: one box sets both the origin and the clip. A clip alone
// leaves the origin border-box.
bool boxEntry(const Expansion* e, Items layer, PrCssProperty longhand) {
    if (componentEntry(e, layer, longhand))
        return true;
    Boxes boxes = boxesOf(e->shorthand);
    if (longhand == boxes.clip)
        return componentEntry(e, layer, boxes.origin);
    if (longhand == boxes.origin && componentIn(poolOf(e), layer, boxes.clip).end != 0) {
        addValue(poolOf(e), cssKeywordValue(PrCssKeyword_BorderBox));
        return true;
    }
    return false;
}

// css-backgrounds-3/#propdef-background: the color belongs to the final layer.
bool expandBackground(const Expansion* e, Items value) {
    uint64_t color = uint64_t(1) << indexOf(e, PrCssProperty_BackgroundColor);
    return expandLayers(e, value, color, boxEntry);
}

// css-lists-3/#propdef-list-style: a `none` sets whichever of the image and the type no other component sets.
bool expandListStyle(const Expansion* e, Items value) {
    if (!setComponents(e, value))
        return false;
    uint32_t image = indexOf(e, PrCssProperty_ListStyleImage);
    uint32_t type = indexOf(e, PrCssProperty_ListStyleType);
    CssValueSpan set = slot(e, image);
    if (set.count == 1 && cssValueIsKeyword(&poolOf(e)->values.data[set.start], PrCssKeyword_None)
        && slot(e, type).count == 0)
        slot(e, type) = set;
    fillInitial(e);
    return true;
}

// Returns the keyword if `value` is a single keyword, or PrCssKeyword_Count otherwise.
PrCssKeyword soleKeyword(const CssValuePool* pool, Items value) {
    const CssValueNode* first = &pool->values.data[value.first];
    return value.end - value.first == 1 && first->kind == PrCssValueKind_Keyword
               ? cssValueKeyword(first)
               : PrCssKeyword_Count;
}

// Sets the longhand at `index` to the keyword `keyword`.
void setKeyword(const Expansion* e, uint32_t index, PrCssKeyword keyword) {
    slot(e, index) = addValue(poolOf(e), cssKeywordValue(keyword));
}

// css-flexbox-1/#propdef-flex: `none` is `0 0 auto`. An omitted flex-grow or flex-shrink is 1, and an omitted
// flex-basis is 0%.
bool expandFlex(const Expansion* e, Items value) {
    CssValuePool* pool = poolOf(e);
    if (soleKeyword(pool, value) == PrCssKeyword_None) {
        slot(e, 0) = addValue(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 0));
        slot(e, 1) = addValue(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 0));
        setKeyword(e, 2, PrCssKeyword_Auto);
        return true;
    }
    if (!setComponents(e, value))
        return false;
    for (uint32_t i = 0; i < 2; ++i) {
        if (slot(e, i).count == 0)
            slot(e, i) = addValue(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 1));
    }
    if (slot(e, 2).count == 0)
        slot(e, 2) = addValue(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, 0));
    return true;
}

// css-fonts-4/#propdef-font-synthesis: each keyword written makes its longhand auto, and each omitted keyword makes its
// longhand none.
bool expandFontSynthesis(const Expansion* e, Items value) {
    static constexpr PrCssKeyword kKeyword[] = {PrCssKeyword_Weight, PrCssKeyword_Style,
                                              PrCssKeyword_SmallCaps, PrCssKeyword_Position};
    const CssValuePool* pool = poolOf(e);
    for (uint32_t j = 0; j < 4; ++j) {
        bool written = false;
        for (uint32_t i = value.first; i < value.end; ++i)
            written = written || cssValueIsKeyword(&pool->values.data[i], kKeyword[j]);
        setKeyword(e, j, written ? PrCssKeyword_Auto : PrCssKeyword_None);
    }
    return true;
}

// css-fonts-4/#propdef-font-variant: `normal` makes every longhand normal, and `none` makes font-variant-ligatures none
// and the rest normal.
bool expandFontVariant(const Expansion* e, Items value) {
    PrCssKeyword keyword = soleKeyword(poolOf(e), value);
    if (keyword == PrCssKeyword_None)
        setKeyword(e, indexOf(e, PrCssProperty_FontVariantLigatures), PrCssKeyword_None);
    else if (keyword != PrCssKeyword_Normal && !setComponents(e, value))
        return false;
    fillInitial(e);
    return true;
}

// css-inline-3/#propdef-text-box: `normal` is `none auto`. An omitted text-box-trim is trim-both, and an omitted
// text-box-edge is auto.
bool expandTextBox(const Expansion* e, Items value) {
    if (soleKeyword(poolOf(e), value) != PrCssKeyword_Normal) {
        if (!setComponents(e, value))
            return false;
        if (slot(e, 0).count == 0)
            setKeyword(e, 0, PrCssKeyword_TrimBoth);
    }
    fillInitial(e);
    return true;
}

// css-text-4/#propdef-white-space: normal, pre, pre-wrap and pre-line each expand to a white-space-collapse and a
// text-wrap-mode.
bool expandWhiteSpace(const Expansion* e, Items value) {
    PrCssKeyword collapse = PrCssKeyword_Count;
    PrCssKeyword wrap = PrCssKeyword_Wrap;
    switch (soleKeyword(poolOf(e), value)) {
        case PrCssKeyword_Normal:     collapse = PrCssKeyword_Collapse; break;
        case PrCssKeyword_Pre:
            collapse = PrCssKeyword_Preserve;
            wrap = PrCssKeyword_Nowrap;
            break;
        case PrCssKeyword_PreWrap:    collapse = PrCssKeyword_Preserve; break;
        case PrCssKeyword_PreLine:    collapse = PrCssKeyword_PreserveBreaks; break;
        default:                    return expandComponents(e, value);
    }
    setKeyword(e, 0, collapse);
    setKeyword(e, 1, wrap);
    fillInitial(e);
    return true;
}

// css-writing-modes-4/#glyph-orientation: auto is mixed. The grammar allows an angle or a number of 0 or 90 degrees,
// which is upright at 0 and sideways at 90.
bool expandGlyphOrientationVertical(const Expansion* e, Items value) {
    const CssValueNode* sole = &poolOf(e)->values.data[value.first];
    PrCssKeyword orientation = PrCssKeyword_Mixed;
    if (sole->kind != PrCssValueKind_Keyword) {
        double degrees =
            sole->kind == PrCssValueKind_Dimension ? sole->number * cssUnitScale(sole->unit) : sole->number;
        orientation = degrees < 45 ? PrCssKeyword_Upright : PrCssKeyword_Sideways;
    }
    setKeyword(e, 0, orientation);
    return true;
}

// css-text-3/#propdef-text-align: justify-all sets both longhands to justify, and match-parent sets both to
// match-parent. Any other value sets text-align-all, and resets text-align-last to auto.
bool expandTextAlign(const Expansion* e, Items value) {
    PrCssKeyword keyword = soleKeyword(poolOf(e), value);
    switch (keyword) {
        case PrCssKeyword_Count: return false;
        case PrCssKeyword_JustifyAll:
            setKeyword(e, 0, PrCssKeyword_Justify);
            setKeyword(e, 1, PrCssKeyword_Justify);
            break;
        case PrCssKeyword_MatchParent:
            setKeyword(e, 0, PrCssKeyword_MatchParent);
            setKeyword(e, 1, PrCssKeyword_MatchParent);
            break;
        default:
            setKeyword(e, 0, keyword);
            setKeyword(e, 1, PrCssKeyword_Auto);
            break;
    }
    return true;
}

// css-align-3/#propdef-place-content: if the second longhand is omitted, it copies the first. The exception is a
// baseline place-content, whose justify-content is start.
bool expandPlace(const Expansion* e, Items value) {
    if (!setComponents(e, value))
        return false;
    if (slot(e, 1).count == 0) {
        const CssValueNode* first = &poolOf(e)->values.data[slot(e, 0).start];
        bool baseline = cssValueIsKeyword(first, PrCssKeyword_Baseline)
                        || cssValueIsKeyword(first, PrCssKeyword_First)
                        || cssValueIsKeyword(first, PrCssKeyword_Last);
        if (e->shorthand == PrCssProperty_PlaceContent && baseline)
            setKeyword(e, 1, PrCssKeyword_Start);
        else
            slot(e, 1) = slot(e, 0);
    }
    return true;
}

// Writes to `parts` the parts of `value` between its '/' literals, at most `limit` of them. Returns the number of
// parts.
uint32_t slashParts(const CssValuePool* pool, Items value, Items* parts, uint32_t limit) {
    uint32_t count = 0;
    uint32_t first = value.first;
    for (uint32_t i = value.first; i < value.end; i += pool->values.data[i].next) {
        if (cssValueIsLiteral(&pool->values.data[i], '/') && count + 1 < limit) {
            parts[count++] = Items{first, i};
            first = i + 1;
        }
    }
    parts[count++] = Items{first, value.end};
    return count;
}

// css-backgrounds-3/#propdef-border-radius: the horizontal radii of the four corners, given in the same form as the
// four sides of a box, then the vertical radii after a '/'. A corner whose two radii are the same holds one value. A
// shorthand of one side's two corners (css-borders-4/#corner-sizing-side-shorthands) gives each axis's radii as a pair,
// whose second value is the first if it is omitted.
bool expandBorderRadius(const Expansion* e, Items value) {
    CssValuePool* pool = poolOf(e);
    Items parts[2];
    uint32_t count = slashParts(pool, value, parts, 2);
    Items vertical = count == 2 ? parts[1] : parts[0];
    static constexpr uint8_t kSide[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    uint32_t across = countItems(pool, parts[0]);
    uint32_t down = countItems(pool, vertical);
    uint32_t corners = e->longhands.count;
    for (uint32_t corner = 0; corner < corners; ++corner) {
        CssValueSpan h = spanOf(itemAt(pool, parts[0], kSide[across - 1][corner]));
        CssValueSpan v = spanOf(itemAt(pool, vertical, kSide[down - 1][corner]));
        if (cssValueRunEquals(cssValuePoolRun(pool, h), cssValuePoolRun(pool, v))) {
            slot(e, corner) = h;
            continue;
        }
        uint32_t start = pool->values.count;
        cssValuePoolRepeat(pool, h);
        cssValuePoolRepeat(pool, v);
        slot(e, corner) = CssValueSpan{start, pool->values.count - start};
    }
    return true;
}

// css-grid-2/#propdef-grid-row: an omitted line copies the other line if that is a lone <custom-ident>, and is auto
// otherwise.
CssValueSpan omittedLine(const Expansion* e, CssValueSpan from) {
    const CssValueNode* first = &poolOf(e)->values.data[from.start];
    if (from.count == 1 && first->kind == PrCssValueKind_Ident)
        return from;
    return addValue(poolOf(e), cssKeywordValue(PrCssKeyword_Auto));
}

bool expandGridLine(const Expansion* e, Items value) {
    Items parts[2];
    uint32_t count = slashParts(poolOf(e), value, parts, 2);
    slot(e, 0) = spanOf(parts[0]);
    slot(e, 1) = count == 2 ? spanOf(parts[1]) : omittedLine(e, slot(e, 0));
    return true;
}

// css-grid-2/#propdef-grid-area: row-start, column-start, row-end and column-end, in that order. Each omitted line
// copies the start of its axis by the rule of grid-row, and an omitted column-start copies row-start.
bool expandGridArea(const Expansion* e, Items value) {
    Items parts[4];
    uint32_t count = slashParts(poolOf(e), value, parts, 4);
    static constexpr uint8_t kFrom[4] = {0, 0, 0, 1};
    for (uint32_t i = 0; i < 4; ++i)
        slot(e, i) = i < count ? spanOf(parts[i]) : omittedLine(e, slot(e, kFrom[i]));
    return true;
}

// css-gaps-1/#propdef-column-rule-inset-start, css-cascade-5/#propdef-all: every longhand takes the value.
bool expandSame(const Expansion* e, Items value) {
    for (uint32_t i = 0; i < e->longhands.count; ++i)
        slot(e, i) = spanOf(value);
    return true;
}

// css-gaps-1/#propdef-column-rule-inset: omitted junction insets copy the cap insets.
bool expandRuleInset(const Expansion* e, Items value) {
    if (!setComponents(e, value))
        return false;
    for (uint32_t i = 2; i < 4; ++i) {
        if (slot(e, i).count == 0)
            slot(e, i) = slot(e, i - 2);
    }
    return true;
}

// Adds the entry of one <gap-rule> for `longhand`: the value of its component, or the initial value.
void gapRuleEntry(const Expansion* e, Items rule, PrCssProperty longhand) {
    if (!componentEntry(e, rule, longhand))
        initialValue(e->parser, longhand);
}

// css-gaps-1/#propdef-column-rule: a list of <gap-rule>s and repeat()s of them. Each longhand takes a list of the same
// shape, whose entries are the value of the longhand's component in each rule, or its initial value.
bool expandGapRule(const Expansion* e, Items value) {
    CssValuePool* pool = poolOf(e);
    for (uint32_t j = 0; j < e->longhands.count; ++j) {
        PrCssProperty longhand = e->longhands.data[j];
        uint32_t start = pool->values.count;
        Items item;
        for (uint32_t at = value.first; nextLayer(pool, value, &at, &item);) {
            if (pool->values.count != start)
                cssValuePoolAdd(pool, cssLiteralValue(','));
            CssValueNode repeat = pool->values.data[item.first];
            if (!cssValueIsFunction(&repeat, PrCssKeyword_Repeat)) {
                gapRuleEntry(e, item, longhand);
                continue;
            }

            // repeat(): the count, then an entry for each rule that it repeats.
            uint32_t function = cssValuePoolAdd(pool, repeat);
            Items arguments = childrenOf(pool, item.first);
            Items part;
            for (uint32_t inner = arguments.first; nextLayer(pool, arguments, &inner, &part);) {
                if (part.first == arguments.first) {
                    cssValuePoolRepeat(pool, spanOf(part));
                    continue;
                }
                cssValuePoolAdd(pool, cssLiteralValue(','));
                gapRuleEntry(e, part, longhand);
            }
            pool->values.data[function].next = pool->values.count - function;
        }
        slot(e, j) = CssValueSpan{start, pool->values.count - start};
    }
    return true;
}

// css-grid-2/#propdef-grid-template: `none` makes all three none. Rows and columns written as such leave the areas
// none. The form with areas is read row by row. Its strings, each of which is a component, are the areas. Its line
// names and track sizes are the rows. A row with no size is auto, and the names between two rows form one set.
bool expandGridTemplate(const Expansion* e, Items value) {
    CssValuePool* pool = poolOf(e);
    const CssValueNode* first = &pool->values.data[value.first];
    if (soleKeyword(pool, value) == PrCssKeyword_None
        || (first->kind == PrCssValueKind_Longhand
            && cssValueLonghand(first) == PrCssProperty_GridTemplateRows)) {
        if (!setComponents(e, value))
            return false;
        fillInitial(e);
        return true;
    }

    Items parts[2];
    uint32_t count = slashParts(pool, value, parts, 2);
    uint32_t rows = pool->values.count;
    bool sized = true;     // true if the last row has its size, or there is no row yet
    bool names = false;    // true inside a set of line names
    bool closed = false;   // true if the last value added is the ']' that closes a set of names
    for (uint32_t i = parts[0].first; i < parts[0].end; i += pool->values.data[i].next) {
        const CssValueNode* item = &pool->values.data[i];
        uint32_t next = item->next;
        bool string = item->kind == PrCssValueKind_Longhand;
        bool open = cssValueIsLiteral(item, '[');
        bool close = cssValueIsLiteral(item, ']');
        if (!sized && (string || open)) {
            addValue(pool, cssKeywordValue(PrCssKeyword_Auto));
            sized = true;
            closed = false;
        }
        if (string) {
            sized = false;
            continue;
        }
        if (open && closed) {
            pool->values.pop();
            closed = false;
            names = true;
            continue;
        }
        // An empty set of names is none.
        if (close && cssValueIsLiteral(&pool->values.last(), '[')) {
            pool->values.pop();
            names = false;
            closed = pool->values.count != rows
                     && cssValueIsLiteral(&pool->values.last(), ']');
            continue;
        }
        sized = sized || (!names && !open && !close);
        names = (names || open) && !close;
        closed = close;
        cssValuePoolRepeat(pool, CssValueSpan{i, next});
    }
    if (!sized)
        addValue(pool, cssKeywordValue(PrCssKeyword_Auto));
    slot(e, 0) = CssValueSpan{rows, pool->values.count - rows};

    uint32_t areas = pool->values.count;
    for (uint32_t i = parts[0].first; i < parts[0].end; i += pool->values.data[i].next) {
        if (pool->values.data[i].kind == PrCssValueKind_Longhand)
            cssValuePoolRepeat(pool, CssValueSpan{i + 1, 1});
    }
    slot(e, 2) = CssValueSpan{areas, pool->values.count - areas};
    if (!cssGridAreasFold(pool, areas, pool->values.count - areas))
        return false;
    if (count == 2)
        slot(e, 1) = spanOf(parts[1]);
    fillInitial(e);
    return true;
}

// css-grid-2/#propdef-grid: a grid-template, or the rows or columns with auto-flow on the other axis, and `dense` if
// written. The row axis is grid-auto-flow's own, which `dense` alone selects.
bool expandGrid(const Expansion* e, Items value) {
    CssValuePool* pool = poolOf(e);
    if (!setComponents(e, value))
        return false;
    uint32_t flow = value.end;
    uint32_t slash = value.end;
    bool dense = false;
    for (uint32_t i = value.first; i < value.end; i += pool->values.data[i].next) {
        const CssValueNode* item = &pool->values.data[i];
        if (cssValueIsKeyword(item, PrCssKeyword_AutoFlow))
            flow = i;
        dense = dense || cssValueIsKeyword(item, PrCssKeyword_Dense);
        if (cssValueIsLiteral(item, '/') && slash == value.end)
            slash = i;
    }
    if (flow != value.end) {
        uint32_t start = pool->values.count;
        if (flow > slash || !dense)
            addValue(pool, cssKeywordValue(flow < slash ? PrCssKeyword_Row : PrCssKeyword_Column));
        if (dense)
            addValue(pool, cssKeywordValue(PrCssKeyword_Dense));
        slot(e, indexOf(e, PrCssProperty_GridAutoFlow)) =
            CssValueSpan{start, pool->values.count - start};
    }
    fillInitial(e);
    return true;
}

bool expand(const Expansion* e, Items value) {
    switch (kCssPropertyShorthand[e->shorthand]) {
        case CssShorthand_Box:           return expandBox(e, value);
        case CssShorthand_Pair:          return expandPair(e, value);
        case CssShorthand_Mirror:        return expandMirror(e, value);
        case CssShorthand_Components:    return expandComponents(e, value);
        case CssShorthand_Layers:        return expandLayers(e, value, 0, componentEntry);
        case CssShorthand_Background:    return expandBackground(e, value);
        case CssShorthand_Mask:          return expandLayers(e, value, 0, boxEntry);
        case CssShorthand_ListStyle:     return expandListStyle(e, value);
        case CssShorthand_BorderRadius:  return expandBorderRadius(e, value);
        case CssShorthand_Flex:          return expandFlex(e, value);
        case CssShorthand_Font:          return expandComponents(e, value);
        case CssShorthand_FontSynthesis: return expandFontSynthesis(e, value);
        case CssShorthand_FontVariant:   return expandFontVariant(e, value);
        case CssShorthand_GridArea:      return expandGridArea(e, value);
        case CssShorthand_GridLine:      return expandGridLine(e, value);
        case CssShorthand_GridTemplate:  return expandGridTemplate(e, value);
        case CssShorthand_Grid:          return expandGrid(e, value);
        case CssShorthand_Place:         return expandPlace(e, value);
        case CssShorthand_TextBox:       return expandTextBox(e, value);
        case CssShorthand_WhiteSpace:    return expandWhiteSpace(e, value);
        case CssShorthand_TextAlign:     return expandTextAlign(e, value);
        case CssShorthand_GlyphOrientationVertical: return expandGlyphOrientationVertical(e, value);
        case CssShorthand_Same:          return expandSame(e, value);
        case CssShorthand_RuleInset:     return expandRuleInset(e, value);
        case CssShorthand_GapRule:       return expandGapRule(e, value);
        case CssShorthand_All:           return expandSame(e, value);
        default:                         return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Serialization

// The number of longhands that font sets (css-fonts-4/#font-prop).
constexpr uint32_t kFontLonghands = 20;

// The maximum number of longhands that a shorthand other than `all` sets, which the generator enforces.
constexpr uint32_t kLonghandsWritten = 64;

// The longhands of a shorthand being serialized, with the value and the serialization of each.
struct Longhands {
    PrCssProperty shorthand;
    CssPropertyList properties;
    const CssValueRun* values;
    const PrStringView* texts;
};

bool isInitial(const Longhands* l, uint32_t index) {
    return l->texts[index] == cssPropertyInitial(l->properties.data[index]);
}

// Returns true if the longhand at `index` serializes as `text`.
bool textIs(const Longhands* l, uint32_t index, PrStringView text) {
    return l->texts[index] == text;
}

// Appends a space if the output holds something from `start` on.
void space(TextBuffer* out, uint32_t start) {
    if (out->text.count != start)
        textAppendChar(out, ' ');
}

// Appends `text`, preceded by a space if the output holds something from `start` on.
void word(TextBuffer* out, uint32_t start, PrStringView text) {
    space(out, start);
    textAppend(out, text);
}

// A walk of a shorthand's grammar that writes the value of each component. It omits a component that the grammar allows
// omitting if its longhands hold their initial values.
struct Walk {
    const Longhands* l;
    TextBuffer* out;
    uint32_t start;     // the offset in `out` where the shorthand's value begins
    uint64_t used;      // the longhands that a component wrote, one bit per index
    uint64_t force;     // longhands that a component writes whatever they hold
    uint64_t skip;      // longhands that no component writes
    uint64_t valued;    // longhands that are not at their initial values
    bool failed;        // true if the longhands hold values that the grammar cannot write
};

// Returns the bits of the longhands that a component names: the longhand itself, or a shorthand's longhands.
uint64_t bitsOf(const Longhands* l, PrCssProperty property) {
    uint64_t bits = 0;
    CssPropertyList named =
        cssIsShorthand(property) ? cssPropertyLonghands(property) : CssPropertyList{&property, 1};
    for (uint32_t i = 0; i < named.count; ++i) {
        for (uint32_t j = 0; j < l->properties.count; ++j) {
            if (l->properties.data[j] == named.data[i])
                bits |= uint64_t(1) << j;
        }
    }
    return bits;
}

// Returns true if the longhands that a component names hold their initial values, or if none of them may be written.
bool componentInitial(const Walk* w, PrCssProperty property) {
    uint64_t bits = bitsOf(w->l, property);
    return (bits & w->force) == 0 && (bits & w->valued & ~w->skip) == 0;
}

// The longhands that the components in the subtree at `index` name, and the subset of those that have a value to write.
// Each holds one bit per index.
struct Reach {
    uint64_t named;
    uint64_t valued;
};

Reach reachOf(const Walk* w, CssGrammarIndex index) {
    Reach reach = {0, 0};
    for (CssGrammarIndex i = index; i < cssGrammarAfter(index); ++i) {
        CssGrammarNode node = kCssGrammarNodes[i];
        if (cssGrammarType(node) != CssGrammarType_Longhand)
            continue;
        PrCssProperty property = PrCssProperty(cssGrammarValue(node));
        uint64_t bits = bitsOf(w->l, property);
        reach.named |= bits;
        if (!componentInitial(w, property))
            reach.valued |= bits;
    }
    return reach;
}

// Returns true if a component in the subtree at `index` has a value to write.
bool hasValue(const Walk* w, CssGrammarIndex index) {
    return reachOf(w, index).valued != 0;
}

// Writes a component's value: the value of the longhand, or the single value that every longhand of a shorthand holds.
void writeComponent(Walk* w, PrCssProperty property) {
    uint64_t bits = bitsOf(w->l, property) & ~w->skip;
    uint32_t first = w->l->properties.count;
    for (uint32_t j = 0; j < w->l->properties.count; ++j) {
        if ((bits & (uint64_t(1) << j)) == 0)
            continue;
        if (first == w->l->properties.count)
            first = j;
        else if (w->l->texts[j] != w->l->texts[first])
            w->failed = true;
    }
    w->used |= bits;
    if (first != w->l->properties.count)
        word(w->out, w->start, w->l->texts[first]);
}

// Returns true if the component at `index` can write what its longhands hold. A component made only of keywords writes
// only those keywords. A component of any other grammar is assumed to write whatever its longhands hold. A component
// may be only a part of its longhand's grammar. For example, in vertical-align, `first | last` is part of the grammar
// of baseline-source, which also accepts `auto`.
bool canWrite(const Walk* w, CssGrammarIndex index) {
    CssGrammarNode node = kCssGrammarNodes[index];
    CssGrammarIndex inner = CssGrammarIndex(index + 1);
    if (cssGrammarType(node) != CssGrammarType_Longhand || inner >= cssGrammarAfter(index))
        return true;
    CssGrammarType type = cssGrammarType(kCssGrammarNodes[inner]);
    if (type != CssGrammarType_Keyword && type != CssGrammarType_KeywordSet)
        return true;
    uint64_t bits = bitsOf(w->l, PrCssProperty(cssGrammarValue(node)));
    for (uint32_t j = 0; j < w->l->properties.count; ++j) {
        if ((bits & (uint64_t(1) << j)) == 0)
            continue;
        bool found = false;
        for (CssGrammarIndex k = inner; k < cssGrammarAfter(inner); ++k) {
            CssGrammarNode keyword = kCssGrammarNodes[k];
            found = found
                    || (cssGrammarType(keyword) == CssGrammarType_Keyword
                        && w->l->texts[j] == cssKeywordName(PrCssKeyword(cssGrammarValue(keyword))));
        }
        if (!found)
            return false;
    }
    return true;
}

// Returns the child of the any-of at `index` that writes if none of its children has a value to write. That is the
// first child that can write what its longhands hold, or else the first child.
CssGrammarIndex anyOfFallback(const Walk* w, CssGrammarIndex index) {
    for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
         child = cssGrammarAfter(child)) {
        if (canWrite(w, child))
            return child;
    }
    return CssGrammarIndex(index + 1);
}

// Writes the subtree at `index`, which may be omitted if `omittable` is set or its multiplier allows zero repetitions.
// Returns true if it wrote anything.
bool walkNode(Walk* w, CssGrammarIndex index, bool omittable) {
    CssGrammarNode node = kCssGrammarNodes[index];
    omittable = omittable || cssGrammarModifier(node)->min == 0;
    if (omittable && !hasValue(w, index))
        return false;

    switch (cssGrammarType(node)) {
        case CssGrammarType_Longhand:
            writeComponent(w, PrCssProperty(cssGrammarValue(node)));
            return true;

        case CssGrammarType_AnyOf: {
            bool wrote = false;
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
                 child = cssGrammarAfter(child))
                wrote = walkNode(w, child, true) || wrote;
            if (!wrote)
                walkNode(w, anyOfFallback(w, index), false);
            return true;
        }

        case CssGrammarType_AllOf:
        case CssGrammarType_Sequence:
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
                 child = cssGrammarAfter(child))
                walkNode(w, child, false);
            return true;

        // Writes the first alternative that names every longhand for which any alternative has a value to write.
        case CssGrammarType_OneOf: {
            uint64_t valued = 0;
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
                 child = cssGrammarAfter(child))
                valued |= reachOf(w, child).valued;
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
                 child = cssGrammarAfter(child)) {
                Reach reach = reachOf(w, child);
                if (reach.named != 0 && (reach.named & valued) == valued)
                    return walkNode(w, child, false);
            }
            w->failed = true;
            return false;
        }

        case CssGrammarType_Literal: {
            char character = char(cssGrammarValue(node));
            word(w->out, w->start, PrStringView{&character, 1});
            return true;
        }

        // Any other node holds no longhand, so the grammar cannot write it from the longhands.
        default:
            w->failed = !omittable;
            return false;
    }
}

// Writes the shorthand's value by walking the subtree at `index`. Returns false if the longhands hold something that
// the walk cannot write: a component whose longhands differ, or a longhand that no component names and that is not at
// its initial value.
bool walkGrammar(Walk* w, CssGrammarIndex index) {
    walkNode(w, index, false);
    w->failed = w->failed || (w->valued & ~w->used & ~w->skip) != 0;
    return !w->failed;
}

Walk walkOf(const Longhands* l, TextBuffer* out) {
    uint64_t valued = 0;
    for (uint32_t j = 0; j < l->properties.count; ++j)
        valued |= isInitial(l, j) ? 0 : uint64_t(1) << j;
    return Walk{l, out, out->text.count, 0, 0, 0, valued, false};
}

// css-cascade-5/#shorthand-property, in reverse.
bool serializeComponents(const Longhands* l, TextBuffer* out) {
    Walk w = walkOf(l, out);
    return walkGrammar(&w, kCssPropertyGrammarRoot[l->shorthand]);
}

// css-box-4/#propdef-margin: writes the fewest values that give the four sides.
bool serializeBox(const Longhands* l, TextBuffer* out) {
    uint32_t count = 4;
    if (textIs(l, 3, l->texts[1])) {
        count = 3;
        if (textIs(l, 2, l->texts[0]))
            count = textIs(l, 1, l->texts[0]) ? 1 : 2;
    }
    uint32_t start = out->text.count;
    for (uint32_t i = 0; i < count; ++i)
        word(out, start, l->texts[i]);
    return true;
}

// css-logical-1/#propdef-margin-block: writes one value if the two are the same.
bool serializePair(const Longhands* l, TextBuffer* out) {
    uint32_t start = out->text.count;
    word(out, start, l->texts[0]);
    if (!textIs(l, 1, l->texts[0]))
        word(out, start, l->texts[1]);
    return true;
}

// Returns the number of entries in a comma-separated list.
uint32_t countEntries(CssValueRun run) {
    uint32_t count = 1;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next)
        count += cssValueIsLiteral(&run.values[i], ',') ? 1 : 0;
    return count;
}

// Returns entry `k` of a comma-separated list.
CssValueRun entryOf(CssValueRun run, uint32_t k) {
    uint32_t first = 0;
    uint32_t i = 0;
    for (; i < run.count; i += run.values[i].next) {
        if (!cssValueIsLiteral(&run.values[i], ','))
            continue;
        if (k == 0)
            break;
        --k;
        first = i + 1;
    }
    return CssValueRun{run.values + first, i - first, run.text};
}

// The longhands of one layer: the entry of each longhand that a component sets, and the value of each longhand that
// takes a single entry, or none, on every layer. After layerOf, `texts` holds their serializations.
struct Layer {
    TextBuffer text;
    Array<uint32_t> ends;
    Array<PrStringView> texts;
};

void layerOf(Layer* layer, const Longhands* l, uint64_t listed, uint32_t k) {
    textClear(&layer->text);
    layer->ends.count = 0;
    layer->texts.count = 0;
    for (uint32_t j = 0; j < l->properties.count; ++j) {
        bool entry = (listed & (uint64_t(1) << j)) != 0;
        cssSerializeValues(&layer->text, entry ? entryOf(l->values[j], k) : l->values[j]);
        layer->ends.push(layer->text.text.count);
    }
    for (uint32_t j = 0; j < l->properties.count; ++j) {
        uint32_t from = j == 0 ? 0 : layer->ends.data[j - 1];
        layer->texts.push(PrStringView{layer->text.text.data + from, layer->ends.data[j] - from});
    }
}

void layerReset(Layer* layer) {
    textReset(&layer->text);
    layer->ends.reset();
    layer->texts.reset();
}

// Returns the grammar that a layer is walked with: the root, or, for background, the grammar of its final layer or of
// the other layers.
CssGrammarIndex layerGrammar(PrCssProperty shorthand, bool final) {
    CssGrammarIndex root = kCssPropertyGrammarRoot[shorthand];
    if (cssGrammarType(kCssGrammarNodes[root]) != CssGrammarType_Sequence)
        return root;
    CssGrammarIndex layers = CssGrammarIndex(root + 1);
    return final ? cssGrammarAfter(cssGrammarAfter(layers)) : layers;
}

// Returns the index of `property` among the longhands.
uint32_t longhandIndex(const Longhands* l, PrCssProperty property) {
    uint32_t i = 0;
    while (l->properties.data[i] != property)
        ++i;
    return i;
}

// css-backgrounds-4/#background, css-masking-1/#the-mask: one box is both the origin and the clip. A clip that is not a
// box is written alone if the origin is border-box.
void writeBoxes(Walk* w) {
    const Longhands* l = w->l;
    Boxes boxes = boxesOf(l->shorthand);
    uint32_t origin = longhandIndex(l, boxes.origin);
    uint32_t clip = longhandIndex(l, boxes.clip);
    // The clip values that no origin accepts, which are written alone.
    static constexpr PrStringView kClipOnly[] = {"border-area", "text", "border-area text", "no-clip"};
    bool clipOnly = false;
    for (PrStringView only : kClipOnly)
        clipOnly = clipOnly || textIs(l, clip, only);
    if (textIs(l, clip, l->texts[origin])) {
        w->skip |= uint64_t(1) << clip;
        if (!isInitial(l, origin))
            w->force |= uint64_t(1) << origin;
    } else if (textIs(l, origin, "border-box") && clipOnly) {
        w->skip |= uint64_t(1) << origin;
        w->force |= uint64_t(1) << clip;
    } else if (!isInitial(l, origin) || !isInitial(l, clip)) {
        w->force |= (uint64_t(1) << origin) | (uint64_t(1) << clip);
    }
}

// css-backgrounds-3/#layering: each layer is written by the grammar, and the layers are joined with commas. Every
// longhand that a component sets has the same number of entries, and the others hold their initial values. The
// longhands in `last` belong to the final layer only. `boxes` means the layers have an origin and a clip that one box
// can write.
bool serializeLayers(const Longhands* l, TextBuffer* out, uint64_t last, bool boxes) {
    uint64_t components = componentsOf(l->shorthand, l->properties);
    uint64_t listed = components & ~last;
    uint32_t layers = 0;
    for (uint32_t j = 0; j < l->properties.count; ++j) {
        uint64_t bit = uint64_t(1) << j;
        if ((components & bit) == 0 && !isInitial(l, j))
            return false;
        if ((listed & bit) == 0)
            continue;
        uint32_t count = countEntries(l->values[j]);
        if (layers != 0 && count != layers)
            return false;
        layers = count;
    }

    Layer layer = {};
    bool written = true;
    for (uint32_t k = 0; k < layers && written; ++k) {
        bool final = k + 1 == layers;
        layerOf(&layer, l, listed, k);
        Longhands entries = {l->shorthand, l->properties, l->values, layer.texts.data};
        if (k != 0)
            textAppend(out, ", ");
        Walk w = walkOf(&entries, out);
        if (boxes)
            writeBoxes(&w);
        if (!final)
            w.skip |= last;
        written = walkGrammar(&w, layerGrammar(l->shorthand, final));
    }
    layerReset(&layer);
    return written;
}

bool serializeLonghands(const Longhands* l, TextBuffer* out);

// css-logical-1/#propdef-border-block: every run of longhands holds the same values as the first run, which is written
// as the property that the grammar names.
bool serializeMirror(const Longhands* l, TextBuffer* out) {
    CssGrammarIndex root = kCssPropertyGrammarRoot[l->shorthand];
    PrCssProperty named = PrCssProperty(cssGrammarValue(kCssGrammarNodes[root]));
    uint32_t run = cssIsShorthand(named) ? cssPropertyLonghands(named).count : 1;
    for (uint32_t j = run; j < l->properties.count; ++j) {
        if (!textIs(l, j, l->texts[j % run]))
            return false;
    }
    if (!cssIsShorthand(named)) {
        textAppend(out, l->texts[0]);
        return true;
    }
    Longhands first = {named, cssPropertyLonghands(named), l->values, l->texts};
    return serializeLonghands(&first, out);
}

// Writes one of a corner's radii: the horizontal radius, or the vertical radius. If the corner holds one value, the
// vertical radius is the horizontal radius.
void writeRadius(TextBuffer* out, const Longhands* l, uint32_t corner, bool vertical) {
    CssValueRun run = l->values[corner];
    uint32_t second = run.values[0].next;
    if (second >= run.count) {
        textAppend(out, l->texts[corner]);
        return;
    }
    uint32_t at = vertical ? second : 0;
    uint32_t count = vertical ? run.count - second : second;
    cssSerializeValues(out, CssValueRun{run.values + at, count, run.text});
}

// css-backgrounds-3/#propdef-border-radius: writes the horizontal radii in the same form as the four sides of a box, or
// as a pair for one side's two corners, then the vertical radii after a '/' if they differ.
bool serializeBorderRadius(const Longhands* l, TextBuffer* out) {
    uint32_t corners = l->properties.count;
    TextBuffer axes[2][4] = {};
    PrStringView radii[2][4];
    for (uint32_t axis = 0; axis < 2; ++axis) {
        for (uint32_t corner = 0; corner < corners; ++corner) {
            writeRadius(&axes[axis][corner], l, corner, axis == 1);
            radii[axis][corner] = textView(&axes[axis][corner]);
        }
    }
    Longhands across = {l->shorthand, l->properties, l->values, radii[0]};
    Longhands down = {l->shorthand, l->properties, l->values, radii[1]};
    bool (*serializeAxis)(const Longhands*, TextBuffer*) = corners == 4 ? serializeBox : serializePair;
    serializeAxis(&across, out);
    bool same = true;
    for (uint32_t corner = 0; corner < corners; ++corner)
        same = same && radii[0][corner] == radii[1][corner];
    if (!same) {
        textAppend(out, " / ");
        serializeAxis(&down, out);
    }
    for (uint32_t axis = 0; axis < 2; ++axis) {
        for (uint32_t corner = 0; corner < corners; ++corner)
            textReset(&axes[axis][corner]);
    }
    return true;
}

// css-flexbox-1/#propdef-flex: writes all three values.
bool serializeFlex(const Longhands* l, TextBuffer* out) {
    uint32_t start = out->text.count;
    for (uint32_t j = 0; j < 3; ++j)
        word(out, start, l->texts[j]);
    return true;
}

// Returns the <font-width-css3> keyword for a font-width, using the percentage that css-fonts-4/#font-width-prop gives
// each keyword. Returns the null view if no keyword matches.
PrStringView fontWidthKeyword(CssValueRun width, PrStringView text) {
    struct FontWidth {
        double percentage;
        PrStringView keyword;
    };
    static constexpr FontWidth kWidths[] = {
        {50, "ultra-condensed"}, {62.5, "extra-condensed"}, {75, "condensed"},
        {87.5, "semi-condensed"}, {100, "normal"}, {112.5, "semi-expanded"},
        {125, "expanded"}, {150, "extra-expanded"}, {200, "ultra-expanded"},
    };
    const CssValueNode* value = &width.values[0];
    if (value->kind == PrCssValueKind_Keyword)
        return text;

    // A math function whose calculation simplifies to one percentage is that percentage
    // (css-values-4/#calc-simplification).
    if (value->kind == PrCssValueKind_Math && width.count == 2)
        value = &width.values[1];
    for (const FontWidth& known : kWidths) {
        if (value->kind == PrCssValueKind_Percentage && value->number == known.percentage)
            return known.keyword;
    }
    return PrStringView{};
}

// css-fonts-4/#propdef-font: requires the longhands that the grammar resets to be at their initial values, and a
// font-variant-caps that font-variant can write. Writes the longhands that the grammar writes, and writes a font-width
// as its keyword if it has one.
bool serializeFont(const Longhands* l, TextBuffer* out) {
    PrStringView texts[kFontLonghands];
    PR_ASSERT(l->properties.count == kFontLonghands);
    for (uint32_t j = 0; j < l->properties.count; ++j) {
        PrCssProperty property = l->properties.data[j];
        texts[j] = l->texts[j];
        if (property == PrCssProperty_FontVariantCaps && !isInitial(l, j) && !textIs(l, j, "small-caps"))
            return false;
        if (property == PrCssProperty_FontWidth)
            texts[j] = fontWidthKeyword(l->values[j], l->texts[j]);
        if (texts[j].data == nullptr)
            return false;
    }
    Longhands font = {l->shorthand, l->properties, l->values, texts};
    return serializeComponents(&font, out);
}

// css-fonts-4/#propdef-font-synthesis: writes the keyword of each longhand that is auto, or none if all are none. A
// longhand at any other value cannot be written.
bool serializeFontSynthesis(const Longhands* l, TextBuffer* out) {
    static constexpr PrStringView kKeyword[] = {"weight", "style", "small-caps", "position"};
    uint32_t start = out->text.count;
    for (uint32_t j = 0; j < 4; ++j) {
        if (textIs(l, j, "auto"))
            word(out, start, kKeyword[j]);
        else if (!textIs(l, j, "none"))
            return false;
    }
    if (out->text.count == start)
        textAppend(out, "none");
    return true;
}

// css-fonts-4/#propdef-font-variant: writes `normal` if every longhand is normal, and `none` if the ligatures are none
// and the rest are normal. Otherwise writes each longhand that is not normal.
bool serializeFontVariant(const Longhands* l, TextBuffer* out) {
    bool rest = true;
    for (uint32_t j = 1; j < l->properties.count; ++j)
        rest = rest && isInitial(l, j);
    if (textIs(l, 0, "none")) {
        if (!rest)
            return false;
        textAppend(out, "none");
        return true;
    }
    return serializeComponents(l, out);
}

// Returns true if omitting a grid line would give it the value that `end` holds, given `start`
// (css-grid-2/#propdef-grid-row).
bool omittedLine(const Longhands* l, uint32_t start, uint32_t end) {
    CssValueRun run = l->values[start];
    bool ident = run.count == 1 && run.values[0].kind == PrCssValueKind_Ident;
    return textIs(l, end, ident ? l->texts[start] : "auto");
}

// css-grid-2/#propdef-grid-row: writes the start, then the end after a '/', unless omitting the end gives the same
// value.
bool serializeGridLine(const Longhands* l, TextBuffer* out) {
    textAppend(out, l->texts[0]);
    if (!omittedLine(l, 0, 1)) {
        textAppend(out, " / ");
        textAppend(out, l->texts[1]);
    }
    return true;
}

// css-grid-2/#propdef-grid-area: writes the lines up to the last line that differs from the value that omitting it
// gives.
bool serializeGridArea(const Longhands* l, TextBuffer* out) {
    static constexpr uint8_t kFrom[4] = {0, 0, 0, 1};
    uint32_t count = 4;
    while (count > 1 && omittedLine(l, kFrom[count - 1], count - 1))
        --count;
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            textAppend(out, " / ");
        textAppend(out, l->texts[i]);
    }
    return true;
}

// css-inline-3/#propdef-text-box: writes `normal` for `none auto`. Otherwise writes each longhand that differs from the
// value that omitting it gives.
bool serializeTextBox(const Longhands* l, TextBuffer* out) {
    bool none = textIs(l, 0, "none");
    bool automatic = isInitial(l, 1);
    if (none && automatic) {
        textAppend(out, "normal");
        return true;
    }
    uint32_t start = out->text.count;
    if (!textIs(l, 0, "trim-both") || automatic || none)
        word(out, start, l->texts[0]);
    if (!automatic)
        word(out, start, l->texts[1]);
    return true;
}

// css-text-4/#propdef-white-space: writes the keyword that expands to the longhands, or else the longhands.
bool serializeWhiteSpace(const Longhands* l, TextBuffer* out) {
    struct WhiteSpaceKeyword {
        PrStringView keyword;
        PrStringView collapse;
        PrStringView wrap;
    };
    static constexpr WhiteSpaceKeyword kKeywords[] = {
        {"normal", "collapse", "wrap"},
        {"pre", "preserve", "nowrap"},
        {"pre-wrap", "preserve", "wrap"},
        {"pre-line", "preserve-breaks", "wrap"},
    };
    if (isInitial(l, 2)) {
        for (const WhiteSpaceKeyword& keyword : kKeywords) {
            if (textIs(l, 0, keyword.collapse) && textIs(l, 1, keyword.wrap)) {
                textAppend(out, keyword.keyword);
                return true;
            }
        }
    }
    return serializeComponents(l, out);
}

// css-writing-modes-4/#glyph-orientation: writes the value that sets the longhand's text-orientation.
bool serializeGlyphOrientationVertical(const Longhands* l, TextBuffer* out) {
    struct GlyphOrientation {
        PrStringView orientation;
        PrStringView keyword;
    };
    static constexpr GlyphOrientation kKeywords[] = {
        {"mixed", "auto"},
        {"upright", "0deg"},
        {"sideways", "90deg"},
    };
    for (const GlyphOrientation& keyword : kKeywords) {
        if (textIs(l, 0, keyword.orientation)) {
            textAppend(out, keyword.keyword);
            return true;
        }
    }
    return false;
}

// css-text-3/#propdef-text-align: writes text-align-all if text-align-last is auto, justify-all if both are justify,
// and match-parent if both are match-parent. No other pair has a value of the shorthand.
bool serializeTextAlign(const Longhands* l, TextBuffer* out) {
    if (textIs(l, 1, "auto")) {
        if (textIs(l, 0, "match-parent"))
            return false;
        textAppend(out, l->texts[0]);
        return true;
    }
    if (textIs(l, 0, "justify") && textIs(l, 1, "justify")) {
        textAppend(out, "justify-all");
        return true;
    }
    if (textIs(l, 0, "match-parent") && textIs(l, 1, "match-parent")) {
        textAppend(out, "match-parent");
        return true;
    }
    return false;
}

// css-grid-2/#propdef-grid-template: writes `none`, the rows and columns, or the areas form. The areas form writes each
// row's size after its string, and the line names in their positions.
bool serializeGridTemplate(const Longhands* l, TextBuffer* out) {
    if (isInitial(l, 2)) {
        if (isInitial(l, 0) && isInitial(l, 1)) {
            textAppend(out, "none");
            return true;
        }
        textAppend(out, l->texts[0]);
        textAppend(out, " / ");
        textAppend(out, l->texts[1]);
        return true;
    }

    CssValueRun rows = l->values[0];
    CssValueRun areas = l->values[2];
    uint32_t area = 0;
    uint32_t start = out->text.count;
    for (uint32_t i = 0; i < rows.count; i += rows.values[i].next) {
        const CssValueNode* item = &rows.values[i];
        space(out, start);
        if (cssValueIsLiteral(item, '[')) {
            uint32_t close = i;
            while (!cssValueIsLiteral(&rows.values[close], ']'))
                ++close;
            cssSerializeValues(out, CssValueRun{item, close + 1 - i, rows.text});
            i = close;
            continue;
        }
        if (area == areas.count || cssValueIsFunction(item, PrCssKeyword_Repeat))
            return false;
        cssSerializeValues(out, CssValueRun{areas.values + area, 1, areas.text});
        ++area;
        if (!cssValueIsKeyword(item, PrCssKeyword_Auto)) {
            textAppendChar(out, ' ');
            cssSerializeValues(out, CssValueRun{item, item->next, rows.text});
        }
    }
    if (area != areas.count)
        return false;
    if (!isInitial(l, 1)) {
        textAppend(out, " / ");
        textAppend(out, l->texts[1]);
    }
    return true;
}

// css-grid-2/#propdef-grid: writes a grid-template if the auto longhands are initial. Otherwise writes the rows or the
// columns, with auto-flow along the other axis.
bool serializeGrid(const Longhands* l, TextBuffer* out) {
    PrStringView flow = l->texts[5];
    bool autoRows = isInitial(l, 3);
    bool autoColumns = isInitial(l, 4);
    if (isInitial(l, 5) && autoRows && autoColumns) {
        Longhands grid = {PrCssProperty_GridTemplate, cssPropertyLonghands(PrCssProperty_GridTemplate),
                          l->values, l->texts};
        return serializeGridTemplate(&grid, out);
    }
    if (!isInitial(l, 2))
        return false;
    bool dense = stringEndsWith(flow, "dense");
    bool column = stringStartsWith(flow, "column");
    if (column && isInitial(l, 1) && autoRows) {
        textAppend(out, l->texts[0]);
        textAppend(out, " / auto-flow");
        if (dense)
            textAppend(out, " dense");
        if (!autoColumns) {
            textAppendChar(out, ' ');
            textAppend(out, l->texts[4]);
        }
        return true;
    }
    if (!column && isInitial(l, 0) && autoColumns) {
        textAppend(out, "auto-flow");
        if (dense)
            textAppend(out, " dense");
        if (!autoRows) {
            textAppendChar(out, ' ');
            textAppend(out, l->texts[3]);
        }
        textAppend(out, " / ");
        textAppend(out, l->texts[1]);
        return true;
    }
    return false;
}

// css-gaps-1/#propdef-column-rule-inset-start: writes the single value that every longhand holds.
bool serializeSame(const Longhands* l, TextBuffer* out) {
    for (uint32_t j = 1; j < l->properties.count; ++j) {
        if (!textIs(l, j, l->texts[0]))
            return false;
    }
    textAppend(out, l->texts[0]);
    return true;
}

// css-gaps-1/#propdef-column-rule-inset: writes one value if the four insets are the same. Otherwise writes all four:
// the cap insets, then the junction insets after a '/'.
bool serializeRuleInset(const Longhands* l, TextBuffer* out) {
    if (serializeSame(l, out))
        return true;
    for (uint32_t j = 0; j < 4; ++j) {
        if (j != 0)
            textAppend(out, j == 2 ? " / " : " ");
        textAppend(out, l->texts[j]);
    }
    return true;
}

// Reads into `entries` the next entry of each of the three lists of a gap rule shorthand, starting at `at`. Returns
// false past the last entry of the first list.
bool nextEntries(const Longhands* l, uint32_t* at, CssValueRun* entries) {
    if (at[0] > l->values[0].count)
        return false;
    for (uint32_t j = 0; j < 3; ++j) {
        CssValueRun run = l->values[j];
        uint32_t i = at[j];
        while (i < run.count && !cssValueIsLiteral(&run.values[i], ','))
            i += run.values[i].next;
        entries[j] = i >= at[j] ? CssValueRun{run.values + at[j], i - at[j], run.text}
                                : CssValueRun{nullptr, 0, nullptr};
        at[j] = i + 1;
    }
    return true;
}

// Writes one <gap-rule>: the width, style and color that are not at their initial values, or the width if all three are
// at their initial values.
void writeGapRule(const Longhands* l, const CssValueRun* entries, TextBuffer* out) {
    TextBuffer texts[3] = {};
    uint32_t start = out->text.count;
    for (uint32_t j = 0; j < 3; ++j) {
        cssSerializeValues(&texts[j], entries[j]);
        if (textView(&texts[j]) != cssPropertyInitial(l->properties.data[j]))
            word(out, start, textView(&texts[j]));
    }
    if (out->text.count == start)
        textAppend(out, textView(&texts[0]));
    for (TextBuffer& text : texts)
        textReset(&text);
}

// css-gaps-1/#propdef-column-rule: writes the lists entry by entry, if they have the same shape.
bool serializeGapRule(const Longhands* l, TextBuffer* out) {
    uint32_t at[3] = {0, 0, 0};
    CssValueRun entries[3];
    while (nextEntries(l, at, entries)) {
        bool repeat[3];
        for (uint32_t j = 0; j < 3; ++j) {
            if (entries[j].count == 0)
                return false;
            repeat[j] = cssValueIsFunction(&entries[j].values[0], PrCssKeyword_Repeat);
        }
        if (repeat[0] != repeat[1] || repeat[0] != repeat[2])
            return false;
        if (!repeat[0]) {
            writeGapRule(l, entries, out);
        } else {
            // repeat(): the count, then each rule that it repeats.
            Longhands inner = {l->shorthand, l->properties, nullptr, l->texts};
            CssValueRun arguments[3];
            for (uint32_t j = 0; j < 3; ++j)
                arguments[j] = CssValueRun{entries[j].values + 1, entries[j].values[0].next - 1,
                                           entries[j].text};
            inner.values = arguments;
            uint32_t within[3] = {0, 0, 0};
            CssValueRun parts[3];
            textAppend(out, "repeat(");
            for (uint32_t k = 0; nextEntries(&inner, within, parts); ++k) {
                if (k == 0) {
                    const CssValueNode* count = parts[0].values;
                    for (uint32_t j = 1; j < 3; ++j) {
                        const CssValueNode* other = parts[j].values;
                        if (other->kind != count->kind || other->aux != count->aux
                            || other->number != count->number)
                            return false;
                    }
                    cssSerializeValues(out, parts[0]);
                    continue;
                }
                textAppend(out, ", ");
                writeGapRule(l, parts, out);
            }
            textAppendChar(out, ')');
        }
        if (at[0] <= l->values[0].count)
            textAppend(out, ", ");
    }
    return at[1] > l->values[1].count && at[2] > l->values[2].count;
}

// Writes the shorthand's value from its longhands' values, according to its form. Returns false if it cannot.
bool serializeLonghands(const Longhands* l, TextBuffer* out) {
    switch (kCssPropertyShorthand[l->shorthand]) {
        case CssShorthand_Box:           return serializeBox(l, out);
        case CssShorthand_Pair:          return serializePair(l, out);
        case CssShorthand_Mirror:        return serializeMirror(l, out);
        case CssShorthand_Components:    return serializeComponents(l, out);
        case CssShorthand_Layers:        return serializeLayers(l, out, 0, false);
        case CssShorthand_Background:
            return serializeLayers(
                l, out, uint64_t(1) << longhandIndex(l, PrCssProperty_BackgroundColor), true);
        case CssShorthand_Mask:          return serializeLayers(l, out, 0, true);
        case CssShorthand_ListStyle:     return serializeComponents(l, out);
        case CssShorthand_BorderRadius:  return serializeBorderRadius(l, out);
        case CssShorthand_Flex:          return serializeFlex(l, out);
        case CssShorthand_Font:          return serializeFont(l, out);
        case CssShorthand_FontSynthesis: return serializeFontSynthesis(l, out);
        case CssShorthand_FontVariant:   return serializeFontVariant(l, out);
        case CssShorthand_GridArea:      return serializeGridArea(l, out);
        case CssShorthand_GridLine:      return serializeGridLine(l, out);
        case CssShorthand_GridTemplate:  return serializeGridTemplate(l, out);
        case CssShorthand_Grid:          return serializeGrid(l, out);
        case CssShorthand_Place:         return serializePair(l, out);
        case CssShorthand_TextBox:       return serializeTextBox(l, out);
        case CssShorthand_WhiteSpace:    return serializeWhiteSpace(l, out);
        case CssShorthand_TextAlign:     return serializeTextAlign(l, out);
        case CssShorthand_GlyphOrientationVertical: return serializeGlyphOrientationVertical(l, out);
        case CssShorthand_Same:          return serializeSame(l, out);
        case CssShorthand_RuleInset:     return serializeRuleInset(l, out);
        case CssShorthand_GapRule:       return serializeGapRule(l, out);
        default:                         return false;
    }
}

// Returns the CSS-wide keyword if the run is one, or PrCssKeyword_Count otherwise.
PrCssKeyword cssWideKeyword(CssValueRun run) {
    const CssValueNode* value = &run.values[0];
    bool wide = run.count == 1 && value->kind == PrCssValueKind_Keyword
                && cssIsCssWideKeyword(cssValueKeyword(value));
    return wide ? cssValueKeyword(value) : PrCssKeyword_Count;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Expansion

bool cssShorthandPending(PrCssProperty shorthand, CssValueRun value) {
    const CssValueNode* first = &value.values[0];
    return first->kind == PrCssValueKind_Tokens
           || (kCssPropertyShorthand[shorthand] == CssShorthand_Font
               && first->kind == PrCssValueKind_Keyword);
}

bool cssExpandShorthand(CssParser* parser, PrCssProperty shorthand, CssValueSpan value) {
    Expansion e = {parser, shorthand, cssPropertyLonghands(shorthand), 0};
    parser->expanded.count = 0;
    parser->expanded.reserve(e.longhands.count);
    for (uint32_t i = 0; i < e.longhands.count; ++i)
        parser->expanded.push(kUnset);
    return expand(&e, Items{value.start, value.start + value.count});
}

// ---------------------------------------------------------------------------------------------------------------------
// Serialization

void cssSerializeShorthand(TextBuffer* out, PrCssProperty shorthand, const CssValueRun* values) {
    CssPropertyList properties = cssPropertyLonghands(shorthand);

    // css-cascade-5/#shorthand-property: longhands that all hold one CSS-wide keyword serialize as that keyword. A
    // shorthand cannot write a CSS-wide keyword that only some of its longhands hold.
    PrCssKeyword wide = cssWideKeyword(values[0]);
    for (uint32_t i = 0; i < properties.count; ++i) {
        PrCssKeyword keyword = cssWideKeyword(values[i]);
        if (keyword != wide || values[i].values[0].kind == PrCssValueKind_Tokens)
            return;
    }
    if (wide != PrCssKeyword_Count) {
        textAppend(out, cssKeywordName(wide));
        return;
    }
    if (kCssPropertyShorthand[shorthand] == CssShorthand_All)
        return;

    // Every longhand's serialization, concatenated in `texts`.
    TextBuffer texts;
    uint32_t ends[kLonghandsWritten];
    PrStringView views[kLonghandsWritten];
    PR_ASSERT(properties.count <= kLonghandsWritten);
    for (uint32_t i = 0; i < properties.count; ++i) {
        cssSerializeValues(&texts, values[i]);
        ends[i] = texts.text.count;
    }
    for (uint32_t i = 0; i < properties.count; ++i) {
        uint32_t from = i == 0 ? 0 : ends[i - 1];
        views[i] = PrStringView{texts.text.data + from, ends[i] - from};
    }

    Longhands l = {shorthand, properties, values, views};
    uint32_t start = out->text.count;
    if (!serializeLonghands(&l, out))
        out->text.count = start;
    textReset(&texts);
}

} // namespace pr_impl
