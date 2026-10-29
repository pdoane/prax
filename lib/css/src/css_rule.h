// Parsed Rules
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
// css-nesting-1 - https://drafts.csswg.org/css-nesting-1/
//
// The data a sheet parses to. The objects of css_object.h present it. Data is reference counted, and each piece names
// only what it holds: a sheet names its rules, and a rule names the rules of its block and the sheet it imports.

#pragma once

#include "pr/array.h"
#include "pr/css.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_property.h"
#include "css/src/css_selector.h"
#include "css/src/css_serialize.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssEdit;
struct CssParser;
struct CssRuleData;
struct CssSheetData;

// The place where a change to parsed CSS keeps the parser that it runs on. A slot holds the one parser that a caller
// keeps between changes, which is created on first use. A null slot gives a parse its own parser.
typedef CssParser* PR_NULLABLE* PR_NULLABLE CssParserSlot;

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

// One declaration of a block (cssom-1/#css-declarations), and the run of the block's values that it declares. Its name
// is a property, a descriptor of the block's at-rule, or an author's name. An author's name is a custom property, or
// any name in a block whose at-rule accepts a kCssAtRuleNamedValue.
struct CssDeclaration {
    uint32_t value;               // index of the first value in the block's storage
    uint32_t valueCount;
    uint32_t name;                // offset of an author's name in the declaration's text
    uint32_t nameSize;

    // The property, the descriptor offset past PrCssProperty_Count, or the nameHash of an author's name under
    // kNameSeed.
    uint32_t hash;

    // A longhand, or the shorthand of a shared declaration. PrCssProperty_Count for a descriptor or an author's name.
    PrCssProperty property;

    // The shorthand that the value belongs to until it is computed: a pending-substitution value
    // (css-values-5/#pending-substitution-value) or a system font's value. PrCssProperty_Count for any other value.
    PrCssProperty shorthand;

    PrCssDescriptor descriptor;   // PrCssDescriptor_None for a property or an author's name
    bool important;
};

// The declarations of a rule or of an element's style attribute, in the order they serialize. A shorthand is held as
// its longhands (cssom-1/#css-declaration-blocks). The exception is a shorthand whose longhands all take its single
// value: a CSS-wide keyword, or a value pending on the shorthand. That value is held once, as a shared declaration of
// the shorthand.
//
// Each longhand that the block declares has one declaration. It is the longhand's own declaration if the block holds
// one. Otherwise it is the last important shared declaration of a shorthand that sets it, or the last such declaration
// if none is important. A shared declaration lists the longhands that it declares, in canonical order, at its own
// position (cssDeclarationBlockItem).
//
// A block is packed or mutable. A packed block is one allocation: the values of its declarations, then `declarations`,
// then the text that the values and the authors' names reference. A mutable block holds `declarations` in an array of
// its own, and each declaration in an allocation of its own at its index in `stored`. That allocation holds the
// declaration's values, then the text that the values and the name reference, so a change writes only what it declares.
// A parse writes a packed block, and a change to one declaration makes the block mutable.
struct CssDeclarationBlock {
    CssDeclaration* PR_NULLABLE declarations;

    // Mutable: the allocation that holds each declaration's values, by index. Null if the block is packed.
    CssValueNode* PR_NULLABLE* PR_NULLABLE stored;

    uint32_t count;
    uint32_t length;                  // number of declarations the block lists (cssDeclarationBlockLength)
    uint32_t capacity;                // mutable: capacity of `declarations` and `stored`
    uint32_t valueCount;              // packed: number of values before `declarations`
    uint32_t textSize;                // packed: size of the text after `declarations`
    CssAtRule rule;                   // the at-rule whose descriptors the names are, or CssAtRule_None
};

// Returns the block's declarations.
inline const CssDeclaration* PR_NULLABLE cssDeclarations(const CssDeclarationBlock* block) {
    return block->declarations;
}

// Returns the text that the declaration's values and name reference.
const char* PR_NULLABLE cssDeclarationText(const CssDeclarationBlock* block,
                                           const CssDeclaration* declaration);

// Returns the declaration's name: the name of the property, of the descriptor, or the author's name.
PrStringView cssDeclarationName(const CssDeclarationBlock* block,
                                const CssDeclaration* declaration);

// Returns the run of values that the declaration declares.
CssValueRun cssDeclarationValue(const CssDeclarationBlock* block,
                                const CssDeclaration* declaration);

// A declaration as the block lists it (cssom-1/#dom-cssstyledeclaration-item): the block's declaration, and the
// longhand that it declares. The longhand is the declaration's own property, except for a shared declaration.
struct CssDeclarationItem {
    const CssDeclaration* declaration;
    PrCssProperty property;
};

// Returns the number of declarations that the block lists (cssom-1/#dom-cssstyledeclaration-length).
uint32_t cssDeclarationBlockLength(const CssDeclarationBlock* block);

// Returns the declaration that the block lists at `index`, which must be below the length.
CssDeclarationItem cssDeclarationBlockItem(const CssDeclarationBlock* block, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Media lists

// A media query list (mediaqueries-5/#mq-list). It holds the <media-query> matches, joined by ',' literals.
struct CssMediaQueries {
    CssValueList queries;
};

// Returns the number of media queries in the list.
uint32_t cssMediaListLength(const CssMediaQueries* list);

// Returns the media query at `index`, which must be below the length.
CssValueRun cssMediaQueryAt(const CssMediaQueries* list, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Rules

// The kind of a rule.
enum {
    CssRuleDataKind_Style,              // a style rule (cssom-1/#the-cssstylerule-interface)

    // Declarations after a rule in a block of style rules (css-nesting-1/#nested-declarations-rule).
    CssRuleDataKind_NestedDeclarations,

    CssRuleDataKind_Keyframe,           // a keyframe block of @keyframes (css-animations-1/#keyframes)
    CssRuleDataKind_At,                 // an at-rule
};
typedef uint8_t CssRuleDataKind;

// One rule. Each field serves the kinds named in its comment.
struct CssRuleData {
    uint32_t references;         // one for each list that holds the rule and each object that presents it
    CssRuleDataKind kind;
    CssAtRule atRule;            // At: the at-rule as written, which may be a legacy alias
    bool block;                  // At: written with a block, not a ';'

    // Style: the rule's selectors. At: the selectors that the prelude holds, in order (cssCompilePreludeSelectors).
    PrCssSelectors* PR_NULLABLE selectors;

    // At: the prelude, without its media queries. Keyframe: the keyframe selectors.
    CssValueList prelude;

    CssMediaQueries media;                     // @media and @import: the media queries of the prelude
    CssDeclarationBlock style;                 // the declarations of the block, if it holds any
    Array<CssRuleData* PR_NONNULL> children;   // the rules of the block; the rule holds a reference to each
    CssSheetData* PR_NULLABLE imported;        // @import: the loaded sheet; the rule holds a reference to it
};

// A rule and the rules whose blocks hold it, outward to the sheet. What a rule's block accepts depends on this chain. A
// rule does not name the rules that hold it, so the code that parses into a block builds the chain. A parse builds it
// from the rules it recursed through, and a change through an object builds it from the object's ancestors.
struct CssRuleChain {
    const CssRuleData* rule;
    const CssRuleChain* PR_NULLABLE outer;
};

// Returns the CSSRule type constant of the rule (cssom-1/#the-cssrule-interface), or zero for a rule that has none.
PrCssRuleType cssRuleType(const CssRuleData* rule);

// Returns true if the at-rule is a conditional group rule, whose interface inherits from CSSConditionRule
// (css-conditional-5/#apis).
bool cssAtRuleConditional(CssAtRule atRule);

// Returns the index of the block for feature type `type` among the rules that an @font-feature-values rule holds. That
// block is the map of that type (css-fonts-4/#om-fontfeaturevalues). A rule holds one block of each type.
uint32_t cssFeatureBlock(const CssRuleData* rule, CssAtRule type);

// ---------------------------------------------------------------------------------------------------------------------
// Sheets

// The rules a sheet parses to.
struct CssSheetData {
    uint32_t references;                    // one for each sheet and @import rule that holds this data
    Array<CssRuleData* PR_NONNULL> rules;   // the sheet holds a reference to each rule
    PrString href;                          // the URL the sheet was fetched from, or the null string for given text
};

// ---------------------------------------------------------------------------------------------------------------------
// Lifetime

// Creates a sheet with no rules. The caller holds its one reference.
CssSheetData* cssSheetDataCreate();

// Adds or releases a reference. The last release frees the sheet and releases its rules.
void cssSheetDataRetain(CssSheetData* sheet);
void cssSheetDataRelease(CssSheetData* PR_NULLABLE sheet);

// Adds or releases a reference. The last release frees the rule and releases what it holds.
void cssRuleDataRetain(CssRuleData* rule);
void cssRuleDataRelease(CssRuleData* PR_NULLABLE rule);

// Copies a rule that imports no sheet. The copy has one reference, which the caller holds. The rule's own data is
// copied, and the rules of its block are shared with the copy.
CssRuleData* cssRuleDataCopy(const CssRuleData* rule);

// Copies the sheet, sharing its rules. The copy has one reference, which the caller holds.
CssSheetData* cssSheetDataCopy(const CssSheetData* sheet);

// Returns true if the sheet imports another sheet (css-cascade-5/#at-import).
bool cssSheetDataImports(const CssSheetData* sheet);

// Frees the block's declarations and leaves it empty.
void cssDeclarationBlockReset(CssDeclarationBlock* block);

// ---------------------------------------------------------------------------------------------------------------------
// Edits
//
// A change is prepared against the data it changes, which may be shared. It stages what it writes on the edit
// (css_parser.h). Committing writes the staged change to data the caller owns: the same data, or a copy of it. Only an
// edit prepared as CssEditResult_Changed is committed. Nothing else runs on the edit's parser between the two steps. An
// edit is ended whether or not it was committed.

// The result of preparing a change.
enum {
    CssEditResult_Rejected,    // the data does not accept the change, and nothing is staged
    CssEditResult_Unchanged,   // the data already holds what the change would write
    CssEditResult_Changed,
};
typedef uint8_t CssEditResult;

// Begins an edit on the parser that `slot` keeps, or on the edit's own parser if `slot` is null.
void cssEditBegin(CssEdit* edit, CssParserSlot slot);

// Releases what the edit staged and did not commit.
void cssEditEnd(CssEdit* edit);

// The origin of a declaration (css-cascade-5/#cascading-origins). The members are in the order in which normal
// declarations apply.
enum {
    CssOrigin_UserAgent,
    CssOrigin_Hint,                 // presentational hints, which have zero specificity (css-cascade-5/#preshint)
    CssOrigin_Author,
};
typedef uint8_t CssOrigin;

// ---------------------------------------------------------------------------------------------------------------------
// Parsing
//
// A parse through a sheet runs on the parser that `slot` keeps.

// Replaces the sheet's rules with the ones in `text` (cssom-1/#dom-cssstylesheet-replacesync). `origin` is the origin
// of the sheet's rules. The selectors of a user agent sheet may name the engine's private pseudo-classes
// (CssPseudoClass_FirstPrivate).
void cssSheetDataParse(CssSheetData* sheet, PrStringView text, CssParserSlot slot, CssOrigin origin);

// Prepares setting a style rule's selectors to the list in `text`. The list is relative if the rule is nested in the
// rules that `outer` starts at (cssom-1/#dom-cssstylerule-selectortext, css-nesting-1/#nesting). The selectors use the
// prefixes that the @namespace rules of `sheet` declare. Rejected for text that is not a selector list.
CssEditResult cssStyleRulePrepareSelectorText(CssEdit* edit, const CssRuleChain* PR_NULLABLE outer,
                                              const CssSheetData* PR_NULLABLE sheet,
                                              PrStringView text);

// Prepares setting an at-rule's prelude, or a keyframe rule's selectors, to the contents of `text`
// (cssom-1/#dom-csspagerule-selectortext, css-animations-1/#dom-csskeyframerule-keytext). Rejected for text that is not
// a prelude the rule accepts.
CssEditResult cssRulePreparePrelude(CssEdit* edit, const CssRuleData* rule,
                                    const CssSheetData* PR_NULLABLE sheet, PrStringView text);

// Commits a prepared change to a rule's selectors or prelude.
void cssRuleCommit(CssEdit* edit, CssRuleData* rule);

// Writes the prefix and the namespace that an @namespace rule declares (css-namespaces-3/#syntax). The prefix is the
// null view if the rule declares the default namespace.
void cssNamespaceRuleParts(const CssRuleData* rule, PrStringView* prefix, PrStringView* url);

// Writes the namespace that an @namespace rule of the sheet declares for `prefix`, or the default namespace for the
// null view (css-namespaces-3/#declaration). Returns false if the sheet declares none.
bool cssSheetNamespace(const CssSheetData* sheet, PrStringView prefix, PrStringView* url);

// Returns the index of the last keyframe rule in an @keyframes rule whose selectors are the ones in `select`, or the
// number of its rules if there is none (css-animations-1/#dom-csskeyframesrule-findrule).
uint32_t cssKeyframesFind(const CssRuleData* rule, PrStringView select, CssParserSlot slot);

// Replaces the list's queries with the ones in `text`. Each entry that is not a media query becomes `not all`
// (cssom-1/#dom-medialist-mediatext).
void cssMediaListParse(CssMediaQueries* list, PrStringView text);

// Prepares the change that cssMediaListParse makes.
CssEditResult cssMediaListPrepareText(CssEdit* edit, const CssMediaQueries* list,
                                      PrStringView text);

// Prepares appending the media query in `text`. Unchanged if `text` holds no media query, or holds one that the list
// already has (cssom-1/#dom-medialist-appendmedium).
CssEditResult cssMediaListPrepareAppend(CssEdit* edit, const CssMediaQueries* list,
                                        PrStringView text);

// Prepares removing each query of the list that equals the media query in `text` (cssom-1/#dom-medialist-deletemedium).
// Unchanged if `text` does not hold exactly one query. Rejected if the list holds no matching query.
CssEditResult cssMediaListPrepareDelete(CssEdit* edit, const CssMediaQueries* list,
                                        PrStringView text);

// Commits a prepared change to a media list.
void cssMediaListCommit(CssEdit* edit, CssMediaQueries* list);

// Replaces the block's declarations with the ones in `text` (cssom-1/#parse-a-css-declaration-block).
void cssDeclarationBlockParse(CssDeclarationBlock* block, PrStringView text, CssParserSlot slot);

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

// What a declaration names in a block of a rule's declarations (PrCssDeclarationName), resolved against the rule. A
// legacy name resolves to the name that it is an alias of. An author's name comes with the value that the block gives
// it.
struct CssDeclaredName {
    PrCssProperty property;       // PrCssProperty_Count for a descriptor or an author's name
    PrCssDescriptor descriptor;   // PrCssDescriptor_None for a property or an author's name

    // The value that an author's name accepts in a block of named values. CssGrammarRoot_None otherwise.
    CssGrammarIndex grammar;

    PrStringView text;            // an author's name, or the null view
    uint32_t hash;                // the same hash as CssDeclaration::hash
    bool unicodeRanges;           // true if the value is tokenized with unicode ranges allowed
};

// Resolves what `text` names in a block of the declarations of `rule`. `rule` is CssAtRule_None for a block of
// properties. A descriptor of the rule takes precedence over a property of the same name. A feature value block accepts
// any identifier as a name (css-fonts-4/#font-feature-values-syntax). Returns false, leaving `out` unchanged, if the
// block accepts no declaration of that name.
bool cssNameFromText(CssAtRule rule, PrStringView text, CssDeclaredName* out);

// Resolves what `name` names in a block of the declarations of `rule`. Returns false, leaving `out` unchanged, if the
// block accepts no such declaration.
bool cssDeclaredNameFromName(CssAtRule rule, const PrCssDeclarationName* name, CssDeclaredName* out);

// Returns the name of `property` in a block of properties, or of `descriptor` in a block of its rule's descriptors.
// Neither may be an alias.
CssDeclaredName cssPropertyDeclaredName(PrCssProperty property);
CssDeclaredName cssDescriptorDeclaredName(PrCssDescriptor descriptor);

// Returns the index of the declaration of `name`, or the block's count if there is none. The declaration is the
// longhand's own, or the shared declaration that declares it. A shorthand always gets the count
// (cssom-1/#dom-cssstyledeclaration-getpropertyvalue).
uint32_t cssDeclarationBlockFind(const CssDeclarationBlock* block, const CssDeclaredName& name);

// Returns true if a style rule accepts a declaration of `name` with `value`. That is a known property with a value it
// accepts, or a custom property (css-conditional-3/#dom-css-supports).
bool cssDeclarationSupported(const CssDeclaredName& name, PrStringView value);

// Serializes the value of `name`. A shorthand's value comes from its longhands. Writes nothing if the block holds no
// value for the name (cssom-1/#dom-cssstyledeclaration-getpropertyvalue).
void cssSerializePropertyValue(TextBuffer* out, const CssDeclarationBlock* block,
                               const CssDeclaredName& name);

// Returns true if `name` is important. For a shorthand, every longhand must be important
// (cssom-1/#dom-cssstyledeclaration-getpropertypriority).
bool cssDeclarationBlockImportant(const CssDeclarationBlock* block, const CssDeclaredName& name);

// Prepares declaring `name` (cssom-1/#dom-cssstyledeclaration-setproperty). This sets its declaration, or the
// declaration of each longhand of a shorthand, in place (cssom-1/#set-a-css-declaration). Rejected for a value the
// block does not support, or an importance its rule does not accept. Unchanged if each declaration it sets already has
// that value and importance. An empty value prepares removing the declarations instead.
CssEditResult cssDeclarationBlockPrepareSet(CssEdit* edit, const CssDeclarationBlock* block,
                                            const CssDeclaredName& name, PrStringView value,
                                            bool important);

// Prepares declaring `name` with the value in `run` rather than a written one. The value is matched in the same way as
// its written tokens (cssValueTokens). With `append`, prepares appending `run` to the list that the declaration of
// `name` holds instead. A custom property's value, or a value kept as tokens, is the text that it is written as.
// Rejected in the same cases as cssDeclarationBlockPrepareSet, and also if appending to a declaration that holds no
// list.
CssEditResult cssDeclarationBlockPrepareSetValue(CssEdit* edit, const CssDeclarationBlock* block,
                                                 const CssDeclaredName& name, CssValueRun run,
                                                 bool important, bool append);

// Prepares removing the declaration of `name`, or of each longhand of a shorthand, and writes its value to `out`
// (cssom-1/#dom-cssstyledeclaration-removeproperty). Unchanged if the block holds none.
CssEditResult cssDeclarationBlockPrepareRemove(CssEdit* edit, const CssDeclarationBlock* block,
                                               const CssDeclaredName& name,
                                               PrString* PR_NULLABLE out);

// Commits a prepared change to a block's declarations.
void cssDeclarationBlockCommit(CssEdit* edit, CssDeclarationBlock* block);

// Prepares a change and commits it to the block it was prepared against. For a block the caller owns.
CssEditResult cssDeclarationBlockSet(CssDeclarationBlock* block, const CssDeclaredName& name,
                                     PrStringView value, bool important, CssParserSlot slot);
CssEditResult cssDeclarationBlockSetValue(CssDeclarationBlock* block, const CssDeclaredName& name,
                                          CssValueRun run, bool important, bool append,
                                          CssParserSlot slot);
CssEditResult cssDeclarationBlockRemove(CssDeclarationBlock* block, const CssDeclaredName& name,
                                        PrString* PR_NULLABLE out);

// Adds the value of `name` to `out`. Adds nothing if the block holds no value, or holds a value that is pending on the
// substitution of its shorthand. A shorthand's value is the serialization of its longhands, matched as a value of the
// shorthand. A value pending on the shorthand is added as its tokens.
void cssDeclarationBlockValue(const CssDeclarationBlock* block, const CssDeclaredName& name,
                              CssValuePool* out, CssParserSlot slot);

// Adds the value that `text` holds for `name` to `out`. Returns false if `text` holds none. A shorthand's value is the
// one that a block declaring it would hold (cssDeclarationBlockValue).
bool cssParseDeclaredValue(const CssDeclaredName& name, PrStringView text, CssValuePool* out,
                           CssParserSlot slot);

// ---------------------------------------------------------------------------------------------------------------------
// Rule lists

// Prepares inserting the rule in `text` into the block of the rule that `parent` starts at, or into the rules of
// `sheet` if `parent` is null, at `index` (cssom-1/#insert-a-css-rule). The rule must be valid at that position and
// with the prefixes that the @namespace rules of `sheet` declare. `sheet` may be null for a rule taken out of its
// sheet. PrStatus_Ok means a change is staged, and any other status stages nothing.
PrStatus cssRuleListPrepareInsert(CssEdit* edit, const CssSheetData* PR_NULLABLE sheet,
                                  const CssRuleChain* PR_NULLABLE parent, PrStringView text,
                                  uint32_t index);

// Commits a prepared insertion into the block of `parent`, or into the rules of `sheet` if `parent` is null.
void cssRuleListCommitInsert(CssEdit* edit, CssSheetData* PR_NULLABLE sheet,
                             CssRuleData* PR_NULLABLE parent);

// Prepares an insertion and commits it to the list it was prepared against. For a list the caller owns: the block of
// `parent`, in the rules that `outer` starts at.
PrStatus cssRuleListInsert(CssSheetData* PR_NULLABLE sheet, CssRuleData* PR_NULLABLE parent,
                           const CssRuleChain* PR_NULLABLE outer, PrStringView text, uint32_t index,
                           CssParserSlot slot);

// Checks whether the rule at `index` of the block of `parent`, or of the rules of `sheet` if `parent` is null, may be
// removed (cssom-1/#remove-a-css-rule). Returns OutOfRange past the end, and InvalidState for an @namespace rule while
// the list holds other rules.
PrStatus cssRuleListRemovable(const CssSheetData* PR_NULLABLE sheet,
                              const CssRuleData* PR_NULLABLE parent, uint32_t index);

// Takes the rule at `index` out of the block of `parent`, or out of the rules of `sheet` if `parent` is null, and
// releases the list's reference to it.
void cssRuleListRemove(CssSheetData* PR_NULLABLE sheet, CssRuleData* PR_NULLABLE parent,
                       uint32_t index);

// Puts `rule` at `index` of a rule list, in place of the rule there. The list takes over the caller's reference to
// `rule` and releases its reference to the old rule.
void cssRuleListReplace(Array<CssRuleData* PR_NONNULL>* list, uint32_t index, CssRuleData* rule);

// ---------------------------------------------------------------------------------------------------------------------
// Serialization

// Serializes the declarations joined with a space, each ending in a semicolon. Longhands are written as a shorthand
// where possible (cssom-1/#serialize-a-css-declaration-block).
void cssSerializeDeclarationBlock(TextBuffer* out, const CssDeclarationBlock* block);

// Serializes a declaration's value (cssom-1/#serialize-a-css-value). A value pending on a shorthand serializes as
// nothing.
void cssSerializeDeclarationValue(TextBuffer* out, const CssDeclarationBlock* block,
                                  const CssDeclaration* declaration);

// Serializes a style rule's selectors (cssom-1/#dom-cssstylerule-selectortext).
void cssSerializeSelectorText(TextBuffer* out, const CssRuleData* rule);

// Serializes an at-rule's prelude, with its media queries after the rest, or a keyframe rule's selectors
// (css-animations-1/#dom-csskeyframerule-keytext).
void cssSerializePrelude(TextBuffer* out, const CssRuleData* rule);

// Serializes the queries joined with ", " (cssom-1/#serialize-a-media-query-list).
void cssSerializeMediaList(TextBuffer* out, const CssMediaQueries* list);

// Serializes one rule (cssom-1/#serialize-a-css-rule).
void cssSerializeRule(TextBuffer* out, const CssRuleData* rule);

// Serializes the rules joined with a newline (cssom-1/#serialize-a-css-rule).
void cssSerializeRuleList(TextBuffer* out, const Array<CssRuleData* PR_NONNULL>& rules);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
