// CSSOM Objects
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// The objects of the CSS object model, which present parsed rules (css_rule.h). An object is created on first access,
// holds a reference to the data that it presents, and lives as long as its tree. A sheet owns the objects of its rules.
// A rule owns the objects of the rules in its block and of the sheet it imports. An element owns its sheet, and counts
// the sheet's references as its own.

#pragma once

#include "pr/css.h"
#include "pr/dom.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

#include "css/src/css_object_member.h"
#include "css/src/css_rule.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssFontFace;
struct CssRoot;
struct CssRule;
struct CssStyleSheet;

// ---------------------------------------------------------------------------------------------------------------------
// Objects

// A rule (cssom-1/#the-cssrule-interface). Its parent rule or its sheet owns it. A rule removed from its list is the
// root of its own tree.
struct CssRule {
    CssRuleData* data;                    // the data that the rule presents; the rule holds a reference to it

    // The parent sheet (cssom-1/#concept-css-rule-parent-css-style-sheet). Null once the rule or an enclosing rule is
    // removed from its list.
    PrCssStyleSheet* PR_NULLABLE sheet;

    PrCssRule* PR_NULLABLE parent;          // cssom-1/#concept-css-rule-parent-css-rule
    uint32_t references;
    uint32_t retained;                         // references to the rule's tree, if the rule is a root
    RawArray<PrCssRule* PR_NULLABLE> children; // objects of the rules in the block, by index, created on first access
    PrCssStyleSheet* PR_NULLABLE imported;     // @import: the object of the imported sheet, created on first access

    // @font-face: the rule's CSS-connected FontFace, created on first access. The rule holds a reference to it.
    PrCssFontFace* PR_NULLABLE face;

    PrCssRuleList cssRules;
    PrCssStyleDeclaration style;
    PrCssMediaList media;
};

// A CSS style sheet (cssom-1/#css-style-sheet). An element owns its sheet, and an @import rule owns the sheet it
// imports. Any other sheet is the root of its own tree.
struct CssStyleSheet {
    CssSheetData* data;                     // the data that the sheet presents; the sheet holds a reference to it
    RawArray<PrCssRule* PR_NULLABLE> rules; // objects of the sheet's rules, by index, created on first access
    PrDomNode* PR_NULLABLE ownerNode;
    PrCssRule* PR_NULLABLE ownerRule;
    PrRawString href;
    PrRawString title;

    // The sheet's media (cssom-1/#concept-css-style-sheet-media). An imported sheet uses its rule's media instead.
    CssMediaQueries media;

    // The parser that every parse through the sheet uses. It is created on first use, and kept between calls while it
    // is small.
    CssParser* PR_NULLABLE parser;

    uint32_t references;
    uint32_t retained;                    // references to the sheet's tree, if the sheet is a root
    bool disabled;
    bool constructed;                     // true if built by a caller rather than parsed from a document

    // The constructor document of a constructed sheet (cssom-1/#concept-css-style-sheet-constructor-document), or null.
    // The document lists the sheet among its constructed sheets, which hold no reference, and clears this as it goes.
    PrDomDocument* PR_NULLABLE constructorDocument;
    uint32_t adoptions;                   // the places in adopted sheet lists that hold the sheet
    PrCssRuleList cssRules;
    PrCssMediaList mediaList;
};

// A live, ordered view of the sheets of a document or of a shadow root. The sheets' owner nodes own them.
struct CssStyleSheetList {
    RawArray<PrCssStyleSheet* PR_NONNULL> sheets;
};

// ---------------------------------------------------------------------------------------------------------------------
// Lifetime

// Creates a sheet that presents `data`, and takes over the caller's reference to `data`. The sheet has no references,
// and belongs to its creator until an owner takes it or a reference is taken.
PrCssStyleSheet* cssStyleSheetCreate(CssSheetData* data);

// Adds or releases a reference to a sheet or a rule. A reference keeps the object's tree alive.
void cssStyleSheetRetain(PrCssStyleSheet* sheet);
void cssStyleSheetRelease(PrCssStyleSheet* PR_NULLABLE sheet);
void cssRuleRetain(PrCssRule* rule);
void cssRuleRelease(PrCssRule* PR_NULLABLE rule);

// Detaches the sheet from its owner node (cssom-1/#remove-a-css-style-sheet). The sheet becomes the root of its own
// tree, and takes with it the references that the owner node counted. It is freed if there are none.
void cssStyleSheetLeaveOwner(PrCssStyleSheet* sheet);

// Frees a sheet that nothing references, while its owner node is being freed.
void cssStyleSheetFreeWithOwner(PrCssStyleSheet* PR_NULLABLE sheet);

// Returns the number of references to the objects of the sheet's tree. The owner node counts these references as its
// own.
uint32_t cssStyleSheetReferences(const PrCssStyleSheet* sheet);

// ---------------------------------------------------------------------------------------------------------------------
// The objects of rules

// Returns the kind of the most specific interface of the object that presents `rule` (prCssRuleKind).
PrCssRuleKind cssRuleKind(const CssRuleData* rule);

// Returns the rule at `index` of the sheet, or of the rule's block. `index` must be less than the number of rules.
PrCssRule* cssStyleSheetRule(PrCssStyleSheet* sheet, uint32_t index);
PrCssRule* cssRuleChild(PrCssRule* rule, uint32_t index);

// cssRuleListData returns the data of the rules in the list's holder. cssRuleListItem returns the object of the rule at
// `index`.
const RawArray<CssRuleData* PR_NONNULL>& cssRuleListData(const PrCssRuleList* list);
PrCssRule* cssRuleListItem(PrCssRuleList* list, uint32_t index);

// Returns the sheet that an @import rule imports, or null if it loaded none.
PrCssStyleSheet* PR_NULLABLE cssImportRuleSheet(PrCssRule* rule);

// Returns the parser slot that a change to `rule` uses: the slot of its sheet, or none for a rule removed from its
// sheet.
CssParserSlot cssRuleParser(PrCssRule* rule);

// Links in `out` the data that `rule` and the objects of its enclosing rules present, one link per level from `rule`
// outward. Returns the first link, or null if `rule` is null.
const CssRuleChain* PR_NULLABLE cssRuleChain(const PrCssRule* PR_NULLABLE rule, RawArray<CssRuleChain>* out);

// Returns the declarations of a declaration block.
const CssDeclarationBlock* cssStyleDeclarationBlock(const PrCssStyleDeclaration* declaration);

// cssStyleDeclarationAtRule returns the at-rule whose descriptors are the names in a declaration block, or
// CssAtRule_None if the names are properties. The at-rule does not change for the declaration's life, so reading it
// brings no values up to date, unlike cssStyleDeclarationBlock. cssStyleDeclarationParser returns the parser slot that
// a change to the declarations uses.
CssAtRule cssStyleDeclarationAtRule(const PrCssStyleDeclaration* declaration);
CssParserSlot cssStyleDeclarationParser(PrCssStyleDeclaration* declaration);

// Returns the data that a change through `rule` writes. If anything else holds the data, this first copies the data of
// the rules on the path to it, including the sheet's list of rules. The copies share everything else with the
// originals, and each object on the path then presents its rule's copy. A change is prepared against the data that the
// object presents, and calls this function only to commit a change that modifies the data (css_rule.h, "Edits").
CssRuleData* cssRuleWritable(PrCssRule* rule);

// Returns the data that a change through `rule` writes, made writable in the same way as cssRuleWritable. The data of
// the rules nested in its block is also copied if anything else holds it. A change to a style rule's selectors writes
// those rules too, because they use the selectors' specificity.
CssRuleData* cssRuleWritableTree(PrCssRule* rule);

// Returns the declarations or the queries that a change through the object writes. They are made writable in the same
// way that cssRuleWritable makes the data of the rule that holds them writable.
CssDeclarationBlock* cssStyleDeclarationWritable(PrCssStyleDeclaration* declaration);
CssMediaQueries* cssMediaListWritable(PrCssMediaList* list);

// Sets the owner node's style attribute to the block's serialization (cssom-1/#update-style-attribute-for). Does
// nothing for a block with no owner node.
void cssStyleDeclarationUpdateOwner(PrCssStyleDeclaration* declaration);

// Returns the queries of a media list.
const CssMediaQueries* cssMediaListQueries(const PrCssMediaList* list);

// Returns the document whose elements the sheet styles: the document of its owner node, or of the sheet that imports
// it. Returns null for a sheet that no node owns.
const PrDomDocument* PR_NULLABLE cssStyleSheetDocument(const PrCssStyleSheet* sheet);

// Replaces the adopted sheets of `root`, a document's or a shadow root's whose node document is `document`, with
// `sheets` (cssom-1/#dom-documentorshadowroot-adoptedstylesheets). Returns NotAllowed, and changes nothing, if a sheet
// is not constructed or its constructor document is not `document`.
PrStatus cssRootSetAdopted(CssRoot* root, PrDomDocument* document, PrCssStyleSheet* const PR_NONNULL* PR_NULLABLE sheets,
    uint32_t count);

// Lets go of the root's adopted sheets, and of a document's constructed sheets, as the root is destroyed.
void cssRootResetSheets(CssRoot* root);

// ---------------------------------------------------------------------------------------------------------------------
// Changes

// Replaces the sheet's rules with the ones in `text` (cssom-1/#dom-cssstylesheet-replacesync). The objects of the
// previous rules are removed from the sheet.
void cssStyleSheetReplace(PrCssStyleSheet* sheet, PrStringView text);

// Inserts the rule that `text` holds at `index` of the sheet, or of the rule's block (cssom-1/#insert-a-css-rule).
// Fails in the same cases as cssRuleListPrepareInsert.
PrStatus cssStyleSheetInsertRule(PrCssStyleSheet* sheet, PrStringView text, uint32_t index);
PrStatus cssRuleInsertRule(PrCssRule* rule, PrStringView text, uint32_t index);

// Removes the rule at `index` from the sheet, or from the rule's block (cssom-1/#remove-a-css-rule). Fails in the same
// cases as cssRuleListRemovable.
PrStatus cssStyleSheetDeleteRule(PrCssStyleSheet* sheet, uint32_t index);
PrStatus cssRuleDeleteRule(PrCssRule* rule, uint32_t index);

} // namespace pr_impl

// C opaque types. A rule's interfaces are the ones its kind names.
struct PrCssRule : pr_impl::CssRule {};
struct PrCssGroupingRule : PrCssRule {};
struct PrCssConditionRule : PrCssGroupingRule {};
struct PrCssMediaRule : PrCssConditionRule {};
struct PrCssSupportsRule : PrCssConditionRule {};
struct PrCssContainerRule : PrCssConditionRule {};
struct PrCssStyleRule : PrCssGroupingRule {};
struct PrCssPageRule : PrCssGroupingRule {};
struct PrCssScopeRule : PrCssGroupingRule {};
struct PrCssStartingStyleRule : PrCssGroupingRule {};
struct PrCssLayerBlockRule : PrCssGroupingRule {};
struct PrCssFunctionRule : PrCssGroupingRule {};
struct PrCssMixinRule : PrCssGroupingRule {};
struct PrCssApplyBlockRule : PrCssGroupingRule {};
struct PrCssContentsBlockRule : PrCssGroupingRule {};
struct PrCssSupportsConditionRule : PrCssGroupingRule {};
struct PrCssApplyStatementRule : PrCssRule {};
struct PrCssContentsStatementRule : PrCssRule {};
struct PrCssCounterStyleRule : PrCssRule {};
struct PrCssCustomMediaRule : PrCssRule {};
struct PrCssFontFaceRule : PrCssRule {};
struct PrCssFontFeatureValuesRule : PrCssRule {};
struct PrCssFontPaletteValuesRule : PrCssRule {};
struct PrCssFunctionDeclarations : PrCssRule {};
struct PrCssImportRule : PrCssRule {};
struct PrCssKeyframeRule : PrCssRule {};
struct PrCssKeyframesRule : PrCssRule {};
struct PrCssLayerStatementRule : PrCssRule {};
struct PrCssMarginRule : PrCssRule {};
struct PrCssNamespaceRule : PrCssRule {};
struct PrCssNestedDeclarations : PrCssRule {};
struct PrCssPositionTryRule : PrCssRule {};
struct PrCssPropertyRule : PrCssRule {};
struct PrCssViewTransitionRule : PrCssRule {};
struct PrCssFontFeatureValuesMap : PrCssRule {};
struct PrCssStyleSheet : pr_impl::CssStyleSheet {};
struct PrCssStyleSheetList : pr_impl::CssStyleSheetList {};

PR_ASSUME_NONNULL_END
