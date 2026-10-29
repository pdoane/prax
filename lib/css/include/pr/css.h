// CSS Object Model
//
// cssom-1 - https://drafts.csswg.org/cssom/
// css-typed-om-1 - https://drafts.css-houdini.org/css-typed-om-1/
// css-values-4 - https://drafts.csswg.org/css-values-4/
//
// Each web interface has a section of its own, which holds its members. A value that a declaration holds is a sequence
// of node trees (PrCssValue) rather than a graph of objects.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/css_descriptors.h"
#include "pr/css_keywords.h"
#include "pr/css_properties.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"
#include "pr/font_set.h"
#include "pr/layout_style.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The objects of the model, each named for the interface that it presents. A rule of any interface is also a PrCssRule,
// and a declaration block of any interface is also a PrCssStyleDeclaration.
typedef struct PrCssMediaList PrCssMediaList;
typedef struct PrCssMediaQueryList PrCssMediaQueryList;
typedef struct PrCssStyleSheet PrCssStyleSheet;
typedef struct PrCssRule PrCssRule;
typedef struct PrCssGroupingRule PrCssGroupingRule;
typedef struct PrCssStyleRule PrCssStyleRule;
typedef struct PrCssImportRule PrCssImportRule;
typedef struct PrCssNamespaceRule PrCssNamespaceRule;
typedef struct PrCssConditionRule PrCssConditionRule;
typedef struct PrCssMediaRule PrCssMediaRule;
typedef struct PrCssSupportsRule PrCssSupportsRule;
typedef struct PrCssContainerRule PrCssContainerRule;
typedef struct PrCssNestedDeclarations PrCssNestedDeclarations;
typedef struct PrCssFontFaceRule PrCssFontFaceRule;
typedef struct PrCssPageRule PrCssPageRule;
typedef struct PrCssMarginRule PrCssMarginRule;
typedef struct PrCssKeyframesRule PrCssKeyframesRule;
typedef struct PrCssKeyframeRule PrCssKeyframeRule;
typedef struct PrCssLayerBlockRule PrCssLayerBlockRule;
typedef struct PrCssLayerStatementRule PrCssLayerStatementRule;
typedef struct PrCssScopeRule PrCssScopeRule;
typedef struct PrCssStartingStyleRule PrCssStartingStyleRule;
typedef struct PrCssCounterStyleRule PrCssCounterStyleRule;
typedef struct PrCssPropertyRule PrCssPropertyRule;
typedef struct PrCssFontFeatureValuesRule PrCssFontFeatureValuesRule;
typedef struct PrCssFontFeatureValuesMap PrCssFontFeatureValuesMap;
typedef struct PrCssFontPaletteValuesRule PrCssFontPaletteValuesRule;
typedef struct PrCssPositionTryRule PrCssPositionTryRule;
typedef struct PrCssViewTransitionRule PrCssViewTransitionRule;
typedef struct PrCssFunctionRule PrCssFunctionRule;
typedef struct PrCssFunctionDeclarations PrCssFunctionDeclarations;
typedef struct PrCssCustomMediaRule PrCssCustomMediaRule;
typedef struct PrCssSupportsConditionRule PrCssSupportsConditionRule;
typedef struct PrCssMixinRule PrCssMixinRule;
typedef struct PrCssApplyBlockRule PrCssApplyBlockRule;
typedef struct PrCssApplyStatementRule PrCssApplyStatementRule;
typedef struct PrCssContentsBlockRule PrCssContentsBlockRule;
typedef struct PrCssContentsStatementRule PrCssContentsStatementRule;
typedef struct PrCssRuleList PrCssRuleList;
typedef struct PrCssStyleSheetList PrCssStyleSheetList;
typedef struct PrCssStyleDeclaration PrCssStyleDeclaration;
typedef struct PrCssStyleProperties PrCssStyleProperties;
typedef struct PrCssFontFaceDescriptors PrCssFontFaceDescriptors;
typedef struct PrCssPageDescriptors PrCssPageDescriptors;
typedef struct PrCssPositionTryDescriptors PrCssPositionTryDescriptors;
typedef struct PrCssFunctionDescriptors PrCssFunctionDescriptors;

// ---------------------------------------------------------------------------------------------------------------------
// Values (css-values-4)

// The unit of a numeric value: the unit of a number, of a percentage, and each unit of a dimension
// (css-typed-om-1/#dom-cssunitvalue-unit).
enum {
    PrCssUnit_Number,       // no unit: the unit of a number, or of a zero length
    PrCssUnit_Percent,      // the unit of a percentage

    // Font-relative Lengths (css-values-4/#font-relative-lengths)
    PrCssUnit_Em,           // font size of the element
    PrCssUnit_Rem,          // font size of the root element
    PrCssUnit_Ex,           // x-height of the element's font
    PrCssUnit_Rex,          // x-height of the root element's font
    PrCssUnit_Cap,          // cap height of the element's font
    PrCssUnit_Rcap,         // cap height of the root element's font
    PrCssUnit_Ch,           // typical character advance of a narrow glyph in the element's font
    PrCssUnit_Rch,          // typical character advance of a narrow glyph in the root element's font
    PrCssUnit_Ic,           // typical character advance of a fullwidth glyph in the element's font
    PrCssUnit_Ric,          // typical character advance of a fullwidth glyph in the root element's font
    PrCssUnit_Lh,           // line height of the element
    PrCssUnit_Rlh,          // line height of the root element

    // Viewport-percentage Lengths (css-values-4/#viewport-relative-lengths)
    PrCssUnit_Vw,           // 1% of the default viewport's width
    PrCssUnit_Svw,          // 1% of the small viewport's width
    PrCssUnit_Lvw,          // 1% of the large viewport's width
    PrCssUnit_Dvw,          // 1% of the dynamic viewport's width

    PrCssUnit_Vh,           // 1% of the default viewport's height
    PrCssUnit_Svh,          // 1% of the small viewport's height
    PrCssUnit_Lvh,          // 1% of the large viewport's height
    PrCssUnit_Dvh,          // 1% of the dynamic viewport's height

    PrCssUnit_Vi,           // 1% of the default viewport's size in the element's inline axis
    PrCssUnit_Svi,          // 1% of the small viewport's size in the element's inline axis
    PrCssUnit_Lvi,          // 1% of the large viewport's size in the element's inline axis
    PrCssUnit_Dvi,          // 1% of the dynamic viewport's size in the element's inline axis

    PrCssUnit_Vb,           // 1% of the default viewport's size in the element's block axis
    PrCssUnit_Svb,          // 1% of the small viewport's size in the element's block axis
    PrCssUnit_Lvb,          // 1% of the large viewport's size in the element's block axis
    PrCssUnit_Dvb,          // 1% of the dynamic viewport's size in the element's block axis

    PrCssUnit_Vmin,         // 1% of the default viewport's smaller dimension
    PrCssUnit_Svmin,        // 1% of the small viewport's smaller dimension
    PrCssUnit_Lvmin,        // 1% of the large viewport's smaller dimension
    PrCssUnit_Dvmin,        // 1% of the dynamic viewport's smaller dimension

    PrCssUnit_Vmax,         // 1% of the default viewport's larger dimension
    PrCssUnit_Svmax,        // 1% of the small viewport's larger dimension
    PrCssUnit_Lvmax,        // 1% of the large viewport's larger dimension
    PrCssUnit_Dvmax,        // 1% of the dynamic viewport's larger dimension

    // Container Relative Lengths (css-conditional-5/#container-lengths)
    PrCssUnit_Cqw,          // 1% of the query container's width
    PrCssUnit_Cqh,          // 1% of the query container's height
    PrCssUnit_Cqi,          // 1% of the query container's inline size
    PrCssUnit_Cqb,          // 1% of the query container's block size
    PrCssUnit_Cqmin,        // 1% of the query container's smaller dimension
    PrCssUnit_Cqmax,        // 1% of the query container's larger dimension

    // Absolute Lengths (css-values-4/#absolute-lengths)
    PrCssUnit_Cm,           // centimeters. 1cm = 96px/2.54
    PrCssUnit_Mm,           // millimeters. 1mm = 1/10th of 1cm
    PrCssUnit_Q,            // quarter-millimeters. 1Q = 1/40th of 1cm
    PrCssUnit_In,           // inches. 1in = 2.54cm = 96px
    PrCssUnit_Pc,           // picas. 1pc = 1/6th of 1in
    PrCssUnit_Pt,           // points. 1pt = 1/72nd of 1in
    PrCssUnit_Px,           // pixels. 1px = 1/96th of 1in

    // Angle Units (css-values-4/#angles)
    PrCssUnit_Deg,          // degrees. 360 degrees in a circle
    PrCssUnit_Grad,         // gradians. 400 gradians in a circle
    PrCssUnit_Rad,          // radians. 2π radians in a circle
    PrCssUnit_Turn,         // turns. 1 turn in a circle

    // Duration Units (css-values-4/#time)
    PrCssUnit_S,            // seconds
    PrCssUnit_Ms,           // milliseconds. 1000 milliseconds in a second

    // Frequency Units (css-values-4/#frequency)
    PrCssUnit_Hz,           // hertz, occurrences per second
    PrCssUnit_KHz,          // kilohertz. 1kHz = 1000Hz

    // Resolution Units (css-values-4/#resolution)
    PrCssUnit_Dpi,          // dots per inch
    PrCssUnit_Dpcm,         // dots per centimeter
    PrCssUnit_Dppx,         // dots per px unit
    PrCssUnit_X,            // dots per px unit, the same as dppx

    // Flexible Lengths (css-grid-1/#fr-unit)
    PrCssUnit_Fr,           // fraction of the leftover space in the grid container

    // Voice Characteristics (css-speech-1/#mixing-props-voice-volume, css-speech-1/#voice-props-voice-pitch)
    PrCssUnit_DB,           // decibels, relative to the inherited volume
    PrCssUnit_St,           // semitones, relative to the inherited pitch

    PrCssUnit_Unknown,      // a unit of a dimension that no table names, kept as written
    PrCssUnit_Count,
};
typedef uint8_t PrCssUnit;

// The kind of a node, which decides the member of PrCssValueFields that it uses and the children that it takes. A node
// takes no children unless its kind says otherwise.
enum {
    PrCssValueKind_Keyword,        // `keyword`
    PrCssValueKind_Ident,          // `text`: an identifier that is not a keyword of the grammar
    PrCssValueKind_String,         // `text`
    PrCssValueKind_Url,            // `text`
    PrCssValueKind_Integer,        // `number`, in PrCssUnit_Number
    PrCssValueKind_Number,         // `number`, in PrCssUnit_Number
    PrCssValueKind_Percentage,     // `number`, in PrCssUnit_Percent

    // `number`. A unit that no table names is PrCssUnit_Unknown, and the node's child is an Ident of the unit's text.
    PrCssValueKind_Dimension,

    // `color`. The children are the channels in the order that the color's space writes them, then the alpha. Each is a
    // numeric value, a Keyword or a Math (prCssValueColorChannels).
    PrCssValueKind_Color,

    // `keyword`, the function's name. The children are the arguments, with the ',' Literals between them.
    PrCssValueKind_Function,

    // A math function in place of a numeric value. The child is the root of the calculation tree
    // (css-values-4/#calculation-tree): a numeric value, a Calc, or a math Function over calculations.
    PrCssValueKind_Math,

    // `calc`, an operator in a Math's tree. The children are its operands: one for Negate or Invert, and one or more
    // otherwise.
    PrCssValueKind_Calc,

    PrCssValueKind_UnicodeRange,   // `range`

    // `text`, the tokens as written. It holds the value of a custom property, or a value that contains an arbitrary
    // substitution function.
    PrCssValueKind_Tokens,

    PrCssValueKind_Literal,        // `literal`, a character that the grammar writes

    // A component of the value of a shorthand. `longhand` is the property that the component sets, and the child is the
    // component.
    PrCssValueKind_Longhand,

    // A match of a structural production, such as a selector, an at-rule prelude or a query. Its children are what the
    // production matched. The value of a property holds no such node.
    PrCssValueKind_Production,

    PrCssValueKind_Count,
};
typedef uint8_t PrCssValueKind;

// The operators of a calculation tree (css-values-4/#calculation-tree-calc-operator-nodes).
enum {
    PrCssCalc_Sum,
    PrCssCalc_Product,
    PrCssCalc_Negate,
    PrCssCalc_Invert,
};
typedef uint8_t PrCssCalc;

// A sequence of trees, such as the value that a declaration holds: the components of `1px solid red`, or the items of a
// list and the ',' literals between them. The nodes are numbered in preorder from zero, so a node's children follow it.
// prCssValueNext returns the node after a subtree, which is its next sibling or the next tree.
//
// A value is built one node at a time. A node is opened, its children are added, and it is closed, which checks that
// the children are what its kind accepts. A value with no node open consists of whole trees of valid shape. A function
// that reads a value requires that no node is open, and that each node it is given is in the value. A call that breaks
// either is fatal. A function that writes a value appends trees to one with no node open, and leaves it unchanged if it
// fails.
typedef struct PrCssValue PrCssValue;

// The maximum depth of a node. A tree's root is at depth one.
enum { kPrCssValueMaxDepth = 1024 };

// The data that a node holds besides its children. The kind decides which member of the union it uses. A string is a
// view. A string passed in is copied, and a string read out belongs to the value and is valid until the value changes.
typedef struct PrCssValueFields {
    PrCssValueKind kind;
    union {
        PrCssKeyword keyword;      // Keyword, and Function: the function's name
        PrStringView text;         // Ident, String, Url, Tokens
        struct {
            double value;
            PrCssUnit unit;
        } number;                  // Integer, Number, Percentage, Dimension
        struct {
            PrColorSpace space;
            bool legacy;           // sRGB written as rgb() or a hex color rather than color()
        } color;                   // Color
        PrCssCalc calc;            // Calc
        uint32_t literal;          // Literal: the character, which is ASCII
        PrCssProperty longhand;    // Longhand
        struct {
            uint32_t first;
            uint32_t last;
        } range;                   // UnicodeRange: the code points
    };
} PrCssValueFields;

// The meaning of a color channel: the values that a number and a percentage written for it represent, in the units of
// its space.
typedef struct PrCssColorChannel {
    uint32_t node;             // the channel as written
    double number;             // value that a number of 1 represents
    double percent;            // value that a percentage of 1 represents
    bool hue;                  // a hue, in degrees
} PrCssColorChannel;

// The base types of the type of a numeric value (css-typed-om-1/#cssnumericvalue-base-type).
enum {
    PrCssNumericBaseType_Length,
    PrCssNumericBaseType_Angle,
    PrCssNumericBaseType_Time,
    PrCssNumericBaseType_Frequency,
    PrCssNumericBaseType_Resolution,
    PrCssNumericBaseType_Flex,
    PrCssNumericBaseType_Percent,

    PrCssNumericBaseType_Count,
};
typedef uint8_t PrCssNumericBaseType;

// The type of a numeric value (css-typed-om-1/#dictdef-cssnumerictype).
typedef struct PrCssNumericType {
    int32_t exponents[PrCssNumericBaseType_Count];   // power of each base type, zero for a base type it lacks
    PrCssNumericBaseType percentHint;
    bool hasPercentHint;
} PrCssNumericType;

// A var() among tokens (css-variables-1/#using-variables). The offsets are byte offsets into the text of the tokens.
typedef struct PrCssVariableReference {
    uint32_t start;            // `var(`
    uint32_t end;              // one past its `)`, or the end of the text if it is not closed
    PrStringView name;         // the custom property's name as written
    bool hasFallback;
    uint32_t fallbackStart;    // past the ',' before the fallback
    uint32_t fallbackEnd;      // the `)`, or the end of the text
} PrCssVariableReference;

// The name that a declaration uses (css-syntax-3/#declaration). Exactly one member gives it: a property, a descriptor
// of the block's rule, or an author-defined name. An author-defined name is a custom property, or an entry of a block
// of named values. An author-defined name from a caller need not be copied (prStringExternal). A name that a function
// writes is owned, and prCssDeclarationNameReset frees it.
typedef struct PrCssDeclarationName {
    PrCssProperty property;       // PrCssProperty_Count if it names no property
    PrCssDescriptor descriptor;   // PrCssDescriptor_None if it names no descriptor
    PrString author;              // the null string if it names a property or a descriptor
} PrCssDeclarationName;

// Returns the name of `property`.
static inline PrCssDeclarationName prCssDeclarationNameFromProperty(PrCssProperty property) {
    PrCssDeclarationName value;
    value.property = property;
    value.descriptor = PrCssDescriptor_None;
    value.author = prStringNull();
    return value;
}

// ---------------------------------------------------------------------------------------------------------------------
// Media types (mediaqueries-5/#media-types)

// The media types that a document can be presented for. The other types match nothing.
enum {
    PrCssMediaType_Screen,
    PrCssMediaType_Print,
};
typedef uint8_t PrCssMediaType;

// ---------------------------------------------------------------------------------------------------------------------
// MediaList (cssom)

// Get and set the list's queries, serialized and joined with ", ". Setting the text replaces the queries with the ones
// in `value`. An entry of `value` that is not a media query becomes `not all`.
PrString prCssMediaListMediaText(const PrCssMediaList* self);
void prCssMediaListSetMediaText(PrCssMediaList* self, PrStringView value);

// Return the number of queries, and the query at `index`, serialized. The query is the null string past the last query.
uint32_t prCssMediaListLength(const PrCssMediaList* self);
PrString prCssMediaListQueryAt(const PrCssMediaList* self, uint32_t index);

// Appends the media query in `medium`. Does nothing if `medium` holds none, or holds one that the list already has.
void prCssMediaListAppendMedium(PrCssMediaList* self, PrStringView medium);

// Removes each query equal to the media query in `medium`. Does nothing if `medium` does not hold exactly one query.
// Returns NotFound if the list holds no equal query.
PrStatus prCssMediaListDeleteMedium(PrCssMediaList* self, PrStringView medium);

// ---------------------------------------------------------------------------------------------------------------------
// MediaQueryList : EventTarget (cssom-view)
//
// The query list that a window's matchMedia creates.

// Converts a MediaQueryList to the interface that it inherits from.
PR_HANDLE_UPCAST(CssMediaQueryList, DomEventTarget, EventTarget)

// Returns an EventTarget as a MediaQueryList, or null if it is not one.
PrCssMediaQueryList* PR_NULLABLE prCssMediaQueryListQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the list's queries, serialized.
PrString prCssMediaQueryListMedia(const PrCssMediaQueryList* self);

// Returns true if the queries match the window's environment at the time of the call.
bool prCssMediaQueryListMatches(const PrCssMediaQueryList* self);

// Add or remove `callback` as a listener for `change` events, in the same way as the event target's functions. A null
// `callback` does nothing.
void prCssMediaQueryListAddListener(PrCssMediaQueryList* self,
                                    const PrDomEventListener* PR_NULLABLE callback);
void prCssMediaQueryListRemoveListener(PrCssMediaQueryList* self,
                                       const PrDomEventListener* PR_NULLABLE callback);

// Adds or releases a reference. The last release frees the list.
void prCssMediaQueryListRetain(PrCssMediaQueryList* self);
void prCssMediaQueryListRelease(PrCssMediaQueryList* PR_NULLABLE self);

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheet (cssom), the interface that CSSStyleSheet inherits. Every style sheet is a CSSStyleSheet.

// Returns the URL that the sheet was fetched from, or the null view if it was not fetched. A constructed sheet's URL is
// its `baseUrl`.
PrStringView prCssStyleSheetHref(const PrCssStyleSheet* self);

// Returns the element or processing instruction that owns the sheet (cssom-1/#dom-stylesheet-ownernode). Returns null
// for a sheet that another sheet imports, or that a caller constructed.
PrDomNode* PR_NULLABLE prCssStyleSheetOwnerNode(PrCssStyleSheet* self);

// Returns the sheet whose @import rule imports this sheet, or null.
PrCssStyleSheet* PR_NULLABLE prCssStyleSheetParentStyleSheet(PrCssStyleSheet* self);

// Returns the value of the owner element's title attribute, or the empty string if the element has none. A constructed
// sheet's title is the null view.
PrStringView prCssStyleSheetTitle(const PrCssStyleSheet* self);

// Returns the sheet's media list, which the sheet owns. An imported sheet's media list is the media list of its @import
// rule.
PrCssMediaList* prCssStyleSheetMedia(PrCssStyleSheet* self);

// Get and set whether the sheet is disabled.
bool prCssStyleSheetDisabled(const PrCssStyleSheet* self);
void prCssStyleSheetSetDisabled(PrCssStyleSheet* self, bool value);

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleSheet : StyleSheet (cssom)

// The media of a constructed sheet: a list whose queries are copied, or text if the list is null.
typedef struct PrCssMediaListOrString {
    PrCssMediaList* PR_NULLABLE mediaList;
    PrStringView string;
} PrCssMediaListOrString;

// The options of a constructed sheet (cssom-1/#dictdef-cssstylesheetinit).
typedef struct PrCssStyleSheetInit {
    PrStringView baseUrl;
    PrCssMediaListOrString media;
    bool disabled;
} PrCssStyleSheetInit;

// Returns the options that apply if a caller leaves them out: no base URL, no media, and enabled.
static inline PrCssStyleSheetInit prCssStyleSheetInitDefault(void) {
    PrCssStyleSheetInit value;
    value.baseUrl = PR_LITERAL(PrStringView){NULL, 0};
    value.media = PR_LITERAL(PrCssMediaListOrString){NULL, PR_SV("")};
    value.disabled = false;
    return value;
}

// Creates a constructed sheet with no rules (cssom-1/#dom-cssstylesheet-cssstylesheet). The caller holds its one
// reference. A null `init` means the defaults.
PrCssStyleSheet* prCssStyleSheetCreate(const PrCssStyleSheetInit* PR_NULLABLE init);

// Adds or releases a reference. A reference to a sheet or a rule keeps its tree alive. A sheet that a node owns, and
// its rules, keep the node's tree alive. A sheet or rule with no owner keeps alive the objects that it owns.
void prCssStyleSheetRetain(PrCssStyleSheet* self);
void prCssStyleSheetRelease(PrCssStyleSheet* PR_NULLABLE self);

// Returns the @import rule that imports the sheet, or null.
PrCssRule* PR_NULLABLE prCssStyleSheetOwnerRule(PrCssStyleSheet* self);

// Returns the sheet's rules, which the sheet owns.
PrCssRuleList* prCssStyleSheetCssRules(PrCssStyleSheet* self);

// Inserts the rule in `rule` at `index`, and writes `index` to `out`. Returns OutOfRange if `index` is past the end,
// Syntax if `rule` is not one valid rule, NotAllowed if the rule may not go at `index`, and InvalidState for an
// @namespace rule while the sheet holds a rule that is neither @import nor @namespace.
PrStatus prCssStyleSheetInsertRule(PrCssStyleSheet* self, PrStringView rule, uint32_t index,
                                   uint32_t* PR_NULLABLE out);

// Removes the rule at `index`. Returns OutOfRange if there is none, and InvalidState for an @namespace rule while the
// sheet holds a rule that is neither @import nor @namespace.
PrStatus prCssStyleSheetDeleteRule(PrCssStyleSheet* self, uint32_t index);

// Replaces the sheet's rules with the ones in `text`. Returns NotAllowed for a sheet that is not constructed.
PrStatus prCssStyleSheetReplaceSync(PrCssStyleSheet* self, PrStringView text);

// Inserts the rule `selector { style }` at `index`, or at the end if `index` is null
// (cssom-1/#dom-cssstylesheet-addrule). Writes -1, which the legacy method always returns, to `out`. Fails in the same
// cases as prCssStyleSheetInsertRule.
PrStatus prCssStyleSheetAddRule(PrCssStyleSheet* self, PrStringView selector, PrStringView style,
                                const uint32_t* PR_NULLABLE index, int32_t* PR_NULLABLE out);

// ---------------------------------------------------------------------------------------------------------------------
// CSSRule (cssom)

// The type constants of CSSRule (cssom-1/#dom-cssrule-type), and 0 for a rule that no constant names.
enum {
    PrCssRuleType_Style             = 1,
    PrCssRuleType_Charset           = 2,
    PrCssRuleType_Import            = 3,
    PrCssRuleType_Media             = 4,
    PrCssRuleType_FontFace          = 5,
    PrCssRuleType_Page              = 6,
    PrCssRuleType_Keyframes         = 7,
    PrCssRuleType_Keyframe          = 8,
    PrCssRuleType_Margin            = 9,
    PrCssRuleType_Namespace         = 10,
    PrCssRuleType_CounterStyle      = 11,
    PrCssRuleType_Supports          = 12,
    PrCssRuleType_FontFeatureValues = 14,
};
typedef uint16_t PrCssRuleType;

// The interface that most specifically describes a rule, which is the interface that a binding presents it as.
enum {
    PrCssRuleKind_Rule,                   // CSSRule alone
    PrCssRuleKind_Style,
    PrCssRuleKind_Import,
    PrCssRuleKind_Namespace,
    PrCssRuleKind_Media,
    PrCssRuleKind_Supports,
    PrCssRuleKind_Container,
    PrCssRuleKind_NestedDeclarations,
    PrCssRuleKind_FontFace,
    PrCssRuleKind_Page,
    PrCssRuleKind_Margin,
    PrCssRuleKind_Keyframes,
    PrCssRuleKind_Keyframe,
    PrCssRuleKind_LayerBlock,
    PrCssRuleKind_LayerStatement,
    PrCssRuleKind_Scope,
    PrCssRuleKind_StartingStyle,
    PrCssRuleKind_CounterStyle,
    PrCssRuleKind_Property,
    PrCssRuleKind_FontFeatureValues,
    PrCssRuleKind_FontPaletteValues,
    PrCssRuleKind_PositionTry,
    PrCssRuleKind_ViewTransition,
    PrCssRuleKind_Function,
    PrCssRuleKind_FunctionDeclarations,
    PrCssRuleKind_CustomMedia,
    PrCssRuleKind_SupportsCondition,
    PrCssRuleKind_Mixin,
    PrCssRuleKind_ApplyBlock,
    PrCssRuleKind_ApplyStatement,
    PrCssRuleKind_ContentsBlock,
    PrCssRuleKind_ContentsStatement,

    PrCssRuleKind_Count,
};
typedef uint8_t PrCssRuleKind;

// Get and set the rule, serialized (cssom-1/#serialize-a-css-rule). Setting the text does nothing.
PrString prCssRuleCssText(const PrCssRule* self);
void prCssRuleSetCssText(PrCssRule* self, PrStringView value);

// Returns the rule whose block holds this rule, or null for a rule that is not in a block.
PrCssRule* PR_NULLABLE prCssRuleParentRule(PrCssRule* self);

// Returns the sheet that holds the rule (cssom-1/#concept-css-rule-parent-css-style-sheet). Returns null for a rule
// removed from its list, and for the rules inside it.
PrCssStyleSheet* PR_NULLABLE prCssRuleParentStyleSheet(PrCssRule* self);

// Adds or releases a reference, which keeps the rule's tree alive in the same way as a reference to a sheet
// (prCssStyleSheetRetain).
void prCssRuleRetain(PrCssRule* self);
void prCssRuleRelease(PrCssRule* PR_NULLABLE self);

// Return the rule's type constant, and the interface that most specifically describes it.
PrCssRuleType prCssRuleType(const PrCssRule* self);
PrCssRuleKind prCssRuleKind(const PrCssRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSGroupingRule : CSSRule (cssom)

// Converts a CSSGroupingRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssGroupingRule, CssRule, CssRule)

// Returns a CSSRule as a CSSGroupingRule, or null if it is not one.
PrCssGroupingRule* PR_NULLABLE prCssGroupingRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the rules of the block, which the rule owns.
PrCssRuleList* prCssGroupingRuleCssRules(PrCssGroupingRule* self);

// Inserts the rule in `rule` at `index` of the block, and writes `index` to `out`. If the block may hold declarations,
// text that is not a rule may be declarations instead, which are inserted as a nested declarations rule. Returns
// OutOfRange if `index` is past the end, Syntax if `rule` is not one valid rule, and NotAllowed if the rule may not go
// in the block.
PrStatus prCssGroupingRuleInsertRule(PrCssGroupingRule* self, PrStringView rule, uint32_t index,
                                     uint32_t* PR_NULLABLE out);

// Removes the rule at `index` of the block. Returns OutOfRange if there is none.
PrStatus prCssGroupingRuleDeleteRule(PrCssGroupingRule* self, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleRule : CSSGroupingRule (cssom)

// Converts a CSSStyleRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssStyleRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssStyleRule, CssRule, CssRule)

// Returns a CSSRule as a CSSStyleRule, or null if it is not one.
PrCssStyleRule* PR_NULLABLE prCssStyleRuleQuery(PrCssRule* PR_NULLABLE object);

// Get and set the rule's selector list, serialized. Setting it parses `value` as a selector list, relative to the
// enclosing style rule if the rule is nested. Setting text that is not a selector list does nothing.
PrString prCssStyleRuleSelectorText(const PrCssStyleRule* self);
void prCssStyleRuleSetSelectorText(PrCssStyleRule* self, PrStringView value);

// Returns the rule's declarations, which the rule owns.
PrCssStyleProperties* prCssStyleRuleStyle(PrCssStyleRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSImportRule : CSSRule (cssom)

// Converts a CSSImportRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssImportRule, CssRule, CssRule)

// Returns a CSSRule as a CSSImportRule, or null if it is not one.
PrCssImportRule* PR_NULLABLE prCssImportRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the URL of the imported sheet, as written.
PrStringView prCssImportRuleHref(const PrCssImportRule* self);

// Returns the media queries of the rule's prelude, which the rule owns.
PrCssMediaList* prCssImportRuleMedia(PrCssImportRule* self);

// Returns the imported sheet, or null if none was loaded.
PrCssStyleSheet* PR_NULLABLE prCssImportRuleStyleSheet(PrCssImportRule* self);

// Returns the layer that the sheet is imported into: the empty string for `layer`, the name in layer(), and the null
// string for neither.
PrString prCssImportRuleLayerName(const PrCssImportRule* self);

// Returns the condition in the rule's supports(), serialized, or the null string if the rule has none.
PrString prCssImportRuleSupportsText(const PrCssImportRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSNamespaceRule : CSSRule (cssom)

// Converts a CSSNamespaceRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssNamespaceRule, CssRule, CssRule)

// Returns a CSSRule as a CSSNamespaceRule, or null if it is not one.
PrCssNamespaceRule* PR_NULLABLE prCssNamespaceRuleQuery(PrCssRule* PR_NULLABLE object);

// Return the namespace that the rule declares, and its prefix. The prefix is the empty string for the default
// namespace.
PrStringView prCssNamespaceRuleNamespaceUri(const PrCssNamespaceRule* self);
PrStringView prCssNamespaceRulePrefix(const PrCssNamespaceRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSConditionRule : CSSGroupingRule (css-conditional)

// Converts a CSSConditionRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssConditionRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssConditionRule, CssRule, CssRule)

// Returns a CSSRule as a CSSConditionRule, or null if it is not one.
PrCssConditionRule* PR_NULLABLE prCssConditionRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the rule's condition, serialized. A media rule's condition is its media list.
PrString prCssConditionRuleConditionText(const PrCssConditionRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSMediaRule : CSSConditionRule (css-conditional)

// Converts a CSSMediaRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssMediaRule, CssConditionRule, CssConditionRule)
PR_HANDLE_UPCAST(CssMediaRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssMediaRule, CssRule, CssRule)

// Returns a CSSRule as a CSSMediaRule, or null if it is not one.
PrCssMediaRule* PR_NULLABLE prCssMediaRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the rule's media queries, which the rule owns.
PrCssMediaList* prCssMediaRuleMedia(PrCssMediaRule* self);

// Returns true if the queries match the environment of the sheet's document. Returns false for a rule that is not in a
// sheet of a document.
bool prCssMediaRuleMatches(const PrCssMediaRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsRule : CSSConditionRule (css-conditional)

// Converts a CSSSupportsRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssSupportsRule, CssConditionRule, CssConditionRule)
PR_HANDLE_UPCAST(CssSupportsRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssSupportsRule, CssRule, CssRule)

// Returns a CSSRule as a CSSSupportsRule, or null if it is not one.
PrCssSupportsRule* PR_NULLABLE prCssSupportsRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns true if the engine supports what the rule's condition tests.
bool prCssSupportsRuleMatches(const PrCssSupportsRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSContainerRule : CSSConditionRule (css-conditional-5)

// Converts a CSSContainerRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssContainerRule, CssConditionRule, CssConditionRule)
PR_HANDLE_UPCAST(CssContainerRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssContainerRule, CssRule, CssRule)

// One condition of a container rule (css-conditional-5/#dictdef-csscontainercondition). A name or a query that the
// condition omits is the empty string.
typedef struct PrCssContainerCondition {
    PrString name;
    PrString query;
} PrCssContainerCondition;

// Frees the condition's strings.
static inline void prCssContainerConditionReset(PrCssContainerCondition* self) {
    prStringReset(&self->name);
    prStringReset(&self->query);
}

// Returns a CSSRule as a CSSContainerRule, or null if it is not one.
PrCssContainerRule* PR_NULLABLE prCssContainerRuleQuery(PrCssRule* PR_NULLABLE object);

// Return the name and the query of the rule's condition. Each is the empty string if the condition omits it, or if the
// rule has more than one condition.
PrString prCssContainerRuleContainerName(const PrCssContainerRule* self);
PrString prCssContainerRuleContainerQuery(const PrCssContainerRule* self);

// Writes up to `capacity` of the rule's conditions to `items`, which the caller releases, and returns the total count.
uint32_t prCssContainerRuleConditions(const PrCssContainerRule* self,
                                      PrCssContainerCondition* PR_NULLABLE items,
                                      uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// CSSNestedDeclarations : CSSRule (css-nesting)

// Converts a CSSNestedDeclarations to the interface that it inherits from.
PR_HANDLE_UPCAST(CssNestedDeclarations, CssRule, CssRule)

// Returns a CSSRule as a CSSNestedDeclarations, or null if it is not one.
PrCssNestedDeclarations* PR_NULLABLE prCssNestedDeclarationsQuery(PrCssRule* PR_NULLABLE object);

// Returns the rule's declarations, which the rule owns.
PrCssStyleProperties* prCssNestedDeclarationsStyle(PrCssNestedDeclarations* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceRule : CSSRule (css-fonts-5)

// Converts a CSSFontFaceRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFontFaceRule, CssRule, CssRule)

// Returns a CSSRule as a CSSFontFaceRule, or null if it is not one.
PrCssFontFaceRule* PR_NULLABLE prCssFontFaceRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the rule's descriptors, which the rule owns.
PrCssFontFaceDescriptors* prCssFontFaceRuleStyle(PrCssFontFaceRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSPageRule : CSSGroupingRule (cssom)

// Converts a CSSPageRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssPageRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssPageRule, CssRule, CssRule)

// Returns a CSSRule as a CSSPageRule, or null if it is not one.
PrCssPageRule* PR_NULLABLE prCssPageRuleQuery(PrCssRule* PR_NULLABLE object);

// Get and set the rule's page selectors, serialized. Setting text that the rule does not accept as its prelude does
// nothing.
PrString prCssPageRuleSelectorText(const PrCssPageRule* self);
void prCssPageRuleSetSelectorText(PrCssPageRule* self, PrStringView value);

// Returns the rule's descriptors, which the rule owns.
PrCssPageDescriptors* prCssPageRuleStyle(PrCssPageRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSMarginRule : CSSRule (cssom)

// Converts a CSSMarginRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssMarginRule, CssRule, CssRule)

// The page-margin boxes (css-page-3/#margin-boxes), each named after its at-rule without the '@'.
enum {
    PrCssMargin_TopLeftCorner,
    PrCssMargin_TopLeft,
    PrCssMargin_TopCenter,
    PrCssMargin_TopRight,
    PrCssMargin_TopRightCorner,
    PrCssMargin_RightTop,
    PrCssMargin_RightMiddle,
    PrCssMargin_RightBottom,
    PrCssMargin_BottomRightCorner,
    PrCssMargin_BottomRight,
    PrCssMargin_BottomCenter,
    PrCssMargin_BottomLeft,
    PrCssMargin_BottomLeftCorner,
    PrCssMargin_LeftBottom,
    PrCssMargin_LeftMiddle,
    PrCssMargin_LeftTop,
    PrCssMargin_Count,
};
typedef uint8_t PrCssMargin;

// Returns the name of the at-rule of `margin` without the '@', such as `top-left-corner`. The text lives as long as the
// process.
PrStringView prCssMarginName(PrCssMargin margin);

// Returns a CSSRule as a CSSMarginRule, or null if it is not one.
PrCssMarginRule* PR_NULLABLE prCssMarginRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the box that the rule is for (cssom-1/#dom-cssmarginrule-name).
PrCssMargin prCssMarginRuleMargin(const PrCssMarginRule* self);

// Returns the rule's declarations, which the rule owns, as a plain declaration block. No spec defines the
// CSSMarginDescriptors that the IDL names.
PrCssStyleDeclaration* prCssMarginRuleStyle(PrCssMarginRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframesRule : CSSRule (css-animations)

// Converts a CSSKeyframesRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssKeyframesRule, CssRule, CssRule)

// Returns a CSSRule as a CSSKeyframesRule, or null if it is not one.
PrCssKeyframesRule* PR_NULLABLE prCssKeyframesRuleQuery(PrCssRule* PR_NULLABLE object);

// Get and set the animation's name. Setting it writes `value` as an identifier if it can be one, and as a string
// otherwise, such as for `none` or a CSS-wide keyword.
PrStringView prCssKeyframesRuleName(const PrCssKeyframesRule* self);
void prCssKeyframesRuleSetName(PrCssKeyframesRule* self, PrStringView value);

// Returns the rule's keyframes, which the rule owns.
PrCssRuleList* prCssKeyframesRuleCssRules(PrCssKeyframesRule* self);

// Return the number of keyframes, and the keyframe at `index` or null past the last keyframe.
uint32_t prCssKeyframesRuleLength(const PrCssKeyframesRule* self);
PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleKeyframeRuleAt(PrCssKeyframesRule* self,
                                                                uint32_t index);

// Appends the keyframe in `rule`. Does nothing if `rule` is not a keyframe.
void prCssKeyframesRuleAppendRule(PrCssKeyframesRule* self, PrStringView rule);

// Removes the last keyframe whose selectors are the ones in `select`, if there is one.
void prCssKeyframesRuleDeleteRule(PrCssKeyframesRule* self, PrStringView select);

// Returns the last keyframe whose selectors are the ones in `select`, or null if there is none.
PrCssKeyframeRule* PR_NULLABLE prCssKeyframesRuleFindRule(PrCssKeyframesRule* self,
                                                          PrStringView select);

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeyframeRule : CSSRule (css-animations)

// Converts a CSSKeyframeRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssKeyframeRule, CssRule, CssRule)

// Returns a CSSRule as a CSSKeyframeRule, or null if it is not one.
PrCssKeyframeRule* PR_NULLABLE prCssKeyframeRuleQuery(PrCssRule* PR_NULLABLE object);

// Get and set the keyframe's selectors, serialized. Setting them returns Syntax, and leaves the rule unchanged, if
// `value` is not a list of keyframe selectors.
PrString prCssKeyframeRuleKeyText(const PrCssKeyframeRule* self);
PrStatus prCssKeyframeRuleSetKeyText(PrCssKeyframeRule* self, PrStringView value);

// Returns the keyframe's declarations, which the rule owns.
PrCssStyleProperties* prCssKeyframeRuleStyle(PrCssKeyframeRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerBlockRule : CSSGroupingRule (css-cascade)

// Converts a CSSLayerBlockRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssLayerBlockRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssLayerBlockRule, CssRule, CssRule)

// Returns a CSSRule as a CSSLayerBlockRule, or null if it is not one.
PrCssLayerBlockRule* PR_NULLABLE prCssLayerBlockRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the layer's name, or the empty string for an anonymous layer.
PrString prCssLayerBlockRuleName(const PrCssLayerBlockRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSLayerStatementRule : CSSRule (css-cascade)

// Converts a CSSLayerStatementRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssLayerStatementRule, CssRule, CssRule)

// Returns a CSSRule as a CSSLayerStatementRule, or null if it is not one.
PrCssLayerStatementRule* PR_NULLABLE prCssLayerStatementRuleQuery(PrCssRule* PR_NULLABLE object);

// Writes up to `capacity` of the layer names that the rule declares to `items`, which the caller releases, and returns
// the total count.
uint32_t prCssLayerStatementRuleNameList(const PrCssLayerStatementRule* self,
                                         PrString* PR_NULLABLE items, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// CSSScopeRule : CSSGroupingRule (css-cascade-6)

// Converts a CSSScopeRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssScopeRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssScopeRule, CssRule, CssRule)

// Returns a CSSRule as a CSSScopeRule, or null if it is not one.
PrCssScopeRule* PR_NULLABLE prCssScopeRuleQuery(PrCssRule* PR_NULLABLE object);

// Return the selectors of the scoping root, and of the scoping limit, serialized. Each is the null string if the rule
// does not write it.
PrString prCssScopeRuleStart(const PrCssScopeRule* self);
PrString prCssScopeRuleEnd(const PrCssScopeRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSStartingStyleRule : CSSGroupingRule (css-transitions-2)

// Converts a CSSStartingStyleRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssStartingStyleRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssStartingStyleRule, CssRule, CssRule)

// Returns a CSSRule as a CSSStartingStyleRule, or null if it is not one.
PrCssStartingStyleRule* PR_NULLABLE prCssStartingStyleRuleQuery(PrCssRule* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSCounterStyleRule : CSSRule (css-counter-styles)

// Converts a CSSCounterStyleRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssCounterStyleRule, CssRule, CssRule)

// Returns a CSSRule as a CSSCounterStyleRule, or null if it is not one.
PrCssCounterStyleRule* PR_NULLABLE prCssCounterStyleRuleQuery(PrCssRule* PR_NULLABLE object);

// Get and set the counter style's name. Setting it writes `value` as an identifier, and does nothing if the rule does
// not accept that name.
PrStringView prCssCounterStyleRuleName(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetName(PrCssCounterStyleRule* self, PrStringView value);

// Get and set the rule's descriptors, one pair of functions for each descriptor. A descriptor reads as the value of its
// declaration, serialized, or as the empty string if the rule declares none. Setting one to the empty string, or to a
// value that its grammar does not accept, does nothing.
PrString prCssCounterStyleRuleSystem(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetSystem(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleSymbols(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetSymbols(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleAdditiveSymbols(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetAdditiveSymbols(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleNegative(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetNegative(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRulePrefix(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetPrefix(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleSuffix(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetSuffix(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleRange(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetRange(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRulePad(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetPad(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleSpeakAs(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetSpeakAs(PrCssCounterStyleRule* self, PrStringView value);
PrString prCssCounterStyleRuleFallback(const PrCssCounterStyleRule* self);
void prCssCounterStyleRuleSetFallback(PrCssCounterStyleRule* self, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// CSSPropertyRule : CSSRule (css-properties-values-api)

// Converts a CSSPropertyRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssPropertyRule, CssRule, CssRule)

// Returns a CSSRule as a CSSPropertyRule, or null if it is not one.
PrCssPropertyRule* PR_NULLABLE prCssPropertyRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the name of the custom property that the rule registers.
PrStringView prCssPropertyRuleName(const PrCssPropertyRule* self);

// Returns the value of the string that the syntax descriptor declares.
PrStringView prCssPropertyRuleSyntax(const PrCssPropertyRule* self);

// Returns true if the inherits descriptor is `true`.
bool prCssPropertyRuleInherits(const PrCssPropertyRule* self);

// Returns the value of the initial-value descriptor, serialized, or the null string if the rule declares none.
PrString prCssPropertyRuleInitialValue(const PrCssPropertyRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesRule : CSSRule (css-fonts)

// Converts a CSSFontFeatureValuesRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFontFeatureValuesRule, CssRule, CssRule)

// Returns a CSSRule as a CSSFontFeatureValuesRule, or null if it is not one.
PrCssFontFeatureValuesRule* PR_NULLABLE prCssFontFeatureValuesRuleQuery(
    PrCssRule* PR_NULLABLE object);

// Get and set the font families that the rule applies to, serialized. Setting text that the rule does not accept as its
// prelude does nothing.
PrString prCssFontFeatureValuesRuleFontFamily(const PrCssFontFeatureValuesRule* self);
void prCssFontFeatureValuesRuleSetFontFamily(PrCssFontFeatureValuesRule* self, PrStringView value);

// Return the rule's map for each feature type, one function per type, named after the type's at-rule. The rule owns the
// maps.
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleAnnotation(PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleOrnaments(PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStylistic(PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleSwash(PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleCharacterVariant(
    PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleStyleset(PrCssFontFeatureValuesRule* self);
PrCssFontFeatureValuesMap* prCssFontFeatureValuesRuleHistoricalForms(
    PrCssFontFeatureValuesRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFeatureValuesMap (css-fonts)
//
// The named values of one feature type. An entry maps a name to a list of feature indexes.

// Returns the number of entries.
uint32_t prCssFontFeatureValuesMapSize(const PrCssFontFeatureValuesMap* self);

// Returns the key of the entry at `index`, which is below the size.
PrStringView prCssFontFeatureValuesMapKeyAt(const PrCssFontFeatureValuesMap* self, uint32_t index);

// Writes the value of the entry at `index`, which is below the size. Writes up to `capacity` items to `items` and
// returns the total count.
uint32_t prCssFontFeatureValuesMapValueAt(const PrCssFontFeatureValuesMap* self, uint32_t index,
                                          uint32_t* PR_NULLABLE items, uint32_t capacity);

// Returns true if the map has an entry for `key`.
bool prCssFontFeatureValuesMapHas(const PrCssFontFeatureValuesMap* self, PrStringView key);

// Writes the value of the entry for `key`, which must be in the map. Writes up to `capacity` items to `items` and
// returns the total count.
uint32_t prCssFontFeatureValuesMapGet(const PrCssFontFeatureValuesMap* self, PrStringView key,
                                      uint32_t* PR_NULLABLE items, uint32_t capacity);

// Removes the entry for `key`. Returns false if there is none.
bool prCssFontFeatureValuesMapDelete(PrCssFontFeatureValuesMap* self, PrStringView key);

// Removes every entry.
void prCssFontFeatureValuesMapClear(PrCssFontFeatureValuesMap* self);

// Sets the entry for `key` to the `count` indexes in `values`. Does nothing if `key` is not an identifier, or if the
// feature type of the map does not accept `count` indexes.
void prCssFontFeatureValuesMapSet(PrCssFontFeatureValuesMap* self, PrStringView key,
                                  const uint32_t* PR_NULLABLE values, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontPaletteValuesRule : CSSRule (css-fonts)

// Converts a CSSFontPaletteValuesRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFontPaletteValuesRule, CssRule, CssRule)

// Returns a CSSRule as a CSSFontPaletteValuesRule, or null if it is not one.
PrCssFontPaletteValuesRule* PR_NULLABLE prCssFontPaletteValuesRuleQuery(
    PrCssRule* PR_NULLABLE object);

// Returns the palette's name.
PrStringView prCssFontPaletteValuesRuleName(const PrCssFontPaletteValuesRule* self);

// Return the rule's descriptors, one function for each descriptor. A descriptor reads as the value of its declaration,
// serialized, or as the empty string if the rule declares none.
PrString prCssFontPaletteValuesRuleFontFamily(const PrCssFontPaletteValuesRule* self);
PrString prCssFontPaletteValuesRuleBasePalette(const PrCssFontPaletteValuesRule* self);
PrString prCssFontPaletteValuesRuleOverrideColors(const PrCssFontPaletteValuesRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSPositionTryRule : CSSRule (css-anchor-position)

// Converts a CSSPositionTryRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssPositionTryRule, CssRule, CssRule)

// Returns a CSSRule as a CSSPositionTryRule, or null if it is not one.
PrCssPositionTryRule* PR_NULLABLE prCssPositionTryRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the name of the position option.
PrStringView prCssPositionTryRuleName(const PrCssPositionTryRule* self);

// Returns the rule's descriptors, which the rule owns.
PrCssPositionTryDescriptors* prCssPositionTryRuleStyle(PrCssPositionTryRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSViewTransitionRule : CSSRule (css-view-transitions)

// Converts a CSSViewTransitionRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssViewTransitionRule, CssRule, CssRule)

// Returns a CSSRule as a CSSViewTransitionRule, or null if it is not one.
PrCssViewTransitionRule* PR_NULLABLE prCssViewTransitionRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the value of the navigation descriptor, serialized, or the empty string if the rule declares none.
PrString prCssViewTransitionRuleNavigation(const PrCssViewTransitionRule* self);

// Writes up to `capacity` of the names that the types descriptor declares to `items`, and returns the total count.
// `none` declares no names.
uint32_t prCssViewTransitionRuleTypes(const PrCssViewTransitionRule* self,
                                      PrStringView* PR_NULLABLE items, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionRule : CSSGroupingRule (css-mixins)

// Converts a CSSFunctionRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssFunctionRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssFunctionRule, CssRule, CssRule)

// A parameter of a function or a mixin (css-mixins-1/#dictdef-functionparameter). A type that the parameter omits is
// `*`, and a default that it omits is the null string.
typedef struct PrCssFunctionParameter {
    PrString name;
    PrString type;
    PrString defaultValue;
} PrCssFunctionParameter;

// Frees the parameter's strings.
static inline void prCssFunctionParameterReset(PrCssFunctionParameter* self) {
    prStringReset(&self->name);
    prStringReset(&self->type);
    prStringReset(&self->defaultValue);
}

// Returns a CSSRule as a CSSFunctionRule, or null if it is not one.
PrCssFunctionRule* PR_NULLABLE prCssFunctionRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the function's name.
PrStringView prCssFunctionRuleName(const PrCssFunctionRule* self);

// Writes up to `capacity` of the function's parameters to `items`, which the caller releases, and returns the total
// count.
uint32_t prCssFunctionRuleGetParameters(const PrCssFunctionRule* self,
                                        PrCssFunctionParameter* PR_NULLABLE items,
                                        uint32_t capacity);

// Returns the function's return type, or `*` if the rule writes none.
PrString prCssFunctionRuleReturnType(const PrCssFunctionRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionDeclarations : CSSRule (css-mixins)

// Converts a CSSFunctionDeclarations to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFunctionDeclarations, CssRule, CssRule)

// Returns a CSSRule as a CSSFunctionDeclarations, or null if it is not one.
PrCssFunctionDeclarations* PR_NULLABLE prCssFunctionDeclarationsQuery(
    PrCssRule* PR_NULLABLE object);

// Returns the declarations in the function's body, which the rule owns.
PrCssFunctionDescriptors* prCssFunctionDeclarationsStyle(PrCssFunctionDeclarations* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSCustomMediaRule : CSSRule (mediaqueries-5)

// Converts a CSSCustomMediaRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssCustomMediaRule, CssRule, CssRule)

// The value of a custom media query (mediaqueries-5/#dom-csscustommediarule-query). It is the rule's media list, or the
// boolean that the query was written as if `mediaList` is null.
typedef struct PrCssMediaListOrBoolean {
    PrCssMediaList* PR_NULLABLE mediaList;
    bool boolean;
} PrCssMediaListOrBoolean;

// Returns a CSSRule as a CSSCustomMediaRule, or null if it is not one.
PrCssCustomMediaRule* PR_NULLABLE prCssCustomMediaRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the custom media query's name.
PrStringView prCssCustomMediaRuleName(const PrCssCustomMediaRule* self);

// Returns the custom media query's value. The rule owns the media list.
PrCssMediaListOrBoolean prCssCustomMediaRuleQueryValue(PrCssCustomMediaRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSSupportsConditionRule : CSSGroupingRule (css-conditional-5)

// Converts a CSSSupportsConditionRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssSupportsConditionRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssSupportsConditionRule, CssRule, CssRule)

// Returns a CSSRule as a CSSSupportsConditionRule, or null if it is not one.
PrCssSupportsConditionRule* PR_NULLABLE prCssSupportsConditionRuleQuery(
    PrCssRule* PR_NULLABLE object);

// Returns the name that the rule gives its condition.
PrStringView prCssSupportsConditionRuleName(const PrCssSupportsConditionRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSMixinRule : CSSGroupingRule (css-mixins)

// Converts a CSSMixinRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssMixinRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssMixinRule, CssRule, CssRule)

// Returns a CSSRule as a CSSMixinRule, or null if it is not one.
PrCssMixinRule* PR_NULLABLE prCssMixinRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the mixin's name, serialized as an identifier.
PrString prCssMixinRuleName(const PrCssMixinRule* self);

// Writes up to `capacity` of the mixin's parameters to `items`, which the caller releases, and returns the total count.
uint32_t prCssMixinRuleGetParameters(const PrCssMixinRule* self,
                                     PrCssFunctionParameter* PR_NULLABLE items, uint32_t capacity);

// Returns true if the mixin's body holds an @contents rule, at any depth.
bool prCssMixinRuleContents(const PrCssMixinRule* self);

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyBlockRule : CSSGroupingRule (css-mixins)

// Converts a CSSApplyBlockRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssApplyBlockRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssApplyBlockRule, CssRule, CssRule)

// Returns a CSSRule as a CSSApplyBlockRule, or null if it is not one.
PrCssApplyBlockRule* PR_NULLABLE prCssApplyBlockRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the name of the mixin that the rule applies, serialized as an identifier.
PrString prCssApplyBlockRuleName(const PrCssApplyBlockRule* self);

// Writes up to `capacity` of the rule's arguments, each as written, to `items`, and returns the total count.
uint32_t prCssApplyBlockRuleGetArguments(const PrCssApplyBlockRule* self,
                                         PrStringView* PR_NULLABLE items, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// CSSApplyStatementRule : CSSRule (css-mixins)

// Converts a CSSApplyStatementRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssApplyStatementRule, CssRule, CssRule)

// Returns a CSSRule as a CSSApplyStatementRule, or null if it is not one.
PrCssApplyStatementRule* PR_NULLABLE prCssApplyStatementRuleQuery(PrCssRule* PR_NULLABLE object);

// Returns the name of the mixin that the rule applies, serialized as an identifier.
PrString prCssApplyStatementRuleName(const PrCssApplyStatementRule* self);

// Writes up to `capacity` of the rule's arguments, each as written, to `items`, and returns the total count.
uint32_t prCssApplyStatementRuleGetArguments(const PrCssApplyStatementRule* self,
                                             PrStringView* PR_NULLABLE items, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsBlockRule : CSSGroupingRule (css-mixins)

// Converts a CSSContentsBlockRule to each interface that it inherits from.
PR_HANDLE_UPCAST(CssContentsBlockRule, CssGroupingRule, CssGroupingRule)
PR_HANDLE_UPCAST(CssContentsBlockRule, CssRule, CssRule)

// Returns a CSSRule as a CSSContentsBlockRule, or null if it is not one.
PrCssContentsBlockRule* PR_NULLABLE prCssContentsBlockRuleQuery(PrCssRule* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSContentsStatementRule : CSSRule (css-mixins)

// Converts a CSSContentsStatementRule to the interface that it inherits from.
PR_HANDLE_UPCAST(CssContentsStatementRule, CssRule, CssRule)

// Returns a CSSRule as a CSSContentsStatementRule, or null if it is not one.
PrCssContentsStatementRule* PR_NULLABLE prCssContentsStatementRuleQuery(
    PrCssRule* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSRuleList (cssom)

// Return the rule at `index` or null past the last rule, and the number of rules.
PrCssRule* PR_NULLABLE prCssRuleListRuleAt(PrCssRuleList* self, uint32_t index);
uint32_t prCssRuleListLength(const PrCssRuleList* self);

// ---------------------------------------------------------------------------------------------------------------------
// StyleSheetList (cssom)

// Return the sheet at `index` or null past the last sheet, and the number of sheets.
PrCssStyleSheet* PR_NULLABLE prCssStyleSheetListStyleSheetAt(PrCssStyleSheetList* self,
                                                             uint32_t index);
uint32_t prCssStyleSheetListLength(const PrCssStyleSheetList* self);

// ---------------------------------------------------------------------------------------------------------------------
// Declaration names

// Return the name of a property or a descriptor as its defining spec writes it. The text lives as long as the process.
PrStringView prCssPropertyName(PrCssProperty property);
PrStringView prCssDescriptorName(PrCssDescriptor descriptor);

// Copies `name`. The copy owns its author-defined name.
PrCssDeclarationName prCssDeclarationNameCopy(const PrCssDeclarationName* name);

// Frees the author-defined name, and leaves `self` naming nothing.
void prCssDeclarationNameReset(PrCssDeclarationName* self);

// Writes the name that `text` refers to in a block of properties, such as the block of a style rule, following legacy
// name aliases (css-cascade-5/#legacy-name-alias). Returns false if no property has that name, and leaves `out`
// unchanged.
bool prCssPropertyNameFromText(PrStringView text, PrCssDeclarationName* out);

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleDeclaration (cssom)
//
// A read-only block (cssom-1/#cssstyledeclaration-readonly-flag), such as a computed style, returns ReadOnly from each
// change and stays unchanged.

// Get and set the declarations, serialized. Setting the text replaces them with the declarations in `value`.
PrString prCssStyleDeclarationCssText(const PrCssStyleDeclaration* self);
PrStatus prCssStyleDeclarationSetCssText(PrCssStyleDeclaration* self, PrStringView value);

// Returns the number of declarations that the block lists. A shorthand is listed as its longhands.
uint32_t prCssStyleDeclarationLength(const PrCssStyleDeclaration* self);

// Writes the name that `text` refers to in the block. A descriptor of the block's rule takes precedence over a property
// of the same name, and a legacy name resolves to the name that it is an alias of. Returns false if the block accepts
// no declaration of that name, and leaves `out` unchanged.
bool prCssStyleDeclarationNameFromText(const PrCssStyleDeclaration* self, PrStringView text,
                                       PrCssDeclarationName* out);

// Writes the name of the declaration at `index`, which is below the length.
void prCssStyleDeclarationNameAt(const PrCssStyleDeclaration* self, uint32_t index,
                                 PrCssDeclarationName* out);

// Returns the value of `name`, serialized (cssom-1/#dom-cssstyledeclaration-getpropertyvalue), or the empty string if
// the block has none or accepts no such declaration.
PrString prCssStyleDeclarationGetPropertyValue(const PrCssStyleDeclaration* self,
                                               const PrCssDeclarationName* name);

// Returns true if `name` is declared !important (cssom-1/#dom-cssstyledeclaration-getpropertypriority).
bool prCssStyleDeclarationIsImportant(const PrCssStyleDeclaration* self,
                                      const PrCssDeclarationName* name);

// Declares `name` as the value in `value` (cssom-1/#dom-cssstyledeclaration-setproperty). An empty `value` removes the
// declaration. Returns InvalidData, and leaves the block unchanged, for a name that the block does not accept, a value
// that its grammar does not accept, or an importance that the block's rule does not accept.
PrStatus prCssStyleDeclarationSetProperty(PrCssStyleDeclaration* self,
                                          const PrCssDeclarationName* name, PrStringView value,
                                          bool important);

// Removes the declaration of `name` (cssom-1/#dom-cssstyledeclaration-removeproperty). Writes the value that it had to
// `out`, or the empty string if the block declared none. Returns InvalidData for a name that the block does not accept.
PrStatus prCssStyleDeclarationRemoveProperty(PrCssStyleDeclaration* self,
                                             const PrCssDeclarationName* name,
                                             PrString* PR_NULLABLE out);

// Returns the rule whose declarations these are, or null for an element's style attribute.
PrCssRule* PR_NULLABLE prCssStyleDeclarationParentRule(PrCssStyleDeclaration* self);

// Writes the value of `name`. Writes nothing if the block declares none, or holds a value that is pending on the
// substitution of a shorthand (css-values-5/#pending-substitution-value). A shorthand's value is what its longhands
// serialize to, as matched by its grammar. Returns InvalidData for a name that the block does not accept.
PrStatus prCssStyleDeclarationValue(const PrCssStyleDeclaration* self,
                                    const PrCssDeclarationName* name, PrCssValue* out);

// Declares `name` as `value`. The value is matched against the grammar in the same way as its written tokens would be,
// with a numeric value in place of a number, a percentage or a dimension of its type. A number is never a length,
// however it would be written. A number outside the range of its place in the grammar, or a non-integer where an
// integer is required, is wrapped in a calculation (css-typed-om-1/#create-an-internal-representation). The value of a
// custom property, or a value of only Tokens, is the tokens that it is written as. Returns InvalidData, and leaves the
// block unchanged, for a name that the block does not accept, a value that its grammar does not accept, or an
// importance that the block's rule does not accept.
PrStatus prCssStyleDeclarationSetValue(PrCssStyleDeclaration* self,
                                       const PrCssDeclarationName* name, const PrCssValue* value,
                                       bool important);

// Appends the items of `value` to the items of a list-valued property (prCssPropertyListValued). Returns InvalidData if
// the declaration holds no list of the items, and otherwise fails in the same cases as prCssStyleDeclarationSetValue.
PrStatus prCssStyleDeclarationAppendValue(PrCssStyleDeclaration* self,
                                          const PrCssDeclarationName* name,
                                          const PrCssValue* value);

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleProperties : CSSStyleDeclaration (cssom)

// Converts a CSSStyleProperties to the interface that it inherits from.
PR_HANDLE_UPCAST(CssStyleProperties, CssStyleDeclaration, CssStyleDeclaration)

// Returns a CSSStyleDeclaration as a CSSStyleProperties, or null if it is not one.
PrCssStyleProperties* PR_NULLABLE prCssStylePropertiesQuery(
    PrCssStyleDeclaration* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFontFaceDescriptors : CSSStyleDeclaration (css-fonts-5)

// Converts a CSSFontFaceDescriptors to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFontFaceDescriptors, CssStyleDeclaration, CssStyleDeclaration)

// Returns a CSSStyleDeclaration as a CSSFontFaceDescriptors, or null if it is not one.
PrCssFontFaceDescriptors* PR_NULLABLE prCssFontFaceDescriptorsQuery(
    PrCssStyleDeclaration* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSPageDescriptors : CSSStyleDeclaration (cssom)

// Converts a CSSPageDescriptors to the interface that it inherits from.
PR_HANDLE_UPCAST(CssPageDescriptors, CssStyleDeclaration, CssStyleDeclaration)

// Returns a CSSStyleDeclaration as a CSSPageDescriptors, or null if it is not one.
PrCssPageDescriptors* PR_NULLABLE prCssPageDescriptorsQuery(
    PrCssStyleDeclaration* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSPositionTryDescriptors : CSSStyleDeclaration (css-anchor-position)

// Converts a CSSPositionTryDescriptors to the interface that it inherits from.
PR_HANDLE_UPCAST(CssPositionTryDescriptors, CssStyleDeclaration, CssStyleDeclaration)

// Returns a CSSStyleDeclaration as a CSSPositionTryDescriptors, or null if it is not one.
PrCssPositionTryDescriptors* PR_NULLABLE prCssPositionTryDescriptorsQuery(
    PrCssStyleDeclaration* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSSFunctionDescriptors : CSSStyleDeclaration (css-mixins)

// Converts a CSSFunctionDescriptors to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFunctionDescriptors, CssStyleDeclaration, CssStyleDeclaration)

// Returns a CSSStyleDeclaration as a CSSFunctionDescriptors, or null if it is not one.
PrCssFunctionDescriptors* PR_NULLABLE prCssFunctionDescriptorsQuery(
    PrCssStyleDeclaration* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// CSS (cssom)

// Returns `ident`, escaped as an identifier (cssom-1/#dom-css-escape).
PrString prCssEscape(PrStringView ident);

// Returns true if a style rule accepts a declaration of `name` with `value` (css-conditional-3/#dom-css-supports).
bool prCssSupportsPropertyValue(const PrCssDeclarationName* name, PrStringView value);

// Returns true if `conditionText` is a supported <supports-condition>, or one wrapped in parentheses
// (css-conditional-3/#dom-css-supports-conditiontext).
bool prCssSupportsConditionText(PrStringView conditionText);

// A custom property to register (css-properties-values-api-1/#dictdef-propertydefinition).
typedef struct PrCssPropertyDefinition {
    PrStringView name;
    PrStringView syntax;
    bool inherits;
    PrStringView initialValue;   // the null view if it is not given
} PrCssPropertyDefinition;

// Returns the members that apply if a caller leaves them out: the universal syntax `*`, not inherited, and no initial
// value.
static inline PrCssPropertyDefinition prCssPropertyDefinitionDefault(void) {
    PrCssPropertyDefinition value;
    value.name = PR_SV("");
    value.syntax = PR_SV("*");
    value.inherits = false;
    value.initialValue.data = NULL;
    value.initialValue.size = 0;
    return value;
}

// ---------------------------------------------------------------------------------------------------------------------
// FontFace (css-font-loading)

// A font's descriptors and the font they name. A face is reference counted, so that a script and each set that holds
// the face share it.
typedef struct PrCssFontFace PrCssFontFace;

// A document's set of font faces, which the document owns.
typedef struct PrCssFontFaceSet PrCssFontFaceSet;

// The load status of a face (css-font-loading-3/#enumdef-fontfaceloadstatus).
enum {
    PrCssFontFaceStatus_Unloaded,
    PrCssFontFaceStatus_Loading,
    PrCssFontFaceStatus_Loaded,
    PrCssFontFaceStatus_Error,
};
typedef uint8_t PrCssFontFaceStatus;

// The @font-face descriptors that a face reflects (css-font-loading-3/#fontface-interface).
enum {
    PrCssFontFaceDescriptor_Family,
    PrCssFontFaceDescriptor_Style,
    PrCssFontFaceDescriptor_Weight,
    PrCssFontFaceDescriptor_Stretch,
    PrCssFontFaceDescriptor_UnicodeRange,
    PrCssFontFaceDescriptor_FeatureSettings,
    PrCssFontFaceDescriptor_VariationSettings,
    PrCssFontFaceDescriptor_Display,
    PrCssFontFaceDescriptor_AscentOverride,
    PrCssFontFaceDescriptor_DescentOverride,
    PrCssFontFaceDescriptor_LineGapOverride,
    PrCssFontFaceDescriptor_Count,
};
typedef uint8_t PrCssFontFaceDescriptor;

// Creates a face (css-font-loading-3/#font-face-constructor). The caller holds its one reference. `family`, each of
// `descriptors`, and a text `source` parse as their @font-face descriptors. If one does not parse, the face has status
// Error, and that descriptor is the empty string. `descriptors` holds a value for each descriptor after the family, and
// a null view keeps the default. A face with a `source` is Unloaded until it loads.
PrCssFontFace* prCssFontFaceCreate(PrStringView family, PrStringView source,
                                   const PrStringView* PR_NULLABLE descriptors);

// Creates a face in the same way as prCssFontFaceCreate, for a face whose source is the font data itself, in the realm
// of `document`. The face is Loaded if the data makes a font, and Error otherwise. The codec of the document's host
// decompresses compressed data. Without a document or a codec, compressed data is an error.
PrCssFontFace* prCssFontFaceCreateFromData(PrDomDocument* PR_NULLABLE document, PrStringView family, PrByteView data,
                                           const PrStringView* PR_NULLABLE descriptors);

// Adds or releases a reference. The last release frees the face.
void prCssFontFaceRetain(PrCssFontFace* self);
void prCssFontFaceRelease(PrCssFontFace* PR_NULLABLE self);

// Returns the value of `descriptor`, serialized, or the empty string if it did not parse.
PrString prCssFontFaceDescriptor(const PrCssFontFace* self, PrCssFontFaceDescriptor descriptor);

// Sets `descriptor` to the value in `value`. Returns Syntax, and leaves the descriptor unchanged, if the descriptor
// does not accept `value`.
PrStatus prCssFontFaceSetDescriptor(PrCssFontFace* self, PrCssFontFaceDescriptor descriptor,
                                    PrStringView value);

// Returns the face's load status.
PrCssFontFaceStatus prCssFontFaceStatus(const PrCssFontFace* self);

// Returns the document through which a loading face loads. That document's font callback is notified when the load
// settles. Returns null if the face is not loading.
PrDomDocument* PR_NULLABLE prCssFontFaceLoadingDocument(const PrCssFontFace* self);

// Returns true if the face is in error because a descriptor or its source did not parse, rather than because its font
// did not load.
bool prCssFontFaceFailedToParse(const PrCssFontFace* self);

// Fetches an unloaded face's font through `document`'s browsing context, from the first source that is a font
// (css-font-loading-3/#dom-fontface-load). The face loads in a separate task, or immediately if the document has no
// browsing context. Returns the face's status afterward.
PrCssFontFaceStatus prCssFontFaceLoad(PrCssFontFace* self, PrDomDocument* document);

// Returns true if the face is CSS-connected (css-font-loading-3/#css-connected), which means that it is the face of an
// @font-face rule in a style sheet and reflects the rule's descriptors.
bool prCssFontFaceIsCssConnected(const PrCssFontFace* self);

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSet : EventTarget (css-font-loading)

// The load status of a set (css-font-loading-3/#enumdef-fontfacesetloadstatus).
enum {
    PrCssFontFaceSetLoadStatus_Loading,
    PrCssFontFaceSetLoadStatus_Loaded,
};
typedef uint8_t PrCssFontFaceSetLoadStatus;

// Receives notifications about a document's fonts.
typedef struct PrCssFontCallback {
    // Called with a face whose status changes during a load through the document.
    void (*PR_NULLABLE faceStatus)(void* PR_NULLABLE context, PrDomDocument* document,
                                   PrCssFontFace* face);

    // Called when the document's set switches between loading and loaded.
    void (*PR_NULLABLE setStatus)(void* PR_NULLABLE context, PrDomDocument* document,
                                  PrCssFontFaceSet* set);

    void* PR_NULLABLE context;
} PrCssFontCallback;

// Returns an EventTarget as a FontFaceSet, or null if it is not one.
PrCssFontFaceSet* PR_NULLABLE prCssFontFaceSetQuery(PrDomEventTarget* PR_NULLABLE object);

// Converts a FontFaceSet to the interface that it inherits from.
PrDomEventTarget* prCssFontFaceSetAsEventTarget(PrCssFontFaceSet* self);

// Returns the number of faces in the set.
uint32_t prCssFontFaceSetSize(const PrCssFontFaceSet* self);

// Returns the face at `index`, in the set's order, or null past the last face.
PrCssFontFace* PR_NULLABLE prCssFontFaceSetAt(PrCssFontFaceSet* self, uint32_t index);

// Returns true if the set holds `face`.
bool prCssFontFaceSetHas(const PrCssFontFaceSet* self, const PrCssFontFace* face);

// Adds `face` and retains it, unless the set already holds it. Returns NotAllowed for a CSS-connected face.
PrStatus prCssFontFaceSetAdd(PrCssFontFaceSet* self, PrCssFontFace* face);

// Removes `face` and releases it. Returns false for a face that the set does not hold, or for a CSS-connected face.
bool prCssFontFaceSetDelete(PrCssFontFaceSet* self, PrCssFontFace* face);

// Removes the faces added to the set, and keeps the CSS-connected ones.
void prCssFontFaceSetClear(PrCssFontFaceSet* self);

// Returns whether the set is loading or loaded.
PrCssFontFaceSetLoadStatus prCssFontFaceSetStatus(const PrCssFontFaceSet* self);

// Writes true to `out` if every face that `font` matches has loaded (css-font-loading-3/#dom-fontfaceset-check).
// Returns Syntax if `font` is not a value of the font property.
PrStatus prCssFontFaceSetCheck(const PrCssFontFaceSet* self, PrStringView font, bool* out);

// Loads each face that `font` matches, through `document`'s browsing context
// (css-font-loading-3/#dom-fontfaceset-load). Writes up to `capacity` of the faces to `items`, and the total to
// `count`. Returns Syntax if `font` is not a value of the font property.
PrStatus prCssFontFaceSetLoad(PrCssFontFaceSet* self, PrDomDocument* document, PrStringView font,
                              PrCssFontFace* PR_NONNULL* PR_NULLABLE items, uint32_t capacity,
                              uint32_t* count);

// ---------------------------------------------------------------------------------------------------------------------
// Highlight (css-highlight-api)

// A set of ranges, which holds a reference to each. A highlight is reference counted, so that a script and the
// registries that name it share it. When it is freed, the registries that name it drop the name.
typedef struct PrCssHighlight PrCssHighlight;

// A document's names for highlights. A registry names its highlights without holding them.
typedef struct PrCssHighlightRegistry PrCssHighlightRegistry;

// The type of a highlight (css-highlight-api-1/#enumdef-highlighttype).
enum {
    PrCssHighlightType_Highlight,
    PrCssHighlightType_SpellingError,
    PrCssHighlightType_GrammarError,
};
typedef uint8_t PrCssHighlightType;

// Creates an empty highlight of type Highlight and priority 0. The caller holds its one reference.
PrCssHighlight* prCssHighlightCreate(void);

// Adds or releases a reference. The last release frees the highlight.
void prCssHighlightRetain(PrCssHighlight* self);
void prCssHighlightRelease(PrCssHighlight* PR_NULLABLE self);

// Returns the number of ranges.
uint32_t prCssHighlightSize(const PrCssHighlight* self);

// Returns the range at `index` in the order in which ranges were added, or null past the last range.
PrDomRange* PR_NULLABLE prCssHighlightAt(PrCssHighlight* self, uint32_t index);

// Returns true if the highlight holds `range`.
bool prCssHighlightHas(const PrCssHighlight* self, const PrDomRange* range);

// Adds `range` and retains it, unless the highlight already holds it.
void prCssHighlightAdd(PrCssHighlight* self, PrDomRange* range);

// Removes `range` and releases it. Returns false for a range that the highlight does not hold.
bool prCssHighlightDelete(PrCssHighlight* self, const PrDomRange* range);

// Removes and releases every range.
void prCssHighlightClear(PrCssHighlight* self);

// Returns a count that changes whenever the highlight's ranges change. A view of the ranges compares it to tell whether
// the view is current.
uint32_t prCssHighlightVersion(const PrCssHighlight* self);

// Get and set the highlight's priority and its type.
int32_t prCssHighlightPriority(const PrCssHighlight* self);
void prCssHighlightSetPriority(PrCssHighlight* self, int32_t priority);
PrCssHighlightType prCssHighlightType(const PrCssHighlight* self);
void prCssHighlightSetType(PrCssHighlight* self, PrCssHighlightType type);

// Returns the number of names in the registry.
uint32_t prCssHighlightRegistrySize(PrCssHighlightRegistry* self);

// Return the name and the highlight at `index`, or the null view and null past the last entry.
PrStringView prCssHighlightRegistryNameAt(PrCssHighlightRegistry* self, uint32_t index);
PrCssHighlight* PR_NULLABLE prCssHighlightRegistryAt(PrCssHighlightRegistry* self, uint32_t index);

// Returns the highlight that `name` names, or null if the registry does not have `name`.
PrCssHighlight* PR_NULLABLE prCssHighlightRegistryGet(PrCssHighlightRegistry* self,
                                                      PrStringView name);

// Names `highlight` `name`, replacing the highlight that the name had.
void prCssHighlightRegistrySet(PrCssHighlightRegistry* self, PrStringView name,
                               PrCssHighlight* highlight);

// Removes `name`. Returns false for a name that the registry does not have.
bool prCssHighlightRegistryDelete(PrCssHighlightRegistry* self, PrStringView name);

// Removes every name.
void prCssHighlightRegistryClear(PrCssHighlightRegistry* self);

// Returns a count that changes whenever the registry's names and highlights change.
uint32_t prCssHighlightRegistryVersion(PrCssHighlightRegistry* self);

// ---------------------------------------------------------------------------------------------------------------------
// Values (css-values-4)

// Returns the name of `keyword`. The text lives as long as the process.
PrStringView prCssKeywordName(PrCssKeyword keyword);

// Returns the keyword that `text` names, compared ASCII case-insensitively, or PrCssKeyword_Unknown if it names none.
PrCssKeyword prCssKeywordFromText(PrStringView text);

// Returns the name of `unit` in CSSUnitValue's unit attribute (css-typed-om-1/#dom-cssunitvalue-unit): "number",
// "percent", or the unit in lowercase. Returns the null view for Unknown.
PrStringView prCssUnitName(PrCssUnit unit);

// Writes the unit that `name` names, compared ASCII case-insensitively. Returns false if there is none.
bool prCssUnitFromName(PrStringView name, PrCssUnit* out);

// Writes the sRGB color that a <named-color> or transparent names (css-color-4/#named-colors). Returns false for any
// other keyword.
bool prCssColorFromKeyword(PrCssKeyword keyword, PrColor8* out);

// Writes the longhands of a shorthand (css-cascade-5/#shorthand-property), or none for a longhand. Writes up to
// `capacity` items to `items` and returns the total count.
uint32_t prCssPropertyLonghands(PrCssProperty property, PrCssProperty* PR_NULLABLE items,
                                uint32_t capacity);

// Returns true if a value of the property is a list of items separated by ','. That is the case if its grammar, or one
// of its alternatives, is such a list (css-typed-om-1/#list-valued-properties).
bool prCssPropertyListValued(PrCssProperty property);

// Creates an empty value.
PrCssValue* prCssValueCreate(void);

// Frees the value.
void prCssValueDestroy(PrCssValue* PR_NULLABLE self);

// Removes every node, including open ones, and keeps the storage.
void prCssValueClear(PrCssValue* self);

// Opens a node. The nodes added until it closes are its children. Returns InvalidData for a kind, unit, keyword, color
// space, literal or property with no member in its enumeration, a legacy color that is not sRGB, or a Calc outside a
// Math. Returns OutOfRange for a node deeper than kPrCssValueMaxDepth.
PrStatus prCssValueOpen(PrCssValue* self, const PrCssValueFields* fields);

// Closes the innermost open node, which must exist. Returns InvalidData, and removes the node, if its children are not
// what its kind accepts, or if it is a Math whose tree has no type (css-typed-om-1/#cssnumericvalue-type).
PrStatus prCssValueClose(PrCssValue* self);

// Adds a node with no children, which is the same as Open followed by Close.
PrStatus prCssValueAdd(PrCssValue* self, const PrCssValueFields* fields);

// Adds the tree at `node` of `from`, which is not `self`, with the same result as the equivalent Open, Close and
// intermediate calls. Returns OutOfRange if it would be deeper than kPrCssValueMaxDepth.
PrStatus prCssValueAddTree(PrCssValue* self, const PrCssValue* from, uint32_t node);

// Returns the number of nodes in the value, including open ones.
uint32_t prCssValueSize(const PrCssValue* self);

// Returns the node after the subtree at `node`: its next sibling, the next tree, or the size.
uint32_t prCssValueNext(const PrCssValue* self, uint32_t node);

// Returns the data of `node`.
PrCssValueFields prCssValueFields(const PrCssValue* self, uint32_t node);

// Writes the channels of the Color at `node`, then its alpha.
void prCssValueColorChannels(const PrCssValue* self, uint32_t node,
                             PrCssColorChannel channels[PR_NONNULL 4]);

// Parses `text` as a value of `name` (css-typed-om-1/#dom-cssstylevalue-parseall). The result is what
// prCssStyleDeclarationValue returns for a declaration of `text`. Returns InvalidData for a name that is neither a
// property nor a custom property, and Syntax for text that the grammar does not accept.
PrStatus prCssValueParse(const PrCssDeclarationName* name, PrStringView text, PrCssValue* out);

// Parses a numeric value of any type (css-typed-om-1/#dom-cssnumericvalue-parse). Returns Syntax for text that is not
// one.
PrStatus prCssValueParseNumeric(PrStringView text, PrCssValue* out);

// Parses a <color> (css-typed-om-1/#dom-csscolorvalue-parse). A named color becomes the Color it names in sRGB. Returns
// Syntax for text that is not a color.
PrStatus prCssValueParseColor(PrStringView text, PrCssValue* out);

// A used color (css-color-4/#resolving-color-values): its space, its components in the units that PrColorSpace gives,
// and its alpha in [0, 1]. A legacy color is an sRGB color that serializes with rgb()
// (css-color-4/#legacy-color-syntax), as a named color, a hex color, and rgb(), hsl() and hwb() do. `missing` has a bit
// for each missing component, alpha's last (css-color-4/#missing-color-component).
typedef struct PrCssUsedColor {
    PrColorSpace space;
    bool legacy;
    uint8_t missing;
    double components[3];
    double alpha;
} PrCssUsedColor;

// Parses `text` as a <color> and resolves it to a used color (css-color-4/#parse-color). currentcolor, the system
// colors and light-dark() resolve against the computed style of `element` where its document's render styles it, and
// otherwise against the initial values: black, in the light scheme. Returns Syntax for text that is not a color.
PrStatus prCssParseColor(PrStringView text, const PrDomElement* PR_NULLABLE element, PrCssUsedColor* out);

// How a color serializes: as CSS serializes it (css-color-4/#serializing-color-values), or as hex where HTML asks for
// it (css-color-4/#HTML-compatible-serialization-of-srgb), which an opaque legacy color takes if its components are
// whole numbers of 255ths.
enum {
    PrCssColorSerialization_Css,
    PrCssColorSerialization_HtmlCompatible,
};
typedef uint8_t PrCssColorSerialization;

// Returns the serialization of `color`.
PrString prCssUsedColorSerialize(const PrCssUsedColor* color, PrCssColorSerialization serialization);

// A font that a value of the font shorthand computes to outside the cascade, as a canvas's font does
// (html/#dom-context-2d-font): its family list, its style at its size in px, whether it asks for small capitals, and
// its serialization.
typedef struct PrCssFont PrCssFont;

// Parses `text` as a value of the font shorthand, which may not be a CSS-wide keyword or hold a substitution, and
// computes it as the font of a child of `element` computes, with line-height normal. Relative sizes and weights are
// relative to the computed font of `element` where its document's render styles it, and otherwise to a normal weight at
// 10px. Writes the font, which the caller destroys, or null and returns Syntax for text that is not such a value.
PrStatus prCssFontCreate(PrStringView text, const PrDomElement* PR_NULLABLE element,
                         PrCssFont* PR_NULLABLE* PR_NONNULL out);

// Frees the font.
void prCssFontDestroy(PrCssFont* PR_NULLABLE font);

// Returns the font's family list, which lives as long as the font.
uint32_t prCssFontFamilyCount(const PrCssFont* font);
const PrFontFamily* PR_NULLABLE prCssFontFamilies(const PrCssFont* font);

// Returns the style that the font asks of faces, with its size in px.
PrFontStyle prCssFontStyle(const PrCssFont* font);

// Returns true if the font asks for small capitals.
bool prCssFontSmallCaps(const PrCssFont* font);

// Returns the serialization of the font as the font shorthand (cssom-1/#serialize-a-css-value), without line-height. It
// lives as long as the font.
PrStringView prCssFontSerialization(const PrCssFont* font);

// Parses `text` as a <filter-value-list> (filter-effects-1/#typedef-filter-value-list) and resolves its items as the
// filter property of a child of `element` computes them, as a canvas's filter does (html/#dom-context-2d-filter).
// Lengths relative to a font are relative to the computed font of `element` where its document's render styles it, and
// otherwise to a 10px font, and currentcolor is its color, or black. A reference to a filter element is a Source item
// with no source. Writes up to `capacity` items to `out`, and the number of items to `count`. Returns Syntax for text
// that is not such a list, which neither none nor a CSS-wide keyword is.
PrStatus prCssParseFilterList(PrStringView text, const PrDomElement* PR_NULLABLE element,
                              PrLayoutFilter* PR_NULLABLE out, uint32_t capacity, uint32_t* count);

// Returns the trees from `first` up to `end`, serialized in the way that a declaration's value is written
// (cssom-1/#serialize-a-css-value).
PrString prCssValueSerialize(const PrCssValue* self, uint32_t first, uint32_t end);

// Returns true if the trees at `node` of `self` and at `otherNode` of `other` are the same value.
bool prCssValueEquals(const PrCssValue* self, uint32_t node, const PrCssValue* other,
                      uint32_t otherNode);

// Writes the type of the numeric value at `node` (css-typed-om-1/#cssnumericvalue-type), which is a number, a
// percentage, a dimension, a Math, or a node of a Math's tree. Returns InvalidData for a value with no type.
PrStatus prCssValueNumericType(const PrCssValue* self, uint32_t node, PrCssNumericType* out);

// Writes the type of a value in `unit` (css-typed-om-1/#cssnumericvalue-create-a-type). Returns false for a unit that
// measures no base type.
bool prCssNumericTypeFromUnit(PrCssUnit unit, PrCssNumericType* out);

// Writes the type of the sum of values of types `a` and `b` (css-typed-om-1/#cssnumericvalue-add-two-types). Returns
// false if the types cannot be added.
bool prCssNumericTypeAdd(const PrCssNumericType* a, const PrCssNumericType* b,
                         PrCssNumericType* out);

// Writes the type of the product of values of types `a` and `b` (css-typed-om-1/#cssnumericvalue-multiply-two-types).
// Returns false if the types cannot be multiplied.
bool prCssNumericTypeMultiply(const PrCssNumericType* a, const PrCssNumericType* b,
                              PrCssNumericType* out);

// Returns the type of the reciprocal of a value of `type` (css-typed-om-1/#cssnumericvalue-invert-a-type).
PrCssNumericType prCssNumericTypeInvert(const PrCssNumericType* type);

// Writes the numeric value at `node` as a number in `unit` (css-typed-om-1/#dom-cssnumericvalue-to). Returns
// InvalidData if it cannot be converted.
PrStatus prCssValueTo(const PrCssValue* self, uint32_t node, PrCssUnit unit, double* out);

// Writes the numeric value at `node` as a Math over a Sum of numeric values in `units`, or in the value's own units if
// `count` is zero (css-typed-om-1/#dom-cssnumericvalue-tosum). Returns InvalidData if the value cannot be expressed in
// those units.
PrStatus prCssValueToSum(const PrCssValue* self, uint32_t node, const PrCssUnit* PR_NULLABLE units,
                         uint32_t count, PrCssValue* out);

// Writes the matrix of the transform function at `node`, which is a Function
// (css-transforms-2/#mathematical-description). Returns InvalidData if it is not a transform function, or if its
// arguments are not absolute, such as a percentage or a length that is not a fixed number of pixels.
PrStatus prCssValueTransformMatrix(const PrCssValue* self, uint32_t node, PrDomMatrix* out);

// Writes the var() references among the tokens of `text` that are not nested in another reference. Writes up to
// `capacity` items to `items` and returns the total count.
uint32_t prCssVariableReferences(PrStringView text, PrCssVariableReference* PR_NULLABLE items,
                                 uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// Gives the document CSS: the sheets of its style and link elements and its xml-stylesheet instructions follow their
// owners, and its nodes keep their style data. The owners of sheets that the document already holds take their sheets
// as if the parser had just inserted them. A document that the DOM creates from it, such as by DOMImplementation, has
// CSS too. Does nothing for a document that has it.
void prCssEnable(PrDomDocument* document);

// ---------------------------------------------------------------------------------------------------------------------
// Selectors on nodes and elements (dom/#dom-parentnode-queryselector, dom/#dom-element-matches)

// A selector list, parsed once.
typedef struct PrCssSelectors PrCssSelectors;

// Writes the first descendant element that matches `selectors`, or null if none does
// (dom/#dom-parentnode-queryselector). Returns Syntax if `selectors` is not a selector list.
PrStatus prCssNodeQuerySelector(PrDomNode* self, PrStringView selectors,
                                PrDomElement* PR_NULLABLE* PR_NONNULL out);

// Makes `list` a snapshot of the descendant elements that match `selectors`, in tree order
// (dom/#dom-parentnode-queryselectorall). The snapshot holds no reference to `self`. Returns Syntax if `selectors` is
// not a selector list, and then leaves `list` unchanged.
PrStatus prCssNodeQuerySelectorAll(PrDomNode* self, PrStringView selectors, PrDomNodeList* list);

// Parses a selector list once, for any number of the queries that take one. It is parsed in the same way as the queries
// that take a string parse theirs (dom/#scope-match-a-selectors-string): no namespace prefix is declared, and `&` is
// :scope. Returns Syntax if `text` is not a selector list. The list is immutable, and may be used from any thread.
PrStatus prCssSelectorsCreate(PrStringView text, PrCssSelectors* PR_NULLABLE* PR_NONNULL out);

// Frees the selector list.
void prCssSelectorsDestroy(PrCssSelectors* PR_NULLABLE self);

// Implement prCssNodeQuerySelector and prCssNodeQuerySelectorAll with a selector list that is already parsed.
PrDomElement* PR_NULLABLE prCssNodeQuerySelectorCompiled(PrDomNode* self,
                                                         const PrCssSelectors* selectors);
void prCssNodeQuerySelectorAllCompiled(PrDomNode* self, const PrCssSelectors* selectors, PrDomNodeList* list);

// prCssElementClosest writes the element or its nearest ancestor element that matches `selectors`, or null if none does
// (dom/#dom-element-closest). prCssElementMatches writes true if the element matches `selectors`
// (dom/#dom-element-matches). Both return Syntax if `selectors` is not a selector list.
PrStatus prCssElementClosest(PrDomElement* self, PrStringView selectors,
                             PrDomElement* PR_NULLABLE* PR_NONNULL out);
PrStatus prCssElementMatches(const PrDomElement* self, PrStringView selectors, bool* out);

// Implement prCssElementClosest and prCssElementMatches with a selector list that is already parsed.
PrDomElement* PR_NULLABLE prCssElementClosestCompiled(PrDomElement* self,
                                                      const PrCssSelectors* selectors);
bool prCssElementMatchesCompiled(const PrDomElement* self, const PrCssSelectors* selectors);

// ---------------------------------------------------------------------------------------------------------------------
// The CSS of documents, shadow roots and elements (cssom-1/#extensions-to-the-document-or-shadow-root-interface,
// cssom-1/#the-elementcssinlinestyle-mixin)

// Returns the document's style sheets (cssom-1/#dom-documentorshadowroot-stylesheets), which are a view of the
// document.
PrCssStyleSheetList* prCssDocumentStyleSheets(PrDomDocument* self);

// Registers a custom property with the document (css-properties-values-api-1/#dom-css-registerproperty). Returns Syntax
// for a name that is not a custom property name, a syntax that is not a syntax string, or an initial value that the
// syntax does not accept or that depends on the element. Returns NotAllowed for a name that the document has already
// registered.
PrStatus prCssDocumentRegisterProperty(PrDomDocument* self, const PrCssPropertyDefinition* definition);

// Sets the document's font callback, or removes it if `callback` is null.
void prCssDocumentSetFontCallback(PrDomDocument* self, const PrCssFontCallback* PR_NULLABLE callback);

// Returns the document's set (css-font-loading-3/#document-font-face-set), which the document owns. Its faces are the
// CSS-connected faces of the document's style sheets, excluding those in a conditional group rule, followed by the
// faces added to it.
PrCssFontFaceSet* prCssDocumentFonts(PrDomDocument* self);

// Returns the document's registry (css-highlight-api-1/#highlight-registry), which the document owns. It holds names in
// the order in which they were first set.
PrCssHighlightRegistry* prCssDocumentHighlights(PrDomDocument* self);

// Returns the shadow root's style sheets (cssom-1/#dom-documentorshadowroot-stylesheets), which are a view of the root.
PrCssStyleSheetList* prCssShadowRootStyleSheets(PrDomShadowRoot* self);

// Returns the declarations of the element's style attribute (cssom-1/#dom-elementcssinlinestyle-style), which the
// element owns.
PrCssStyleProperties* prCssElementStyle(PrDomElement* self);

// ---------------------------------------------------------------------------------------------------------------------
// Matrices as CSS text (geometry-1/#dommatrixreadonly)

// Creates the matrix that the transform list in `text` describes (geometry-1/#parse-a-string-into-an-abstract-matrix).
// The empty string is the identity. Returns Syntax for text that is not a transform list, or that uses a relative
// length or a percentage.
PrStatus prCssMatrixFromString(PrStringView text, PrDomMatrix* out);

// Writes the matrix as CSS text, which is a matrix() or matrix3d() function
// (geometry-1/#dommatrixreadonly-stringification-behavior). Returns InvalidState if an element is not finite.
PrStatus prCssMatrixToString(const PrDomMatrix* self, PrString* out);

// ---------------------------------------------------------------------------------------------------------------------
// FontFaceSetLoadEvent : Event (css-font-loading-3)

// The event that a FontFaceSet fires when its fonts load or fail. It names the faces.
typedef struct PrCssFontFaceSetLoadEvent PrCssFontFaceSetLoadEvent;

// Converts a FontFaceSetLoadEvent to the interface that it inherits from.
PR_HANDLE_UPCAST(CssFontFaceSetLoadEvent, DomEvent, Event)

// The members of FontFaceSetLoadEventInit (css-font-loading-3/#dictdef-fontfacesetloadeventinit).
typedef struct PrCssFontFaceSetLoadEventInit {
    PrDomEventInit event;
    PrCssFontFace* PR_NONNULL const* PR_NULLABLE fontfaces;
    uint32_t count;                         // number of `fontfaces`
} PrCssFontFaceSetLoadEventInit;

// Returns an init with every member false and no faces.
static inline PrCssFontFaceSetLoadEventInit prCssFontFaceSetLoadEventInitDefault(void) {
    PrCssFontFaceSetLoadEventInit value;
    value.event = prDomEventInitDefault();
    value.fontfaces = NULL;
    value.count = 0;
    return value;
}

// Returns an Event as a FontFaceSetLoadEvent, or null if it is not one.
PrCssFontFaceSetLoadEvent* PR_NULLABLE
prCssFontFaceSetLoadEventQuery(PrDomEvent* PR_NULLABLE object);

// Creates a FontFaceSetLoadEvent of `type`, with one reference that the caller holds (prDomEventRelease). The event
// holds its faces.
PrCssFontFaceSetLoadEvent*
prCssFontFaceSetLoadEventCreate(PrStringView type,
                                const PrCssFontFaceSetLoadEventInit* PR_NULLABLE init);

// Writes the faces that the event names. Writes up to `capacity` of them to `items`, and returns the total count.
uint32_t prCssFontFaceSetLoadEventFontFaces(PrCssFontFaceSetLoadEvent* self,
                                            PrCssFontFace* PR_NONNULL* PR_NULLABLE items,
                                            uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// TransitionEvent : Event (css-transitions-1) and AnimationEvent : Event (css-animations-1)

// The events that a CSS transition fires as it runs, starts, ends or is canceled.
typedef struct PrCssTransitionEvent PrCssTransitionEvent;

// The events that a CSS animation fires as it starts, iterates, ends or is canceled.
typedef struct PrCssAnimationEvent PrCssAnimationEvent;

// Converts either event to the interface that it inherits from.
PR_HANDLE_UPCAST(CssTransitionEvent, DomEvent, Event)
PR_HANDLE_UPCAST(CssAnimationEvent, DomEvent, Event)

// The members of TransitionEventInit (css-transitions-1/#dictdef-transitioneventinit) and AnimationEventInit
// (css-animations-1/#dictdef-animationeventinit). The name is the property that transitioned, or the animation's name.
typedef struct PrCssAnimationEventInit {
    PrDomEventInit event;
    PrStringView name;
    double elapsedTime;                     // in seconds
    PrStringView pseudoElement;
} PrCssAnimationEventInit;

// Returns an init with every member false, empty or zero.
static inline PrCssAnimationEventInit prCssAnimationEventInitDefault(void) {
    PrCssAnimationEventInit value;
    value.event = prDomEventInitDefault();
    value.name = PR_SV("");
    value.elapsedTime = 0;
    value.pseudoElement = PR_SV("");
    return value;
}

// Return an Event as a TransitionEvent or an AnimationEvent, or null if it is not one.
PrCssTransitionEvent* PR_NULLABLE prCssTransitionEventQuery(PrDomEvent* PR_NULLABLE object);
PrCssAnimationEvent* PR_NULLABLE prCssAnimationEventQuery(PrDomEvent* PR_NULLABLE object);

// Create a TransitionEvent or an AnimationEvent of `type`, with one reference that the caller holds
// (prDomEventRelease).
PrCssTransitionEvent* prCssTransitionEventCreate(PrStringView type, const PrCssAnimationEventInit* PR_NULLABLE init);
PrCssAnimationEvent* prCssAnimationEventCreate(PrStringView type, const PrCssAnimationEventInit* PR_NULLABLE init);

// Return the property that the transition animates, or the animation's name
// (css-transitions-1/#interface-transitionevent-attributes, css-animations-1/#interface-animationevent-attributes).
PrStringView prCssTransitionEventPropertyName(const PrCssTransitionEvent* self);
PrStringView prCssAnimationEventAnimationName(const PrCssAnimationEvent* self);

// Return how far into the active time the event occurs, in seconds.
double prCssTransitionEventElapsedTime(const PrCssTransitionEvent* self);
double prCssAnimationEventElapsedTime(const PrCssAnimationEvent* self);

// Return the pseudo-element that the transition or animation applies to, or empty for the element itself.
PrStringView prCssTransitionEventPseudoElement(const PrCssTransitionEvent* self);
PrStringView prCssAnimationEventPseudoElement(const PrCssAnimationEvent* self);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
