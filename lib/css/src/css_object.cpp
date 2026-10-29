// CSSOM Objects
//
// cssom-1 - https://drafts.csswg.org/cssom-1/

#include "css/src/css_object.h"

#include "pr/alloc.h"
#include "pr/assert.h"

#include "css/src/css_font_face.h"
#include "css/src/css_parser.h"
#include "css/src/css_root.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// The root of an object's tree: the element that owns the topmost sheet, or a sheet or rule with no owner. An owning
// element counts the references to the objects as its own.
struct Root {
    PrDomNode* PR_NULLABLE owner;
    PrCssStyleSheet* PR_NULLABLE sheet;
    PrCssRule* PR_NULLABLE rule;
};

Root rootOfRule(PrCssRule* rule);

Root rootOfSheet(PrCssStyleSheet* sheet) {
    if (sheet->ownerRule != nullptr)
        return rootOfRule(sheet->ownerRule);
    if (sheet->ownerNode != nullptr)
        return Root{sheet->ownerNode, nullptr, nullptr};
    return Root{nullptr, sheet, nullptr};
}

Root rootOfRule(PrCssRule* rule) {
    while (rule->parent != nullptr)
        rule = rule->parent;
    if (rule->sheet != nullptr)
        return rootOfSheet(rule->sheet);
    return Root{nullptr, nullptr, rule};
}

void freeSheet(PrCssStyleSheet* sheet);

void freeRule(PrCssRule* rule) {
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        if (rule->children.data[i] != nullptr)
            freeRule(rule->children.data[i]);
    }
    rule->children.reset();
    if (rule->imported != nullptr)
        freeSheet(rule->imported);
    if (rule->face != nullptr) {
        cssFontFaceDisconnect(rule->face);
        cssFontFaceRelease(rule->face);
    }
    cssRuleDataRelease(rule->data);
    prMemoryFree(rule);
}

void freeSheet(PrCssStyleSheet* sheet) {
    for (uint32_t i = 0; i < sheet->rules.count; ++i) {
        if (sheet->rules.data[i] != nullptr)
            freeRule(sheet->rules.data[i]);
    }
    sheet->rules.reset();
    cssSheetDataRelease(sheet->data);
    prStringReset(&sheet->href);
    prStringReset(&sheet->title);
    cssValueListReset(&sheet->media.queries);
    if (sheet->parser != nullptr) {
        cssParserReset(sheet->parser);
        prMemoryFree(sheet->parser);
    }
    prMemoryFree(sheet);
}

// Adds `count` references to the tree whose root is `root`.
void rootAdd(Root root, uint32_t count) {
    if (count == 0)
        return;
    if (root.owner != nullptr)
        domNodeRetainBy(root.owner, count);
    else if (root.sheet != nullptr)
        root.sheet->retained += count;
    else
        root.rule->retained += count;
}

// Removes `count` references from the tree whose root is `root`, and frees the tree if it is left with none.
void rootTake(Root root, uint32_t count) {
    if (count == 0)
        return;
    if (root.owner != nullptr) {
        domNodeReleaseBy(root.owner, count);
    } else if (root.sheet != nullptr) {
        PR_ASSERT(root.sheet->retained >= count);
        root.sheet->retained -= count;
        if (root.sheet->retained == 0)
            freeSheet(root.sheet);
    } else {
        PR_ASSERT(root.rule->retained >= count);
        root.rule->retained -= count;
        if (root.rule->retained == 0)
            freeRule(root.rule);
    }
}

uint32_t ruleReferences(const PrCssRule* rule) {
    uint32_t count = rule->references;
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        if (rule->children.data[i] != nullptr)
            count += ruleReferences(rule->children.data[i]);
    }
    if (rule->imported != nullptr)
        count += cssStyleSheetReferences(rule->imported);
    return count;
}

// Detaches a rule and the rules inside it from their sheet (cssom-1/#remove-a-css-rule).
void leaveSheet(PrCssRule* rule) {
    rule->sheet = nullptr;
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        if (rule->children.data[i] != nullptr)
            leaveSheet(rule->children.data[i]);
    }
}

// Makes the object of a rule removed from its list the root of its own tree. The object takes its references with it,
// and is freed if it has none.
void takeOut(PrCssRule* rule) {
    Root from = rootOfRule(rule);
    uint32_t count = ruleReferences(rule);
    leaveSheet(rule);
    rule->parent = nullptr;
    rule->retained = count;
    if (count == 0)
        freeRule(rule);
    else
        rootTake(from, count);
}

// Returns true if anything besides the sheet holds the sheet's data: another sheet, or a document's cache. The data of
// an imported sheet belongs to its rule, which nothing else holds.
bool shared(const PrCssStyleSheet* sheet) {
    return sheet->ownerRule == nullptr && sheet->data->references > 1;
}

// Whom a change made through a sheet or its rules notifies. Notify_Document notifies the document, as a change to every
// rule. Notify_Caller notifies no one, because the caller tells the document which rule it changes
// (cssDocumentRuleChanged).
enum {
    Notify_Document,
    Notify_Caller,
};
typedef uint8_t Notify;

// Gives the sheet its own data if it shares its data. The copy shares the rules, and the objects of the rules continue
// to present them.
void ownData(PrCssStyleSheet* sheet, Notify notify = Notify_Document) {
    // Every change through the sheet or its rules makes its data exclusive first.
    if (notify == Notify_Document)
        cssDocumentStyleChanged(cssStyleSheetDocument(sheet));
    if (!shared(sheet))
        return;
    CssSheetData* copy = cssSheetDataCopy(sheet->data);
    cssSheetDataRelease(sheet->data);
    sheet->data = copy;
}

// Returns the index of `object` in `objects`.
uint32_t indexOf(const Array<PrCssRule* PR_NULLABLE>& objects, const PrCssRule* object) {
    uint32_t index = 0;
    while (objects.data[index] != object)
        ++index;
    return index;
}

// Returns true if anything holds the rule besides the list that it is in, if `listed`, and the object that presents it,
// if `presented`.
bool sharedRule(const CssRuleData* rule, bool listed, bool presented) {
    return rule->references > (listed ? 1u : 0u) + (presented ? 1u : 0u);
}

// Returns a copy of the rule at `index` of `list`, or of the rule of `object` if the rule is in no list. The copy
// replaces the original in both the list and the object.
CssRuleData* copyInto(Array<CssRuleData* PR_NONNULL>* PR_NULLABLE list, uint32_t index,
                      PrCssRule* PR_NULLABLE object) {
    PR_ASSERT(list != nullptr || object != nullptr);
    CssRuleData* copy = cssRuleDataCopy(list != nullptr ? list->data[index] : object->data);
    if (object != nullptr) {
        cssRuleDataRetain(copy);
        cssRuleDataRelease(object->data);
        object->data = copy;
    }
    if (list != nullptr)
        cssRuleListReplace(list, index, copy);
    else
        cssRuleDataRelease(copy);
    return copy;
}

// Returns the data that `rule` presents, made exclusive to its tree. The data of the enclosing rules and of its sheet
// is made exclusive first.
CssRuleData* ownRule(PrCssRule* rule, Notify notify = Notify_Document) {
    Array<CssRuleData* PR_NONNULL>* list = nullptr;
    const Array<PrCssRule* PR_NULLABLE>* objects = nullptr;
    if (rule->parent != nullptr) {
        list = &ownRule(rule->parent, notify)->children;
        objects = &rule->parent->children;
    } else if (rule->sheet != nullptr) {
        ownData(rule->sheet, notify);
        list = &rule->sheet->data->rules;
        objects = &rule->sheet->rules;
    }
    if (!sharedRule(rule->data, list != nullptr, true))
        return rule->data;
    return copyInto(list, objects != nullptr ? indexOf(*objects, rule) : 0, rule);
}

// Returns the document whose sheet holds the rule, or null for a rule in no sheet or a sheet in no document.
const PrDomDocument* PR_NULLABLE documentOf(const PrCssRule* rule) {
    return rule->sheet != nullptr ? cssStyleSheetDocument(rule->sheet) : nullptr;
}

// Makes the rules in the block of `data`, and in their blocks, exclusive to their tree. `object` presents `data` if it
// is not null.
void ownBlock(CssRuleData* data, PrCssRule* PR_NULLABLE object) {
    for (uint32_t i = 0; i < data->children.count; ++i) {
        PrCssRule* child = object != nullptr ? object->children.data[i] : nullptr;
        CssRuleData* held = data->children.data[i];
        if (sharedRule(held, true, child != nullptr))
            held = copyInto(&data->children, i, child);
        ownBlock(held, child);
    }
}

// Allocates a zeroed object for `data`, of the most specific interface that presents it.
PrCssRule* allocRule(const CssRuleData* data) {
    switch (cssRuleKind(data)) {
        case PrCssRuleKind_Style: return allocZeroed<PrCssStyleRule>();
        case PrCssRuleKind_Import: return allocZeroed<PrCssImportRule>();
        case PrCssRuleKind_Namespace: return allocZeroed<PrCssNamespaceRule>();
        case PrCssRuleKind_Media: return allocZeroed<PrCssMediaRule>();
        case PrCssRuleKind_Supports: return allocZeroed<PrCssSupportsRule>();
        case PrCssRuleKind_Container: return allocZeroed<PrCssContainerRule>();
        case PrCssRuleKind_NestedDeclarations: return allocZeroed<PrCssNestedDeclarations>();
        case PrCssRuleKind_FontFace: return allocZeroed<PrCssFontFaceRule>();
        case PrCssRuleKind_Page: return allocZeroed<PrCssPageRule>();
        case PrCssRuleKind_Margin: return allocZeroed<PrCssMarginRule>();
        case PrCssRuleKind_Keyframes: return allocZeroed<PrCssKeyframesRule>();
        case PrCssRuleKind_Keyframe: return allocZeroed<PrCssKeyframeRule>();
        case PrCssRuleKind_LayerBlock: return allocZeroed<PrCssLayerBlockRule>();
        case PrCssRuleKind_LayerStatement: return allocZeroed<PrCssLayerStatementRule>();
        case PrCssRuleKind_Scope: return allocZeroed<PrCssScopeRule>();
        case PrCssRuleKind_StartingStyle: return allocZeroed<PrCssStartingStyleRule>();
        case PrCssRuleKind_CounterStyle: return allocZeroed<PrCssCounterStyleRule>();
        case PrCssRuleKind_Property: return allocZeroed<PrCssPropertyRule>();
        case PrCssRuleKind_FontFeatureValues: return allocZeroed<PrCssFontFeatureValuesRule>();
        case PrCssRuleKind_FontPaletteValues: return allocZeroed<PrCssFontPaletteValuesRule>();
        case PrCssRuleKind_PositionTry: return allocZeroed<PrCssPositionTryRule>();
        case PrCssRuleKind_ViewTransition: return allocZeroed<PrCssViewTransitionRule>();
        case PrCssRuleKind_Function: return allocZeroed<PrCssFunctionRule>();
        case PrCssRuleKind_FunctionDeclarations: return allocZeroed<PrCssFunctionDeclarations>();
        case PrCssRuleKind_CustomMedia: return allocZeroed<PrCssCustomMediaRule>();
        case PrCssRuleKind_SupportsCondition: return allocZeroed<PrCssSupportsConditionRule>();
        case PrCssRuleKind_Mixin: return allocZeroed<PrCssMixinRule>();
        case PrCssRuleKind_ApplyBlock: return allocZeroed<PrCssApplyBlockRule>();
        case PrCssRuleKind_ApplyStatement: return allocZeroed<PrCssApplyStatementRule>();
        case PrCssRuleKind_ContentsBlock: return allocZeroed<PrCssContentsBlockRule>();
        case PrCssRuleKind_ContentsStatement: return allocZeroed<PrCssContentsStatementRule>();
        default: break;
    }
    // A block of an @font-feature-values rule is a CSSFontFeatureValuesMap (css-fonts-4/#om-fontfeaturevalues).
    switch (data->atRule) {
        case CssAtRule_Annotation:
        case CssAtRule_Ornaments:
        case CssAtRule_Stylistic:
        case CssAtRule_Swash:
        case CssAtRule_CharacterVariant:
        case CssAtRule_Styleset:
        case CssAtRule_HistoricalForms: return allocZeroed<PrCssFontFeatureValuesMap>();
        default: return allocZeroed<PrCssRule>();
    }
}

PrCssRule* makeRule(CssRuleData* data, PrCssStyleSheet* PR_NULLABLE sheet,
                    PrCssRule* PR_NULLABLE parent) {
    PrCssRule* rule = allocRule(data);
    rule->data = data;
    cssRuleDataRetain(data);
    rule->sheet = sheet;
    rule->parent = parent;
    for (uint32_t i = 0; i < data->children.count; ++i)
        rule->children.push(nullptr);
    rule->cssRules = PrCssRuleList{{nullptr, rule}};
    rule->style = PrCssStyleDeclaration{{rule, nullptr, nullptr, nullptr, nullptr, nullptr, false}};
    rule->media = PrCssMediaList{{nullptr, rule}};
    return rule;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Lifetime

PrCssStyleSheet* cssStyleSheetCreate(CssSheetData* data) {
    PrCssStyleSheet* sheet = allocZeroed<PrCssStyleSheet>();
    sheet->data = data;
    for (uint32_t i = 0; i < data->rules.count; ++i)
        sheet->rules.push(nullptr);
    sheet->href = prStringNull();
    sheet->title = prStringNull();
    sheet->cssRules = PrCssRuleList{{sheet, nullptr}};
    sheet->mediaList = PrCssMediaList{{sheet, nullptr}};
    return sheet;
}

void cssStyleSheetRetain(PrCssStyleSheet* sheet) {
    ++sheet->references;
    rootAdd(rootOfSheet(sheet), 1);
}

void cssStyleSheetRelease(PrCssStyleSheet* PR_NULLABLE sheet) {
    if (sheet == nullptr)
        return;
    PR_ASSERT(sheet->references != 0);
    --sheet->references;
    rootTake(rootOfSheet(sheet), 1);
}

void cssRuleRetain(PrCssRule* rule) {
    ++rule->references;
    rootAdd(rootOfRule(rule), 1);
}

void cssRuleRelease(PrCssRule* PR_NULLABLE rule) {
    if (rule == nullptr)
        return;
    PR_ASSERT(rule->references != 0);
    --rule->references;
    rootTake(rootOfRule(rule), 1);
}

void cssStyleSheetLeaveOwner(PrCssStyleSheet* sheet) {
    PrDomNode* owner = sheet->ownerNode;
    PR_ASSERT(owner != nullptr && sheet->ownerRule == nullptr);
    uint32_t count = cssStyleSheetReferences(sheet);
    sheet->ownerNode = nullptr;
    sheet->retained = count;
    if (count == 0)
        freeSheet(sheet);
    else
        domNodeReleaseBy(owner, count);
}

void cssStyleSheetFreeWithOwner(PrCssStyleSheet* PR_NULLABLE sheet) {
    if (sheet == nullptr)
        return;
    PR_ASSERT(cssStyleSheetReferences(sheet) == 0);
    freeSheet(sheet);
}

uint32_t cssStyleSheetReferences(const PrCssStyleSheet* sheet) {
    uint32_t count = sheet->references;
    for (uint32_t i = 0; i < sheet->rules.count; ++i) {
        if (sheet->rules.data[i] != nullptr)
            count += ruleReferences(sheet->rules.data[i]);
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// The objects of rules

PrCssRuleKind cssRuleKind(const CssRuleData* rule) {
    switch (rule->kind) {
        case CssRuleDataKind_Style:
            return PrCssRuleKind_Style;
        case CssRuleDataKind_Keyframe:
            return PrCssRuleKind_Keyframe;
        case CssRuleDataKind_NestedDeclarations:
            // The declarations of an @function body are CSSFunctionDeclarations rules instead.
            return rule->style.rule == CssAtRule_Function ? PrCssRuleKind_FunctionDeclarations
                                                          : PrCssRuleKind_NestedDeclarations;
        case CssRuleDataKind_At:
            break;
    }
    if (cssRuleType(rule) == PrCssRuleType_Margin)
        return PrCssRuleKind_Margin;
    // A block and a statement of the same at-rule are different interfaces.
    switch (kCssAtRuleAlias[rule->atRule]) {
        case CssAtRule_Import: return PrCssRuleKind_Import;
        case CssAtRule_Namespace: return PrCssRuleKind_Namespace;
        case CssAtRule_Media: return PrCssRuleKind_Media;
        case CssAtRule_Supports: return PrCssRuleKind_Supports;
        case CssAtRule_Container: return PrCssRuleKind_Container;
        case CssAtRule_FontFace: return PrCssRuleKind_FontFace;
        case CssAtRule_Page: return PrCssRuleKind_Page;
        case CssAtRule_Keyframes: return PrCssRuleKind_Keyframes;
        case CssAtRule_Layer:
            return rule->block ? PrCssRuleKind_LayerBlock : PrCssRuleKind_LayerStatement;
        case CssAtRule_Scope: return PrCssRuleKind_Scope;
        case CssAtRule_StartingStyle: return PrCssRuleKind_StartingStyle;
        case CssAtRule_CounterStyle: return PrCssRuleKind_CounterStyle;
        case CssAtRule_Property: return PrCssRuleKind_Property;
        case CssAtRule_FontFeatureValues: return PrCssRuleKind_FontFeatureValues;
        case CssAtRule_FontPaletteValues: return PrCssRuleKind_FontPaletteValues;
        case CssAtRule_PositionTry: return PrCssRuleKind_PositionTry;
        case CssAtRule_ViewTransition: return PrCssRuleKind_ViewTransition;
        case CssAtRule_Function: return PrCssRuleKind_Function;
        case CssAtRule_CustomMedia: return PrCssRuleKind_CustomMedia;
        case CssAtRule_SupportsCondition: return PrCssRuleKind_SupportsCondition;
        case CssAtRule_Mixin: return PrCssRuleKind_Mixin;
        case CssAtRule_Apply:
            return rule->block ? PrCssRuleKind_ApplyBlock : PrCssRuleKind_ApplyStatement;
        case CssAtRule_Contents:
            return rule->block ? PrCssRuleKind_ContentsBlock : PrCssRuleKind_ContentsStatement;
        default: return PrCssRuleKind_Rule;
    }
}

PrCssRule* cssStyleSheetRule(PrCssStyleSheet* sheet, uint32_t index) {
    PR_ASSERT(sheet->rules.count == sheet->data->rules.count && index < sheet->rules.count);
    if (sheet->rules.data[index] == nullptr)
        sheet->rules.data[index] = makeRule(sheet->data->rules.data[index], sheet, nullptr);
    return sheet->rules.data[index];
}

PrCssRule* cssRuleChild(PrCssRule* rule, uint32_t index) {
    PR_ASSERT(rule->children.count == rule->data->children.count && index < rule->children.count);
    if (rule->children.data[index] == nullptr)
        rule->children.data[index] = makeRule(rule->data->children.data[index], rule->sheet, rule);
    return rule->children.data[index];
}

const Array<CssRuleData* PR_NONNULL>& cssRuleListData(const PrCssRuleList* list) {
    return list->sheet != nullptr ? list->sheet->data->rules : list->rule->data->children;
}

PrCssRule* cssRuleListItem(PrCssRuleList* list, uint32_t index) {
    return list->sheet != nullptr ? cssStyleSheetRule(list->sheet, index)
                                  : cssRuleChild(list->rule, index);
}

PrCssStyleSheet* PR_NULLABLE cssImportRuleSheet(PrCssRule* rule) {
    if (rule->imported == nullptr && rule->data->imported != nullptr) {
        CssSheetData* data = rule->data->imported;
        cssSheetDataRetain(data);
        PrCssStyleSheet* sheet = cssStyleSheetCreate(data);
        sheet->href = prStringClone(&data->href);
        sheet->title = prStringEmpty();
        sheet->ownerRule = rule;
        rule->imported = sheet;
    }
    return rule->imported;
}

CssParserSlot cssRuleParser(PrCssRule* rule) {
    return rule->sheet != nullptr ? &rule->sheet->parser : nullptr;
}

const CssRuleChain* PR_NULLABLE cssRuleChain(const PrCssRule* PR_NULLABLE rule,
                                             Array<CssRuleChain>* out) {
    uint32_t depth = 0;
    for (const PrCssRule* at = rule; at != nullptr; at = at->parent)
        ++depth;
    out->count = 0;
    out->reserve(depth);
    for (const PrCssRule* at = rule; at != nullptr; at = at->parent)
        out->push(CssRuleChain{at->data, nullptr});
    for (uint32_t i = 1; i < depth; ++i)
        out->data[i - 1].outer = &out->data[i];
    return depth != 0 ? &out->data[0] : nullptr;
}

const CssDeclarationBlock* cssStyleDeclarationBlock(const PrCssStyleDeclaration* declaration) {
    if (declaration->refresh != nullptr)
        declaration->refresh(declaration);
    return declaration->parentRule != nullptr ? &declaration->parentRule->data->style
                                              : declaration->block;
}

CssAtRule cssStyleDeclarationAtRule(const PrCssStyleDeclaration* declaration) {
    return declaration->parentRule != nullptr ? declaration->parentRule->data->style.rule : declaration->block->rule;
}

CssParserSlot cssStyleDeclarationParser(PrCssStyleDeclaration* declaration) {
    return declaration->parentRule != nullptr ? cssRuleParser(declaration->parentRule) : nullptr;
}

CssRuleData* cssRuleWritable(PrCssRule* rule) {
    return ownRule(rule);
}

CssRuleData* cssRuleWritableTree(PrCssRule* rule) {
    CssRuleData* data = ownRule(rule);
    ownBlock(data, rule);
    return data;
}

CssDeclarationBlock* cssStyleDeclarationWritable(PrCssStyleDeclaration* declaration) {
    PrCssRule* rule = declaration->parentRule;
    if (rule == nullptr)
        return declaration->block;
    // A change to a rule's declarations restyles what its selectors can match.
    CssRuleData* data = ownRule(rule, Notify_Caller);
    cssDocumentRuleChanged(documentOf(rule), data);
    return &data->style;
}

CssMediaQueries* cssMediaListWritable(PrCssMediaList* list) {
    if (list->rule != nullptr)
        return &cssRuleWritable(list->rule)->media;
    PrCssStyleSheet* sheet = list->sheet;
    cssDocumentStyleChanged(cssStyleSheetDocument(sheet));
    return sheet->ownerRule != nullptr ? &cssRuleWritable(sheet->ownerRule)->media : &sheet->media;
}

void cssStyleDeclarationUpdateOwner(PrCssStyleDeclaration* declaration) {
    if (declaration->updateOwner != nullptr)
        declaration->updateOwner(declaration);
}

const CssMediaQueries* cssMediaListQueries(const PrCssMediaList* list) {
    if (list->rule != nullptr)
        return &list->rule->data->media;
    const PrCssStyleSheet* sheet = list->sheet;
    return sheet->ownerRule != nullptr ? &sheet->ownerRule->data->media : &sheet->media;
}

const PrDomDocument* PR_NULLABLE cssStyleSheetDocument(const PrCssStyleSheet* sheet) {
    while (sheet->ownerRule != nullptr && sheet->ownerRule->sheet != nullptr)
        sheet = sheet->ownerRule->sheet;
    return sheet->ownerNode != nullptr ? sheet->ownerNode->document : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

void cssStyleSheetReplace(PrCssStyleSheet* sheet, PrStringView text) {
    cssDocumentStyleChanged(cssStyleSheetDocument(sheet));
    // Shared data is left to the holders that share it, and the sheet parses into data of its own. The objects of its
    // rules continue to present their old data. A change through one of them copies that data first.
    if (shared(sheet)) {
        CssSheetData* data = cssSheetDataCreate();
        data->href = prStringClone(&sheet->data->href);
        cssSheetDataRelease(sheet->data);
        sheet->data = data;
    }
    // The objects of the old rules are removed first, so that each removes its own reference.
    Array<PrCssRule* PR_NULLABLE> taken = sheet->rules;
    sheet->rules = {};
    cssSheetDataParse(sheet->data, text, &sheet->parser, CssOrigin_Author);
    for (uint32_t i = 0; i < sheet->data->rules.count; ++i)
        sheet->rules.push(nullptr);
    for (uint32_t i = 0; i < taken.count; ++i) {
        if (taken.data[i] != nullptr)
            takeOut(taken.data[i]);
    }
    taken.reset();
}

PrStatus cssStyleSheetInsertRule(PrCssStyleSheet* sheet, PrStringView text, uint32_t index) {
    CssEdit edit;
    cssEditBegin(&edit, &sheet->parser);
    PrStatus status = cssRuleListPrepareInsert(&edit, sheet->data, nullptr, text, index);
    if (status == PrStatus_Ok) {
        ownData(sheet, Notify_Caller);
        cssRuleListCommitInsert(&edit, sheet->data, nullptr);
        sheet->rules.insert(index, nullptr);
        cssDocumentRuleChanged(cssStyleSheetDocument(sheet), sheet->data->rules.data[index]);
    }
    cssEditEnd(&edit);
    return status;
}

PrStatus cssRuleInsertRule(PrCssRule* rule, PrStringView text, uint32_t index) {
    CssEdit edit;
    cssEditBegin(&edit, cssRuleParser(rule));
    Array<CssRuleChain> chain;
    PrStatus status =
        cssRuleListPrepareInsert(&edit, rule->sheet != nullptr ? rule->sheet->data : nullptr,
                                 cssRuleChain(rule, &chain), text, index);
    chain.reset();
    if (status == PrStatus_Ok) {
        CssRuleData* parent = ownRule(rule, Notify_Caller);
        cssRuleListCommitInsert(&edit, rule->sheet != nullptr ? rule->sheet->data : nullptr,
                                parent);
        rule->children.insert(index, nullptr);
        cssDocumentRuleChanged(documentOf(rule), parent->children.data[index]);
    }
    cssEditEnd(&edit);
    return status;
}

PrStatus cssStyleSheetDeleteRule(PrCssStyleSheet* sheet, uint32_t index) {
    PrStatus status = cssRuleListRemovable(sheet->data, nullptr, index);
    if (status != PrStatus_Ok)
        return status;
    ownData(sheet, Notify_Caller);
    cssDocumentRuleChanged(cssStyleSheetDocument(sheet), sheet->data->rules.data[index]);
    PrCssRule* object = sheet->rules.data[index];
    sheet->rules.erase(index);
    cssRuleListRemove(sheet->data, nullptr, index);
    if (object != nullptr)
        takeOut(object);
    return PrStatus_Ok;
}

PrStatus cssRuleDeleteRule(PrCssRule* rule, uint32_t index) {
    PrStatus status = cssRuleListRemovable(rule->sheet != nullptr ? rule->sheet->data : nullptr,
                                           rule->data, index);
    if (status != PrStatus_Ok)
        return status;
    CssRuleData* parent = ownRule(rule, Notify_Caller);
    cssDocumentRuleChanged(documentOf(rule), parent->children.data[index]);
    CssSheetData* sheet = rule->sheet != nullptr ? rule->sheet->data : nullptr;
    PrCssRule* object = rule->children.data[index];
    rule->children.erase(index);
    cssRuleListRemove(sheet, rule->data, index);
    if (object != nullptr)
        takeOut(object);
    return PrStatus_Ok;
}

} // namespace pr_impl
