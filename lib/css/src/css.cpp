// CSS object model C API.
//
// cssom-1 - https://drafts.csswg.org/cssom/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/

#include "pr/css.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/string_ops.h"

#include "css/src/css_color.h"
#include "css/src/css_color_value.h"
#include "css/src/css_counter.h"
#include "css/src/css_filter.h"
#include "css/src/css_font_face.h"
#include "css/src/css_highlight.h"
#include "css/src/css_inline_style.h"
#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "css/src/css_media.h"
#include "css/src/css_object.h"
#include "css/src/css_parser.h"
#include "css/src/css_root.h"
#include "css/src/css_selector_match.h"
#include "css/src/css_serialize.h"
#include "css/src/css_shorthand.h"
#include "css/src/css_supports.h"
#include "css/src/css_transform.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_geometry.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_range.h"

namespace pr_impl {

namespace {

// Returns what `serialize` writes, as a string.
template <typename Serialize>
PrString serialized(Serialize serialize) {
    PrTextBuffer buffer = {};
    serialize(&buffer);
    return prTextBufferTake(&buffer);
}

// Returns the data of the rule behind `handle`, or null if there is none.
template <typename Handle>
CssRuleData* PR_NULLABLE dataOf(const Handle* PR_NULLABLE handle) {
    const PrCssRule* rule = handle;
    return rule != nullptr ? rule->data : nullptr;
}

// Returns the data of the sheet that holds the rule, or null if there is none.
const CssSheetData* PR_NULLABLE sheetDataOf(const PrCssRule* rule) {
    return rule->sheet != nullptr ? rule->sheet->data : nullptr;
}

// Commits `edit`, which was prepared against the data that the object presents and returned `result`. If the edit
// changes the data, commits it to data that the object owns. Then ends the edit. A style declaration's owner is
// notified of the change.
CssEditResult commitRule(PrCssRule* rule, CssEdit* edit, CssEditResult result) {
    if (result == CssEditResult_Changed)
        cssRuleCommit(edit, cssRuleWritable(rule));
    cssEditEnd(edit);
    return result;
}

CssEditResult commitRuleBlock(PrCssRule* rule, CssEdit* edit, CssEditResult result) {
    if (result == CssEditResult_Changed)
        cssDeclarationBlockCommit(edit, &cssRuleWritable(rule)->style);
    cssEditEnd(edit);
    return result;
}

CssEditResult commitDeclarations(PrCssStyleDeclaration* declaration, CssEdit* edit, CssEditResult result) {
    if (result == CssEditResult_Changed) {
        cssDeclarationBlockCommit(edit, cssStyleDeclarationWritable(declaration));
        cssStyleDeclarationUpdateOwner(declaration);
    }
    cssEditEnd(edit);
    return result;
}

CssEditResult commitMedia(PrCssMediaList* list, CssEdit* edit, CssEditResult result) {
    if (result == CssEditResult_Changed)
        cssMediaListCommit(edit, cssMediaListWritable(list));
    cssEditEnd(edit);
    return result;
}

// Sets the prelude of `rule` to what `text` holds. Returns false if `text` is not a prelude that the rule accepts
// (cssRulePreparePrelude).
bool setPrelude(PrCssRule* rule, PrStringView text) {
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    return commitRule(rule, &edit, cssRulePreparePrelude(&edit, rule->data, sheetDataOf(rule), text)) !=
        CssEditResult_Rejected;
}

// Returns the serialization of the prelude of the rule behind a handle.
template <typename Handle>
PrString preludeText(const Handle* handle) {
    const CssRuleData* rule = dataOf(handle);
    return serialized([&](PrTextBuffer* out) { cssSerializePrelude(out, rule); });
}

// Returns the rule behind `handle` as a `Handle`, or null if `kind` does not describe it.
template <typename Handle>
Handle* PR_NULLABLE ruleOfKind(PrCssRule* PR_NULLABLE handle, PrCssRuleKind kind) {
    return handle != nullptr && prCssRuleKind(handle) == kind ? static_cast<Handle*>(handle) : nullptr;
}

// Returns the block behind `handle` as a `Handle`, or null if it is not the declarations of `atRule`.
template <typename Handle>
Handle* PR_NULLABLE blockOf(PrCssStyleDeclaration* PR_NULLABLE handle, CssAtRule atRule) {
    return handle != nullptr && cssStyleDeclarationBlock(handle)->rule == atRule ? static_cast<Handle*>(handle) : nullptr;
}

// Returns the serialization of the values of `run`.
PrString serializedValues(CssValueRun run) {
    return serialized([&](PrTextBuffer* out) { cssSerializeValues(out, run); });
}

// Returns the prelude of the rule behind a handle.
template <typename Handle>
CssValueRun preludeOf(const Handle* handle) {
    return cssValueListRun(&dataOf(handle)->prelude);
}

// Returns the name that a rule's prelude starts with: the text of its first value that has text.
template <typename Handle>
PrStringView preludeName(const Handle* handle) {
    CssValueRun run = preludeOf(handle);
    const CssValueNode* value = run.values;
    while (!cssValueHasText(value->kind))
        ++value;
    return cssValueText(run, value);
}

// Returns the index of the first top-level value of `run` that is a match of `production`, or the run's count.
uint32_t findProduction(CssValueRun run, CssValueDef production) {
    uint32_t i = 0;
    while (i < run.count && !cssValueIsProduction(&run.values[i], production))
        i += run.values[i].next;
    return i;
}

// Writes what `make` produces from the index of each top-level value of `run` that `matches`, up to `capacity` of them.
// Returns how many there are.
template <typename Item, typename Matches, typename Make>
uint32_t eachMatching(CssValueRun run, Item* PR_NULLABLE items, uint32_t capacity, Matches matches, Make make) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (!matches(&run.values[i]))
            continue;
        if (items != nullptr && count < capacity)
            items[count] = make(i);
        ++count;
    }
    return count;
}

// Writes to `items` the serialization of each top-level value of `run` that is a match of `production` (eachMatching).
uint32_t serializeEach(CssValueRun run, CssValueDef production, PrString* PR_NULLABLE items, uint32_t capacity) {
    return eachMatching(run, items, capacity,
        [&](const CssValueNode* value) { return cssValueIsProduction(value, production); },
        [&](uint32_t i) { return serializedValues(cssValueSubtree(run, i)); });
}

// Writes to `items` the text of each top-level value of `run` of `kind` (eachMatching).
uint32_t textOfEach(CssValueRun run, PrCssValueKind kind, PrStringView* PR_NULLABLE items, uint32_t capacity) {
    return eachMatching(run, items, capacity, [&](const CssValueNode* value) { return value->kind == kind; },
        [&](uint32_t i) { return cssValueText(run, &run.values[i]); });
}

// Returns the <container-name> or the <container-query> of the <container-condition> at `index` of an @container rule's
// prelude, or the empty string if the condition has none.
PrString conditionPart(CssValueRun run, uint32_t index, CssValueDef production) {
    CssValueRun condition = cssValueChildren(run, index);
    uint32_t part = findProduction(condition, production);
    return part != condition.count ? serializedValues(cssValueSubtree(condition, part)) : prStringEmpty();
}

// Returns the part of an @container rule's condition if it has one condition, or the empty string if it has several
// (css-conditional-5/#dom-csscontainerrule-containername).
PrString soleConditionPart(const PrCssContainerRule* rule, CssValueDef production) {
    CssValueRun run = preludeOf(rule);
    return run.values[0].next == run.count ? conditionPart(run, 0, production) : prStringEmpty();
}

// Returns the declaration of `descriptor` in the rule's block, or null if the block declares none.
const CssDeclaration* PR_NULLABLE declarationOf(const CssRuleData* rule, PrCssDescriptor descriptor) {
    uint32_t index = cssDeclarationBlockFind(&rule->style, cssDescriptorDeclaredName(descriptor));
    return index != rule->style.count ? &cssDeclarations(&rule->style)[index] : nullptr;
}

// Returns the serialization of the value of the declaration of `descriptor` in the rule's block, or the empty string if
// the block declares none.
template <typename Handle>
PrString descriptorValue(const Handle* handle, PrCssDescriptor descriptor) {
    const CssRuleData* rule = dataOf(handle);
    const CssDeclaration* declaration = declarationOf(rule, descriptor);
    if (declaration == nullptr)
        return prStringEmpty();
    return serialized([&](PrTextBuffer* out) { cssSerializeDeclarationValue(out, &rule->style, declaration); });
}

// Returns the selectors of an @scope rule's scoping root, or of its limit, or the null string if the rule has none. The
// prelude is a <scope-boundaries>: the root's selectors in parentheses if written, then `to` and the limit's selectors
// in parentheses if written. The rule's selectors are the root's, then the limit's (cssCompilePreludeSelectors).
PrString scopeSelectors(const PrCssScopeRule* rule, bool limit) {
    const CssRuleData* data = dataOf(rule);
    CssValueRun run = cssValueListRun(&data->prelude);
    uint32_t counts[2] = {0, 0};    // number of selectors of the root and of the limit
    bool written[2] = {false, false};
    bool past = false;              // true once past the `to`
    for (uint32_t i = 0; i < run.count; ++i) {
        const CssValueNode* value = &run.values[i];
        past = past || cssValueIsKeyword(value, PrCssKeyword_To);
        written[past] = written[past] || cssValueIsLiteral(value, '(');
        if (cssIsComplexSelectorNode(value)) {
            ++counts[past];
            i += value->next - 1;
        }
    }
    if (!written[limit] || data->selectors == nullptr)
        return prStringNull();
    return serialized([&](PrTextBuffer* out) {
        cssSerializeSelectorRange(out, data->selectors, limit ? counts[0] : 0, counts[limit]);
    });
}

// Returns the index of the entry that `key` names in a feature value block, or the block's count if it holds none.
uint32_t featureIndex(const CssDeclarationBlock* block, PrStringView key) {
    CssDeclaredName name = {};
    return cssNameFromText(block->rule, key, &name) ? cssDeclarationBlockFind(block, name) : block->count;
}

// Writes the <font-feature-index>es of the feature value at `index` of a feature value block, up to `capacity` of them.
// Returns how many there are.
uint32_t featureIndexes(const CssDeclarationBlock* block, uint32_t index, uint32_t* PR_NULLABLE items,
        uint32_t capacity) {
    CssValueRun run = cssDeclarationValue(block, &cssDeclarations(block)[index]);
    for (uint32_t i = 0; i < run.count && i < capacity; ++i)
        items[i] = prF64SaturateU32(run.values[i].number);
    return run.count;
}

// Declares `descriptor` in the rule's block if the block accepts `value`. Otherwise, including for the empty string,
// does nothing.
template <typename Handle>
void setDescriptor(Handle* handle, PrCssDescriptor descriptor, PrStringView value) {
    PrCssRule* rule = handle;
    if (value.size == 0)
        return;
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    commitRuleBlock(rule, &edit, cssDeclarationBlockPrepareSet(&edit, &rule->data->style,
        cssDescriptorDeclaredName(descriptor), value, false));
}

uint32_t functionParameters(CssValueRun run, PrCssFunctionParameter* PR_NULLABLE items, uint32_t capacity) {
    // css-mixins-1/#dom-cssfunctionrule-getparameters: a <function-parameter> is its name, its <css-type> if written,
    // and ':' and its default value if written. An unwritten type is `*`, and an unwritten default value is the null
    // string.
    auto matches = [](const CssValueNode* value) {
        return cssValueIsProduction(value, CssValueDef_FunctionParameter);
    };
    return eachMatching(run, items, capacity, matches, [&](uint32_t i) {
        CssValueRun parameter = cssValueChildren(run, i);
        uint32_t colon = parameter.values[0].next;
        while (colon < parameter.count && !cssValueIsLiteral(&parameter.values[colon], ':'))
            colon += parameter.values[colon].next;
        CssValueRun type = {parameter.values + 1, colon - 1, parameter.text};
        if (type.count != 0 && cssValueIsFunction(&type.values[0], PrCssKeyword_Type))
            type = cssValueChildren(type, 0);
        PrCssFunctionParameter item;
        item.name = prStringCopy(cssValueText(parameter, parameter.values));
        item.type = type.count != 0 ? serializedValues(type) : prStringExternal("*");
        if (colon < parameter.count)
            item.defaultValue = serializedValues(CssValueRun{
                parameter.values + colon + 1, parameter.count - colon - 1,
                parameter.text,
            });
        else
            item.defaultValue = prStringNull();
        return item;
    });
}

// Serializes a mixin or apply name as an identifier, with any necessary escaping.
template <typename Handle>
PrString serializedName(const Handle* handle) {
    PrStringView name = preludeName(handle);
    return serialized([&](PrTextBuffer* out) { cssSerializeIdent(out, name); });
}

// Returns the depth of the deepest tree of a run, counting its roots as depth one.
uint32_t depthOf(CssValueRun run) {
    uint32_t deepest = 0;
    Array<uint32_t> ends;
    for (uint32_t i = 0; i < run.count; ++i) {
        while (ends.count != 0 && ends.last() <= i)
            ends.pop();
        ends.push(i + run.values[i].next);
        deepest = ends.count > deepest ? ends.count : deepest;
    }
    ends.reset();
    return deepest;
}

// Adds what `build` adds to `out`, which must have no node open. Adds nothing if `build` fails. Returns OutOfRange, and
// adds nothing, if `build` adds a tree deeper than a value may be.
template <typename Build>
PrStatus valueWrite(PrCssValue* handle, Build build) {
    PrCssValue* out = handle;
    if (out->open.count != 0)
        PR_FATAL("a value written with a node open");
    uint32_t values = out->pool.values.count;
    uint32_t text = out->pool.text.count;
    PrStatus status = build(&out->pool);
    CssValueRun added = {out->pool.values.data + values, out->pool.values.count - values, out->pool.text.data};
    if (status == PrStatus_Ok && depthOf(added) > kPrCssValueMaxDepth)
        status = PrStatus_OutOfRange;
    if (status != PrStatus_Ok) {
        out->pool.values.count = values;
        out->pool.text.count = text;
    }
    return status;
}

// Returns a value for reading. The value must have no node open.
const PrCssValue* valueRead(const PrCssValue* value) {
    if (value->open.count != 0)
        PR_FATAL("a value read with a node open");
    return value;
}

// Returns a value for reading at `node`. The value must have no node open and must hold `node`.
const PrCssValue* valueRead(const PrCssValue* handle, uint32_t node) {
    const PrCssValue* value = valueRead(handle);
    if (node >= value->pool.values.count)
        PR_FATAL("a value read at a node it does not hold");
    return value;
}

// Returns true if `fields` names members of its enumerations, and a legacy color is sRGB.
bool fieldsValid(const PrCssValueFields* fields) {
    switch (fields->kind) {
        case PrCssValueKind_Keyword:
        case PrCssValueKind_Function:     return fields->keyword < PrCssKeyword_Count;
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
        case PrCssValueKind_Percentage:   return true;
        case PrCssValueKind_Dimension:
            return fields->number.unit > PrCssUnit_Percent && fields->number.unit <= PrCssUnit_Unknown;
        case PrCssValueKind_Color:
            return fields->color.space < PrColorSpace_Count &&
                (!fields->color.legacy || fields->color.space == PrColorSpace_Srgb);
        case PrCssValueKind_Calc:         return fields->calc <= PrCssCalc_Invert;
        case PrCssValueKind_Literal:      return fields->literal != 0 && fields->literal < 0x80;
        case PrCssValueKind_Longhand:     return fields->longhand < PrCssProperty_Count;
        case PrCssValueKind_UnicodeRange:
            return fields->range.first <= fields->range.last && fields->range.last <= kPrUtf8MaxCodePoint;
        default:                      return fields->kind < PrCssValueKind_Production;
    }
}

// Returns the node that `fields` describes. `fields` must satisfy cssValueShapeValid. Adds the node's text to the pool.
CssValueNode nodeOf(const PrCssValueFields* fields, CssValuePool* pool) {
    CssValueNode node = {};
    node.kind = PrCssValueKind(fields->kind);
    node.next = 1;
    switch (fields->kind) {
        case PrCssValueKind_Keyword:
        case PrCssValueKind_Function:
            node.aux = fields->keyword;
            break;
        case PrCssValueKind_Ident:
        case PrCssValueKind_String:
        case PrCssValueKind_Url:
        case PrCssValueKind_Tokens:
            node.text.start = cssValuePoolAddText(pool, fields->text);
            node.text.length = uint32_t(fields->text.size);
            break;
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
        case PrCssValueKind_Percentage:
            node.number = fields->number.value;
            break;
        case PrCssValueKind_Dimension:
            node.number = fields->number.value;
            node.unit = fields->number.unit;
            break;
        case PrCssValueKind_Color:
            node.aux = CssValueAuxFlags(fields->color.space | (fields->color.legacy ? CssValueAuxFlag_ColorLegacy : 0));
            break;
        case PrCssValueKind_Calc:
            node.aux = fields->calc;
            break;
        case PrCssValueKind_Literal:
            node.aux = uint16_t(fields->literal);
            break;
        case PrCssValueKind_Longhand:
            node.aux = fields->longhand;
            break;
        case PrCssValueKind_UnicodeRange:
            node.range.first = fields->range.first;
            node.range.last = fields->range.last;
            break;
        default:
            break;
    }
    return node;
}

PrCssValueFields fieldsOf(CssValueRun run, uint32_t index) {
    const CssValueNode* node = &run.values[index];
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind(node->kind);
    switch (node->kind) {
        case PrCssValueKind_Keyword:
        case PrCssValueKind_Function:
            fields.keyword = cssValueKeyword(node);
            break;
        case PrCssValueKind_Ident:
        case PrCssValueKind_String:
        case PrCssValueKind_Url:
        case PrCssValueKind_Tokens:
            fields.text = cssValueText(run, node);
            break;
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
        case PrCssValueKind_Percentage:
        case PrCssValueKind_Dimension:
            fields.number.value = node->number;
            if (!cssNumericUnit(node, &fields.number.unit))
                fields.number.unit = PrCssUnit_Unknown;
            break;
        case PrCssValueKind_Color:
            fields.color.space = cssValueColorSpace(node);
            fields.color.legacy = cssValueColorLegacy(node);
            break;
        case PrCssValueKind_Calc:
            fields.calc = PrCssCalc(cssValueCalcOperator(node));
            break;
        case PrCssValueKind_Literal:
            fields.literal = uint8_t(cssValueLiteral(node));
            break;
        case PrCssValueKind_Longhand:
            fields.longhand = cssValueLonghand(node);
            break;
        case PrCssValueKind_UnicodeRange:
            fields.range.first = node->range.first;
            fields.range.last = node->range.last;
            break;
        default:
            break;
    }
    return fields;
}

// Returns true if a value of the property is a comma-separated list: its grammar, or one of its alternatives, is a list
// (css-typed-om-1/#list-valued-properties).
bool listValued(PrCssProperty property) {
    CssGrammarIndex root = kCssPropertyGrammarRoot[kCssPropertyAlias[property]];
    auto isList = [](CssGrammarIndex index) {
        const CssGrammarModifier* modifier = cssGrammarModifier(kCssGrammarNodes[index]);
        return modifier->max > 1 && (modifier->flags & CssGrammarFlag_Comma) != 0;
    };
    if (isList(root))
        return true;
    if (cssGrammarType(kCssGrammarNodes[root]) != CssGrammarType_OneOf)
        return false;
    for (CssGrammarIndex child = CssGrammarIndex(root + 1); child < cssGrammarAfter(root);
            child = cssGrammarAfter(child)) {
        if (isList(child))
            return true;
    }
    return false;
}

// Adds the value that `parse` matches from the whole of `text`, excluding surrounding whitespace. `parse` receives the
// stream and a parser to match with.
template <typename Parse>
bool parseInto(PrStringView text, CssValuePool* pool, Parse parse) {
    CssParser parser;
    cssParserInit(&parser);
    CssTokenStream ts = cssParserTokenize(&parser, text);
    CssTokenStream trimmed = cssSubStream(&ts, cssStreamTrimmed(&ts));
    CssValueSpan span;
    bool parsed = trimmed.pos != trimmed.end && parse(&trimmed, &parser, &span);
    if (parsed)
        cssValuePoolAddRun(pool, cssValuePoolRun(&parser.pool, span));
    cssParserReset(&parser);
    return parsed;
}

bool hasContents(const CssRuleData* rule) {
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        const CssRuleData* child = rule->children.data[i];
        if (child->kind == CssRuleDataKind_At && child->atRule == CssAtRule_Contents)
            return true;
        if (hasContents(child))
            return true;
    }
    return false;
}

// Returns the map of the feature type `type`: the object for the rule's block of that type.
PrCssFontFeatureValuesMap* featureMap(PrCssRule* rule, CssAtRule type) {
    return static_cast<PrCssFontFeatureValuesMap*>(cssRuleChild(rule, cssFeatureBlock(rule->data, type)));
}

// The at-rule of each page-margin box.
constexpr CssAtRule kMarginRules[PrCssMargin_Count] = {
    CssAtRule_TopLeftCorner, CssAtRule_TopLeft, CssAtRule_TopCenter, CssAtRule_TopRight,
    CssAtRule_TopRightCorner, CssAtRule_RightTop, CssAtRule_RightMiddle, CssAtRule_RightBottom,
    CssAtRule_BottomRightCorner, CssAtRule_BottomRight, CssAtRule_BottomCenter,
    CssAtRule_BottomLeft, CssAtRule_BottomLeftCorner, CssAtRule_LeftBottom, CssAtRule_LeftMiddle,
    CssAtRule_LeftTop,
};

// Sets a descriptor of a @counter-style rule to `value`, unless `value` does not parse as the descriptor, or the rule
// would then define no counter style or, for its system, change its algorithm.
void setCounterStyleDescriptor(PrCssRule* rule, PrCssDescriptor descriptor, PrStringView value) {
    CssValuePool pool{};
    bool parsed = parseInto(value, &pool, [&](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
        return cssParseDescriptorValue(descriptor, ts, &parser->pool, &parser->match, span);
    });
    CssValueRun run = cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count});
    bool accepted = parsed && cssCounterStyleRuleAccepts(rule->data, descriptor, run);
    cssValuePoolReset(&pool);
    if (accepted)
        setDescriptor(rule, descriptor, value);
}

// Returns true if resolving the <color> `run` reads the element that it is used on: its color, which currentcolor
// names, or its color scheme, which the system colors and light-dark() read. A font computed outside the cascade
// (PrCssFont). The families' names are views of `names`.
struct CssFont {
    PrTextBuffer names = {};
    Array<PrFontFamily> families;
    PrFontStyle style;
    bool smallCaps;
    PrTextBuffer serialization = {};
};

// The longhands of the font shorthand that a font computed outside the cascade reads, in the order computed.
constexpr PrCssProperty kFontLonghands[] = {
    PrCssProperty_FontSize, PrCssProperty_FontStyle, PrCssProperty_FontVariantCaps,
    PrCssProperty_FontWeight, PrCssProperty_FontWidth, PrCssProperty_FontFamily,
};

// Computes the font longhands of `block` in `context` into `out`, replacing their values in `block` with the computed
// ones and line-height with normal. Returns false if a longhand has no value, as one does while it waits on a
// substitution.
bool computeFont(CssDeclarationBlock* block, const CssStyleContext& context, CssFont* out) {
    out->style = prFontStyleDefault();
    for (PrCssProperty property : kFontLonghands) {
        CssDeclaredName name = cssPropertyDeclaredName(property);
        CssValuePool specified{};
        cssDeclarationBlockValue(block, name, &specified, nullptr);
        CssValuePool computed{};
        bool given = specified.values.count != 0;
        if (given) {
            CssValueRun run = cssValuePoolRun(&specified, CssValueSpan{0, specified.values.count});
            cssStyleComputeValue(&computed, property, run, context);
        }
        CssValueRun run = cssValuePoolRun(&computed, CssValueSpan{0, computed.values.count});
        given = given && run.count != 0;
        if (given) {
            const CssValueNode* value = run.values;
            switch (property) {
                case PrCssProperty_FontSize: {
                    double px = context.font.em;
                    cssResolveLength(run, 0, context.font.em, &px);
                    out->style.size = float(px);
                    break;
                }
                case PrCssProperty_FontStyle:
                    if (cssValueIsKeyword(value, PrCssKeyword_Italic)) {
                        out->style.slope = PrFontSlope_Italic;
                    } else if (cssValueIsKeyword(value, PrCssKeyword_Oblique)) {
                        double degrees = double(kPrFontSetObliqueAngle);
                        if (run.count > 1)
                            cssMathTo(run, value->next, PrCssUnit_Deg, &degrees);
                        out->style.slope = PrFontSlope_Oblique;
                        out->style.obliqueAngle = float(degrees);
                    }
                    break;
                case PrCssProperty_FontVariantCaps:
                    out->smallCaps = cssValueIsKeyword(value, PrCssKeyword_SmallCaps);
                    break;
                case PrCssProperty_FontWeight:
                    out->style.weight = run.count == 1 ? float(value->number) : out->style.weight;
                    break;
                case PrCssProperty_FontWidth:
                    if (value->kind == PrCssValueKind_Percentage)
                        out->style.width = float(value->number);
                    break;
                default:
                    cssFontFamilies(run, &out->names, &out->families);
                    break;
            }
            // A normal weight or width computes to a number, which the shorthand writes unless it is the keyword.
            bool normal = (property == PrCssProperty_FontWeight && out->style.weight == 400) ||
                (property == PrCssProperty_FontWidth && out->style.width == 100);
            if (normal)
                cssDeclarationBlockSet(block, name, "normal", false, nullptr);
            else
                cssDeclarationBlockSetValue(block, name, run, false, false, nullptr);
        }
        cssValuePoolReset(&specified);
        cssValuePoolReset(&computed);
        if (!given)
            return false;
    }
    cssDeclarationBlockSet(block, cssPropertyDeclaredName(PrCssProperty_LineHeight), "normal", false, nullptr);
    return true;
}

bool colorReadsElement(CssValueRun run) {
    for (uint32_t i = 0; i < run.count; ++i) {
        const CssValueNode* node = &run.values[i];
        if (node->kind == PrCssValueKind_Keyword &&
                (cssValueKeyword(node) == PrCssKeyword_Currentcolor || cssIsSystemColor(cssValueKeyword(node))))
            return true;
        if (node->kind == PrCssValueKind_Function && cssValueKeyword(node) == PrCssKeyword_LightDark)
            return true;
    }
    return false;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCssFont : CssFont {};

// ---------------------------------------------------------------------------------------------------------------------
// MediaList

PrString prCssMediaListMediaText(const PrCssMediaList* list) {
    const CssMediaQueries* queries = cssMediaListQueries(list);
    return serialized([&](PrTextBuffer* out) { cssSerializeMediaList(out, queries); });
}

void prCssMediaListSetMediaText(PrCssMediaList* list, PrStringView value) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    commitMedia(list, &edit, cssMediaListPrepareText(&edit, cssMediaListQueries(list), value));
}

uint32_t prCssMediaListLength(const PrCssMediaList* list) {
    return cssMediaListLength(cssMediaListQueries(list));
}

PrString prCssMediaListQueryAt(const PrCssMediaList* list, uint32_t index) {
    const CssMediaQueries* queries = cssMediaListQueries(list);
    if (index >= cssMediaListLength(queries))
        return prStringNull();
    return serializedValues(cssMediaQueryAt(queries, index));
}

void prCssMediaListAppendMedium(PrCssMediaList* list, PrStringView medium) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    commitMedia(list, &edit, cssMediaListPrepareAppend(&edit, cssMediaListQueries(list), medium));
}

PrStatus prCssMediaListDeleteMedium(PrCssMediaList* list, PrStringView medium) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    CssEditResult result = commitMedia(list, &edit, cssMediaListPrepareDelete(&edit, cssMediaListQueries(list), medium));
    return result == CssEditResult_Rejected ? PrStatus_NotFound : PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// MediaQueryList

PrCssMediaQueryList* PR_NULLABLE prCssMediaQueryListQuery(PrDomEventTarget* PR_NULLABLE object) {
    if (object == nullptr || object->type != DomTargetType_MediaQueryList)
        return nullptr;
    return static_cast<PrCssMediaQueryList*>(object);
}

PrString prCssMediaQueryListMedia(const PrCssMediaQueryList* mediaQueryList) {
    const CssMediaQueries* list = &mediaQueryList->media;
    return serialized([&](PrTextBuffer* out) { cssSerializeMediaList(out, list); });
}

bool prCssMediaQueryListMatches(const PrCssMediaQueryList* list) {
    return cssMediaListMatches(&list->media, list->environment);
}

// Adds a `change` listener in the same way as addEventListener() (cssom-view-1/#dom-mediaquerylist-addlistener).
void prCssMediaQueryListAddListener(PrCssMediaQueryList* list, const PrDomEventListener* PR_NULLABLE callback) {
    if (callback != nullptr)
        domListenerAdd(list, "change", callback, nullptr);
}

// cssom-view-1/#dom-mediaquerylist-removelistener
void prCssMediaQueryListRemoveListener(PrCssMediaQueryList* list, const PrDomEventListener* PR_NULLABLE callback) {
    if (callback != nullptr)
        domListenerRemove(list, "change", callback, false);
}

void prCssMediaQueryListRetain(PrCssMediaQueryList* list) {
    cssMediaQueryListRetain(list);
}

void prCssMediaQueryListRelease(PrCssMediaQueryList* PR_NULLABLE list) {
    cssMediaQueryListRelease(list);
}

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheet

PrStringView prCssStyleSheetHref(const PrCssStyleSheet* sheet) {
    return prStringView(&sheet->href);
}

PrDomNode* PR_NULLABLE prCssStyleSheetOwnerNode(PrCssStyleSheet* sheet) {
    return sheet->ownerNode;
}

PrCssStyleSheet* PR_NULLABLE prCssStyleSheetParentStyleSheet(PrCssStyleSheet* sheet) {
    const PrCssRule* owner = sheet->ownerRule;
    return owner != nullptr ? owner->sheet : nullptr;
}

PrStringView prCssStyleSheetTitle(const PrCssStyleSheet* sheet) {
    return prStringView(&sheet->title);
}

PrCssMediaList* prCssStyleSheetMedia(PrCssStyleSheet* sheet) {
    // An imported sheet's media is its rule's media (cssom-1/#dom-cssimportrule-media).
    return sheet->ownerRule != nullptr ? &sheet->ownerRule->media : &sheet->mediaList;
}

bool prCssStyleSheetDisabled(const PrCssStyleSheet* sheet) {
    return sheet->disabled;
}

void prCssStyleSheetSetDisabled(PrCssStyleSheet* sheet, bool value) {
    sheet->disabled = value;
    cssDocumentStyleChanged(cssStyleSheetDocument(sheet));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleSheet

PrCssStyleSheet* prCssStyleSheetCreate(const PrCssStyleSheetInit* PR_NULLABLE init) {
    PrCssStyleSheet* sheet = cssStyleSheetCreate(cssSheetDataCreate());
    cssStyleSheetRetain(sheet);
    sheet->constructed = true;
    PrCssStyleSheetInit defaults = prCssStyleSheetInitDefault();
    if (init == nullptr)
        init = &defaults;
    sheet->href = prStringCopy(init->baseUrl);
    sheet->disabled = init->disabled;
    // cssom-1/#dom-cssstylesheet-cssstylesheet: the media are a MediaList's queries, or the queries that the string
    // holds.
    const PrCssMediaList* media = init->media.mediaList;
    if (media != nullptr)
        sheet->media.queries = cssValueListCopy(cssValueListRun(&cssMediaListQueries(media)->queries));
    else
        cssMediaListParse(&sheet->media, init->media.string);
    return sheet;
}

void prCssStyleSheetRetain(PrCssStyleSheet* sheet) {
    cssStyleSheetRetain(sheet);
}

void prCssStyleSheetRelease(PrCssStyleSheet* PR_NULLABLE sheet) {
    cssStyleSheetRelease(sheet);
}

PrCssRule* PR_NULLABLE prCssStyleSheetOwnerRule(PrCssStyleSheet* sheet) {
    return sheet->ownerRule;
}

PrCssRuleList* prCssStyleSheetCssRules(PrCssStyleSheet* sheet) {
    return &sheet->cssRules;
}

PrStatus prCssStyleSheetInsertRule(PrCssStyleSheet* sheet, PrStringView rule, uint32_t index, uint32_t* PR_NULLABLE out) {
    PrStatus status = cssStyleSheetInsertRule(sheet, rule, index);
    if (status == PrStatus_Ok && out != nullptr)
        *out = index;
    return status;
}

PrStatus prCssStyleSheetDeleteRule(PrCssStyleSheet* sheet, uint32_t index) {
    return cssStyleSheetDeleteRule(sheet, index);
}

PrStatus prCssStyleSheetReplaceSync(PrCssStyleSheet* sheet, PrStringView text) {
    if (!sheet->constructed)
        return PrStatus_NotAllowed;

    cssStyleSheetReplace(sheet, text);
    return PrStatus_Ok;
}

PrStatus prCssStyleSheetAddRule(PrCssStyleSheet* sheet, PrStringView selector, PrStringView style,
        const uint32_t* PR_NULLABLE index, int32_t* PR_NULLABLE out) {
    // cssom-1/#dom-cssstylesheet-addrule: the rule is the selector, then ` { `, the style and a space if the style is
    // not empty, and `}`. It is inserted at the end if no index is given. The result is always -1.
    PrTextBuffer rule = {};
    prTextBufferAppend(&rule, selector);
    prTextBufferAppend(&rule, " { ");
    if (style.size != 0) {
        prTextBufferAppend(&rule, style);
        prTextBufferAppendChar(&rule, ' ');
    }
    prTextBufferAppendChar(&rule, '}');
    uint32_t position = index != nullptr ? *index : sheet->rules.count;
    PrStatus status = cssStyleSheetInsertRule(sheet, prTextBufferView(&rule), position);
    prTextBufferReset(&rule);
    if (status == PrStatus_Ok && out != nullptr)
        *out = -1;
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSRule

PrString prCssRuleCssText(const PrCssRule* rule) {
    const CssRuleData* data = dataOf(rule);
    return serialized([&](PrTextBuffer* out) { cssSerializeRule(out, data); });
}

// The setter does nothing (cssom-1/#the-cssrule-interface). It exists because the attribute is not readonly.
void prCssRuleSetCssText(PrCssRule*, PrStringView) {}

PrCssRule* PR_NULLABLE prCssRuleParentRule(PrCssRule* rule) {
    return rule->parent;
}

PrCssStyleSheet* PR_NULLABLE prCssRuleParentStyleSheet(PrCssRule* rule) {
    return rule->sheet;
}

void prCssRuleRetain(PrCssRule* rule) {
    cssRuleRetain(rule);
}

void prCssRuleRelease(PrCssRule* PR_NULLABLE rule) {
    cssRuleRelease(rule);
}

PrCssRuleType prCssRuleType(const PrCssRule* rule) {
    return cssRuleType(dataOf(rule));
}

PrCssRuleKind prCssRuleKind(const PrCssRule* rule) {
    return cssRuleKind(rule->data);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSGroupingRule

PrCssGroupingRule* PR_NULLABLE prCssGroupingRuleQuery(PrCssRule* PR_NULLABLE object) {
    const CssRuleData* rule = dataOf(object);
    if (rule == nullptr)
        return nullptr;

    // The rules whose interface inherits from CSSGroupingRule (cssom-1/#the-cssgroupingrule-interface), as declared by
    // the specs that define each one.
    if (rule->kind == CssRuleDataKind_Style)
        return static_cast<PrCssGroupingRule*>(object);
    if (rule->kind != CssRuleDataKind_At)
        return nullptr;
    switch (kCssAtRuleAlias[rule->atRule]) {
        case CssAtRule_Page:
        case CssAtRule_Media:
        case CssAtRule_Supports:
        case CssAtRule_Container:
        case CssAtRule_Scope:
        case CssAtRule_StartingStyle:
        case CssAtRule_Function:
        case CssAtRule_Mixin:
        case CssAtRule_SupportsCondition:
            return static_cast<PrCssGroupingRule*>(object);
        case CssAtRule_Layer:
        case CssAtRule_Apply:
        case CssAtRule_Contents:
            return rule->block ? static_cast<PrCssGroupingRule*>(object) : nullptr;
        default:
            return nullptr;
    }
}

PrCssRuleList* prCssGroupingRuleCssRules(PrCssGroupingRule* rule) {
    return &rule->cssRules;
}

PrStatus prCssGroupingRuleInsertRule(PrCssGroupingRule* groupingRule, PrStringView rule, uint32_t index,
        uint32_t* PR_NULLABLE out) {
    PrStatus status = cssRuleInsertRule(groupingRule, rule, index);
    if (status == PrStatus_Ok && out != nullptr)
        *out = index;
    return status;
}

PrStatus prCssGroupingRuleDeleteRule(PrCssGroupingRule* rule, uint32_t index) {
    return cssRuleDeleteRule(rule, index);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleRule

PrCssStyleRule* PR_NULLABLE prCssStyleRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssStyleRule>(object, PrCssRuleKind_Style);
}

PrString prCssStyleRuleSelectorText(const PrCssStyleRule* rule) {
    const CssRuleData* data = dataOf(rule);
    return serialized([&](PrTextBuffer* out) { cssSerializeSelectorText(out, data); });
}

void prCssStyleRuleSetSelectorText(PrCssStyleRule* rule, PrStringView value) {
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    Array<CssRuleChain> outer;
    CssEditResult result = cssStyleRulePrepareSelectorText(&edit, cssRuleChain(rule->parent, &outer), sheetDataOf(rule), value);
    outer.reset();
    if (result == CssEditResult_Changed)
        cssRuleCommit(&edit, cssRuleWritableTree(rule));
    cssEditEnd(&edit);
}

PrCssStyleProperties* prCssStyleRuleStyle(PrCssStyleRule* rule) {
    return static_cast<PrCssStyleProperties*>(&rule->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSImportRule
//
// The prelude is the URL, as a <string> or a url() holding one, then `layer` or a layer() if written, then a
// <supports-import-condition> if written. The media queries after them are the rule's media list.

PrCssImportRule* PR_NULLABLE prCssImportRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssImportRule>(object, PrCssRuleKind_Import);
}

PrStringView prCssImportRuleHref(const PrCssImportRule* rule) {
    return preludeName(rule);
}

PrCssMediaList* prCssImportRuleMedia(PrCssImportRule* rule) {
    return &rule->media;
}

PrCssStyleSheet* PR_NULLABLE prCssImportRuleStyleSheet(PrCssImportRule* rule) {
    return cssImportRuleSheet(rule);
}

PrString prCssImportRuleLayerName(const PrCssImportRule* rule) {
    // cssom-1/#dom-cssimportrule-layername: the empty string for `layer`, the layer's name for layer(), and the null
    // string for neither.
    CssValueRun run = preludeOf(rule);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsKeyword(value, PrCssKeyword_Layer))
            return prStringEmpty();
        if (cssValueIsFunction(value, PrCssKeyword_Layer))
            return serializedValues(cssValueChildren(run, i));
    }
    return prStringNull();
}

PrString prCssImportRuleSupportsText(const PrCssImportRule* rule) {
    // cssom-1/#dom-cssimportrule-supportstext: the contents of supports(), or the null string if the data has none. All
    // the selectors that the data holds belong to the condition.
    const CssRuleData* data = dataOf(rule);
    CssValueRun run = cssValueListRun(&data->prelude);
    uint32_t condition = findProduction(run, CssValueDef_SupportsImportCondition);
    if (condition == run.count)
        return prStringNull();
    return serialized([&](PrTextBuffer* out) {
        cssSerializePreludeValues(out, cssValueChildren(run, condition + 1), false, data->selectors, 0);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSNamespaceRule

PrCssNamespaceRule* PR_NULLABLE prCssNamespaceRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssNamespaceRule>(object, PrCssRuleKind_Namespace);
}

PrStringView prCssNamespaceRuleNamespaceUri(const PrCssNamespaceRule* rule) {
    PrStringView prefix;
    PrStringView url;
    cssNamespaceRuleParts(dataOf(rule), &prefix, &url);
    return url;
}

PrStringView prCssNamespaceRulePrefix(const PrCssNamespaceRule* rule) {
    PrStringView prefix;
    PrStringView url;
    cssNamespaceRuleParts(dataOf(rule), &prefix, &url);
    return prefix.data != nullptr ? prefix : "";
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSConditionRule

PrCssConditionRule* PR_NULLABLE prCssConditionRuleQuery(PrCssRule* PR_NULLABLE object) {
    const CssRuleData* rule = dataOf(object);
    bool conditional = rule != nullptr && rule->kind == CssRuleDataKind_At &&
        cssAtRuleConditional(kCssAtRuleAlias[rule->atRule]);
    return conditional ? static_cast<PrCssConditionRule*>(object) : nullptr;
}

PrString prCssConditionRuleConditionText(const PrCssConditionRule* rule) {
    // css-conditional-3/#the-cssmediarule-interface: a media rule's condition text is its media list's serialization. A
    // supports rule's condition text is the condition as written.
    return preludeText(rule);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMediaRule

PrCssMediaRule* PR_NULLABLE prCssMediaRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMediaRule>(object, PrCssRuleKind_Media);
}

PrCssMediaList* prCssMediaRuleMedia(PrCssMediaRule* rule) {
    return &rule->media;
}

bool prCssMediaRuleMatches(const PrCssMediaRule* rule) {
    const CssMediaEnvironment* environment = rule->sheet != nullptr ? cssStyleSheetMedia(rule->sheet) : nullptr;
    return environment != nullptr && cssMediaListMatches(&rule->data->media, environment);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsRule

PrCssSupportsRule* PR_NULLABLE prCssSupportsRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssSupportsRule>(object, PrCssRuleKind_Supports);
}

bool prCssSupportsRuleMatches(const PrCssSupportsRule* rule) {
    return cssSupportsEvaluate(cssValueListRun(&dataOf(rule)->prelude));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContainerRule
//
// The prelude is <container-condition>s joined by ','s. Each is a <container-name>, a <container-query>, or both.

PrCssContainerRule* PR_NULLABLE prCssContainerRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContainerRule>(object, PrCssRuleKind_Container);
}

PrString prCssContainerRuleContainerName(const PrCssContainerRule* rule) {
    return soleConditionPart(rule, CssValueDef_ContainerName);
}

PrString prCssContainerRuleContainerQuery(const PrCssContainerRule* rule) {
    return soleConditionPart(rule, CssValueDef_ContainerQuery);
}

uint32_t prCssContainerRuleConditions(const PrCssContainerRule* rule, PrCssContainerCondition* PR_NULLABLE items,
        uint32_t capacity) {
    CssValueRun run = preludeOf(rule);
    auto matches = [](const CssValueNode* value) {
        return cssValueIsProduction(value, CssValueDef_ContainerCondition);
    };
    return eachMatching(run, items, capacity, matches, [&](uint32_t i) {
        return PrCssContainerCondition{
            conditionPart(run, i, CssValueDef_ContainerName),
            conditionPart(run, i, CssValueDef_ContainerQuery),
        };
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSNestedDeclarations

PrCssNestedDeclarations* PR_NULLABLE prCssNestedDeclarationsQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssNestedDeclarations>(object, PrCssRuleKind_NestedDeclarations);
}

PrCssStyleProperties* prCssNestedDeclarationsStyle(PrCssNestedDeclarations* declarations) {
    return static_cast<PrCssStyleProperties*>(&declarations->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceRule

PrCssFontFaceRule* PR_NULLABLE prCssFontFaceRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontFaceRule>(object, PrCssRuleKind_FontFace);
}

PrCssFontFaceDescriptors* prCssFontFaceRuleStyle(PrCssFontFaceRule* rule) {
    return static_cast<PrCssFontFaceDescriptors*>(&rule->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPageRule

PrCssPageRule* PR_NULLABLE prCssPageRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPageRule>(object, PrCssRuleKind_Page);
}

PrString prCssPageRuleSelectorText(const PrCssPageRule* rule) {
    return preludeText(rule);
}

void prCssPageRuleSetSelectorText(PrCssPageRule* rule, PrStringView value) {
    setPrelude(rule, value);
}

PrCssPageDescriptors* prCssPageRuleStyle(PrCssPageRule* rule) {
    return static_cast<PrCssPageDescriptors*>(&rule->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMarginRule

PrStringView prCssMarginName(PrCssMargin margin) {
    return cssAtRuleName(kMarginRules[margin]);
}

PrCssMarginRule* PR_NULLABLE prCssMarginRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMarginRule>(object, PrCssRuleKind_Margin);
}

PrCssMargin prCssMarginRuleMargin(const PrCssMarginRule* marginRule) {
    CssAtRule rule = dataOf(marginRule)->atRule;
    for (PrCssMargin margin = 0; margin < PrCssMargin_Count; ++margin) {
        if (kMarginRules[margin] == rule)
            return margin;
    }
    PR_ASSERT(false);
    return PrCssMargin_TopLeftCorner;
}

PrCssStyleDeclaration* prCssMarginRuleStyle(PrCssMarginRule* rule) {
    return &rule->style;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframesRule

PrCssKeyframesRule* PR_NULLABLE prCssKeyframesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssKeyframesRule>(object, PrCssRuleKind_Keyframes);
}

PrStringView prCssKeyframesRuleName(const PrCssKeyframesRule* rule) {
    return preludeName(rule);
}

void prCssKeyframesRuleSetName(PrCssKeyframesRule* rule, PrStringView value) {
    // css-animations-1/#dom-csskeyframesrule-name: the name is an identifier if it can be one. Otherwise it is a
    // string, as it is for `none` and the CSS-wide keywords.
    PrTextBuffer name = {};
    cssSerializeIdent(&name, value);
    if (!setPrelude(rule, prTextBufferView(&name))) {
        prTextBufferClear(&name);
        cssSerializeString(&name, value);
        setPrelude(rule, prTextBufferView(&name));
    }
    prTextBufferReset(&name);
}

PrCssRuleList* prCssKeyframesRuleCssRules(PrCssKeyframesRule* rule) {
    return &rule->cssRules;
}

uint32_t prCssKeyframesRuleLength(const PrCssKeyframesRule* rule) {
    return dataOf(rule)->children.count;
}

PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleKeyframeRuleAt(PrCssKeyframesRule* rule, uint32_t index) {
    return index < rule->children.count ? static_cast<PrCssKeyframeRule*>(cssRuleChild(rule, index)) : nullptr;
}

void prCssKeyframesRuleAppendRule(PrCssKeyframesRule* keyframesRule, PrStringView rule) {
    // css-animations-1/#dom-csskeyframesrule-appendrule: text that is not a keyframe rule does nothing.
    cssRuleInsertRule(keyframesRule, rule, keyframesRule->children.count);
}

void prCssKeyframesRuleDeleteRule(PrCssKeyframesRule* rule, PrStringView select) {
    uint32_t index = cssKeyframesFind(rule->data, select, cssRuleParser(rule));
    if (index != rule->children.count)
        cssRuleDeleteRule(rule, index);
}

PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleFindRule(PrCssKeyframesRule* rule, PrStringView select) {
    uint32_t index = cssKeyframesFind(rule->data, select, cssRuleParser(rule));
    if (index == rule->children.count)
        return nullptr;
    return static_cast<PrCssKeyframeRule*>(cssRuleChild(rule, index));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframeRule

PrCssKeyframeRule* PR_NULLABLE prCssKeyframeRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssKeyframeRule>(object, PrCssRuleKind_Keyframe);
}

PrString prCssKeyframeRuleKeyText(const PrCssKeyframeRule* rule) {
    return preludeText(rule);
}

PrStatus prCssKeyframeRuleSetKeyText(PrCssKeyframeRule* rule, PrStringView value) {
    return setPrelude(rule, value) ? PrStatus_Ok : PrStatus_Syntax;
}

PrCssStyleProperties* prCssKeyframeRuleStyle(PrCssKeyframeRule* rule) {
    return static_cast<PrCssStyleProperties*>(&rule->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerBlockRule

PrCssLayerBlockRule* PR_NULLABLE prCssLayerBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssLayerBlockRule>(object, PrCssRuleKind_LayerBlock);
}

PrString prCssLayerBlockRuleName(const PrCssLayerBlockRule* rule) {
    // css-cascade-5/#dom-csslayerblockrule-name: the empty string for an anonymous layer.
    return serializedValues(preludeOf(rule));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerStatementRule

PrCssLayerStatementRule* PR_NULLABLE prCssLayerStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssLayerStatementRule>(object, PrCssRuleKind_LayerStatement);
}

uint32_t prCssLayerStatementRuleNameList(const PrCssLayerStatementRule* rule, PrString* PR_NULLABLE items,
        uint32_t capacity) {
    return serializeEach(preludeOf(rule), CssValueDef_LayerName, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSScopeRule

PrCssScopeRule* PR_NULLABLE prCssScopeRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssScopeRule>(object, PrCssRuleKind_Scope);
}

PrString prCssScopeRuleStart(const PrCssScopeRule* rule) {
    return scopeSelectors(rule, false);
}

PrString prCssScopeRuleEnd(const PrCssScopeRule* rule) {
    return scopeSelectors(rule, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStartingStyleRule

PrCssStartingStyleRule* PR_NULLABLE prCssStartingStyleRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssStartingStyleRule>(object, PrCssRuleKind_StartingStyle);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSCounterStyleRule
//
// css-counter-styles-3/#the-csscounterstylerule-interface: a descriptor attribute is its declaration's serialization.
// Setting one to a value that its grammar does not accept does nothing.

PrCssCounterStyleRule* PR_NULLABLE prCssCounterStyleRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssCounterStyleRule>(object, PrCssRuleKind_CounterStyle);
}

PrStringView prCssCounterStyleRuleName(const PrCssCounterStyleRule* rule) {
    return preludeName(rule);
}

void prCssCounterStyleRuleSetName(PrCssCounterStyleRule* rule, PrStringView value) {
    // css-counter-styles-3/#dom-csscounterstylerule-name: setting a name that the rule cannot accept does nothing, and
    // a predefined style's name is lowercased. The value must be a <counter-style-name> as written, which the prelude's
    // grammar checks.
    if (!cssCounterStyleNameDefinable(value))
        return;
    const CssCounterStyle* predefined = cssCounterStylePredefined(value);
    setPrelude(rule, predefined != nullptr ? predefined->name : value);
}

PrString prCssCounterStyleRuleSystem(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleSystem);
}

void prCssCounterStyleRuleSetSystem(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleSystem, value);
}

PrString prCssCounterStyleRuleSymbols(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleSymbols);
}

void prCssCounterStyleRuleSetSymbols(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleSymbols, value);
}

PrString prCssCounterStyleRuleAdditiveSymbols(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleAdditiveSymbols);
}

void prCssCounterStyleRuleSetAdditiveSymbols(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleAdditiveSymbols, value);
}

PrString prCssCounterStyleRuleNegative(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleNegative);
}

void prCssCounterStyleRuleSetNegative(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleNegative, value);
}

PrString prCssCounterStyleRulePrefix(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStylePrefix);
}

void prCssCounterStyleRuleSetPrefix(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStylePrefix, value);
}

PrString prCssCounterStyleRuleSuffix(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleSuffix);
}

void prCssCounterStyleRuleSetSuffix(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleSuffix, value);
}

PrString prCssCounterStyleRuleRange(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleRange);
}

void prCssCounterStyleRuleSetRange(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleRange, value);
}

PrString prCssCounterStyleRulePad(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStylePad);
}

void prCssCounterStyleRuleSetPad(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStylePad, value);
}

PrString prCssCounterStyleRuleSpeakAs(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleSpeakAs);
}

void prCssCounterStyleRuleSetSpeakAs(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleSpeakAs, value);
}

PrString prCssCounterStyleRuleFallback(const PrCssCounterStyleRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_CounterStyleFallback);
}

void prCssCounterStyleRuleSetFallback(PrCssCounterStyleRule* rule, PrStringView value) {
    setCounterStyleDescriptor(rule, PrCssDescriptor_CounterStyleFallback, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPropertyRule

PrCssPropertyRule* PR_NULLABLE prCssPropertyRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPropertyRule>(object, PrCssRuleKind_Property);
}

PrStringView prCssPropertyRuleName(const PrCssPropertyRule* rule) {
    return preludeName(rule);
}

PrStringView prCssPropertyRuleSyntax(const PrCssPropertyRule* rule) {
    // css-properties-values-api-1/#dom-csspropertyrule-syntax: the <string>'s value.
    const CssRuleData* data = dataOf(rule);
    const CssDeclaration* syntax = declarationOf(data, PrCssDescriptor_PropertySyntax);
    CssValueRun value = cssDeclarationValue(&data->style, syntax);
    return cssValueText(value, value.values);
}

bool prCssPropertyRuleInherits(const PrCssPropertyRule* rule) {
    const CssRuleData* data = dataOf(rule);
    CssValueRun value = cssDeclarationValue(&data->style, declarationOf(data, PrCssDescriptor_PropertyInherits));
    return cssValueKeyword(&value.values[0]) == PrCssKeyword_True;
}

PrString prCssPropertyRuleInitialValue(const PrCssPropertyRule* rule) {
    // The null string if the rule declares none.
    if (declarationOf(dataOf(rule), PrCssDescriptor_PropertyInitialValue) == nullptr)
        return prStringNull();
    return descriptorValue(rule, PrCssDescriptor_PropertyInitialValue);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesRule

PrCssFontFeatureValuesRule* PR_NULLABLE prCssFontFeatureValuesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontFeatureValuesRule>(object, PrCssRuleKind_FontFeatureValues);
}

PrString prCssFontFeatureValuesRuleFontFamily(const PrCssFontFeatureValuesRule* rule) {
    return serializedValues(preludeOf(rule));
}

void prCssFontFeatureValuesRuleSetFontFamily(PrCssFontFeatureValuesRule* rule, PrStringView value) {
    setPrelude(rule, value);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleAnnotation(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_Annotation);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleOrnaments(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_Ornaments);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStylistic(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_Stylistic);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleSwash(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_Swash);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleCharacterVariant(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_CharacterVariant);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStyleset(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_Styleset);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleHistoricalForms(PrCssFontFeatureValuesRule* rule) {
    return featureMap(rule, CssAtRule_HistoricalForms);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesMap
//
// The map is a feature value block. Its declarations are its entries, each a name and the <font-feature-index>es of its
// value (css-fonts-4/#font-feature-values-syntax).

uint32_t prCssFontFeatureValuesMapSize(const PrCssFontFeatureValuesMap* map) {
    return dataOf(map)->style.count;
}

PrStringView prCssFontFeatureValuesMapKeyAt(const PrCssFontFeatureValuesMap* map, uint32_t index) {
    const CssDeclarationBlock* block = &dataOf(map)->style;
    return cssDeclarationName(block, &cssDeclarations(block)[index]);
}

uint32_t prCssFontFeatureValuesMapValueAt(const PrCssFontFeatureValuesMap* map, uint32_t index,
        uint32_t* PR_NULLABLE items, uint32_t capacity) {
    return featureIndexes(&dataOf(map)->style, index, items, capacity);
}

bool prCssFontFeatureValuesMapHas(const PrCssFontFeatureValuesMap* map, PrStringView key) {
    const CssDeclarationBlock* block = &dataOf(map)->style;
    return featureIndex(block, key) != block->count;
}

uint32_t prCssFontFeatureValuesMapGet(const PrCssFontFeatureValuesMap* map, PrStringView key,
        uint32_t* PR_NULLABLE items, uint32_t capacity) {
    const CssDeclarationBlock* block = &dataOf(map)->style;
    return featureIndexes(block, featureIndex(block, key), items, capacity);
}

bool prCssFontFeatureValuesMapDelete(PrCssFontFeatureValuesMap* map, PrStringView key) {
    PrCssRule* rule = map;
    CssDeclaredName name = {};
    if (!cssNameFromText(rule->data->style.rule, key, &name))
        return false;
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    return commitRuleBlock(rule, &edit, cssDeclarationBlockPrepareRemove(&edit, &rule->data->style, name, nullptr)) ==
        CssEditResult_Changed;
}

void prCssFontFeatureValuesMapClear(PrCssFontFeatureValuesMap* map) {
    if (map->data->style.count != 0)
        cssDeclarationBlockReset(&cssRuleWritable(map)->style);
}

void prCssFontFeatureValuesMapSet(PrCssFontFeatureValuesMap* map, PrStringView key,
        const uint32_t* PR_NULLABLE values, uint32_t count) {
    // css-fonts-4/#om-fontfeaturevalues: the entry's value is the indexes, if the map's feature type accepts that many.
    // Setting any other count does nothing.
    PrTextBuffer value = {};
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&value, ' ');
        cssSerializeNumber(&value, double(values[i]));
    }
    PrCssRule* rule = map;
    CssDeclaredName name = {};
    if (cssNameFromText(rule->data->style.rule, key, &name)) {
        CssEdit edit;
        cssEditBegin(&edit, cssRuleParser(rule));
        PrStringView serialized = prTextBufferView(&value);
        commitRuleBlock(rule, &edit, cssDeclarationBlockPrepareSet(&edit, &rule->data->style, name, serialized, false));
    }
    prTextBufferReset(&value);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontPaletteValuesRule

PrCssFontPaletteValuesRule* PR_NULLABLE prCssFontPaletteValuesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontPaletteValuesRule>(object, PrCssRuleKind_FontPaletteValues);
}

PrStringView prCssFontPaletteValuesRuleName(const PrCssFontPaletteValuesRule* rule) {
    return preludeName(rule);
}

PrString prCssFontPaletteValuesRuleFontFamily(const PrCssFontPaletteValuesRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_FontPaletteValuesFontFamily);
}

PrString prCssFontPaletteValuesRuleBasePalette(const PrCssFontPaletteValuesRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_FontPaletteValuesBasePalette);
}

PrString prCssFontPaletteValuesRuleOverrideColors(const PrCssFontPaletteValuesRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_FontPaletteValuesOverrideColors);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPositionTryRule

PrCssPositionTryRule* PR_NULLABLE prCssPositionTryRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPositionTryRule>(object, PrCssRuleKind_PositionTry);
}

PrStringView prCssPositionTryRuleName(const PrCssPositionTryRule* rule) {
    return preludeName(rule);
}

PrCssPositionTryDescriptors* prCssPositionTryRuleStyle(PrCssPositionTryRule* rule) {
    return static_cast<PrCssPositionTryDescriptors*>(&rule->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSViewTransitionRule

PrCssViewTransitionRule* PR_NULLABLE prCssViewTransitionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssViewTransitionRule>(object, PrCssRuleKind_ViewTransition);
}

PrString prCssViewTransitionRuleNavigation(const PrCssViewTransitionRule* rule) {
    return descriptorValue(rule, PrCssDescriptor_ViewTransitionNavigation);
}

uint32_t prCssViewTransitionRuleTypes(const PrCssViewTransitionRule* rule, PrStringView* PR_NULLABLE items,
        uint32_t capacity) {
    // css-view-transitions-2/#dom-cssviewtransitionrule-types: the names that the types descriptor declares. `none`
    // declares no names.
    const CssRuleData* data = dataOf(rule);
    const CssDeclaration* types = declarationOf(data, PrCssDescriptor_ViewTransitionTypes);
    if (types == nullptr)
        return 0;
    return textOfEach(cssDeclarationValue(&data->style, types), PrCssValueKind_Ident, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionRule
//
// The prelude is the <function-token>'s name and '(', the <function-parameter>s joined by ','s, the ')', and `returns`
// and a <css-type> if written.

PrCssFunctionRule* PR_NULLABLE prCssFunctionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFunctionRule>(object, PrCssRuleKind_Function);
}

PrStringView prCssFunctionRuleName(const PrCssFunctionRule* rule) {
    return preludeName(rule);
}

uint32_t prCssFunctionRuleGetParameters(const PrCssFunctionRule* rule, PrCssFunctionParameter* PR_NULLABLE items,
        uint32_t capacity) {
    return functionParameters(preludeOf(rule), items, capacity);
}

PrString prCssFunctionRuleReturnType(const PrCssFunctionRule* rule) {
    // css-mixins-1/#dom-cssfunctionrule-returntype: `*` if the rule writes none.
    CssValueRun run = preludeOf(rule);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Returns))
            return serializedValues(CssValueRun{run.values + i + 1, run.count - i - 1, run.text});
    }
    return prStringExternal("*");
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionDeclarations

PrCssFunctionDeclarations* PR_NULLABLE prCssFunctionDeclarationsQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFunctionDeclarations>(object, PrCssRuleKind_FunctionDeclarations);
}

PrCssFunctionDescriptors* prCssFunctionDeclarationsStyle(PrCssFunctionDeclarations* declarations) {
    return static_cast<PrCssFunctionDescriptors*>(&declarations->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSCustomMediaRule

PrCssCustomMediaRule* PR_NULLABLE prCssCustomMediaRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssCustomMediaRule>(object, PrCssRuleKind_CustomMedia);
}

PrStringView prCssCustomMediaRuleName(const PrCssCustomMediaRule* rule) {
    return preludeName(rule);
}

PrCssMediaListOrBoolean prCssCustomMediaRuleQueryValue(PrCssCustomMediaRule* rule) {
    CssValueRun run = cssValueListRun(&rule->data->prelude);
    if (run.count > 1 && (cssValueIsKeyword(&run.values[1], PrCssKeyword_True) ||
            cssValueIsKeyword(&run.values[1], PrCssKeyword_False))) {
        return PrCssMediaListOrBoolean{nullptr, cssValueIsKeyword(&run.values[1], PrCssKeyword_True)};
    }
    return PrCssMediaListOrBoolean{(&rule->media), false};
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsConditionRule

PrCssSupportsConditionRule* PR_NULLABLE prCssSupportsConditionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssSupportsConditionRule>(object, PrCssRuleKind_SupportsCondition);
}

PrStringView prCssSupportsConditionRuleName(const PrCssSupportsConditionRule* rule) {
    return preludeName(rule);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMixinRule

PrCssMixinRule* PR_NULLABLE prCssMixinRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMixinRule>(object, PrCssRuleKind_Mixin);
}

PrString prCssMixinRuleName(const PrCssMixinRule* rule) {
    return serializedName(rule);
}

uint32_t prCssMixinRuleGetParameters(const PrCssMixinRule* rule, PrCssFunctionParameter* PR_NULLABLE items,
        uint32_t capacity) {
    return functionParameters(preludeOf(rule), items, capacity);
}

bool prCssMixinRuleContents(const PrCssMixinRule* rule) {
    return hasContents(dataOf(rule));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyBlockRule

PrCssApplyBlockRule* PR_NULLABLE prCssApplyBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssApplyBlockRule>(object, PrCssRuleKind_ApplyBlock);
}

PrString prCssApplyBlockRuleName(const PrCssApplyBlockRule* rule) {
    return serializedName(rule);
}

uint32_t prCssApplyBlockRuleGetArguments(const PrCssApplyBlockRule* rule, PrStringView* PR_NULLABLE items,
        uint32_t capacity) {
    return textOfEach(preludeOf(rule), PrCssValueKind_Tokens, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyStatementRule

PrCssApplyStatementRule* PR_NULLABLE prCssApplyStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssApplyStatementRule>(object, PrCssRuleKind_ApplyStatement);
}

PrString prCssApplyStatementRuleName(const PrCssApplyStatementRule* rule) {
    return serializedName(rule);
}

uint32_t prCssApplyStatementRuleGetArguments(const PrCssApplyStatementRule* rule,
        PrStringView* PR_NULLABLE items, uint32_t capacity) {
    return textOfEach(preludeOf(rule), PrCssValueKind_Tokens, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsBlockRule

PrCssContentsBlockRule* PR_NULLABLE prCssContentsBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContentsBlockRule>(object, PrCssRuleKind_ContentsBlock);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsStatementRule

PrCssContentsStatementRule* PR_NULLABLE prCssContentsStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContentsStatementRule>(object, PrCssRuleKind_ContentsStatement);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSRuleList

PrCssRule* PR_NULLABLE prCssRuleListRuleAt(PrCssRuleList* list, uint32_t index) {
    if (index >= cssRuleListData(list).count)
        return nullptr;
    return cssRuleListItem(list, index);
}

uint32_t prCssRuleListLength(const PrCssRuleList* list) {
    return cssRuleListData(list).count;
}

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheetList

PrCssStyleSheet* PR_NULLABLE prCssStyleSheetListStyleSheetAt(PrCssStyleSheetList* list, uint32_t index) {
    return index < list->sheets.count ? list->sheets.data[index] : nullptr;
}

uint32_t prCssStyleSheetListLength(const PrCssStyleSheetList* list) {
    return list->sheets.count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Declaration names

PrStringView prCssPropertyName(PrCssProperty property) {
    return cssPropertyName(property);
}

PrStringView prCssDescriptorName(PrCssDescriptor descriptor) {
    return cssDescriptorName(descriptor);
}

PrCssDeclarationName prCssDeclarationNameCopy(const PrCssDeclarationName* name) {
    return PrCssDeclarationName{name->property, name->descriptor, prStringClone(&name->author)};
}

void prCssDeclarationNameReset(PrCssDeclarationName* name) {
    prStringReset(&name->author);
    *name = prCssDeclarationNameFromProperty(PrCssProperty_Count);
}

bool prCssPropertyNameFromText(PrStringView text, PrCssDeclarationName* out) {
    CssDeclaredName declared = {};
    if (!cssNameFromText(CssAtRule_None, text, &declared))
        return false;
    *out = PrCssDeclarationName{declared.property, declared.descriptor, prStringCopy(declared.text)};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleDeclaration

PrString prCssStyleDeclarationCssText(const PrCssStyleDeclaration* declaration) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(declaration);
    return serialized([&](PrTextBuffer* out) { cssSerializeDeclarationBlock(out, block); });
}

PrStatus prCssStyleDeclarationSetCssText(PrCssStyleDeclaration* declaration, PrStringView value) {
    if (declaration->readOnly)
        return PrStatus_ReadOnly;
    cssDeclarationBlockParse(cssStyleDeclarationWritable(declaration), value, cssStyleDeclarationParser(declaration));
    cssStyleDeclarationUpdateOwner(declaration);
    return PrStatus_Ok;
}

uint32_t prCssStyleDeclarationLength(const PrCssStyleDeclaration* declaration) {
    return cssDeclarationBlockLength(cssStyleDeclarationBlock(declaration));
}

bool prCssStyleDeclarationNameFromText(const PrCssStyleDeclaration* declaration, PrStringView text,
        PrCssDeclarationName* out) {
    CssDeclaredName declared = {};
    if (!cssNameFromText(cssStyleDeclarationAtRule(declaration), text, &declared))
        return false;
    *out = PrCssDeclarationName{declared.property, declared.descriptor, prStringCopy(declared.text)};
    return true;
}

void prCssStyleDeclarationNameAt(const PrCssStyleDeclaration* declaration, uint32_t index, PrCssDeclarationName* out) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(declaration);
    // A property's or a descriptor's name comes from the generated table, and an author's name from the block.
    CssDeclarationItem item = cssDeclarationBlockItem(block, index);
    bool named = item.property != PrCssProperty_Count || item.declaration->descriptor != PrCssDescriptor_None;
    *out = PrCssDeclarationName{
        item.property, item.declaration->descriptor,
        named ? prStringNull() : prStringCopy(cssDeclarationName(block, item.declaration)),
    };
}

PrString prCssStyleDeclarationGetPropertyValue(const PrCssStyleDeclaration* declaration, const PrCssDeclarationName* name) {
    // A legacy name alias reads the property that it is an alias of (css-cascade-5/#legacy-name-alias).
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationAtRule(declaration), name, &declared))
        return prStringEmpty();
    bool single;
    if (declared.property < PrCssProperty_Count)
        single = kCssPropertyLonghands[declared.property].count == 0;
    else
        single = declared.text.data != nullptr;
    if (declaration->serializeValue != nullptr && single) {
        PrTextBuffer text{};
        if (declaration->serializeValue(declaration, declared, &text))
            return prTextBufferTake(&text);
        prTextBufferReset(&text);
    }
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(declaration);
    return serialized([&](PrTextBuffer* out) { cssSerializePropertyValue(out, block, declared); });
}

bool prCssStyleDeclarationIsImportant(const PrCssStyleDeclaration* declaration, const PrCssDeclarationName* name) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(declaration);
    CssDeclaredName declared = {};
    return cssDeclaredNameFromName(block->rule, name, &declared) && cssDeclarationBlockImportant(block, declared);
}

PrStatus prCssStyleDeclarationSetProperty(PrCssStyleDeclaration* declaration,
        const PrCssDeclarationName* name, PrStringView value, bool important) {
    if (declaration->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(declaration)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(declaration));
    CssEditResult result = commitDeclarations(declaration, &edit,
        cssDeclarationBlockPrepareSet(&edit, cssStyleDeclarationBlock(declaration), declared, value, important));
    return result == CssEditResult_Rejected ? PrStatus_InvalidData : PrStatus_Ok;
}

PrStatus prCssStyleDeclarationRemoveProperty(PrCssStyleDeclaration* declaration, const PrCssDeclarationName* name,
        PrString* PR_NULLABLE out) {
    if (declaration->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(declaration)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(declaration));
    commitDeclarations(declaration, &edit,
        cssDeclarationBlockPrepareRemove(&edit, cssStyleDeclarationBlock(declaration), declared, out));
    return PrStatus_Ok;
}

PrCssRule* PR_NULLABLE prCssStyleDeclarationParentRule(PrCssStyleDeclaration* declaration) {
    return declaration->parentRule;
}

PrStatus prCssStyleDeclarationValue(const PrCssStyleDeclaration* declaration, const PrCssDeclarationName* name,
        PrCssValue* out) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(declaration);
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(block->rule, name, &declared))
        return PrStatus_InvalidData;
    return valueWrite(out, [&](CssValuePool* pool) {
        cssDeclarationBlockValue(block, declared, pool, nullptr);
        return PrStatus_Ok;
    });
}

PrStatus prCssStyleDeclarationSetValue(PrCssStyleDeclaration* declaration,
        const PrCssDeclarationName* name, const PrCssValue* value, bool important) {
    if (declaration->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(declaration)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(declaration));
    CssEditResult result = cssDeclarationBlockPrepareSetValue(&edit, cssStyleDeclarationBlock(declaration), declared,
        cssValueRun(valueRead(value)), important, false);
    CssEditResult committed = commitDeclarations(declaration, &edit, result);
    return committed == CssEditResult_Rejected ? PrStatus_InvalidData : PrStatus_Ok;
}

PrStatus prCssStyleDeclarationAppendValue(PrCssStyleDeclaration* declaration, const PrCssDeclarationName* name,
        const PrCssValue* value) {
    if (declaration->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(declaration)->rule, name, &declared) ||
            declared.property == PrCssProperty_Count || !listValued(declared.property))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(declaration));
    CssEditResult result = cssDeclarationBlockPrepareSetValue(&edit, cssStyleDeclarationBlock(declaration), declared,
        cssValueRun(valueRead(value)), false, true);
    CssEditResult committed = commitDeclarations(declaration, &edit, result);
    return committed == CssEditResult_Rejected ? PrStatus_InvalidData : PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleProperties

PrCssStyleProperties* PR_NULLABLE prCssStylePropertiesQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    // The declarations of a style rule, a nested declarations rule, a style attribute, or a keyframe rule
    // (css-animations-1/#interface-csskeyframerule).
    PrCssStyleProperties* properties = blockOf<PrCssStyleProperties>(object, CssAtRule_None);
    return properties != nullptr ? properties : blockOf<PrCssStyleProperties>(object, CssAtRule_Keyframes);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceDescriptors, CSSPageDescriptors, CSSPositionTryDescriptors and CSSFunctionDescriptors
//
// The declarations of an at-rule's block. A caller reads and writes them like any other CSSStyleDeclaration.

PrCssFontFaceDescriptors* PR_NULLABLE prCssFontFaceDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssFontFaceDescriptors>(object, CssAtRule_FontFace);
}

PrCssPageDescriptors* PR_NULLABLE prCssPageDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssPageDescriptors>(object, CssAtRule_Page);
}

PrCssPositionTryDescriptors* PR_NULLABLE prCssPositionTryDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssPositionTryDescriptors>(object, CssAtRule_PositionTry);
}

PrCssFunctionDescriptors* PR_NULLABLE prCssFunctionDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssFunctionDescriptors>(object, CssAtRule_Function);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSS

PrString prCssEscape(PrStringView ident) {
    return serialized([&](PrTextBuffer* out) { cssSerializeIdent(out, ident); });
}

bool prCssSupportsPropertyValue(const PrCssDeclarationName* name, PrStringView value) {
    CssDeclaredName declared = {};
    return cssDeclaredNameFromName(CssAtRule_None, name, &declared) && cssDeclarationSupported(declared, value);
}

bool prCssSupportsConditionText(PrStringView conditionText) {
    return cssSupportsText(conditionText);
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFace

PrCssFontFace* prCssFontFaceCreate(PrStringView family, PrStringView source,
        const PrStringView* PR_NULLABLE descriptors) {
    return cssFontFaceCreate(family, source, PrByteView{}, nullptr, descriptors);
}

PrCssFontFace* prCssFontFaceCreateFromData(PrDomDocument* PR_NULLABLE document, PrStringView family, PrByteView data,
        const PrStringView* PR_NULLABLE descriptors) {
    static constexpr uint8_t kNone = 0;
    PrCompressCodec* compress = document != nullptr && document->host != nullptr ? document->host->compress : nullptr;
    return cssFontFaceCreate(family, PrStringView{}, PrByteView{data.data != nullptr ? data.data : &kNone, data.size},
        compress, descriptors);
}

void prCssFontFaceRetain(PrCssFontFace* face) {
    cssFontFaceRetain(face);
}

void prCssFontFaceRelease(PrCssFontFace* PR_NULLABLE face) {
    cssFontFaceRelease(face);
}

PrString prCssFontFaceDescriptor(const PrCssFontFace* face, PrCssFontFaceDescriptor descriptor) {
    return cssFontFaceDescriptor(face, descriptor);
}

PrStatus prCssFontFaceSetDescriptor(PrCssFontFace* face, PrCssFontFaceDescriptor descriptor, PrStringView value) {
    return cssFontFaceSetDescriptor(face, descriptor, value);
}

PrCssFontFaceStatus prCssFontFaceStatus(const PrCssFontFace* face) {
    return face->status;
}

PrDomDocument* prCssFontFaceLoadingDocument(const PrCssFontFace* face) {
    return face->loading;
}

bool prCssFontFaceFailedToParse(const PrCssFontFace* face) {
    return face->unparsed;
}

PrCssFontFaceStatus prCssFontFaceLoad(PrCssFontFace* face, PrDomDocument* document) {
    cssFontFaceLoad(face, document);
    return face->status;
}

bool prCssFontFaceIsCssConnected(const PrCssFontFace* face) {
    return face->rule != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSet

PrCssFontFaceSet* PR_NULLABLE prCssFontFaceSetQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_FontFaceSet ? static_cast<PrCssFontFaceSet*>(object) : nullptr;
}

PrDomEventTarget* prCssFontFaceSetAsEventTarget(PrCssFontFaceSet* set) {
    return set;
}

uint32_t prCssFontFaceSetSize(const PrCssFontFaceSet* set) {
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(set, &faces);
    uint32_t count = faces.count;
    faces.reset();
    return count;
}

PrCssFontFace* PR_NULLABLE prCssFontFaceSetAt(PrCssFontFaceSet* set, uint32_t index) {
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(set, &faces);
    PrCssFontFace* face = index < faces.count ? faces.data[index] : nullptr;
    faces.reset();
    return face;
}

bool prCssFontFaceSetHas(const PrCssFontFaceSet* set, const PrCssFontFace* face) {
    return cssFontFaceSetHas(set, face);
}

PrStatus prCssFontFaceSetAdd(PrCssFontFaceSet* set, PrCssFontFace* face) {
    return cssFontFaceSetAdd(set, face);
}

bool prCssFontFaceSetDelete(PrCssFontFaceSet* set, PrCssFontFace* face) {
    return cssFontFaceSetDelete(set, face);
}

void prCssFontFaceSetClear(PrCssFontFaceSet* set) {
    cssFontFaceSetClear(set);
}

PrCssFontFaceSetLoadStatus prCssFontFaceSetStatus(const PrCssFontFaceSet* set) {
    return set->status;
}

PrStatus prCssFontFaceSetCheck(const PrCssFontFaceSet* set, PrStringView font, bool* out) {
    Array<PrCssFontFace* PR_NONNULL> matched;
    PrStatus status = cssFontFaceSetMatch(set, font, &matched);
    *out = true;
    for (uint32_t i = 0; i < matched.count; ++i) {
        if (matched.data[i]->status != PrCssFontFaceStatus_Loaded)
            *out = false;
    }
    matched.reset();
    return status;
}

PrStatus prCssFontFaceSetLoad(PrCssFontFaceSet* set, PrDomDocument* document, PrStringView font,
        PrCssFontFace* PR_NONNULL* PR_NULLABLE items, uint32_t capacity, uint32_t* count) {
    Array<PrCssFontFace* PR_NONNULL> matched;
    PrStatus status = cssFontFaceSetMatch(set, font, &matched);
    *count = matched.count;
    for (uint32_t i = 0; i < matched.count; ++i) {
        cssFontFaceLoad(matched.data[i], document);
        if (items != nullptr && i < capacity)
            items[i] = matched.data[i];
    }
    matched.reset();
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Highlight

PrCssHighlight* prCssHighlightCreate(void) {
    return cssHighlightCreate();
}

void prCssHighlightRetain(PrCssHighlight* highlight) {
    cssHighlightRetain(highlight);
}

void prCssHighlightRelease(PrCssHighlight* PR_NULLABLE highlight) {
    cssHighlightRelease(highlight);
}

uint32_t prCssHighlightSize(const PrCssHighlight* highlight) {
    return highlight->ranges.count;
}

PrDomRange* PR_NULLABLE prCssHighlightAt(PrCssHighlight* highlight, uint32_t index) {
    return index < highlight->ranges.count ? highlight->ranges.data[index] : nullptr;
}

bool prCssHighlightHas(const PrCssHighlight* highlight, const PrDomRange* range) {
    return highlight->ranges.find(const_cast<PrDomRange*>(range)) != kArrayMissing;
}

void prCssHighlightAdd(PrCssHighlight* highlight, PrDomRange* range) {
    cssHighlightAdd(highlight, range);
}

bool prCssHighlightDelete(PrCssHighlight* highlight, const PrDomRange* range) {
    return cssHighlightDelete(highlight, const_cast<PrDomRange*>(range));
}

void prCssHighlightClear(PrCssHighlight* highlight) {
    cssHighlightClear(highlight);
}

uint32_t prCssHighlightVersion(const PrCssHighlight* highlight) {
    return highlight->version;
}

int32_t prCssHighlightPriority(const PrCssHighlight* highlight) {
    return highlight->priority;
}

void prCssHighlightSetPriority(PrCssHighlight* highlight, int32_t priority) {
    highlight->priority = priority;
}

PrCssHighlightType prCssHighlightType(const PrCssHighlight* highlight) {
    return highlight->type;
}

void prCssHighlightSetType(PrCssHighlight* highlight, PrCssHighlightType type) {
    highlight->type = type;
}

// ---------------------------------------------------------------------------------------------------------------------
// HighlightRegistry

uint32_t prCssHighlightRegistrySize(PrCssHighlightRegistry* registry) {
    return cssHighlightRegistryLive(registry)->entries.count;
}

PrStringView prCssHighlightRegistryNameAt(PrCssHighlightRegistry* registry, uint32_t index) {
    const PrCssHighlightRegistry* live = cssHighlightRegistryLive(registry);
    return index < live->entries.count ? prStringView(&live->entries.data[index]->name) : PrStringView{};
}

PrCssHighlight* PR_NULLABLE prCssHighlightRegistryAt(PrCssHighlightRegistry* registry, uint32_t index) {
    const PrCssHighlightRegistry* live = cssHighlightRegistryLive(registry);
    return index < live->entries.count ? (weakGet<PrCssHighlight>(live->entries.data[index]->highlight)) : nullptr;
}

PrCssHighlight* PR_NULLABLE prCssHighlightRegistryGet(PrCssHighlightRegistry* registry, PrStringView name) {
    const PrCssHighlightRegistry* live = cssHighlightRegistryLive(registry);
    uint32_t at = cssHighlightRegistryFind(live, name);
    return at != kArrayMissing ? (weakGet<PrCssHighlight>(live->entries.data[at]->highlight)) : nullptr;
}

void prCssHighlightRegistrySet(PrCssHighlightRegistry* registry, PrStringView name, PrCssHighlight* highlight) {
    cssHighlightRegistrySet(cssHighlightRegistryLive(registry), name, highlight);
}

bool prCssHighlightRegistryDelete(PrCssHighlightRegistry* registry, PrStringView name) {
    return cssHighlightRegistryDelete(cssHighlightRegistryLive(registry), name);
}

void prCssHighlightRegistryClear(PrCssHighlightRegistry* registry) {
    cssHighlightRegistryClear(cssHighlightRegistryLive(registry));
}

uint32_t prCssHighlightRegistryVersion(PrCssHighlightRegistry* registry) {
    return cssHighlightRegistryLive(registry)->version;
}

// ---------------------------------------------------------------------------------------------------------------------
// Values

PrStringView prCssKeywordName(PrCssKeyword keyword) {
    return cssKeywordName(keyword);
}

PrCssKeyword prCssKeywordFromText(PrStringView text) {
    return cssKeywordFromText(text);
}

PrStringView prCssUnitName(PrCssUnit unit) {
    return cssNumericUnitName(unit);
}

bool prCssUnitFromName(PrStringView name, PrCssUnit* out) {
    return cssNumericUnitFromName(name, out);
}

bool prCssColorFromKeyword(PrCssKeyword keyword, PrColor8* out) {
    if (keyword == PrCssKeyword_Transparent) {
        *out = PrColor8{0, 0, 0, 0};
        return true;
    }
    return cssNamedColor(keyword, out);
}

uint32_t prCssPropertyLonghands(PrCssProperty property, PrCssProperty* PR_NULLABLE items, uint32_t capacity) {
    CssPropertyList longhands = cssPropertyLonghands(kCssPropertyAlias[property]);
    for (uint32_t i = 0; items != nullptr && i < longhands.count && i < capacity; ++i)
        items[i] = longhands.data[i];
    return longhands.count;
}

bool prCssPropertyListValued(PrCssProperty property) {
    return listValued(property);
}

PrCssValue* prCssValueCreate(void) {
    return allocZeroed<PrCssValue>();
}

void prCssValueDestroy(PrCssValue* PR_NULLABLE value) {
    if (value == nullptr)
        return;
    cssValueReset(value);
    prMemoryFree(value);
}

void prCssValueClear(PrCssValue* value) {
    cssValueClear(value);
}

PrStatus prCssValueOpen(PrCssValue* value, const PrCssValueFields* fields) {
    if (!fieldsValid(fields))
        return PrStatus_InvalidData;
    if (fields->kind == PrCssValueKind_Calc) {
        bool calculated = false;
        for (uint32_t i = 0; i < value->open.count && !calculated; ++i)
            calculated = value->pool.values.data[value->open.data[i].node].kind == PrCssValueKind_Math;
        if (!calculated)
            return PrStatus_InvalidData;
    }
    if (value->open.count == kPrCssValueMaxDepth)
        return PrStatus_OutOfRange;
    uint32_t text = value->pool.text.count;
    uint32_t node = cssValuePoolAdd(&value->pool, nodeOf(fields, &value->pool));
    value->open.push(CssValueOpen{node, text});
    return PrStatus_Ok;
}

PrStatus prCssValueClose(PrCssValue* value) {
    if (value->open.count == 0)
        PR_FATAL("a value closed with no node open");
    CssValueOpen open = value->open.data[--value->open.count];
    CssValueNode* node = &value->pool.values.data[open.node];
    node->next = value->pool.values.count - open.node;
    CssValueRun run = cssValueRun(value);
    bool valid = cssValueShapeValid(run, open.node);
    if (valid && node->kind == PrCssValueKind_Math) {
        PrCssNumericType type;
        valid = cssMathType(run, open.node, &type);
        node->aux = valid ? cssMathCategory(type) : CssUnitCategory_Unknown;
    }
    if (!valid) {
        value->pool.values.count = open.node;
        value->pool.text.count = open.text;
        return PrStatus_InvalidData;
    }
    return PrStatus_Ok;
}

PrStatus prCssValueAdd(PrCssValue* value, const PrCssValueFields* fields) {
    PrStatus status = prCssValueOpen(value, fields);
    return status == PrStatus_Ok ? prCssValueClose(value) : status;
}

PrStatus prCssValueAddTree(PrCssValue* value, const PrCssValue* from, uint32_t node) {
    if (value == from)
        PR_FATAL("a value's tree added to itself");
    CssValueRun tree = cssValueSubtree(cssValueRun(valueRead(from, node)), node);
    if (value->open.count + depthOf(tree) > kPrCssValueMaxDepth)
        return PrStatus_OutOfRange;
    cssValuePoolAddRun(&value->pool, tree);
    return PrStatus_Ok;
}

uint32_t prCssValueSize(const PrCssValue* value) {
    return value->pool.values.count;
}

uint32_t prCssValueNext(const PrCssValue* value, uint32_t node) {
    return node + valueRead(value, node)->pool.values.data[node].next;
}

PrCssValueFields prCssValueFields(const PrCssValue* value, uint32_t node) {
    return fieldsOf(cssValueRun(valueRead(value, node)), node);
}

void prCssValueColorChannels(const PrCssValue* value, uint32_t node, PrCssColorChannel channels[PR_NONNULL 4]) {
    CssValueRun run = cssValueRun(valueRead(value, node));
    if (run.values[node].kind != PrCssValueKind_Color)
        PR_FATAL("the channels of a value that is not a color");
    CssColorChannels of = cssColorChannelsFromValue(run, node);
    for (uint32_t i = 0; i < 4; ++i)
        channels[i] = PrCssColorChannel{of.at[i], of.meaning[i]->number, of.meaning[i]->percent, of.meaning[i]->hue};
}

PrStatus prCssValueParse(const PrCssDeclarationName* name, PrStringView text, PrCssValue* out) {
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(CssAtRule_None, name, &declared))
        return PrStatus_InvalidData;
    return valueWrite(out, [&](CssValuePool* pool) {
        return cssParseDeclaredValue(declared, text, pool, nullptr) ? PrStatus_Ok : PrStatus_Syntax;
    });
}

PrStatus prCssValueParseNumeric(PrStringView text, PrCssValue* out) {
    return valueWrite(out, [&](CssValuePool* pool) {
        bool parsed = parseInto(text, pool, [](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
            return cssParseNumericValue(ts, &parser->pool, &parser->match, span);
        });
        return parsed ? PrStatus_Ok : PrStatus_Syntax;
    });
}

PrStatus prCssValueParseColor(PrStringView text, PrCssValue* out) {
    return valueWrite(out, [&](CssValuePool* pool) {
        uint32_t first = pool->values.count;
        bool parsed = parseInto(text, pool, [](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
            return cssParseValue(kCssValueDefGrammarRoot[CssValueDef_Color], ts, &parser->pool, &parser->match, span);
        });
        if (!parsed)
            return PrStatus_Syntax;
        const CssValueNode* value = &pool->values.data[first];
        PrColor8 named;
        if (value->kind == PrCssValueKind_Keyword && prCssColorFromKeyword(cssValueKeyword(value), &named)) {
            pool->values.count = first;
            cssValuePoolAddColor(pool, named);
        }
        return PrStatus_Ok;
    });
}

PrStatus prCssParseColor(PrStringView text, const PrDomElement* PR_NULLABLE element, PrCssUsedColor* out) {
    CssValuePool pool{};
    bool parsed = parseInto(text, &pool, [](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
        return cssParseValue(kCssValueDefGrammarRoot[CssValueDef_Color], ts, &parser->pool, &parser->match, span);
    });
    CssValueRun run = cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count});
    CssColorContext context{.current = PrColor{0, 0, 0, 1}};
    if (parsed && element != nullptr && colorReadsElement(run)) {
        const CssRoot* documentStyle = element->document->style;
        if (domIsConnected(element) && documentStyle != nullptr && documentStyle->presentationHooks != nullptr)
            context = documentStyle->presentationHooks->colorContext(documentStyle->presentation, element);
    }
    CssColor color{};
    bool resolved = parsed && cssResolveColorComponents(run, 0, context, &color);
    // A keyword resolves to an sRGB color, which serializes as rgb() does, as do hex colors, rgb(), hsl() and hwb(). A
    // color function's result keeps its own space.
    const CssValueNode* value = run.values;
    bool legacy = resolved && (value->kind == PrCssValueKind_Keyword || (value->kind == PrCssValueKind_Color &&
        (cssValueColorLegacy(value) || color.space == PrColorSpace_Hsl || color.space == PrColorSpace_Hwb)));
    cssValuePoolReset(&pool);
    if (!resolved)
        return PrStatus_Syntax;
    if (color.space == PrColorSpace_Hsl || color.space == PrColorSpace_Hwb)
        color = cssColorConvert(color, PrColorSpace_Srgb);
    *out = PrCssUsedColor{
        .space = color.space,
        .legacy = legacy,
        .missing = uint8_t(color.missing),
        .components = {color.c[0], color.c[1], color.c[2]},
        .alpha = color.alpha,
    };
    return PrStatus_Ok;
}

PrString prCssUsedColorSerialize(const PrCssUsedColor* color, PrCssColorSerialization serialization) {
    CssColor value{
        .space = color->space,
        .c = {color->components[0], color->components[1], color->components[2]},
        .alpha = color->alpha,
        .missing = color->missing,
    };
    if (color->legacy && serialization == PrCssColorSerialization_HtmlCompatible &&
            (color->missing & 8u) == 0 && color->alpha == 1) {
        // A missing component is zero in hex, which cannot write none.
        uint32_t bytes[3] = {};
        bool whole = true;
        for (uint32_t i = 0; i < 3; ++i) {
            double scaled = (color->missing & (1u << i)) != 0 ? 0 : color->components[i] * 255;
            double rounded = prF64Round(scaled);
            whole = whole && prF64Abs(scaled - rounded) < 1e-6 && rounded >= 0 && rounded <= 255;
            bytes[i] = whole ? uint32_t(rounded) : 0;
        }
        if (whole) {
            constexpr char kHex[] = "0123456789abcdef";
            char hex[7] = {'#'};
            for (uint32_t i = 0; i < 3; ++i) {
                hex[1 + 2 * i] = kHex[bytes[i] >> 4];
                hex[2 + 2 * i] = kHex[bytes[i] & 15];
            }
            return prStringCopy(PrStringView{hex, 7});
        }
    }
    CssValuePool pool{};
    if (color->legacy)
        cssValuePoolAddLegacyColor(&pool, value);
    else
        cssValuePoolAddColorComponents(&pool, value);
    PrString text = serializedValues(cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count}));
    cssValuePoolReset(&pool);
    return text;
}

PrStatus prCssFontCreate(PrStringView text, const PrDomElement* PR_NULLABLE element, PrCssFont* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // The value of the font property, which cannot be a CSS-wide keyword or a substitution here.
    PrStringView trimmed = prStringTrim(text);
    if (cssIsCssWideKeyword(cssKeywordFromText(trimmed)))
        return PrStatus_Syntax;
    CssDeclarationBlock block = {};
    if (trimmed.size == 0 ||
            cssDeclarationBlockSet(&block, cssPropertyDeclaredName(PrCssProperty_Font), text, false, nullptr) ==
            CssEditResult_Rejected) {
        cssDeclarationBlockReset(&block);
        return PrStatus_Syntax;
    }
    // Relative values are relative to the element's font, or to a normal weight at 10px without one.
    constexpr double kDefaultSize = 10;
    CssStyleContext context{
        .font = cssFontBases(kDefaultSize, kDefaultSize * kCssNormalLineHeight),
        .root = kCssInitialFontBases,
        .parentWeight = 400,
        .parentColor = PrColor{0, 0, 0, 1},
    };
    const CssRoot* documentStyle = element != nullptr ? element->document->style : nullptr;
    if (element != nullptr && domIsConnected(element) && documentStyle != nullptr &&
            documentStyle->presentationHooks != nullptr) {
        const CssPresentationHooks* hooks = documentStyle->presentationHooks;
        if (const CssComputedStyle* style = hooks->styleCurrent(documentStyle->presentation, element)) {
            context.font = cssStyleFontBases(style);
            context.parentWeight = cssStyleFontWeight(style);
        }
    }
    PrCssFont* font = allocZeroed<PrCssFont>();
    bool computed = computeFont(&block, context, font);
    if (computed)
        cssSerializePropertyValue(&font->serialization, &block, cssPropertyDeclaredName(PrCssProperty_Font));
    cssDeclarationBlockReset(&block);
    if (!computed || font->families.count == 0) {
        prCssFontDestroy(font);
        return PrStatus_Syntax;
    }
    *out = font;
    return PrStatus_Ok;
}

void prCssFontDestroy(PrCssFont* PR_NULLABLE font) {
    if (font == nullptr)
        return;
    prTextBufferReset(&font->names);
    font->families.reset();
    prTextBufferReset(&font->serialization);
    prMemoryFree(font);
}

uint32_t prCssFontFamilyCount(const PrCssFont* font) {
    return font->families.count;
}

const PrFontFamily* PR_NULLABLE prCssFontFamilies(const PrCssFont* font) {
    return font->families.data;
}

PrFontStyle prCssFontStyle(const PrCssFont* font) {
    return font->style;
}

bool prCssFontSmallCaps(const PrCssFont* font) {
    return font->smallCaps;
}

PrStringView prCssFontSerialization(const PrCssFont* font) {
    return prTextBufferView(&font->serialization);
}

PrStatus prCssParseFilterList(PrStringView text, const PrDomElement* PR_NULLABLE element,
        PrLayoutFilter* PR_NULLABLE out, uint32_t capacity, uint32_t* count) {
    *count = 0;
    PrStringView trimmed = prStringTrim(text);
    PrCssKeyword keyword = cssKeywordFromText(trimmed);
    if (cssIsCssWideKeyword(keyword) || keyword == PrCssKeyword_None)
        return PrStatus_Syntax;
    CssValuePool specified{};
    if (trimmed.size == 0 ||
            !cssParseDeclaredValue(cssPropertyDeclaredName(PrCssProperty_Filter), text, &specified, nullptr) ||
            specified.values.count == 0 || specified.values.data[0].kind == PrCssValueKind_Tokens) {
        cssValuePoolReset(&specified);
        return PrStatus_Syntax;
    }
    // Lengths are relative to the element's font, or to a 10px font without one, and currentcolor is its color.
    constexpr double kDefaultSize = 10;
    CssStyleContext context{
        .font = cssFontBases(kDefaultSize, kDefaultSize * kCssNormalLineHeight),
        .root = kCssInitialFontBases,
        .parentWeight = 400,
        .parentColor = PrColor{0, 0, 0, 1},
    };
    CssColorContext colors{.current = PrColor{0, 0, 0, 1}};
    const CssRoot* documentStyle = element != nullptr ? element->document->style : nullptr;
    if (element != nullptr && domIsConnected(element) && documentStyle != nullptr &&
            documentStyle->presentationHooks != nullptr) {
        colors = documentStyle->presentationHooks->colorContext(documentStyle->presentation, element);
        const CssComputedStyle* style = documentStyle->presentationHooks->styleCurrent(documentStyle->presentation, element);
        if (style != nullptr)
            context.font = cssStyleFontBases(style);
    }
    CssValuePool computed{};
    cssStyleComputeValue(&computed, PrCssProperty_Filter,
        cssValuePoolRun(&specified, CssValueSpan{0, specified.values.count}), context);
    Array<CssFilterFunction> items;
    cssFilterFunctions(cssValuePoolRun(&computed, CssValueSpan{0, computed.values.count}), colors, &items);
    for (uint32_t i = 0; i < items.count && i < capacity; ++i) {
        out[i] = items.data[i].filter;
        if (out[i].kind == PrLayoutFilterKind_Source)
            out[i].source = PrLayoutSource{};
    }
    *count = items.count;
    items.reset();
    cssValuePoolReset(&computed);
    cssValuePoolReset(&specified);
    return PrStatus_Ok;
}

PrString prCssValueSerialize(const PrCssValue* value, uint32_t first, uint32_t end) {
    CssValueRun run = cssValueRun(valueRead(value));
    if (first > end || end > run.count)
        PR_FATAL("a value serialized past its end");
    return serialized([&](PrTextBuffer* out) {
        cssSerializeValues(out, CssValueRun{run.values + first, end - first, run.text});
    });
}

bool prCssValueEquals(const PrCssValue* value, uint32_t node, const PrCssValue* other, uint32_t otherNode) {
    return cssValueRunEquals(cssValueSubtree(cssValueRun(valueRead(value, node)), node),
        cssValueSubtree(cssValueRun(valueRead(other, otherNode)), otherNode));
}

PrStatus prCssValueNumericType(const PrCssValue* value, uint32_t node, PrCssNumericType* out) {
    return cssMathType(cssValueRun(valueRead(value, node)), node, out) ? PrStatus_Ok : PrStatus_InvalidData;
}

bool prCssNumericTypeFromUnit(PrCssUnit unit, PrCssNumericType* out) {
    return cssNumericTypeOfUnit(unit, out);
}

bool prCssNumericTypeAdd(const PrCssNumericType* a, const PrCssNumericType* b, PrCssNumericType* out) {
    return cssNumericTypeAdd(*a, *b, out);
}

bool prCssNumericTypeMultiply(const PrCssNumericType* a, const PrCssNumericType* b, PrCssNumericType* out) {
    return cssNumericTypeMultiply(*a, *b, out);
}

PrCssNumericType prCssNumericTypeInvert(const PrCssNumericType* type) {
    return cssNumericTypeInvert(*type);
}

PrStatus prCssValueTo(const PrCssValue* value, uint32_t node, PrCssUnit unit, double* out) {
    return cssMathTo(cssValueRun(valueRead(value, node)), node, unit, out) ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus prCssValueToSum(const PrCssValue* value, uint32_t node, const PrCssUnit* PR_NULLABLE units,
        uint32_t count, PrCssValue* out) {
    CssValueRun run = cssValueRun(valueRead(value, node));
    if (value == out)
        PR_FATAL("a value's sum written to itself");
    return valueWrite(out, [&](CssValuePool* pool) {
        return cssMathToSum(run, node, units, count, pool) ? PrStatus_Ok : PrStatus_InvalidData;
    });
}

PrStatus prCssValueTransformMatrix(const PrCssValue* value, uint32_t node, PrDomMatrix* out) {
    CssValueRun run = cssValueRun(valueRead(value, node));
    return cssTransformMatrix(cssValueSubtree(run, node), out) ? PrStatus_Ok : PrStatus_InvalidData;
}

uint32_t prCssVariableReferences(PrStringView text, PrCssVariableReference* PR_NULLABLE items, uint32_t capacity) {
    CssTokenArray tokens = {};
    CssTokenScratch scratch = {};
    cssTokenize(&tokens, &scratch, text);
    uint32_t count = cssTokenCount(&tokens);
    uint32_t size = uint32_t(text.size);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (cssTokenKind(&tokens, i) != CssTokenKind_Function || cssTokenKeyword(&tokens, i) != PrCssKeyword_Var)
            continue;
        uint32_t closer = cssTokenCloser(&tokens, i);
        uint32_t at = i + 1;
        while (at < closer && cssTokenKind(&tokens, at) == CssTokenKind_Whitespace)
            ++at;
        if (at >= closer || !cssTokenIsDashedIdent(&tokens, at))
            continue;
        PrCssVariableReference reference = {};
        reference.start = tokens.starts.data[i];
        reference.end = closer < count ? tokens.ends.data[closer] : size;
        reference.name = cssTokenValueText(&tokens, at);
        ++at;
        while (at < closer && cssTokenKind(&tokens, at) == CssTokenKind_Whitespace)
            ++at;
        if (at < closer && cssTokenKind(&tokens, at) == CssTokenKind_Comma) {
            reference.hasFallback = true;
            reference.fallbackStart = tokens.ends.data[at];
            reference.fallbackEnd = closer < count ? tokens.starts.data[closer] : size;
        }
        if (items != nullptr && found < capacity)
            items[found] = reference;
        ++found;
        i = closer;
    }
    cssTokenArrayReset(&tokens);
    cssTokenScratchReset(&scratch);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------------
// Documents

void prCssEnable(PrDomDocument* document) {
    if (!domDocumentSubscribe(document, cssDocumentHooks(), nullptr))
        return;
    // The owners of sheets that the document already holds take their sheets, as if they were inserted now.
    for (PrDomNode* node = document->first; node != nullptr; node = domNodeNext(document, node)) {
        if (PrDomElement* element = domElementFromNode(node)) {
            cssStyleBlockUpdate(element, DomInsertedBy_Parser);
        } else {
            cssXmlStylesheetInserted(node);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Selectors (dom/#dom-parentnode-queryselector, dom/#dom-element-matches)

// dom/#dom-parentnode-queryselector
PrStatus prCssNodeQuerySelector(PrDomNode* node, PrStringView selectors, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrCssSelectors* parsed = cssDomSelectors(node->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssNodeQuerySelectorCompiled(node, parsed);
    return PrStatus_Ok;
}

// dom/#dom-parentnode-queryselectorall
PrStatus prCssNodeQuerySelectorAll(PrDomNode* node, PrStringView selectors, PrDomNodeList* list) {
    const PrCssSelectors* parsed = cssDomSelectors(node->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    prCssNodeQuerySelectorAllCompiled(node, parsed, list);
    return PrStatus_Ok;
}

PrStatus prCssSelectorsCreate(PrStringView text, PrCssSelectors* PR_NULLABLE* PR_NONNULL out) {
    *out = cssParseDomSelectors(text);
    return *out != nullptr ? PrStatus_Ok : PrStatus_Syntax;
}

void prCssSelectorsDestroy(PrCssSelectors* PR_NULLABLE selectors) {
    cssSelectorsDestroy(selectors);
}

PrDomElement* PR_NULLABLE prCssNodeQuerySelectorCompiled(PrDomNode* node, const PrCssSelectors* selectors) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, domElementFromNode(node), node->document, node);
    PrDomElement* found = cssSelectorMatcherNext(&matcher, node, node);
    cssSelectorMatcherReset(&matcher);
    return found;
}

void prCssNodeQuerySelectorAllCompiled(PrDomNode* node, const PrCssSelectors* selectors, PrDomNodeList* list) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, domElementFromNode(node), node->document, node);
    Array<PrDomNode* PR_NONNULL> found;
    for (PrDomElement* element = cssSelectorMatcherNext(&matcher, node, node); element != nullptr;
            element = cssSelectorMatcherNext(&matcher, node, element)) {
        found.push(element);
    }
    cssSelectorMatcherReset(&matcher);
    domNodeListAssign(list, found.data, found.count);
    found.reset();
}

PrStatus prCssElementClosest(PrDomElement* element, PrStringView selectors, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrCssSelectors* parsed = cssDomSelectors(element->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssElementClosestCompiled(element, parsed);
    return PrStatus_Ok;
}

PrStatus prCssElementMatches(const PrDomElement* element, PrStringView selectors, bool* out) {
    const PrCssSelectors* parsed = cssDomSelectors(element->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssElementMatchesCompiled(element, parsed);
    return PrStatus_Ok;
}

// dom/#dom-element-closest
PrDomElement* PR_NULLABLE prCssElementClosestCompiled(PrDomElement* element, const PrCssSelectors* selectors) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, element, element->document, element);
    PrDomElement* found = element;
    while (found != nullptr && !cssSelectorMatcherMatches(&matcher, found))
        found = domElementFromNode(domNodeParent(found));
    cssSelectorMatcherReset(&matcher);
    return found;
}

// dom/#dom-element-matches
bool prCssElementMatchesCompiled(const PrDomElement* element, const PrCssSelectors* selectors) {
    return cssSelectorsMatch(selectors, element, element);
}

// ---------------------------------------------------------------------------------------------------------------------
// The CSS state of documents, shadow roots and elements (cssom-1/#extensions-to-the-document-or-shadow-root-interface)

PrCssStyleSheetList* prCssDocumentStyleSheets(PrDomDocument* document) {
    return cssDocumentStyleSheets(document);
}

PrStatus prCssDocumentRegisterProperty(PrDomDocument* document, const PrCssPropertyDefinition* definition) {
    return cssRegisterProperty(document, definition->name, definition->syntax, definition->inherits,
        definition->initialValue);
}

void prCssDocumentSetFontCallback(PrDomDocument* document, const PrCssFontCallback* PR_NULLABLE callback) {
    cssDocumentRoot(document)->fontCallback = callback != nullptr ? *callback : PrCssFontCallback{};
}

PrCssFontFaceSet* prCssDocumentFonts(PrDomDocument* document) {
    CssRoot* style = cssDocumentRoot(document);
    if (style->fonts == nullptr)
        style->fonts = cssFontFaceSetCreate(document);
    return style->fonts;
}

PrCssHighlightRegistry* prCssDocumentHighlights(PrDomDocument* document) {
    CssRoot* style = cssDocumentRoot(document);
    if (style->highlights == nullptr)
        style->highlights = allocZeroed<PrCssHighlightRegistry>();
    return style->highlights;
}

PrCssStyleSheetList* prCssShadowRootStyleSheets(PrDomShadowRoot* shadow) {
    if (shadow->style == nullptr)
        shadow->style = allocZeroed<CssRoot>();
    return &shadow->style->sheets;
}

PrCssStyleProperties* prCssElementStyle(PrDomElement* element) {
    return static_cast<PrCssStyleProperties*>(cssElementInlineStyle(element));
}

// ---------------------------------------------------------------------------------------------------------------------
// Matrices as CSS transforms (geometry-1/#dommatrix-parse)

PrStatus prCssMatrixFromString(PrStringView text, PrDomMatrix* out) {
    PrDomMatrix result = prDomMatrixIdentity();
    bool valid = true;
    if (text.size != 0) {
        CssParser parser = {};
        CssTokenStream stream = cssParserTokenize(&parser, text);
        CssValueSpan span;
        valid = cssParsePropertyValue(PrCssProperty_Transform, &stream, &parser.pool, &parser.match, &span);
        CssValueRun run = valid ? cssValuePoolRun(&parser.pool, span) : CssValueRun{};
        if (valid && !(run.count == 1 && cssValueIsKeyword(run.values, PrCssKeyword_None))) {
            for (uint32_t i = 0; valid && i < run.count; i += run.values[i].next) {
                PrDomMatrix next;
                valid = cssTransformMatrix(cssValueSubtree(run, i), &next);
                if (valid)
                    prDomMatrixMultiply(&result, &next);
            }
        }
        cssParserReset(&parser);
    }
    if (!valid)
        return PrStatus_Syntax;
    *out = result;
    return PrStatus_Ok;
}

PrStatus prCssMatrixToString(const PrDomMatrix* matrix, PrString* out) {
    for (double element : matrix->m) {
        if (!prF64IsFinite(element))
            return PrStatus_InvalidState;
    }
    PrTextBuffer text = {};
    prTextBufferAppend(&text, matrix->is2D ? "matrix(" : "matrix3d(");
    uint32_t count = matrix->is2D ? 6 : 16;
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppend(&text, ", ");
        cssSerializeNumber(&text, matrix->m[matrix->is2D ? kDomMatrix2DIndices[i] : i]);
    }
    prTextBufferAppendChar(&text, ')');
    *out = prTextBufferTake(&text);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSetLoadEvent

PrCssFontFaceSetLoadEvent* PR_NULLABLE prCssFontFaceSetLoadEventQuery(PrDomEvent* PR_NULLABLE object) {
    if (object != nullptr && object->kind == PrDomEventKind_FontFaceSetLoad)
        return static_cast<PrCssFontFaceSetLoadEvent*>(object);
    return nullptr;
}

PrCssFontFaceSetLoadEvent* prCssFontFaceSetLoadEventCreate(PrStringView type,
        const PrCssFontFaceSetLoadEventInit* PR_NULLABLE init) {
    PrCssFontFaceSetLoadEventInit values = init != nullptr ? *init : prCssFontFaceSetLoadEventInitDefault();
    PrCssFontFaceSetLoadEvent* event =
        domEventCreate<PrCssFontFaceSetLoadEvent>(type, values.event, PrDomEventKind_FontFaceSetLoad);
    cssFontFaceSetLoadEventSetFaces(event, values.fontfaces, values.count);
    return event;
}

uint32_t prCssFontFaceSetLoadEventFontFaces(PrCssFontFaceSetLoadEvent* event,
        PrCssFontFace* PR_NONNULL* PR_NULLABLE items, uint32_t capacity) {
    const Array<PrCssFontFace* PR_NONNULL>& faces = static_cast<const CssFontFacesData*>(event->extra)->faces;
    for (uint32_t i = 0; items != nullptr && i < faces.count && i < capacity; ++i)
        items[i] = faces.data[i];
    return faces.count;
}

// ---------------------------------------------------------------------------------------------------------------------
// TransitionEvent and AnimationEvent

PrCssTransitionEvent* PR_NULLABLE prCssTransitionEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Transition ? static_cast<PrCssTransitionEvent*>(object) : nullptr;
}

PrCssAnimationEvent* PR_NULLABLE prCssAnimationEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Animation ? static_cast<PrCssAnimationEvent*>(object) : nullptr;
}

PrCssTransitionEvent* prCssTransitionEventCreate(PrStringView type, const PrCssAnimationEventInit* PR_NULLABLE init) {
    PrCssAnimationEventInit values = init != nullptr ? *init : prCssAnimationEventInitDefault();
    PrCssTransitionEvent* event = domEventCreate<PrCssTransitionEvent>(type, values.event, PrDomEventKind_Transition);
    domEventSetAnimation(event, values.name, values.elapsedTime, values.pseudoElement);
    return event;
}

PrCssAnimationEvent* prCssAnimationEventCreate(PrStringView type, const PrCssAnimationEventInit* PR_NULLABLE init) {
    PrCssAnimationEventInit values = init != nullptr ? *init : prCssAnimationEventInitDefault();
    PrCssAnimationEvent* event = domEventCreate<PrCssAnimationEvent>(type, values.event, PrDomEventKind_Animation);
    domEventSetAnimation(event, values.name, values.elapsedTime, values.pseudoElement);
    return event;
}

PrStringView prCssTransitionEventPropertyName(const PrCssTransitionEvent* event) {
    return prStringView(&event->animation->name);
}

PrStringView prCssAnimationEventAnimationName(const PrCssAnimationEvent* event) {
    return prStringView(&event->animation->name);
}

double prCssTransitionEventElapsedTime(const PrCssTransitionEvent* event) {
    return event->animation->elapsedTime;
}

double prCssAnimationEventElapsedTime(const PrCssAnimationEvent* event) {
    return event->animation->elapsedTime;
}

PrStringView prCssTransitionEventPseudoElement(const PrCssTransitionEvent* event) {
    return prStringView(&event->animation->pseudoElement);
}

PrStringView prCssAnimationEventPseudoElement(const PrCssAnimationEvent* event) {
    return prStringView(&event->animation->pseudoElement);
}
