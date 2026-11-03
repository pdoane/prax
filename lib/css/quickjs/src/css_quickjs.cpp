// JavaScript binding of CSS: the CSS Object Model, the Typed OM, and CSSOM View.
//
// cssom-1 - https://drafts.csswg.org/cssom/
// cssom-view-1 - https://drafts.csswg.org/cssom-view-1/

#include "pr/css_quickjs.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/css_intersection_observer.h"
#include "pr/css_resize_observer.h"
#include "pr/css_typed_om_quickjs.h"
#include "pr/css_view.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/quickjs_callback.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

JSClassID cssFontFaceClass;
JSClassID cssFontFaceSetLoadEventClass;
JSClassID cssTransitionEventClass;
JSClassID cssAnimationEventClass;
JSClassID cssHighlightClass;
JSClassID cssHighlightRegistryClass;
JSClassID cssIntersectionObserverClass;
JSClassID cssIntersectionObserverEntryClass;
JSClassID cssResizeObserverClass;
JSClassID cssResizeObserverEntryClass;
JSClassID cssResizeObserverSizeClass;

namespace {

JSClassID cssMediaListClass;
JSClassID styleSheetClass;
JSClassID cssRuleListClass;
JSClassID cssStyleDeclarationClass;
JSClassID cssFontFaceDescriptorsClass;
JSClassID cssPageDescriptorsClass;
JSClassID cssPositionTryDescriptorsClass;
JSClassID cssFunctionDescriptorsClass;
JSClassID cssFontFeatureValuesMapClass;

// The classes of the rules, indexed by the kind of rule that each presents, followed by the abstract classes.
JSClassID cssRuleClass;
JSClassID cssStyleRuleClass;
JSClassID cssImportRuleClass;
JSClassID cssNamespaceRuleClass;
JSClassID cssMediaRuleClass;
JSClassID cssSupportsRuleClass;
JSClassID cssContainerRuleClass;
JSClassID cssNestedDeclarationsClass;
JSClassID cssFontFaceRuleClass;
JSClassID cssPageRuleClass;
JSClassID cssMarginRuleClass;
JSClassID cssKeyframesRuleClass;
JSClassID cssKeyframeRuleClass;
JSClassID cssLayerBlockRuleClass;
JSClassID cssLayerStatementRuleClass;
JSClassID cssScopeRuleClass;
JSClassID cssStartingStyleRuleClass;
JSClassID cssCounterStyleRuleClass;
JSClassID cssPropertyRuleClass;
JSClassID cssFontFeatureValuesRuleClass;
JSClassID cssFontPaletteValuesRuleClass;
JSClassID cssPositionTryRuleClass;
JSClassID cssViewTransitionRuleClass;
JSClassID cssFunctionRuleClass;
JSClassID cssFunctionDeclarationsClass;
JSClassID cssCustomMediaRuleClass;
JSClassID cssSupportsConditionRuleClass;
JSClassID cssMixinRuleClass;
JSClassID cssApplyBlockRuleClass;
JSClassID cssApplyStatementRuleClass;
JSClassID cssContentsBlockRuleClass;
JSClassID cssContentsStatementRuleClass;
JSClassID cssGroupingRuleClass;
JSClassID cssConditionRuleClass;

constexpr const JSClassID* kRuleClasses[PrCssRuleKind_Count] = {
    &cssRuleClass,
    &cssStyleRuleClass,
    &cssImportRuleClass,
    &cssNamespaceRuleClass,
    &cssMediaRuleClass,
    &cssSupportsRuleClass,
    &cssContainerRuleClass,
    &cssNestedDeclarationsClass,
    &cssFontFaceRuleClass,
    &cssPageRuleClass,
    &cssMarginRuleClass,
    &cssKeyframesRuleClass,
    &cssKeyframeRuleClass,
    &cssLayerBlockRuleClass,
    &cssLayerStatementRuleClass,
    &cssScopeRuleClass,
    &cssStartingStyleRuleClass,
    &cssCounterStyleRuleClass,
    &cssPropertyRuleClass,
    &cssFontFeatureValuesRuleClass,
    &cssFontPaletteValuesRuleClass,
    &cssPositionTryRuleClass,
    &cssViewTransitionRuleClass,
    &cssFunctionRuleClass,
    &cssFunctionDeclarationsClass,
    &cssCustomMediaRuleClass,
    &cssSupportsConditionRuleClass,
    &cssMixinRuleClass,
    &cssApplyBlockRuleClass,
    &cssApplyStatementRuleClass,
    &cssContentsBlockRuleClass,
    &cssContentsStatementRuleClass,
};

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

JSClassID classifyRule(void* handle) {
    return *kRuleClasses[prCssRuleKind(static_cast<PrCssRule*>(handle))];
}

JSClassID classifyDeclarations(void* handle) {
    PrCssStyleDeclaration* declarations = static_cast<PrCssStyleDeclaration*>(handle);
    if (prCssStylePropertiesQuery(declarations) != nullptr)
        return prJsCssStylePropertiesClass;
    if (prCssFontFaceDescriptorsQuery(declarations) != nullptr)
        return cssFontFaceDescriptorsClass;
    if (prCssPageDescriptorsQuery(declarations) != nullptr)
        return cssPageDescriptorsClass;
    if (prCssPositionTryDescriptorsQuery(declarations) != nullptr)
        return cssPositionTryDescriptorsClass;
    if (prCssFunctionDescriptorsQuery(declarations) != nullptr)
        return cssFunctionDescriptorsClass;
    return cssStyleDeclarationClass;
}

JSValue ruleToJs(JSContext* ctx, JSValueConst owner, PrCssRule* PR_NULLABLE rule) {
    return jsWrapAs(ctx, cssRuleClass, owner, rule, false);
}

// The errors that a rule list's insertRule and deleteRule throw (cssom-1/#insert-a-css-rule,
// cssom-1/#remove-a-css-rule), and a declaration block's setters (cssom-1/#dom-cssstyledeclaration-setproperty).
JSValue cssThrow(JSContext* ctx, PrStatus status) {
    switch (status) {
        case PrStatus_OutOfRange: return jsDomExceptionThrow(ctx, "IndexSizeError");
        case PrStatus_Syntax: return jsDomExceptionThrow(ctx, "SyntaxError");
        case PrStatus_NotAllowed: return jsDomExceptionThrow(ctx, "HierarchyRequestError");
        case PrStatus_InvalidState: return jsDomExceptionThrow(ctx, "InvalidStateError");
        case PrStatus_NotFound: return jsDomExceptionThrow(ctx, "NotFoundError");
        case PrStatus_ReadOnly: return jsDomExceptionThrow(ctx, "NoModificationAllowedError");
        default: PR_FATAL("a CSSOM call answered a status the binding does not map");
    }
}

JSValue statusToJs(JSContext* ctx, PrStatus status) {
    return status == PrStatus_Ok ? JS_UNDEFINED : cssThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// MediaList (cssom-1/#the-medialist-interface)

JSValue mediaItem(JSContext* ctx, JSValueConst, void* handle, uint32_t index) {
    return jsTakeString(ctx, prCssMediaListQueryAt(static_cast<PrCssMediaList*>(handle), index));
}

uint32_t mediaLength(void* handle) {
    return prCssMediaListLength(static_cast<PrCssMediaList*>(handle));
}

constexpr JsIndexed kMediaIndexed = {mediaLength, mediaItem, nullptr};

// Implements appendMedium and deleteMedium, selected by the magic.
JSValue mediumMethod(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv, int magic) {
    PrCssMediaList* list = jsThis<PrCssMediaList>(ctx, listObject, cssMediaListClass);
    StringArgument medium = {ctx};
    if (list == nullptr || !jsArgCount(ctx, argc, 1) || !medium.read(argv[0]))
        return JS_EXCEPTION;
    if (magic == 0) {
        prCssMediaListAppendMedium(list, medium.value());
        return JS_UNDEFINED;
    }
    return statusToJs(ctx, prCssMediaListDeleteMedium(list, medium.value()));
}

// Implements the stringifier, which returns mediaText.
JSValue mediaToString(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    return jsStringGetter<&cssMediaListClass, prCssMediaListMediaText>(ctx, object);
}

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheet and CSSStyleSheet (cssom-1/#the-stylesheet-interface, cssom-1/#the-cssstylesheet-interface)

// cssom-1/#dom-stylesheet-type: every sheet here is a CSS style sheet.
JSValue styleSheetType(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrCssStyleSheet>(ctx, object, styleSheetClass) == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, "text/css");
}

JSValue setDisabled(JSContext* ctx, JSValueConst sheetObject, JSValueConst input) {
    PrCssStyleSheet* sheet = jsThis<PrCssStyleSheet>(ctx, sheetObject, styleSheetClass);
    if (sheet == nullptr)
        return JS_EXCEPTION;
    prCssStyleSheetSetDisabled(sheet, JS_ToBool(ctx, input) != 0);
    return JS_UNDEFINED;
}

// cssom-1/#dom-cssstylesheet-cssstylesheet. Its options are a CSSStyleSheetInit (cssom-1/#dictdef-cssstylesheetinit).
JSValue sheetConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrCssStyleSheetInit init = prCssStyleSheetInitDefault();
    StringArgument baseUrl = {ctx, PrStringView{}};
    StringArgument media = {ctx};
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, options, &present))
        return JS_EXCEPTION;
    if (present) {
        JSValue value = JS_GetPropertyStr(ctx, options, "media");
        bool read = !JS_IsException(value) && jsDictString(ctx, options, "baseURL", &baseUrl) &&
            jsDictBool(ctx, options, "disabled", &init.disabled);
        // (MediaList or DOMString) media: a list if the value is one (webidl/#js-union).
        if (read && !JS_IsUndefined(value)) {
            init.media.mediaList = jsHandleAs<PrCssMediaList>(ctx, value, cssMediaListClass);
            read = init.media.mediaList != nullptr || media.read(value);
        }
        JS_FreeValue(ctx, value);
        if (!read)
            return JS_EXCEPTION;
    }
    init.baseUrl = baseUrl.value();
    if (init.media.mediaList == nullptr)
        init.media.string = media.value();
    PrCssStyleSheet* sheet = prCssStyleSheetCreate(&init);
    // The object replaces the reference that the sheet was created with.
    JSValue object = jsWrap(ctx, prJsCssStyleSheetClass, JS_UNDEFINED, sheet, true);
    if (JS_IsException(object))
        prCssStyleSheetRelease(sheet);
    return jsConstructed(ctx, newTarget, object);
}

JSValue sheetInsertRule(JSContext* ctx, JSValueConst sheetObject, int argc, JSValueConst* argv) {
    PrCssStyleSheet* sheet = jsThis<PrCssStyleSheet>(ctx, sheetObject, prJsCssStyleSheetClass);
    StringArgument rule = {ctx};
    uint32_t index = 0;
    if (sheet == nullptr || !jsArgCount(ctx, argc, 1) || !rule.read(argv[0]) ||
            (jsArgGiven(argc, argv, 1) && JS_ToUint32(ctx, &index, argv[1]) < 0))
        return JS_EXCEPTION;
    uint32_t inserted = 0;
    PrStatus status = prCssStyleSheetInsertRule(sheet, rule.value(), index, &inserted);
    return status == PrStatus_Ok ? JS_NewUint32(ctx, inserted) : cssThrow(ctx, status);
}

// Implements deleteRule and removeRule, selected by the magic. removeRule's index is optional.
JSValue sheetDeleteRule(JSContext* ctx, JSValueConst sheetObject, int argc, JSValueConst* argv, int magic) {
    PrCssStyleSheet* sheet = jsThis<PrCssStyleSheet>(ctx, sheetObject, prJsCssStyleSheetClass);
    uint32_t index = 0;
    if (sheet == nullptr || (magic == 0 && !jsArgCount(ctx, argc, 1)) ||
            (jsArgGiven(argc, argv, 0) && JS_ToUint32(ctx, &index, argv[0]) < 0))
        return JS_EXCEPTION;
    return statusToJs(ctx, prCssStyleSheetDeleteRule(sheet, index));
}

JSValue replaceSync(JSContext* ctx, JSValueConst sheetObject, int argc, JSValueConst* argv) {
    PrCssStyleSheet* sheet = jsThis<PrCssStyleSheet>(ctx, sheetObject, prJsCssStyleSheetClass);
    StringArgument text = {ctx};
    if (sheet == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    // cssom-1/#dom-cssstylesheet-replacesync: a sheet that script did not construct, or one that an @import is loading,
    // throws a NotAllowedError.
    PrStatus status = prCssStyleSheetReplaceSync(sheet, text.value());
    return status == PrStatus_NotAllowed ? jsDomExceptionThrow(ctx, "NotAllowedError") : statusToJs(ctx, status);
}

// cssom-1/#dom-cssstylesheet-addrule. The selector and the block default to "undefined".
JSValue addRule(JSContext* ctx, JSValueConst sheetObject, int argc, JSValueConst* argv) {
    PrCssStyleSheet* sheet = jsThis<PrCssStyleSheet>(ctx, sheetObject, prJsCssStyleSheetClass);
    StringArgument selector = {ctx, "undefined"};
    StringArgument style = {ctx, "undefined"};
    uint32_t indexValue = 0;
    if (sheet == nullptr || (jsArgGiven(argc, argv, 0) && !selector.read(argv[0])) ||
            (jsArgGiven(argc, argv, 1) && !style.read(argv[1])) ||
            (jsArgGiven(argc, argv, 2) && JS_ToUint32(ctx, &indexValue, argv[2]) < 0))
        return JS_EXCEPTION;
    int32_t result = 0;
    const uint32_t* index = jsArgGiven(argc, argv, 2) ? &indexValue : nullptr;
    PrStatus status = prCssStyleSheetAddRule(sheet, selector.value(), style.value(), index, &result);
    return status == PrStatus_Ok ? JS_NewInt32(ctx, result) : cssThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSRuleList and StyleSheetList (cssom-1/#the-cssrulelist-interface, cssom-1/#the-stylesheetlist-interface)

uint32_t ruleListLength(void* handle) {
    return prCssRuleListLength(static_cast<PrCssRuleList*>(handle));
}

JSValue ruleListItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    return ruleToJs(ctx, object, prCssRuleListRuleAt(static_cast<PrCssRuleList*>(handle), index));
}

uint32_t sheetListLength(void* handle) {
    return prCssStyleSheetListLength(static_cast<PrCssStyleSheetList*>(handle));
}

JSValue sheetListItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(static_cast<PrCssStyleSheetList*>(handle), index);
    return jsWrap(ctx, prJsCssStyleSheetClass, object, sheet, false);
}

constexpr JsIndexed kRuleListIndexed = {ruleListLength, ruleListItem, nullptr};
constexpr JsIndexed kSheetListIndexed = {sheetListLength, sheetListItem, nullptr};

// ---------------------------------------------------------------------------------------------------------------------
// CSSRule and CSSGroupingRule (cssom-1/#the-cssrule-interface, cssom-1/#the-cssgroupingrule-interface)

constexpr JsConstant kRuleConstants[] = {
    {"STYLE_RULE", PrCssRuleType_Style},
    {"CHARSET_RULE", PrCssRuleType_Charset},
    {"IMPORT_RULE", PrCssRuleType_Import},
    {"MEDIA_RULE", PrCssRuleType_Media},
    {"FONT_FACE_RULE", PrCssRuleType_FontFace},
    {"PAGE_RULE", PrCssRuleType_Page},
    {"MARGIN_RULE", PrCssRuleType_Margin},
    {"NAMESPACE_RULE", PrCssRuleType_Namespace},
    {"KEYFRAMES_RULE", PrCssRuleType_Keyframes},
    {"KEYFRAME_RULE", PrCssRuleType_Keyframe},
    {"SUPPORTS_RULE", PrCssRuleType_Supports},
    {"COUNTER_STYLE_RULE", PrCssRuleType_CounterStyle},
    {"FONT_FEATURE_VALUES_RULE", PrCssRuleType_FontFeatureValues},
};

JSValue groupingInsertRule(JSContext* ctx, JSValueConst ruleObject, int argc, JSValueConst* argv) {
    PrCssGroupingRule* rule = jsThis<PrCssGroupingRule>(ctx, ruleObject, cssGroupingRuleClass);
    StringArgument text = {ctx};
    uint32_t index = 0;
    if (rule == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]) ||
            (jsArgGiven(argc, argv, 1) && JS_ToUint32(ctx, &index, argv[1]) < 0))
        return JS_EXCEPTION;
    uint32_t inserted = 0;
    PrStatus status = prCssGroupingRuleInsertRule(rule, text.value(), index, &inserted);
    return status == PrStatus_Ok ? JS_NewUint32(ctx, inserted) : cssThrow(ctx, status);
}

JSValue groupingDeleteRule(JSContext* ctx, JSValueConst ruleObject, int argc, JSValueConst* argv) {
    PrCssGroupingRule* rule = jsThis<PrCssGroupingRule>(ctx, ruleObject, cssGroupingRuleClass);
    uint32_t index = 0;
    if (rule == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return statusToJs(ctx, prCssGroupingRuleDeleteRule(rule, index));
}

// ---------------------------------------------------------------------------------------------------------------------
// The rules' own members

// css-typed-om-1/#dom-cssstylerule-stylemap: the rule's declarations as a [SameObject] StylePropertyMap.
JSValue styleMap(JSContext* ctx, JSValueConst ruleObject) {
    PrCssStyleRule* rule = jsThis<PrCssStyleRule>(ctx, ruleObject, cssStyleRuleClass);
    if (rule == nullptr)
        return JS_EXCEPTION;
    PrCssStyleDeclaration* declarations = prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(rule));
    return jsSameObject(ctx, ruleObject, "styleMap", [&] {
        return jsWrap(ctx, prJsCssStylePropertyMapClass, ruleObject, declarations, false);
    });
}

// css-conditional-5/#dom-csscontainerrule-conditions: a frozen array of CSSContainerCondition dictionaries.
JSValue containerConditions(JSContext* ctx, JSValueConst ruleObject) {
    PrCssContainerRule* rule = jsThis<PrCssContainerRule>(ctx, ruleObject, cssContainerRuleClass);
    if (rule == nullptr)
        return JS_EXCEPTION;
    RawArray<PrCssContainerCondition> items;
    items.reserve(prCssContainerRuleConditions(rule, nullptr, 0));
    items.count = prCssContainerRuleConditions(rule, items.data, items.capacity);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < items.count; ++i) {
        JSValue condition = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, condition, "name", jsNewString(ctx, &items.data[i].name));
        JS_SetPropertyStr(ctx, condition, "query", jsNewString(ctx, &items.data[i].query));
        JS_SetPropertyUint32(ctx, array, i, condition);
        prCssContainerConditionReset(&items.data[i]);
    }
    items.reset();
    JS_FreezeObject(ctx, array);
    return array;
}

JSValue marginRuleName(JSContext* ctx, JSValueConst ruleObject) {
    PrCssMarginRule* rule = jsThis<PrCssMarginRule>(ctx, ruleObject, cssMarginRuleClass);
    return rule != nullptr ? jsFixedString(ctx, prCssMarginName(prCssMarginRuleMargin(rule))) : JS_EXCEPTION;
}

JSValue setKeyText(JSContext* ctx, JSValueConst ruleObject, JSValueConst input) {
    PrCssKeyframeRule* rule = jsThis<PrCssKeyframeRule>(ctx, ruleObject, cssKeyframeRuleClass);
    StringArgument text = {ctx};
    if (rule == nullptr || !text.read(input))
        return JS_EXCEPTION;
    return statusToJs(ctx, prCssKeyframeRuleSetKeyText(rule, text.value()));
}

uint32_t keyframesLength(void* handle) {
    return prCssKeyframesRuleLength(static_cast<PrCssKeyframesRule*>(handle));
}

JSValue keyframesItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    PrCssKeyframeRule* rule = prCssKeyframesRuleKeyframeRuleAt(static_cast<PrCssKeyframesRule*>(handle), index);
    return jsWrap(ctx, cssKeyframeRuleClass, object, rule, false);
}

constexpr JsIndexed kKeyframesIndexed = {keyframesLength, keyframesItem, nullptr};

JSValue keyframesRules(JSContext* ctx, JSValueConst ruleObject) {
    PrCssKeyframesRule* rule = jsThis<PrCssKeyframesRule>(ctx, ruleObject, cssKeyframesRuleClass);
    if (rule == nullptr)
        return JS_EXCEPTION;
    return jsWrap(ctx, cssRuleListClass, ruleObject, prCssKeyframesRuleCssRules(rule), false);
}

enum {
    KeyframesMethod_AppendRule,
    KeyframesMethod_DeleteRule,
    KeyframesMethod_FindRule,
};
typedef uint8_t KeyframesMethod;

// Implements appendRule, deleteRule and findRule, selected by the magic.
JSValue keyframesMethod(JSContext* ctx, JSValueConst ruleObject, int argc, JSValueConst* argv, int magic) {
    PrCssKeyframesRule* rule = jsThis<PrCssKeyframesRule>(ctx, ruleObject, cssKeyframesRuleClass);
    StringArgument text = {ctx};
    if (rule == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    switch (KeyframesMethod(magic)) {
        case KeyframesMethod_AppendRule:
            prCssKeyframesRuleAppendRule(rule, text.value());
            return JS_UNDEFINED;
        case KeyframesMethod_DeleteRule:
            prCssKeyframesRuleDeleteRule(rule, text.value());
            return JS_UNDEFINED;
        case KeyframesMethod_FindRule:
            return jsWrap(ctx, cssKeyframeRuleClass, ruleObject, prCssKeyframesRuleFindRule(rule, text.value()), false);
    }
    return JS_UNDEFINED;
}

// The parameters of a function or mixin (css-mixins-1/#dictdef-functionparameter).
JSValue parametersToJs(JSContext* ctx, RawArray<PrCssFunctionParameter>* items) {
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < items->count; ++i) {
        PrCssFunctionParameter* item = &items->data[i];
        JSValue parameter = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, parameter, "name", jsNewString(ctx, &item->name));
        JS_SetPropertyStr(ctx, parameter, "type", jsNewString(ctx, &item->type));
        if (!prRawStringIsNull(&item->defaultValue))
            JS_SetPropertyStr(ctx, parameter, "defaultValue", jsNewString(ctx, &item->defaultValue));
        JS_SetPropertyUint32(ctx, array, i, parameter);
        prCssFunctionParameterReset(item);
    }
    items->reset();
    return array;
}

template <const JSClassID* PR_NONNULL Id, auto Write>
JSValue getParameters(JSContext* ctx, JSValueConst ruleObject, int, JSValueConst*) {
    using T = typename JsHandleOf<decltype(Write)>::Type;
    T* rule = jsThis<T>(ctx, ruleObject, *Id);
    if (rule == nullptr)
        return JS_EXCEPTION;
    RawArray<PrCssFunctionParameter> items;
    items.reserve(Write(rule, nullptr, 0));
    items.count = Write(rule, items.data, items.capacity);
    return parametersToJs(ctx, &items);
}

// mediaqueries-5/#dom-csscustommediarule-query: a MediaList, which the rule keeps, or a boolean.
JSValue customMediaQuery(JSContext* ctx, JSValueConst ruleObject) {
    PrCssCustomMediaRule* rule = jsThis<PrCssCustomMediaRule>(ctx, ruleObject, cssCustomMediaRuleClass);
    if (rule == nullptr)
        return JS_EXCEPTION;
    PrCssMediaListOrBoolean query = prCssCustomMediaRuleQueryValue(rule);
    if (query.mediaList == nullptr)
        return JS_NewBool(ctx, query.boolean);
    return jsWrap(ctx, cssMediaListClass, ruleObject, query.mediaList, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesMap (css-fonts-4/#cssfontfeaturevaluesmap), a maplike<CSSOMString, sequence<unsigned long>>

// Returns the sequence of unsigned longs that a map function writes, as an array.
template <typename Write>
JSValue valuesToJs(JSContext* ctx, Write write) {
    RawArray<uint32_t> items;
    items.reserve(write(nullptr, 0));
    items.count = write(items.data, items.capacity);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < items.count; ++i)
        JS_SetPropertyUint32(ctx, array, i, JS_NewUint32(ctx, items.data[i]));
    items.reset();
    return array;
}

// Returns the map's entries, as an array of [key, value] arrays.
JSValue mapEntries(JSContext* ctx, PrCssFontFeatureValuesMap* map) {
    JSValue entries = JS_NewArray(ctx);
    for (uint32_t index = 0; index < prCssFontFeatureValuesMapSize(map); ++index) {
        JSValue values = valuesToJs(ctx, [&](uint32_t* PR_NULLABLE items, uint32_t capacity) {
            return prCssFontFeatureValuesMapValueAt(map, index, items, capacity);
        });
        JS_SetPropertyUint32(ctx, entries, index,
            jsEntry(ctx, jsNewStringView(ctx, prCssFontFeatureValuesMapKeyAt(map, index)), values));
    }
    return entries;
}

// Implements entries, keys and values, selected by the magic, which is the JsMapPart.
JSValue mapIterate(JSContext* ctx, JSValueConst mapObject, int, JSValueConst*, int magic) {
    PrCssFontFeatureValuesMap* map = jsThis<PrCssFontFeatureValuesMap>(ctx, mapObject, cssFontFeatureValuesMapClass);
    return map != nullptr ? jsMapIterate(ctx, mapEntries(ctx, map), JsMapPart(magic)) : JS_EXCEPTION;
}

JSValue mapForEach(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv) {
    PrCssFontFeatureValuesMap* map = jsThis<PrCssFontFeatureValuesMap>(ctx, mapObject, cssFontFeatureValuesMapClass);
    return map != nullptr ? jsMapForEach(ctx, mapObject, mapEntries(ctx, map), argc, argv) : JS_EXCEPTION;
}

enum {
    MapMethod_Get,
    MapMethod_Has,
    MapMethod_Delete,
};
typedef uint8_t MapMethod;

// Implements get, has and delete, selected by the magic.
JSValue mapKeyMethod(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv, int magic) {
    PrCssFontFeatureValuesMap* map = jsThis<PrCssFontFeatureValuesMap>(ctx, mapObject, cssFontFeatureValuesMapClass);
    StringArgument key = {ctx};
    if (map == nullptr || !jsArgCount(ctx, argc, 1) || !key.read(argv[0]))
        return JS_EXCEPTION;
    switch (MapMethod(magic)) {
        case MapMethod_Get:
            if (!prCssFontFeatureValuesMapHas(map, key.value()))
                return JS_UNDEFINED;
            return valuesToJs(ctx, [&](uint32_t* PR_NULLABLE items, uint32_t capacity) {
                return prCssFontFeatureValuesMapGet(map, key.value(), items, capacity);
            });
        case MapMethod_Has:
            return JS_NewBool(ctx, prCssFontFeatureValuesMapHas(map, key.value()));
        case MapMethod_Delete:
            return JS_NewBool(ctx, prCssFontFeatureValuesMapDelete(map, key.value()));
    }
    return JS_UNDEFINED;
}

// css-fonts-4/#dom-cssfontfeaturevaluesmap-set. Its values are (unsigned long or sequence<unsigned long>).
JSValue mapSet(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv) {
    PrCssFontFeatureValuesMap* map = jsThis<PrCssFontFeatureValuesMap>(ctx, mapObject, cssFontFeatureValuesMapClass);
    StringArgument name = {ctx};
    Uint32Sequence values = {ctx};
    if (map == nullptr || !jsArgCount(ctx, argc, 2) || !name.read(argv[0]) || !values.read(argv[1]))
        return JS_EXCEPTION;
    prCssFontFeatureValuesMapSet(map, name.value(), values.values.data, values.values.count);
    return JS_UNDEFINED;
}

JSValue mapClear(JSContext* ctx, JSValueConst mapObject, int, JSValueConst*) {
    PrCssFontFeatureValuesMap* map = jsThis<PrCssFontFeatureValuesMap>(ctx, mapObject, cssFontFeatureValuesMapClass);
    if (map == nullptr)
        return JS_EXCEPTION;
    prCssFontFeatureValuesMapClear(map);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleDeclaration (cssom-1/#the-cssstyledeclaration-interface)

uint32_t declarationLength(void* handle) {
    return prCssStyleDeclarationLength(static_cast<PrCssStyleDeclaration*>(handle));
}

// Returns a declaration's name as script sees it. The name of a property or a descriptor is the runtime's single string
// for that name, and an author-defined name is created.
JSValue declarationNameToJs(JSContext* ctx, PrCssDeclarationName* name) {
    JSValue value;
    if (name->property != PrCssProperty_Count)
        value = jsFixedString(ctx, prCssPropertyName(name->property));
    else if (name->descriptor != PrCssDescriptor_None)
        value = jsFixedString(ctx, prCssDescriptorName(name->descriptor));
    else
        value = jsNewString(ctx, &name->author);
    prCssDeclarationNameReset(name);
    return value;
}

JSValue declarationItem(JSContext* ctx, JSValueConst, void* handle, uint32_t index) {
    PrCssStyleDeclaration* declarations = static_cast<PrCssStyleDeclaration*>(handle);
    // cssom-1/#dom-cssstyledeclaration-item: the empty string past the last declaration.
    if (index >= prCssStyleDeclarationLength(declarations))
        return jsFixedString(ctx, "");
    PrCssDeclarationName name = {};
    prCssStyleDeclarationNameAt(declarations, index, &name);
    return declarationNameToJs(ctx, &name);
}

// The name that a script's string refers to in a block, and whether the block accepts a declaration of that name.
struct DeclarationName {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Count);
    bool found = false;

    // Returns false, with the exception pending, if the argument is not a string. The call's scope holds the name.
    bool read(JSContext* ctx, JSValueConst input, const PrCssStyleDeclaration* PR_NULLABLE block) {
        StringArgument text = {ctx};
        if (!text.read(input))
            return false;
        if (block != nullptr)
            found = prCssStyleDeclarationNameFromText(block, text.value(), &name);
        else
            found = prCssPropertyNameFromText(text.value(), &name);
        if (found)
            jsHoldString(ctx, name.author);
        return true;
    }
};

constexpr JsIndexed kDeclarationIndexed = {declarationLength, declarationItem, nullptr};

JSValue setCssText(JSContext* ctx, JSValueConst declarationsObject, JSValueConst input) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    StringArgument text = {ctx};
    if (declarations == nullptr || !text.read(input))
        return JS_EXCEPTION;
    return statusToJs(ctx, prCssStyleDeclarationSetCssText(declarations, text.value()));
}

JSValue declarationItemMethod(JSContext* ctx, JSValueConst declarationsObject, int argc, JSValueConst* argv) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    uint32_t index = 0;
    if (declarations == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return declarationItem(ctx, declarationsObject, declarations, index);
}

enum {
    PropertyMethod_GetValue,
    PropertyMethod_GetPriority,
    PropertyMethod_Remove,
};
typedef uint8_t PropertyMethod;

// Implements getPropertyValue, getPropertyPriority and removeProperty, selected by the magic. Each returns the empty
// string for a name that the block accepts no declaration of.
JSValue propertyMethod(JSContext* ctx, JSValueConst declarationsObject, int argc, JSValueConst* argv, int magic) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    DeclarationName property = {};
    if (declarations == nullptr || !jsArgCount(ctx, argc, 1) || !property.read(ctx, argv[0], declarations))
        return JS_EXCEPTION;
    if (!property.found)
        return jsFixedString(ctx, "");
    switch (PropertyMethod(magic)) {
        case PropertyMethod_GetValue:
            return jsTakeString(ctx, prCssStyleDeclarationGetPropertyValue(declarations, &property.name));
        case PropertyMethod_GetPriority:
            return jsFixedString(ctx, prCssStyleDeclarationIsImportant(declarations, &property.name) ? "important" : "");
        case PropertyMethod_Remove: {
            PrRawString removed = {};
            PrStatus status = prCssStyleDeclarationRemoveProperty(declarations, &property.name, &removed);
            return status == PrStatus_Ok ? jsTakeString(ctx, removed) : cssThrow(ctx, status);
        }
    }
    return JS_UNDEFINED;
}

// Returns the result of setProperty() for `status`. A value that the declaration does not accept is ignored
// (cssom-1/#dom-cssstyledeclaration-setproperty).
JSValue setStatusToJs(JSContext* ctx, PrStatus status) {
    return status == PrStatus_InvalidData ? JS_UNDEFINED : statusToJs(ctx, status);
}

// cssom-1/#dom-cssstyledeclaration-setproperty. The value is [LegacyNullToEmptyString], and the priority defaults to
// the empty string.
JSValue setProperty(JSContext* ctx, JSValueConst declarationsObject, int argc, JSValueConst* argv) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    DeclarationName property = {};
    StringArgument value = {ctx};
    StringArgument priority = {ctx};
    if (declarations == nullptr || !jsArgCount(ctx, argc, 2) ||
            !property.read(ctx, argv[0], declarations) || !value.readNullable(argv[1]) ||
            (jsArgGiven(argc, argv, 2) && !priority.readNullable(argv[2])))
        return JS_EXCEPTION;
    // A priority that is neither empty nor "important", a name that the block accepts no declaration of, or a value
    // that the declaration does not accept leaves the block unchanged.
    bool important = prAsciiEqualsFold(priority.value(), "important");
    if (!property.found || (!important && priority.value().size != 0))
        return JS_UNDEFINED;
    return setStatusToJs(ctx, prCssStyleDeclarationSetProperty(declarations, &property.name, value.value(), important));
}

// ---------------------------------------------------------------------------------------------------------------------
// The attributes of CSSStyleProperties and the descriptors interfaces, one for each property or descriptor that the
// block accepts (cssom-1/#dom-cssstyleproperties-camel-cased-attribute)

// css-fonts-5/#cssfontfacedescriptors
constexpr PrStringView kFontFaceDescriptors[] = {
    "ascent-override",
    "descent-override",
    "font-display",
    "font-family",
    "font-feature-settings",
    "font-language-override",
    "font-named-instance",
    "font-size",
    "font-stretch",
    "font-style",
    "font-variation-settings",
    "font-weight",
    "font-width",
    "line-gap-override",
    "size-adjust",
    "src",
    "subscript-position-override",
    "subscript-size-override",
    "superscript-position-override",
    "superscript-size-override",
    "unicode-range",
};

// cssom-1/#csspagedescriptors
constexpr PrStringView kPageDescriptors[] = {
    "bleed",       "margin",           "margin-bottom",
    "margin-left", "margin-right",     "margin-top",
    "marks",       "page-orientation", "size",
};

// css-anchor-position-1/#csspositiontrydescriptors
constexpr PrStringView kPositionTryDescriptors[] = {
    "align-self",
    "block-size",
    "bottom",
    "height",
    "inline-size",
    "inset",
    "inset-block",
    "inset-block-end",
    "inset-block-start",
    "inset-inline",
    "inset-inline-end",
    "inset-inline-start",
    "justify-self",
    "left",
    "margin",
    "margin-block",
    "margin-block-end",
    "margin-block-start",
    "margin-bottom",
    "margin-inline",
    "margin-inline-end",
    "margin-inline-start",
    "margin-left",
    "margin-right",
    "margin-top",
    "max-block-size",
    "max-height",
    "max-inline-size",
    "max-width",
    "min-block-size",
    "min-height",
    "min-inline-size",
    "min-width",
    "place-self",
    "position-anchor",
    "position-area",
    "right",
    "top",
    "width",
};

// css-mixins-1/#cssfunctiondescriptors
constexpr PrStringView kFunctionDescriptors[] = {"result"};

// cssom-1/#dom-cssstyleproperties-cssfloat
constexpr PrStringView kFloat[] = {"float"};

// The lists through which an attribute's magic names its property. The magic holds the list in its high bits and the
// index in its low bits.
enum {
    Names_Properties,   // every supported CSS property, which the C API lists
    Names_FontFace,
    Names_Page,
    Names_PositionTry,
    Names_Function,
    Names_Float,
};
typedef uint8_t Names;

constexpr int kNamesShift = 12;

struct NameList {
    const PrStringView* names;
    uint32_t count;
};

constexpr NameList kNameLists[] = {
    {nullptr, 0},
    {kFontFaceDescriptors, countOf(kFontFaceDescriptors)},
    {kPageDescriptors, countOf(kPageDescriptors)},
    {kPositionTryDescriptors, countOf(kPositionTryDescriptors)},
    {kFunctionDescriptors, countOf(kFunctionDescriptors)},
    {kFloat, countOf(kFloat)},
};

// Returns the name of the attribute's property or descriptor. A supported property's name is found by its index, and a
// descriptor's name is the name that the block uses.
PrStringView attributeText(int magic) {
    Names names = Names(magic >> kNamesShift);
    uint32_t index = uint32_t(magic & ((1 << kNamesShift) - 1));
    return names == Names_Properties ? prCssPropertyName(PrCssProperty(index)) : kNameLists[names].names[index];
}

// Writes the name that the attribute refers to in the block. Returns false if the block accepts no declaration of it.
bool attributeName(const PrCssStyleDeclaration* declarations, int magic, PrCssDeclarationName* out) {
    if (Names(magic >> kNamesShift) == Names_Properties) {
        *out = prCssDeclarationNameFromProperty(PrCssProperty(magic & ((1 << kNamesShift) - 1)));
        return true;
    }
    return prCssStyleDeclarationNameFromText(declarations, attributeText(magic), out);
}

JSValue attributeGet(JSContext* ctx, JSValueConst declarationsObject, int magic) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    if (declarations == nullptr)
        return JS_EXCEPTION;
    PrCssDeclarationName name = {};
    if (!attributeName(declarations, magic, &name))
        return jsFixedString(ctx, "");
    JSValue value = jsTakeString(ctx, prCssStyleDeclarationGetPropertyValue(declarations, &name));
    prCssDeclarationNameReset(&name);
    return value;
}

// Implements the setter, which is setProperty() with no priority, and is [LegacyNullToEmptyString].
JSValue attributeSet(JSContext* ctx, JSValueConst declarationsObject, JSValueConst input, int magic) {
    PrCssStyleDeclaration* declarations =
        jsThis<PrCssStyleDeclaration>(ctx, declarationsObject, cssStyleDeclarationClass);
    StringArgument value = {ctx};
    if (declarations == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    PrCssDeclarationName name = {};
    if (!attributeName(declarations, magic, &name))
        return JS_UNDEFINED;
    PrStatus status = prCssStyleDeclarationSetProperty(declarations, &name, value.value(), false);
    prCssDeclarationNameReset(&name);
    return setStatusToJs(ctx, status);
}

void defineAttribute(JSContext* ctx, JSValueConst proto, const char* name, size_t size, int magic) {
    JSAtom atom = JS_NewAtomLen(ctx, name, size);
    JSCFunctionType get;
    get.getter_magic = JsCallScoped<attributeGet>::call;
    JSCFunctionType set;
    // The attribute is [CEReactions], so its setter runs in the member scope.
    set.setter_magic = JsCallScoped<JsScoped<attributeSet>::call>::call;
    JS_DefinePropertyGetSet(ctx, proto, atom, JS_NewCFunction2(ctx, get.generic, "get", 0, JS_CFUNC_getter_magic, magic),
        JS_NewCFunction2(ctx, set.generic, "set", 1, JS_CFUNC_setter_magic, magic),
        JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
}

// cssom-1/#css-property-to-idl-attribute. If `lowercaseFirst` is set, it drops the leading dash.
uint32_t idlAttributeName(PrStringView property, bool lowercaseFirst, char* out, uint32_t capacity) {
    uint32_t size = 0;
    bool upper = false;
    for (size_t i = lowercaseFirst ? 1 : 0; i < property.size && size < capacity; ++i) {
        if (property.data[i] == '-') {
            upper = true;
            continue;
        }
        out[size++] = upper ? prAsciiUpper(property.data[i]) : property.data[i];
        upper = false;
    }
    return size;
}

// Defines the attributes for every name of a list: the camel-cased attribute, the dashed attribute of a name with a
// dash, and the webkit-cased attribute of a -webkit- name (cssom-1/#dom-cssstyleproperties-dashed-attribute,
// cssom-1/#dom-cssstyleproperties-webkit-cased-attribute).
void defineAttributes(JSContext* ctx, JSValueConst proto, Names names) {
    uint32_t count = names == Names_Properties ? uint32_t(PrCssProperty_Count) : kNameLists[names].count;
    static_assert(PrCssProperty_Count < 1 << kNamesShift);
    for (uint32_t index = 0; index < count; ++index) {
        int magic = int(names) << kNamesShift | int(index);
        PrStringView property = attributeText(magic);
        defineAttribute(ctx, proto, property.data, property.size, magic);
        char name[64];
        if (prMemFind(property.data, '-', property.size) != nullptr)
            defineAttribute(ctx, proto, name, idlAttributeName(property, false, name, uint32_t(sizeof(name))), magic);
        if (prStringStartsWith(property, "-webkit-"))
            defineAttribute(ctx, proto, name, idlAttributeName(property, true, name, uint32_t(sizeof(name))), magic);
    }
}

// The object of a sheet or a rule holds a reference to it, which keeps its tree alive.
constexpr JsReferences kSheetReferences = {
    [](void* handle) { prCssStyleSheetRetain(static_cast<PrCssStyleSheet*>(handle)); },
    [](void* handle) { prCssStyleSheetRelease(static_cast<PrCssStyleSheet*>(handle)); },
    false,
};

constexpr JsReferences kRuleReferences = {
    [](void* handle) { prCssRuleRetain(static_cast<PrCssRule*>(handle)); },
    [](void* handle) { prCssRuleRelease(static_cast<PrCssRule*>(handle)); },
    false,
};

// ---------------------------------------------------------------------------------------------------------------------
// Highlight and HighlightRegistry (css-highlight-api-1)

// A highlight's object holds a reference to it.
constexpr JsReferences kHighlightReferences = {
    [](void* handle) { prCssHighlightRetain(static_cast<PrCssHighlight*>(handle)); },
    [](void* handle) { prCssHighlightRelease(static_cast<PrCssHighlight*>(handle)); },
    false,
};

JSValue highlightToJs(JSContext* ctx, PrCssHighlight* highlight) {
    return jsWrap(ctx, cssHighlightClass, JS_UNDEFINED, highlight, false);
}

PrCssHighlight* highlightOf(void* handle) {
    return static_cast<PrCssHighlight*>(handle);
}

PrDomRange* rangeOf(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrDomRange>(ctx, value, prJsDomAbstractRangeClass);
}

// setlike<AbstractRange> (css-highlight-api-1/#highlight)
constexpr JsCollection kHighlightRanges = {
    .map = false,
    .key = [](JSContext* ctx, JSValueConst argument) {
        return rangeOf(ctx, argument) != nullptr ? JS_DupValue(ctx, argument) : JS_ThrowTypeError(ctx, "Expected an AbstractRange");
    },
    .value = nullptr,
    .add = [](JSContext* ctx, void* handle, JSValueConst key, JSValueConst) {
        prCssHighlightAdd(highlightOf(handle), rangeOf(ctx, key));
        return true;
    },
    .remove = [](JSContext* ctx, void* handle, JSValueConst key) {
        prCssHighlightDelete(highlightOf(handle), rangeOf(ctx, key));
    },
    .clear = [](JSContext*, void* handle) { prCssHighlightClear(highlightOf(handle)); },
    .version = [](void* handle) { return prCssHighlightVersion(highlightOf(handle)); },
    .entries = [](JSContext* ctx, JSValueConst, void* handle) {
        PrCssHighlight* highlight = highlightOf(handle);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ranges = JS_NewArray(ctx);
        for (uint32_t i = 0; i < prCssHighlightSize(highlight); ++i) {
            PrDomRange* range = prCssHighlightAt(highlight, i);
            prDomRangeRetain(range);
            JS_SetPropertyUint32(ctx, ranges, i, prJsDomWrapRange(ctx, global, range));
        }
        JS_FreeValue(ctx, global);
        return ranges;
    },
};

// css-highlight-api-1/#dom-highlight-highlight
JSValue highlightConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    for (int i = 0; i < argc; ++i) {
        if (rangeOf(ctx, argv[i]) == nullptr)
            return JS_ThrowTypeError(ctx, "Expected an AbstractRange");
    }
    PrCssHighlight* highlight = prCssHighlightCreate();
    for (int i = 0; i < argc; ++i)
        prCssHighlightAdd(highlight, rangeOf(ctx, argv[i]));
    JSValue object = jsWrap(ctx, cssHighlightClass, JS_UNDEFINED, highlight, true);
    if (JS_IsException(object))
        prCssHighlightRelease(highlight);
    return jsConstructed(ctx, newTarget, object);
}

JSValue highlightPriority(JSContext* ctx, JSValueConst highlightObject) {
    PrCssHighlight* highlight = jsThis<PrCssHighlight>(ctx, highlightObject, cssHighlightClass);
    return highlight != nullptr ? JS_NewInt32(ctx, prCssHighlightPriority(highlight)) : JS_EXCEPTION;
}

JSValue highlightSetPriority(JSContext* ctx, JSValueConst highlightObject, JSValueConst value) {
    PrCssHighlight* highlight = jsThis<PrCssHighlight>(ctx, highlightObject, cssHighlightClass);
    int32_t priority = 0;
    if (highlight == nullptr || JS_ToInt32(ctx, &priority, value) < 0)
        return JS_EXCEPTION;
    prCssHighlightSetPriority(highlight, priority);
    return JS_UNDEFINED;
}

// css-highlight-api-1/#enumdef-highlighttype, in the order of PrCssHighlightType.
constexpr PrStringView kHighlightTypes[] = {"highlight", "spelling-error", "grammar-error"};

JSValue highlightType(JSContext* ctx, JSValueConst highlightObject) {
    PrCssHighlight* highlight = jsThis<PrCssHighlight>(ctx, highlightObject, cssHighlightClass);
    return highlight != nullptr ? jsFixedString(ctx, kHighlightTypes[prCssHighlightType(highlight)]) : JS_EXCEPTION;
}

// Sets the type. A value that is not a HighlightType leaves the type unchanged (webidl/#js-enumeration, as converted by
// an attribute's setter).
JSValue highlightSetType(JSContext* ctx, JSValueConst highlightObject, JSValueConst value) {
    PrCssHighlight* highlight = jsThis<PrCssHighlight>(ctx, highlightObject, cssHighlightClass);
    StringArgument type = {ctx};
    if (highlight == nullptr || !type.read(value))
        return JS_EXCEPTION;
    for (uint32_t i = 0; i < countOf(kHighlightTypes); ++i) {
        if (type.value() == kHighlightTypes[i])
            prCssHighlightSetType(highlight, PrCssHighlightType(i));
    }
    return JS_UNDEFINED;
}

PrCssHighlightRegistry* registryOf(void* handle) {
    return static_cast<PrCssHighlightRegistry*>(handle);
}

// maplike<DOMString, Highlight> (css-highlight-api-1/#highlight-registry). The registry names its highlights without
// holding them, and the backing Map of its object holds their objects. A highlight that a script registers therefore
// lives as long as it is registered.
constexpr JsCollection kRegistryEntries = {
    .map = true,
    .key = [](JSContext* ctx, JSValueConst argument) { return JS_ToString(ctx, argument); },
    .value = [](JSContext* ctx, JSValueConst argument) {
        if (jsHandleAs<PrCssHighlight>(ctx, argument, cssHighlightClass) != nullptr)
            return JS_DupValue(ctx, argument);
        return JS_ThrowTypeError(ctx, "Expected a Highlight");
    },
    .add = [](JSContext* ctx, void* handle, JSValueConst key, JSValueConst value) {
        StringArgument name = {ctx};
        if (!name.read(key))
            return false;
        prCssHighlightRegistrySet(registryOf(handle), name.value(),
            jsHandleAs<PrCssHighlight>(ctx, value, cssHighlightClass));
        return true;
    },
    .remove = [](JSContext* ctx, void* handle, JSValueConst key) {
        StringArgument name = {ctx};
        if (name.read(key))
            prCssHighlightRegistryDelete(registryOf(handle), name.value());
    },
    .clear = [](JSContext*, void* handle) { prCssHighlightRegistryClear(registryOf(handle)); },
    .version = [](void* handle) { return prCssHighlightRegistryVersion(registryOf(handle)); },
    .entries = [](JSContext* ctx, JSValueConst, void* handle) {
        PrCssHighlightRegistry* registry = registryOf(handle);
        JSValue entries = JS_NewArray(ctx);
        for (uint32_t i = 0; i < prCssHighlightRegistrySize(registry); ++i) {
            JSValue name = jsNewStringView(ctx, prCssHighlightRegistryNameAt(registry, i));
            JS_SetPropertyUint32(ctx, entries, i,
                jsEntry(ctx, name, highlightToJs(ctx, prCssHighlightRegistryAt(registry, i))));
        }
        return entries;
    },
};

// css-highlight-api-1/#dom-css-highlights: returns the registry of the global's document. The document's object keeps
// the registry alive.
JSValue cssHighlights(JSContext* ctx, JSValueConst) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_EXCEPTION;
    JSValue owner = prJsDomWrapDocument(ctx, document);
    if (JS_IsException(owner))
        return owner;
    JSValue registry = jsSameObject(ctx, owner, "highlights", [&] {
        return jsWrap(ctx, cssHighlightRegistryClass, owner, prCssDocumentHighlights(document), false);
    });
    JS_FreeValue(ctx, owner);
    return registry;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSS (cssom-1/#namespacedef-css)

JSValue escape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument ident = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !ident.read(argv[0]))
        return JS_EXCEPTION;
    return jsTakeString(ctx, prCssEscape(ident.value()));
}

// supports(property, value) and supports(conditionText) (css-conditional-3/#the-css-namespace), selected by the
// argument count (webidl/#dfn-overload-resolution-algorithm).
JSValue supports(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument first = {ctx};
    StringArgument second = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !first.read(argv[0]))
        return JS_EXCEPTION;
    if (argc == 1)
        return JS_NewBool(ctx, prCssSupportsConditionText(first.value()));
    if (!second.read(argv[1]))
        return JS_EXCEPTION;
    PrCssDeclarationName name = {};
    if (!prCssPropertyNameFromText(first.value(), &name))
        return JS_FALSE;
    bool supported = prCssSupportsPropertyValue(&name, second.value());
    prCssDeclarationNameReset(&name);
    return JS_NewBool(ctx, supported);
}

// css-properties-values-api-1/#dom-css-registerproperty, with the global's document.
JSValue registerProperty(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValueConst dictionary = argc > 0 ? argv[0] : JS_UNDEFINED;
    PrCssPropertyDefinition definition = prCssPropertyDefinitionDefault();
    StringArgument initialValue = {ctx};
    StringArgument name = {ctx};
    StringArgument syntax = {ctx};
    bool present = false;
    bool inherits = false;
    bool initial = false;
    bool named = false;
    bool typed = false;
    // The members of PropertyDefinition in lexicographic order. inherits and name are required
    // (css-properties-values-api-1/#dictdef-propertydefinition).
    if (!jsArgCount(ctx, argc, 1) || !jsDictionary(ctx, dictionary, &present) ||
            !jsDictBool(ctx, dictionary, "inherits", &definition.inherits, &inherits) ||
            !jsDictString(ctx, dictionary, "initialValue", &initialValue, &initial) ||
            !jsDictString(ctx, dictionary, "name", &name, &named) || !jsDictString(ctx, dictionary, "syntax", &syntax, &typed))
        return JS_EXCEPTION;
    if (!inherits || !named)
        return JS_ThrowTypeError(ctx, "A PropertyDefinition needs a name and inherits");
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_EXCEPTION;
    definition.name = name.value();
    if (typed)
        definition.syntax = syntax.value();
    if (initial)
        definition.initialValue = initialValue.value();
    PrStatus status = prCssDocumentRegisterProperty(document, &definition);
    if (status == PrStatus_NotAllowed)
        return jsDomExceptionThrow(ctx, "InvalidModificationError");
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "SyntaxError");
}

constexpr JSCFunctionListEntry kCssNamespace[] = {
    PR_JS_TAG("CSS"),
    PR_JS_METHOD("escape", 1, escape),
    PR_JS_METHOD("supports", 1, supports),
    PR_JS_METHOD("registerProperty", 1, registerProperty),
    PR_JS_GET("highlights", cssHighlights),
};

// ---------------------------------------------------------------------------------------------------------------------
// FontFace (css-font-loading-3/#fontface-interface)

// The slots of a FontFace's object: its [[FontStatusPromise]] and the functions that settle the promise.
enum {
    FontSlot_Promise,
    FontSlot_Resolve,
    FontSlot_Reject,
};
typedef uint8_t FontSlot;

// A face's object holds a reference to it.
constexpr JsReferences kFontFaceReferences = {
    [](void* handle) { prCssFontFaceRetain(static_cast<PrCssFontFace*>(handle)); },
    [](void* handle) { prCssFontFaceRelease(static_cast<PrCssFontFace*>(handle)); },
    false,
};

JSValue fontFaceToJs(JSContext* ctx, PrCssFontFace* face) {
    return jsWrap(ctx, cssFontFaceClass, JS_UNDEFINED, face, false);
}

// Settles the face's promise if its status is Loaded or Error. It resolves with the face, or rejects with a SyntaxError
// for a face whose descriptors did not parse, or a NetworkError for one whose font did not load.
void settleFontFace(JSContext* ctx, JSValueConst object) {
    PrCssFontFace* face = jsHandleAs<PrCssFontFace>(ctx, object, cssFontFaceClass);
    PrCssFontFaceStatus status = prCssFontFaceStatus(face);
    if (status != PrCssFontFaceStatus_Loaded && status != PrCssFontFaceStatus_Error)
        return;
    bool loaded = status == PrCssFontFaceStatus_Loaded;
    JSValue settle = jsReference(ctx, object, loaded ? FontSlot_Resolve : FontSlot_Reject);
    if (JS_IsUndefined(settle))
        return;
    JSValue value;
    if (loaded)
        value = JS_DupValue(ctx, object);
    else
        value = jsDomExceptionNew(ctx, prCssFontFaceFailedToParse(face) ? "SyntaxError" : "NetworkError");
    JS_FreeValue(ctx, JS_Call(ctx, settle, JS_UNDEFINED, 1, &value));
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, settle);
    jsSetReference(ctx, object, FontSlot_Resolve, JS_UNDEFINED);
    jsSetReference(ctx, object, FontSlot_Reject, JS_UNDEFINED);
}

// Makes the document's object hold the objects of the faces loading through the document, because the load keeps the
// face alive (css-font-loading-3/#font-face-load). Their promises settle regardless of who else holds them.
void holdLoading(JSContext* ctx, PrDomDocument* document, JSValueConst face, bool hold) {
    JSValue owner = prJsDomWrapDocument(ctx, document);
    if (!JS_IsException(owner))
        jsHold(ctx, owner, "[[LoadingFonts]]", face, hold);
    JS_FreeValue(ctx, owner);
}

// Each face's object has its [[FontStatusPromise]] from the start, settled if the face's status is settled. The object
// is held while the face is loading.
void fontFaceWrapped(JSContext* ctx, JSValueConst object, void*) {
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise))
        return;
    jsSetReference(ctx, object, FontSlot_Promise, promise);
    jsSetReference(ctx, object, FontSlot_Resolve, functions[0]);
    jsSetReference(ctx, object, FontSlot_Reject, functions[1]);
    // The promise is marked as handled, because a face that fails to load is not a script's error.
    JSValue ignore = JS_NewCFunction(ctx, [](JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }, "", 1);
    JSAtom name = JS_NewAtom(ctx, "catch");
    JS_FreeValue(ctx, JS_Invoke(ctx, promise, name, 1, &ignore));
    JS_FreeAtom(ctx, name);
    JS_FreeValue(ctx, ignore);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    PrCssFontFace* face = jsHandleAs<PrCssFontFace>(ctx, object, cssFontFaceClass);
    if (PrDomDocument* document = prCssFontFaceLoadingDocument(face))
        holdLoading(ctx, document, object, true);
    settleFontFace(ctx, object);
}

void faceStatusChanged(void* PR_NULLABLE context, PrDomDocument* document, PrCssFontFace* face) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = fontFaceToJs(ctx, face);
    if (JS_IsException(object))
        return;
    bool loading = prCssFontFaceStatus(face) == PrCssFontFaceStatus_Loading;
    if (!loading)
        settleFontFace(ctx, object);
    holdLoading(ctx, document, object, loading);
    JS_FreeValue(ctx, object);
}

// The slots of a FontFaceSet's object: its [[ReadyPromise]], and the function that resolves the promise while it is
// pending.
enum {
    ReadySlot_Promise,
    ReadySlot_Resolve,
};
typedef uint8_t ReadySlot;

// Gives the set's object a new [[ReadyPromise]], which is pending while the set is loading.
bool replaceReady(JSContext* ctx, JSValueConst object, PrCssFontFaceSet* set) {
    JSValue functions[2];
    JSValue ready = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(ready))
        return false;
    jsSetReference(ctx, object, ReadySlot_Promise, ready);
    if (prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loaded)
        JS_FreeValue(ctx, JS_Call(ctx, functions[0], JS_UNDEFINED, 1, &object));
    else
        jsSetReference(ctx, object, ReadySlot_Resolve, functions[0]);
    JS_FreeValue(ctx, ready);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    return true;
}

// css-font-loading-3/#fontfaceset-pending-on-the-environment. When the set switches to loading, its ready promise is
// replaced with a new pending one if the last was fulfilled. When it switches to loaded, the pending promise resolves
// with the set.
void setStatusChanged(void* PR_NULLABLE context, PrDomDocument*, PrCssFontFaceSet* set) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = jsWrap(ctx, prJsCssFontFaceSetClass, JS_UNDEFINED, set, false);
    if (JS_IsException(object))
        return;
    JSValue resolve = jsReference(ctx, object, ReadySlot_Resolve);
    bool pending = !JS_IsUndefined(resolve);
    JSValue ready = jsReference(ctx, object, ReadySlot_Promise);
    if (prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loading) {
        if (!pending && !JS_IsUndefined(ready))
            replaceReady(ctx, object, set);
    } else if (pending) {
        jsSetReference(ctx, object, ReadySlot_Resolve, JS_UNDEFINED);
        JS_FreeValue(ctx, JS_Call(ctx, resolve, JS_UNDEFINED, 1, &object));
    }
    JS_FreeValue(ctx, ready);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, object);
}

// Makes the binding receive notifications about the fonts of `document`, which a load through the document or its set
// can change.
void observeFonts(JSContext* ctx, PrDomDocument* document) {
    const PrCssFontCallback callback = {faceStatusChanged, setStatusChanged, ctx};
    prCssDocumentSetFontCallback(document, &callback);
}

JSValue fontFaceConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    // The members of FontFaceDescriptors in lexicographic order, in which webidl/#js-dictionary reads them, and the
    // descriptor that each sets. width and stretch both set font-width, and width is last.
    constexpr struct {
        const char* name;
        PrCssFontFaceDescriptor descriptor;
    } kMembers[] = {
        {"ascentOverride", PrCssFontFaceDescriptor_AscentOverride},
            {"descentOverride", PrCssFontFaceDescriptor_DescentOverride}, {"display", PrCssFontFaceDescriptor_Display},
            {"featureSettings", PrCssFontFaceDescriptor_FeatureSettings},
            {"lineGapOverride", PrCssFontFaceDescriptor_LineGapOverride}, {"stretch", PrCssFontFaceDescriptor_Stretch},
            {"style", PrCssFontFaceDescriptor_Style}, {"unicodeRange", PrCssFontFaceDescriptor_UnicodeRange},
            {"variationSettings", PrCssFontFaceDescriptor_VariationSettings},
            {"weight", PrCssFontFaceDescriptor_Weight}, {"width", PrCssFontFaceDescriptor_Stretch},
    };
    StringArgument family = {ctx};
    StringArgument source = {ctx};
    PrStringView descriptors[PrCssFontFaceDescriptor_Count - 1] = {};
    bool present = false;
    if (!jsArgCount(ctx, argc, 2) || !family.read(argv[0]))
        return JS_EXCEPTION;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool binary = jsBufferSource(ctx, argv[1], &data, &size);
    if (!binary && !source.read(argv[1]))
        return JS_EXCEPTION;
    JSValueConst dictionary = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (!jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    for (uint32_t i = 0; present && i < countOf(kMembers); ++i) {
        JSValue member = JS_GetPropertyStr(ctx, dictionary, kMembers[i].name);
        bool read = !JS_IsException(member) && (JS_IsUndefined(member) ||
            jsReadString(ctx, member, &descriptors[kMembers[i].descriptor - 1]));
        JS_FreeValue(ctx, member);
        if (!read)
            return JS_EXCEPTION;
    }
    PrCssFontFace* face;
    if (binary)
        face = prCssFontFaceCreateFromData(prJsDomGlobalDocument(ctx), family.value(), PrByteView{data, size},
            descriptors);
    else
        face = prCssFontFaceCreate(family.value(), source.value(), descriptors);
    JSValue object = jsWrap(ctx, cssFontFaceClass, JS_UNDEFINED, face, true);
    if (JS_IsException(object))
        prCssFontFaceRelease(face);
    return jsConstructed(ctx, newTarget, object);
}

JSValue fontDescriptor(JSContext* ctx, JSValueConst faceObject, int magic) {
    PrCssFontFace* face = jsThis<PrCssFontFace>(ctx, faceObject, cssFontFaceClass);
    if (face == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prCssFontFaceDescriptor(face, PrCssFontFaceDescriptor(magic)));
}

JSValue setFontDescriptor(JSContext* ctx, JSValueConst faceObject, JSValueConst input, int magic) {
    PrCssFontFace* face = jsThis<PrCssFontFace>(ctx, faceObject, cssFontFaceClass);
    StringArgument value = {ctx};
    if (face == nullptr || !value.read(input))
        return JS_EXCEPTION;
    PrStatus status = prCssFontFaceSetDescriptor(face, PrCssFontFaceDescriptor(magic), value.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "SyntaxError");
}

JSValue fontStatus(JSContext* ctx, JSValueConst faceObject) {
    constexpr PrStringView kStatuses[] = {"unloaded", "loading", "loaded", "error"};
    PrCssFontFace* face = jsThis<PrCssFontFace>(ctx, faceObject, cssFontFaceClass);
    return face != nullptr ? jsFixedString(ctx, kStatuses[prCssFontFaceStatus(face)]) : JS_EXCEPTION;
}

JSValue fontLoaded(JSContext* ctx, JSValueConst object) {
    if (jsThis<PrCssFontFace>(ctx, object, cssFontFaceClass) == nullptr)
        return JS_EXCEPTION;
    return jsReference(ctx, object, FontSlot_Promise);
}

// Loads the face through the global's document. The load settles the returned promise.
JSValue fontLoad(JSContext* ctx, JSValueConst faceObject, int, JSValueConst*) {
    PrCssFontFace* face = jsThis<PrCssFontFace>(ctx, faceObject, cssFontFaceClass);
    PrDomDocument* document = face != nullptr ? prJsDomGlobalDocument(ctx) : nullptr;
    if (document == nullptr)
        return JS_EXCEPTION;
    observeFonts(ctx, document);
    prCssFontFaceLoad(face, document);
    settleFontFace(ctx, faceObject);
    return jsReference(ctx, faceObject, FontSlot_Promise);
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSet (css-font-loading-3/#fontfaceset)

// The objects of the set's faces, which the set's object holds. A face in the set keeps its object, and therefore its
// promise, alive whoever else releases it.
constexpr char kSetFaces[] = "[[Faces]]";

// Returns the faces as an array of [face, face] arrays, in the form that a setlike's iterators and forEach read
// (webidl/#js-setlike).
JSValue setEntries(JSContext* ctx, JSValueConst object, PrCssFontFaceSet* set) {
    JSValue entries = JS_NewArray(ctx);
    for (uint32_t i = 0; i < prCssFontFaceSetSize(set); ++i) {
        JSValue face = fontFaceToJs(ctx, prCssFontFaceSetAt(set, i));
        jsHold(ctx, object, kSetFaces, face, true);
        JS_SetPropertyUint32(ctx, entries, i, jsEntry(ctx, JS_DupValue(ctx, face), face));
    }
    return entries;
}

// Implements entries, keys and values, selected by the magic, which is the JsMapPart.
JSValue setIterate(JSContext* ctx, JSValueConst setObject, int, JSValueConst*, int magic) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    return set != nullptr ? jsMapIterate(ctx, setEntries(ctx, setObject, set), JsMapPart(magic)) : JS_EXCEPTION;
}

JSValue setForEach(JSContext* ctx, JSValueConst setObject, int argc, JSValueConst* argv) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    return set != nullptr ? jsMapForEach(ctx, setObject, setEntries(ctx, setObject, set), argc, argv) : JS_EXCEPTION;
}

// Implements add, delete and has, selected by the magic, each taking a FontFace.
JSValue setMember(JSContext* ctx, JSValueConst setObject, int argc, JSValueConst* argv, int magic) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    if (set == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrCssFontFace* face = jsHandleAs<PrCssFontFace>(ctx, argv[0], cssFontFaceClass);
    if (face == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a FontFace");
    switch (magic) {
        case 0:
            if (prCssFontFaceSetAdd(set, face) != PrStatus_Ok)
                return jsDomExceptionThrow(ctx, "InvalidModificationError");
            jsHold(ctx, setObject, kSetFaces, argv[0], true);
            return JS_DupValue(ctx, setObject);
        case 1: {
            bool deleted = prCssFontFaceSetDelete(set, face);
            if (deleted)
                jsHold(ctx, setObject, kSetFaces, argv[0], false);
            return JS_NewBool(ctx, deleted);
        }
        default:
            return JS_NewBool(ctx, prCssFontFaceSetHas(set, face));
    }
}

JSValue setClear(JSContext* ctx, JSValueConst setObject, int, JSValueConst*) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    if (set == nullptr)
        return JS_EXCEPTION;
    prCssFontFaceSetClear(set);
    jsSetSameObject(ctx, setObject, kSetFaces, JS_UNDEFINED);
    return JS_UNDEFINED;
}

JSValue setStatus(JSContext* ctx, JSValueConst setObject) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    if (set == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loading ? "loading" : "loaded");
}

// Implements ready (css-font-loading-3/#dom-fontfaceset-ready)
JSValue setReady(JSContext* ctx, JSValueConst setObject) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    if (set == nullptr)
        return JS_EXCEPTION;
    JSValue ready = jsReference(ctx, setObject, ReadySlot_Promise);
    if (!JS_IsUndefined(ready) || !replaceReady(ctx, setObject, set))
        return ready;
    return jsReference(ctx, setObject, ReadySlot_Promise);
}

JSValue setCheck(JSContext* ctx, JSValueConst setObject, int argc, JSValueConst* argv) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    StringArgument font = {ctx};
    if (set == nullptr || !jsArgCount(ctx, argc, 1) || !font.read(argv[0]))
        return JS_EXCEPTION;
    bool available = false;
    PrStatus status = prCssFontFaceSetCheck(set, font.value(), &available);
    return status == PrStatus_Ok ? JS_NewBool(ctx, available) : jsDomExceptionThrow(ctx, "SyntaxError");
}

// Implements load (css-font-loading-3/#dom-fontfaceset-load), which returns a promise of the faces that `font` matches,
// fulfilled once each has loaded. The promise rejects with a SyntaxError if `font` is not a font value, and otherwise
// with the error of the first face that fails.
JSValue setLoad(JSContext* ctx, JSValueConst setObject, int argc, JSValueConst* argv) {
    PrCssFontFaceSet* set = jsThis<PrCssFontFaceSet>(ctx, setObject, prJsCssFontFaceSetClass);
    StringArgument font = {ctx};
    PrDomDocument* document = set != nullptr ? prJsDomGlobalDocument(ctx) : nullptr;
    if (document == nullptr || !jsArgCount(ctx, argc, 1) || !font.read(argv[0]))
        return JS_EXCEPTION;
    observeFonts(ctx, document);
    uint32_t count = 0;
    if (prCssFontFaceSetLoad(set, document, font.value(), nullptr, 0, &count) != PrStatus_Ok) {
        JSValue error = jsDomExceptionNew(ctx, "SyntaxError");
        JSValue promise = jsSettledPromise(ctx, error, true);
        JS_FreeValue(ctx, error);
        return promise;
    }
    RawArray<PrCssFontFace* PR_NONNULL> faces;
    faces.reserve(count);
    prCssFontFaceSetLoad(set, document, font.value(), faces.data, count, &count);
    JSValue promises = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue face = fontFaceToJs(ctx, faces.data[i]);
        JSValue promise = JS_UNDEFINED;
        if (!JS_IsException(face))
            promise = jsReference(ctx, face, FontSlot_Promise);
        JS_SetPropertyUint32(ctx, promises, i, promise);
        JS_FreeValue(ctx, face);
    }
    faces.reset();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue constructor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue all = JS_GetPropertyStr(ctx, constructor, "all");
    JSValue result = JS_Call(ctx, all, constructor, 1, &promises);
    JS_FreeValue(ctx, all);
    JS_FreeValue(ctx, constructor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, promises);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSetLoadEvent (css-font-loading-3/#fontfacesetloadevent)

JSClassID classifyCssEvent(void* handle) {
    PrDomEvent* event = static_cast<PrDomEvent*>(handle);
    if (prCssFontFaceSetLoadEventQuery(event) != nullptr)
        return cssFontFaceSetLoadEventClass;
    if (prCssTransitionEventQuery(event) != nullptr)
        return cssTransitionEventClass;
    if (prCssAnimationEventQuery(event) != nullptr)
        return cssAnimationEventClass;
    return prJsDomEventClass;
}

JSValue fontEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrCssFontFaceSetLoadEventInit init = prCssFontFaceSetLoadEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    RawArray<PrCssFontFace* PR_NONNULL> faces;
    bool read = jsDictionary(ctx, dictionary, &present);
    // The members of FontFaceSetLoadEventInit are read after those of EventInit (webidl/#js-dictionary).
    if (read && present) {
        read = prJsDomEventInitRead(ctx, dictionary, &init.event);
        JSValue value = read ? JS_GetPropertyStr(ctx, dictionary, "fontfaces") : JS_UNDEFINED;
        read = read && !JS_IsException(value);
        if (read && !JS_IsUndefined(value)) {
            read = jsSequence(ctx, value, [&](JSValue item) {
                PrCssFontFace* face = jsHandleAs<PrCssFontFace>(ctx, item, cssFontFaceClass);
                JS_FreeValue(ctx, item);
                if (face == nullptr) {
                    JS_ThrowTypeError(ctx, "Expected a FontFace");
                    return false;
                }
                faces.push(face);
                return true;
            });
        }
        JS_FreeValue(ctx, value);
    }
    JSValue object = JS_EXCEPTION;
    if (read) {
        init.fontfaces = faces.data;
        init.count = faces.count;
        PrDomEvent* event = prCssFontFaceSetLoadEventAsEvent(prCssFontFaceSetLoadEventCreate(type.value(), &init));
        object = jsWrap(ctx, cssFontFaceSetLoadEventClass, JS_UNDEFINED, event, true);
        if (JS_IsException(object))
            prDomEventRelease(event);
    }
    faces.reset();
    return JS_IsException(object) ? object : jsConstructed(ctx, newTarget, object);
}

// ---------------------------------------------------------------------------------------------------------------------
// TransitionEvent (css-transitions-1/#interface-transitionevent) and AnimationEvent
// (css-animations-1/#interface-animationevent), selected by the magic: 0 for a transition, 1 for an animation.

// Constructs either event from its type and init dictionary. The dictionary's members are read after those of
// EventInit, in lexicographic order (webidl/#js-dictionary).
JSValue animationEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int magic) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrCssAnimationEventInit init = prCssAnimationEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    StringArgument name = {ctx};
    StringArgument pseudo = {ctx};
    const char* nameMember = magic == 0 ? "propertyName" : "animationName";
    bool read = jsDictionary(ctx, dictionary, &present);
    if (read && present) {
        bool given = false;
        bool pseudoGiven = false;
        read = prJsDomEventInitRead(ctx, dictionary, &init.event);
        if (read && magic == 1)
            read = jsDictString(ctx, dictionary, nameMember, &name, &given);
        read = read && jsDictNumber(ctx, dictionary, "elapsedTime", &init.elapsedTime);
        if (read && magic == 0)
            read = jsDictString(ctx, dictionary, nameMember, &name, &given);
        read = read && jsDictString(ctx, dictionary, "pseudoElement", &pseudo, &pseudoGiven);
        if (given)
            init.name = name.value();
        if (pseudoGiven)
            init.pseudoElement = pseudo.value();
        // elapsedTime is a double (webidl/#js-double).
        if (read && !prF64IsFinite(init.elapsedTime)) {
            JS_ThrowTypeError(ctx, "The provided double value is non-finite");
            read = false;
        }
    }
    if (!read)
        return JS_EXCEPTION;
    PrDomEvent* event;
    if (magic == 0)
        event = prCssTransitionEventAsEvent(prCssTransitionEventCreate(type.value(), &init));
    else
        event = prCssAnimationEventAsEvent(prCssAnimationEventCreate(type.value(), &init));
    JSValue object = jsWrap(ctx, magic == 0 ? cssTransitionEventClass : cssAnimationEventClass, JS_UNDEFINED, event, true);
    if (JS_IsException(object)) {
        prDomEventRelease(event);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

template <int Magic>
JSValue animationEventConstructAs(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return animationEventConstruct(ctx, newTarget, argc, argv, Magic);
}

// Implements propertyName or animationName, elapsedTime, and pseudoElement, selected by the magic: 0, 1 and 2 for a
// transition, and 3, 4 and 5 for an animation.
JSValue animationEventGet(JSContext* ctx, JSValueConst eventObject, int magic) {
    if (magic < 3) {
        PrCssTransitionEvent* event = jsThis<PrCssTransitionEvent>(ctx, eventObject, cssTransitionEventClass);
        if (event == nullptr)
            return JS_EXCEPTION;
        if (magic == 1)
            return JS_NewFloat64(ctx, prCssTransitionEventElapsedTime(event));
        if (magic == 0)
            return jsNewStringView(ctx, prCssTransitionEventPropertyName(event));
        return jsNewStringView(ctx, prCssTransitionEventPseudoElement(event));
    }
    PrCssAnimationEvent* event = jsThis<PrCssAnimationEvent>(ctx, eventObject, cssAnimationEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    if (magic == 4)
        return JS_NewFloat64(ctx, prCssAnimationEventElapsedTime(event));
    if (magic == 3)
        return jsNewStringView(ctx, prCssAnimationEventAnimationName(event));
    return jsNewStringView(ctx, prCssAnimationEventPseudoElement(event));
}

// Implements fontfaces, a FrozenArray that the object holds, so that each read returns the same array.
JSValue fontEventFaces(JSContext* ctx, JSValueConst eventObject) {
    PrCssFontFaceSetLoadEvent* event =
        jsThis<PrCssFontFaceSetLoadEvent>(ctx, eventObject, cssFontFaceSetLoadEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, eventObject, "fontfaces", [&] {
        uint32_t count = prCssFontFaceSetLoadEventFontFaces(event, nullptr, 0);
        RawArray<PrCssFontFace* PR_NONNULL> faces;
        faces.reserve(count);
        faces.count = prCssFontFaceSetLoadEventFontFaces(event, faces.data, count);
        JSValue array = JS_NewArray(ctx);
        for (uint32_t i = 0; i < faces.count; ++i)
            JS_SetPropertyUint32(ctx, array, i, fontFaceToJs(ctx, faces.data[i]));
        faces.reset();
        JS_FreezeObject(ctx, array);
        return array;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

#define PR_STRING(name, id, getter) PR_JS_GET(name, (jsStringGetter<&id, getter>))
#define PR_STRING_SET(name, id, getter, setter) \
    PR_JS_GET_SET(name, (jsStringGetter<&id, getter>), (jsStringSetter<&id, setter>))
#define PR_OBJECT(name, id, getter, as) PR_JS_GET(name, (jsObjectGetter<&id, getter, &as>))
// The entry for a [SameObject, PutForwards=cssText] style attribute.
#define PR_STYLE(id, getter) \
    PR_JS_GET_SET("style", (jsSameObjectGetter<&id, getter, &cssStyleDeclarationClass, "style">), \
                  (jsPutForwards<"style", "cssText">))
// The entry for a [SameObject, PutForwards=mediaText] media attribute.
#define PR_MEDIA(id, getter) \
    PR_JS_GET_SET("media", (jsSameObjectGetter<&id, getter, &cssMediaListClass, "media">), \
                  (jsPutForwards<"media", "mediaText">))

constexpr JSCFunctionListEntry kMediaListProto[] = {
    PR_JS_TAG("MediaList"),
    PR_JS_GET_SET("mediaText", (jsStringGetter<&cssMediaListClass, prCssMediaListMediaText>),
        (jsStringSetter<&cssMediaListClass, prCssMediaListSetMediaText, true>)),
    PR_JS_GET("length", (jsNumberGetter<&cssMediaListClass, prCssMediaListLength>)),
    PR_JS_METHOD("item", 1, (jsItemMethod<&cssMediaListClass, &kMediaIndexed>)),
    PR_JS_MAGIC("appendMedium", 1, mediumMethod, 0),
    PR_JS_MAGIC("deleteMedium", 1, mediumMethod, 1),
    PR_JS_METHOD("toString", 0, mediaToString),
};

constexpr JSCFunctionListEntry kStyleSheetProto[] = {
    PR_JS_TAG("StyleSheet"),
    PR_JS_GET("type", styleSheetType),
    PR_STRING("href", styleSheetClass, prCssStyleSheetHref),
    PR_OBJECT("ownerNode", styleSheetClass, prCssStyleSheetOwnerNode, prJsDomNodeClass),
    PR_OBJECT("parentStyleSheet", styleSheetClass, prCssStyleSheetParentStyleSheet, prJsCssStyleSheetClass),
    PR_STRING("title", styleSheetClass, prCssStyleSheetTitle),
    PR_MEDIA(styleSheetClass, prCssStyleSheetMedia),
    PR_JS_GET_SET("disabled", (jsBoolGetter<&styleSheetClass, prCssStyleSheetDisabled>), setDisabled),
};

constexpr JSCFunctionListEntry kCssStyleSheetProto[] = {
    PR_JS_TAG("CSSStyleSheet"),
    PR_OBJECT("ownerRule", prJsCssStyleSheetClass, prCssStyleSheetOwnerRule, cssRuleClass),
    PR_JS_GET("cssRules",
        (jsSameObjectGetter<&prJsCssStyleSheetClass, prCssStyleSheetCssRules, &cssRuleListClass, "cssRules">)),
    PR_JS_METHOD("insertRule", 1, sheetInsertRule),
    PR_JS_MAGIC("deleteRule", 1, sheetDeleteRule, 0),
    PR_JS_METHOD("replaceSync", 1, replaceSync),
    // cssom-1/#legacy-css-style-sheet-members: rules is cssRules.
    PR_JS_GET("rules",
        (jsSameObjectGetter<&prJsCssStyleSheetClass, prCssStyleSheetCssRules, &cssRuleListClass, "cssRules">)),
    PR_JS_METHOD("addRule", 0, addRule),
    PR_JS_MAGIC("removeRule", 0, sheetDeleteRule, 1),
};

constexpr JSCFunctionListEntry kRuleListProto[] = {
    PR_JS_TAG("CSSRuleList"),
    PR_JS_METHOD("item", 1, (jsItemMethod<&cssRuleListClass, &kRuleListIndexed>)),
    PR_JS_GET("length", (jsNumberGetter<&cssRuleListClass, prCssRuleListLength>)),
};

constexpr JSCFunctionListEntry kStyleSheetListProto[] = {
    PR_JS_TAG("StyleSheetList"),
    PR_JS_METHOD("item", 1, (jsItemMethod<&prJsCssStyleSheetListClass, &kSheetListIndexed>)),
    PR_JS_GET("length", (jsNumberGetter<&prJsCssStyleSheetListClass, prCssStyleSheetListLength>)),
};

constexpr JSCFunctionListEntry kRuleProto[] = {
    PR_JS_TAG("CSSRule"),
    // cssom-1/#dom-cssrule-csstext: setting it does nothing.
    PR_STRING_SET("cssText", cssRuleClass, prCssRuleCssText, prCssRuleSetCssText),
    PR_OBJECT("parentRule", cssRuleClass, prCssRuleParentRule, cssRuleClass),
    PR_OBJECT("parentStyleSheet", cssRuleClass, prCssRuleParentStyleSheet, prJsCssStyleSheetClass),
    PR_JS_GET("type", (jsNumberGetter<&cssRuleClass, prCssRuleType>)),
};

constexpr JSCFunctionListEntry kGroupingRuleProto[] = {
    PR_JS_TAG("CSSGroupingRule"),
    PR_JS_GET("cssRules",
        (jsSameObjectGetter<&cssGroupingRuleClass, prCssGroupingRuleCssRules, &cssRuleListClass, "cssRules">)),
    PR_JS_METHOD("insertRule", 1, groupingInsertRule),
    PR_JS_METHOD("deleteRule", 1, groupingDeleteRule),
};

constexpr JSCFunctionListEntry kStyleRuleProto[] = {
    PR_JS_TAG("CSSStyleRule"),
    PR_STRING_SET("selectorText", cssStyleRuleClass, prCssStyleRuleSelectorText, prCssStyleRuleSetSelectorText),
    PR_STYLE(cssStyleRuleClass, prCssStyleRuleStyle),
    PR_JS_GET("styleMap", styleMap),
};

constexpr JSCFunctionListEntry kImportRuleProto[] = {
    PR_JS_TAG("CSSImportRule"),
    PR_STRING("href", cssImportRuleClass, prCssImportRuleHref),
    PR_MEDIA(cssImportRuleClass, prCssImportRuleMedia),
    PR_OBJECT("styleSheet", cssImportRuleClass, prCssImportRuleStyleSheet, prJsCssStyleSheetClass),
    PR_STRING("layerName", cssImportRuleClass, prCssImportRuleLayerName),
    PR_STRING("supportsText", cssImportRuleClass, prCssImportRuleSupportsText),
};

constexpr JSCFunctionListEntry kNamespaceRuleProto[] = {
    PR_JS_TAG("CSSNamespaceRule"),
    PR_STRING("namespaceURI", cssNamespaceRuleClass, prCssNamespaceRuleNamespaceUri),
    PR_STRING("prefix", cssNamespaceRuleClass, prCssNamespaceRulePrefix),
};

constexpr JSCFunctionListEntry kConditionRuleProto[] = {
    PR_JS_TAG("CSSConditionRule"),
    PR_STRING("conditionText", cssConditionRuleClass, prCssConditionRuleConditionText),
};

constexpr JSCFunctionListEntry kMediaRuleProto[] = {
    PR_JS_TAG("CSSMediaRule"),
    PR_MEDIA(cssMediaRuleClass, prCssMediaRuleMedia),
    PR_JS_GET("matches", (jsBoolGetter<&cssMediaRuleClass, prCssMediaRuleMatches>)),
};

constexpr JSCFunctionListEntry kSupportsRuleProto[] = {
    PR_JS_TAG("CSSSupportsRule"),
    PR_JS_GET("matches", (jsBoolGetter<&cssSupportsRuleClass, prCssSupportsRuleMatches>)),
};

constexpr JSCFunctionListEntry kContainerRuleProto[] = {
    PR_JS_TAG("CSSContainerRule"),
    PR_STRING("containerName", cssContainerRuleClass, prCssContainerRuleContainerName),
    PR_STRING("containerQuery", cssContainerRuleClass, prCssContainerRuleContainerQuery),
    PR_JS_GET("conditions", containerConditions),
};

constexpr JSCFunctionListEntry kNestedDeclarationsProto[] = {
    PR_JS_TAG("CSSNestedDeclarations"),
    PR_STYLE(cssNestedDeclarationsClass, prCssNestedDeclarationsStyle),
};

constexpr JSCFunctionListEntry kFontFaceRuleProto[] = {
    PR_JS_TAG("CSSFontFaceRule"),
    PR_STYLE(cssFontFaceRuleClass, prCssFontFaceRuleStyle),
};

constexpr JSCFunctionListEntry kPageRuleProto[] = {
    PR_JS_TAG("CSSPageRule"),
    PR_STRING_SET("selectorText", cssPageRuleClass, prCssPageRuleSelectorText, prCssPageRuleSetSelectorText),
    PR_STYLE(cssPageRuleClass, prCssPageRuleStyle),
};

constexpr JSCFunctionListEntry kMarginRuleProto[] = {
    PR_JS_TAG("CSSMarginRule"),
    PR_JS_GET("name", marginRuleName),
    PR_STYLE(cssMarginRuleClass, prCssMarginRuleStyle),
};

constexpr JSCFunctionListEntry kKeyframesRuleProto[] = {
    PR_JS_TAG("CSSKeyframesRule"),
    PR_STRING_SET("name", cssKeyframesRuleClass, prCssKeyframesRuleName, prCssKeyframesRuleSetName),
    PR_JS_GET("cssRules", keyframesRules),
    PR_JS_GET("length", (jsNumberGetter<&cssKeyframesRuleClass, prCssKeyframesRuleLength>)),
    PR_JS_MAGIC("appendRule", 1, keyframesMethod, KeyframesMethod_AppendRule),
    PR_JS_MAGIC("deleteRule", 1, keyframesMethod, KeyframesMethod_DeleteRule),
    PR_JS_MAGIC("findRule", 1, keyframesMethod, KeyframesMethod_FindRule),
};

constexpr JSCFunctionListEntry kKeyframeRuleProto[] = {
    PR_JS_TAG("CSSKeyframeRule"),
    PR_JS_GET_SET("keyText", (jsStringGetter<&cssKeyframeRuleClass, prCssKeyframeRuleKeyText>), setKeyText),
    PR_STYLE(cssKeyframeRuleClass, prCssKeyframeRuleStyle),
};

constexpr JSCFunctionListEntry kLayerBlockRuleProto[] = {
    PR_JS_TAG("CSSLayerBlockRule"),
    PR_STRING("name", cssLayerBlockRuleClass, prCssLayerBlockRuleName),
};

constexpr JSCFunctionListEntry kLayerStatementRuleProto[] = {
    PR_JS_TAG("CSSLayerStatementRule"),
    PR_JS_GET("nameList", (jsFrozenStringsGetter<&cssLayerStatementRuleClass, prCssLayerStatementRuleNameList>)),
};

constexpr JSCFunctionListEntry kScopeRuleProto[] = {
    PR_JS_TAG("CSSScopeRule"),
    PR_STRING("start", cssScopeRuleClass, prCssScopeRuleStart),
    PR_STRING("end", cssScopeRuleClass, prCssScopeRuleEnd),
};

constexpr JSCFunctionListEntry kStartingStyleRuleProto[] = {PR_JS_TAG("CSSStartingStyleRule")};

constexpr JSCFunctionListEntry kCounterStyleRuleProto[] = {
    PR_JS_TAG("CSSCounterStyleRule"),
    PR_STRING_SET("name", cssCounterStyleRuleClass, prCssCounterStyleRuleName, prCssCounterStyleRuleSetName),
    PR_STRING_SET("system", cssCounterStyleRuleClass, prCssCounterStyleRuleSystem, prCssCounterStyleRuleSetSystem),
    PR_STRING_SET("symbols", cssCounterStyleRuleClass, prCssCounterStyleRuleSymbols, prCssCounterStyleRuleSetSymbols),
    PR_STRING_SET("additiveSymbols", cssCounterStyleRuleClass, prCssCounterStyleRuleAdditiveSymbols,
        prCssCounterStyleRuleSetAdditiveSymbols),
    PR_STRING_SET("negative", cssCounterStyleRuleClass, prCssCounterStyleRuleNegative,
        prCssCounterStyleRuleSetNegative),
    PR_STRING_SET("prefix", cssCounterStyleRuleClass, prCssCounterStyleRulePrefix, prCssCounterStyleRuleSetPrefix),
    PR_STRING_SET("suffix", cssCounterStyleRuleClass, prCssCounterStyleRuleSuffix, prCssCounterStyleRuleSetSuffix),
    PR_STRING_SET("range", cssCounterStyleRuleClass, prCssCounterStyleRuleRange, prCssCounterStyleRuleSetRange),
    PR_STRING_SET("pad", cssCounterStyleRuleClass, prCssCounterStyleRulePad, prCssCounterStyleRuleSetPad),
    PR_STRING_SET("speakAs", cssCounterStyleRuleClass, prCssCounterStyleRuleSpeakAs, prCssCounterStyleRuleSetSpeakAs),
    PR_STRING_SET("fallback", cssCounterStyleRuleClass, prCssCounterStyleRuleFallback,
        prCssCounterStyleRuleSetFallback),
};

constexpr JSCFunctionListEntry kPropertyRuleProto[] = {
    PR_JS_TAG("CSSPropertyRule"),
    PR_STRING("name", cssPropertyRuleClass, prCssPropertyRuleName),
    PR_STRING("syntax", cssPropertyRuleClass, prCssPropertyRuleSyntax),
    PR_JS_GET("inherits", (jsBoolGetter<&cssPropertyRuleClass, prCssPropertyRuleInherits>)),
    PR_STRING("initialValue", cssPropertyRuleClass, prCssPropertyRuleInitialValue),
};

#define PR_FEATURE_MAP(name, getter) \
    PR_OBJECT(name, cssFontFeatureValuesRuleClass, getter, cssFontFeatureValuesMapClass)

constexpr JSCFunctionListEntry kFontFeatureValuesRuleProto[] = {
    PR_JS_TAG("CSSFontFeatureValuesRule"),
    PR_STRING_SET("fontFamily", cssFontFeatureValuesRuleClass, prCssFontFeatureValuesRuleFontFamily,
        prCssFontFeatureValuesRuleSetFontFamily),
    PR_FEATURE_MAP("annotation", prCssFontFeatureValuesRuleAnnotation),
    PR_FEATURE_MAP("ornaments", prCssFontFeatureValuesRuleOrnaments),
    PR_FEATURE_MAP("stylistic", prCssFontFeatureValuesRuleStylistic),
    PR_FEATURE_MAP("swash", prCssFontFeatureValuesRuleSwash),
    PR_FEATURE_MAP("characterVariant", prCssFontFeatureValuesRuleCharacterVariant),
    PR_FEATURE_MAP("styleset", prCssFontFeatureValuesRuleStyleset),
    PR_FEATURE_MAP("historicalForms", prCssFontFeatureValuesRuleHistoricalForms),
};

#undef PR_FEATURE_MAP

constexpr JSCFunctionListEntry kFontFeatureValuesMapProto[] = {
    PR_JS_TAG("CSSFontFeatureValuesMap"),
    PR_JS_GET("size", (jsNumberGetter<&cssFontFeatureValuesMapClass, prCssFontFeatureValuesMapSize>)),
    PR_JS_MAGIC("entries", 0, mapIterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, mapIterate, JsMapPart_Keys),
    PR_JS_MAGIC("values", 0, mapIterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, mapForEach),
    PR_JS_MAGIC("get", 1, mapKeyMethod, MapMethod_Get),
    PR_JS_MAGIC("has", 1, mapKeyMethod, MapMethod_Has),
    PR_JS_METHOD("set", 2, mapSet),
    PR_JS_MAGIC("delete", 1, mapKeyMethod, MapMethod_Delete),
    PR_JS_METHOD("clear", 0, mapClear),
};

constexpr JSCFunctionListEntry kFontPaletteValuesRuleProto[] = {
    PR_JS_TAG("CSSFontPaletteValuesRule"),
    PR_STRING("name", cssFontPaletteValuesRuleClass, prCssFontPaletteValuesRuleName),
    PR_STRING("fontFamily", cssFontPaletteValuesRuleClass, prCssFontPaletteValuesRuleFontFamily),
    PR_STRING("basePalette", cssFontPaletteValuesRuleClass, prCssFontPaletteValuesRuleBasePalette),
    PR_STRING("overrideColors", cssFontPaletteValuesRuleClass, prCssFontPaletteValuesRuleOverrideColors),
};

constexpr JSCFunctionListEntry kPositionTryRuleProto[] = {
    PR_JS_TAG("CSSPositionTryRule"),
    PR_STRING("name", cssPositionTryRuleClass, prCssPositionTryRuleName),
    PR_STYLE(cssPositionTryRuleClass, prCssPositionTryRuleStyle),
};

constexpr JSCFunctionListEntry kViewTransitionRuleProto[] = {
    PR_JS_TAG("CSSViewTransitionRule"),
    PR_STRING("navigation", cssViewTransitionRuleClass, prCssViewTransitionRuleNavigation),
    PR_JS_GET("types", (jsFrozenStringsGetter<&cssViewTransitionRuleClass, prCssViewTransitionRuleTypes>)),
};

constexpr JSCFunctionListEntry kFunctionRuleProto[] = {
    PR_JS_TAG("CSSFunctionRule"),
    PR_STRING("name", cssFunctionRuleClass, prCssFunctionRuleName),
    PR_JS_METHOD("getParameters", 0, (getParameters<&cssFunctionRuleClass, prCssFunctionRuleGetParameters>)),
    PR_STRING("returnType", cssFunctionRuleClass, prCssFunctionRuleReturnType),
};

constexpr JSCFunctionListEntry kFunctionDeclarationsProto[] = {
    PR_JS_TAG("CSSFunctionDeclarations"),
    PR_STYLE(cssFunctionDeclarationsClass, prCssFunctionDeclarationsStyle),
};

constexpr JSCFunctionListEntry kCustomMediaRuleProto[] = {
    PR_JS_TAG("CSSCustomMediaRule"),
    PR_STRING("name", cssCustomMediaRuleClass, prCssCustomMediaRuleName),
    PR_JS_GET("query", customMediaQuery),
};

constexpr JSCFunctionListEntry kSupportsConditionRuleProto[] = {
    PR_JS_TAG("CSSSupportsConditionRule"),
    PR_STRING("name", cssSupportsConditionRuleClass, prCssSupportsConditionRuleName),
};

constexpr JSCFunctionListEntry kMixinRuleProto[] = {
    PR_JS_TAG("CSSMixinRule"),
    PR_STRING("name", cssMixinRuleClass, prCssMixinRuleName),
    PR_JS_METHOD("getParameters", 0, (getParameters<&cssMixinRuleClass, prCssMixinRuleGetParameters>)),
    PR_JS_GET("contents", (jsBoolGetter<&cssMixinRuleClass, prCssMixinRuleContents>)),
};

constexpr JSCFunctionListEntry kApplyBlockRuleProto[] = {
    PR_JS_TAG("CSSApplyBlockRule"),
    PR_STRING("name", cssApplyBlockRuleClass, prCssApplyBlockRuleName),
    PR_JS_METHOD("getArguments", 0, (jsStringsMethod<&cssApplyBlockRuleClass, prCssApplyBlockRuleGetArguments>)),
};

constexpr JSCFunctionListEntry kApplyStatementRuleProto[] = {
    PR_JS_TAG("CSSApplyStatementRule"),
    PR_STRING("name", cssApplyStatementRuleClass, prCssApplyStatementRuleName),
    PR_JS_METHOD("getArguments", 0,
        (jsStringsMethod<&cssApplyStatementRuleClass, prCssApplyStatementRuleGetArguments>)),
};

constexpr JSCFunctionListEntry kContentsBlockRuleProto[] = {PR_JS_TAG("CSSContentsBlockRule")};

constexpr JSCFunctionListEntry kContentsStatementRuleProto[] = {PR_JS_TAG("CSSContentsStatementRule")};

constexpr JSCFunctionListEntry kStyleDeclarationProto[] = {
    PR_JS_TAG("CSSStyleDeclaration"),
    PR_JS_GET_SET("cssText", (jsStringGetter<&cssStyleDeclarationClass, prCssStyleDeclarationCssText>), setCssText),
    PR_JS_GET("length", (jsNumberGetter<&cssStyleDeclarationClass, prCssStyleDeclarationLength>)),
    PR_JS_METHOD("item", 1, declarationItemMethod),
    PR_JS_MAGIC("getPropertyValue", 1, propertyMethod, PropertyMethod_GetValue),
    PR_JS_MAGIC("getPropertyPriority", 1, propertyMethod, PropertyMethod_GetPriority),
    PR_JS_METHOD("setProperty", 2, setProperty),
    PR_JS_MAGIC("removeProperty", 1, propertyMethod, PropertyMethod_Remove),
    PR_OBJECT("parentRule", cssStyleDeclarationClass, prCssStyleDeclarationParentRule, cssRuleClass),
};

constexpr JSCFunctionListEntry kStylePropertiesProto[] = {
    PR_JS_TAG("CSSStyleProperties"),
    PR_JS_MAGIC_GET_SET("cssFloat", attributeGet, attributeSet, Names_Float << kNamesShift),
};

constexpr JSCFunctionListEntry kFontFaceDescriptorsProto[] = {PR_JS_TAG("CSSFontFaceDescriptors")};

constexpr JSCFunctionListEntry kPageDescriptorsProto[] = {PR_JS_TAG("CSSPageDescriptors")};

constexpr JSCFunctionListEntry kPositionTryDescriptorsProto[] = {PR_JS_TAG("CSSPositionTryDescriptors")};

constexpr JSCFunctionListEntry kFunctionDescriptorsProto[] = {PR_JS_TAG("CSSFunctionDescriptors")};

#undef PR_STRING
#undef PR_STRING_SET
#undef PR_OBJECT
#undef PR_STYLE
#undef PR_MEDIA

constexpr JSCFunctionListEntry kFontFaceProto[] = {
    PR_JS_TAG("FontFace"),
    PR_JS_MAGIC_GET_SET("family", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Family),
    PR_JS_MAGIC_GET_SET("style", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Style),
    PR_JS_MAGIC_GET_SET("weight", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Weight),
    PR_JS_MAGIC_GET_SET("stretch", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Stretch),
    PR_JS_MAGIC_GET_SET("width", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Stretch),
    PR_JS_MAGIC_GET_SET("unicodeRange", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_UnicodeRange),
    PR_JS_MAGIC_GET_SET("featureSettings", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_FeatureSettings),
    PR_JS_MAGIC_GET_SET("variationSettings", fontDescriptor, setFontDescriptor,
        PrCssFontFaceDescriptor_VariationSettings),
    PR_JS_MAGIC_GET_SET("display", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_Display),
    PR_JS_MAGIC_GET_SET("ascentOverride", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_AscentOverride),
    PR_JS_MAGIC_GET_SET("descentOverride", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_DescentOverride),
    PR_JS_MAGIC_GET_SET("lineGapOverride", fontDescriptor, setFontDescriptor, PrCssFontFaceDescriptor_LineGapOverride),
    PR_JS_GET("status", fontStatus),
    PR_JS_METHOD("load", 0, fontLoad),
    PR_JS_GET("loaded", fontLoaded),
};

constexpr JSCFunctionListEntry kFontFaceSetLoadEventProto[] = {
    PR_JS_TAG("FontFaceSetLoadEvent"),
    PR_JS_GET("fontfaces", fontEventFaces),
};

constexpr JSCFunctionListEntry kTransitionEventProto[] = {
    PR_JS_TAG("TransitionEvent"),
    PR_JS_MAGIC_GET("propertyName", animationEventGet, 0),
    PR_JS_MAGIC_GET("elapsedTime", animationEventGet, 1),
    PR_JS_MAGIC_GET("pseudoElement", animationEventGet, 2),
};

constexpr JSCFunctionListEntry kAnimationEventProto[] = {
    PR_JS_TAG("AnimationEvent"),
    PR_JS_MAGIC_GET("animationName", animationEventGet, 3),
    PR_JS_MAGIC_GET("elapsedTime", animationEventGet, 4),
    PR_JS_MAGIC_GET("pseudoElement", animationEventGet, 5),
};

constexpr JSCFunctionListEntry kFontFaceSetProto[] = {
    PR_JS_TAG("FontFaceSet"),
    PR_JS_DOM_EVENT_HANDLER(onloading), PR_JS_DOM_EVENT_HANDLER(onloadingdone), PR_JS_DOM_EVENT_HANDLER(onloadingerror),
    PR_JS_GET("size", (jsNumberGetter<&prJsCssFontFaceSetClass, prCssFontFaceSetSize>)),
    PR_JS_MAGIC("add", 1, setMember, 0),
    PR_JS_MAGIC("delete", 1, setMember, 1),
    PR_JS_MAGIC("has", 1, setMember, 2),
    PR_JS_METHOD("clear", 0, setClear),
    PR_JS_MAGIC("entries", 0, setIterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, setIterate, JsMapPart_Values),
    PR_JS_MAGIC("values", 0, setIterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, setForEach),
    PR_JS_GET("status", setStatus),
    PR_JS_GET("ready", setReady),
    PR_JS_METHOD("load", 1, setLoad),
    PR_JS_METHOD("check", 1, setCheck),
};

constexpr JSCFunctionListEntry kHighlightProto[] = {
    PR_JS_TAG("Highlight"),
    PR_JS_GET_SET("priority", highlightPriority, highlightSetPriority),
    PR_JS_GET_SET("type", highlightType, highlightSetType),
};

constexpr JSCFunctionListEntry kHighlightRegistryProto[] = {PR_JS_TAG("HighlightRegistry")};

// ---------------------------------------------------------------------------------------------------------------------
// Selectors on nodes and elements (dom/#dom-parentnode-queryselector, dom/#dom-element-matches)

// Implements querySelector and querySelectorAll, selected by the magic.
JSValue querySelector(JSContext* ctx, JSValueConst nodeObject, int argc, JSValueConst* argv, int magic) {
    PrDomNode* node = jsThis<PrDomNode>(ctx, nodeObject, prJsDomNodeClass);
    StringArgument selectors = {ctx};
    if (node == nullptr || !jsArgCount(ctx, argc, 1) || !selectors.read(argv[0]))
        return JS_EXCEPTION;
    if (magic == 0) {
        PrDomElement* found = nullptr;
        PrStatus status = prCssNodeQuerySelector(node, selectors.value(), &found);
        return status == PrStatus_Ok ? jsWrapAs(ctx, prJsDomElementClass, nodeObject, found, false) : prJsDomThrow(ctx, status);
    }
    PrDomNodeList* list = prDomNodeListCreate();
    PrStatus status = prCssNodeQuerySelectorAll(node, selectors.value(), list);
    if (status != PrStatus_Ok) {
        prDomNodeListDestroy(list);
        return prJsDomThrow(ctx, status);
    }
    JSValue object = jsWrapAs(ctx, prJsDomNodeListClass, nodeObject, list, true);
    if (JS_IsException(object))
        prDomNodeListDestroy(list);
    return object;
}

// Implements closest for magic 0, and matches for 1.
JSValue elementSelectors(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    StringArgument text = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    if (magic == 0) {
        PrDomElement* found = nullptr;
        PrStatus status = prCssElementClosest(element, text.value(), &found);
        if (status != PrStatus_Ok)
            return prJsDomThrow(ctx, status);
        return jsWrapAs(ctx, prJsDomElementClass, elementObject, found, false);
    }
    bool matched = false;
    PrStatus status = prCssElementMatches(element, text.value(), &matched);
    return status == PrStatus_Ok ? JS_NewBool(ctx, matched) : prJsDomThrow(ctx, status);
}

// ---------------------------------------------------------------------------------------------------------------------
// MediaQueryList (cssom-view-1/#mediaquerylist)

// Implements addListener and removeListener, selected by the magic.
JSValue mediaListener(JSContext* ctx, JSValueConst listObject, int argc, JSValueConst* argv, int magic) {
    PrCssMediaQueryList* list = jsThis<PrCssMediaQueryList>(ctx, listObject, prJsCssMediaQueryListClass);
    CallbackArgument<PrDomEventListener> callback = {ctx, listObject};
    if (list == nullptr || !callback.read(argc > 0 ? argv[0] : JS_UNDEFINED, prJsDomInvokeListener, true))
        return JS_EXCEPTION;
    if (magic == 0)
        prCssMediaQueryListAddListener(list, callback.get());
    else
        prCssMediaQueryListRemoveListener(list, callback.get());
    return JS_UNDEFINED;
}

constexpr JSCFunctionListEntry kMediaQueryListProto[] = {
    PR_JS_TAG("MediaQueryList"),
    PR_JS_GET("media", (jsStringGetter<&prJsCssMediaQueryListClass, prCssMediaQueryListMedia>)),
    PR_JS_GET("matches", (jsBoolGetter<&prJsCssMediaQueryListClass, prCssMediaQueryListMatches>)),
    PR_JS_MAGIC("addListener", 1, mediaListener, 0),
    PR_JS_MAGIC("removeListener", 1, mediaListener, 1),
    PR_JS_DOM_EVENT_HANDLER(onchange),
};

// ---------------------------------------------------------------------------------------------------------------------
// ResizeObserver, ResizeObserverEntry and ResizeObserverSize (resize-observer-1)

void retainResizeObserver(void* handle) {
    prCssResizeObserverRetain(static_cast<PrCssResizeObserver*>(handle));
}

// Returns a copy of the entry, which holds a reference to its target, for an entry's object to own.
PrCssResizeObserverEntry* resizeEntryCopy(const PrCssResizeObserverEntry& entry) {
    PrCssResizeObserverEntry* copy = alloc<PrCssResizeObserverEntry>();
    *copy = entry;
    prDomNodeRetain(prDomElementAsNode(copy->target));
    return copy;
}

void resizeEntryDestroy(PrCssResizeObserverEntry* PR_NULLABLE entry) {
    if (entry == nullptr)
        return;
    prDomNodeRelease(prDomElementAsNode(entry->target));
    prMemoryFree(entry);
}

// Calls the observer's callback with its entries and the observer, which is also the receiver
// (resize-observer-1/#broadcast-active-resize-observations). The callback's exception is reported.
void invokeResizeObserver(void* PR_NULLABLE context, PrCssResizeObserver* observer,
        const PrCssResizeObserverEntry* entries, uint32_t count) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue object = jsWrap(ctx, cssResizeObserverClass, JS_UNDEFINED, observer, false, retainResizeObserver);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        PrCssResizeObserverEntry* entry = resizeEntryCopy(entries[i]);
        JSValue value = jsWrap(ctx, cssResizeObserverEntryClass, JS_UNDEFINED, entry, true);
        if (JS_IsException(value))
            resizeEntryDestroy(entry);
        JS_SetPropertyUint32(ctx, array, i, value);
    }
    JSValue args[] = {array, object};
    JSValue result = JS_IsException(object) ? JS_EXCEPTION : jsCallbackCall(callback, nullptr, object, 2, args);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, array);
    JS_FreeValue(ctx, object);
}

// resize-observer-1/#dom-resizeobserver-resizeobserver. The observer belongs to the global object's document. Its
// object lives while the observer observes an element, even if script cannot reach it (resize-observer-1/#lifetime).
JSValue resizeObserverConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    CallbackArgument<PrCssResizeCallback> callback = {ctx, JS_UNDEFINED};
    if (!jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeResizeObserver))
        return JS_EXCEPTION;
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrCssResizeCallback value = *callback.get();
    value.observing = [](void* PR_NULLABLE context, PrCssResizeObserver* observer, bool observing) {
        jsPin(JS_GetRuntime(static_cast<JsCallback*>(context)->ctx), cssResizeObserverClass, observer, observing);
    };
    PrCssResizeObserver* observer = prCssResizeObserverCreate(document, &value);
    JSValue object = jsWrap(ctx, cssResizeObserverClass, JS_UNDEFINED, observer, true);
    if (JS_IsException(object)) {
        prCssResizeObserverRelease(observer);
        return object;
    }
    // The callback belongs to the observer's object, which is created after the callback.
    jsCallbackOwn(callback.callback, object);
    return jsConstructed(ctx, newTarget, object);
}

// resize-observer-1/#enumdef-resizeobserverboxoptions, in the order of PrCssResizeObserverBox.
constexpr PrStringView kResizeObserverBoxes[] = {"content-box", "border-box", "device-pixel-content-box"};

// Returns the element argument, or null with a TypeError thrown if `value` is not an Element.
PrDomElement* PR_NULLABLE elementArgument(JSContext* ctx, JSValueConst value) {
    PrDomElement* element = jsHandleAs<PrDomElement>(ctx, value, prJsDomElementClass);
    if (element == nullptr)
        JS_ThrowTypeError(ctx, "Expected an Element");
    return element;
}

// resize-observer-1/#dom-resizeobserver-observe. ResizeObserverOptions has one member, box, which is content-box if it
// is not given.
JSValue resizeObserverObserve(JSContext* ctx, JSValueConst observerObject, int argc, JSValueConst* argv) {
    PrCssResizeObserver* observer = jsThis<PrCssResizeObserver>(ctx, observerObject, cssResizeObserverClass);
    if (observer == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomElement* target = elementArgument(ctx, argv[0]);
    if (target == nullptr)
        return JS_EXCEPTION;
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    PrCssResizeObserverBox box = PrCssResizeObserverBox_ContentBox;
    if (present) {
        JSValue value = JS_GetPropertyStr(ctx, dictionary, "box");
        if (JS_IsException(value))
            return value;
        if (!JS_IsUndefined(value)) {
            StringArgument text = {ctx};
            bool read = text.read(value);
            JS_FreeValue(ctx, value);
            if (!read)
                return JS_EXCEPTION;
            uint32_t index = 0;
            while (index < countOf(kResizeObserverBoxes) && kResizeObserverBoxes[index] != text.value())
                ++index;
            if (index == countOf(kResizeObserverBoxes))
                return JS_ThrowTypeError(ctx, "The box is not a ResizeObserverBoxOptions value");
            box = PrCssResizeObserverBox(index);
        }
    }
    prCssResizeObserverObserve(observer, target, box);
    return JS_UNDEFINED;
}

JSValue resizeObserverUnobserve(JSContext* ctx, JSValueConst observerObject, int argc, JSValueConst* argv) {
    PrCssResizeObserver* observer = jsThis<PrCssResizeObserver>(ctx, observerObject, cssResizeObserverClass);
    if (observer == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomElement* target = elementArgument(ctx, argv[0]);
    if (target == nullptr)
        return JS_EXCEPTION;
    prCssResizeObserverUnobserve(observer, target);
    return JS_UNDEFINED;
}

JSValue resizeObserverDisconnect(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrCssResizeObserver* observer = jsThis<PrCssResizeObserver>(ctx, observerObject, cssResizeObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    prCssResizeObserverDisconnect(observer);
    return JS_UNDEFINED;
}

JSValue resizeEntryTarget(JSContext* ctx, JSValueConst entryObject) {
    PrCssResizeObserverEntry* entry = jsThis<PrCssResizeObserverEntry>(ctx, entryObject, cssResizeObserverEntryClass);
    return entry != nullptr ? jsWrapAs(ctx, prJsDomElementClass, entryObject, entry->target, false) : JS_EXCEPTION;
}

JSValue resizeEntryContentRect(JSContext* ctx, JSValueConst entryObject) {
    PrCssResizeObserverEntry* entry = jsThis<PrCssResizeObserverEntry>(ctx, entryObject, cssResizeObserverEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, entryObject, "contentRect", [&] { return prJsDomRectReadOnlyNew(ctx, entry->contentRect); });
}

// Implements the FrozenArray attributes borderBoxSize, contentBoxSize and devicePixelContentBoxSize, selected by the
// magic. Each holds one size, since a box of this level has one fragment.
JSValue resizeEntrySizes(JSContext* ctx, JSValueConst entryObject, int magic) {
    constexpr const char* kNames[] = {"borderBoxSize", "contentBoxSize", "devicePixelContentBoxSize"};
    PrCssResizeObserverEntry* entry = jsThis<PrCssResizeObserverEntry>(ctx, entryObject, cssResizeObserverEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, entryObject, kNames[magic], [&] {
        const PrCssResizeObserverSize sizes[] = {
            entry->borderBoxSize, entry->contentBoxSize,
            entry->devicePixelContentBoxSize,
        };
        JSValue size = jsNewValue(ctx, cssResizeObserverSizeClass, sizes[magic]);
        if (JS_IsException(size))
            return size;
        JSValue array = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, array, 0, size);
        JS_FreezeObject(ctx, array);
        return array;
    });
}

// Implements inlineSize and blockSize, selected by the magic.
JSValue resizeSizeGet(JSContext* ctx, JSValueConst sizeObject, int magic) {
    PrCssResizeObserverSize* size =
        jsValueOf<PrCssResizeObserverSize>(ctx, sizeObject, cssResizeObserverSizeClass, "ResizeObserverSize");
    if (size == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, magic == 0 ? size->inlineSize : size->blockSize);
}

constexpr JSCFunctionListEntry kResizeObserverProto[] = {
    PR_JS_TAG("ResizeObserver"),
    PR_JS_METHOD("observe", 1, resizeObserverObserve),
    PR_JS_METHOD("unobserve", 1, resizeObserverUnobserve),
    PR_JS_METHOD("disconnect", 0, resizeObserverDisconnect),
};

constexpr JSCFunctionListEntry kResizeObserverEntryProto[] = {
    PR_JS_TAG("ResizeObserverEntry"),
    PR_JS_GET("target", resizeEntryTarget),
    PR_JS_GET("contentRect", resizeEntryContentRect),
    PR_JS_MAGIC_GET("borderBoxSize", resizeEntrySizes, 0),
    PR_JS_MAGIC_GET("contentBoxSize", resizeEntrySizes, 1),
    PR_JS_MAGIC_GET("devicePixelContentBoxSize", resizeEntrySizes, 2),
};

constexpr JSCFunctionListEntry kResizeObserverSizeProto[] = {
    PR_JS_TAG("ResizeObserverSize"),
    PR_JS_MAGIC_GET("inlineSize", resizeSizeGet, 0),
    PR_JS_MAGIC_GET("blockSize", resizeSizeGet, 1),
};

// ---------------------------------------------------------------------------------------------------------------------
// IntersectionObserver and IntersectionObserverEntry (intersection-observer)

void retainIntersectionObserver(void* handle) {
    prCssIntersectionObserverRetain(static_cast<PrCssIntersectionObserver*>(handle));
}

void intersectionEntryDestroy(PrCssIntersectionObserverEntry* PR_NULLABLE entry) {
    if (entry == nullptr)
        return;
    prDomNodeRelease(prDomElementAsNode(entry->target));
    prMemoryFree(entry);
}

// Takes the observer's queued entries and returns them as an array of IntersectionObserverEntry objects. Each object
// owns its entry, and the reference to the target that the entry holds.
JSValue takeIntersectionRecords(JSContext* ctx, PrCssIntersectionObserver* observer) {
    JSValue array = JS_NewArray(ctx);
    PrCssIntersectionObserverEntry taken{};
    for (uint32_t i = 0; prCssIntersectionObserverTakeRecords(observer, &taken, 1) != 0; ++i) {
        PrCssIntersectionObserverEntry* entry = alloc<PrCssIntersectionObserverEntry>();
        *entry = taken;
        JSValue value = jsWrap(ctx, cssIntersectionObserverEntryClass, JS_UNDEFINED, entry, true);
        if (JS_IsException(value))
            intersectionEntryDestroy(entry);
        JS_SetPropertyUint32(ctx, array, i, value);
    }
    return array;
}

// Calls the observer's callback with its queued entries and the observer, which is also the receiver
// (intersection-observer/#notify-intersection-observers-algo). The callback's exception is reported.
void invokeIntersectionObserver(void* PR_NULLABLE context, PrCssIntersectionObserver* observer) {
    JsCallback* callback = static_cast<JsCallback*>(context);
    JSContext* ctx = callback->ctx;
    JSValue object =
        jsWrap(ctx, cssIntersectionObserverClass, JS_UNDEFINED, observer, false, retainIntersectionObserver);
    JSValue args[] = {takeIntersectionRecords(ctx, observer), object};
    JSValue result = JS_IsException(object) ? JS_EXCEPTION : jsCallbackCall(callback, nullptr, object, 2, args);
    if (JS_IsException(result))
        jsReportException(ctx);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, object);
}

// Reads a restricted double (webidl/#js-double). Throws a TypeError for a value that is not finite.
bool readThreshold(JSContext* ctx, JSValueConst value, RawArray<double>* out) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    if (!prF64IsFinite(number)) {
        JS_ThrowTypeError(ctx, "The threshold is not a finite number");
        return false;
    }
    out->push(number);
    return true;
}

// Reads the members of an IntersectionObserverInit (intersection-observer/#dictdef-intersectionobserverinit) in
// lexicographic order. The init's strings and thresholds stay valid while `margins` and `thresholds` live.
bool readIntersectionInit(JSContext* ctx, JSValueConst dictionary, PrCssIntersectionObserverInit* init,
        StringArgument margins[2], RawArray<double>* thresholds) {
    bool present = false;
    if (!jsDictionary(ctx, dictionary, &present))
        return false;
    if (!present)
        return true;
    JSValue delay = JS_GetPropertyStr(ctx, dictionary, "delay");
    bool ok = !JS_IsException(delay) && (JS_IsUndefined(delay) || JS_ToInt32(ctx, &init->delay, delay) >= 0);
    JS_FreeValue(ctx, delay);
    if (!ok)
        return false;
    // root: (Element or Document)?, where undefined and null are no root.
    JSValue root = JS_GetPropertyStr(ctx, dictionary, "root");
    if (JS_IsException(root))
        return false;
    if (!JS_IsUndefined(root) && !JS_IsNull(root)) {
        PrDomElement* element = jsHandleAs<PrDomElement>(ctx, root, prJsDomElementClass);
        PrDomDocument* document = element == nullptr ? jsHandleAs<PrDomDocument>(ctx, root, prJsDomDocumentClass) : nullptr;
        if (element != nullptr)
            init->root = prDomElementAsNode(element);
        else if (document != nullptr)
            init->root = prDomDocumentAsNode(document);
        else
            ok = false;
    }
    JS_FreeValue(ctx, root);
    if (!ok) {
        JS_ThrowTypeError(ctx, "The root is not an Element or a Document");
        return false;
    }
    bool given = false;
    if (!jsDictString(ctx, dictionary, "rootMargin", &margins[0], &given))
        return false;
    if (given)
        init->rootMargin = margins[0].value();
    if (!jsDictString(ctx, dictionary, "scrollMargin", &margins[1], &given))
        return false;
    if (given)
        init->scrollMargin = margins[1].value();
    // threshold: (double or sequence<double>), where an object is a sequence.
    JSValue threshold = JS_GetPropertyStr(ctx, dictionary, "threshold");
    if (JS_IsException(threshold))
        return false;
    if (JS_IsObject(threshold)) {
        ok = jsSequence(ctx, threshold, [&](JSValue item) {
            bool read = readThreshold(ctx, item, thresholds);
            JS_FreeValue(ctx, item);
            return read;
        });
    } else if (!JS_IsUndefined(threshold)) {
        ok = readThreshold(ctx, threshold, thresholds);
    }
    JS_FreeValue(ctx, threshold);
    if (!ok)
        return false;
    init->thresholds = thresholds->data;
    init->thresholdCount = thresholds->count;
    return jsDictBool(ctx, dictionary, "trackVisibility", &init->trackVisibility);
}

// intersection-observer/#dom-intersectionobserver-intersectionobserver. The observer belongs to the global object's
// document. Its object lives while the observer observes an element, even if script cannot reach it
// (intersection-observer/#lifetime).
JSValue intersectionObserverConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    CallbackArgument<PrCssIntersectionCallback> callback = {ctx, JS_UNDEFINED};
    if (!jsArgCount(ctx, argc, 1) || !callback.readFunction(argv[0], invokeIntersectionObserver))
        return JS_EXCEPTION;
    PrCssIntersectionObserverInit init = prCssIntersectionObserverInitDefault();
    StringArgument margins[2] = {{ctx}, {ctx}};
    RawArray<double> thresholds;
    bool read = readIntersectionInit(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &init, margins, &thresholds);
    PrDomDocument* document = read ? prJsDomGlobalDocument(ctx) : nullptr;
    if (document == nullptr) {
        thresholds.reset();
        return JS_EXCEPTION;
    }
    PrCssIntersectionCallback value = *callback.get();
    value.observing = [](void* PR_NULLABLE context, PrCssIntersectionObserver* observer, bool observing) {
        jsPin(JS_GetRuntime(static_cast<JsCallback*>(context)->ctx), cssIntersectionObserverClass, observer, observing);
    };
    PrCssIntersectionObserver* observer = nullptr;
    PrStatus status = prCssIntersectionObserverCreate(document, &init, &value, &observer);
    thresholds.reset();
    if (status == PrStatus_Syntax)
        return prJsDomThrow(ctx, status);
    if (status != PrStatus_Ok)
        return JS_ThrowRangeError(ctx, "A threshold is outside the range from 0 to 1");
    JSValue object = jsWrap(ctx, cssIntersectionObserverClass, JS_UNDEFINED, observer, true);
    if (JS_IsException(object)) {
        prCssIntersectionObserverRelease(observer);
        return object;
    }
    // The callback belongs to the observer's object, which is created after the callback.
    jsCallbackOwn(callback.callback, object);
    return jsConstructed(ctx, newTarget, object);
}

// Implements observe for magic 0, and unobserve for 1.
JSValue intersectionObserverTarget(JSContext* ctx, JSValueConst observerObject, int argc, JSValueConst* argv, int magic) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    if (observer == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomElement* target = elementArgument(ctx, argv[0]);
    if (target == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        prCssIntersectionObserverObserve(observer, target);
    else
        prCssIntersectionObserverUnobserve(observer, target);
    return JS_UNDEFINED;
}

JSValue intersectionObserverDisconnect(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    prCssIntersectionObserverDisconnect(observer);
    return JS_UNDEFINED;
}

JSValue intersectionObserverTakeRecords(JSContext* ctx, JSValueConst observerObject, int, JSValueConst*) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    return observer != nullptr ? takeIntersectionRecords(ctx, observer) : JS_EXCEPTION;
}

JSValue intersectionObserverRoot(JSContext* ctx, JSValueConst observerObject) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    PrDomNode* root = prCssIntersectionObserverRoot(observer);
    return root != nullptr ? jsWrapAs(ctx, prJsDomNodeClass, observerObject, root, false) : JS_NULL;
}

// Implements rootMargin for magic 0, and scrollMargin for 1.
JSValue intersectionObserverMargin(JSContext* ctx, JSValueConst observerObject, int magic) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return jsTakeString(ctx, prCssIntersectionObserverRootMargin(observer));
    return jsTakeString(ctx, prCssIntersectionObserverScrollMargin(observer));
}

// The [SameObject]-like FrozenArray of the thresholds, which never change.
JSValue intersectionObserverThresholds(JSContext* ctx, JSValueConst observerObject) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    if (observer == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, observerObject, "thresholds", [&] {
        uint32_t count = 0;
        const double* thresholds = prCssIntersectionObserverThresholds(observer, &count);
        JSValue array = JS_NewArray(ctx);
        for (uint32_t i = 0; i < count; ++i)
            JS_SetPropertyUint32(ctx, array, i, JS_NewFloat64(ctx, thresholds[i]));
        JS_FreezeObject(ctx, array);
        return array;
    });
}

JSValue intersectionObserverDelay(JSContext* ctx, JSValueConst observerObject) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    return observer != nullptr ? JS_NewInt32(ctx, prCssIntersectionObserverDelay(observer)) : JS_EXCEPTION;
}

JSValue intersectionObserverTrackVisibility(JSContext* ctx, JSValueConst observerObject) {
    PrCssIntersectionObserver* observer =
        jsThis<PrCssIntersectionObserver>(ctx, observerObject, cssIntersectionObserverClass);
    return observer != nullptr ? JS_NewBool(ctx, prCssIntersectionObserverTrackVisibility(observer)) : JS_EXCEPTION;
}

// The members of IntersectionObserverEntry, selected by the magic.
enum {
    IntersectionMember_Time,
    IntersectionMember_RootBounds,
    IntersectionMember_BoundingClientRect,
    IntersectionMember_IntersectionRect,
    IntersectionMember_IsIntersecting,
    IntersectionMember_IsVisible,
    IntersectionMember_IntersectionRatio,
    IntersectionMember_Target,
};
typedef uint8_t IntersectionMember;

JSValue intersectionEntryGet(JSContext* ctx, JSValueConst entryObject, int magic) {
    PrCssIntersectionObserverEntry* entry =
        jsThis<PrCssIntersectionObserverEntry>(ctx, entryObject, cssIntersectionObserverEntryClass);
    if (entry == nullptr)
        return JS_EXCEPTION;
    // Each rectangle is the same object each time it is read.
    auto rect = [&](const char* name, const PrDomRect& value) {
        return jsSameObject(ctx, entryObject, name, [&] { return prJsDomRectReadOnlyNew(ctx, value); });
    };
    switch (magic) {
        case IntersectionMember_Time:
            return JS_NewFloat64(ctx, entry->time);
        case IntersectionMember_RootBounds:
            return rect("rootBounds", entry->rootBounds);
        case IntersectionMember_BoundingClientRect:
            return rect("boundingClientRect", entry->boundingClientRect);
        case IntersectionMember_IntersectionRect:
            return rect("intersectionRect", entry->intersectionRect);
        case IntersectionMember_IsIntersecting:
            return JS_NewBool(ctx, entry->isIntersecting);
        case IntersectionMember_IsVisible:
            return JS_NewBool(ctx, entry->isVisible);
        case IntersectionMember_IntersectionRatio:
            return JS_NewFloat64(ctx, entry->intersectionRatio);
        default:
            return jsWrapAs(ctx, prJsDomElementClass, entryObject, entry->target, false);
    }
}

constexpr JSCFunctionListEntry kIntersectionObserverProto[] = {
    PR_JS_TAG("IntersectionObserver"),
    PR_JS_GET("root", intersectionObserverRoot),
    PR_JS_MAGIC_GET("rootMargin", intersectionObserverMargin, 0),
    PR_JS_MAGIC_GET("scrollMargin", intersectionObserverMargin, 1),
    PR_JS_GET("thresholds", intersectionObserverThresholds),
    PR_JS_GET("delay", intersectionObserverDelay),
    PR_JS_GET("trackVisibility", intersectionObserverTrackVisibility),
    PR_JS_MAGIC("observe", 1, intersectionObserverTarget, 0),
    PR_JS_MAGIC("unobserve", 1, intersectionObserverTarget, 1),
    PR_JS_METHOD("disconnect", 0, intersectionObserverDisconnect),
    PR_JS_METHOD("takeRecords", 0, intersectionObserverTakeRecords),
};

constexpr JSCFunctionListEntry kIntersectionObserverEntryProto[] = {
    PR_JS_TAG("IntersectionObserverEntry"),
    PR_JS_MAGIC_GET("time", intersectionEntryGet, IntersectionMember_Time),
    PR_JS_MAGIC_GET("rootBounds", intersectionEntryGet, IntersectionMember_RootBounds),
    PR_JS_MAGIC_GET("boundingClientRect", intersectionEntryGet, IntersectionMember_BoundingClientRect),
    PR_JS_MAGIC_GET("intersectionRect", intersectionEntryGet, IntersectionMember_IntersectionRect),
    PR_JS_MAGIC_GET("isIntersecting", intersectionEntryGet, IntersectionMember_IsIntersecting),
    PR_JS_MAGIC_GET("isVisible", intersectionEntryGet, IntersectionMember_IsVisible),
    PR_JS_MAGIC_GET("intersectionRatio", intersectionEntryGet, IntersectionMember_IntersectionRatio),
    PR_JS_MAGIC_GET("target", intersectionEntryGet, IntersectionMember_Target),
};

// ---------------------------------------------------------------------------------------------------------------------
// Matrices as CSS text (geometry-1/#dommatrixreadonly-stringification-behavior,
// geometry-1/#dom-dommatrix-setmatrixvalue)

JSValue matrixToString(JSContext* ctx, JSValueConst matrixObject, int, JSValueConst*) {
    PrDomMatrix* matrix = prJsDomMatrixFromValue(ctx, matrixObject);
    if (matrix == nullptr)
        return JS_EXCEPTION;
    PrRawString text = {};
    if (prCssMatrixToString(matrix, &text) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return jsTakeString(ctx, text);
}

// geometry-1/#dom-dommatrix-setmatrixvalue
JSValue setMatrixValue(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    StringArgument text = {ctx};
    if (!prJsDomMatrixIsMutable(ctx, object))
        return JS_ThrowTypeError(ctx, "The receiver is not a DOMMatrix");
    if (!text.read(argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (prCssMatrixFromString(text.value(), prJsDomMatrixFromValue(ctx, object)) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    return JS_DupValue(ctx, object);
}

constexpr PrJsDomMatrixParser kMatrixParser = {.parse = prCssMatrixFromString};

// A class whose objects are views of data that the engine owns.
#define PR_VIEW(interface, id, proto) PR_JS_CLASS(interface, id, jsViewFinalizer<&id>, proto)

// The module's classes, each after its base.
constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("MediaQueryList", prJsCssMediaQueryListClass,
            (jsFinalizer<&prJsCssMediaQueryListClass, prCssMediaQueryListRelease>), kMediaQueryListProto),
        .base = &prJsDomEventTargetClass,
    },
    {
        PR_JS_CLASS("IntersectionObserver", cssIntersectionObserverClass,
            (jsFinalizer<&cssIntersectionObserverClass, prCssIntersectionObserverRelease>), kIntersectionObserverProto),
        .constructor = intersectionObserverConstruct,
        .length = 1,
    },
    {
        PR_JS_CLASS("IntersectionObserverEntry", cssIntersectionObserverEntryClass,
            (jsFinalizer<&cssIntersectionObserverEntryClass, intersectionEntryDestroy>),
            kIntersectionObserverEntryProto)
    },
    {
        PR_JS_CLASS("ResizeObserver", cssResizeObserverClass,
            (jsFinalizer<&cssResizeObserverClass, prCssResizeObserverRelease>), kResizeObserverProto),
        .constructor = resizeObserverConstruct,
        .length = 1,
    },
    {
        PR_JS_CLASS("ResizeObserverEntry", cssResizeObserverEntryClass,
            (jsFinalizer<&cssResizeObserverEntryClass, resizeEntryDestroy>), kResizeObserverEntryProto)
    },
    {
        .name = "ResizeObserverSize",
        .id = &cssResizeObserverSizeClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kResizeObserverSizeProto,
        .protoCount = int(countOf(kResizeObserverSizeProto)),
    },
    {
        PR_JS_CLASS("FontFace", cssFontFaceClass, jsViewFinalizer<&cssFontFaceClass>, kFontFaceProto),
        .constructor = fontFaceConstruct,
        .length = 2,
        .references = &kFontFaceReferences,
        .wrapped = fontFaceWrapped,
    },
    {
        PR_JS_CLASS("FontFaceSet", prJsCssFontFaceSetClass, jsViewFinalizer<&prJsCssFontFaceSetClass>,
            kFontFaceSetProto),
        .base = &prJsDomEventTargetClass,
        .iterator = "values",
    },
    {
        PR_JS_CLASS("FontFaceSetLoadEvent", cssFontFaceSetLoadEventClass,
            jsViewFinalizer<&cssFontFaceSetLoadEventClass>, kFontFaceSetLoadEventProto),
        .constructor = fontEventConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("TransitionEvent", cssTransitionEventClass, jsViewFinalizer<&cssTransitionEventClass>,
            kTransitionEventProto),
        .constructor = animationEventConstructAs<0>,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("AnimationEvent", cssAnimationEventClass, jsViewFinalizer<&cssAnimationEventClass>,
            kAnimationEventProto),
        .constructor = animationEventConstructAs<1>,
        .length = 1,
        .base = &prJsDomEventClass,
    },
    {
        PR_JS_CLASS("Highlight", cssHighlightClass, jsViewFinalizer<&cssHighlightClass>, kHighlightProto),
        .constructor = highlightConstruct,
        .references = &kHighlightReferences,
        .collection = &kHighlightRanges,
    },
    {PR_VIEW("HighlightRegistry", cssHighlightRegistryClass, kHighlightRegistryProto), .collection = &kRegistryEntries},
    {PR_VIEW("MediaList", cssMediaListClass, kMediaListProto), .indexed = &kMediaIndexed},
    {PR_VIEW("StyleSheet", styleSheetClass, kStyleSheetProto), .references = &kSheetReferences},
    {
        PR_JS_CLASS("CSSStyleSheet", prJsCssStyleSheetClass, jsViewFinalizer<&prJsCssStyleSheetClass>,
            kCssStyleSheetProto),
        .constructor = sheetConstruct,
        .base = &styleSheetClass,
    },
    {PR_VIEW("CSSRuleList", cssRuleListClass, kRuleListProto), .indexed = &kRuleListIndexed},
    {PR_VIEW("StyleSheetList", prJsCssStyleSheetListClass, kStyleSheetListProto), .indexed = &kSheetListIndexed},

    {PR_VIEW("CSSRule", cssRuleClass, kRuleProto), PR_JS_CONSTANTS(kRuleConstants), .references = &kRuleReferences},
    {PR_VIEW("CSSGroupingRule", cssGroupingRuleClass, kGroupingRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSConditionRule", cssConditionRuleClass, kConditionRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSStyleRule", cssStyleRuleClass, kStyleRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSImportRule", cssImportRuleClass, kImportRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSNamespaceRule", cssNamespaceRuleClass, kNamespaceRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSMediaRule", cssMediaRuleClass, kMediaRuleProto), .base = &cssConditionRuleClass},
    {PR_VIEW("CSSSupportsRule", cssSupportsRuleClass, kSupportsRuleProto), .base = &cssConditionRuleClass},
    {PR_VIEW("CSSContainerRule", cssContainerRuleClass, kContainerRuleProto), .base = &cssConditionRuleClass},
    {PR_VIEW("CSSNestedDeclarations", cssNestedDeclarationsClass, kNestedDeclarationsProto), .base = &cssRuleClass},
    {PR_VIEW("CSSFontFaceRule", cssFontFaceRuleClass, kFontFaceRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSPageRule", cssPageRuleClass, kPageRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSMarginRule", cssMarginRuleClass, kMarginRuleProto), .base = &cssRuleClass},
    {
        PR_VIEW("CSSKeyframesRule", cssKeyframesRuleClass, kKeyframesRuleProto),
        .base = &cssRuleClass,
        .indexed = &kKeyframesIndexed,
    },
    {PR_VIEW("CSSKeyframeRule", cssKeyframeRuleClass, kKeyframeRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSLayerBlockRule", cssLayerBlockRuleClass, kLayerBlockRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSLayerStatementRule", cssLayerStatementRuleClass, kLayerStatementRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSScopeRule", cssScopeRuleClass, kScopeRuleProto), .base = &cssGroupingRuleClass},
    {
        PR_VIEW("CSSStartingStyleRule", cssStartingStyleRuleClass, kStartingStyleRuleProto),
        .base = &cssGroupingRuleClass,
    },
    {PR_VIEW("CSSCounterStyleRule", cssCounterStyleRuleClass, kCounterStyleRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSPropertyRule", cssPropertyRuleClass, kPropertyRuleProto), .base = &cssRuleClass},
    {
        PR_VIEW("CSSFontFeatureValuesRule", cssFontFeatureValuesRuleClass, kFontFeatureValuesRuleProto),
        .base = &cssRuleClass,
    },
    {
        PR_VIEW("CSSFontFeatureValuesMap", cssFontFeatureValuesMapClass, kFontFeatureValuesMapProto),
        .iterator = "entries",
    },
    {
        PR_VIEW("CSSFontPaletteValuesRule", cssFontPaletteValuesRuleClass, kFontPaletteValuesRuleProto),
        .base = &cssRuleClass,
    },
    {PR_VIEW("CSSPositionTryRule", cssPositionTryRuleClass, kPositionTryRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSViewTransitionRule", cssViewTransitionRuleClass, kViewTransitionRuleProto), .base = &cssRuleClass},
    {PR_VIEW("CSSFunctionRule", cssFunctionRuleClass, kFunctionRuleProto), .base = &cssGroupingRuleClass},
    {
        PR_VIEW("CSSFunctionDeclarations", cssFunctionDeclarationsClass, kFunctionDeclarationsProto),
        .base = &cssRuleClass,
    },
    {PR_VIEW("CSSCustomMediaRule", cssCustomMediaRuleClass, kCustomMediaRuleProto), .base = &cssRuleClass},
    {
        PR_VIEW("CSSSupportsConditionRule", cssSupportsConditionRuleClass, kSupportsConditionRuleProto),
        .base = &cssGroupingRuleClass,
    },
    {PR_VIEW("CSSMixinRule", cssMixinRuleClass, kMixinRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSApplyBlockRule", cssApplyBlockRuleClass, kApplyBlockRuleProto), .base = &cssGroupingRuleClass},
    {PR_VIEW("CSSApplyStatementRule", cssApplyStatementRuleClass, kApplyStatementRuleProto), .base = &cssRuleClass},
    {
        PR_VIEW("CSSContentsBlockRule", cssContentsBlockRuleClass, kContentsBlockRuleProto),
        .base = &cssGroupingRuleClass,
    },
    {
        PR_VIEW("CSSContentsStatementRule", cssContentsStatementRuleClass, kContentsStatementRuleProto),
        .base = &cssRuleClass,
    },

    {PR_VIEW("CSSStyleDeclaration", cssStyleDeclarationClass, kStyleDeclarationProto), .indexed = &kDeclarationIndexed},
    {
        PR_VIEW("CSSStyleProperties", prJsCssStylePropertiesClass, kStylePropertiesProto),
        .base = &cssStyleDeclarationClass,
        .indexed = &kDeclarationIndexed,
    },
    {
        PR_VIEW("CSSFontFaceDescriptors", cssFontFaceDescriptorsClass, kFontFaceDescriptorsProto),
        .base = &cssStyleDeclarationClass,
        .indexed = &kDeclarationIndexed,
    },
    {
        PR_VIEW("CSSPageDescriptors", cssPageDescriptorsClass, kPageDescriptorsProto),
        .base = &cssStyleDeclarationClass,
        .indexed = &kDeclarationIndexed,
    },
    {
        PR_VIEW("CSSPositionTryDescriptors", cssPositionTryDescriptorsClass, kPositionTryDescriptorsProto),
        .base = &cssStyleDeclarationClass,
        .indexed = &kDeclarationIndexed,
    },
    {
        PR_VIEW("CSSFunctionDescriptors", cssFunctionDescriptorsClass, kFunctionDescriptorsProto),
        .base = &cssStyleDeclarationClass,
        .indexed = &kDeclarationIndexed,
    },
};

#undef PR_VIEW

// The classes whose prototypes hold an attribute for each name in a list.
struct AttributeList {
    const JSClassID* id;
    Names names;
};
constexpr AttributeList kAttributes[] = {
    {&prJsCssStylePropertiesClass, Names_Properties},
    {&cssFontFaceDescriptorsClass, Names_FontFace},
    {&cssPageDescriptorsClass, Names_Page},
    {&cssPositionTryDescriptorsClass, Names_PositionTry},
    {&cssFunctionDescriptorsClass, Names_Function},
};

// The members that CSS adds to the DOM's interfaces (cssom-1/#extensions-to-the-document-or-shadow-root-interface,
// css-font-loading-3/#document-font-face-set).
constexpr JSCFunctionListEntry kParentNodeMembers[] = {
    PR_JS_MAGIC("querySelector", 1, querySelector, 0),
    PR_JS_MAGIC("querySelectorAll", 1, querySelector, 1),
};
constexpr JSCFunctionListEntry kElementMembers[] = {
    PR_JS_MAGIC("closest", 1, elementSelectors, 0),
    PR_JS_MAGIC("matches", 1, elementSelectors, 1),
    PR_JS_MAGIC("webkitMatchesSelector", 1, elementSelectors, 1),
};
constexpr JSCFunctionListEntry kMatrixReadOnlyMembers[] = {PR_JS_METHOD("toString", 0, matrixToString)};
constexpr JSCFunctionListEntry kMatrixMembers[] = {PR_JS_METHOD("setMatrixValue", 1, setMatrixValue)};
constexpr JSCFunctionListEntry kDocumentMembers[] = {
    PR_JS_GET("styleSheets", (jsSameObjectGetter<&prJsDomDocumentClass, prCssDocumentStyleSheets,
        &prJsCssStyleSheetListClass, "styleSheets">)),
    PR_JS_GET("fonts", prJsCssFonts),
};
constexpr JSCFunctionListEntry kShadowRootMembers[] = {
    PR_JS_GET("styleSheets", (jsSameObjectGetter<&prJsDomShadowRootClass, prCssShadowRootStyleSheets,
        &prJsCssStyleSheetListClass, "styleSheets">)),
};

// Adds `members` to the prototype of the class `id`, which another module defines.
// ---------------------------------------------------------------------------------------------------------------------
// CSSOM View on Element and Document (cssom-view-1/#extension-to-the-element-interface)

JSValue nodeToJs(JSContext* ctx, JSValueConst owner, PrDomNode* PR_NULLABLE node) {
    return jsWrapAs(ctx, prJsDomNodeClass, owner, node, false);
}

JSValue getClientRects(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    uint32_t count = prCssElementGetClientRects(element, nullptr, 0);
    RawArray<PrDomRect> rects;
    rects.resize(count);
    prCssElementGetClientRects(element, rects.data, count);
    JSValue list = prJsDomRectListNew(ctx, rects.data, count);
    rects.reset();
    return list;
}

JSValue getBoundingClientRect(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    return element != nullptr ? prJsDomRectNew(ctx, prCssElementGetBoundingClientRect(element)) : JS_EXCEPTION;
}

// Implements scrollTop and scrollLeft, selected by the magic.
JSValue scrollOffsetGet(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, magic == 0 ? prCssElementScrollTop(element) : prCssElementScrollLeft(element));
}

JSValue scrollOffsetSet(JSContext* ctx, JSValueConst elementObject, JSValueConst input, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    double value = 0;
    if (element == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    if (magic == 0)
        prCssElementSetScrollTop(element, value);
    else
        prCssElementSetScrollLeft(element, value);
    return JS_UNDEFINED;
}

// Implements the measures of cssom-view-1 that are longs, selected by the magic.
enum {
    Measure_ScrollWidth,
    Measure_ScrollHeight,
    Measure_ClientTop,
    Measure_ClientLeft,
    Measure_ClientWidth,
    Measure_ClientHeight,
};
typedef uint8_t Measure;

JSValue measureGet(JSContext* ctx, JSValueConst elementObject, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    int32_t (*const measures[])(PrDomElement*) = {
        prCssElementScrollWidth, prCssElementScrollHeight,
        prCssElementClientTop,   prCssElementClientLeft,
        prCssElementClientWidth, prCssElementClientHeight,
    };
    return JS_NewInt32(ctx, measures[magic](element));
}

// Implements scroll and scrollTo for magic 0, scrollBy for 1. Each scroll completes as it is made.
JSValue scrollMethod(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    PrCssScrollToOptions options{};
    if (element == nullptr || !prJsCssScrollToOptionsRead(ctx, argc, argv, &options))
        return prJsCssScrollPromise(ctx, false);
    if (magic == 0)
        prCssElementScroll(element, &options);
    else
        prCssElementScrollBy(element, &options);
    return prJsCssScrollPromise(ctx, true);
}

// Reads the ScrollLogicalPosition that a member names. Returns false with a TypeError thrown for a name that the
// enumeration does not have.
bool logicalPositionRead(JSContext* ctx, JSValueConst dictionary, const char* name, PrCssScrollLogicalPosition* out) {
    StringArgument value = {ctx};
    bool given = false;
    if (!jsDictString(ctx, dictionary, name, &value, &given))
        return false;
    if (!given)
        return true;
    constexpr PrStringView kNames[] = {"start", "center", "end", "nearest"};
    for (uint32_t i = 0; i < countOf(kNames); ++i) {
        if (value.value() == kNames[i]) {
            *out = PrCssScrollLogicalPosition(i);
            return true;
        }
    }
    JS_ThrowTypeError(ctx, "The provided value is not a valid ScrollLogicalPosition");
    return false;
}

// Implements scrollIntoView, whose argument is a boolean or a ScrollIntoViewOptions
// (cssom-view-1/#dom-element-scrollintoview).
JSValue scrollIntoView(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return prJsCssScrollPromise(ctx, false);
    PrCssScrollIntoViewOptions options = prCssScrollIntoViewOptionsDefault();
    JSValueConst arg = argc != 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsBool(arg) || (!JS_IsObject(arg) && !JS_IsUndefined(arg) && !JS_IsNull(arg))) {
        if (!JS_ToBool(ctx, arg))
            options.block = PrCssScrollLogicalPosition_End;
    } else {
        bool present = false;
        StringArgument behavior = {ctx};
        // ScrollOptions's behavior is read first, then block and inline in order.
        if (!jsDictionary(ctx, arg, &present) || (present && (!jsDictString(ctx, arg, "behavior", &behavior) ||
                !logicalPositionRead(ctx, arg, "block", &options.block) ||
                !logicalPositionRead(ctx, arg, "inline", &options.inlinePosition))))
            return prJsCssScrollPromise(ctx, false);
    }
    prCssElementScrollIntoView(element, &options);
    return prJsCssScrollPromise(ctx, true);
}

JSValue scrollingElement(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrDomElement* element = prCssDocumentScrollingElement(document);
    return nodeToJs(ctx, documentObject, element != nullptr ? prDomElementAsNode(element) : nullptr);
}

// Reads the point that elementFromPoint and elementsFromPoint take, which is two doubles (webidl/#js-double). Each must
// be finite, or a TypeError is thrown.
bool pointArguments(JSContext* ctx, int argc, JSValueConst* argv, double* x, double* y) {
    if (!jsArgCount(ctx, argc, 2) || !jsArgNumber(ctx, argc, argv, 0, 0, x) || !jsArgNumber(ctx, argc, argv, 1, 0, y))
        return false;
    if (!prF64IsFinite(*x) || !prF64IsFinite(*y)) {
        JS_ThrowTypeError(ctx, "The provided double value is non-finite");
        return false;
    }
    return true;
}

JSValue elementFromPoint(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    double x = 0;
    double y = 0;
    if (document == nullptr || !pointArguments(ctx, argc, argv, &x, &y))
        return JS_EXCEPTION;
    PrDomElement* element = prCssDocumentElementFromPoint(document, x, y);
    return nodeToJs(ctx, documentObject, element != nullptr ? prDomElementAsNode(element) : nullptr);
}

JSValue elementsFromPoint(JSContext* ctx, JSValueConst documentObject, int argc, JSValueConst* argv) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    double x = 0;
    double y = 0;
    if (document == nullptr || !pointArguments(ctx, argc, argv, &x, &y))
        return JS_EXCEPTION;
    uint32_t count = prCssDocumentElementsFromPoint(document, x, y, nullptr, 0);
    RawArray<PrDomElement*> elements;
    elements.resize(count);
    prCssDocumentElementsFromPoint(document, x, y, elements.data, count);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue node = nodeToJs(ctx, documentObject, prDomElementAsNode(elements.data[i]));
        if (JS_IsException(node) || JS_SetPropertyUint32(ctx, array, i, node) < 0) {
            JS_FreeValue(ctx, array);
            array = JS_EXCEPTION;
            break;
        }
    }
    elements.reset();
    return array;
}

// The members of CSSOM View on Element (cssom-view-1/#extension-to-the-element-interface) and on Document
// (cssom-view-1/#extensions-to-the-document-interface).
constexpr JSCFunctionListEntry kViewElementMembers[] = {
    PR_JS_METHOD("getClientRects", 0, getClientRects),
    PR_JS_METHOD("getBoundingClientRect", 0, getBoundingClientRect),
    PR_JS_MAGIC("scroll", 0, scrollMethod, 0),
    PR_JS_MAGIC("scrollTo", 0, scrollMethod, 0),
    PR_JS_MAGIC("scrollBy", 0, scrollMethod, 1),
    PR_JS_METHOD("scrollIntoView", 0, scrollIntoView),
    PR_JS_MAGIC_GET_SET("scrollTop", scrollOffsetGet, scrollOffsetSet, 0),
    PR_JS_MAGIC_GET_SET("scrollLeft", scrollOffsetGet, scrollOffsetSet, 1),
    PR_JS_MAGIC_GET("scrollWidth", measureGet, Measure_ScrollWidth),
    PR_JS_MAGIC_GET("scrollHeight", measureGet, Measure_ScrollHeight),
    PR_JS_MAGIC_GET("clientTop", measureGet, Measure_ClientTop),
    PR_JS_MAGIC_GET("clientLeft", measureGet, Measure_ClientLeft),
    PR_JS_MAGIC_GET("clientWidth", measureGet, Measure_ClientWidth),
    PR_JS_MAGIC_GET("clientHeight", measureGet, Measure_ClientHeight),
};
constexpr JSCFunctionListEntry kViewDocumentMembers[] = {
    PR_JS_GET("scrollingElement", scrollingElement),
    PR_JS_METHOD("elementFromPoint", 2, elementFromPoint),
    PR_JS_METHOD("elementsFromPoint", 2, elementsFromPoint),
};

// Adds `members` to the prototype of the class `id`, which another module defines.

void addMembers(JSContext* ctx, JSClassID id, const JSCFunctionListEntry* members, int count) {
    JSValue proto = JS_GetClassProto(ctx, id);
    JS_SetPropertyFunctionList(ctx, proto, members, count);
    JS_FreeValue(ctx, proto);
}

// Gives the event targets of CSS their classes.
JSClassID classifyCssTarget(void* handle) {
    PrDomEventTarget* target = static_cast<PrDomEventTarget*>(handle);
    if (prCssFontFaceSetQuery(target) != nullptr)
        return prJsCssFontFaceSetClass;
    if (prCssMediaQueryListQuery(target) != nullptr)
        return prJsCssMediaQueryListClass;
    return prJsDomEventTargetClass;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsCssStylePropertiesClass;
JSClassID prJsCssStyleSheetClass;
JSClassID prJsCssStyleSheetListClass;
JSClassID prJsCssFontFaceSetClass;
JSClassID prJsCssMediaQueryListClass;

void prJsCssBind(JSContext* ctx) {
    PR_ASSERT(jsIndexAtomsTagged(ctx));
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    for (const AttributeList& attributes : kAttributes) {
        JSValue proto = JS_GetClassProto(ctx, *attributes.id);
        defineAttributes(ctx, proto, attributes.names);
        JS_FreeValue(ctx, proto);
    }
    const JSClassID parents[] = {prJsDomDocumentClass, prJsDomDocumentFragmentClass, prJsDomElementClass};
    for (JSClassID parent : parents)
        addMembers(ctx, parent, kParentNodeMembers, int(countOf(kParentNodeMembers)));
    addMembers(ctx, prJsDomElementClass, kElementMembers, int(countOf(kElementMembers)));
    addMembers(ctx, prJsDomDocumentClass, kDocumentMembers, int(countOf(kDocumentMembers)));
    addMembers(ctx, prJsDomShadowRootClass, kShadowRootMembers, int(countOf(kShadowRootMembers)));
    addMembers(ctx, prJsDomMatrixReadOnlyClass, kMatrixReadOnlyMembers, int(countOf(kMatrixReadOnlyMembers)));
    addMembers(ctx, prJsDomMatrixClass, kMatrixMembers, int(countOf(kMatrixMembers)));
    prJsDomSetMatrixParser(JS_GetRuntime(ctx), &kMatrixParser);
    addMembers(ctx, prJsDomElementClass, kViewElementMembers, int(countOf(kViewElementMembers)));
    addMembers(ctx, prJsDomDocumentClass, kViewDocumentMembers, int(countOf(kViewDocumentMembers)));

    JSRuntime* runtime = JS_GetRuntime(ctx);
    jsAddClassifier(runtime, cssRuleClass, classifyRule);
    jsAddClassifier(runtime, prJsDomEventClass, classifyCssEvent);
    jsAddClassifier(runtime, prJsDomEventTargetClass, classifyCssTarget);
    jsAddClassifier(runtime, cssStyleDeclarationClass, classifyDeclarations);

    // cssom-1/#namespacedef-css, to which the Typed OM adds its factories.
    JSValue css = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, css, kCssNamespace, int(countOf(kCssNamespace)));
    prJsCssTypedOmBind(ctx, css);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, "CSS", css, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
}

JSValue prJsCssInlineStyle(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, "style", [&] {
        return jsWrap(ctx, prJsCssStylePropertiesClass, elementObject, prCssElementStyle(element), false);
    });
}

JSValue prJsCssSetInlineStyle(JSContext* ctx, JSValueConst object, JSValueConst input) {
    return jsPutForwards<"style", "cssText">(ctx, object, input);
}

JSValue prJsCssAttributeStyleMap(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrCssStyleDeclaration* declarations = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(element));
    return jsSameObject(ctx, elementObject, "attributeStyleMap", [&] {
        return jsWrap(ctx, prJsCssStylePropertyMapClass, elementObject, declarations, false);
    });
}

JSValue prJsCssFonts(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    observeFonts(ctx, document);
    return jsSameObject(ctx, documentObject, "fonts", [&] {
        return jsWrap(ctx, prJsCssFontFaceSetClass, documentObject, prCssDocumentFonts(document), false);
    });
}

JSValue prJsCssScrollPromise(JSContext* ctx, bool scrolled) {
    if (scrolled)
        return jsSettledPromise(ctx, JS_UNDEFINED, false);
    JSValue error = JS_GetException(ctx);
    JSValue promise = jsSettledPromise(ctx, error, true);
    JS_FreeValue(ctx, error);
    return promise;
}

bool prJsCssScrollToOptionsRead(JSContext* ctx, int argc, JSValueConst* argv, PrCssScrollToOptions* out) {
    *out = PrCssScrollToOptions{};
    if (argc >= 2) {
        out->hasLeft = true;
        out->hasTop = true;
        return jsArgNumber(ctx, argc, argv, 0, 0, &out->left) && jsArgNumber(ctx, argc, argv, 1, 0, &out->top);
    }
    JSValueConst options = argc != 0 ? argv[0] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, options, &present))
        return false;
    if (!present)
        return true;
    // ScrollOptions's behavior, read first, is a ScrollBehavior (cssom-view-1/#enumdef-scrollbehavior).
    StringArgument behavior = {ctx};
    bool given = false;
    if (!jsDictString(ctx, options, "behavior", &behavior, &given))
        return false;
    PrStringView value = behavior.value();
    if (given && value != "auto" && value != "instant" && value != "smooth") {
        JS_ThrowTypeError(ctx, "The provided value is not a valid ScrollBehavior");
        return false;
    }
    return jsDictNumber(ctx, options, "left", &out->left, &out->hasLeft) &&
        jsDictNumber(ctx, options, "top", &out->top, &out->hasTop);
}
