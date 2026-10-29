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
    TextBuffer buffer;
    serialize(&buffer);
    return textTake(&buffer);
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

CssEditResult commitDeclarations(PrCssStyleDeclaration* declaration, CssEdit* edit,
                                 CssEditResult result) {
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
    return commitRule(rule, &edit,
                      cssRulePreparePrelude(&edit, rule->data, sheetDataOf(rule), text))
           != CssEditResult_Rejected;
}

// Returns the serialization of the prelude of the rule behind a handle.
template <typename Handle>
PrString preludeText(const Handle* handle) {
    const CssRuleData* rule = dataOf(handle);
    return serialized([&](TextBuffer* out) { cssSerializePrelude(out, rule); });
}

// Returns the rule behind `handle` as a `Handle`, or null if `kind` does not describe it.
template <typename Handle>
Handle* PR_NULLABLE ruleOfKind(PrCssRule* PR_NULLABLE handle, PrCssRuleKind kind) {
    return handle != nullptr && prCssRuleKind(handle) == kind ? static_cast<Handle*>(handle) : nullptr;
}

// Returns the block behind `handle` as a `Handle`, or null if it is not the declarations of `atRule`.
template <typename Handle>
Handle* PR_NULLABLE blockOf(PrCssStyleDeclaration* PR_NULLABLE handle, CssAtRule atRule) {
    return handle != nullptr && cssStyleDeclarationBlock(handle)->rule == atRule ? static_cast<Handle*>(handle)
                                                                                  : nullptr;
}

// Returns the serialization of the values of `run`.
PrString serializedValues(CssValueRun run) {
    return serialized([&](TextBuffer* out) { cssSerializeValues(out, run); });
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
uint32_t eachMatching(CssValueRun run, Item* PR_NULLABLE items, uint32_t capacity, Matches matches,
                      Make make) {
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
uint32_t serializeEach(CssValueRun run, CssValueDef production, PrString* PR_NULLABLE items,
                       uint32_t capacity) {
    return eachMatching(
        run, items, capacity,
        [&](const CssValueNode* value) { return cssValueIsProduction(value, production); },
        [&](uint32_t i) { return serializedValues(cssValueSubtree(run, i)); });
}

// Writes to `items` the text of each top-level value of `run` of `kind` (eachMatching).
uint32_t textOfEach(CssValueRun run, PrCssValueKind kind, PrStringView* PR_NULLABLE items,
                    uint32_t capacity) {
    return eachMatching(
        run, items, capacity, [&](const CssValueNode* value) { return value->kind == kind; },
        [&](uint32_t i) { return cssValueText(run, &run.values[i]); });
}

// Returns the <container-name> or the <container-query> of the <container-condition> at `index` of an @container rule's
// prelude, or the empty string if the condition has none.
PrString conditionPart(CssValueRun run, uint32_t index, CssValueDef production) {
    CssValueRun condition = cssValueChildren(run, index);
    uint32_t part = findProduction(condition, production);
    return part != condition.count ? serializedValues(cssValueSubtree(condition, part))
                                   : prStringEmpty();
}

// Returns the part of an @container rule's condition if it has one condition, or the empty string if it has several
// (css-conditional-5/#dom-csscontainerrule-containername).
PrString soleConditionPart(const PrCssContainerRule* self, CssValueDef production) {
    CssValueRun run = preludeOf(self);
    return run.values[0].next == run.count ? conditionPart(run, 0, production) : prStringEmpty();
}

// Returns the declaration of `descriptor` in the rule's block, or null if the block declares none.
const CssDeclaration* PR_NULLABLE declarationOf(const CssRuleData* rule,
                                                PrCssDescriptor descriptor) {
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
    return serialized(
        [&](TextBuffer* out) { cssSerializeDeclarationValue(out, &rule->style, declaration); });
}

// Returns the selectors of an @scope rule's scoping root, or of its limit, or the null string if the rule has none. The
// prelude is a <scope-boundaries>: the root's selectors in parentheses if written, then `to` and the limit's selectors
// in parentheses if written. The rule's selectors are the root's, then the limit's (cssCompilePreludeSelectors).
PrString scopeSelectors(const PrCssScopeRule* self, bool limit) {
    const CssRuleData* rule = dataOf(self);
    CssValueRun run = cssValueListRun(&rule->prelude);
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
    if (!written[limit] || rule->selectors == nullptr)
        return prStringNull();
    return serialized([&](TextBuffer* out) {
        cssSerializeSelectorRange(out, rule->selectors, limit ? counts[0] : 0, counts[limit]);
    });
}

// Returns the index of the entry that `key` names in a feature value block, or the block's count if it holds none.
uint32_t featureIndex(const CssDeclarationBlock* block, PrStringView key) {
    CssDeclaredName name = {};
    return cssNameFromText(block->rule, key, &name) ? cssDeclarationBlockFind(block, name)
                                                    : block->count;
}

// Writes the <font-feature-index>es of the feature value at `index` of a feature value block, up to `capacity` of them.
// Returns how many there are.
uint32_t featureIndexes(const CssDeclarationBlock* block, uint32_t index,
                        uint32_t* PR_NULLABLE items, uint32_t capacity) {
    CssValueRun run = cssDeclarationValue(block, &cssDeclarations(block)[index]);
    for (uint32_t i = 0; i < run.count && i < capacity; ++i)
        items[i] = saturateU32(run.values[i].number);
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
    commitRuleBlock(rule, &edit,
                    cssDeclarationBlockPrepareSet(&edit, &rule->data->style,
                                                  cssDescriptorDeclaredName(descriptor), value,
                                                  false));
}

uint32_t functionParameters(CssValueRun run, PrCssFunctionParameter* PR_NULLABLE items,
                            uint32_t capacity) {
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
        item.defaultValue =
            colon < parameter.count
                ? serializedValues(CssValueRun{parameter.values + colon + 1,
                                               parameter.count - colon - 1, parameter.text})
                : prStringNull();
        return item;
    });
}

// Serializes a mixin or apply name as an identifier, with any necessary escaping.
template <typename Handle>
PrString serializedName(const Handle* handle) {
    PrStringView name = preludeName(handle);
    return serialized([&](TextBuffer* out) { cssSerializeIdent(out, name); });
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
    CssValueRun added = {out->pool.values.data + values, out->pool.values.count - values,
                         out->pool.text.data};
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
            return fields->color.space < PrColorSpace_Count
                   && (!fields->color.legacy || fields->color.space == PrColorSpace_Srgb);
        case PrCssValueKind_Calc:         return fields->calc <= PrCssCalc_Invert;
        case PrCssValueKind_Literal:      return fields->literal != 0 && fields->literal < 0x80;
        case PrCssValueKind_Longhand:     return fields->longhand < PrCssProperty_Count;
        case PrCssValueKind_UnicodeRange:
            return fields->range.first <= fields->range.last
                   && fields->range.last <= kPrUtf8MaxCodePoint;
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
            node.aux = CssValueAuxFlags(fields->color.space
                                        | (fields->color.legacy ? CssValueAuxFlag_ColorLegacy : 0));
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
    TextBuffer names;
    Array<PrFontFamily> families;
    PrFontStyle style;
    bool smallCaps;
    TextBuffer serialization;
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
            bool normal = (property == PrCssProperty_FontWeight && out->style.weight == 400)
                          || (property == PrCssProperty_FontWidth && out->style.width == 100);
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
        if (node->kind == PrCssValueKind_Keyword
            && (cssValueKeyword(node) == PrCssKeyword_Currentcolor || cssIsSystemColor(cssValueKeyword(node))))
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

PrString prCssMediaListMediaText(const PrCssMediaList* self) {
    const CssMediaQueries* list = cssMediaListQueries(self);
    return serialized([&](TextBuffer* out) { cssSerializeMediaList(out, list); });
}

void prCssMediaListSetMediaText(PrCssMediaList* self, PrStringView value) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    commitMedia(self, &edit, cssMediaListPrepareText(&edit, cssMediaListQueries(self), value));
}

uint32_t prCssMediaListLength(const PrCssMediaList* self) {
    return cssMediaListLength(cssMediaListQueries(self));
}

PrString prCssMediaListQueryAt(const PrCssMediaList* self, uint32_t index) {
    const CssMediaQueries* list = cssMediaListQueries(self);
    if (index >= cssMediaListLength(list))
        return prStringNull();
    return serializedValues(cssMediaQueryAt(list, index));
}

void prCssMediaListAppendMedium(PrCssMediaList* self, PrStringView medium) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    commitMedia(self, &edit, cssMediaListPrepareAppend(&edit, cssMediaListQueries(self), medium));
}

PrStatus prCssMediaListDeleteMedium(PrCssMediaList* self, PrStringView medium) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    CssEditResult result = commitMedia(
        self, &edit, cssMediaListPrepareDelete(&edit, cssMediaListQueries(self), medium));
    return result == CssEditResult_Rejected ? PrStatus_NotFound : PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// MediaQueryList

PrCssMediaQueryList* PR_NULLABLE prCssMediaQueryListQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_MediaQueryList
               ? static_cast<PrCssMediaQueryList*>(object)
               : nullptr;
}

PrString prCssMediaQueryListMedia(const PrCssMediaQueryList* self) {
    const CssMediaQueries* list = &self->media;
    return serialized([&](TextBuffer* out) { cssSerializeMediaList(out, list); });
}

bool prCssMediaQueryListMatches(const PrCssMediaQueryList* self) {
    const PrCssMediaQueryList* list = self;
    return cssMediaListMatches(&list->media, list->environment);
}

// Adds a `change` listener in the same way as addEventListener() (cssom-view-1/#dom-mediaquerylist-addlistener).
void prCssMediaQueryListAddListener(PrCssMediaQueryList* self,
                                    const PrDomEventListener* PR_NULLABLE callback) {
    if (callback != nullptr)
        domListenerAdd(self, "change", callback, nullptr);
}

// cssom-view-1/#dom-mediaquerylist-removelistener
void prCssMediaQueryListRemoveListener(PrCssMediaQueryList* self,
                                       const PrDomEventListener* PR_NULLABLE callback) {
    if (callback != nullptr)
        domListenerRemove(self, "change", callback, false);
}

void prCssMediaQueryListRetain(PrCssMediaQueryList* self) {
    cssMediaQueryListRetain(self);
}

void prCssMediaQueryListRelease(PrCssMediaQueryList* PR_NULLABLE self) {
    cssMediaQueryListRelease(self);
}

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheet

PrStringView prCssStyleSheetHref(const PrCssStyleSheet* self) {
    return prStringView(&self->href);
}

PrDomNode* PR_NULLABLE prCssStyleSheetOwnerNode(PrCssStyleSheet* self) {
    return self->ownerNode;
}

PrCssStyleSheet* PR_NULLABLE prCssStyleSheetParentStyleSheet(PrCssStyleSheet* self) {
    const PrCssRule* owner = self->ownerRule;
    return owner != nullptr ? owner->sheet : nullptr;
}

PrStringView prCssStyleSheetTitle(const PrCssStyleSheet* self) {
    return prStringView(&self->title);
}

PrCssMediaList* prCssStyleSheetMedia(PrCssStyleSheet* self) {
    // An imported sheet's media is its rule's media (cssom-1/#dom-cssimportrule-media).
    return self->ownerRule != nullptr ? &self->ownerRule->media : &self->mediaList;
}

bool prCssStyleSheetDisabled(const PrCssStyleSheet* self) {
    return self->disabled;
}

void prCssStyleSheetSetDisabled(PrCssStyleSheet* self, bool value) {
    self->disabled = value;
    cssDocumentStyleChanged(cssStyleSheetDocument(self));
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
        sheet->media.queries =
            cssValueListCopy(cssValueListRun(&cssMediaListQueries(media)->queries));
    else
        cssMediaListParse(&sheet->media, init->media.string);
    return sheet;
}

void prCssStyleSheetRetain(PrCssStyleSheet* self) {
    cssStyleSheetRetain(self);
}

void prCssStyleSheetRelease(PrCssStyleSheet* PR_NULLABLE self) {
    cssStyleSheetRelease(self);
}

PrCssRule* PR_NULLABLE prCssStyleSheetOwnerRule(PrCssStyleSheet* self) {
    return self->ownerRule;
}

PrCssRuleList* prCssStyleSheetCssRules(PrCssStyleSheet* self) {
    return &self->cssRules;
}

PrStatus prCssStyleSheetInsertRule(PrCssStyleSheet* self, PrStringView rule, uint32_t index,
                                   uint32_t* PR_NULLABLE out) {
    PrStatus status = cssStyleSheetInsertRule(self, rule, index);
    if (status == PrStatus_Ok && out != nullptr)
        *out = index;
    return status;
}

PrStatus prCssStyleSheetDeleteRule(PrCssStyleSheet* self, uint32_t index) {
    return cssStyleSheetDeleteRule(self, index);
}

PrStatus prCssStyleSheetReplaceSync(PrCssStyleSheet* self, PrStringView text) {
    if (!self->constructed)
        return PrStatus_NotAllowed;

    cssStyleSheetReplace(self, text);
    return PrStatus_Ok;
}

PrStatus prCssStyleSheetAddRule(PrCssStyleSheet* self, PrStringView selector, PrStringView style,
                                const uint32_t* PR_NULLABLE index, int32_t* PR_NULLABLE out) {
    // cssom-1/#dom-cssstylesheet-addrule: the rule is the selector, then ` { `, the style and a space if the style is
    // not empty, and `}`. It is inserted at the end if no index is given. The result is always -1.
    TextBuffer rule;
    textAppend(&rule, selector);
    textAppend(&rule, " { ");
    if (style.size != 0) {
        textAppend(&rule, style);
        textAppendChar(&rule, ' ');
    }
    textAppendChar(&rule, '}');
    PrStatus status = cssStyleSheetInsertRule(self, textView(&rule),
                                              index != nullptr ? *index : self->rules.count);
    textReset(&rule);
    if (status == PrStatus_Ok && out != nullptr)
        *out = -1;
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSRule

PrString prCssRuleCssText(const PrCssRule* self) {
    const CssRuleData* rule = dataOf(self);
    return serialized([&](TextBuffer* out) { cssSerializeRule(out, rule); });
}

// The setter does nothing (cssom-1/#the-cssrule-interface). It exists because the attribute is not readonly.
void prCssRuleSetCssText(PrCssRule*, PrStringView) {}

PrCssRule* PR_NULLABLE prCssRuleParentRule(PrCssRule* self) {
    return self->parent;
}

PrCssStyleSheet* PR_NULLABLE prCssRuleParentStyleSheet(PrCssRule* self) {
    return self->sheet;
}

void prCssRuleRetain(PrCssRule* self) {
    cssRuleRetain(self);
}

void prCssRuleRelease(PrCssRule* PR_NULLABLE self) {
    cssRuleRelease(self);
}

PrCssRuleType prCssRuleType(const PrCssRule* self) {
    return cssRuleType(dataOf(self));
}

PrCssRuleKind prCssRuleKind(const PrCssRule* self) {
    return cssRuleKind(self->data);
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

PrCssRuleList* prCssGroupingRuleCssRules(PrCssGroupingRule* self) {
    return &self->cssRules;
}

PrStatus prCssGroupingRuleInsertRule(PrCssGroupingRule* self, PrStringView rule, uint32_t index,
                                     uint32_t* PR_NULLABLE out) {
    PrStatus status = cssRuleInsertRule(self, rule, index);
    if (status == PrStatus_Ok && out != nullptr)
        *out = index;
    return status;
}

PrStatus prCssGroupingRuleDeleteRule(PrCssGroupingRule* self, uint32_t index) {
    return cssRuleDeleteRule(self, index);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleRule

PrCssStyleRule* PR_NULLABLE prCssStyleRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssStyleRule>(object, PrCssRuleKind_Style);
}

PrString prCssStyleRuleSelectorText(const PrCssStyleRule* self) {
    const CssRuleData* rule = dataOf(self);
    return serialized([&](TextBuffer* out) { cssSerializeSelectorText(out, rule); });
}

void prCssStyleRuleSetSelectorText(PrCssStyleRule* self, PrStringView value) {
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(self));
    Array<CssRuleChain> outer;
    CssEditResult result = cssStyleRulePrepareSelectorText(
        &edit, cssRuleChain(self->parent, &outer), sheetDataOf(self), value);
    outer.reset();
    if (result == CssEditResult_Changed)
        cssRuleCommit(&edit, cssRuleWritableTree(self));
    cssEditEnd(&edit);
}

PrCssStyleProperties* prCssStyleRuleStyle(PrCssStyleRule* self) {
    return static_cast<PrCssStyleProperties*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSImportRule
//
// The prelude is the URL, as a <string> or a url() holding one, then `layer` or a layer() if written, then a
// <supports-import-condition> if written. The media queries after them are the rule's media list.

PrCssImportRule* PR_NULLABLE prCssImportRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssImportRule>(object, PrCssRuleKind_Import);
}

PrStringView prCssImportRuleHref(const PrCssImportRule* self) {
    return preludeName(self);
}

PrCssMediaList* prCssImportRuleMedia(PrCssImportRule* self) {
    return &self->media;
}

PrCssStyleSheet* PR_NULLABLE prCssImportRuleStyleSheet(PrCssImportRule* self) {
    return cssImportRuleSheet(self);
}

PrString prCssImportRuleLayerName(const PrCssImportRule* self) {
    // cssom-1/#dom-cssimportrule-layername: the empty string for `layer`, the layer's name for layer(), and the null
    // string for neither.
    CssValueRun run = preludeOf(self);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsKeyword(value, PrCssKeyword_Layer))
            return prStringEmpty();
        if (cssValueIsFunction(value, PrCssKeyword_Layer))
            return serializedValues(cssValueChildren(run, i));
    }
    return prStringNull();
}

PrString prCssImportRuleSupportsText(const PrCssImportRule* self) {
    // cssom-1/#dom-cssimportrule-supportstext: the contents of supports(), or the null string if the rule has none. All
    // the selectors that the rule holds belong to the condition.
    const CssRuleData* rule = dataOf(self);
    CssValueRun run = cssValueListRun(&rule->prelude);
    uint32_t condition = findProduction(run, CssValueDef_SupportsImportCondition);
    if (condition == run.count)
        return prStringNull();
    return serialized([&](TextBuffer* out) {
        cssSerializePreludeValues(out, cssValueChildren(run, condition + 1), false, rule->selectors,
                                  0);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSNamespaceRule

PrCssNamespaceRule* PR_NULLABLE prCssNamespaceRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssNamespaceRule>(object, PrCssRuleKind_Namespace);
}

PrStringView prCssNamespaceRuleNamespaceUri(const PrCssNamespaceRule* self) {
    PrStringView prefix;
    PrStringView url;
    cssNamespaceRuleParts(dataOf(self), &prefix, &url);
    return url;
}

PrStringView prCssNamespaceRulePrefix(const PrCssNamespaceRule* self) {
    PrStringView prefix;
    PrStringView url;
    cssNamespaceRuleParts(dataOf(self), &prefix, &url);
    return prefix.data != nullptr ? prefix : "";
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSConditionRule

PrCssConditionRule* PR_NULLABLE prCssConditionRuleQuery(PrCssRule* PR_NULLABLE object) {
    const CssRuleData* rule = dataOf(object);
    bool conditional = rule != nullptr && rule->kind == CssRuleDataKind_At
                       && cssAtRuleConditional(kCssAtRuleAlias[rule->atRule]);
    return conditional ? static_cast<PrCssConditionRule*>(object) : nullptr;
}

PrString prCssConditionRuleConditionText(const PrCssConditionRule* self) {
    // css-conditional-3/#the-cssmediarule-interface: a media rule's condition text is its media list's serialization. A
    // supports rule's condition text is the condition as written.
    return preludeText(self);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMediaRule

PrCssMediaRule* PR_NULLABLE prCssMediaRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMediaRule>(object, PrCssRuleKind_Media);
}

PrCssMediaList* prCssMediaRuleMedia(PrCssMediaRule* self) {
    return &self->media;
}

bool prCssMediaRuleMatches(const PrCssMediaRule* self) {
    const PrCssRule* rule = self;
    const CssMediaEnvironment* environment =
        rule->sheet != nullptr ? cssStyleSheetMedia(rule->sheet) : nullptr;
    return environment != nullptr && cssMediaListMatches(&rule->data->media, environment);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsRule

PrCssSupportsRule* PR_NULLABLE prCssSupportsRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssSupportsRule>(object, PrCssRuleKind_Supports);
}

bool prCssSupportsRuleMatches(const PrCssSupportsRule* self) {
    return cssSupportsEvaluate(cssValueListRun(&dataOf(self)->prelude));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContainerRule
//
// The prelude is <container-condition>s joined by ','s. Each is a <container-name>, a <container-query>, or both.

PrCssContainerRule* PR_NULLABLE prCssContainerRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContainerRule>(object, PrCssRuleKind_Container);
}

PrString prCssContainerRuleContainerName(const PrCssContainerRule* self) {
    return soleConditionPart(self, CssValueDef_ContainerName);
}

PrString prCssContainerRuleContainerQuery(const PrCssContainerRule* self) {
    return soleConditionPart(self, CssValueDef_ContainerQuery);
}

uint32_t prCssContainerRuleConditions(const PrCssContainerRule* self,
                                      PrCssContainerCondition* PR_NULLABLE items,
                                      uint32_t capacity) {
    CssValueRun run = preludeOf(self);
    auto matches = [](const CssValueNode* value) {
        return cssValueIsProduction(value, CssValueDef_ContainerCondition);
    };
    return eachMatching(run, items, capacity, matches, [&](uint32_t i) {
        return PrCssContainerCondition{conditionPart(run, i, CssValueDef_ContainerName),
                                       conditionPart(run, i, CssValueDef_ContainerQuery)};
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSNestedDeclarations

PrCssNestedDeclarations* PR_NULLABLE prCssNestedDeclarationsQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssNestedDeclarations>(object, PrCssRuleKind_NestedDeclarations);
}

PrCssStyleProperties* prCssNestedDeclarationsStyle(PrCssNestedDeclarations* self) {
    return static_cast<PrCssStyleProperties*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceRule

PrCssFontFaceRule* PR_NULLABLE prCssFontFaceRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontFaceRule>(object, PrCssRuleKind_FontFace);
}

PrCssFontFaceDescriptors* prCssFontFaceRuleStyle(PrCssFontFaceRule* self) {
    return static_cast<PrCssFontFaceDescriptors*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPageRule

PrCssPageRule* PR_NULLABLE prCssPageRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPageRule>(object, PrCssRuleKind_Page);
}

PrString prCssPageRuleSelectorText(const PrCssPageRule* self) {
    return preludeText(self);
}

void prCssPageRuleSetSelectorText(PrCssPageRule* self, PrStringView value) {
    setPrelude(self, value);
}

PrCssPageDescriptors* prCssPageRuleStyle(PrCssPageRule* self) {
    return static_cast<PrCssPageDescriptors*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMarginRule

PrStringView prCssMarginName(PrCssMargin margin) {
    return cssAtRuleName(kMarginRules[margin]);
}

PrCssMarginRule* PR_NULLABLE prCssMarginRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMarginRule>(object, PrCssRuleKind_Margin);
}

PrCssMargin prCssMarginRuleMargin(const PrCssMarginRule* self) {
    CssAtRule rule = dataOf(self)->atRule;
    for (PrCssMargin margin = 0; margin < PrCssMargin_Count; ++margin) {
        if (kMarginRules[margin] == rule)
            return margin;
    }
    PR_ASSERT(false);
    return PrCssMargin_TopLeftCorner;
}

PrCssStyleDeclaration* prCssMarginRuleStyle(PrCssMarginRule* self) {
    return &self->style;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframesRule

PrCssKeyframesRule* PR_NULLABLE prCssKeyframesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssKeyframesRule>(object, PrCssRuleKind_Keyframes);
}

PrStringView prCssKeyframesRuleName(const PrCssKeyframesRule* self) {
    return preludeName(self);
}

void prCssKeyframesRuleSetName(PrCssKeyframesRule* self, PrStringView value) {
    // css-animations-1/#dom-csskeyframesrule-name: the name is an identifier if it can be one. Otherwise it is a
    // string, as it is for `none` and the CSS-wide keywords.
    TextBuffer name;
    cssSerializeIdent(&name, value);
    if (!setPrelude(self, textView(&name))) {
        textClear(&name);
        cssSerializeString(&name, value);
        setPrelude(self, textView(&name));
    }
    textReset(&name);
}

PrCssRuleList* prCssKeyframesRuleCssRules(PrCssKeyframesRule* self) {
    return &self->cssRules;
}

uint32_t prCssKeyframesRuleLength(const PrCssKeyframesRule* self) {
    return dataOf(self)->children.count;
}

PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleKeyframeRuleAt(PrCssKeyframesRule* self,
                                                                uint32_t index) {
    return index < self->children.count ? static_cast<PrCssKeyframeRule*>(cssRuleChild(self, index)) : nullptr;
}

void prCssKeyframesRuleAppendRule(PrCssKeyframesRule* self, PrStringView rule) {
    // css-animations-1/#dom-csskeyframesrule-appendrule: text that is not a keyframe rule does nothing.
    cssRuleInsertRule(self, rule, self->children.count);
}

void prCssKeyframesRuleDeleteRule(PrCssKeyframesRule* self, PrStringView select) {
    uint32_t index = cssKeyframesFind(self->data, select, cssRuleParser(self));
    if (index != self->children.count)
        cssRuleDeleteRule(self, index);
}

PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleFindRule(PrCssKeyframesRule* self,
                                                          PrStringView select) {
    uint32_t index = cssKeyframesFind(self->data, select, cssRuleParser(self));
    if (index == self->children.count)
        return nullptr;
    return static_cast<PrCssKeyframeRule*>(cssRuleChild(self, index));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframeRule

PrCssKeyframeRule* PR_NULLABLE prCssKeyframeRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssKeyframeRule>(object, PrCssRuleKind_Keyframe);
}

PrString prCssKeyframeRuleKeyText(const PrCssKeyframeRule* self) {
    return preludeText(self);
}

PrStatus prCssKeyframeRuleSetKeyText(PrCssKeyframeRule* self, PrStringView value) {
    return setPrelude(self, value) ? PrStatus_Ok : PrStatus_Syntax;
}

PrCssStyleProperties* prCssKeyframeRuleStyle(PrCssKeyframeRule* self) {
    return static_cast<PrCssStyleProperties*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerBlockRule

PrCssLayerBlockRule* PR_NULLABLE prCssLayerBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssLayerBlockRule>(object, PrCssRuleKind_LayerBlock);
}

PrString prCssLayerBlockRuleName(const PrCssLayerBlockRule* self) {
    // css-cascade-5/#dom-csslayerblockrule-name: the empty string for an anonymous layer.
    return serializedValues(preludeOf(self));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerStatementRule

PrCssLayerStatementRule* PR_NULLABLE prCssLayerStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssLayerStatementRule>(object, PrCssRuleKind_LayerStatement);
}

uint32_t prCssLayerStatementRuleNameList(const PrCssLayerStatementRule* self,
                                         PrString* PR_NULLABLE items, uint32_t capacity) {
    return serializeEach(preludeOf(self), CssValueDef_LayerName, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSScopeRule

PrCssScopeRule* PR_NULLABLE prCssScopeRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssScopeRule>(object, PrCssRuleKind_Scope);
}

PrString prCssScopeRuleStart(const PrCssScopeRule* self) {
    return scopeSelectors(self, false);
}

PrString prCssScopeRuleEnd(const PrCssScopeRule* self) {
    return scopeSelectors(self, true);
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

PrStringView prCssCounterStyleRuleName(const PrCssCounterStyleRule* self) {
    return preludeName(self);
}

void prCssCounterStyleRuleSetName(PrCssCounterStyleRule* self, PrStringView value) {
    // css-counter-styles-3/#dom-csscounterstylerule-name: setting a name that the rule cannot accept does nothing, and
    // a predefined style's name is lowercased. The value must be a <counter-style-name> as written, which the prelude's
    // grammar checks.
    if (!cssCounterStyleNameDefinable(value))
        return;
    const CssCounterStyle* predefined = cssCounterStylePredefined(value);
    setPrelude(self, predefined != nullptr ? predefined->name : value);
}

PrString prCssCounterStyleRuleSystem(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleSystem);
}

void prCssCounterStyleRuleSetSystem(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleSystem, value);
}

PrString prCssCounterStyleRuleSymbols(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleSymbols);
}

void prCssCounterStyleRuleSetSymbols(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleSymbols, value);
}

PrString prCssCounterStyleRuleAdditiveSymbols(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleAdditiveSymbols);
}

void prCssCounterStyleRuleSetAdditiveSymbols(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleAdditiveSymbols, value);
}

PrString prCssCounterStyleRuleNegative(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleNegative);
}

void prCssCounterStyleRuleSetNegative(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleNegative, value);
}

PrString prCssCounterStyleRulePrefix(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStylePrefix);
}

void prCssCounterStyleRuleSetPrefix(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStylePrefix, value);
}

PrString prCssCounterStyleRuleSuffix(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleSuffix);
}

void prCssCounterStyleRuleSetSuffix(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleSuffix, value);
}

PrString prCssCounterStyleRuleRange(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleRange);
}

void prCssCounterStyleRuleSetRange(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleRange, value);
}

PrString prCssCounterStyleRulePad(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStylePad);
}

void prCssCounterStyleRuleSetPad(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStylePad, value);
}

PrString prCssCounterStyleRuleSpeakAs(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleSpeakAs);
}

void prCssCounterStyleRuleSetSpeakAs(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleSpeakAs, value);
}

PrString prCssCounterStyleRuleFallback(const PrCssCounterStyleRule* self) {
    return descriptorValue(self, PrCssDescriptor_CounterStyleFallback);
}

void prCssCounterStyleRuleSetFallback(PrCssCounterStyleRule* self, PrStringView value) {
    setCounterStyleDescriptor(self, PrCssDescriptor_CounterStyleFallback, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPropertyRule

PrCssPropertyRule* PR_NULLABLE prCssPropertyRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPropertyRule>(object, PrCssRuleKind_Property);
}

PrStringView prCssPropertyRuleName(const PrCssPropertyRule* self) {
    return preludeName(self);
}

PrStringView prCssPropertyRuleSyntax(const PrCssPropertyRule* self) {
    // css-properties-values-api-1/#dom-csspropertyrule-syntax: the <string>'s value.
    const CssRuleData* rule = dataOf(self);
    const CssDeclaration* syntax = declarationOf(rule, PrCssDescriptor_PropertySyntax);
    CssValueRun value = cssDeclarationValue(&rule->style, syntax);
    return cssValueText(value, value.values);
}

bool prCssPropertyRuleInherits(const PrCssPropertyRule* self) {
    const CssRuleData* rule = dataOf(self);
    CssValueRun value =
        cssDeclarationValue(&rule->style, declarationOf(rule, PrCssDescriptor_PropertyInherits));
    return cssValueKeyword(&value.values[0]) == PrCssKeyword_True;
}

PrString prCssPropertyRuleInitialValue(const PrCssPropertyRule* self) {
    // The null string if the rule declares none.
    if (declarationOf(dataOf(self), PrCssDescriptor_PropertyInitialValue) == nullptr)
        return prStringNull();
    return descriptorValue(self, PrCssDescriptor_PropertyInitialValue);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesRule

PrCssFontFeatureValuesRule* PR_NULLABLE
prCssFontFeatureValuesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontFeatureValuesRule>(object, PrCssRuleKind_FontFeatureValues);
}

PrString prCssFontFeatureValuesRuleFontFamily(const PrCssFontFeatureValuesRule* self) {
    return serializedValues(preludeOf(self));
}

void prCssFontFeatureValuesRuleSetFontFamily(PrCssFontFeatureValuesRule* self, PrStringView value) {
    setPrelude(self, value);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleAnnotation(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_Annotation);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleOrnaments(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_Ornaments);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStylistic(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_Stylistic);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleSwash(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_Swash);
}

PrCssFontFeatureValuesMap*
prCssFontFeatureValuesRuleCharacterVariant(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_CharacterVariant);
}

PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStyleset(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_Styleset);
}

PrCssFontFeatureValuesMap*
prCssFontFeatureValuesRuleHistoricalForms(PrCssFontFeatureValuesRule* self) {
    return featureMap(self, CssAtRule_HistoricalForms);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesMap
//
// The map is a feature value block. Its declarations are its entries, each a name and the <font-feature-index>es of its
// value (css-fonts-4/#font-feature-values-syntax).

uint32_t prCssFontFeatureValuesMapSize(const PrCssFontFeatureValuesMap* self) {
    return dataOf(self)->style.count;
}

PrStringView prCssFontFeatureValuesMapKeyAt(const PrCssFontFeatureValuesMap* self, uint32_t index) {
    const CssDeclarationBlock* block = &dataOf(self)->style;
    return cssDeclarationName(block, &cssDeclarations(block)[index]);
}

uint32_t prCssFontFeatureValuesMapValueAt(const PrCssFontFeatureValuesMap* self, uint32_t index,
                                          uint32_t* PR_NULLABLE items, uint32_t capacity) {
    return featureIndexes(&dataOf(self)->style, index, items, capacity);
}

bool prCssFontFeatureValuesMapHas(const PrCssFontFeatureValuesMap* self, PrStringView key) {
    const CssDeclarationBlock* block = &dataOf(self)->style;
    return featureIndex(block, key) != block->count;
}

uint32_t prCssFontFeatureValuesMapGet(const PrCssFontFeatureValuesMap* self, PrStringView key,
                                      uint32_t* PR_NULLABLE items, uint32_t capacity) {
    const CssDeclarationBlock* block = &dataOf(self)->style;
    return featureIndexes(block, featureIndex(block, key), items, capacity);
}

bool prCssFontFeatureValuesMapDelete(PrCssFontFeatureValuesMap* self, PrStringView key) {
    PrCssRule* rule = self;
    CssDeclaredName name = {};
    if (!cssNameFromText(rule->data->style.rule, key, &name))
        return false;
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    return commitRuleBlock(
               rule, &edit,
               cssDeclarationBlockPrepareRemove(&edit, &rule->data->style, name, nullptr))
           == CssEditResult_Changed;
}

void prCssFontFeatureValuesMapClear(PrCssFontFeatureValuesMap* self) {
    if (self->data->style.count != 0)
        cssDeclarationBlockReset(&cssRuleWritable(self)->style);
}

void prCssFontFeatureValuesMapSet(PrCssFontFeatureValuesMap* self, PrStringView key,
                                  const uint32_t* PR_NULLABLE values, uint32_t count) {
    // css-fonts-4/#om-fontfeaturevalues: the entry's value is the indexes, if the map's feature type accepts that many.
    // Setting any other count does nothing.
    TextBuffer value;
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            textAppendChar(&value, ' ');
        cssSerializeNumber(&value, double(values[i]));
    }
    PrCssRule* rule = self;
    CssDeclaredName name = {};
    if (cssNameFromText(rule->data->style.rule, key, &name)) {
        CssEdit edit;
        cssEditBegin(&edit, cssRuleParser(rule));
        commitRuleBlock(rule, &edit,
                        cssDeclarationBlockPrepareSet(&edit, &rule->data->style, name,
                                                      textView(&value), false));
    }
    textReset(&value);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontPaletteValuesRule

PrCssFontPaletteValuesRule* PR_NULLABLE
prCssFontPaletteValuesRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFontPaletteValuesRule>(object, PrCssRuleKind_FontPaletteValues);
}

PrStringView prCssFontPaletteValuesRuleName(const PrCssFontPaletteValuesRule* self) {
    return preludeName(self);
}

PrString prCssFontPaletteValuesRuleFontFamily(const PrCssFontPaletteValuesRule* self) {
    return descriptorValue(self, PrCssDescriptor_FontPaletteValuesFontFamily);
}

PrString prCssFontPaletteValuesRuleBasePalette(const PrCssFontPaletteValuesRule* self) {
    return descriptorValue(self, PrCssDescriptor_FontPaletteValuesBasePalette);
}

PrString prCssFontPaletteValuesRuleOverrideColors(const PrCssFontPaletteValuesRule* self) {
    return descriptorValue(self, PrCssDescriptor_FontPaletteValuesOverrideColors);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSPositionTryRule

PrCssPositionTryRule* PR_NULLABLE prCssPositionTryRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssPositionTryRule>(object, PrCssRuleKind_PositionTry);
}

PrStringView prCssPositionTryRuleName(const PrCssPositionTryRule* self) {
    return preludeName(self);
}

PrCssPositionTryDescriptors* prCssPositionTryRuleStyle(PrCssPositionTryRule* self) {
    return static_cast<PrCssPositionTryDescriptors*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSViewTransitionRule

PrCssViewTransitionRule* PR_NULLABLE prCssViewTransitionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssViewTransitionRule>(object, PrCssRuleKind_ViewTransition);
}

PrString prCssViewTransitionRuleNavigation(const PrCssViewTransitionRule* self) {
    return descriptorValue(self, PrCssDescriptor_ViewTransitionNavigation);
}

uint32_t prCssViewTransitionRuleTypes(const PrCssViewTransitionRule* self,
                                      PrStringView* PR_NULLABLE items, uint32_t capacity) {
    // css-view-transitions-2/#dom-cssviewtransitionrule-types: the names that the types descriptor declares. `none`
    // declares no names.
    const CssRuleData* rule = dataOf(self);
    const CssDeclaration* types = declarationOf(rule, PrCssDescriptor_ViewTransitionTypes);
    if (types == nullptr)
        return 0;
    return textOfEach(cssDeclarationValue(&rule->style, types), PrCssValueKind_Ident, items,
                      capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionRule
//
// The prelude is the <function-token>'s name and '(', the <function-parameter>s joined by ','s, the ')', and `returns`
// and a <css-type> if written.

PrCssFunctionRule* PR_NULLABLE prCssFunctionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFunctionRule>(object, PrCssRuleKind_Function);
}

PrStringView prCssFunctionRuleName(const PrCssFunctionRule* self) {
    return preludeName(self);
}

uint32_t prCssFunctionRuleGetParameters(const PrCssFunctionRule* self,
                                        PrCssFunctionParameter* PR_NULLABLE items,
                                        uint32_t capacity) {
    return functionParameters(preludeOf(self), items, capacity);
}

PrString prCssFunctionRuleReturnType(const PrCssFunctionRule* self) {
    // css-mixins-1/#dom-cssfunctionrule-returntype: `*` if the rule writes none.
    CssValueRun run = preludeOf(self);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Returns))
            return serializedValues(CssValueRun{run.values + i + 1, run.count - i - 1, run.text});
    }
    return prStringExternal("*");
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionDeclarations

PrCssFunctionDeclarations* PR_NULLABLE
prCssFunctionDeclarationsQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssFunctionDeclarations>(object, PrCssRuleKind_FunctionDeclarations);
}

PrCssFunctionDescriptors* prCssFunctionDeclarationsStyle(PrCssFunctionDeclarations* self) {
    return static_cast<PrCssFunctionDescriptors*>(&self->style);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSCustomMediaRule

PrCssCustomMediaRule* PR_NULLABLE prCssCustomMediaRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssCustomMediaRule>(object, PrCssRuleKind_CustomMedia);
}

PrStringView prCssCustomMediaRuleName(const PrCssCustomMediaRule* self) {
    return preludeName(self);
}

PrCssMediaListOrBoolean prCssCustomMediaRuleQueryValue(PrCssCustomMediaRule* self) {
    PrCssRule* rule = self;
    CssValueRun run = cssValueListRun(&rule->data->prelude);
    if (run.count > 1 && (cssValueIsKeyword(&run.values[1], PrCssKeyword_True)
                          || cssValueIsKeyword(&run.values[1], PrCssKeyword_False))) {
        return PrCssMediaListOrBoolean{nullptr,
                                       cssValueIsKeyword(&run.values[1], PrCssKeyword_True)};
    }
    return PrCssMediaListOrBoolean{(&rule->media), false};
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsConditionRule

PrCssSupportsConditionRule* PR_NULLABLE
prCssSupportsConditionRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssSupportsConditionRule>(object, PrCssRuleKind_SupportsCondition);
}

PrStringView prCssSupportsConditionRuleName(const PrCssSupportsConditionRule* self) {
    return preludeName(self);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSMixinRule

PrCssMixinRule* PR_NULLABLE prCssMixinRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssMixinRule>(object, PrCssRuleKind_Mixin);
}

PrString prCssMixinRuleName(const PrCssMixinRule* self) {
    return serializedName(self);
}

uint32_t prCssMixinRuleGetParameters(const PrCssMixinRule* self,
                                     PrCssFunctionParameter* PR_NULLABLE items, uint32_t capacity) {
    return functionParameters(preludeOf(self), items, capacity);
}

bool prCssMixinRuleContents(const PrCssMixinRule* self) {
    return hasContents(dataOf(self));
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyBlockRule

PrCssApplyBlockRule* PR_NULLABLE prCssApplyBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssApplyBlockRule>(object, PrCssRuleKind_ApplyBlock);
}

PrString prCssApplyBlockRuleName(const PrCssApplyBlockRule* self) {
    return serializedName(self);
}

uint32_t prCssApplyBlockRuleGetArguments(const PrCssApplyBlockRule* self,
                                         PrStringView* PR_NULLABLE items, uint32_t capacity) {
    return textOfEach(preludeOf(self), PrCssValueKind_Tokens, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyStatementRule

PrCssApplyStatementRule* PR_NULLABLE prCssApplyStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssApplyStatementRule>(object, PrCssRuleKind_ApplyStatement);
}

PrString prCssApplyStatementRuleName(const PrCssApplyStatementRule* self) {
    return serializedName(self);
}

uint32_t prCssApplyStatementRuleGetArguments(const PrCssApplyStatementRule* self,
                                             PrStringView* PR_NULLABLE items, uint32_t capacity) {
    return textOfEach(preludeOf(self), PrCssValueKind_Tokens, items, capacity);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsBlockRule

PrCssContentsBlockRule* PR_NULLABLE prCssContentsBlockRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContentsBlockRule>(object, PrCssRuleKind_ContentsBlock);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsStatementRule

PrCssContentsStatementRule* PR_NULLABLE
prCssContentsStatementRuleQuery(PrCssRule* PR_NULLABLE object) {
    return ruleOfKind<PrCssContentsStatementRule>(object, PrCssRuleKind_ContentsStatement);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSRuleList

PrCssRule* PR_NULLABLE prCssRuleListRuleAt(PrCssRuleList* self, uint32_t index) {
    if (index >= cssRuleListData(self).count)
        return nullptr;
    return cssRuleListItem(self, index);
}

uint32_t prCssRuleListLength(const PrCssRuleList* self) {
    return cssRuleListData(self).count;
}

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheetList

PrCssStyleSheet* PR_NULLABLE prCssStyleSheetListStyleSheetAt(PrCssStyleSheetList* self,
                                                             uint32_t index) {
    return index < self->sheets.count ? self->sheets.data[index]
                                      : nullptr;
}

uint32_t prCssStyleSheetListLength(const PrCssStyleSheetList* self) {
    return self->sheets.count;
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

void prCssDeclarationNameReset(PrCssDeclarationName* self) {
    prStringReset(&self->author);
    *self = prCssDeclarationNameFromProperty(PrCssProperty_Count);
}

bool prCssPropertyNameFromText(PrStringView text, PrCssDeclarationName* out) {
    CssDeclaredName declared = {};
    if (!cssNameFromText(CssAtRule_None, text, &declared))
        return false;
    *out =
        PrCssDeclarationName{declared.property, declared.descriptor, prStringCopy(declared.text)};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleDeclaration

PrString prCssStyleDeclarationCssText(const PrCssStyleDeclaration* self) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(self);
    return serialized([&](TextBuffer* out) { cssSerializeDeclarationBlock(out, block); });
}

PrStatus prCssStyleDeclarationSetCssText(PrCssStyleDeclaration* self, PrStringView value) {
    if (self->readOnly)
        return PrStatus_ReadOnly;
    cssDeclarationBlockParse(cssStyleDeclarationWritable(self), value,
                             cssStyleDeclarationParser(self));
    cssStyleDeclarationUpdateOwner(self);
    return PrStatus_Ok;
}

uint32_t prCssStyleDeclarationLength(const PrCssStyleDeclaration* self) {
    return cssDeclarationBlockLength(cssStyleDeclarationBlock(self));
}

bool prCssStyleDeclarationNameFromText(const PrCssStyleDeclaration* self, PrStringView text,
                                       PrCssDeclarationName* out) {
    CssDeclaredName declared = {};
    if (!cssNameFromText(cssStyleDeclarationAtRule(self), text, &declared))
        return false;
    *out =
        PrCssDeclarationName{declared.property, declared.descriptor, prStringCopy(declared.text)};
    return true;
}

void prCssStyleDeclarationNameAt(const PrCssStyleDeclaration* self, uint32_t index,
                                 PrCssDeclarationName* out) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(self);
    // A property's or a descriptor's name comes from the generated table, and an author's name from the block.
    CssDeclarationItem item = cssDeclarationBlockItem(block, index);
    bool named = item.property != PrCssProperty_Count
                 || item.declaration->descriptor != PrCssDescriptor_None;
    *out = PrCssDeclarationName{item.property, item.declaration->descriptor,
                                named ? prStringNull()
                                      : prStringCopy(cssDeclarationName(block, item.declaration))};
}

PrString prCssStyleDeclarationGetPropertyValue(const PrCssStyleDeclaration* self,
                                               const PrCssDeclarationName* name) {
    // A legacy name alias reads the property that it is an alias of (css-cascade-5/#legacy-name-alias).
    PrCssProperty property = name->property < PrCssProperty_Count ? kCssPropertyAlias[name->property]
                                                                  : name->property;
    if (self->serializeLonghand != nullptr && property < PrCssProperty_Count
        && kCssPropertyLonghands[property].count == 0) {
        TextBuffer text{};
        if (self->serializeLonghand(self, property, &text))
            return textTake(&text);
        textReset(&text);
    }
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(self);
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(block->rule, name, &declared))
        return prStringEmpty();
    return serialized([&](TextBuffer* out) { cssSerializePropertyValue(out, block, declared); });
}

bool prCssStyleDeclarationIsImportant(const PrCssStyleDeclaration* self,
                                      const PrCssDeclarationName* name) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(self);
    CssDeclaredName declared = {};
    return cssDeclaredNameFromName(block->rule, name, &declared) && cssDeclarationBlockImportant(block, declared);
}

PrStatus prCssStyleDeclarationSetProperty(PrCssStyleDeclaration* self,
                                          const PrCssDeclarationName* name, PrStringView value,
                                          bool important) {
    if (self->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(self)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(self));
    CssEditResult result = commitDeclarations(
        self, &edit,
        cssDeclarationBlockPrepareSet(&edit, cssStyleDeclarationBlock(self), declared, value,
                                      important));
    return result == CssEditResult_Rejected ? PrStatus_InvalidData : PrStatus_Ok;
}

PrStatus prCssStyleDeclarationRemoveProperty(PrCssStyleDeclaration* self,
                                             const PrCssDeclarationName* name,
                                             PrString* PR_NULLABLE out) {
    if (self->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(self)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(self));
    commitDeclarations(self, &edit,
                       cssDeclarationBlockPrepareRemove(
                           &edit, cssStyleDeclarationBlock(self), declared, out));
    return PrStatus_Ok;
}

PrCssRule* PR_NULLABLE prCssStyleDeclarationParentRule(PrCssStyleDeclaration* self) {
    return self->parentRule;
}

PrStatus prCssStyleDeclarationValue(const PrCssStyleDeclaration* self,
                                    const PrCssDeclarationName* name, PrCssValue* out) {
    const CssDeclarationBlock* block = cssStyleDeclarationBlock(self);
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(block->rule, name, &declared))
        return PrStatus_InvalidData;
    return valueWrite(out, [&](CssValuePool* pool) {
        cssDeclarationBlockValue(block, declared, pool, nullptr);
        return PrStatus_Ok;
    });
}

PrStatus prCssStyleDeclarationSetValue(PrCssStyleDeclaration* self,
                                       const PrCssDeclarationName* name, const PrCssValue* value,
                                       bool important) {
    if (self->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(self)->rule, name, &declared))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(self));
    CssEditResult result =
        cssDeclarationBlockPrepareSetValue(&edit, cssStyleDeclarationBlock(self), declared,
                                           cssValueRun(valueRead(value)), important, false);
    return commitDeclarations(self, &edit, result) == CssEditResult_Rejected
               ? PrStatus_InvalidData
               : PrStatus_Ok;
}

PrStatus prCssStyleDeclarationAppendValue(PrCssStyleDeclaration* self,
                                          const PrCssDeclarationName* name,
                                          const PrCssValue* value) {
    if (self->readOnly)
        return PrStatus_ReadOnly;
    CssDeclaredName declared = {};
    if (!cssDeclaredNameFromName(cssStyleDeclarationBlock(self)->rule, name, &declared)
        || declared.property == PrCssProperty_Count || !listValued(declared.property))
        return PrStatus_InvalidData;
    CssEdit edit;
    cssEditBegin(&edit, cssStyleDeclarationParser(self));
    CssEditResult result =
        cssDeclarationBlockPrepareSetValue(&edit, cssStyleDeclarationBlock(self), declared,
                                           cssValueRun(valueRead(value)), false, true);
    return commitDeclarations(self, &edit, result) == CssEditResult_Rejected
               ? PrStatus_InvalidData
               : PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleProperties

PrCssStyleProperties* PR_NULLABLE
prCssStylePropertiesQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    // The declarations of a style rule, a nested declarations rule, a style attribute, or a keyframe rule
    // (css-animations-1/#interface-csskeyframerule).
    PrCssStyleProperties* properties = blockOf<PrCssStyleProperties>(object, CssAtRule_None);
    return properties != nullptr ? properties
                                 : blockOf<PrCssStyleProperties>(object, CssAtRule_Keyframes);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceDescriptors, CSSPageDescriptors, CSSPositionTryDescriptors and CSSFunctionDescriptors
//
// The declarations of an at-rule's block. A caller reads and writes them like any other CSSStyleDeclaration.

PrCssFontFaceDescriptors* PR_NULLABLE
prCssFontFaceDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssFontFaceDescriptors>(object, CssAtRule_FontFace);
}

PrCssPageDescriptors* PR_NULLABLE
prCssPageDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssPageDescriptors>(object, CssAtRule_Page);
}

PrCssPositionTryDescriptors* PR_NULLABLE
prCssPositionTryDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssPositionTryDescriptors>(object, CssAtRule_PositionTry);
}

PrCssFunctionDescriptors* PR_NULLABLE
prCssFunctionDescriptorsQuery(PrCssStyleDeclaration* PR_NULLABLE object) {
    return blockOf<PrCssFunctionDescriptors>(object, CssAtRule_Function);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSS

PrString prCssEscape(PrStringView ident) {
    return serialized([&](TextBuffer* out) { cssSerializeIdent(out, ident); });
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

void prCssFontFaceRetain(PrCssFontFace* self) {
    cssFontFaceRetain(self);
}

void prCssFontFaceRelease(PrCssFontFace* PR_NULLABLE self) {
    cssFontFaceRelease(self);
}

PrString prCssFontFaceDescriptor(const PrCssFontFace* self, PrCssFontFaceDescriptor descriptor) {
    return cssFontFaceDescriptor(self, descriptor);
}

PrStatus prCssFontFaceSetDescriptor(PrCssFontFace* self, PrCssFontFaceDescriptor descriptor,
                                    PrStringView value) {
    return cssFontFaceSetDescriptor(self, descriptor, value);
}

PrCssFontFaceStatus prCssFontFaceStatus(const PrCssFontFace* self) {
    return self->status;
}

PrDomDocument* prCssFontFaceLoadingDocument(const PrCssFontFace* self) {
    return self->loading;
}

bool prCssFontFaceFailedToParse(const PrCssFontFace* self) {
    return self->unparsed;
}

PrCssFontFaceStatus prCssFontFaceLoad(PrCssFontFace* self, PrDomDocument* document) {
    cssFontFaceLoad(self, document);
    return self->status;
}

bool prCssFontFaceIsCssConnected(const PrCssFontFace* self) {
    return self->rule != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSet

PrCssFontFaceSet* PR_NULLABLE prCssFontFaceSetQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_FontFaceSet
               ? static_cast<PrCssFontFaceSet*>(object)
               : nullptr;
}

PrDomEventTarget* prCssFontFaceSetAsEventTarget(PrCssFontFaceSet* self) {
    return self;
}

uint32_t prCssFontFaceSetSize(const PrCssFontFaceSet* self) {
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(self, &faces);
    uint32_t count = faces.count;
    faces.reset();
    return count;
}

PrCssFontFace* PR_NULLABLE prCssFontFaceSetAt(PrCssFontFaceSet* self, uint32_t index) {
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssFontFaceSetFaces(self, &faces);
    PrCssFontFace* face = index < faces.count ? faces.data[index] : nullptr;
    faces.reset();
    return face;
}

bool prCssFontFaceSetHas(const PrCssFontFaceSet* self, const PrCssFontFace* face) {
    return cssFontFaceSetHas(self, face);
}

PrStatus prCssFontFaceSetAdd(PrCssFontFaceSet* self, PrCssFontFace* face) {
    return cssFontFaceSetAdd(self, face);
}

bool prCssFontFaceSetDelete(PrCssFontFaceSet* self, PrCssFontFace* face) {
    return cssFontFaceSetDelete(self, face);
}

void prCssFontFaceSetClear(PrCssFontFaceSet* self) {
    cssFontFaceSetClear(self);
}

PrCssFontFaceSetLoadStatus prCssFontFaceSetStatus(const PrCssFontFaceSet* self) {
    return self->status;
}

PrStatus prCssFontFaceSetCheck(const PrCssFontFaceSet* self, PrStringView font, bool* out) {
    Array<PrCssFontFace* PR_NONNULL> matched;
    PrStatus status = cssFontFaceSetMatch(self, font, &matched);
    *out = true;
    for (uint32_t i = 0; i < matched.count; ++i) {
        if (matched.data[i]->status != PrCssFontFaceStatus_Loaded)
            *out = false;
    }
    matched.reset();
    return status;
}

PrStatus prCssFontFaceSetLoad(PrCssFontFaceSet* self, PrDomDocument* document, PrStringView font,
                              PrCssFontFace* PR_NONNULL* PR_NULLABLE items, uint32_t capacity,
                              uint32_t* count) {
    Array<PrCssFontFace* PR_NONNULL> matched;
    PrStatus status = cssFontFaceSetMatch(self, font, &matched);
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

void prCssHighlightRetain(PrCssHighlight* self) {
    cssHighlightRetain(self);
}

void prCssHighlightRelease(PrCssHighlight* PR_NULLABLE self) {
    cssHighlightRelease(self);
}

uint32_t prCssHighlightSize(const PrCssHighlight* self) {
    return self->ranges.count;
}

PrDomRange* PR_NULLABLE prCssHighlightAt(PrCssHighlight* self, uint32_t index) {
    return index < self->ranges.count ? self->ranges.data[index]
                                           : nullptr;
}

bool prCssHighlightHas(const PrCssHighlight* self, const PrDomRange* range) {
    return self->ranges.find(
               const_cast<PrDomRange*>(range))
           != kArrayMissing;
}

void prCssHighlightAdd(PrCssHighlight* self, PrDomRange* range) {
    cssHighlightAdd(self, range);
}

bool prCssHighlightDelete(PrCssHighlight* self, const PrDomRange* range) {
    return cssHighlightDelete(self,
                              const_cast<PrDomRange*>(range));
}

void prCssHighlightClear(PrCssHighlight* self) {
    cssHighlightClear(self);
}

uint32_t prCssHighlightVersion(const PrCssHighlight* self) {
    return self->version;
}

int32_t prCssHighlightPriority(const PrCssHighlight* self) {
    return self->priority;
}

void prCssHighlightSetPriority(PrCssHighlight* self, int32_t priority) {
    self->priority = priority;
}

PrCssHighlightType prCssHighlightType(const PrCssHighlight* self) {
    return self->type;
}

void prCssHighlightSetType(PrCssHighlight* self, PrCssHighlightType type) {
    self->type = type;
}

// ---------------------------------------------------------------------------------------------------------------------
// HighlightRegistry

uint32_t prCssHighlightRegistrySize(PrCssHighlightRegistry* self) {
    return cssHighlightRegistryLive(self)->entries.count;
}

PrStringView prCssHighlightRegistryNameAt(PrCssHighlightRegistry* self, uint32_t index) {
    const PrCssHighlightRegistry* registry = cssHighlightRegistryLive(self);
    return index < registry->entries.count ? prStringView(&registry->entries.data[index]->name)
                                           : PrStringView{};
}

PrCssHighlight* PR_NULLABLE prCssHighlightRegistryAt(PrCssHighlightRegistry* self, uint32_t index) {
    const PrCssHighlightRegistry* registry = cssHighlightRegistryLive(self);
    return index < registry->entries.count ? (weakGet<PrCssHighlight>(
                                                 registry->entries.data[index]->highlight))
                                           : nullptr;
}

PrCssHighlight* PR_NULLABLE prCssHighlightRegistryGet(PrCssHighlightRegistry* self,
                                                      PrStringView name) {
    const PrCssHighlightRegistry* registry = cssHighlightRegistryLive(self);
    uint32_t at = cssHighlightRegistryFind(registry, name);
    return at != kArrayMissing ? (
                                     weakGet<PrCssHighlight>(registry->entries.data[at]->highlight))
                               : nullptr;
}

void prCssHighlightRegistrySet(PrCssHighlightRegistry* self, PrStringView name,
                               PrCssHighlight* highlight) {
    cssHighlightRegistrySet(cssHighlightRegistryLive(self), name,
                            highlight);
}

bool prCssHighlightRegistryDelete(PrCssHighlightRegistry* self, PrStringView name) {
    return cssHighlightRegistryDelete(
        cssHighlightRegistryLive(self), name);
}

void prCssHighlightRegistryClear(PrCssHighlightRegistry* self) {
    cssHighlightRegistryClear(cssHighlightRegistryLive(self));
}

uint32_t prCssHighlightRegistryVersion(PrCssHighlightRegistry* self) {
    return cssHighlightRegistryLive(self)->version;
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

uint32_t prCssPropertyLonghands(PrCssProperty property, PrCssProperty* PR_NULLABLE items,
                                uint32_t capacity) {
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

void prCssValueDestroy(PrCssValue* PR_NULLABLE self) {
    if (self == nullptr)
        return;
    cssValueReset(self);
    prMemoryFree(self);
}

void prCssValueClear(PrCssValue* self) {
    cssValueClear(self);
}

PrStatus prCssValueOpen(PrCssValue* self, const PrCssValueFields* fields) {
    PrCssValue* value = self;
    if (!fieldsValid(fields))
        return PrStatus_InvalidData;
    if (fields->kind == PrCssValueKind_Calc) {
        bool calculated = false;
        for (uint32_t i = 0; i < value->open.count && !calculated; ++i)
            calculated =
                value->pool.values.data[value->open.data[i].node].kind == PrCssValueKind_Math;
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

PrStatus prCssValueClose(PrCssValue* self) {
    PrCssValue* value = self;
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

PrStatus prCssValueAdd(PrCssValue* self, const PrCssValueFields* fields) {
    PrStatus status = prCssValueOpen(self, fields);
    return status == PrStatus_Ok ? prCssValueClose(self) : status;
}

PrStatus prCssValueAddTree(PrCssValue* self, const PrCssValue* from, uint32_t node) {
    PrCssValue* value = self;
    if (self == from)
        PR_FATAL("a value's tree added to itself");
    CssValueRun tree = cssValueSubtree(cssValueRun(valueRead(from, node)), node);
    if (value->open.count + depthOf(tree) > kPrCssValueMaxDepth)
        return PrStatus_OutOfRange;
    cssValuePoolAddRun(&value->pool, tree);
    return PrStatus_Ok;
}

uint32_t prCssValueSize(const PrCssValue* self) {
    return self->pool.values.count;
}

uint32_t prCssValueNext(const PrCssValue* self, uint32_t node) {
    return node + valueRead(self, node)->pool.values.data[node].next;
}

PrCssValueFields prCssValueFields(const PrCssValue* self, uint32_t node) {
    return fieldsOf(cssValueRun(valueRead(self, node)), node);
}

void prCssValueColorChannels(const PrCssValue* self, uint32_t node,
                             PrCssColorChannel channels[PR_NONNULL 4]) {
    CssValueRun run = cssValueRun(valueRead(self, node));
    if (run.values[node].kind != PrCssValueKind_Color)
        PR_FATAL("the channels of a value that is not a color");
    CssColorChannels of = cssColorChannelsFromValue(run, node);
    for (uint32_t i = 0; i < 4; ++i)
        channels[i] = PrCssColorChannel{of.at[i], of.meaning[i]->number, of.meaning[i]->percent,
                                        of.meaning[i]->hue};
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
        bool parsed =
            parseInto(text, pool, [](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
                return cssParseNumericValue(ts, &parser->pool, &parser->match, span);
            });
        return parsed ? PrStatus_Ok : PrStatus_Syntax;
    });
}

PrStatus prCssValueParseColor(PrStringView text, PrCssValue* out) {
    return valueWrite(out, [&](CssValuePool* pool) {
        uint32_t first = pool->values.count;
        bool parsed =
            parseInto(text, pool, [](CssTokenStream* ts, CssParser* parser, CssValueSpan* span) {
                return cssParseValue(kCssValueDefGrammarRoot[CssValueDef_Color], ts, &parser->pool,
                                     &parser->match, span);
            });
        if (!parsed)
            return PrStatus_Syntax;
        const CssValueNode* value = &pool->values.data[first];
        PrColor8 named;
        if (value->kind == PrCssValueKind_Keyword
            && prCssColorFromKeyword(cssValueKeyword(value), &named)) {
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
    bool legacy = resolved
                  && (value->kind == PrCssValueKind_Keyword
                      || (value->kind == PrCssValueKind_Color
                          && (cssValueColorLegacy(value) || color.space == PrColorSpace_Hsl
                              || color.space == PrColorSpace_Hwb)));
    cssValuePoolReset(&pool);
    if (!resolved)
        return PrStatus_Syntax;
    if (color.space == PrColorSpace_Hsl || color.space == PrColorSpace_Hwb)
        color = cssColorConvert(color, PrColorSpace_Srgb);
    *out = PrCssUsedColor{.space = color.space, .legacy = legacy, .missing = uint8_t(color.missing),
                      .components = {color.c[0], color.c[1], color.c[2]}, .alpha = color.alpha};
    return PrStatus_Ok;
}

PrString prCssUsedColorSerialize(const PrCssUsedColor* color, PrCssColorSerialization serialization) {
    CssColor value{.space = color->space, .c = {color->components[0], color->components[1], color->components[2]},
                   .alpha = color->alpha, .missing = color->missing};
    if (color->legacy && serialization == PrCssColorSerialization_HtmlCompatible
        && (color->missing & 8u) == 0 && color->alpha == 1) {
        // A missing component is zero in hex, which cannot write none.
        uint32_t bytes[3] = {};
        bool whole = true;
        for (uint32_t i = 0; i < 3; ++i) {
            double scaled = (color->missing & (1u << i)) != 0 ? 0 : color->components[i] * 255;
            double rounded = roundF64(scaled);
            whole = whole && absF64(scaled - rounded) < 1e-6 && rounded >= 0 && rounded <= 255;
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

PrStatus prCssFontCreate(PrStringView text, const PrDomElement* PR_NULLABLE element,
                         PrCssFont* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // The value of the font property, which cannot be a CSS-wide keyword or a substitution here.
    PrStringView trimmed = stringTrim(text);
    if (cssIsCssWideKeyword(cssKeywordFromText(trimmed)))
        return PrStatus_Syntax;
    CssDeclarationBlock block = {};
    if (trimmed.size == 0
        || cssDeclarationBlockSet(&block, cssPropertyDeclaredName(PrCssProperty_Font), text, false, nullptr)
               == CssEditResult_Rejected) {
        cssDeclarationBlockReset(&block);
        return PrStatus_Syntax;
    }
    // Relative values are relative to the element's font, or to a normal weight at 10px without one.
    constexpr double kDefaultSize = 10;
    CssStyleContext context{.font = cssFontBases(kDefaultSize, kDefaultSize * kCssNormalLineHeight),
                            .root = kCssInitialFontBases, .parentWeight = 400, .parentColor = PrColor{0, 0, 0, 1}};
    const CssRoot* documentStyle = element != nullptr ? element->document->style : nullptr;
    if (element != nullptr && domIsConnected(element) && documentStyle != nullptr
        && documentStyle->presentationHooks != nullptr) {
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
    textReset(&font->names);
    font->families.reset();
    textReset(&font->serialization);
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
    return textView(&font->serialization);
}

PrStatus prCssParseFilterList(PrStringView text, const PrDomElement* PR_NULLABLE element,
                              PrLayoutFilter* PR_NULLABLE out, uint32_t capacity, uint32_t* count) {
    *count = 0;
    PrStringView trimmed = stringTrim(text);
    PrCssKeyword keyword = cssKeywordFromText(trimmed);
    if (cssIsCssWideKeyword(keyword) || keyword == PrCssKeyword_None)
        return PrStatus_Syntax;
    CssValuePool specified{};
    if (trimmed.size == 0
        || !cssParseDeclaredValue(cssPropertyDeclaredName(PrCssProperty_Filter), text, &specified, nullptr)
        || specified.values.count == 0 || specified.values.data[0].kind == PrCssValueKind_Tokens) {
        cssValuePoolReset(&specified);
        return PrStatus_Syntax;
    }
    // Lengths are relative to the element's font, or to a 10px font without one, and currentcolor is its color.
    constexpr double kDefaultSize = 10;
    CssStyleContext context{.font = cssFontBases(kDefaultSize, kDefaultSize * kCssNormalLineHeight),
                            .root = kCssInitialFontBases, .parentWeight = 400, .parentColor = PrColor{0, 0, 0, 1}};
    CssColorContext colors{.current = PrColor{0, 0, 0, 1}};
    const CssRoot* documentStyle = element != nullptr ? element->document->style : nullptr;
    if (element != nullptr && domIsConnected(element) && documentStyle != nullptr
        && documentStyle->presentationHooks != nullptr) {
        colors = documentStyle->presentationHooks->colorContext(documentStyle->presentation, element);
        const CssComputedStyle* style = documentStyle->presentationHooks->styleCurrent(documentStyle->presentation,
                                                                                       element);
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

PrString prCssValueSerialize(const PrCssValue* self, uint32_t first, uint32_t end) {
    CssValueRun run = cssValueRun(valueRead(self));
    if (first > end || end > run.count)
        PR_FATAL("a value serialized past its end");
    return serialized([&](TextBuffer* out) {
        cssSerializeValues(out, CssValueRun{run.values + first, end - first, run.text});
    });
}

bool prCssValueEquals(const PrCssValue* self, uint32_t node, const PrCssValue* other,
                      uint32_t otherNode) {
    return cssValueRunEquals(cssValueSubtree(cssValueRun(valueRead(self, node)), node),
                             cssValueSubtree(cssValueRun(valueRead(other, otherNode)), otherNode));
}

PrStatus prCssValueNumericType(const PrCssValue* self, uint32_t node, PrCssNumericType* out) {
    return cssMathType(cssValueRun(valueRead(self, node)), node, out) ? PrStatus_Ok
                                                                      : PrStatus_InvalidData;
}

bool prCssNumericTypeFromUnit(PrCssUnit unit, PrCssNumericType* out) {
    return cssNumericTypeOfUnit(unit, out);
}

bool prCssNumericTypeAdd(const PrCssNumericType* a, const PrCssNumericType* b,
                         PrCssNumericType* out) {
    return cssNumericTypeAdd(*a, *b, out);
}

bool prCssNumericTypeMultiply(const PrCssNumericType* a, const PrCssNumericType* b,
                              PrCssNumericType* out) {
    return cssNumericTypeMultiply(*a, *b, out);
}

PrCssNumericType prCssNumericTypeInvert(const PrCssNumericType* type) {
    return cssNumericTypeInvert(*type);
}

PrStatus prCssValueTo(const PrCssValue* self, uint32_t node, PrCssUnit unit, double* out) {
    return cssMathTo(cssValueRun(valueRead(self, node)), node, unit, out)
               ? PrStatus_Ok
               : PrStatus_InvalidData;
}

PrStatus prCssValueToSum(const PrCssValue* self, uint32_t node, const PrCssUnit* PR_NULLABLE units,
                         uint32_t count, PrCssValue* out) {
    CssValueRun run = cssValueRun(valueRead(self, node));
    if (self == out)
        PR_FATAL("a value's sum written to itself");
    return valueWrite(out, [&](CssValuePool* pool) {
        return cssMathToSum(run, node, units, count, pool) ? PrStatus_Ok : PrStatus_InvalidData;
    });
}

PrStatus prCssValueTransformMatrix(const PrCssValue* self, uint32_t node, PrDomMatrix* out) {
    CssValueRun run = cssValueRun(valueRead(self, node));
    return cssTransformMatrix(cssValueSubtree(run, node), out) ? PrStatus_Ok
                                                               : PrStatus_InvalidData;
}

uint32_t prCssVariableReferences(PrStringView text, PrCssVariableReference* PR_NULLABLE items,
                                 uint32_t capacity) {
    CssTokenArray tokens = {};
    CssTokenScratch scratch = {};
    cssTokenize(&tokens, &scratch, text);
    uint32_t count = cssTokenCount(&tokens);
    uint32_t size = uint32_t(text.size);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (cssTokenKind(&tokens, i) != CssTokenKind_Function
            || cssTokenKeyword(&tokens, i) != PrCssKeyword_Var)
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
PrStatus prCssNodeQuerySelector(PrDomNode* self, PrStringView selectors,
                                PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrCssSelectors* parsed = cssDomSelectors(self->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssNodeQuerySelectorCompiled(self, parsed);
    return PrStatus_Ok;
}

// dom/#dom-parentnode-queryselectorall
PrStatus prCssNodeQuerySelectorAll(PrDomNode* self, PrStringView selectors, PrDomNodeList* list) {
    const PrCssSelectors* parsed = cssDomSelectors(self->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    prCssNodeQuerySelectorAllCompiled(self, parsed, list);
    return PrStatus_Ok;
}

PrStatus prCssSelectorsCreate(PrStringView text, PrCssSelectors* PR_NULLABLE* PR_NONNULL out) {
    *out = cssParseDomSelectors(text);
    return *out != nullptr ? PrStatus_Ok : PrStatus_Syntax;
}

void prCssSelectorsDestroy(PrCssSelectors* PR_NULLABLE self) {
    cssSelectorsDestroy(self);
}

PrDomElement* PR_NULLABLE prCssNodeQuerySelectorCompiled(PrDomNode* self,
                                                         const PrCssSelectors* selectors) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, domElementFromNode(self), self->document, self);
    PrDomElement* found = cssSelectorMatcherNext(&matcher, self, self);
    cssSelectorMatcherReset(&matcher);
    return found;
}

void prCssNodeQuerySelectorAllCompiled(PrDomNode* self, const PrCssSelectors* selectors, PrDomNodeList* list) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, domElementFromNode(self), self->document, self);
    Array<PrDomNode* PR_NONNULL> found;
    for (PrDomElement* element = cssSelectorMatcherNext(&matcher, self, self); element != nullptr;
         element = cssSelectorMatcherNext(&matcher, self, element)) {
        found.push(element);
    }
    cssSelectorMatcherReset(&matcher);
    domNodeListAssign(list, found.data, found.count);
    found.reset();
}

PrStatus prCssElementClosest(PrDomElement* self, PrStringView selectors,
                             PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrCssSelectors* parsed = cssDomSelectors(self->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssElementClosestCompiled(self, parsed);
    return PrStatus_Ok;
}

PrStatus prCssElementMatches(const PrDomElement* self, PrStringView selectors, bool* out) {
    const PrCssSelectors* parsed = cssDomSelectors(self->document, selectors);
    if (parsed == nullptr)
        return PrStatus_Syntax;
    *out = prCssElementMatchesCompiled(self, parsed);
    return PrStatus_Ok;
}

// dom/#dom-element-closest
PrDomElement* PR_NULLABLE prCssElementClosestCompiled(PrDomElement* self,
                                                      const PrCssSelectors* selectors) {
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, self, self->document, self);
    PrDomElement* found = self;
    while (found != nullptr && !cssSelectorMatcherMatches(&matcher, found))
        found = domElementFromNode(domNodeParent(found));
    cssSelectorMatcherReset(&matcher);
    return found;
}

// dom/#dom-element-matches
bool prCssElementMatchesCompiled(const PrDomElement* self, const PrCssSelectors* selectors) {
    return cssSelectorsMatch(selectors, self, self);
}

// ---------------------------------------------------------------------------------------------------------------------
// The CSS state of documents, shadow roots and elements (cssom-1/#extensions-to-the-document-or-shadow-root-interface)

PrCssStyleSheetList* prCssDocumentStyleSheets(PrDomDocument* self) {
    return cssDocumentStyleSheets(self);
}

PrStatus prCssDocumentRegisterProperty(PrDomDocument* self, const PrCssPropertyDefinition* definition) {
    return cssRegisterProperty(self, definition->name, definition->syntax, definition->inherits,
                               definition->initialValue);
}

void prCssDocumentSetFontCallback(PrDomDocument* self, const PrCssFontCallback* PR_NULLABLE callback) {
    cssDocumentRoot(self)->fontCallback = callback != nullptr ? *callback : PrCssFontCallback{};
}

PrCssFontFaceSet* prCssDocumentFonts(PrDomDocument* self) {
    CssRoot* style = cssDocumentRoot(self);
    if (style->fonts == nullptr)
        style->fonts = cssFontFaceSetCreate(self);
    return style->fonts;
}

PrCssHighlightRegistry* prCssDocumentHighlights(PrDomDocument* self) {
    CssRoot* style = cssDocumentRoot(self);
    if (style->highlights == nullptr)
        style->highlights = allocZeroed<PrCssHighlightRegistry>();
    return style->highlights;
}

PrCssStyleSheetList* prCssShadowRootStyleSheets(PrDomShadowRoot* self) {
    PrDomShadowRoot* shadow = self;
    if (shadow->style == nullptr)
        shadow->style = allocZeroed<CssRoot>();
    return &shadow->style->sheets;
}

PrCssStyleProperties* prCssElementStyle(PrDomElement* self) {
    return static_cast<PrCssStyleProperties*>(cssElementInlineStyle(self));
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
        valid = cssParsePropertyValue(PrCssProperty_Transform, &stream, &parser.pool, &parser.match,
                                      &span);
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

PrStatus prCssMatrixToString(const PrDomMatrix* self, PrString* out) {
    for (double element : self->m) {
        if (!isFiniteF64(element))
            return PrStatus_InvalidState;
    }
    TextBuffer text;
    textAppend(&text, self->is2D ? "matrix(" : "matrix3d(");
    uint32_t count = self->is2D ? 6 : 16;
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            textAppend(&text, ", ");
        cssSerializeNumber(&text, self->m[self->is2D ? kDomMatrix2DIndices[i] : i]);
    }
    textAppendChar(&text, ')');
    *out = textTake(&text);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSetLoadEvent

PrCssFontFaceSetLoadEvent* PR_NULLABLE
prCssFontFaceSetLoadEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_FontFaceSetLoad
               ? static_cast<PrCssFontFaceSetLoadEvent*>(object)
               : nullptr;
}

PrCssFontFaceSetLoadEvent*
prCssFontFaceSetLoadEventCreate(PrStringView type,
                                const PrCssFontFaceSetLoadEventInit* PR_NULLABLE init) {
    PrCssFontFaceSetLoadEventInit values =
        init != nullptr ? *init : prCssFontFaceSetLoadEventInitDefault();
    PrCssFontFaceSetLoadEvent* event =
        domEventCreate<PrCssFontFaceSetLoadEvent>(type, values.event, PrDomEventKind_FontFaceSetLoad);
    cssFontFaceSetLoadEventSetFaces(event, values.fontfaces, values.count);
    return event;
}

uint32_t prCssFontFaceSetLoadEventFontFaces(PrCssFontFaceSetLoadEvent* self,
                                            PrCssFontFace* PR_NONNULL* PR_NULLABLE items,
                                            uint32_t capacity) {
    const Array<PrCssFontFace* PR_NONNULL>& faces = static_cast<const CssFontFacesData*>(self->extra)->faces;
    for (uint32_t i = 0; items != nullptr && i < faces.count && i < capacity; ++i)
        items[i] = faces.data[i];
    return faces.count;
}

// ---------------------------------------------------------------------------------------------------------------------
// TransitionEvent and AnimationEvent

PrCssTransitionEvent* PR_NULLABLE prCssTransitionEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Transition
               ? static_cast<PrCssTransitionEvent*>(object)
               : nullptr;
}

PrCssAnimationEvent* PR_NULLABLE prCssAnimationEventQuery(PrDomEvent* PR_NULLABLE object) {
    return object != nullptr && object->kind == PrDomEventKind_Animation
               ? static_cast<PrCssAnimationEvent*>(object)
               : nullptr;
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

PrStringView prCssTransitionEventPropertyName(const PrCssTransitionEvent* self) {
    return prStringView(&self->animation->name);
}

PrStringView prCssAnimationEventAnimationName(const PrCssAnimationEvent* self) {
    return prStringView(&self->animation->name);
}

double prCssTransitionEventElapsedTime(const PrCssTransitionEvent* self) {
    return self->animation->elapsedTime;
}

double prCssAnimationEventElapsedTime(const PrCssAnimationEvent* self) {
    return self->animation->elapsedTime;
}

PrStringView prCssTransitionEventPseudoElement(const PrCssTransitionEvent* self) {
    return prStringView(&self->animation->pseudoElement);
}

PrStringView prCssAnimationEventPseudoElement(const PrCssAnimationEvent* self) {
    return prStringView(&self->animation->pseudoElement);
}
